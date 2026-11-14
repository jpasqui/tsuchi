// Typed text through the system's software keyboard (DESIGN.md, "Menu entries and typed text").
//
// The keyboard is the game's own nn::swkbd import, the one its name entry uses. The call to show it blocks
// until the keyboard closes, and it only returns while the game's own loop keeps running, so it is made on a
// thread of the framework's, as the game makes it on one of its own. A request is handed to that thread at
// the start of a frame; the text comes back to the mod at the start of a later one, on the game thread.
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
Request request{};   // the next one, on the game thread

// What the keyboard thread is doing: nothing, a request handed to it, or a request done.
enum : uint32_t { kIdle, kAsked, kDone };
uint32_t state = kIdle;
Request current{};   // the one the keyboard thread has, while the state is kAsked
bool accepted = false;
uint32_t shown_result = 0;   // the system's result, for the log
// The keyboard's work memory goes to the system, in whole pages; its text comes back in page-aligned
// memory, as the game gives it. Both are passed at exactly the sizes the keyboard asks for: given a larger
// text buffer, the call to show it never returns once the keyboard closes.
alignas(4096) uint8_t work[0x20000];
alignas(8) uint8_t arguments[0x800];
alignas(4096) char result[0x2000];

uint32_t load(const uint32_t& value) { return __atomic_load_n(&value, __ATOMIC_ACQUIRE); }
void store(uint32_t& value, uint32_t to) { __atomic_store_n(&value, to, __ATOMIC_RELEASE); }

bool valid(const char* text, size_t room) {
    char16_t units[kMost * 4 + 1]; size_t count = 0;
    return strlen(text) < room && game::utf8_to_units(text, units, kMost * 4, count);
}

// On the keyboard thread: shows the keyboard for `current` and waits for it to close.
void show_now() {
    memset(work, 0, sizeof work); memset(arguments, 0, sizeof arguments); memset(result, 0, sizeof result);
    accepted = false;
    const uint64_t needed_work = t::fn::ui_keyboard_work_size(false), needed_text = t::fn::ui_keyboard_text_size();
    const bool fits = t::ui_keyboard::size <= sizeof arguments && needed_work && needed_work <= sizeof work && needed_text && needed_text <= sizeof result;
    if (!fits) { shown_result = 0xFFFFFFFF; return; }
    t::fn::ui_keyboard_preset(arguments, 0);
    if (current.number) memcpy(arguments, &kNumberPad, 4);   // the keyboard's mode: the number pad
    const uint32_t most = current.most, least = 0;
    memcpy(arguments + t::ui_keyboard::maximum, &most, 4);
    memcpy(arguments + t::ui_keyboard::minimum, &least, 4);
    // One line, where Enter is OK, as the game's name entry asks for it.
    const uint32_t one_line = 0;
    memcpy(arguments + t::ui_keyboard::input_form, &one_line, 4);
    arguments[t::ui_keyboard::new_lines] = 0;
    arguments[t::ui_keyboard::utf8] = 1;
    const uint64_t at = reinterpret_cast<uint64_t>(work), bytes = needed_work;
    memcpy(arguments + t::ui_keyboard::work, &at, 8);
    memcpy(arguments + t::ui_keyboard::work_bytes, &bytes, 8);
    t::fn::ui_keyboard_guide(arguments, current.prompt);
    t::fn::ui_keyboard_initial(arguments, current.initial);
    struct Output { char* data; uint64_t bytes; } output{result, needed_text};
    shown_result = t::fn::ui_keyboard_show(&output, arguments);
    accepted = shown_result == 0;
    result[sizeof result - 1] = 0;
    if (!accepted) result[0] = 0;
}

void keyboard_thread(void*) {
    for (;;) {
        if (load(state) == kAsked) { show_now(); store(state, kDone); }
        else t::fn::os_sleep_thread(int64_t(20) * 1000000);   // 20 ms
    }
}

// The keyboard thread, made once from memory of its own, at the game thread's priority and on its cores.
bool thread_started() {
    static bool started = false, refused = false;
    if (started || refused) return started;
    refused = true;
    if (!t::bound(t::fn::os_create_thread) || !t::bound(t::fn::os_start_thread) || !t::bound(t::fn::os_sleep_thread) ||
        !t::bound(t::fn::os_current_thread) || !t::bound(t::fn::os_thread_priority)) return false;
    alignas(16) static uint8_t thread[0x200];
    alignas(4096) static uint8_t stack[0x10000];
    void* now = t::fn::os_current_thread();
    if (t::fn::os_create_thread(thread, reinterpret_cast<void*>(&keyboard_thread), nullptr, stack, sizeof stack, t::fn::os_thread_priority(now))) return false;
    if (t::bound(t::fn::os_set_thread_name)) t::fn::os_set_thread_name(thread, "Tsuchi keyboard");
    t::fn::os_start_thread(thread);
    started = true; refused = false;
    return true;
}

// Each frame, on the game thread: a finished keyboard's text to its mod, then the next request to the thread.
void step() {
    if (load(state) == kDone) {
        const Request done = current;
        if (shown_result) {   // cancelled, or refused: the system's result
            char line[64] = "keyboard closed without text: result 0x"; size_t used = strlen(line);
            for (int shift = 28; shift >= 0; shift -= 4) line[used++] = "0123456789abcdef"[(shown_result >> shift) & 15];
            line[used] = 0; mod_note(kFrameworkLogId, line);
        }
        if (mod_usable(done.mod)) ui::emit_text_entered_to(done.mod, {accepted, accepted ? result : ""});
        store(state, kIdle);
    }
    if (!request.waiting || load(state) != kIdle) return;
    current = request;
    request.waiting = false;
    if (!mod_usable(current.mod)) return;
    if (!thread_started()) {
        mod_note(kFrameworkLogId, "keyboard not shown: its thread could not be made");
        ui::emit_text_entered_to(current.mod, {false, ""});
        return;
    }
    store(state, kAsked);
}

TSUCHI_Result ask_text(TSUCHI_Mod* mod, const char* prompt, const char* initial, uint32_t most, TSUCHI_TextKind kind) {
    if (!mod || !prompt || !initial) return TSUCHI_INVALID;
    if (most < 1 || most > kMost || (kind != TSUCHI_TEXT_KIND_TEXT && kind != TSUCHI_TEXT_KIND_NUMBER)) return TSUCHI_INVALID;
    if (!valid(prompt, kPromptBytes) || !valid(initial, sizeof request.initial)) return TSUCHI_INVALID;
    if (request.waiting || load(state) != kIdle) return TSUCHI_BUSY;
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
    if (!listening) listening = each_frame(step);
    if (!listening) return;
    auto& backend = ui_screen_backend();
    backend.ask_text = ask_text;
    ui::set_backend(&backend);
}
} // namespace tsuchi
