// The player's requested activation and order, read before containers mount.
#pragma once
#include "list.h"
#include "text.h"
namespace tsuchi {
struct ModChoice { char id[kModIdBytes]; bool enabled; uint32_t rank; };
class ModChoices {
  public:
    // Activation uses the p4a1: format. Order uses p4o1:.
    // Both values are validated before either is published. Missing values are empty.
    TSUCHI_Result load(const char* activation, const char* order);
    bool enabled(const char* id) const;
    uint32_t rank(const char* id) const;
    TSUCHI_Result set_enabled(const char* id, bool value);
    // Places id before another ID, or at the end when before is empty.
    TSUCHI_Result move(const char* id, const char* before);
    bool activation_text(Text& out) const;
    bool order_text(Text& out) const;
    void clear() { choices_.clear(); }
    void swap(ModChoices& other) { auto before = choices_; choices_ = other.choices_; other.choices_ = before; }
  private:
    List<ModChoice> choices_;
    ModChoice* find(const char* id);
    const ModChoice* find(const char* id) const;
};
// The game installs the loader once. No loader uses the default enabled/ID order.
using ModChoicesLoader = TSUCHI_Result (*)(ModChoices& choices);
void set_mod_choices_loader(ModChoicesLoader loader);
TSUCHI_Result load_mod_choices();
const ModChoices& mod_choices();
void swap_mod_choices(ModChoices& replacement);
} // namespace tsuchi
