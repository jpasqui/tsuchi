// Tells the player when the game is a build the table does not describe. The framework then
// knows no address in it, so this uses the loaded programs' own lists of symbols: the SDK's
// error dialog by name, and an import of the game's that it calls while it reads controllers.
// The game calling that import is what runs the dialog, on a thread of the game's and once the
// game has been up a while. The game goes on without mods after the player closes it.
#include <string.h>

#include "module_symbols.h"
#include "platform.h"
#include "status.h"

namespace tsuchi::target {
namespace {

constexpr char kShort[] = "Mods are off.";
constexpr char kText[] = "Tsuchi does not support this version of the game, so mods are off. The game runs as usual. "
                         "A newer Tsuchi may support it.";
constexpr char kDebugText[] = "[tsuchi] this build of the game is not one the table describes: no mods run\n";
constexpr uint32_t kErrorNumber = 1;
constexpr uint64_t kSecondsFirst = 20; // the game's own start-up and first screens are over by then

// In the order they are tried. Each is one the game's reading of controllers calls.
constexpr const char* kImports[] = {
    "_ZN2nn3hid15GetNpadStyleSetERKj",
    "_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",
    "_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj",
};
constexpr char kDialogMake[] = "_ZN2nn3err19ApplicationErrorArgC1EjPKcS3_RKNS_8settings12LanguageCodeE";
constexpr char kDialogShow[] = "_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE";

using DialogMake = void (*)(void* dialog, uint32_t number, const char* short_text, const char* long_text, const void* language);
using DialogShow = void (*)(const void* dialog);

uint64_t slot = 0, next = 0, shown_after = 0;
DialogMake dialog_make = nullptr;
DialogShow dialog_show = nullptr;
bool taken = false;
alignas(16) uint8_t dialog[0x2000]; // the SDK's size for it is smaller and can grow; never on the game's stack

uint64_t ticks() {
    uint64_t now;
    asm volatile("mrs %0, cntpct_el0" : "=r"(now));
    return now;
}
uint64_t ticks_per_second() {
    uint64_t rate;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(rate));
    return rate ? rate : 19200000;
}

// Regions of code the system reports, in the order they are mapped. `visit` stops the walk by returning true.
template <class Visit>
void each_module(uint64_t own_base, Visit visit) {
    uint64_t address = 0;
    for (unsigned region = 0; region < 4096; ++region) {
        uint64_t base = 0, size = 0;
        uint32_t permission = 0;
        if (!query_region(address, base, size, permission) || !size) return;
        game::LoadedModule module;
        if (permission == 5 && base != own_base && game::read_module(base, module) && visit(module)) return;
        if (base + size <= address) return;
        address = base + size;
    }
}

} // namespace

// Where the game's call goes first: it hands back where the call was going.
extern "C" uint64_t tsuchi_notice_step() {
    if (ticks() < shown_after || __atomic_exchange_n(&taken, true, __ATOMIC_ACQ_REL)) return next;
    __atomic_store_n(reinterpret_cast<uint64_t*>(slot), next, __ATOMIC_RELEASE);
    const char language[8] = "en-US";
    dialog_make(dialog, kErrorNumber, kShort, kText, language);
    dialog_show(dialog);
    status.notice = kNoticeShown;
    return next;
}

// Keeps every register a call's arguments or the import's own lazy binding can use.
extern "C" __attribute__((naked)) void tsuchi_notice_entry() {
    __asm__("stp x29, x30, [sp, #-112]!\n"
            "stp x0, x1, [sp, #16]\n"
            "stp x2, x3, [sp, #32]\n"
            "stp x4, x5, [sp, #48]\n"
            "stp x6, x7, [sp, #64]\n"
            "stp x8, x16, [sp, #80]\n"
            "mov x29, sp\n"
            "bl tsuchi_notice_step\n"
            "mov x17, x0\n"
            "ldp x0, x1, [sp, #16]\n"
            "ldp x2, x3, [sp, #32]\n"
            "ldp x4, x5, [sp, #48]\n"
            "ldp x6, x7, [sp, #64]\n"
            "ldp x8, x16, [sp, #80]\n"
            "ldp x29, x30, [sp], #112\n"
            "br x17\n");
}

void unknown_build_notice(uint64_t own_base) {
    debug_output(kDebugText, sizeof kDebugText - 1);
    new_frame(); // the reads below number in the thousands, and the system is asked once per region
    each_module(own_base, [](const game::LoadedModule& module) {
        if (!dialog_make) dialog_make = reinterpret_cast<DialogMake>(game::module_export(module, kDialogMake));
        if (!dialog_show) dialog_show = reinterpret_cast<DialogShow>(game::module_export(module, kDialogShow));
        return false;
    });
    if (!dialog_make || !dialog_show) {
        status.notice = kNoticeNoDialog;
        return;
    }
    const char* import_name = nullptr;
    each_module(own_base, [&](const game::LoadedModule& module) {
        for (const char* name : kImports)
            if (const uint64_t found = game::module_import_slot(module, name)) {
                slot = found, import_name = name;
                return true;
            }
        return false;
    });
    if (!slot) {
        status.notice = kNoticeNoCall;
        return;
    }
    // The module that defines the import, so that the call does not bind itself over this entry later.
    each_module(own_base, [&](const game::LoadedModule& module) { return (next = game::module_export(module, import_name)) != 0; });
    if (!next || !game::writable(slot, sizeof slot)) {
        status.notice = kNoticeNoCall;
        return;
    }
    shown_after = ticks() + kSecondsFirst * ticks_per_second();
    __atomic_store_n(reinterpret_cast<uint64_t*>(slot), reinterpret_cast<uint64_t>(&tsuchi_notice_entry), __ATOMIC_RELEASE);
    status.notice = kNoticeArmed;
}

} // namespace tsuchi::target
