#include "content_identity.h"
#include "game.h"
#include "holds.h"
#include "objects.h"
#include "reflect.h"
namespace tsuchi {
namespace {
namespace g = game;
namespace t = g::table;
// CID is an FName. Its two words retain the original spelling and numeric suffix.
// The engine's name pool owns the text for the run, so no snapshot string can expire.
struct Kept { g::Name before, wanted; bool retained; };
struct Wanted { const char* identity; };
TSUCHI_Result place(uint64_t object, g::Place& field) {
    const auto result = g::find_place(g::type_of(object), object, "CID", field);
    if (result != TSUCHI_OK) return result;
    const uint64_t kind = g::read_or<uint64_t>(field.property + t::property::kind);
    return kind && g::name_is(g::read_or<g::Name>(kind), "NameProperty") ? TSUCHI_OK : TSUCHI_UNSUPPORTED;
}
TSUCHI_Result apply(const Hold& hold, Kept& kept, const Wanted& wanted, bool fresh) {
    const uint64_t object = g::from_handle(hold.object);
    g::Place field;
    if (!object) return TSUCHI_NOT_FOUND;
    auto result = place(object, field);
    if (result != TSUCHI_OK) return result;
    if (fresh && !g::read(field.address, kept.before)) return TSUCHI_NOT_READY;
    g::Name named{};
    result = g::name_from_utf8(wanted.identity, named);
    if (result != TSUCHI_OK) return result;
    // Interning may enter native allocation. Recheck the exact object and hold.
    if (!hold_call_guard(hold).permits() || g::from_handle(hold.object) != object) return TSUCHI_NOT_READY;
    result = place(object, field);
    if (result != TSUCHI_OK) return result;
    const g::Name previous = kept.wanted;
    kept.wanted = named;
    hold_apply_checkpoint(hold);
    if (g::write(field.address, named)) return TSUCHI_OK;
    kept.wanted = previous;
    return TSUCHI_NOT_READY;
}
bool keep(const Hold& hold, Kept& kept) {
    const uint64_t object = g::from_handle(hold.object);
    g::Place field;
    if (!object) return false;
    const auto result = place(object, field);
    if (result == TSUCHI_NOT_READY) return true;
    if (result != TSUCHI_OK) return false;
    return g::write(field.address, kept.wanted);
}
void restore(const Hold& hold, Kept& kept, HoldEnd why) {
    if (kept.retained || why == HoldEnd::destroyed || why == HoldEnd::destroying) return;
    const uint64_t object = g::from_handle(hold.object);
    g::Place field;
    if (object && place(object, field) == TSUCHI_OK) g::write(field.address, kept.before);
}
Holds<Kept, Wanted, apply, keep, restore> identities{"content.class_identity", HoldOn::object, true};
}
void content_identity_accept(TSUCHI_Mod* mod, uint64_t first, uint64_t last) {
    for (uint32_t i = 0; i < identities.count(); ++i) {
        const Hold& held = identities.hold_at(i);
        if (held.mod == mod && ownership_in_range(mod, "content.class_identity", held.id, first, last)) identities.kept_at(i).retained = true;
    }
}
TSUCHI_Result content_identity_hold(TSUCHI_Mod* mod, uint64_t pattern, const char* identity) {
    if (!mod_usable(mod) || !identity) return TSUCHI_INVALID;
    const uint64_t handle = g::to_handle(pattern);
    if (!handle) return TSUCHI_NOT_FOUND;
    return identities.take(mod, handle, 0, Wanted{identity});
}
}
