// The native schedule update keeps delivery inline behind its online-session
// gate. Enter its event-consumer block with one owned download-complete event.
// The original code owns the event allocation, delivery and native epilogue.
#include "academy_internal.h"
#include "game.h"
#include "objects.h"
#include "registry.h"
#include "reflect.h"

namespace tsuchi {
namespace {
namespace t = game::table;

#if defined(__aarch64__)
// Sw2's original 0x180-byte frame, checked below before this adapter is used.
// Branching (rather than calling) lets the original epilogue restore the
// caller's callee-saved registers and authenticate its original return address.
__attribute__((naked)) void enter_delivery(void*, void*, void*) {
    __asm__ volatile(
        ".inst 0xd503237f\n"
        "sub sp, sp, #0x180\n"
        "stp d9, d8, [sp, #0x110]\n"
        "stp x29, x30, [sp, #0x120]\n"
        "stp x28, x27, [sp, #0x130]\n"
        "stp x26, x25, [sp, #0x140]\n"
        "stp x24, x23, [sp, #0x150]\n"
        "stp x22, x21, [sp, #0x160]\n"
        "stp x20, x19, [sp, #0x170]\n"
        "add x29, sp, #0x120\n"
        "str x0, [sp, #0x28]\n"
        "str x2, [sp, #0x30]\n"
        "mov x8, #1\n"
        "orr x8, x8, x8, lsl #32\n"
        "str x8, [sp, #0x38]\n"
        "br x1\n");
}
#else
void enter_delivery(void* interface, void* entry, void* event) {
    reinterpret_cast<void (*)(void*, void*)>(entry)(interface, event);
}
#endif

bool frame_matches() {
    if (uint64_t(t::data::academy_delivery_update) == t::absent_value ||
        uint64_t(t::data::academy_delivery_entry) == t::absent_value) return false;
    // PAC, stack reservation, all preserved register pairs and frame pointer.
    constexpr uint32_t prologue[] = {0xd503237f, 0xd10603ff, 0x6d1123e9, 0xa9127bfd,
        0xa9136ffc, 0xa91467fa, 0xa9155ff8, 0xa91657f6, 0xa9174ff4, 0x910483fd};
    const uint64_t at = game::main_base() + t::data::academy_delivery_update;
    for (unsigned i = 0; i < sizeof prologue / sizeof *prologue; ++i)
        if (game::read_or<uint32_t>(at + i * 4) != prologue[i]) return false;
    const uint64_t entry = game::main_base() + t::data::academy_delivery_entry;
    return game::read_or<uint32_t>(entry) == 0xa942d3eb; // ldp x11,x20,[sp,#0x28]
}
}

bool academy_delivery_ready() { return frame_matches(); }

void academy_deliver(uint64_t manager) {
    if (!on_game_thread() || !manager || !frame_matches() || !game::engine_allocator_ready()) return;
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    const uint64_t save = instance ? game::read_or<uint64_t>(instance + t::game_instance::academy_save) : 0;
    if (!save || !game::readable(save + t::academy_save::service_end, 1) ||
        game::read_or<uint8_t>(save + t::academy_save::service_end) == 1 ||
        game::read_or<uint8_t>(manager + t::academy_schedule::valid) != 1 ||
        game::read_or<int32_t>(manager + t::academy_schedule::status) != 1) return;
    const uint64_t allocator = game::global<uint64_t>(t::data::allocator);
    const auto alloc = allocator ? game::method(allocator, t::allocator_vtable::malloc) : nullptr;
    if (!alloc) return;
    void* event = alloc(reinterpret_cast<void*>(allocator), uint64_t(4), uint32_t(0));
    if (!event) return;
    *static_cast<uint32_t*>(event) = 1;
    enter_delivery(reinterpret_cast<void*>(manager + t::academy_schedule::interface),
                   reinterpret_cast<void*>(game::main_base() + t::data::academy_delivery_entry), event);
}
}
