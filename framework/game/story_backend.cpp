// The game side of the story service.

#include <string.h>

#include "actors.h"
#include "content.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "ids.h"
#include "names.h"
#include "new_maps.h"
#include "objects.h"
#include "people.h"
#include "reflect.h"
#include "services.h"
#include "story_service.h"

namespace tsuchi {
namespace {

namespace t = game::table;

uint64_t story_save() {
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    const uint64_t story = instance ? game::read_or<uint64_t>(instance + t::game_instance::story_save) : 0;
    return story && game::live(story) ? story : 0;
}

// The level's clock, or 0 for a level without one.
uint64_t level_clock() {
    const uint64_t framework = game::game_framework();
    const uint64_t at = framework ? game::read_or<uint64_t>(framework + t::game_framework::clock) : 0;
    return at && game::read_or<uint64_t>(at) == game::main_base() + t::data::clock_table ? at : 0;
}

TSUCHI_Result day(uint32_t* out) {
    const uint64_t story = story_save();
    return story && game::read(story + t::story_save::day, *out) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

TSUCHI_Result time_of_day(float* out) {
    const uint64_t at = story_save() ? level_clock() : 0;
    return at && game::read(at + t::clock::progress, *out) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

TSUCHI_Result set_time(float part) {
    if (!(part >= 0 && part <= 1)) return TSUCHI_INVALID;
    const uint64_t at = story_save() ? level_clock() : 0;
    const auto set = at ? game::method(at, t::clock_table::set_progress) : nullptr;
    if (!set) return TSUCHI_NOT_READY;
    set(reinterpret_cast<void*>(at), part, false); // the saved day keeps its own time
    return TSUCHI_OK;
}

// ---- Treasures ------------------------------------------------------------------------

// The game's row for a treasure, or 0 for a name that is no treasure's.
uint64_t treasure_row(const game::Name& name) {
    const auto row = reinterpret_cast<uint64_t>(t::fn::find_treasure_row(const_cast<game::Name*>(&name)));
    return row && game::readable(row, t::treasure_row::poko + 4) ? row : 0;
}

// The game knows a treasure by the name of its row in the game's table and by nothing else. For one of the
// game's own that name is its ID. A running mod's treasure has a name the framework gave it. TSUCHI_NOT_FOUND
// for an ID that is no treasure's. A placeholder is none, since the game keeps no number for a treasure.
TSUCHI_Result treasure_named(const char* id, game::Name& name, uint64_t& row) {
    const IdForm form = id_form(id);
    if (form == IdForm::placeholder || (form == IdForm::added && !game::added_name(id, name))) return TSUCHI_NOT_FOUND;
    const TSUCHI_Result named = form == IdForm::added ? TSUCHI_OK : game::name_from_utf8(id, name);
    if (named != TSUCHI_OK || !name.index) return named == TSUCHI_NOT_READY ? named : TSUCHI_NOT_FOUND;
    row = treasure_row(name);
    return row ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}

TSUCHI_Result treasure_count(const char* treasure, uint32_t* out) {
    const uint64_t story = story_save();
    if (!story || !game::game_framework() || !t::bound(t::fn::find_treasure_row) || !t::bound(t::fn::find_story_record)) return TSUCHI_NOT_READY;
    game::Name name{};
    uint64_t row = 0;
    const TSUCHI_Result found = treasure_named(treasure, name, row);
    if (found != TSUCHI_OK) return found;
    // A treasure never collected has no record.
    const auto record = reinterpret_cast<uint64_t>(
        t::fn::find_story_record(reinterpret_cast<void*>(story), reinterpret_cast<void*>(story + t::story_save::treasure_records), &name));
    *out = record ? uint32_t(game::read_or<int32_t>(record)) : 0;
    return TSUCHI_OK;
}

// True in the rules the game's own functions for the story's stores act in.
bool in_story_rule() {
    int32_t rule = 0;
    if (!game::game_rule(rule)) return false;
    for (uint64_t each : t::tables::story_rules)
        if (rule == int32_t(each)) return true;
    return false;
}

TSUCHI_Result give_treasure(const char* treasure, bool announce) {
    const uint64_t context = game::game_context();
    if (!context || !story_save() || !game::game_framework() || !in_story_rule() || !t::bound(t::fn::find_treasure_row) || !t::bound(t::fn::collect_treasure) || !t::bound(t::fn::add_sparklium))
        return TSUCHI_NOT_READY;
    game::Name name{};
    uint64_t row = 0, packed = 0;
    const TSUCHI_Result found = treasure_named(treasure, name, row);
    if (found != TSUCHI_OK) return found;
    const int32_t sparklium = game::read_or<int32_t>(row + t::treasure_row::sparklium), poko = game::read_or<int32_t>(row + t::treasure_row::poko);
    memcpy(&packed, &name, sizeof name);
    t::fn::collect_treasure(packed, sparklium, poko);
    if (sparklium) t::fn::add_sparklium(reinterpret_cast<void*>(context), sparklium, announce); // as its delivery does
    return TSUCHI_OK;
}

// ---- Travel ---------------------------------------------------------------------------

// After a request the game needs a moment to begin, and then goes through a
// stage of its own for the journey. Until that stage has come and gone, or ten
// seconds have passed without one, another request would land on top of it.
constexpr uint32_t kRequestFrames = 300;
struct Asked {
    uint32_t frames = 0; // left to wait for the journey to begin
    bool begun = false;
} asked;

bool on_a_journey() {
    game::Name leaf{};
    char name[game::kNameBytes];
    if (!game::leaf_stage(leaf) || !game::name_text(leaf, name, sizeof name)) return false;
    return !strcmp(name, t::name::stage_travel) || !strcmp(name, t::name::stage_portal) || !strcmp(name, t::name::stage_leave);
}

void step() {
    if (!asked.frames && !asked.begun) return;
    const bool journey = on_a_journey();
    if (journey) asked.begun = true;
    else if (asked.begun) asked = Asked{}; // it has come and gone
    else if (asked.frames) --asked.frames;
}

// The story being played with nothing else under way: what the game's own
// requests for travel need. `cave` tells whether the level is a cave.
TSUCHI_Result ready_to_travel(bool& cave) {
    namespace f = t::flow_stage;
    const uint64_t context = game::game_context(), main = game::main_base();
    int32_t rule = 0;
    if (!context || !game::game_rule(rule) || rule != int32_t(t::constant::rule_story)) return TSUCHI_NOT_READY;
    const uint64_t root = game::read_or<uint64_t>(context + t::game_context::flow);
    const uint64_t story = root ? game::read_or<uint64_t>(root + f::child) : 0;
    if (!story || game::read_or<uint64_t>(story) != main + t::data::story_flow_table) return TSUCHI_NOT_READY;
    // Within the story, the stage of plain play and nothing inside it: not the end of a day, a portal, a save.
    const uint64_t play = game::read_or<uint64_t>(story + f::child);
    if (!play || game::read_or<uint64_t>(play) != main + t::data::story_play_table) return TSUCHI_BUSY;
    if (game::read_or<uint64_t>(play + f::child) || game::read_or<uint64_t>(play + f::pending) || game::read_or<int32_t>(play + f::status) != 1 ||
        game::read_or<int32_t>(play + f::stopped))
        return TSUCHI_BUSY;
    if (asked.frames || asked.begun || game::busy_with_saves_or_scenes()) return TSUCHI_BUSY;
    if (!story_save() || !game::world_begun()) return TSUCHI_NOT_READY;
    uint32_t paused = 0;
    if (!game::paused_kinds(paused) || (paused & t::constant::pause_blocks_travel)) return TSUCHI_BUSY;
    const uint64_t at = level_clock();
    if (!at || game::read_or<uint8_t>(at + t::clock::night)) return TSUCHI_NOT_READY;
    if (game::read_or<uint8_t>(at + t::clock::time_up)) return TSUCHI_BUSY;
    const uint64_t mode = game::global<uint64_t>(t::data::game_mode);
    if (mode && game::read_or<uint8_t>(mode + t::game_mode::skip_story)) return TSUCHI_NOT_READY;
    game::Name world{};
    char name[game::kNameBytes] = "";
    if (game::world_name(world)) game::name_text(world, name, sizeof name);
    cave = !memcmp(name, t::name::cave_prefix, strlen(t::name::cave_prefix));
    return TSUCHI_OK;
}

// The queue the story takes its requests from, and its own function for adding one.
bool post(void* message) {
    const uint64_t context = game::game_context();
    const uint64_t queue = context ? game::read_or<uint64_t>(context + t::game_context::requests) : 0;
    const auto push = queue ? game::method(queue, t::request_queue_table::push) : nullptr;
    if (!push) return false;
    push(reinterpret_cast<void*>(queue), message);
    asked = Asked{kRequestFrames, false};
    return true;
}
bool can_post() {
    const uint64_t context = game::game_context();
    const uint64_t queue = context ? game::read_or<uint64_t>(context + t::game_context::requests) : 0;
    return queue && game::table_entry(queue, t::request_queue_table::push);
}

// The first player's captain as the game refers to one in a request; nobody when there is none.
uint64_t traveller() { return game::to_weak_value(game::player_captain(0)); }

// A request of the plain kind: zeroed, with what every request starts with.
uint8_t* plain_request(uint64_t bytes, uint64_t functions, uint32_t id) {
    namespace r = t::request;
    auto* made = static_cast<uint8_t*>(game::engine_memory(bytes));
    if (!made) return nullptr;
    memset(made, 0, bytes);
    const uint64_t table = game::main_base() + functions, self = reinterpret_cast<uint64_t>(made);
    const uint32_t from_menu = uint32_t(t::constant::request_from_menu);
    memcpy(made, &table, 8), memcpy(made + r::self, &self, 8), memcpy(made + r::id, &id, 4), memcpy(made + r::source, &from_menu, 4);
    return made;
}

// The level of the area an ID names, as the game spells it. One of the game's own areas goes by its level's
// name: `Area`, three digits and `_P`, matched as the engine matches names. A mod's area goes by
// `<mod id>:<name>`, which `new_map_level` turns into the level of the area it was given. A cave's floor is no area.
bool area_level(const char* id, char level[10]) {
    const char* shape = t::name::area_level_shape;
    const size_t length = strlen(shape);
    if (id_form(id) != IdForm::plain || strlen(id) != length || length != 9) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = id[i] >= 'A' && id[i] <= 'Z' ? char(id[i] - 'A' + 'a') : id[i];
        const char wanted = shape[i] >= 'A' && shape[i] <= 'Z' ? char(shape[i] - 'A' + 'a') : shape[i];
        if (wanted == '0' ? c < '0' || c > '9' : c != wanted) return false;
    }
    memcpy(level, shape, length + 1);
    memcpy(level + 4, id + 4, 3);
    return true;
}

TSUCHI_Result travel(const char* area, int32_t base) {
    char level[10];
    if (!area_level(area, level)) {
        const TSUCHI_Result mapped = new_map_level(area, level); // whatever the game is doing: an ID names an area or it does not
        if (mapped != TSUCHI_OK) return mapped;
    }
    bool cave = false;
    const TSUCHI_Result ready = ready_to_travel(cave);
    if (ready != TSUCHI_OK) return ready;
    if (cave || !can_post() || !t::bound(t::fn::travel_request)) return TSUCHI_NOT_READY;
    if(id_form(area)==IdForm::added) {
        const TSUCHI_Result prepared=prepare_new_map(area,story_save());
        if(prepared!=TSUCHI_OK) return prepared;
    }
    game::Name name{};
    if (game::name_from_utf8(level, name) != TSUCHI_OK) return TSUCHI_NOT_READY;
    namespace r = t::travel_request;
    auto* made = static_cast<uint8_t*>(game::engine_memory(r::size));
    if (!made) return TSUCHI_NO_MEMORY;
    t::fn::travel_request(made, t::constant::request_from_menu);
    const uint64_t who = traveller();
    memcpy(made + r::captain, &who, 8), memcpy(made + r::area, &name, 8), memcpy(made + r::base, &base, 4);
    post(made);
    return TSUCHI_OK;
}

TSUCHI_Result leave_cave() {
    bool cave = false;
    const TSUCHI_Result ready = ready_to_travel(cave);
    if (ready != TSUCHI_OK) return ready;
    if (!cave || !can_post()) return TSUCHI_NOT_READY; // above ground the same request ends the day
    uint8_t* made = plain_request(t::leave_request::size, t::data::leave_request_table, uint32_t(t::constant::leave_request_id));
    if (!made) return TSUCHI_NO_MEMORY;
    post(made);
    return TSUCHI_OK;
}

TSUCHI_Result enter_portal(TSUCHI_Actor actor) {
    const uint64_t portal = game::from_handle(actor.id);
    if (!portal || !game::is_game_actor(portal)) return TSUCHI_NOT_FOUND;
    bool cave = false;
    const TSUCHI_Result ready = ready_to_travel(cave);
    if (ready != TSUCHI_OK) return ready;
    // A portal is an actor with the part that leads somewhere.
    const uint64_t type = game::global<uint64_t>(t::data::portal_part_class);
    const auto find = game::method(portal, t::actor_table::find_component);
    if (!type || !find || !can_post()) return TSUCHI_NOT_READY;
    const auto part = reinterpret_cast<uint64_t>(find(reinterpret_cast<void*>(portal), reinterpret_cast<void*>(type)));
    if (!part || game::read_or<int32_t>(part + t::portal_part::leads_to, -1) < 0) return TSUCHI_INVALID; // the game drops a request for one that leads nowhere
    namespace r = t::portal_request;
    uint8_t* made = plain_request(r::size, t::data::portal_request_table, uint32_t(t::constant::portal_request_id));
    if (!made) return TSUCHI_NO_MEMORY;
    const uint64_t who = traveller(), which = game::to_weak_value(portal);
    memcpy(made + r::captain, &who, 8), memcpy(made + r::portal, &which, 8);
    post(made);
    return TSUCHI_OK;
}

const story::Backend service{day, time_of_day, set_time, travel, leave_cave, enter_portal, treasure_count, give_treasure};

} // namespace

void story_start() {
    story::set_backend(&service);
    each_frame(step);
}

} // namespace tsuchi
