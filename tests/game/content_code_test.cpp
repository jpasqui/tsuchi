// Real content glue, JSON, metadata and ownership with a made-up manifest route.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "content_service.h"
#include "content_placements.h"
#include "content_sound.h"
#include "mods.h"
#include "registry.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
namespace tsuchi { void content_service_start(); }
namespace {
using tsuchi::Json;
struct Block { void* memory; size_t bytes; };
std::vector<Block> blocks;
bool deny_memory = false;
size_t refused_allocation = 0;
unsigned allocate_calls = 0;
void* allocate(size_t bytes) {
    ++allocate_calls;
    if (deny_memory || bytes == refused_allocation) return nullptr;
    void* memory = malloc(bytes); if (memory) blocks.push_back({memory, bytes}); return memory;
}
void release(void* memory) {
    if (!memory) return;
    for (size_t i = 0; i < blocks.size(); ++i) if (blocks[i].memory == memory) {
        blocks.erase(blocks.begin() + i); free(memory); return;
    }
    CHECK(false);
}
bool retained(const void* memory) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(memory);
    for (auto block : blocks) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(block.memory);
        if (address >= begin && address - begin < block.bytes) return true;
    }
    return false;
}
uint64_t thread = 1;
uint64_t thread_probe() { return thread; }
const char* keys[] = {"content", "new_maps", "maps", "tables", "text"};
bool registered[5] = {true, true, true, true, true};
std::vector<std::string> validations, mutations;
const char* stop_during = nullptr;
const char* fail_during = nullptr;
const Json* stopped_borrow = nullptr;
bool borrowed_after_stop = false;
void (*content_step)() = nullptr;
unsigned ready_events = 0, failed_events = 0;
unsigned foreign_ready_events = 0, foreign_failed_events = 0;
struct Delivered { TSUCHI_Mod* owner; TSUCHI_ContentDeclaration declaration; };
std::vector<Delivered> ready_deliveries, failed_deliveries;
TSUCHI_Result last_failure = TSUCHI_OK;
TSUCHI_ContentDeclaration last_declaration{};
TSUCHI_Mod* stop_on_ready = nullptr;
TSUCHI_Mod* stop_on_failed = nullptr;
TSUCHI_Mod* add_on_ready = nullptr;
TSUCHI_Mod* add_on_failed = nullptr;
TSUCHI_Mod* add_during_apply = nullptr;
TSUCHI_Mod* foreign_during_apply = nullptr;
TSUCHI_ContentDeclaration nested_same{9876543}, nested_foreign{};
TSUCHI_ContentDeclaration callback_declaration{};
TSUCHI_Mod* stop_on_restore = nullptr;
struct Borrow { uint64_t key; const Json* part; bool active; };
std::vector<Borrow> borrowed;
std::vector<uint64_t> restores;
uint64_t next_borrow = 1;
unsigned external_restores = 0;
int overlap_value = 5;
const tsuchi::Kind external_kind{"test.unrelated"};
void end_external(void*, uint64_t) { ++external_restores; }
const tsuchi::Kind layout_kind{"test.layout"};
std::vector<uint64_t> layout_releases;
void end_layout(void*, uint64_t key) { layout_releases.push_back(key); }
const tsuchi::Kind borrowed_kind{"test.manifest-borrow"};
void end_borrow(void*, uint64_t key) {
    for (auto& entry : borrowed) if (entry.key == key) {
        CHECK(entry.active && retained(entry.part) && retained(entry.part->name));
        CHECK(entry.part->kind == Json::Kind::object);
        entry.active = false; restores.push_back(key);
        if (stop_on_restore) {
            TSUCHI_Mod* mod = stop_on_restore; stop_on_restore = nullptr;
            tsuchi::mod_stop(mod);
            CHECK(retained(entry.part));
        }
        return;
    }
    CHECK(false);
}
void reset_route() { validations.clear(); mutations.clear(); stop_during = nullptr; fail_during = nullptr; }
unsigned sound_start = 0, loads = 0, bank_lookups = 0, event_lists = 0, plays = 0, stops = 0, unloads = 0;
void ready(void* user, const TSUCHI_ContentReady* event) {
    CHECK(event->declaration.id);
    auto* owner = static_cast<TSUCHI_Mod*>(user);
    TSUCHI_DeclarationState state = 99;
    CHECK(tsuchi::content::table.state(owner, event->declaration, &state) == TSUCHI_OK && state == TSUCHI_DECLARATION_STATE_READY);
    ready_deliveries.push_back({owner, event->declaration});
    ++ready_events;
    if (add_on_ready) {
        TSUCHI_Mod* mod = add_on_ready; add_on_ready = nullptr;
        CHECK(tsuchi::content::table.add(mod, "{\"text\":{}}", &callback_declaration) == TSUCHI_OK);
        // Nested stepping does not apply the queued callback batch.
        content_step();
        TSUCHI_DeclarationState state = 99;
        CHECK(tsuchi::content::table.state(mod, callback_declaration, &state) == TSUCHI_OK && state == TSUCHI_DECLARATION_STATE_QUEUED);
    }
    if (stop_on_ready) {
        TSUCHI_Mod* mod = stop_on_ready; stop_on_ready = nullptr;
        CHECK(tsuchi::content::table.retract(mod, event->declaration) == TSUCHI_BUSY);
        const Json* part = borrowed.back().part;
        tsuchi::mod_stop(mod);
        CHECK(retained(part));
    }
}
void failed(void* user, const TSUCHI_ContentFailed* event) {
    CHECK(event->declaration.id);
    auto* owner = static_cast<TSUCHI_Mod*>(user);
    TSUCHI_DeclarationState state = 99;
    CHECK(tsuchi::content::table.state(owner, event->declaration, &state) == TSUCHI_OK && state == TSUCHI_DECLARATION_STATE_FAILED);
    failed_deliveries.push_back({owner, event->declaration});
    ++failed_events;
    if (!strcmp(event->reason, "busy")) last_failure = TSUCHI_BUSY;
    else if (!strcmp(event->reason, "unsupported")) last_failure = TSUCHI_UNSUPPORTED;
    else if (!strcmp(event->reason, "not_ready")) last_failure = TSUCHI_NOT_READY;
    else CHECK(false);
    if (add_on_failed) {
        TSUCHI_Mod* mod = add_on_failed; add_on_failed = nullptr;
        CHECK(tsuchi::content::table.add(mod, "{\"maps\":{}}", &callback_declaration) == TSUCHI_OK);
        content_step();
        TSUCHI_DeclarationState queued = 99;
        CHECK(tsuchi::content::table.state(mod, callback_declaration, &queued) == TSUCHI_OK && queued == TSUCHI_DECLARATION_STATE_QUEUED);
    }
    if (stop_on_failed) {
        TSUCHI_Mod* mod = stop_on_failed; stop_on_failed = nullptr;
        CHECK(tsuchi::content::table.retract(mod, event->declaration) == TSUCHI_BUSY);
        const Json* part = borrowed.back().part;
        tsuchi::mod_stop(mod);
        CHECK(retained(part));
    }
}
void foreign_ready(void*, const TSUCHI_ContentReady*) { ++foreign_ready_events; }
void foreign_failed(void*, const TSUCHI_ContentFailed*) { ++foreign_failed_events; }
void watch(TSUCHI_Mod* owner) {
    TSUCHI_Subscription ready_subscription{}, failed_subscription{};
    CHECK(tsuchi::content::table.on_ready(owner, ready, owner, &ready_subscription) == TSUCHI_OK);
    CHECK(tsuchi::content::table.on_failed(owner, failed, owner, &failed_subscription) == TSUCHI_OK);
}
// Convenience for metadata checks. Admission is always checked before the
// explicit made-up world step, and the native outcome comes from the event.
TSUCHI_Result apply_now(TSUCHI_Mod* mod, const char* definition) {
    const size_t changes = mutations.size();
    const auto result = tsuchi::content::table.add(mod, definition, &last_declaration);
    CHECK(mutations.size() == changes);
    if (result != TSUCHI_OK) return result;
    TSUCHI_DeclarationState state = 99;
    CHECK(tsuchi::content::table.state(mod, last_declaration, &state) == TSUCHI_OK && state == TSUCHI_DECLARATION_STATE_QUEUED);
    content_step();
    CHECK(foreign_ready_events == 0 && foreign_failed_events == 0);
    if (!tsuchi::mod_usable(mod)) return TSUCHI_NOT_READY;
    CHECK(tsuchi::content::table.state(mod, last_declaration, &state) == TSUCHI_OK);
    return state == TSUCHI_DECLARATION_STATE_READY ? TSUCHI_OK : last_failure;
}
}
namespace tsuchi {
void content_identity_accept(TSUCHI_Mod* mod, uint64_t first, uint64_t last) { CHECK(mod_usable(mod) && first <= last); }
bool each_frame(void (*step)()) { content_step = step; return true; }
TSUCHI_Result manifest_part(const char* key, TSUCHI_Mod* mod, const Json& part, Text& problem, bool apply) {
    int index = -1;
    for (int i = 0; i < 5; ++i) if (!strcmp(key, keys[i])) index = i;
    CHECK(index >= 0 && mod_usable(mod));
    if (!registered[index]) return TSUCHI_NOT_READY;
    if (!apply) {
        validations.push_back(key);
        if (part.kind != Json::Kind::object) { problem.set("expected a manifest object"); return TSUCHI_INVALID; }
        if (!strcmp(key, "content") && !content_placements_valid(part, problem)) return TSUCHI_INVALID;
        return TSUCHI_OK;
    }
    // No apply runs until all sections have passed their registered validators.
    CHECK(validations.size() > 0);
    mutations.push_back(key);
    if (const Json* reuse = part.find("reuse")) {
        CHECK(reuse->kind == Json::Kind::number && reuse->whole);
        if (!ownership_update_allowed(mod, borrowed_kind, uint64_t(reuse->integer))) return TSUCHI_BUSY;
        overlap_value = 99;
        return TSUCHI_OK;
    }
    if (!strcmp(key, "content")) {
        const auto result = content_placements_begin(mod, part, problem);
        if (result != TSUCHI_OK) return result;
        if (add_during_apply == mod) {
            add_during_apply = nullptr;
            const unsigned allocations_before = allocate_calls;
            const size_t blocks_before = blocks.size();
            const uint32_t owned_before = owned_count(mod);
            const uint64_t mark_before = ownership_mark(mod);
            CHECK(content::table.add(mod, "{\"maps\":{}}", &nested_same) == TSUCHI_BUSY);
            CHECK(nested_same.id == 9876543 && allocate_calls == allocations_before && blocks.size() == blocks_before);
            CHECK(owned_count(mod) == owned_before && ownership_mark(mod) == mark_before);
            CHECK(foreign_during_apply && content::table.add(foreign_during_apply, "{\"maps\":{}}", &nested_foreign) == TSUCHI_OK);
            content_step();
            TSUCHI_DeclarationState queued = 99;
            CHECK(content::table.state(foreign_during_apply, nested_foreign, &queued) == TSUCHI_OK && queued == TSUCHI_DECLARATION_STATE_QUEUED);
        }
    }
    if (stop_during && !strcmp(stop_during, key)) {
        stopped_borrow = &part;
        mod_stop(mod);
        CHECK(retained(&part) && retained(part.name));
        CHECK(part.kind == Json::Kind::object);
        borrowed_after_stop = true;
        return TSUCHI_NOT_READY;
    }
    if (fail_during && !strcmp(fail_during, key)) return TSUCHI_UNSUPPORTED;
    const uint64_t id = next_borrow++;
    borrowed.push_back({id, &part, true});
    return own(mod, borrowed_kind, id, end_borrow, nullptr);
}
void content_sound_start() { ++sound_start; }
TSUCHI_Result content_sound_load(TSUCHI_Mod*, const char* id, const void* data, uint32_t size, uint64_t* out) {
    CHECK(!strcmp(id, "decl.check:bank") && data && size == 4); ++loads; *out = 81; return TSUCHI_OK;
}
TSUCHI_Result content_sound_find(TSUCHI_Mod*, const char* id, uint64_t* out) {
    ++bank_lookups;
    if (strcmp(id, "decl.check:bank")) return TSUCHI_NOT_FOUND;
    *out = 81; return TSUCHI_OK;
}
TSUCHI_Result content_sound_events(TSUCHI_Mod*, uint64_t bank, char* buffer, uint64_t capacity, uint64_t* out) {
    CHECK(bank == 81); ++event_lists;
    const char names[] = "#19\0"; *out = sizeof names;
    if (capacity < sizeof names) return TSUCHI_CAPACITY;
    memcpy(buffer, names, sizeof names); return TSUCHI_OK;
}
TSUCHI_Result content_sound_play(TSUCHI_Mod*, uint64_t bank, const char* event, const TSUCHI_Vec3& point, uint64_t* out) {
    CHECK(bank == 81 && !strcmp(event, "#19") && point.x == 10 && point.y == 20 && point.z == 30);
    ++plays; *out = 82; return TSUCHI_OK;
}
TSUCHI_Result content_sound_stop(TSUCHI_Mod*, uint64_t voice) { CHECK(voice == 82); ++stops; return TSUCHI_OK; }
TSUCHI_Result content_sound_unload(TSUCHI_Mod*, uint64_t bank) { CHECK(bank == 81); ++unloads; return TSUCHI_OK; }
}
int main() {
    tsuchi::set_allocator({allocate, release});
    // Ownership retains its compact record allocation. Ending and reusing a
    // physical slot gives it a newer sequence, outside the older rollback range.
    TSUCHI_Mod* layout = tsuchi::mod_create("decl.layout"); CHECK(layout);
    CHECK(tsuchi::ownership_mark(layout) == 1);
    const size_t layout_blocks = blocks.size();
    refused_allocation = 8 * 40;
    CHECK(tsuchi::reserve_owned(layout, 1) == TSUCHI_NO_MEMORY);
    CHECK(tsuchi::own(layout, layout_kind, 1, end_layout, nullptr) == TSUCHI_NO_MEMORY);
    CHECK(tsuchi::owned_count(layout) == 0 && tsuchi::ownership_mark(layout) == 1
        && blocks.size() == layout_blocks && tsuchi::owner(layout_kind, 1) == nullptr);
    refused_allocation = 0;
    CHECK(tsuchi::own(layout, layout_kind, 1, end_layout, nullptr) == TSUCHI_OK);
    const uint64_t ended_mark = tsuchi::ownership_mark(layout);
    CHECK(tsuchi::own(layout, layout_kind, 2, end_layout, nullptr) == TSUCHI_OK);
    const uint64_t reused_mark = tsuchi::ownership_mark(layout);
    CHECK(tsuchi::disown(layout, layout_kind, 2) == TSUCHI_OK);
    CHECK(tsuchi::owned_count(layout) == 1 && tsuchi::owner(layout_kind, 2) == nullptr);
    CHECK(tsuchi::own(layout, layout_kind, 3, end_layout, nullptr) == TSUCHI_OK);
    tsuchi::release_owned_between(layout, ended_mark, reused_mark);
    CHECK(tsuchi::owned_count(layout) == 2 && layout_releases.empty());
    const uint64_t new_mark = tsuchi::ownership_mark(layout);
    const uint64_t prior_floor = tsuchi::isolate_ownership(layout, new_mark);
    CHECK(tsuchi::own(layout, layout_kind, 1, end_layout, nullptr, true) == TSUCHI_BUSY);
    CHECK(tsuchi::own(layout, layout_kind, 4, end_layout, nullptr) == TSUCHI_OK);
    CHECK(tsuchi::own(layout, layout_kind, 4, end_layout, nullptr, true) == TSUCHI_OK);
    tsuchi::isolate_ownership(layout, prior_floor);
    tsuchi::release_owned_between(layout, reused_mark, new_mark);
    CHECK(layout_releases.size() == 1 && layout_releases[0] == 3);
    CHECK(tsuchi::owner(layout_kind, 1) == layout && tsuchi::owner(layout_kind, 4) == layout
        && tsuchi::owner(layout_kind, 3) == nullptr && tsuchi::owned_count(layout) == 2);
    tsuchi::mod_stop(layout);
    CHECK(layout_releases.size() == 3 && layout_releases[1] == 4 && layout_releases[2] == 1);

    tsuchi::set_thread_probe(thread_probe); tsuchi::note_game_thread();
    TSUCHI_Mod* mod = tsuchi::mod_create("decl.check"); CHECK(mod);
    tsuchi::content_service_start(); CHECK(sound_start == 1);
    const auto& service = tsuchi::content::table;
    CHECK(content_step);
    TSUCHI_Mod* observer = tsuchi::mod_create("decl.observer"); CHECK(observer);
    TSUCHI_Subscription ready_subscription{}, failed_subscription{};
    CHECK(service.on_ready(observer, foreign_ready, nullptr, &ready_subscription) == TSUCHI_OK);
    CHECK(service.on_failed(observer, foreign_failed, nullptr, &failed_subscription) == TSUCHI_OK);
    watch(mod);
    const char* invalid[] = {"", "{", "[]", "null", "{}", "{\"version\":1}",
                             "{\"text\":{},\"text\":{}}", "{\"text\":{},}"};
    for (const char* text : invalid) { reset_route(); CHECK(apply_now(mod, text) == TSUCHI_INVALID); CHECK(mutations.empty()); }
    reset_route(); const size_t before = blocks.size();
    CHECK(apply_now(mod, "{\"content\":{},\"maps\":false}") == TSUCHI_INVALID);
    CHECK(validations.size() == 2 && mutations.empty() && blocks.size() == before);
    registered[2] = false; reset_route();
    CHECK(apply_now(mod, "{\"content\":{},\"maps\":{}}") == TSUCHI_NOT_READY && mutations.empty());
    registered[2] = true;
    deny_memory = true;
    CHECK(apply_now(mod, "{\"content\":{}}") == TSUCHI_NO_MEMORY);
    deny_memory = false;
    reset_route();
    const char envelope[] = "{\"content\":{},\"new_maps\":{},\"maps\":{},\"tables\":{},\"text\":{}}";
    CHECK(apply_now(mod, envelope) == TSUCHI_OK && validations.size() == 5 && mutations.size() == 5);
    for (size_t i = 0; i < 5; ++i) CHECK(validations[i] == keys[i] && mutations[i] == keys[i]);
    for (const auto& entry : borrowed) CHECK(entry.active && retained(entry.part));
    reset_route();
    CHECK(apply_now(mod, "{\"maps\":{}}") == TSUCHI_OK);
    CHECK(borrowed.size() == 6 && borrowed.back().part != borrowed.front().part);

    // Metadata is production code. Numbers beyond script floating-point precision
    // stay exact integers, and text retains UTF-8 rather than byte truncation.
    reset_route();
    const char metadata[] = R"({"content":{"treasures":{"ruby":{"placement_values":{
        "enabled":{"kind":"toggle","default":true},
        "serial":{"kind":"integer","default":9007199254740993},
        "speed":{"kind":"number","default":1.25},
        "label":{"kind":"text","default":"caf\u00e9"}
    }}}}})";
    CHECK(apply_now(mod, metadata) == TSUCHI_OK);
    char text[128] = "unchanged"; uint64_t bytes = 99;
    CHECK(service.placement_values(mod, "decl.check:ruby", text, 2, &bytes) == TSUCHI_CAPACITY);
    CHECK(!strcmp(text, "unchanged") && bytes == 28);
    CHECK(service.placement_values(mod, "decl.check:ruby", text, sizeof text, &bytes) == TSUCHI_OK);
    const char expected[] = "enabled\0serial\0speed\0label\0";
    CHECK(bytes == sizeof expected && !memcmp(text, expected, bytes));
    TSUCHI_PlacementKind kind = 99;
    CHECK(service.placement_kind(mod, "decl.check:ruby", "enabled", &kind) == TSUCHI_OK && kind == TSUCHI_PLACEMENT_KIND_TOGGLE);
    CHECK(service.placement_kind(mod, "decl.check:ruby", "serial", &kind) == TSUCHI_OK && kind == TSUCHI_PLACEMENT_KIND_INTEGER);
    CHECK(service.placement_kind(mod, "decl.check:ruby", "speed", &kind) == TSUCHI_OK && kind == TSUCHI_PLACEMENT_KIND_NUMBER);
    CHECK(service.placement_kind(mod, "decl.check:ruby", "label", &kind) == TSUCHI_OK && kind == TSUCHI_PLACEMENT_KIND_TEXT);
    bool toggle = false; int64_t integer = 77; float number = 77;
    CHECK(service.placement_toggle(mod, "decl.check:ruby", "enabled", &toggle) == TSUCHI_OK && toggle);
    CHECK(service.placement_integer(mod, "decl.check:ruby", "serial", &integer) == TSUCHI_OK && integer == 9007199254740993LL);
    CHECK(service.placement_number(mod, "decl.check:ruby", "speed", &number) == TSUCHI_OK && number == 1.25f);
    memcpy(text, "unchanged", 10);
    CHECK(service.placement_text(mod, "decl.check:ruby", "label", text, 2, &bytes) == TSUCHI_CAPACITY && !strcmp(text, "unchanged"));
    CHECK(bytes == 6 && service.placement_text(mod, "decl.check:ruby", "label", text, sizeof text, &bytes) == TSUCHI_OK);
    CHECK(!strcmp(text, "caf\xC3\xA9"));
    CHECK(service.placement_number(mod, "decl.check:ruby", "serial", &number) == TSUCHI_INVALID && number == 1.25f);
    CHECK(service.placement_integer(mod, "decl.check:ruby", "absent", &integer) == TSUCHI_NOT_FOUND);
    CHECK(service.placement_kind(mod, "decl.check:absent", "serial", &kind) == TSUCHI_NOT_FOUND);
    CHECK(service.placement_kind(mod, "bad:id:again", "serial", &kind) == TSUCHI_INVALID);
    CHECK(service.placement_kind(mod, "decl.check:ruby", "bad/name", &kind) == TSUCHI_INVALID);
    reset_route();
    CHECK(apply_now(mod, metadata) == TSUCHI_BUSY);
    const char* bad_defaults[] = {R"({"kind":"integer","default":1.5})", R"({"kind":"toggle","default":0})",
        R"({"kind":"number","default":1e100})", R"({"kind":"text","default":"a\u0000b"})",
        R"({"kind":"text","default":"ok","extra":1})", R"({"kind":"unknown","default":0})"};
    for (const char* value : bad_defaults) {
        std::string definition = "{\"content\":{\"creatures\":{\"other\":{\"placement_values\":{\"value\":";
        definition += value; definition += "}}}}}";
        reset_route(); CHECK(apply_now(mod, definition.c_str()) == TSUCHI_INVALID && mutations.empty());
    }

    const unsigned char data[4]{}; TSUCHI_SoundBank bank{}; TSUCHI_SoundVoice voice{};
    CHECK(service.load_bank(mod, "decl.check:bank", data, sizeof data, &bank) == TSUCHI_OK && bank.id == 81 && loads == 1);
    TSUCHI_SoundBank looked_up{777};
    CHECK(service.bank(mod, "decl.check:bank", &looked_up) == TSUCHI_OK && looked_up.id == bank.id && bank_lookups == 1);
    looked_up.id = 777;
    CHECK(service.bank(mod, "decl.check:missing", &looked_up) == TSUCHI_NOT_FOUND && looked_up.id == 777 && bank_lookups == 2);
    CHECK(service.bank(mod, "bad:id:again", &looked_up) == TSUCHI_INVALID && bank_lookups == 2);
    CHECK(service.bank(mod, "decl.check:bank", nullptr) == TSUCHI_INVALID && bank_lookups == 2);
    CHECK(service.load_bank(mod, "decl.check:bank", data, 16u * 1024u * 1024u + 1, &bank) == TSUCHI_CAPACITY && loads == 1);
    CHECK(service.bank_events(mod, bank, text, sizeof text, &bytes) == TSUCHI_OK && !strcmp(text, "#19") && event_lists == 1);
    TSUCHI_Vec3 point{10,20,30};
    CHECK(service.play_event(mod, bank, "#19", &point, &voice) == TSUCHI_OK && voice.id == 82 && plays == 1);
    CHECK(service.stop_event(mod, voice) == TSUCHI_OK && stops == 1);
    CHECK(service.unload_bank(mod, bank) == TSUCHI_OK && unloads == 1);
    thread = 2;
    CHECK(apply_now(mod, envelope) == TSUCHI_INVALID);
    CHECK(service.load_bank(mod, "decl.check:bank", data, 4, &bank) == TSUCHI_INVALID);
    CHECK(service.bank(mod, "decl.check:bank", &looked_up) == TSUCHI_INVALID && bank_lookups == 2);
    CHECK(service.bank_events(mod, bank, text, sizeof text, &bytes) == TSUCHI_INVALID);
    CHECK(service.play_event(mod, bank, "#19", &point, &voice) == TSUCHI_INVALID);
    CHECK(service.stop_event(mod, voice) == TSUCHI_INVALID);
    CHECK(service.unload_bank(mod, bank) == TSUCHI_INVALID);
    CHECK(service.placement_values(mod, "decl.check:ruby", text, sizeof text, &bytes) == TSUCHI_INVALID);
    CHECK(service.placement_kind(mod, "decl.check:ruby", "serial", &kind) == TSUCHI_INVALID);
    CHECK(service.placement_toggle(mod, "decl.check:ruby", "enabled", &toggle) == TSUCHI_INVALID);
    CHECK(service.placement_integer(mod, "decl.check:ruby", "serial", &integer) == TSUCHI_INVALID);
    CHECK(service.placement_number(mod, "decl.check:ruby", "speed", &number) == TSUCHI_INVALID);
    CHECK(service.placement_text(mod, "decl.check:ruby", "label", text, sizeof text, &bytes) == TSUCHI_INVALID);
    thread = 1;
    tsuchi::mod_stop(mod);
    CHECK(restores.size() == 7);
    for (size_t i = 0; i < restores.size(); ++i) CHECK(restores[i] == 7 - i);
    CHECK(apply_now(mod, envelope) == TSUCHI_INVALID);
    CHECK(service.placement_values(mod, "decl.check:ruby", text, sizeof text, &bytes) == TSUCHI_INVALID);
    CHECK(service.load_bank(mod, "decl.check:bank", data, 4, &bank) == TSUCHI_INVALID);
    CHECK(service.bank(mod, "decl.check:bank", &looked_up) == TSUCHI_INVALID && bank_lookups == 2);
    CHECK(service.bank_events(mod, bank, text, sizeof text, &bytes) == TSUCHI_INVALID);
    CHECK(service.play_event(mod, bank, "#19", &point, &voice) == TSUCHI_INVALID);
    CHECK(service.stop_event(mod, voice) == TSUCHI_INVALID);
    CHECK(service.unload_bank(mod, bank) == TSUCHI_INVALID);
    CHECK(service.placement_kind(mod, "decl.check:ruby", "serial", &kind) == TSUCHI_INVALID);
    CHECK(service.placement_toggle(mod, "decl.check:ruby", "enabled", &toggle) == TSUCHI_INVALID);
    CHECK(service.placement_integer(mod, "decl.check:ruby", "serial", &integer) == TSUCHI_INVALID);
    CHECK(service.placement_number(mod, "decl.check:ruby", "speed", &number) == TSUCHI_INVALID);
    CHECK(service.placement_text(mod, "decl.check:ruby", "label", text, sizeof text, &bytes) == TSUCHI_INVALID);
    TSUCHI_Mod* reader = tsuchi::mod_create("decl.reader"); CHECK(reader);
    CHECK(service.placement_kind(reader, "decl.check:ruby", "serial", &kind) == TSUCHI_NOT_FOUND);

    // Native failure is reported after admission and rolls back reversible
    // resources, while the failed batch stays available until retracted.
    TSUCHI_Mod* partial = tsuchi::mod_create("decl.partial"); CHECK(partial);
    watch(partial);
    reset_route(); fail_during = "text";
    CHECK(apply_now(partial, "{\"maps\":{},\"text\":{}}") == TSUCHI_UNSUPPORTED);
    CHECK(!borrowed.back().active && retained(borrowed.back().part));
    tsuchi::mod_stop(partial); CHECK(!borrowed.back().active);

    // During an apply callback, stop cleans newer map records first but keeps
    // the declaration tree until the callback returns to content.add.
    TSUCHI_Mod* stopping = tsuchi::mod_create("decl.stop"); CHECK(stopping);
    watch(stopping);
    reset_route(); stop_during = "text";
    CHECK(apply_now(stopping, "{\"maps\":{},\"text\":{}}") == TSUCHI_NOT_READY);
    CHECK(borrowed_after_stop && stopped_borrow && !retained(stopped_borrow));
    CHECK(!borrowed.back().active);
    TSUCHI_DeclarationState declaration_state = 99;
    CHECK(service.state(stopping, last_declaration, &declaration_state) == TSUCHI_INVALID);

    // Retraction can swap the final queued record ahead of an earlier one.
    // Both remaining batches still have to apply on this world step in order.
    TSUCHI_Mod* queue_mod = tsuchi::mod_create("decl.queue"); CHECK(queue_mod);
    watch(queue_mod);
    CHECK(tsuchi::own(queue_mod, external_kind, 100, end_external, nullptr) == TSUCHI_OK);
    reset_route();
    TSUCHI_ContentDeclaration first{}, second{}, third{};
    CHECK(service.add(queue_mod, "{\"maps\":{}}", &first) == TSUCHI_OK);
    CHECK(service.add(queue_mod, "{\"maps\":{}}", &second) == TSUCHI_OK);
    CHECK(service.add(queue_mod, "{\"maps\":{}}", &third) == TSUCHI_OK && mutations.empty());
    CHECK(service.state(queue_mod, second, &declaration_state) == TSUCHI_OK && declaration_state == TSUCHI_DECLARATION_STATE_QUEUED);
    CHECK(service.state(observer, second, &declaration_state) == TSUCHI_DENIED);
    CHECK(service.retract(observer, first) == TSUCHI_DENIED);
    CHECK(service.retract(queue_mod, first) == TSUCHI_OK);
    CHECK(service.state(queue_mod, first, &declaration_state) == TSUCHI_NOT_FOUND);
    unsigned ready_before = ready_events;
    content_step();
    CHECK(mutations.size() == 2 && ready_events == ready_before + 2);
    CHECK(service.state(queue_mod, second, &declaration_state) == TSUCHI_OK && declaration_state == TSUCHI_DECLARATION_STATE_READY);
    CHECK(service.state(queue_mod, third, &declaration_state) == TSUCHI_OK && declaration_state == TSUCHI_DECLARATION_STATE_READY);
    const size_t second_borrow = borrowed.size() - 2, third_borrow = borrowed.size() - 1;
    CHECK(tsuchi::own(queue_mod, external_kind, 101, end_external, nullptr) == TSUCHI_OK);
    CHECK(service.retract(queue_mod, second) == TSUCHI_OK);
    CHECK(!borrowed[second_borrow].active && borrowed[third_borrow].active && external_restores == 0);
    CHECK(tsuchi::owned_count(queue_mod) == 6);
    CHECK(service.retract(queue_mod, second) == TSUCHI_NOT_FOUND);
    thread = 2;
    CHECK(service.state(queue_mod, third, &declaration_state) == TSUCHI_INVALID);
    CHECK(service.retract(queue_mod, third) == TSUCHI_INVALID);
    thread = 1;

    // A new batch cannot update a native record held by an older declaration
    // of this same mod. Failure rollback leaves the older record in force.
    TSUCHI_Mod* overlap_mod = tsuchi::mod_create("decl.overlap"); CHECK(overlap_mod);
    watch(overlap_mod);
    reset_route(); CHECK(apply_now(overlap_mod, "{\"maps\":{}}") == TSUCHI_OK);
    const TSUCHI_ContentDeclaration earlier = last_declaration;
    const uint64_t earlier_key = borrowed.back().key;
    char overlap_definition[96];
    snprintf(overlap_definition, sizeof overlap_definition, "{\"maps\":{\"reuse\":%llu}}", static_cast<unsigned long long>(earlier_key));
    reset_route(); CHECK(apply_now(overlap_mod, overlap_definition) == TSUCHI_BUSY);
    CHECK(overlap_value == 5 && borrowed.back().active);
    CHECK(service.retract(overlap_mod, last_declaration) == TSUCHI_OK && overlap_value == 5 && borrowed.back().active);
    CHECK(service.state(overlap_mod, earlier, &declaration_state) == TSUCHI_OK && declaration_state == TSUCHI_DECLARATION_STATE_READY);
    CHECK(service.retract(overlap_mod, earlier) == TSUCHI_OK && !borrowed.back().active);
    tsuchi::mod_stop(overlap_mod);
    tsuchi::mod_stop(queue_mod);
    CHECK(external_restores == 2 && !borrowed[third_borrow].active);
    CHECK(service.state(queue_mod, third, &declaration_state) == TSUCHI_INVALID);
    CHECK(service.retract(queue_mod, third) == TSUCHI_INVALID);

    // Stop while a ready handler runs. Active batches reject retraction and
    // retain their tree through the handler's final access.
    TSUCHI_Mod* ready_mod = tsuchi::mod_create("decl.ready-stop"); CHECK(ready_mod);
    watch(ready_mod);
    reset_route(); stop_on_ready = ready_mod;
    CHECK(apply_now(ready_mod, "{\"maps\":{}}") == TSUCHI_NOT_READY);
    CHECK(!borrowed.back().active && !retained(borrowed.back().part));

    // Stop from a native restore during retraction. The active declaration
    // tree survives that callback, then ends once retract returns.
    TSUCHI_Mod* restore_mod = tsuchi::mod_create("decl.restore-stop"); CHECK(restore_mod);
    watch(restore_mod);
    reset_route(); CHECK(apply_now(restore_mod, "{\"maps\":{}}") == TSUCHI_OK);
    const Json* restored_part = borrowed.back().part;
    stop_on_restore = restore_mod;
    CHECK(service.retract(restore_mod, last_declaration) == TSUCHI_OK);
    CHECK(!retained(restored_part));

    // A batch added from an event waits for the next step and lives outside
    // the completed batch's ownership range.
    TSUCHI_Mod* callback_mod = tsuchi::mod_create("decl.callback"); CHECK(callback_mod);
    watch(callback_mod);
    reset_route(); add_on_ready = callback_mod;
    CHECK(service.add(callback_mod, "{\"maps\":{}}", &first) == TSUCHI_OK);
    content_step();
    CHECK(mutations.size() == 1 && callback_declaration.id);
    CHECK(service.retract(callback_mod, first) == TSUCHI_OK);
    CHECK(service.state(callback_mod, callback_declaration, &declaration_state) == TSUCHI_OK && declaration_state == TSUCHI_DECLARATION_STATE_QUEUED);
    content_step(); CHECK(mutations.size() == 2);
    CHECK(service.state(callback_mod, callback_declaration, &declaration_state) == TSUCHI_OK && declaration_state == TSUCHI_DECLARATION_STATE_READY);
    tsuchi::mod_stop(callback_mod);

    // A failed handler can queue its owner's next batch after native ownership
    // isolation ends. The new batch survives retraction of the failed one.
    TSUCHI_Mod* failed_callback_mod = tsuchi::mod_create("decl.failed-callback"); CHECK(failed_callback_mod);
    watch(failed_callback_mod);
    reset_route(); fail_during = "text"; add_on_failed = failed_callback_mod;
    CHECK(apply_now(failed_callback_mod, "{\"text\":{}}") == TSUCHI_UNSUPPORTED);
    const TSUCHI_ContentDeclaration failed_parent = last_declaration;
    CHECK(mutations.size() == 1 && callback_declaration.id && callback_declaration.id != failed_parent.id);
    CHECK(service.retract(failed_callback_mod, failed_parent) == TSUCHI_OK);
    CHECK(service.state(failed_callback_mod, callback_declaration, &declaration_state) == TSUCHI_OK && declaration_state == TSUCHI_DECLARATION_STATE_QUEUED);
    content_step();
    CHECK(mutations.size() == 2 && service.state(failed_callback_mod, callback_declaration, &declaration_state) == TSUCHI_OK && declaration_state == TSUCHI_DECLARATION_STATE_READY);
    tsuchi::mod_stop(failed_callback_mod);

    // A native apply callback cannot queue another declaration for the isolated
    // owner. Another mod's queued declaration stays outside the failed rollback.
    TSUCHI_Mod* nested_owner = tsuchi::mod_create("decl.nested-owner"); CHECK(nested_owner);
    TSUCHI_Mod* nested_other = tsuchi::mod_create("decl.nested-other"); CHECK(nested_other);
    watch(nested_owner); watch(nested_other);
    reset_route(); fail_during = "text"; add_during_apply = nested_owner; foreign_during_apply = nested_other;
    const unsigned nested_ready_before = ready_events, nested_failed_before = failed_events;
    CHECK(apply_now(nested_owner, "{\"content\":{},\"text\":{}}") == TSUCHI_UNSUPPORTED);
    const TSUCHI_ContentDeclaration nested_parent = last_declaration;
    CHECK(add_during_apply == nullptr && nested_same.id == 9876543 && nested_foreign.id);
    CHECK(mutations.size() == 2 && ready_events == nested_ready_before && failed_events == nested_failed_before + 1);
    CHECK(tsuchi::owned_count(nested_owner) == 3 && tsuchi::owned_count(nested_other) == 3);
    CHECK(service.state(nested_other, nested_foreign, &declaration_state) == TSUCHI_OK && declaration_state == TSUCHI_DECLARATION_STATE_QUEUED);
    CHECK(service.retract(nested_owner, nested_parent) == TSUCHI_OK);
    CHECK(service.state(nested_other, nested_foreign, &declaration_state) == TSUCHI_OK && declaration_state == TSUCHI_DECLARATION_STATE_QUEUED);
    content_step();
    CHECK(mutations.size() == 3 && mutations.back() == "maps" && ready_events == nested_ready_before + 1);
    CHECK(service.state(nested_other, nested_foreign, &declaration_state) == TSUCHI_OK && declaration_state == TSUCHI_DECLARATION_STATE_READY);
    CHECK(ready_deliveries.back().owner == nested_other && ready_deliveries.back().declaration.id == nested_foreign.id);
    CHECK(foreign_ready_events == 0 && foreign_failed_events == 0);
    foreign_during_apply = nullptr;
    tsuchi::mod_stop(nested_owner); tsuchi::mod_stop(nested_other);

    // A rollback callback can stop the mod. No failed event reaches observers
    // afterward, and the active tree remains readable until rollback returns.
    TSUCHI_Mod* rollback_mod = tsuchi::mod_create("decl.rollback-stop"); CHECK(rollback_mod);
    watch(rollback_mod);
    reset_route(); fail_during = "text"; stop_on_restore = rollback_mod;
    const unsigned rollback_failed_before = failed_events;
    CHECK(apply_now(rollback_mod, "{\"maps\":{},\"text\":{}}") == TSUCHI_NOT_READY);
    CHECK(!retained(borrowed.back().part) && failed_events == rollback_failed_before);

    // Failed-event stop also defers the declaration tree's destruction until
    // the handler returns, after reversible native resources have ended.
    TSUCHI_Mod* failed_mod = tsuchi::mod_create("decl.failed-stop"); CHECK(failed_mod);
    watch(failed_mod);
    reset_route(); fail_during = "text"; stop_on_failed = failed_mod;
    CHECK(apply_now(failed_mod, "{\"maps\":{},\"text\":{}}") == TSUCHI_NOT_READY);
    CHECK(!borrowed.back().active && !retained(borrowed.back().part));

    // A queued batch canceled by stop has no native mutations and no event.
    TSUCHI_Mod* canceled = tsuchi::mod_create("decl.canceled"); CHECK(canceled);
    watch(canceled);
    reset_route();
    CHECK(service.add(canceled, "{\"maps\":{}}", &first) == TSUCHI_OK);
    ready_before = ready_events; const unsigned failed_before = failed_events;
    tsuchi::mod_stop(canceled); content_step();
    CHECK(mutations.empty() && ready_events == ready_before && failed_events == failed_before);
    CHECK(foreign_ready_events == 0 && foreign_failed_events == 0);

    // Two owners queued together hear only their own outcome, in declaration
    // order, while an unrelated subscriber hears neither result.
    TSUCHI_Mod* ordered_ready = tsuchi::mod_create("decl.ordered-ready"); CHECK(ordered_ready);
    TSUCHI_Mod* ordered_failed = tsuchi::mod_create("decl.ordered-failed"); CHECK(ordered_failed);
    watch(ordered_ready); watch(ordered_failed);
    reset_route(); fail_during = "text";
    const size_t ready_offset = ready_deliveries.size(), failed_offset = failed_deliveries.size();
    CHECK(service.add(ordered_ready, "{\"maps\":{}}", &first) == TSUCHI_OK);
    CHECK(service.add(ordered_failed, "{\"text\":{}}", &second) == TSUCHI_OK);
    content_step();
    CHECK(mutations.size() == 2 && mutations[0] == "maps" && mutations[1] == "text");
    CHECK(ready_deliveries.size() == ready_offset + 1 && failed_deliveries.size() == failed_offset + 1);
    CHECK(ready_deliveries.back().owner == ordered_ready && ready_deliveries.back().declaration.id == first.id);
    CHECK(failed_deliveries.back().owner == ordered_failed && failed_deliveries.back().declaration.id == second.id);
    CHECK(foreign_ready_events == 0 && foreign_failed_events == 0);
    tsuchi::mod_stop(ordered_ready); tsuchi::mod_stop(ordered_failed);
    tsuchi::mod_stop(reader);
    tsuchi::mod_stop(observer);
    puts("content declarations, metadata, and adapters passed");
}
