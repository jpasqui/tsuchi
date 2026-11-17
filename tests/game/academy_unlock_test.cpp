// The real scalar-hold ownership implementation, with a controlled reflected field.
#include "fake_game.h"
#include "../../framework/game/academy_unlock.cpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace {
using namespace tsuchi;
namespace t = game::table;
TSUCHI_Result guard_result = TSUCHI_OK;
bool uploads_blocked = false;
uint64_t config = 0;
constexpr uint64_t flag_offset = 0x30;
}

namespace tsuchi {
TSUCHI_Result academy_block_uploads() { if (guard_result == TSUCHI_OK) uploads_blocked = true; return guard_result; }
bool academy_uploads_blocked() { return uploads_blocked; }
void academy_unblock_uploads() { uploads_blocked = false; }
namespace game {
bool live(uint64_t object) { return object && readable(object, 0x80); }
uint64_t to_handle(uint64_t object) { return live(object) ? object : 0; }
uint64_t from_handle(uint64_t handle) { return live(handle) ? handle : 0; }
uint64_t type_of(uint64_t object) { return live(object) ? 3 : 0; }
TSUCHI_Result find_place(uint64_t type, uint64_t base, const char* path, Place& out) {
    if (type != 3 || !live(base)) return TSUCHI_NOT_FOUND;
    if (!strcmp(path, "DDKConfig")) { out = {base + 0x20, 1}; return TSUCHI_OK; }
    if (strcmp(path, "DDKConfig.bCheckOpenedHeld")) return TSUCHI_NOT_FOUND;
    out = {base + flag_offset, 2};
    return TSUCHI_OK;
}
TSUCHI_Result read_value(const Place& place, Value& out) {
    if (place.property == 1) { out.kind = ValueKind::structure; return TSUCHI_OK; }
    uint8_t value = 0;
    if (!read(place.address, value)) return TSUCHI_NOT_READY;
    out.kind = ValueKind::boolean; out.boolean = value != 0;
    return TSUCHI_OK;
}
TSUCHI_Result write_value(const Place& place, const Value& value) {
    if (place.property != 2 || value.kind != ValueKind::boolean) return TSUCHI_INVALID;
    return write(place.address, uint8_t(value.boolean)) ? TSUCHI_OK : TSUCHI_NOT_READY;
}
TSUCHI_Result struct_at(const Place&, uint64_t&, uint64_t&) { return TSUCHI_UNSUPPORTED; }
TSUCHI_Result read_name(const Place&, Name&) { return TSUCHI_INVALID; } // no array here, so no row identity
}
}

int main() {
    fake::start();
    t::cells[t::data::pikmin_scatter_config.index] = 0x380;
    t::cells[t::data::pikmin_scatter_override.index] = 0x388;
    config = fake::memory(0x80);
    const uint64_t save = fake::memory(0x80);
    memset(reinterpret_cast<void*>(save), 0xa5, 0x80);
    unsigned char saved[0x80]; memcpy(saved, reinterpret_cast<void*>(save), sizeof saved);
    fake::put<uint64_t>(fake::main_image() + t::data::pikmin_scatter_config, config);
    fake::put<uint8_t>(config + flag_offset, 1);
    auto* first = mod_create("academy-open-exams");
    auto* second = mod_create("academy-community-board");
    CHECK(first && second);
    TSUCHI_FieldHold hold{};
    guard_result = TSUCHI_BUSY;
    CHECK(academy_hold_unlocked_periods(first, &hold) == TSUCHI_BUSY);
    CHECK(!hold.id && fake::get<uint8_t>(config + flag_offset) == 1 && !uploads_blocked);
    guard_result = TSUCHI_OK;
    CHECK(academy_hold_unlocked_periods(first, &hold) == TSUCHI_OK && hold.id);
    CHECK(fake::get<uint8_t>(config + flag_offset) == 0 && owned_count(first) == 1 && uploads_blocked);
    TSUCHI_FieldHold conflict{};
    CHECK(academy_hold_unlocked_periods(second, &conflict) == TSUCHI_BUSY && !conflict.id);
    CHECK(uploads_blocked); // the first mod's change keeps the block
    uploads_blocked = false; // a refused hold gives back a block it was the first to take
    CHECK(academy_hold_unlocked_periods(second, &conflict) == TSUCHI_BUSY && !uploads_blocked);
    uploads_blocked = true;
    CHECK(field_holds::end(second, hold) == TSUCHI_DENIED);
    CHECK(field_holds::end(first, hold) == TSUCHI_OK);
    CHECK(fake::get<uint8_t>(config + flag_offset) == 1 && owned_count(first) == 0);
    CHECK(academy_hold_unlocked_periods(first, &hold) == TSUCHI_OK);
    mod_stop(first);
    CHECK(fake::get<uint8_t>(config + flag_offset) == 1 && field_holds::records.count == 0);
    CHECK(academy_hold_unlocked_periods(second, &hold) == TSUCHI_OK);
    fake::put<uint8_t>(config + flag_offset, 1); // another writer owns this new value
    CHECK(field_holds::end(second, hold) == TSUCHI_BUSY);
    mod_stop(second);
    CHECK(fake::get<uint8_t>(config + flag_offset) == 1 && field_holds::records.count == 0);
    auto* third = mod_create("academy-lifetime");
    CHECK(academy_hold_unlocked_periods(third, &hold) == TSUCHI_OK);
    fake::unmap(config);
    mod_stop(third);
    CHECK(field_holds::records.count == 0);
    CHECK(!memcmp(saved, reinterpret_cast<void*>(save), sizeof saved));
    std::puts("academy unlock hold checks passed");
}
