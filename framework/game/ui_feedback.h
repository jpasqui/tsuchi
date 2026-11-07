// Scoped guides on the Settings owner's original localized footer.
#pragma once
#include "shared.h"
namespace tsuchi {
struct UiFooterGuide {
    bool tabs = false, edit = false, confirm = false, back = false, reset = false;
    bool details = false;
    bool settings = false;
    // Keep the game's context-specific guide and append only the tab command.
    bool preserve_stock = false;
    // A copied, validated glyph guide replaces the automatic commands when set.
    char custom[641]{};
};
struct UiFooterLease {
    uint64_t host = 0, label = 0, revision = 0;
    game::EngineText saved{}, applied{};
    UiFooterGuide guide{};
    bool installed = false;
};
// host is the generation-checked Settings handle, not a raw object pointer.
// A missing translation or unfamiliar owner leaves the game's guide untouched.
bool ui_footer_update(UiFooterLease& lease, uint64_t host, const UiFooterGuide& guide);
void ui_footer_release(UiFooterLease& lease);
// Takes one more share of a text the caller read from a widget; false when it cannot be kept.
bool ui_retain_text(const game::EngineText& text);
// For an accepted custom-form value change only. Stock selector adjustment
// already plays these animations and must not call this again.
bool ui_feedback_arrow(uint64_t selector, bool increment);
// The game's common Select cue, including passive reading movement. Native
// selector focus already plays it and must not call this again.
bool ui_feedback_focus(uint64_t widget);
}
