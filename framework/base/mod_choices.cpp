#include "mod_choices.h"
#include <string.h>
namespace tsuchi {
namespace {
ModChoices saved;
ModChoicesLoader read_saved = nullptr;
// Read a whole delimiter-terminated list of validated mod IDs.
TSUCHI_Result take(List<ModChoice>& staged, const char* text, const char* prefix, bool order) {
    if (!text || !*text) return TSUCHI_OK;
    for (uint32_t i = 0; prefix[i]; ++i) if (text[i] != prefix[i]) return TSUCHI_INVALID;
    text += strlen(prefix);
    uint32_t rank = 0;
    while (*text) {
        ModChoice item{};
        uint32_t n = 0;
        while (*text && *text != ':') {
            if (n + 1 == sizeof item.id) return TSUCHI_INVALID;
            item.id[n++] = *text++;
        }
        if (*text++ != ':' || !valid_mod_id(item.id)) return TSUCHI_INVALID;
        ModChoice* old = nullptr;
        for (uint32_t i = 0; i < staged.count; ++i)
            if (!strcmp(staged[i].id, item.id)) { old = &staged[i]; break; }
        if (old) {
            if (order ? old->rank != UINT32_MAX : !old->enabled) return TSUCHI_INVALID;
        } else {
            item.enabled = true; item.rank = UINT32_MAX;
            old = staged.add(item);
            if (!old) return TSUCHI_NO_MEMORY;
        }
        if (order) old->rank = rank++;
        else old->enabled = false;
    }
    return TSUCHI_OK;
}
bool encode(const List<ModChoice>& choices, Text& out, bool order) {
    const char* prefix = order ? "p4o1:" : "p4a1:";
    size_t bytes = strlen(prefix) + 1;
    for (uint32_t i = 0; i < choices.count; ++i)
        if (order ? choices[i].rank != UINT32_MAX : !choices[i].enabled) bytes += strlen(choices[i].id) + 1;
    Text staged;
    if (!staged.reserve(bytes)) return false;
    char* at = staged.data;
    memcpy(at, prefix, strlen(prefix)); at += strlen(prefix);
    for (uint32_t place = 0; place < choices.count; ++place)
        for (uint32_t i = 0; i < choices.count; ++i) {
            const ModChoice& choice = choices[i];
            if (order ? choice.rank != place : i != place || choice.enabled) continue;
            const size_t n = strlen(choice.id);
            memcpy(at, choice.id, n); at += n; *at++ = ':';
        }
    *at = 0;
    out.clear(); out = staged;
    return true;
}
} // namespace
ModChoice* ModChoices::find(const char* id) {
    for (uint32_t i = 0; i < choices_.count; ++i) if (!strcmp(choices_[i].id, id)) return &choices_[i];
    return nullptr;
}
const ModChoice* ModChoices::find(const char* id) const { return const_cast<ModChoices*>(this)->find(id); }
TSUCHI_Result ModChoices::load(const char* activation, const char* order) {
    List<ModChoice> staged;
    TSUCHI_Result result = take(staged, activation, "p4a1:", false);
    if (result == TSUCHI_OK) result = take(staged, order, "p4o1:", true);
    if (result != TSUCHI_OK) { staged.clear(); return result; }
    choices_.clear(); choices_ = staged;
    return TSUCHI_OK;
}
bool ModChoices::enabled(const char* id) const { const auto* choice = find(id); return !choice || choice->enabled; }
uint32_t ModChoices::rank(const char* id) const { const auto* choice = find(id); return choice ? choice->rank : UINT32_MAX; }
TSUCHI_Result ModChoices::set_enabled(const char* id, bool value) {
    if (!valid_mod_id(id)) return TSUCHI_INVALID;
    ModChoice* choice = find(id);
    if (!choice) {
        ModChoice fresh{}; memcpy(fresh.id, id, strlen(id) + 1);
        fresh.rank = UINT32_MAX;
        choice = choices_.add(fresh);
        if (!choice) return TSUCHI_NO_MEMORY;
    }
    choice->enabled = value;
    return TSUCHI_OK;
}
TSUCHI_Result ModChoices::move(const char* id, const char* before) {
    if (!valid_mod_id(id) || !before || (*before && !valid_mod_id(before))) return TSUCHI_INVALID;
    if (!strcmp(id, before)) return TSUCHI_OK;
    ModChoice* target = *before ? find(before) : nullptr;
    if (*before && (!target || target->rank == UINT32_MAX)) return TSUCHI_NOT_FOUND;
    const uint32_t wanted = target ? target->rank : UINT32_MAX;
    ModChoice* choice = find(id);
    if (!choice) {
        ModChoice fresh{}; memcpy(fresh.id, id, strlen(id) + 1);
        fresh.enabled = true; fresh.rank = UINT32_MAX;
        choice = choices_.add(fresh);
        if (!choice) return TSUCHI_NO_MEMORY;
    }
    const uint32_t old = choice->rank;
    uint32_t count = 0;
    for (uint32_t i = 0; i < choices_.count; ++i) if (choices_[i].rank != UINT32_MAX) {
        if (choices_[i].rank > old) --choices_[i].rank;
        ++count;
    }
    const uint32_t at = wanted == UINT32_MAX ? count - (old != UINT32_MAX) : wanted - (old < wanted);
    for (uint32_t i = 0; i < choices_.count; ++i)
        if (&choices_[i] != choice && choices_[i].rank != UINT32_MAX && choices_[i].rank >= at) ++choices_[i].rank;
    choice->rank = at;
    return TSUCHI_OK;
}
bool ModChoices::activation_text(Text& out) const { return encode(choices_, out, false); }
bool ModChoices::order_text(Text& out) const { return encode(choices_, out, true); }
void set_mod_choices_loader(ModChoicesLoader loader) { read_saved = loader; }
TSUCHI_Result load_mod_choices() { return read_saved ? read_saved(saved) : TSUCHI_OK; }
const ModChoices& mod_choices() { return saved; }
void swap_mod_choices(ModChoices& replacement) { saved.swap(replacement); }
} // namespace tsuchi
