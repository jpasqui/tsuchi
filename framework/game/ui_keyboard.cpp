// Typed text through the system's software keyboard (DESIGN.md, "Menu entries and typed text").
//
// The keyboard is the game's own nn::swkbd import, the one its name entry uses. A request waits for the
// start of the next frame and is shown there on the game thread; the game waits while it is open.
#include "ui_keyboard.h"
#include "ui_service.h"
#include "ui_screens.h"
#include "frame.h"
#include "game.h"
#include "mods.h"
#include "reflect.h"
#include "registry.h"
#include "shared.h"
#include <string.h>

namespace tsuchi {
namespace {
namespace t = game::table;

constexpr uint32_t kPromptBytes = 257, kMost = 256, kNumberPad = 1;
struct Request {
    TSUCHI_Mod* mod;
    char prompt[kPromptBytes], initial[kMost * 4 + 1];
    uint32_t most;
    bool number, waiting;
};
Request request{};

bool valid(const char* text, size_t room) {
    char16_t units[kMost * 4 + 1]; size_t count = 0;
    return strlen(text) < room && game::utf8_to_units(text, units, kMost * 4, count);
}

void show() {
    if (!request.waiting) return;
    Request asked = request;
    request.waiting = false;
    if (!mod_usable(asked.mod)) return;
    alignas(4096) static uint8_t work[0x1000];
    alignas(8) static uint8_t arguments[0x800];
    // nn::swkbd requires its full string buffer, even when
    // this request's character limit is smaller than the system maximum.
    alignas(4096) static char result[0x1000];
    memset(work, 0, sizeof work); memset(arguments, 0, sizeof arguments); memset(result, 0, sizeof result);
    bool accepted = false;
    const uint64_t needed_work = t::fn::ui_keyboard_work_size(false), needed_text = t::fn::ui_keyboard_text_size();
    if (t::ui_keyboard::size <= sizeof arguments && needed_work && needed_work <= sizeof work && needed_text && needed_text <= sizeof result) {
        t::fn::ui_keyboard_preset(arguments, 0);
        if (asked.number) memcpy(arguments, &kNumberPad, 4);   // the keyboard's mode: the number pad
        const uint32_t most = asked.most, least = 0;
        memcpy(arguments + t::ui_keyboard::maximum, &most, 4);
        memcpy(arguments + t::ui_keyboard::minimum, &least, 4);
        arguments[t::ui_keyboard::utf8] = 1;
        const uint64_t at = reinterpret_cast<uint64_t>(work), bytes = needed_work;
        memcpy(arguments + t::ui_keyboard::work, &at, 8);
        memcpy(arguments + t::ui_keyboard::work_bytes, &bytes, 8);
        t::fn::ui_keyboard_guide(arguments, asked.prompt);
        t::fn::ui_keyboard_initial(arguments, asked.initial);
        struct Output { char* data; uint64_t bytes; } output{result, needed_text};
        accepted = t::fn::ui_keyboard_show(&output, arguments) == 0;
    }
    result[sizeof result - 1] = 0;
    if (!accepted) result[0] = 0;
    if (mod_usable(asked.mod)) ui::emit_text_entered_to(asked.mod, {accepted, result});
}

TSUCHI_Result ask_text(TSUCHI_Mod* mod, const char* prompt, const char* initial, uint32_t most, TSUCHI_TextKind kind) {
    if (!mod || !prompt || !initial) return TSUCHI_INVALID;
    if (most < 1 || most > kMost || (kind != TSUCHI_TEXT_KIND_TEXT && kind != TSUCHI_TEXT_KIND_NUMBER)) return TSUCHI_INVALID;
    if (!valid(prompt, kPromptBytes) || !valid(initial, sizeof request.initial)) return TSUCHI_INVALID;
    if (request.waiting) return TSUCHI_BUSY;
    request = {};
    request.mod = mod; request.most = most; request.number = kind == TSUCHI_TEXT_KIND_NUMBER;
    memcpy(request.prompt, prompt, strlen(prompt) + 1);
    memcpy(request.initial, initial, strlen(initial) + 1);
    request.waiting = true;
    return TSUCHI_OK;
}
} // namespace

void ui_keyboard_start() {
    static bool listening = false;
    if (!listening) listening = each_frame(show);
    if (!listening) return;
    auto& backend = ui_screen_backend();
    backend.ask_text = ask_text;
    ui::set_backend(&backend);
}
} // namespace tsuchi
