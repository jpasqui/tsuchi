// The player's draft activation and order, with dependency changes reviewed together.
#pragma once
#include "mod_choices.h"
namespace tsuchi {
struct ModRecord;
struct ModChange { char id[kModIdBytes]; bool enabled; };
class ModManager {
  public:
    TSUCHI_Result begin(const ModChoices& saved);
    // Extend the player's catalogue without changing the running loader or the
    // draft. Descriptions stay alive until replaced or cleanup runs.
    TSUCHI_Result additional(const ModRecord* const* records, uint32_t count);
    TSUCHI_Result propose(const char* mod_id, bool enabled);
    const List<ModChange>& changes() const { return changes_; }
    TSUCHI_Result confirm();
    TSUCHI_Result cancel();
    TSUCHI_Result move(const char* mod_id, const char* before);
    // Restores enabled/ID order as an unsaved draft after the caller's review.
    TSUCHI_Result defaults();
    TSUCHI_Result save(TSUCHI_Result (*write)(const ModChoices&));
    const ModChoices& draft() const { return draft_; }
    bool pending() const { return changes_.count != 0; }
    bool changed() const;
    const char* problem() const { return problem_.get(); }
    void cleanup();
  private:
    ModChoices draft_, proposed_;
    Text activation_, order_, problem_;
    List<ModChange> changes_;
    List<const ModRecord*> additional_;
    bool begun_ = false, busy_ = false;
};
} // namespace tsuchi
