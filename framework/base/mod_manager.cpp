#include "mod_manager.h"
#include "mods.h"
#include <string.h>
namespace tsuchi {
namespace {
TSUCHI_Result copy_choices(const ModChoices& source, ModChoices& out) {
    Text activation, order;
    TSUCHI_Result result = TSUCHI_NO_MEMORY;
    if (source.activation_text(activation) && source.order_text(order))
        result = out.load(activation.get(), order.get());
    activation.clear(); order.clear();
    return result;
}
bool contains(const List<ModChange>& list, const char* id) {
    for (uint32_t i = 0; i < list.count; ++i)
        if (!strcmp(list[i].id, id)) return true;
    return false;
}
bool add_change(List<ModChange>& list, const char* id, bool enabled) {
    if (contains(list, id)) return true;
    ModChange change{};
    memcpy(change.id, id, strlen(id) + 1); change.enabled = enabled;
    return list.add(change) != nullptr;
}
const Json* section(const ModRecord& record, const char* key) {
    return record.manifest ? record.manifest->find(key) : nullptr;
}
bool present(const ModRecord* record, const ModChoices& choices) {
    return record && record->manifest_valid && record->manifest && choices.enabled(record->id);
}
struct Node { const ModRecord* record; bool placed; };
TSUCHI_Result validate_graph(const ModChoices& choices, Text& problem) {
    List<Node> nodes;
    for (uint32_t i = 0; i < mod_count(); ++i) {
        const ModRecord* record = mod_at(i);
        if (present(record, choices) && !nodes.add({record, false})) {
            nodes.clear(); return TSUCHI_NO_MEMORY;
        }
    }
    for (uint32_t i = 0; i < nodes.count; ++i) {
        const ModRecord& record = *nodes[i].record;
        const Json* conflicts = section(record, "conflicts");
        for (const Json* other = conflicts ? conflicts->first : nullptr; other; other = other->next)
            if (present(find_mod(other->text), choices)) {
                problem.set("The enabled mods conflict."); nodes.clear(); return TSUCHI_INVALID;
            }
        const Json* optional = section(record, "optional");
        for (const Json* need = optional ? optional->first : nullptr; need; need = need->next) {
            const ModRecord* other = find_mod(need->name);
            if (present(other, choices) && !mod_requirement_satisfied(*other, need->text)) {
                problem.set("An enabled optional dependency has an incompatible version.");
                nodes.clear(); return TSUCHI_INVALID;
            }
        }
    }
    // Optional dependencies also constrain the loader's order when present.
    uint32_t placed = 0;
    while (placed < nodes.count) {
        bool progress = false;
        for (uint32_t i = 0; i < nodes.count; ++i) {
            if (nodes[i].placed) continue;
            bool ready = true;
            const char* const kinds[] = {"requires", "optional"};
            for (const char* key : kinds) {
                const Json* needs = section(*nodes[i].record, key);
                for (const Json* need = needs ? needs->first : nullptr; need; need = need->next)
                    for (uint32_t j = 0; j < nodes.count; ++j)
                        if (!strcmp(nodes[j].record->id, need->name) && !nodes[j].placed) ready = false;
            }
            if (ready) { nodes[i].placed = true; ++placed; progress = true; }
        }
        if (!progress) {
            problem.set("The enabled mods have a circular dependency.");
            nodes.clear(); return TSUCHI_INVALID;
        }
    }
    nodes.clear(); return TSUCHI_OK;
}
} // namespace
TSUCHI_Result ModManager::begin(const ModChoices& saved) {
    if (busy_) return TSUCHI_BUSY;
    ModChoices staged;
    Text activation, order;
    TSUCHI_Result result = copy_choices(saved, staged);
    if (result == TSUCHI_OK && (!saved.activation_text(activation) || !saved.order_text(order)))
        result = TSUCHI_NO_MEMORY;
    if (result == TSUCHI_OK) {
        cleanup(); draft_ = staged; staged = ModChoices{};
        activation_ = activation; activation = Text{};
        order_ = order; order = Text{}; begun_ = true;
    }
    staged.clear(); activation.clear(); order.clear(); return result;
}
TSUCHI_Result ModManager::propose(const char* id, bool enabled) {
    if (busy_ || pending()) return TSUCHI_BUSY;
    if (!begun_) return TSUCHI_NOT_READY;
    if (!valid_mod_id(id)) return TSUCHI_INVALID;
    ModRecord* target = find_mod(id);
    if (!target) return TSUCHI_NOT_FOUND;
    problem_.clear();
    ModChoices staged;
    List<ModChange> affected, changed;
    TSUCHI_Result result = copy_choices(draft_, staged);
    if (result == TSUCHI_OK && !add_change(affected, id, enabled)) result = TSUCHI_NO_MEMORY;
    if (enabled) {
        for (uint32_t i = 0; result == TSUCHI_OK && i < affected.count; ++i) {
            const ModRecord* record = find_mod(affected[i].id);
            if (!record || !record->manifest_valid || !record->manifest) {
                problem_.set("A required mod is missing or has an invalid manifest.");
                result = TSUCHI_NOT_READY; break;
            }
            result = staged.set_enabled(record->id, true);
            const Json* needs = section(*record, "requires");
            for (const Json* need = needs ? needs->first : nullptr; result == TSUCHI_OK && need; need = need->next) {
                const ModRecord* provider = find_mod(need->name);
                if (!provider || !provider->manifest_valid || !provider->manifest) {
                    problem_.set("A required mod is missing or has an invalid manifest.");
                    result = TSUCHI_NOT_READY;
                } else if (!mod_requirement_satisfied(*provider, need->text)) {
                    problem_.set("A required mod has an incompatible version."); result = TSUCHI_INVALID;
                } else if (!add_change(affected, provider->id, true)) result = TSUCHI_NO_MEMORY;
            }
        }
        if (result == TSUCHI_OK) result = validate_graph(staged, problem_);
    } else {
        // Walk the requested-enabled dependents to a fixed point, even when blocked.
        for (uint32_t i = 0; result == TSUCHI_OK && i < affected.count; ++i)
            for (uint32_t j = 0; result == TSUCHI_OK && j < mod_count(); ++j) {
                const ModRecord* record = mod_at(j);
                if (!record || !draft_.enabled(record->id)) continue;
                const Json* needs = section(*record, "requires");
                for (const Json* need = needs ? needs->first : nullptr; result == TSUCHI_OK && need; need = need->next)
                    if (!strcmp(need->name, affected[i].id) && !add_change(affected, record->id, false))
                        result = TSUCHI_NO_MEMORY;
            }
        for (uint32_t i = 0; result == TSUCHI_OK && i < affected.count; ++i)
            result = staged.set_enabled(affected[i].id, false);
    }
    for (uint32_t i = 0; result == TSUCHI_OK && i < affected.count; ++i)
        if (draft_.enabled(affected[i].id) != enabled && !add_change(changed, affected[i].id, enabled))
            result = TSUCHI_NO_MEMORY;
    if (result == TSUCHI_OK && changed.count) {
        proposed_.clear(); proposed_ = staged; staged = ModChoices{};
        changes_.clear(); changes_ = changed; changed = List<ModChange>{};
    }
    staged.clear(); affected.clear(); changed.clear(); return result;
}
TSUCHI_Result ModManager::confirm() {
    if (busy_) return TSUCHI_BUSY;
    if (!pending()) return TSUCHI_NOT_FOUND;
    draft_.clear(); draft_ = proposed_; proposed_ = ModChoices{};
    changes_.clear(); problem_.clear(); return TSUCHI_OK;
}
TSUCHI_Result ModManager::cancel() {
    if (busy_) return TSUCHI_BUSY;
    if (!pending()) return TSUCHI_NOT_FOUND;
    proposed_.clear(); changes_.clear(); problem_.clear(); return TSUCHI_OK;
}
TSUCHI_Result ModManager::move(const char* id, const char* before) {
    if (busy_ || pending()) return TSUCHI_BUSY;
    if (!begun_) return TSUCHI_NOT_READY;
    if (!valid_mod_id(id) || !before || (*before && !valid_mod_id(before))) return TSUCHI_INVALID;
    if (!find_mod(id) || (*before && !find_mod(before))) return TSUCHI_NOT_FOUND;
    if (!strcmp(id, before)) return TSUCHI_OK;
    ModChoices staged;
    TSUCHI_Result result = copy_choices(draft_, staged);
    for (uint32_t i = 0; result == TSUCHI_OK && i < mod_count(); ++i) {
        const ModRecord* record = mod_at(i);
        if (record && staged.rank(record->id) == UINT32_MAX) result = staged.move(record->id, "");
    }
    if (result == TSUCHI_OK) result = staged.move(id, before);
    if (result == TSUCHI_OK) { draft_.clear(); draft_ = staged; staged = ModChoices{}; }
    staged.clear(); return result;
}
bool ModManager::changed() const {
    if (!begun_) return false;
    Text activation, order;
    const bool same = draft_.activation_text(activation) && draft_.order_text(order) &&
        !strcmp(activation.get(), activation_.get()) && !strcmp(order.get(), order_.get());
    activation.clear(); order.clear(); return !same;
}
TSUCHI_Result ModManager::defaults() {
    if (busy_ || pending()) return TSUCHI_BUSY;
    if (!begun_) return TSUCHI_NOT_READY;
    ModChoices defaults;
    problem_.clear();
    const TSUCHI_Result result = validate_graph(defaults, problem_);
    if (result != TSUCHI_OK) return result;
    draft_.swap(defaults); defaults.clear();
    return TSUCHI_OK;
}
TSUCHI_Result ModManager::save(TSUCHI_Result (*write)(const ModChoices&)) {
    if (busy_ || pending()) return TSUCHI_BUSY;
    if (!begun_) return TSUCHI_NOT_READY;
    if (!write) return TSUCHI_INVALID;
    Text activation, order;
    if (!draft_.activation_text(activation) || !draft_.order_text(order)) {
        activation.clear(); order.clear(); return TSUCHI_NO_MEMORY;
    }
    busy_ = true;
    const TSUCHI_Result result = write(draft_);
    busy_ = false;
    if (result == TSUCHI_OK) {
        activation_.clear(); activation_ = activation; activation = Text{};
        order_.clear(); order_ = order; order = Text{};
    }
    activation.clear(); order.clear(); return result;
}
void ModManager::cleanup() {
    if (busy_) return;
    draft_.clear(); proposed_.clear(); changes_.clear();
    activation_.clear(); order_.clear(); problem_.clear(); begun_ = false;
}
} // namespace tsuchi
