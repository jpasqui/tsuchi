// External installed-file observations and requests for the prelaunch companion.
#pragma once
#include "list.h"
#include "text.h"

namespace tsuchi {
constexpr uint32_t kExternalModsLimit = 256;
constexpr uint64_t kExternalCatalogBytes = 1024 * 1024;
constexpr uint64_t kExternalRequestsBytes = 256 * 1024;
enum class ExternalModKind : uint8_t { romfs, exefs, mixed, cheats };
enum class ExternalModState : uint8_t { enabled, disabled, conflict, recovery };
struct ExternalModEntry {
    char id[kModIdBytes];
    Text name, version, problem;
    ExternalModKind kind;
    ExternalModState observed;
    bool manageable, requested, saved;
};
class ExternalMods {
  public:
    uint32_t count() const { return entries_.count; }
    ExternalModEntry* at(uint32_t index) { return index < count() ? &entries_[index] : nullptr; }
    const ExternalModEntry* at(uint32_t index) const { return index < count() ? &entries_[index] : nullptr; }
    ExternalModEntry* find(const char* id);
    const ExternalModEntry* find(const char* id) const;
    // A bad catalog preserves the previous snapshot. Bad requests publish the valid
    // catalog with observed defaults and return INVALID; no partial request is used.
    TSUCHI_Result load(const char* catalog, uint64_t size, const char* requests = nullptr,
                       uint64_t request_size = 0);
    TSUCHI_Result request(const char* id, bool enabled);
    bool changed() const; // draft differs from saved requests
    bool queued() const; // saved requests differ from observed files
    void discard();
    // Advances saved requests only after the writer succeeds. File state stays observed.
    TSUCHI_Result save(TSUCHI_Result (*write)(const char*, uint64_t));
    bool requests_json(Text& out) const;
    const char* revision() const { return revision_; }
    // The saved requests exist but could not be read yet. Until a later load reads them, request and save
    // answer NOT_READY, so the observed defaults never overwrite a request that was not applied.
    void requests_unread() { unread_ = true; }
    bool unread() const { return unread_; }
    void clear();
  private:
    List<ExternalModEntry> entries_;
    char revision_[65]{};
    bool unread_ = false;
};
} // namespace tsuchi
