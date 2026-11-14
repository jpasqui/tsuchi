// The Mods tab uses the Settings menu's categories, rows, focus and scrolling.
#pragma once
#include "registry.h"
namespace tsuchi {
struct UiModListRow {
    const char* id;
    const char* name;
    bool requested;
    const char* status;
    bool has_settings = false;
    bool button = false; // a mod's entry on the page: its name shows in the box, and Confirm chooses it
};
struct UiModListNativeCallbacks {
    uint32_t (*count)();
    bool (*row)(uint32_t index, UiModListRow& row);
    // The parent owns proposals, review dialogs, details, order and saving.
    // For arrows, requested is the choice the original selector reached.
    bool (*action)(uint64_t host, uint32_t index, uint32_t action, bool requested);
    void (*host_ready)(uint64_t host);
    void (*host_lost)(uint64_t host);
};
void ui_mod_list_native_callbacks(const UiModListNativeCallbacks* callbacks);
void ui_mod_list_native_start(TSUCHI_Mod* framework);
// Prepare the stock tab template after containers mount and native startup is
// ready, before mod scripts run. Does not construct a menu or start a mod.
TSUCHI_Result ui_mod_list_native_prepare();
// Resolved from NativeConstruct/discovery and generation checked on each use.
// Neither query inventories engine objects.
uint64_t ui_mod_list_native_host();
struct UiContentRect { float left, top, width, height; };
bool ui_mod_list_native_content(uint64_t host, UiContentRect& out);
// Cached generated categories use the same rooting journal as the Mods list.
// A private group keeps independent views from releasing each other's roots.
uint64_t ui_mod_list_native_group();
bool ui_mod_list_native_pin(uint64_t host, uint64_t group, uint64_t object);
void ui_mod_list_native_unpin(uint64_t host, uint64_t group);
// A generated form temporarily covers the native Mods source. Keep that
// source alive until the form restores it; rooting widgets alone is not enough.
bool ui_mod_list_native_borrow(uint64_t host, uint64_t source, uint64_t replacement);
void ui_mod_list_native_return_source(uint64_t host, uint64_t replacement);
// Release the current tab/list guide before a generated view saves its baseline.
void ui_mod_list_native_footer_release(uint64_t host);
TSUCHI_Result ui_mod_list_native_hold(uint64_t host);
TSUCHI_Result ui_mod_list_native_release(uint64_t host);
TSUCHI_Result ui_mod_list_native_cycle(uint64_t host, bool previous);
// Goes to the Mods tab from whichever tab shows.
TSUCHI_Result ui_mod_list_native_show(uint64_t host);
bool ui_mod_list_native_active(uint64_t host);
TSUCHI_Result ui_mod_list_native_selected(uint64_t host, uint32_t& index);
TSUCHI_Result ui_mod_list_native_sync(uint64_t host);
TSUCHI_Result ui_mod_list_native_focus(uint64_t host, uint32_t index);
// Called by the native route after the owning Settings dispatcher's input gates.
// Up and Down return unhandled and continue through original navigation.
bool ui_mod_list_native_dispatch(uint64_t host, uint32_t action, bool& handled, bool& result);
}
