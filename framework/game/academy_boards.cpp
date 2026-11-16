// A board a mod supplies reaches the screen the way a downloaded one does.
//
// The Academy's boards ask the schedule manager for one course's entries, and take them from the manager's set of
// boards once the request tells them it has them. While a mod holds a board for the selected course, that request
// is answered here: the entries are stored in the manager with the game's own calls, as its download stores them,
// and the board's callback is told so. The game then adds the entries, places them, keeps them in its save and
// draws them, all by itself. The medal lines are the course table's, which a mod changes through the tables service.
#include <string.h>
#include "academy_internal.h"
#include "game.h"
#include "hooks.h"
#include "json.h"
#include "mods.h"
#include "objects.h"
#include "reflect.h"
#include "registry.h"
#include "shared.h"

namespace tsuchi {
namespace {
namespace t = game::table;
constexpr uint32_t kEntries = 64, kCourses = 72;
constexpr uint64_t kMemberRoom = 96, kReplyRoom = 64; // bytes kept for the game's records, which a build's table sizes
const Kind kBoard{"academy_board"};

struct Entry {
    char16_t nickname[81], id[80];
    uint32_t nickname_units, id_units;
    int32_t score, type, color, kings;
};
// What a mod holds for one course. `asked`, `given` and `failure` tell of the game's last request for the course.
struct Board {
    TSUCHI_Mod* mod;
    uint32_t count, given;
    bool asked;
    const char* failure;
    Entry entries[kEntries];
};
Board* boards[kCourses]{};
bool ready = false;
void (*next_request)(void*, const void*, uint32_t, const void*) = nullptr;
bool (*next_apply)(void*, void*, const void*) = nullptr;
bool mask_noted = false;

bool local_member(uint64_t member) {
    constexpr char16_t prefix[] = u"tsuchi.local.";
    const uint64_t id = member + t::academy_member::id;
    const uint64_t data = game::read_or<uint64_t>(id + t::array::data);
    const int32_t count = game::read_or<int32_t>(id + t::array::count);
    char16_t units[sizeof(prefix) / sizeof(char16_t) - 1]{};
    return count > int32_t(sizeof(units) / 2) && count <= 80 &&
        game::read(data, units) && !memcmp(units, prefix, sizeof units);
}

bool apply_member(void* chart, void* widget, const void* member) {
    const uint64_t flag = reinterpret_cast<uint64_t>(chart) + t::academy_chart::face_enabled;
    uint8_t before = 0;
    const bool mask = on_game_thread() && member && local_member(reinterpret_cast<uint64_t>(member)) &&
        game::read(flag, before) && game::write(flag, uint8_t(before & ~1u));
    const bool result = next_apply(chart, widget, member);
    if (mask) {
        uint8_t current = 0;
        if (game::read(flag, current) && !(current & 1))
            game::write(flag, uint8_t((current & ~1u) | (before & 1)));
        if (result && !mask_noted) {
            mask_noted = true;
            mod_note(kFrameworkLogId, "Academy supplied members use native mask stickers");
        }
    }
    return result;
}

const char* failure_name(TSUCHI_Result result) {
    switch (result) {
    case TSUCHI_OK: return "";
    case TSUCHI_INVALID: return "invalid";
    case TSUCHI_NO_MEMORY: return "no_memory";
    default: return "not_ready";
    }
}

// ---- Reading a board ----------------------------------------------------------

bool whole(const Json* node) {
    return node && node->kind == Json::Kind::text && node->bytes == strlen(node->text);
}
bool number(const Json* node, int32_t low, int32_t high, int32_t& value) {
    if (!node || node->kind != Json::Kind::number || !node->whole || node->integer < low || node->integer > high) return false;
    value = int32_t(node->integer);
    return true;
}
bool nickname(const Json* node, Entry& entry) {
    if (!whole(node) || !node->bytes || node->bytes > 160) return false;
    uint32_t characters = 0;
    for (size_t i = 0; i < node->bytes; ++i)
        if ((uint8_t(node->text[i]) & 0xc0) != 0x80) ++characters;
    size_t units = 0;
    if (characters > 40 || !game::utf8_to_units(node->text, entry.nickname, 80, units) || units > 80) return false;
    for (size_t i = 0; i < units; ++i)
        if (entry.nickname[i] < 32 || (entry.nickname[i] >= 127 && entry.nickname[i] < 160)) return false;
    entry.nickname_units = uint32_t(units + 1);
    entry.nickname[units] = 0;
    return true;
}
// An entry's ID is kept apart from Nintendo's: the game stores what it shows, and looks its entries up by ID.
bool identity(const Json* node, Entry& entry) {
    if (!whole(node) || node->bytes < 1 || node->bytes > 64) return false;
    constexpr char prefix[] = "tsuchi.local.";
    uint32_t at = 0;
    for (const char* p = prefix; *p; ++p) entry.id[at++] = char16_t(*p);
    for (size_t i = 0; i < node->bytes; ++i) {
        const char c = node->text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
        entry.id[at++] = char16_t(c);
    }
    entry.id[at] = 0;
    entry.id_units = at + 1;
    return true;
}
bool read_board(const Json& root, Board& board) {
    const Json* entries = root.kind == Json::Kind::object ? root.find("entries") : nullptr;
    if (!entries || entries->kind != Json::Kind::list || entries->count > kEntries) return false;
    board.count = 0;
    for (const Json* item = entries->first; item; item = item->next) {
        Entry& entry = board.entries[board.count];
        if (item->kind != Json::Kind::object || !identity(item->find("id"), entry) || !nickname(item->find("nickname"), entry) ||
            !number(item->find("score"), 0, INT32_MAX, entry.score) ||
            !number(item->find("sticker_type"), 0, 63, entry.type) ||
            !number(item->find("sticker_color"), 0, 63, entry.color)) return false;
        const Json* kings = item->find("king_count");
        if (kings && !number(kings, 0, 72, entry.kings)) return false;
        for (uint32_t old = 0; old < board.count; ++old)
            if (entry.id_units == board.entries[old].id_units && !memcmp(entry.id, board.entries[old].id, entry.id_units * 2)) return false;
        ++board.count;
    }
    return true;
}

// ---- Giving the game a board ---------------------------------------------------

template <class T>
void put(uint8_t* bytes, uint64_t offset, T value) { memcpy(bytes + offset, &value, sizeof value); }

// Whether the game's records, as this build's table has them, fit the room kept for them here.
bool layouts_fit() {
    namespace m = t::academy_member;
    namespace l = t::academy_board_list;
    namespace r = t::academy_board_reply;
    const auto within = [](uint64_t size, uint64_t at, uint64_t bytes) { return at <= size && bytes <= size - at; };
    const uint64_t member = m::size, list = l::size, reply = r::size;
    return member && member <= kMemberRoom && reply <= kReplyRoom &&
           within(member, m::nickname, t::array::size) && within(member, m::id, t::array::size) &&
           within(member, m::score, 4) && within(member, m::kings, 4) && within(member, m::type, 4) && within(member, m::color, 4) &&
           within(list, l::key, 4) && within(list, l::members, t::array::size) && within(list, l::next, 4) &&
           within(reply, r::state, 4) && within(reply, r::answered, 1);
}

// One of the game's records of an entry, for the game to copy: its strings stay the board's own memory.
void write_member(uint8_t* record, const Entry& entry) {
    namespace m = t::academy_member;
    const auto text = [&](uint64_t at, const char16_t* units, uint32_t count) {
        put(record, at + t::array::data, reinterpret_cast<uint64_t>(units));
        put(record, at + t::array::count, int32_t(count));
        put(record, at + t::array::max, int32_t(count));
    };
    text(m::nickname, entry.nickname, entry.nickname_units);
    text(m::id, entry.id, entry.id_units);
    put(record, m::score, entry.score);
    put(record, m::kings, entry.kings);
    put(record, m::type, entry.type);
    put(record, m::color, entry.color);
}

// An entry names a sticker picture and a colour by their numbers among those the game has loaded.
// TSUCHI_NOT_READY until the game has loaded them.
TSUCHI_Result stickers_exist(const Board& board) {
    const uint64_t framework = game::global<uint64_t>(t::data::game_framework);
    const uint64_t ui = framework ? game::read_or<uint64_t>(framework + t::game_framework::ui) : 0;
    int32_t masks = 0, colors = 0;
    if (!ui || !game::read(ui + t::academy_sticker_data::mask_count, masks) ||
        !game::read(ui + t::academy_sticker_data::color_count, colors) || masks <= 0 || colors <= 0) return TSUCHI_NOT_READY;
    for (uint32_t i = 0; i < board.count; ++i)
        if (board.entries[i].type >= masks || board.entries[i].color >= colors) return TSUCHI_INVALID;
    return TSUCHI_OK;
}

// Stores the board in the manager as the game's own download does: a new element of the manager's set of boards,
// the course's key, a copy of the members, and the element's place in the set, where it takes over from the board
// the course had there. The copies are the game's, made and later freed by it.
TSUCHI_Result store(uint64_t manager, uint32_t key, const Board& board) {
    namespace l = t::academy_board_list;
    const uint64_t size = t::academy_member::size;
    alignas(8) uint8_t records[kEntries * kMemberRoom]{};
    for (uint32_t i = 0; i < board.count; ++i) write_member(records + i * size, board.entries[i]);
    void* const set = reinterpret_cast<void*>(manager + t::academy_schedule::boards);
    const game::EngineSlot slot = t::fn::academy_board_slot(set);
    const uint64_t element = reinterpret_cast<uint64_t>(slot.place);
    if (!element || !game::writable(element, l::size)) return TSUCHI_NO_MEMORY;
    game::write(element + l::key, key);
    game::write(element + l::members + t::array::data, uint64_t(0)); // an empty array, which the copy then fills
    t::fn::academy_board_copy(reinterpret_cast<void*>(element + l::members), records, board.count);
    game::write(element + l::next, int32_t(-1));
    t::fn::academy_board_link(set, key, reinterpret_cast<void*>(element), slot.index);
    return TSUCHI_OK;
}

// The board held for the course the game has selected, when `key` is that course's key among the manager's boards.
Board* selected_board(uint32_t key) {
    const uint64_t context = game::global<uint64_t>(t::data::game_context);
    uint8_t course = 255;
    int32_t selected = -1;
    if (!context || !game::read(context + t::academy_context::course, course) || course >= kCourses ||
        !game::read(context + t::academy_context::board_key, selected) || uint32_t(selected) != key) return nullptr;
    return boards[course];
}

// The game's request for one course's board. For a board a mod holds, the entries go into the manager and the
// callback hears that they are there, as it does from the game for entries the manager already has. Nothing is asked
// of Nintendo. A board that cannot be given leaves the request to the game.
void request(void* interface, const void* callback, uint32_t key, const void* options) {
    Board* const board = on_game_thread() && ready ? selected_board(key) : nullptr;
    if (board) {
        const uint64_t manager = game::global<uint64_t>(t::data::academy_schedule);
        const uint64_t target = callback ? game::read_or<uint64_t>(reinterpret_cast<uint64_t>(callback) + t::academy_local_callback::target) : 0;
        const auto invoke = target ? game::method(target, t::academy_local_callback_table::invoke) : nullptr;
        TSUCHI_Result result = TSUCHI_NOT_READY;
        if (invoke && manager && reinterpret_cast<uint64_t>(interface) == manager + t::academy_schedule::interface) {
            result = stickers_exist(*board);
            if (result == TSUCHI_OK) result = store(manager, key, *board);
        }
        board->asked = true;
        board->given = result == TSUCHI_OK ? board->count : 0;
        board->failure = failure_name(result);
        if (result == TSUCHI_OK) {
            alignas(8) uint8_t reply[kReplyRoom]{};
            put(reply, t::academy_board_reply::state, uint32_t(t::constant::academy_board_answered));
            put(reply, t::academy_board_reply::answered, uint8_t(1));
            invoke(reinterpret_cast<void*>(target), reply); // the game's code from here: the board is not looked at again
            return;
        }
    }
    next_request(interface, callback, key, options);
}

// ---- A mod's hold on a course's board --------------------------------------------

void ended(void*, uint64_t key) {
    const uint32_t course = uint32_t(key - 1);
    if (course >= kCourses || !boards[course]) return;
    release(boards[course]);
    boards[course] = nullptr;
}
TSUCHI_Result ownership(TSUCHI_Mod* mod, uint32_t course) {
    if (!mod_usable(mod) || course >= kCourses) return TSUCHI_INVALID;
    const Board* held = boards[course];
    if (!held) return TSUCHI_NOT_FOUND;
    return held->mod == mod ? TSUCHI_OK : TSUCHI_DENIED;
}
} // namespace

TSUCHI_Result academy_hold_board(TSUCHI_Mod* mod, uint32_t course, const char* text) {
    if (!mod_usable(mod) || course >= kCourses || !text) return TSUCHI_INVALID;
    if (boards[course] && (boards[course]->mod != mod || !ownership_update_allowed(mod, kBoard, course + 1))) return TSUCHI_BUSY;
    if (!ready) return TSUCHI_NOT_READY;
    size_t bytes = 0;
    while (bytes <= 65536 && text[bytes]) ++bytes;
    if (!bytes || bytes > 65536) return TSUCHI_INVALID;
    JsonError error{};
    const Json* json = read_json(text, bytes, &error);
    if (!json) return error.allocation_failure ? TSUCHI_NO_MEMORY : TSUCHI_INVALID;
    auto* board = static_cast<Board*>(allocate(sizeof(Board)));
    if (!board) {
        release_json(json);
        return TSUCHI_NO_MEMORY;
    }
    memset(board, 0, sizeof *board);
    board->mod = mod;
    board->failure = "";
    const bool valid = read_board(*json, *board);
    release_json(json);
    TSUCHI_Result result = valid ? reserve_owned(mod, boards[course] ? 0 : 1) : TSUCHI_INVALID;
    const bool blocked_before = academy_uploads_blocked();
    if (result == TSUCHI_OK) result = academy_block_uploads();
    if (result == TSUCHI_OK) {
        result = own(mod, kBoard, course + 1, ended, nullptr, true);
        if (result != TSUCHI_OK && !blocked_before) academy_unblock_uploads(); // no board was taken
    }
    if (result != TSUCHI_OK) {
        release(board);
        return result;
    }
    if (boards[course]) release(boards[course]);
    boards[course] = board;
    return TSUCHI_OK;
}

TSUCHI_Result academy_release_board(TSUCHI_Mod* mod, uint32_t course) {
    const TSUCHI_Result result = ownership(mod, course);
    if (result != TSUCHI_OK) return result;
    if (!ownership_update_allowed(mod, kBoard, course + 1)) return TSUCHI_BUSY;
    ended(nullptr, course + 1);
    return disown(mod, kBoard, course + 1);
}

TSUCHI_Result academy_board_state(TSUCHI_Mod* mod, uint32_t course, TSUCHI_AcademyBoardState* out) {
    const TSUCHI_Result result = ownership(mod, course);
    if (result != TSUCHI_OK || !out) return out ? result : TSUCHI_INVALID;
    const Board& held = *boards[course];
    TSUCHI_AcademyBoardState state{};
    state.entries = held.count;
    state.rendered = held.given;
    state.pending = !held.asked;
    state.failure = held.failure;
    *out = state;
    return TSUCHI_OK;
}

void academy_board_hooks_start(TSUCHI_Mod* self) {
    const auto* native = native_hooks_backend();
    if (!native || !native->function || uint64_t(t::data::academy_board_request) == t::absent_value ||
        uint64_t(t::data::academy_board_apply) == t::absent_value ||
        !t::bound(t::fn::academy_board_slot) || !t::bound(t::fn::academy_board_copy) ||
        !t::bound(t::fn::academy_board_link) || !layouts_fit()) return;
    uint64_t id = 0;
    void* original = nullptr;
    if (!next_request && native->function(self, t::data::academy_board_request, uint32_t(t::constant::academy_hook_pac),
                         reinterpret_cast<void*>(request), &id, &original) == TSUCHI_OK)
        next_request = reinterpret_cast<decltype(next_request)>(original);
    if (!next_apply && native->function(self, t::data::academy_board_apply, uint32_t(t::constant::academy_hook_pac),
                         reinterpret_cast<void*>(apply_member), &id, &original) == TSUCHI_OK)
        next_apply = reinterpret_cast<decltype(next_apply)>(original);
    ready = next_request && next_apply;
}
} // namespace tsuchi
