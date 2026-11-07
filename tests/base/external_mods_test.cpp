#include "external_mods.h"
#include "json.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace tsuchi;
namespace {
const std::string rev(64, 'a');
std::string item(const std::string& id = "external.onion", const std::string& state = "enabled",
                 const std::string& kind = "romfs", bool manageable = true) {
    return "{\"id\":\"" + id + "\",\"name\":\"Onion\",\"version\":\"1.0\",\"kind\":\"" + kind +
           "\",\"state\":\"" + state + "\",\"problem\":\"\",\"manageable\":" + (manageable ? "true}" : "false}");
}
std::string catalog(const std::string& entries = item(), const std::string& revision = rev) {
    return "{\"format\":1,\"revision\":\"" + revision + "\",\"backend\":\"companion\",\"mods\":[" + entries + "]}";
}
std::string requests(const std::string& enabled, const std::string& revision = rev) {
    return "{\"format\":1,\"revision\":\"" + revision + "\",\"enabled\":{" + enabled + "}}";
}
TSUCHI_Result load(ExternalMods& mods, const std::string& source) {
    return mods.load(source.data(), source.size());
}
TSUCHI_Result load(ExternalMods& mods, const std::string& source, const std::string& saved) {
    return mods.load(source.data(), source.size(), saved.data(), saved.size());
}
std::string replace(std::string source, const std::string& before, const std::string& after) {
    const size_t at = source.find(before); assert(at != std::string::npos);
    source.replace(at, before.size(), after); return source;
}
void observed_requests() {
    ExternalMods mods;
    const auto source = catalog(item() + "," + item("external.patch", "disabled", "exefs"));
    assert(load(mods, source) == TSUCHI_OK);
    assert(mods.count() == 2 && !mods.changed() && !mods.queued());
    assert(!std::strcmp(mods.revision(), rev.c_str()));
    assert(mods.at(0)->observed == ExternalModState::enabled && mods.at(0)->requested && mods.at(0)->saved);
    assert(mods.at(1)->observed == ExternalModState::disabled && !mods.at(1)->requested);
    assert(!mods.at(2) && !mods.find(nullptr) && !mods.find("absent"));
    assert(mods.request("absent", true) == TSUCHI_NOT_FOUND);
    assert(mods.request("external.onion", false) == TSUCHI_OK && mods.changed() && !mods.queued());
    assert(mods.at(0)->observed == ExternalModState::enabled && mods.at(0)->saved);
    mods.discard(); assert(!mods.changed() && mods.at(0)->requested);
    const auto saved = requests("\"external.onion\":false,\"external.patch\":true");
    assert(load(mods, source, saved) == TSUCHI_OK && !mods.changed() && mods.queued());
    assert(!mods.at(0)->requested && !mods.at(0)->saved && mods.at(0)->observed == ExternalModState::enabled);
    assert(mods.at(1)->requested && mods.at(1)->observed == ExternalModState::disabled);
    mods.clear();
    assert(!mods.count() && !*mods.revision() && !mods.changed() && !mods.queued());
}
void invalid_catalogs() {
    ExternalMods mods;
    const auto source = catalog();
    assert(load(mods, source, requests("\"external.onion\":false")) == TSUCHI_OK);
    const std::string invalid[] = {
        "", "null", "[]", "{}", source + "true", replace(source, "\"format\":1", "\"format\":1.0"),
        replace(source, "\"format\":1", "\"format\":2"), replace(source, "companion", "pretend"),
        replace(source, rev, std::string(64, 'A')), replace(source, rev, std::string(63, 'a')),
        replace(source, "\"format\":1", "\"format\":1,\"unknown\":true"),
        replace(source, "\"name\":\"Onion\"", "\"name\":\"Onion\",\"unknown\":true"),
        replace(source, "\"format\":1", "\"format\":1,\"format\":1"),
        replace(source, "\"name\":\"Onion\"", "\"name\":\"Onion\",\"name\":\"Other\""),
        replace(source, "\"name\":\"Onion\",", ""),
        catalog(item("native.onion")), catalog(item("a")), catalog(item("external.")), catalog(item("external.Bad")),
        catalog(item("external.onion", "unknown")), catalog(item("external.onion", "enabled", "unknown")),
        replace(source, "\"manageable\":true", "\"manageable\":1"), catalog(item() + "," + item()),
        replace(source, "\"name\":\"Onion\"", "\"name\":null"),
        replace(source, "\"name\":\"Onion\"", "\"name\":\"\""),
        replace(source, "\"name\":\"Onion\"", "\"name\":\"Onion\\u0000hidden\""),
        replace(source, "\"name\":\"Onion\"", "\"name\":\"Onion\\nnext\""),
        replace(source, "\"version\":\"1.0\"", "\"version\":\"\\u007f\""),
        replace(source, "\"problem\":\"\"", "\"problem\":\"\\u0080\"")
    };
    for (const auto& bad : invalid) {
        assert(load(mods, bad) == TSUCHI_INVALID);
        assert(mods.count() == 1 && mods.queued() && !mods.at(0)->saved && !mods.at(0)->requested);
    }
    assert(mods.load(nullptr, 1) == TSUCHI_INVALID && mods.count() == 1);
    assert(mods.load(source.data(), kExternalCatalogBytes + 1) == TSUCHI_INVALID && mods.count() == 1);
    mods.clear();
}
void bounds() {
    ExternalMods mods;
    const auto source = catalog();
    const std::pair<std::string, size_t> fields[] = {{"name", 128}, {"version", 64}, {"problem", 512}};
    for (const auto& field : fields) {
        const auto old = field.first == "name" ? "Onion" : field.first == "version" ? "1.0" : "";
        const auto prefix = "\"" + field.first + "\":\"";
        assert(load(mods, replace(source, prefix + old + "\"", prefix + std::string(field.second, 'x') + "\"")) == TSUCHI_OK);
        assert(load(mods, replace(source, prefix + old + "\"", prefix + std::string(field.second + 1, 'x') + "\"")) == TSUCHI_INVALID);
    }
    std::string all;
    for (uint32_t i = 0; i < kExternalModsLimit; ++i) {
        if (i) all += ',';
        all += item("external.pack-" + std::to_string(i), i % 2 ? "enabled" : "disabled");
    }
    assert(load(mods, catalog(all)) == TSUCHI_OK && mods.count() == kExternalModsLimit);
    assert(load(mods, catalog(all + "," + item("external.last"))) == TSUCHI_INVALID && mods.count() == kExternalModsLimit);
    // The byte ceiling includes whitespace and admits exactly the limit.
    auto padded = source + std::string(size_t(kExternalCatalogBytes) - source.size(), ' ');
    assert(load(mods, padded) == TSUCHI_OK && mods.count() == 1);
    padded += ' '; assert(load(mods, padded) == TSUCHI_INVALID);
    mods.clear();
}
void retained_requests();
void invalid_requests() {
    ExternalMods mods;
    const auto source = catalog(item() + "," + item("external.patch", "disabled", "exefs"));
    const std::string invalid[] = {
        "", "null", "{}", requests("\"external.onion\":false", std::string(64, 'b')),
        requests("\"absent\":true"), requests("\"external.onion\":false,\"external.patch\":1"),
        requests("\"external.onion\":false,\"external.onion\":true"),
        replace(requests(""), "\"enabled\":{}", "\"enabled\":[]"),
        replace(requests(""), "\"format\":1", "\"format\":1,\"unknown\":0"),
        replace(requests(""), "\"format\":1", "\"format\":1e0")
    };
    for (const auto& bad : invalid) {
        // A previous saved request must not survive a rejected replacement.
        assert(load(mods, source, requests("\"external.onion\":false")) == TSUCHI_OK && mods.queued());
        assert(load(mods, source, bad) == TSUCHI_INVALID);
        assert(mods.count() == 2 && !mods.changed() && !mods.queued());
        assert(mods.at(0)->requested && mods.at(0)->saved && !mods.at(1)->requested);
    }
    const auto saved = requests("\"external.onion\":false");
    assert(mods.load(source.data(), source.size(), saved.data(), kExternalRequestsBytes + 1) == TSUCHI_INVALID);
    assert(!mods.queued() && mods.at(0)->requested);
    assert(mods.load(source.data(), source.size(), nullptr, 1) == TSUCHI_INVALID && !mods.queued());
    auto padded = saved + std::string(size_t(kExternalRequestsBytes) - saved.size(), ' ');
    assert(load(mods, source, padded) == TSUCHI_OK && mods.queued());
    mods.clear();
    retained_requests();
}
void blocked_entries() {
    ExternalMods mods;
    const auto source = catalog(item("external.cheats", "enabled", "cheats", false) + "," +
        item("external.conflict", "conflict", "cheats") + "," + item("external.recovery", "recovery", "cheats") +
        "," + item("external.unmanaged", "disabled", "romfs", false) + "," +
        item("external.file-conflict", "conflict", "mixed") + "," + item("external.file-recovery", "recovery", "exefs"));
    assert(load(mods, source) == TSUCHI_OK && mods.count() == 6);
    for (uint32_t i = 0; i < mods.count(); ++i) {
        assert(!mods.at(i)->manageable);
        assert(mods.request(mods.at(i)->id, !mods.at(i)->requested) == TSUCHI_INVALID);
    }
    Text encoded;
    assert(mods.requests_json(encoded));
    assert(std::string(encoded.get()) == requests(""));
    assert(load(mods, source, requests("\"external.cheats\":false")) == TSUCHI_INVALID && !mods.queued());
    assert(load(mods, source, requests("\"external.conflict\":true")) == TSUCHI_INVALID && !mods.queued());
    assert(load(mods, source, requests("\"external.recovery\":true")) == TSUCHI_INVALID && !mods.queued());
    encoded.clear(); mods.clear();
}
int writes = 0;
std::string written;
TSUCHI_Result write_ok(const char* bytes, uint64_t size) { ++writes; written.assign(bytes, size_t(size)); return TSUCHI_OK; }
TSUCHI_Result write_failed(const char*, uint64_t) { ++writes; return TSUCHI_IO; }
void retained_requests() {
    ExternalMods mods;
    const std::string current(64, 'b');
    const auto entries = item() + "," + item("external.patch", "disabled", "exefs") + "," +
        item("external.code", "enabled", "cheats") + "," +
        item("external.unsupported", "enabled", "cheats", false) + "," +
        item("external.conflict", "conflict", "mixed");
    const auto source = catalog(entries, current);
    // Completeness is a set comparison; order and unrelated read-only rows do
    // not change a fulfilled request's switch differences.
    const std::string full = "\"external.code\":true,\"external.patch\":false,\"external.onion\":true";
    const auto retained = requests(full);
    const auto original = retained;
    assert(load(mods, source, retained) == TSUCHI_OK && mods.count() == 5);
    assert(!mods.changed() && !mods.queued() && !std::strcmp(mods.revision(), current.c_str()));
    for (uint32_t i = 0; i < mods.count(); ++i) {
        assert(mods.at(i)->requested == (mods.at(i)->observed == ExternalModState::enabled));
        assert(mods.at(i)->saved == mods.at(i)->requested);
    }
    const int before = writes;
    const auto old_written = written;
    assert(mods.save(write_ok) == TSUCHI_OK && writes == before && written == old_written && retained == original);

    const std::string invalid[] = {
        requests(replace(full, "\"external.onion\":true", "\"external.onion\":false")),
        requests("\"external.onion\":true,\"external.patch\":false"), requests(""),
        requests(full + ",\"external.unknown\":true"),
        requests(replace(full, "external.code", "external.unknown")),
        requests(replace(full, "external.code", "external.unsupported")),
        requests(replace(full, "\"external.code\":true", "\"external.conflict\":false")),
        requests(replace(full, "external.code", "bad/name")),
        requests(replace(full, "\"external.code\":true", "\"external.code\":1")),
        requests(replace(full, "\"external.code\":true", "\"external.code\":\"true\"")),
        requests(replace(full, "\"external.code\":true", "\"external.code\":null")),
        requests(full + ",\"external.onion\":true"),
        replace(requests(full), "\"format\":1", "\"format\":1,\"format\":1"),
        replace(requests(full), "\"format\":1", "\"format\":1,\"extra\":0"),
        replace(requests(full), "\"format\":1", "\"format\":1.0"),
        replace(requests(full), "\"enabled\":{", "\"enabled\":[],\"enabled\":{"),
        requests(full, std::string(64, 'A')), requests(full, std::string(63, 'a')),
        requests(full) + "true", requests(full).substr(0, requests(full).size() - 1)
    };
    for (const auto& bad : invalid) {
        assert(load(mods, source, requests("\"external.onion\":false", current)) == TSUCHI_OK && mods.queued());
        assert(load(mods, source, bad) == TSUCHI_INVALID);
        assert(!mods.changed() && !mods.queued());
        assert(mods.at(0)->requested && !mods.at(1)->requested && mods.at(2)->requested);
    }
    // Import/removal and newly blocked choices cannot make an earlier complete
    // request count as a complete current request.
    const std::string different_sets[] = {
        catalog(entries + "," + item("external.added", "disabled"), current),
        catalog(item() + "," + item("external.patch", "disabled", "exefs"), current),
        catalog(item() + "," + item("external.patch", "disabled", "exefs") + "," +
                item("external.code", "enabled", "cheats", false), current),
        catalog(item() + "," + item("external.patch", "disabled", "exefs") + "," +
                item("external.code", "conflict", "cheats"), current),
        catalog(item() + "," + item("external.patch", "disabled", "exefs") + "," +
                item("external.code", "recovery", "cheats"), current)
    };
    for (const auto& changed : different_sets) {
        assert(load(mods, changed, retained) == TSUCHI_INVALID && !mods.changed() && !mods.queued());
    }
    // Matching-revision partial requests retain their earlier behavior.
    assert(load(mods, source, requests("\"external.onion\":false", current)) == TSUCHI_OK && mods.queued());
    assert(!mods.at(0)->saved && !mods.at(0)->requested && !mods.at(1)->requested && mods.at(2)->requested);
    assert(load(mods, source, requests("\"external.onion\":true", current)) == TSUCHI_OK && !mods.queued());
    // The empty complete set has no differences, but including a read-only ID
    // still refuses even when its requested boolean matches the observation.
    const auto readonly = catalog(item("external.unsupported", "disabled", "cheats", false), current);
    assert(load(mods, readonly, requests("")) == TSUCHI_OK && !mods.changed() && !mods.queued());
    assert(load(mods, readonly, requests("\"external.unsupported\":false")) == TSUCHI_INVALID);
    mods.clear();
}
void save_roundtrip() {
    ExternalMods mods, reloaded;
    const auto source = catalog(item() + "," + item("external.patch", "disabled", "exefs") + "," +
        item("external.code-a", "enabled", "cheats") + "," + item("external.code-b", "disabled", "cheats"));
    assert(load(mods, source) == TSUCHI_OK);
    assert(mods.count() == 4 && mods.at(2)->manageable && mods.at(3)->manageable);
    assert(mods.at(2)->kind == ExternalModKind::cheats && mods.at(3)->kind == ExternalModKind::cheats);
    assert(mods.save(write_ok) == TSUCHI_OK && writes == 0);
    assert(mods.request("external.onion", false) == TSUCHI_OK);
    assert(mods.request("external.code-a", false) == TSUCHI_OK);
    assert(mods.request("external.code-b", true) == TSUCHI_OK);
    assert(mods.save(nullptr) == TSUCHI_INVALID && mods.changed());
    assert(mods.save(write_failed) == TSUCHI_IO && writes == 1 && mods.changed() && !mods.queued());
    assert(mods.at(0)->observed == ExternalModState::enabled && mods.at(0)->saved && !mods.at(0)->requested);
    assert(mods.at(2)->observed == ExternalModState::enabled && mods.at(2)->saved && !mods.at(2)->requested);
    assert(mods.at(3)->observed == ExternalModState::disabled && !mods.at(3)->saved && mods.at(3)->requested);
    mods.discard(); assert(!mods.changed() && mods.at(0)->requested);
    assert(mods.at(2)->requested && !mods.at(3)->requested);
    assert(mods.request("external.onion", false) == TSUCHI_OK);
    assert(mods.request("external.patch", true) == TSUCHI_OK);
    assert(mods.request("external.code-a", false) == TSUCHI_OK);
    assert(mods.request("external.code-b", true) == TSUCHI_OK);
    assert(mods.save(write_ok) == TSUCHI_OK && writes == 2 && !mods.changed() && mods.queued());
    assert(load(reloaded, source, written) == TSUCHI_OK && reloaded.queued() && !reloaded.changed());
    assert(!reloaded.at(0)->requested && reloaded.at(1)->requested);
    assert(!reloaded.at(2)->requested && !reloaded.at(2)->saved && reloaded.at(3)->requested && reloaded.at(3)->saved);
    assert(reloaded.at(2)->observed == ExternalModState::enabled && reloaded.at(3)->observed == ExternalModState::disabled);
    assert(mods.at(0)->observed == ExternalModState::enabled && mods.at(1)->observed == ExternalModState::disabled);
    assert(mods.at(2)->observed == ExternalModState::enabled && mods.at(3)->observed == ExternalModState::disabled);
    Text encoded;
    assert(mods.requests_json(encoded) && written == encoded.get());
    assert(mods.save(write_ok) == TSUCHI_OK && writes == 2);
    mods.clear(); reloaded.clear(); encoded.clear();
    assert(!mods.requests_json(encoded));
    assert(mods.load(nullptr, 0) == TSUCHI_OK);
}
size_t live = 0, allocations = 0, fail_at = SIZE_MAX;
void* tracked_allocate(size_t bytes) {
    if (allocations++ == fail_at) return nullptr;
    void* block = std::malloc(bytes); if (block) ++live; return block;
}
void tracked_release(void* block) { if (block) { assert(live); --live; std::free(block); } }
void allocation_failure() {
    set_allocator({tracked_allocate, tracked_release});
    const auto source = catalog(item() + "," + item("external.patch", "disabled", "mixed"));
    const auto saved = requests("\"external.onion\":false");
    // Every allocation point either preserves the old catalog or publishes the
    // valid catalog without applying a partial request. All memory is released.
    bool reached_success = false;
    for (size_t failure = 0; failure < 100; ++failure) {
        ExternalMods mods;
        allocations = 0; fail_at = SIZE_MAX;
        assert(load(mods, catalog(item("external.previous"))) == TSUCHI_OK);
        allocations = 0; fail_at = failure;
        const TSUCHI_Result result = load(mods, source, saved);
        assert(result == TSUCHI_OK || result == TSUCHI_NO_MEMORY);
        if (result == TSUCHI_OK) { reached_success = true; assert(mods.count() == 2 && mods.queued()); }
        else if (mods.find("external.previous")) assert(mods.count() == 1 && !mods.queued());
        else assert(mods.count() == 2 && !mods.queued() && mods.at(0)->requested);
        fail_at = SIZE_MAX; mods.clear(); assert(!live);
        if (reached_success) break;
    }
    assert(reached_success);
    ExternalMods mods;
    assert(load(mods, source) == TSUCHI_OK && mods.request("external.onion", false) == TSUCHI_OK);
    allocations = 0; fail_at = 0;
    assert(mods.save(write_ok) == TSUCHI_NO_MEMORY && mods.changed() && !mods.queued());
    fail_at = SIZE_MAX; mods.clear(); assert(!live);
    set_allocator({std::malloc, std::free});
}
} // namespace
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    if (mode == "observed") observed_requests();
    else if (mode == "catalog") invalid_catalogs();
    else if (mode == "bounds") bounds();
    else if (mode == "requests") invalid_requests();
    else if (mode == "blocked") blocked_entries();
    else if (mode == "save") save_roundtrip();
    else if (mode == "allocation") allocation_failure();
    else return 2;
}
