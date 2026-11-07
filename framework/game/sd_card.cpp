// The SD card, mounted once for whoever reads or writes it: the log's file and the development folders.
//
// The card is mounted through the system's own function, found by its name in the system's library. Unlike the
// save, it needs no session and no commit. Calls on it are made while the thread's file context is the one that
// makes a missing path or a refused permission an error the caller sees, instead of a stop of the game.

#include "sd_card.h"

#include "game.h"
#include "modules.h"

namespace tsuchi {

namespace {

namespace t = game::table;
using GetContext = const void* (*)();
using SetContext = void (*)(const void*);

struct {
    GetContext get = nullptr;
    SetContext set = nullptr;
    const void* returning = nullptr;
    bool tried = false, mounted = false;
} card;

} // namespace

const void* sd_enter() {
    const void* before = card.get();
    card.set(card.returning);
    return before;
}
void sd_leave(const void* before) { card.set(before); }

bool sd_card_mounted() {
    if (card.tried) return card.mounted;
    card.tried = true;
    const uint64_t library = game::module_containing(game::global<uint64_t>(t::data::system_library_anchor));
    card.get = reinterpret_cast<GetContext>(game::module_symbol(library, "_ZN2nn2fs25GetCurrentThreadFsContextEv"));
    card.set = reinterpret_cast<SetContext>(game::module_symbol(library, "_ZN2nn2fs25SetCurrentThreadFsContextEPKNS0_9FsContextE"));
    card.returning = game::returning_fs_context(library);
    auto mount = reinterpret_cast<uint32_t (*)(const char*)>(game::module_symbol(library, "_ZN2nn2fs11MountSdCardEPKc"));
    if (!card.get || !card.set || !card.returning || !mount) return false;
    SdScope scope;
    card.mounted = mount(kSdMount) == 0;
    return card.mounted;
}

} // namespace tsuchi
