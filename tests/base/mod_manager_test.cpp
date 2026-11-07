#include "mod_manager.h"
#include "mods.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
using namespace tsuchi;
namespace {
ModRecord records[96]{};
uint32_t count = 0;
Json json[1024]{};
uint32_t used = 0;
Json* node(Json::Kind kind, const char* name = nullptr, const char* text = nullptr) {
    assert(used < 1024);
    Json* out = &json[used++]; *out = Json{}; out->kind = kind; out->name = name; out->text = text;
    return out;
}
void append(Json* parent, Json* child) {
    if (!parent->first) parent->first = child;
    else { Json* last = const_cast<Json*>(parent->first); while (last->next) last = const_cast<Json*>(last->next); last->next = child; }
    ++parent->count;
}
ModRecord& installed(const char* id, bool valid = true) {
    assert(count < 96); ModRecord& record = records[count++]; record = ModRecord{};
    std::memcpy(record.id, id, std::strlen(id) + 1); record.parts[0] = 1;
    record.manifest_valid = valid; record.manifest = node(Json::Kind::object);
    return record;
}
void edge(ModRecord& from, const char* kind, const char* id, const char* version = "*") {
    Json* root = const_cast<Json*>(from.manifest);
    Json* section = const_cast<Json*>(root->find(kind));
    if (!section) { section = node(!std::strcmp(kind, "conflicts") ? Json::Kind::list : Json::Kind::object, kind); append(root, section); }
    append(section, node(Json::Kind::text, id, !std::strcmp(kind, "conflicts") ? id : version));
}
void reset() { count = 0; used = 0; }
uint32_t allocations_left = 0;
void* fail_after(size_t bytes) {
    if (!allocations_left) return nullptr;
    --allocations_left; return std::malloc(bytes);
}
ModChoices written;
TSUCHI_Result write_fail(const ModChoices&) { return TSUCHI_NOT_READY; }
TSUCHI_Result write_ok(const ModChoices& choices) {
    Text activation, order;
    assert(choices.activation_text(activation) && choices.order_text(order));
    TSUCHI_Result result = written.load(activation.get(), order.get());
    activation.clear(); order.clear(); return result;
}
ModManager* during_save = nullptr;
TSUCHI_Result write_nested(const ModChoices& choices) {
    assert(during_save->propose("leaf", false) == TSUCHI_BUSY);
    assert(during_save->move("leaf", "") == TSUCHI_BUSY);
    assert(during_save->begin(choices) == TSUCHI_BUSY);
    during_save->cleanup();
    return write_ok(choices);
}
}
namespace tsuchi {
const Json* Json::find(const char* key) const {
    for (const Json* child = first; child; child = child->next)
        if (child->name && !std::strcmp(child->name, key)) return child;
    return nullptr;
}
uint32_t mod_count() { return count; }
ModRecord* mod_at(uint32_t index) { return index < count ? &records[index] : nullptr; }
ModRecord* find_mod(const char* id) {
    for (uint32_t i = 0; i < count; ++i) if (!std::strcmp(records[i].id, id)) return &records[i];
    return nullptr;
}
bool mod_requirement_satisfied(const ModRecord& record, const char* wanted) {
    return !std::strcmp(wanted, "*") || (record.parts[0] == 1 && !std::strcmp(wanted, "1.0.0"));
}
}
int main() {
    ModManager manager; ModChoices saved;
    assert(manager.propose("leaf", true) == TSUCHI_NOT_READY);
    assert(manager.move("leaf", "") == TSUCHI_NOT_READY);
    assert(manager.save(write_ok) == TSUCHI_NOT_READY);
    assert(!manager.pending() && !manager.changed());
    installed("base"); auto& middle = installed("middle"); auto& leaf = installed("leaf");
    edge(middle, "requires", "base"); edge(leaf, "requires", "middle");
    assert(saved.load("p4a1:base:middle:leaf:absent.mod:", "p4o1:absent.mod:") == TSUCHI_OK);
    assert(manager.begin(saved) == TSUCHI_OK);
    assert(manager.move("leaf", "leaf") == TSUCHI_OK && !manager.changed());
    assert(manager.propose("bad/name", true) == TSUCHI_INVALID);
    assert(manager.propose("missing", true) == TSUCHI_NOT_FOUND);
    assert(manager.confirm() == TSUCHI_NOT_FOUND && manager.cancel() == TSUCHI_NOT_FOUND);
    assert(manager.propose("leaf", true) == TSUCHI_OK && manager.changes().count == 3);
    assert(!manager.draft().enabled("leaf") && !manager.changed());
    assert(!std::strcmp(manager.changes()[0].id, "leaf") && manager.changes()[0].enabled);
    assert(manager.propose("base", false) == TSUCHI_BUSY);
    assert(manager.move("leaf", "base") == TSUCHI_BUSY);
    assert(manager.save(write_ok) == TSUCHI_BUSY);
    assert(manager.cancel() == TSUCHI_OK && !manager.pending() && !manager.changed());
    assert(manager.propose("leaf", true) == TSUCHI_OK && manager.confirm() == TSUCHI_OK);
    assert(manager.draft().enabled("base") && manager.draft().enabled("middle") && manager.draft().enabled("leaf"));
    assert(!manager.draft().enabled("absent.mod") && manager.changed());
    assert(manager.save(nullptr) == TSUCHI_INVALID);
    assert(manager.save(write_fail) == TSUCHI_NOT_READY && manager.changed());
    assert(manager.draft().enabled("leaf"));
    during_save = &manager;
    assert(manager.save(write_nested) == TSUCHI_OK && !manager.changed());
    assert(written.enabled("leaf") && !written.enabled("absent.mod") && written.rank("absent.mod") == 0);
    // Disabling a provider reviews all requested dependents, without changing runtime records.
    assert(manager.propose("base", false) == TSUCHI_OK && manager.changes().count == 3);
    assert(manager.cancel() == TSUCHI_OK && manager.draft().enabled("leaf"));
    assert(manager.propose("base", false) == TSUCHI_OK && manager.confirm() == TSUCHI_OK);
    assert(!manager.draft().enabled("base") && !manager.draft().enabled("middle") && !manager.draft().enabled("leaf"));
    assert(manager.propose("leaf", false) == TSUCHI_OK && !manager.pending());
    assert(manager.move("leaf", "base") == TSUCHI_OK);
    assert(manager.draft().rank("absent.mod") == 0 && manager.draft().rank("leaf") == 1 && manager.draft().rank("base") == 2);
    assert(manager.draft().rank("middle") == 3 && !manager.draft().enabled("leaf"));
    assert(manager.move("leaf", "missing") == TSUCHI_NOT_FOUND);
    assert(manager.move("leaf", nullptr) == TSUCHI_INVALID);
    assert(manager.save(write_fail) == TSUCHI_NOT_READY && manager.draft().rank("leaf") == 1);
    assert(manager.save(write_ok) == TSUCHI_OK && !manager.changed());
    // Snapshot allocation fails before any draft or baseline can change.
    set_allocator({[](size_t) -> void* { return nullptr; }, std::free});
    assert(manager.begin(saved) == TSUCHI_NO_MEMORY);
    assert(manager.propose("leaf", true) == TSUCHI_NO_MEMORY && !manager.pending());
    assert(manager.move("leaf", "base") == TSUCHI_NO_MEMORY);
    assert(manager.save(write_ok) == TSUCHI_NO_MEMORY);
    set_allocator({std::malloc, std::free});
    assert(!manager.changed() && manager.draft().rank("leaf") == 1);
    manager.cleanup();
    // Refuse at each allocation stage, including after the dependency closure is built.
    for (uint32_t budget = 0; budget < 12; ++budget) {
        assert(manager.begin(saved) == TSUCHI_OK);
        allocations_left = budget; set_allocator({fail_after, std::free});
        const TSUCHI_Result proposed = manager.propose("leaf", true);
        set_allocator({std::malloc, std::free});
        assert(proposed == TSUCHI_NO_MEMORY || proposed == TSUCHI_OK);
        assert(!manager.draft().enabled("leaf") && !manager.changed());
        if (proposed == TSUCHI_OK) assert(manager.cancel() == TSUCHI_OK);
        else assert(!manager.pending());
        manager.cleanup();
    }
    saved.clear(); written.clear(); reset();
    // Missing, invalid, and incompatible required dependencies each refuse the whole proposal.
    auto& dependent = installed("dependent"); edge(dependent, "requires", "provider", "1.0.0");
    assert(manager.begin(saved) == TSUCHI_OK);
    assert(manager.propose("dependent", true) == TSUCHI_NOT_READY && !manager.pending() && *manager.problem());
    auto& provider = installed("provider", false);
    assert(manager.propose("dependent", true) == TSUCHI_NOT_READY && !manager.pending());
    provider.manifest_valid = true; provider.parts[0] = 2;
    assert(manager.propose("dependent", true) == TSUCHI_INVALID && !manager.pending());
    // A disable still succeeds when an independent invalid manifest remains requested.
    installed("broken", false);
    assert(manager.propose("provider", false) == TSUCHI_OK && manager.changes().count == 2);
    assert(manager.confirm() == TSUCHI_OK && manager.draft().enabled("broken"));
    manager.cleanup(); reset();
    // Conflicts are tested in both directions against the resulting enabled graph.
    auto& left = installed("left"); installed("right"); edge(left, "conflicts", "right");
    assert(saved.load("p4a1:right:", "") == TSUCHI_OK && manager.begin(saved) == TSUCHI_OK);
    assert(manager.propose("right", true) == TSUCHI_INVALID && !manager.pending());
    assert(saved.load("p4a1:left:", "") == TSUCHI_OK && manager.begin(saved) == TSUCHI_OK);
    assert(manager.propose("left", true) == TSUCHI_INVALID && !manager.pending());
    manager.cleanup(); saved.clear(); reset();
    auto& optional = installed("optional"); auto& compatible = installed("provider");
    edge(optional, "optional", "provider", "1.0.0"); compatible.parts[0] = 2;
    assert(saved.load("p4a1:optional:", "") == TSUCHI_OK && manager.begin(saved) == TSUCHI_OK);
    assert(manager.propose("optional", true) == TSUCHI_INVALID && !manager.pending());
    // A disabled or absent optional dependency does not auto-enable.
    assert(saved.load("p4a1:optional:provider:", "") == TSUCHI_OK && manager.begin(saved) == TSUCHI_OK);
    assert(manager.propose("optional", true) == TSUCHI_OK && manager.changes().count == 1);
    assert(manager.confirm() == TSUCHI_OK && !manager.draft().enabled("provider"));
    manager.cleanup(); saved.clear(); reset();
    auto& circle_a = installed("circle.a"); auto& circle_b = installed("circle.b");
    edge(circle_a, "requires", "circle.b"); edge(circle_b, "requires", "circle.a");
    assert(saved.load("p4a1:circle.a:circle.b:", "") == TSUCHI_OK && manager.begin(saved) == TSUCHI_OK);
    assert(manager.propose("circle.a", true) == TSUCHI_INVALID && !manager.pending());
    manager.cleanup(); reset();
    auto& optional_a = installed("circle.a"); auto& optional_b = installed("circle.b");
    edge(optional_a, "optional", "circle.b"); edge(optional_b, "optional", "circle.a");
    assert(manager.begin(saved) == TSUCHI_OK);
    assert(manager.propose("circle.a", true) == TSUCHI_OK && manager.confirm() == TSUCHI_OK);
    assert(manager.propose("circle.b", true) == TSUCHI_INVALID && !manager.pending());
    manager.cleanup(); saved.clear(); reset();
    // A long dependency chain has no fixed mod-mask limit.
    char names[40][kModIdBytes]; Text activation;
    assert(activation.reserve(512)); std::memcpy(activation.data, "p4a1:", 6);
    for (uint32_t i = 0; i < 40; ++i) {
        std::snprintf(names[i], sizeof names[i], "chain.%u", i);
        auto& record = installed(names[i]);
        if (i) edge(record, "requires", names[i - 1]);
        const size_t at = std::strlen(activation.data), length = std::strlen(names[i]);
        std::memcpy(activation.data + at, names[i], length);
        activation.data[at + length] = ':'; activation.data[at + length + 1] = 0;
    }
    assert(saved.load(activation.get(), "") == TSUCHI_OK && manager.begin(saved) == TSUCHI_OK);
    assert(manager.propose(names[39], true) == TSUCHI_OK && manager.changes().count == 40);
    assert(manager.confirm() == TSUCHI_OK);
    assert(manager.propose(names[0], false) == TSUCHI_OK && manager.changes().count == 40);
    assert(manager.confirm() == TSUCHI_OK && !manager.changed());
    manager.cleanup(); saved.clear(); activation.clear();
}
