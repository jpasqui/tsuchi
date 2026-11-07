#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "actor_events.h"
#include "content_identity.h"
#include "game.h"
#include "holds.h"
#include "objects.h"
#include "ordinary_lifecycle.h"
#include "reflect.h"

// One translation unit lets this focused test explicitly release the private
// identity table while exercising the production apply/keep/restore functions.
#include "../../framework/game/content_identity.cpp"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
namespace {
using tsuchi::game::Name;
struct Object { Name cid; uint64_t handle; bool live; };
Object first{{12,7},101,true}, second{{13,11},102,true};
unsigned char property[0x100]{};
Name property_kind{999,0};
void (*step)() = nullptr;
uint64_t thread = 1;
uint64_t thread_probe() { return thread; }
bool read_failure = false, write_failure = false;
unsigned fail_next_writes = 0, fail_next_places = 0, writes = 0, intern_calls = 0;
TSUCHI_Result place_result = TSUCHI_OK, intern_result = TSUCHI_OK;
TSUCHI_Mod* owner = nullptr;
TSUCHI_Mod* foreign = nullptr;
enum class Callback { none, stop, foreign_same, foreign_other_stop, stale };
Callback callback = Callback::none;
TSUCHI_Result nested_result = TSUCHI_OK;
uint64_t address(Object& object) { return reinterpret_cast<uint64_t>(&object); }
bool equals(Name left, Name right) { return left.index == right.index && left.number == right.number; }
Object* object(uint64_t at) { return at == address(first) ? &first : at == address(second) ? &second : nullptr; }
bool within(uint64_t at, uint64_t size, const void* base, size_t bytes) {
    uint64_t begin = reinterpret_cast<uint64_t>(base);
    return at >= begin && at - begin <= bytes && size <= bytes - (at - begin);
}
void reset() {
    CHECK(tsuchi::identities.count() == 0);
    first = {{12,7},101,true}; second = {{13,11},102,true};
    read_failure = write_failure = false; fail_next_writes = fail_next_places = 0;
    place_result = intern_result = TSUCHI_OK; callback = Callback::none;
}
}
namespace tsuchi {
bool each_frame(void (*function)()) { step = function; return true; }
bool on_actor_left(void (*)(uint64_t, uint64_t, ActorLeft)) { return true; }
void ordinary_lifecycle_listener(void (*)(uint64_t, uint64_t, bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t handle, uint64_t) { return game::from_handle(handle); }
}
namespace tsuchi::game {
uint64_t from_handle(uint64_t handle) {
    for (Object* entry : {&first, &second}) if (entry->live && entry->handle == handle) return address(*entry);
    return 0;
}
uint64_t to_handle(uint64_t at) { Object* entry = object(at); return entry && entry->live ? entry->handle : 0; }
uint64_t part_owner(uint64_t) { return 0; }
bool is_game_actor(uint64_t) { return false; }
bool readable(uint64_t at, uint64_t bytes) {
    if (within(at, bytes, property, sizeof property) || within(at, bytes, &property_kind, sizeof property_kind)) return true;
    return !read_failure && (within(at, bytes, &first.cid, sizeof first.cid) || within(at, bytes, &second.cid, sizeof second.cid));
}
bool writable(uint64_t at, uint64_t bytes) {
    if (write_failure) return false;
    if (fail_next_writes) { --fail_next_writes; return false; }
    bool found = within(at, bytes, &first.cid, sizeof first.cid) || within(at, bytes, &second.cid, sizeof second.cid);
    if (found) ++writes;
    return found;
}
uint64_t type_of(uint64_t at) { CHECK(object(at)); return 1; }
TSUCHI_Result find_place(uint64_t type, uint64_t base, const char* path, Place& out) {
    CHECK(type == 1 && !strcmp(path, "CID") && object(base));
    if (fail_next_places) { --fail_next_places; return TSUCHI_UNSUPPORTED; }
    if (place_result != TSUCHI_OK) return place_result;
    out = {reinterpret_cast<uint64_t>(&object(base)->cid), reinterpret_cast<uint64_t>(property), false};
    return TSUCHI_OK;
}
bool name_is(Name name, const char* text) { return name.index == 999 && !strcmp(text, "NameProperty"); }
TSUCHI_Result name_from_utf8(const char* text, Name& out) {
    ++intern_calls;
    Callback action = callback; callback = Callback::none;
    if (action == Callback::stop) mod_stop(::owner);
    if (action == Callback::foreign_same) nested_result = content_identity_hold(foreign, address(first), "foreign");
    if (action == Callback::foreign_other_stop) {
        nested_result = content_identity_hold(foreign, address(second), "foreign");
        mod_stop(::owner);
    }
    if (action == Callback::stale) first.handle += 1000;
    if (intern_result != TSUCHI_OK) return intern_result;
    out = {!strcmp(text, "first") ? 40u : !strcmp(text, "updated") ? 41u : 42u, 0};
    return TSUCHI_OK;
}
}
int main() {
    namespace g = tsuchi::game;
    namespace t = g::table;
    const uint64_t kind = reinterpret_cast<uint64_t>(&property_kind);
    memcpy(property + t::property::kind, &kind, sizeof kind);
    tsuchi::set_thread_probe(thread_probe); tsuchi::note_game_thread();
    owner = tsuchi::mod_create("identity.owner"); foreign = tsuchi::mod_create("identity.foreign"); CHECK(owner && foreign);
    const Name original = first.cid, other_original = second.cid;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_OK && step);
    CHECK(equals(first.cid, {40,0}) && tsuchi::identities.count() == 1);
    CHECK(equals(tsuchi::identities.kept_at(0).before, original));
    CHECK(tsuchi::content_identity_hold(owner, address(first), "updated") == TSUCHI_OK);
    CHECK(equals(first.cid, {41,0}) && equals(tsuchi::identities.kept_at(0).before, original));
    // An isolated declaration cannot update an older hold of the same mod.
    // New targets can be acquired and rolled back without changing that hold.
    const uint64_t overlap_floor = tsuchi::ownership_mark(owner);
    const uint64_t previous_floor = tsuchi::isolate_ownership(owner, overlap_floor);
    const unsigned overlap_intern_before = intern_calls;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_BUSY && intern_calls == overlap_intern_before);
    CHECK(equals(first.cid, {41,0}));
    CHECK(tsuchi::content_identity_hold(owner, address(second), "first") == TSUCHI_OK && equals(second.cid, {40,0}));
    const uint64_t overlap_end = tsuchi::ownership_mark(owner);
    tsuchi::isolate_ownership(owner, previous_floor);
    tsuchi::release_owned_between(owner, overlap_floor, overlap_end);
    CHECK(equals(first.cid, {41,0}) && equals(second.cid, other_original) && tsuchi::identities.count() == 1);
    const unsigned intern_before = intern_calls;
    CHECK(tsuchi::content_identity_hold(foreign, address(first), "foreign") == TSUCHI_BUSY && intern_calls == intern_before);
    first.cid = {88,9}; step(); CHECK(equals(first.cid, {41,0}));
    place_result = TSUCHI_NOT_READY; first.cid = {88,9}; step(); CHECK(tsuchi::identities.count() == 1 && equals(first.cid, {88,9}));
    place_result = TSUCHI_OK; step(); CHECK(equals(first.cid, {41,0}));
    CHECK(tsuchi::identities.release(owner, first.handle, 0) == TSUCHI_OK && equals(first.cid, original));

    intern_result = TSUCHI_NO_MEMORY;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_NO_MEMORY && equals(first.cid, original) && tsuchi::identities.count() == 0);
    intern_result = TSUCHI_OK; read_failure = true;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_NOT_READY && tsuchi::identities.count() == 0);
    read_failure = false; write_failure = true;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_NOT_READY && equals(first.cid, original) && tsuchi::identities.count() == 0);
    write_failure = false; place_result = TSUCHI_UNSUPPORTED;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_UNSUPPORTED && equals(first.cid, original));
    place_result = TSUCHI_NOT_READY;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_NOT_READY);
    place_result = TSUCHI_OK;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_OK);
    intern_result = TSUCHI_NO_MEMORY;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "updated") == TSUCHI_NO_MEMORY && equals(first.cid, {40,0}));
    intern_result = TSUCHI_OK; fail_next_writes = 1;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "updated") == TSUCHI_NOT_READY && equals(first.cid, {40,0}));
    first.cid = {88,1}; step(); CHECK(equals(first.cid, {40,0}));
    fail_next_writes = 1; step(); CHECK(tsuchi::identities.count() == 0 && equals(first.cid, original));
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_OK);
    fail_next_places = 1; step(); CHECK(tsuchi::identities.count() == 0 && equals(first.cid, original));

    const uint64_t checkpoint = tsuchi::ownership_mark(owner);
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_OK);
    tsuchi::release_owned_between(owner, checkpoint, tsuchi::ownership_mark(owner));
    CHECK(tsuchi::identities.count() == 0 && equals(first.cid, original));
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_OK);
    tsuchi::mod_stop(owner); CHECK(tsuchi::identities.count() == 0 && equals(first.cid, original));

    reset(); owner = tsuchi::mod_create("identity.stale"); CHECK(owner);
    callback = Callback::stale;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_NOT_READY && equals(first.cid, original) && tsuchi::identities.count() == 0);
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_OK);
    first.live = false; const unsigned writes_before = writes; step();
    CHECK(tsuchi::identities.count() == 0 && writes == writes_before && equals(first.cid, {40,0}));
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_NOT_FOUND);
    tsuchi::mod_stop(owner);

    reset(); owner = tsuchi::mod_create("identity.callback-stop"); CHECK(owner);
    callback = Callback::stop;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_NOT_READY && equals(first.cid, original) && tsuchi::identities.count() == 0);
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_INVALID);

    reset(); owner = tsuchi::mod_create("identity.callback-busy"); CHECK(owner);
    callback = Callback::foreign_same;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_OK && nested_result == TSUCHI_BUSY && equals(first.cid, {40,0}));
    tsuchi::mod_stop(owner); CHECK(equals(first.cid, original));

    reset(); owner = tsuchi::mod_create("identity.callback-other"); CHECK(owner);
    callback = Callback::foreign_other_stop;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_NOT_READY && nested_result == TSUCHI_OK);
    CHECK(equals(first.cid, original) && equals(second.cid, {42,0}) && tsuchi::identities.count() == 1);
    tsuchi::mod_stop(foreign); CHECK(equals(second.cid, other_original) && tsuchi::identities.count() == 0);
    TSUCHI_Mod* active = tsuchi::mod_create("identity.thread-check"); CHECK(active);
    thread = 2;
    CHECK(tsuchi::content_identity_hold(active, address(first), "first") == TSUCHI_INVALID && equals(first.cid, original));
    thread = 1;
    CHECK(tsuchi::content_identity_hold(active, address(first), nullptr) == TSUCHI_INVALID);
    CHECK(tsuchi::content_identity_hold(nullptr, address(first), "first") == TSUCHI_INVALID);
    tsuchi::mod_stop(active);

    // Successful acceptance retains the exact assigned identity after the
    // declaration releases its central hold. Provisional tests above restore.
    reset(); owner = tsuchi::mod_create("identity.accept-release"); CHECK(owner);
    const uint64_t accepted_first = tsuchi::ownership_mark(owner);
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_OK);
    const uint64_t accepted_last = tsuchi::ownership_mark(owner);
    tsuchi::content_identity_accept(owner, accepted_first, accepted_last);
    CHECK(tsuchi::identities.release(owner, first.handle, 0) == TSUCHI_OK && equals(first.cid, {40,0}));
    tsuchi::mod_stop(owner); CHECK(equals(first.cid, {40,0}) && tsuchi::identities.count() == 0);

    // Acceptance applies only to its ownership range. Older holds restore on
    // normal stop, while the successful newer declaration keeps its CID.
    reset(); owner = tsuchi::mod_create("identity.accept-range"); CHECK(owner);
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_OK);
    const uint64_t range_first = tsuchi::ownership_mark(owner);
    CHECK(tsuchi::content_identity_hold(owner, address(second), "updated") == TSUCHI_OK);
    const uint64_t range_last = tsuchi::ownership_mark(owner);
    tsuchi::content_identity_accept(owner, range_first, range_last);
    tsuchi::release_owned_between(owner, range_first, range_last);
    CHECK(equals(first.cid, {40,0}) && equals(second.cid, {41,0}) && tsuchi::identities.count() == 1);
    tsuchi::mod_stop(owner);
    CHECK(equals(first.cid, original) && equals(second.cid, {41,0}) && tsuchi::identities.count() == 0);

    // A hold acquired after the accepted range stays provisional. Retraction
    // of the accepted batch cannot release or commit that newer hold.
    reset(); owner = tsuchi::mod_create("identity.accept-newer"); CHECK(owner);
    const uint64_t newer_first = tsuchi::ownership_mark(owner);
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_OK);
    const uint64_t newer_last = tsuchi::ownership_mark(owner);
    CHECK(tsuchi::content_identity_hold(owner, address(second), "updated") == TSUCHI_OK);
    tsuchi::content_identity_accept(owner, newer_first, newer_last);
    tsuchi::release_owned_between(owner, newer_first, newer_last);
    CHECK(equals(first.cid, {40,0}) && equals(second.cid, {41,0}) && tsuchi::identities.count() == 1);
    tsuchi::mod_stop(owner);
    CHECK(equals(first.cid, {40,0}) && equals(second.cid, other_original) && tsuchi::identities.count() == 0);

    // Another mod's hold is outside this acceptance even when its local hold
    // ID happens to fall numerically inside the range.
    reset(); owner = tsuchi::mod_create("identity.accept-owner"); foreign = tsuchi::mod_create("identity.accept-foreign"); CHECK(owner && foreign);
    CHECK(tsuchi::content_identity_hold(foreign, address(second), "foreign") == TSUCHI_OK);
    const uint64_t foreign_first = tsuchi::ownership_mark(owner);
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_OK);
    tsuchi::content_identity_accept(owner, foreign_first, tsuchi::ownership_mark(owner));
    tsuchi::mod_stop(owner);
    CHECK(equals(first.cid, {40,0}) && equals(second.cid, {42,0}) && tsuchi::identities.count() == 1);
    tsuchi::mod_stop(foreign); CHECK(equals(second.cid, other_original) && tsuchi::identities.count() == 0);

    // A stop during interning ends an uncommitted update and restores the
    // donor's original FName, including its numeric suffix.
    reset(); owner = tsuchi::mod_create("identity.before-commit"); CHECK(owner);
    const uint64_t before_commit = tsuchi::ownership_mark(owner);
    CHECK(tsuchi::content_identity_hold(owner, address(first), "first") == TSUCHI_OK);
    callback = Callback::stop;
    CHECK(tsuchi::content_identity_hold(owner, address(first), "updated") == TSUCHI_NOT_READY);
    tsuchi::content_identity_accept(owner, before_commit, tsuchi::ownership_mark(owner));
    CHECK(equals(first.cid, original) && tsuchi::identities.count() == 0);
    puts("content identity hold paths passed");
}
