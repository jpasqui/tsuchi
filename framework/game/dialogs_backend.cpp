// The game side of the dialogs service.
//
// The game shows a dialog from a request, which carries a function the game
// calls with the player's answer. The game copies that function about as it
// pleases, by the function's own table of operations, so the framework's is a
// small record with such a table: each copy counts as one hold on the dialog.
//
// The text is not in the request. The dialog's widget takes it from the
// game's own table for the kind of dialog while it is set up, in two calls
// this wraps: the set-up itself, after which a message's text is replaced,
// and the call inside it that puts the text and two buttons on a question.
//
// The game is handed one of these at a time. The rest wait here, in the
// order they were asked for.

#include <stddef.h>
#include <string.h>

#include "actors.h"
#include "dialog_service.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "list.h"
#include "objects.h"
#include "reflect.h"
#include "registry.h"
#include "services.h"
#include "shared.h"

namespace tsuchi {
namespace {

namespace t = game::table;

const Kind kDialog{"dialog.dialog"};

struct Dialog {
    uint64_t id;
    TSUCHI_Mod* mod;    // null once its mod has stopped
    char16_t* text;  // with its terminator
    int32_t length;  // the terminator counted
    uint32_t holds;  // copies of its function the game keeps
    bool question;   // two buttons; else a message with one
    bool handed;     // the game has its request
    bool answered, accepted;
    char16_t* labels[2];      // a question's own button labels, with their terminators; none for the game's
    int32_t label_lengths[2];
    uint64_t widget;          // the dialog's widget once the game has set it up, as a handle
    uint32_t choice;          // 0 the first button, 1 the second, 2 Back or closed from code
    bool closing;             // its ending message is with the game: a second close sends none
};
List<Dialog> asked; // oldest first
uint64_t next_dialog = 1;
bool wrapped = false;
bool buttons_wrapped = false; // false on a build that inlines the call that puts a question's text and buttons on

Dialog* find(uint64_t id) {
    for (uint32_t i = 0; i < asked.count; ++i)
        if (asked[i].id == id) return &asked[i];
    return nullptr;
}

// ---- The function the game calls with the answer -------------------------------------------

struct Answerer {
    const void* operations;
    uint64_t dialog;
    uint64_t spare[2];
};
// As the game keeps such a function: room for a small one inside, and where the function is.
struct alignas(16) Carrier {
    Answerer inside;
    Answerer* function;
    uint64_t spare;
};
static_assert(sizeof(Answerer) == 32 && sizeof(Carrier) == 48, "the game's own layout");
// Where a request keeps where its function is.
uint64_t function_place() { return t::dialog_request::answerer + offsetof(Carrier, function); }

extern const void* const kOperations[7];

// Whether what a request holds as its function is one of these. The game's own requests hold the game's.
bool ours(const Answerer* answerer) {
    return game::readable(reinterpret_cast<uint64_t>(answerer), sizeof(Answerer)) && answerer->operations == kOperations;
}

// The game keeps copies after the answer too, and lets go of them when it pleases. By then the
// dialog is forgotten, and a copy of a dialog that is not there does nothing.
void make(Answerer* out, uint64_t id) {
    *out = Answerer{kOperations, id, {}};
    if (Dialog* dialog = find(id)) ++dialog->holds;
}
// One copy fewer. When the game lets go of the last without an answer, the dialog is over all the same.
void destroy(Answerer* answerer) {
    Dialog* dialog = find(answerer->dialog);
    if (!dialog || --dialog->holds || dialog->answered) return;
    dialog->answered = true, dialog->accepted = false;
}
void destroy_and_free(Answerer* answerer) {
    destroy(answerer);
    game::release_engine_memory(answerer);
}
Answerer* clone(const Answerer* from) {
    auto* copy = static_cast<Answerer*>(game::engine_memory(sizeof(Answerer)));
    if (copy) make(copy, from->dialog);
    return copy;
}
void copy_into(const Answerer* from, Answerer* out) { make(out, from->dialog); }
// The game's answer: a name, none when the dialog was closed from code, and the number of the button.
void answer(const Answerer* answerer, const uint64_t* name, const uint32_t* button) {
    Dialog* dialog = find(answerer->dialog);
    if (!dialog || dialog->answered) return;
    dialog->answered = true;
    dialog->accepted = *name != 0 && (dialog->question ? *button == t::constant::dialog_accept : true);
    const uint32_t first = uint32_t(t::constant::dialog_accept);
    dialog->choice = !*name ? 2u : !dialog->question ? 0u : *button == first ? 0u : *button == first + 1 ? 1u : 2u;
}

// The game's order: two ways to end one in place, two to end one in memory of its own, a copy
// in new memory, a copy in place, and the call.
const void* const kOperations[7] = {
    reinterpret_cast<const void*>(&destroy), reinterpret_cast<const void*>(&destroy_and_free), reinterpret_cast<const void*>(&clone),
    reinterpret_cast<const void*>(&copy_into), reinterpret_cast<const void*>(&destroy), reinterpret_cast<const void*>(&destroy_and_free),
    reinterpret_cast<const void*>(&answer),
};

// ---- The text ------------------------------------------------------------------------------

// Calls `use` with text as the engine's own kind of text, and lets go of it afterward.
template <typename Use>
void with_units(const char16_t* units, int32_t length, Use&& use) {
    alignas(8) uint8_t string[t::array::size] = {};
    const uint64_t data = reinterpret_cast<uint64_t>(units);
    memcpy(string + t::array::data, &data, 8), memcpy(string + t::array::count, &length, 4), memcpy(string + t::array::max, &length, 4);
    game::EngineText made = t::fn::text_from_string(string);
    use(made);
    game::drop_shared(made.counter());
}
template <typename Use>
void with_text(const Dialog& dialog, Use&& use) { with_units(dialog.text, dialog.length, use); }

// The set-up of a dialog's widget that is running now, and which of ours it is for, if any.
struct Setup {
    void* widget;
    uint64_t dialog;
    Setup* outer;
};
Setup* running = nullptr;
void (*game_setup)(void*, uint32_t, int32_t) = nullptr;
decltype(t::code_site::dialog_buttons_call)::Function game_buttons = nullptr;

// The game puts the text and two buttons on a dialog. Ours gets its own text.
void on_buttons(void* widget, uint32_t kind, const void* text, const void* first, const void* second) {
    Dialog* dialog = running && running->widget == widget ? find(running->dialog) : nullptr;
    if (!dialog) return game_buttons(widget, kind, text, first, second);
    with_text(*dialog, [&](const game::EngineText& made) {
        if (!dialog->labels[0]) return game_buttons(widget, kind, &made, first, second);
        // A question with its own buttons: their labels as the game's own kind of text too.
        with_units(dialog->labels[0], dialog->label_lengths[0], [&](const game::EngineText& one) {
            with_units(dialog->labels[1], dialog->label_lengths[1], [&](const game::EngineText& two) {
                game_buttons(widget, kind, &made, &one, &two);
            });
        });
    });
}

// ---- Asking ----------------------------------------------------------------------------------

void forget(Dialog* dialog) {
    release(dialog->text);
    release(dialog->labels[0]); release(dialog->labels[1]);
    for (Dialog* last = &asked[asked.count - 1]; dialog < last; ++dialog) dialog[0] = dialog[1]; // the rest keep their order
    --asked.count;
}

// Its mod stopped. One still waiting here is never shown. One the game has, the game may still
// show; nobody is told the answer.
void orphan(void*, uint64_t id) {
    Dialog* dialog = find(id);
    if (dialog->handed) dialog->mod = nullptr;
    else forget(dialog);
}

uint64_t line() {
    const uint64_t queue = game::read_or<uint64_t>(game::game_context() + t::game_context::dialogs);
    return queue && game::table_entry(queue, t::request_queue_table::push) ? queue : 0;
}

// Hands the game its own request for a dialog of the kind that has the buttons wanted, to be answered to us.
TSUCHI_Result hand(Dialog& dialog) {
    namespace r = t::dialog_request;
    const uint64_t queue = line();
    if (!queue) return TSUCHI_NOT_READY;
    auto* request = static_cast<uint8_t*>(game::engine_memory(r::size));
    if (!request) return TSUCHI_NO_MEMORY;
    t::fn::dialog_request(request);
    const int32_t anyone = -1;
    request[r::kind] = uint8_t(dialog.question ? t::constant::dialog_question : t::constant::dialog_message);
    request[r::shape] = uint8_t(t::constant::dialog_shape), request[r::mode] = uint8_t(t::constant::dialog_mode);
    memcpy(request + r::player, &anyone, 4);
    // The function goes into the request the game's own way, which copies ours and ends the one handed over.
    Carrier carrier{};
    make(&carrier.inside, dialog.id);
    carrier.function = &carrier.inside;
    t::fn::dialog_function_transfer(&carrier, request + r::answerer);
    if (carrier.function) destroy(&carrier.inside); // the game left it its own
    if (!ours(game::read_or<const Answerer*>(reinterpret_cast<uint64_t>(request) + function_place()))) { // it did not take
        t::fn::dialog_request_dispose(request);
        game::release_engine_memory(request);
        return TSUCHI_NOT_READY;
    }
    dialog.handed = true;
    game::method(queue, t::request_queue_table::push)(reinterpret_cast<void*>(queue), request);
    return TSUCHI_OK;
}

// UTF-8 as engine text units with a terminator, in memory of the framework's own; null when it is not UTF-8.
char16_t* units_of(const char* text, int32_t& length) {
    size_t count = 0;
    if (!text || !game::utf8_to_units(text, nullptr, 0, count)) return nullptr;
    auto* units = static_cast<char16_t*>(allocate((count + 1) * 2));
    if (!units) return nullptr;
    game::utf8_to_units(text, units, count, count);
    units[count] = 0; length = int32_t(count + 1);
    return units;
}

TSUCHI_Result ask(TSUCHI_Mod* mod, const char* text, bool question, TSUCHI_Dialog* out, const char* first_label = nullptr, const char* second_label = nullptr) {
    size_t count = 0;
    if (!game::utf8_to_units(text, nullptr, 0, count)) return TSUCHI_INVALID;
    char16_t* labels[2] = {nullptr, nullptr}; int32_t lengths[2] = {0, 0};
    if (first_label) {
        if (!buttons_wrapped) return TSUCHI_UNSUPPORTED;   // a build that inlines the buttons' call keeps the game's labels
        labels[0] = units_of(first_label, lengths[0]); labels[1] = units_of(second_label, lengths[1]);
        if (!labels[0] || !labels[1] || lengths[0] < 2 || lengths[1] < 2) { release(labels[0]); release(labels[1]); return TSUCHI_INVALID; }
    }
    if (!wrapped || !line() || !t::bound(t::fn::dialog_request) || !t::bound(t::fn::dialog_request_dispose) || !t::bound(t::fn::dialog_function_transfer) || !t::bound(t::fn::text_from_string) ||
        (!question && !t::bound(t::fn::rich_text_set))) {
        release(labels[0]); release(labels[1]);
        return TSUCHI_NOT_READY;
    }
    auto* units = static_cast<char16_t*>(allocate((count + 1) * 2));
    if (!units) { release(labels[0]); release(labels[1]); return TSUCHI_NO_MEMORY; }
    game::utf8_to_units(text, units, count, count);
    units[count] = 0;
    const uint64_t id = next_dialog;
    const bool first = asked.count == 0; // every one here is waiting for its turn or for its answer
    Dialog* dialog = asked.add(Dialog{id, mod, units, int32_t(count + 1), 0, question, false, false, false,
                                      {labels[0], labels[1]}, {lengths[0], lengths[1]}, 0, 0, false});
    if (!dialog) {
        release(units); release(labels[0]); release(labels[1]);
        return TSUCHI_NO_MEMORY;
    }
    TSUCHI_Result result = own(mod, kDialog, id, orphan, nullptr);
    if (result != TSUCHI_OK) {
        forget(dialog);
        return result;
    }
    if (first && (result = hand(*dialog)) != TSUCHI_OK) { // nothing is ahead of it, and the game does not take it
        disown(mod, kDialog, id);
        forget(dialog);
        return result;
    }
    ++next_dialog;
    out->id = id;
    return TSUCHI_OK;
}

TSUCHI_Result confirm(TSUCHI_Mod* mod, const char* text, TSUCHI_Dialog* out) { return ask(mod, text, true, out); }
TSUCHI_Result message(TSUCHI_Mod* mod, const char* text, TSUCHI_Dialog* out) { return ask(mod, text, false, out); }
TSUCHI_Result ask_with(TSUCHI_Mod* mod, const char* text, const char* first, const char* second, TSUCHI_Dialog* out) {
    if (!first || !second) return TSUCHI_INVALID;
    return ask(mod, text, true, out, first, second);
}

Dialog* mine(TSUCHI_Mod* mod, TSUCHI_Dialog handle) {
    Dialog* dialog = find(handle.id);
    return dialog && dialog->mod == mod && !dialog->answered ? dialog : nullptr;
}

// The block a set-up widget shows its text in.
uint64_t text_block(const Dialog& dialog) {
    const uint64_t widget = game::from_handle(dialog.widget);
    return widget ? game::from_weak_reference(widget + t::dialog_widget::text) : 0;
}

TSUCHI_Result set_text(TSUCHI_Mod* mod, TSUCHI_Dialog handle, const char* text) {
    Dialog* dialog = mine(mod, handle);
    if (!dialog) return TSUCHI_NOT_FOUND;
    int32_t length = 0;
    char16_t* units = units_of(text, length);
    if (!units) return TSUCHI_INVALID;
    release(dialog->text);
    dialog->text = units; dialog->length = length;
    if (const uint64_t block = text_block(*dialog))
        with_text(*dialog, [&](const game::EngineText& made) { t::fn::rich_text_set(reinterpret_cast<void*>(block), const_cast<game::EngineText*>(&made)); });
    return TSUCHI_OK;
}

// One still waiting here is answered as closed. The one on screen is closed as the title flow closes its
// waiting dialog: the game's own result message, with the open dialog's kind, which answers it with no name.
TSUCHI_Result close(TSUCHI_Mod* mod, TSUCHI_Dialog handle) {
    Dialog* dialog = mine(mod, handle);
    if (!dialog) return TSUCHI_NOT_FOUND;
    if (!dialog->handed) { dialog->answered = true; dialog->accepted = false; dialog->choice = 2; return TSUCHI_OK; }
    if (dialog->closing) return TSUCHI_OK; // the game has the message that ends it, and answers it once
    if (!t::present(t::data::dialog_result_table) || !t::present(t::data::dialog_result_header) || !t::present(t::ui_manager::open_dialog) ||
        !t::present(t::dialog_widget::kind) || !t::present(t::dialog_result::size)) return TSUCHI_UNSUPPORTED;
    const uint64_t widget = game::from_handle(dialog->widget);
    const uint64_t framework = game::game_framework();
    const uint64_t ui = framework ? game::read_or<uint64_t>(framework + t::game_framework::ui) : 0;
    if (!widget || !ui || game::read_or<uint64_t>(ui + t::ui_manager::open_dialog) != widget) return TSUCHI_NOT_READY;   // not on screen yet
    const uint64_t queue = line();
    auto* note = static_cast<uint8_t*>(game::engine_memory(t::dialog_result::size));
    if (!queue || !note) { if (note) game::release_engine_memory(note); return TSUCHI_NOT_READY; }
    memset(note, 0, t::dialog_result::size);
    const uint64_t table = game::main_base() + t::data::dialog_result_table, self = reinterpret_cast<uint64_t>(note);
    const uint64_t header = game::read_or<uint64_t>(game::main_base() + t::data::dialog_result_header);
    const uint32_t kind = uint32_t(t::constant::dialog_result_message);
    memcpy(note, &table, 8); memcpy(note + t::dialog_result::self, &self, 8); memcpy(note + t::dialog_result::header, &header, 8);
    memcpy(note + t::dialog_result::message, &kind, 4);
    note[t::dialog_result::dialog_kind] = game::read_or<uint8_t>(widget + t::dialog_widget::kind);
    note[t::dialog_result::as_flow] = 1;
    game::method(queue, t::request_queue_table::push)(reinterpret_cast<void*>(queue), note);
    dialog->closing = true;
    return TSUCHI_OK;
}

// Once a frame: answers are told and their dialogs forgotten, oldest first, and when the game has none of
// ours the next in line is handed to it.
void step() {
    for (uint32_t i = 0; i < asked.count;) {
        if (!asked[i].answered) {
            ++i;
            continue;
        }
        const Dialog done = asked[i];
        forget(&asked[i]); // first: whoever hears of it may ask for another
        if (!done.mod) continue;
        disown(done.mod, kDialog, done.id);
        dialog::emit_answered({{done.id}, done.accepted, done.choice});
    }
    if (!asked.count || asked[0].handed) return;
    if (hand(asked[0]) != TSUCHI_OK) asked[0].answered = true; // the game does not take it: that is told as an answer of no
}

const dialog::Backend service{confirm, message, ask_with, set_text, close};

} // namespace

// The game sets up a dialog's widget from a request. When the request is one of ours, the
// text is ours too: a question's goes in through the call inside, a message's is put on afterward (and a question's
// too, on a build where that call is inlined).
extern "C" void tsuchi_dialog_on_setup(void* widget, uint32_t kind, int32_t layout, void* request) {
    const auto* held = game::read_or<const Answerer*>(reinterpret_cast<uint64_t>(request) + function_place());
    Setup setup{widget, ours(held) ? held->dialog : 0, running};
    running = &setup;
    game_setup(widget, kind, layout);
    running = setup.outer;
    Dialog* dialog = find(setup.dialog);
    if (dialog) dialog->widget = game::to_handle(reinterpret_cast<uint64_t>(widget));
    if (dialog && (!dialog->question || !buttons_wrapped)) {
        const uint64_t block = game::from_weak_reference(reinterpret_cast<uint64_t>(widget) + t::dialog_widget::text);
        if (block) with_text(*dialog, [&](const game::EngineText& made) { t::fn::rich_text_set(reinterpret_cast<void*>(block), const_cast<game::EngineText*>(&made)); });
    }
}

void dialogs_start(TSUCHI_Mod* self, void* setup_entry) {
    dialog::set_backend(&service);
    if (wrapped) return;
    uint64_t id = 0, second = 0;
    void* next = nullptr;
    if (hook_code_site(self, t::code_site::dialog_setup_call, setup_entry, &id, &next) != TSUCHI_OK) return;
    game_setup = reinterpret_cast<decltype(game_setup)>(next);
    const TSUCHI_Result buttons = hook_site(self, t::code_site::dialog_buttons_call, &on_buttons, &game_buttons, &second);
    // A build without the call (its set-up inlines it) gets a question's text after set-up instead.
    if (buttons != TSUCHI_OK && t::present(t::code_sites[t::code_site::dialog_buttons_call.number].address)) {
        hook_remove(self, id);
        return;
    }
    buttons_wrapped = buttons == TSUCHI_OK;
    wrapped = each_frame(step);
}

unsigned dialogs_waiting() { return asked.count; }

} // namespace tsuchi
