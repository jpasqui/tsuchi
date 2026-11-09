// Copied screen definitions shared by the code and file forms.
#pragma once
#include "list.h"
#include "text.h"
#include "texts.h"
#include "tsuchi/ui.h"
namespace tsuchi {
namespace ui { struct Backend; }
// The runtime widget adapter adds its functions during framework startup.
ui::Backend& ui_screen_backend();
enum class UiKind : uint8_t { button, choice, integer, text };
constexpr uint32_t kUiNoSelection = UINT32_MAX;
struct UiControl {
    char id[TSUCHI_ID_SIZE];
    UiKind kind;
    Text label, text;
    Texts choices, labels;
    uint32_t selected;
    int64_t minimum, maximum, step, integer;
    // Framework-generated decimal settings reuse the stock integer arrows.
    // Their value stays a number; the integer is only a presentation revision.
    struct {
        bool enabled;
        double minimum, maximum, step, value;
        void (*format)(float, char*);
    } number;
};
struct UiScreen {
    uint64_t id;
    TSUCHI_Mod* mod;
    Text title, footer;
    List<UiControl> controls;
    uint32_t selected, revision;
    bool settings_tab, resettable;
};
// The native adapter builds stock widgets. A host loss is reported by live.
struct UiRenderer {
    TSUCHI_Result (*open)(const UiScreen& screen, void** view);
    bool (*update)(void* view, const UiScreen& screen);
    bool (*live)(void* view);
    void (*close)(void* view);
    // Input belongs to this particular view. A renderer may use any view shape;
    // the input bridge must not cast another renderer's state to a stock view.
    bool (*action)(void* view, uint64_t host, uint32_t action) = nullptr;
    void (*buttons)(void* view, uint64_t held) = nullptr;
    bool (*uses_host)(void* view, uint64_t host) = nullptr;
    void (*detach_host)(void* view, uint64_t host) = nullptr;
};
void ui_start();
uint32_t ui_events_dropped();
void ui_set_renderer(const UiRenderer* renderer);
const UiScreen* ui_active_screen();
void* ui_active_view();
// Update this exact captured view through its retained Showing operation.
bool ui_sync_screen(uint64_t id, void* view);
// Routes through the callbacks captured when the active view was opened.
bool ui_route_action(uint64_t host, uint32_t action);
void ui_route_buttons(uint64_t held);
// Unwind a generated view before its native host releases source arrays.
void ui_host_lost(uint64_t host);
// Stock dispatcher actions: 0/1 rows, 2/3 arrows, 4 Confirm, 5 Back, 7 Reset.
bool ui_action(uint32_t action);
uint32_t ui_settings_tab_count();
uint32_t ui_settings_tab_revision();
const UiScreen* ui_settings_tab(uint32_t index);
TSUCHI_Result ui_settings_tab_open(uint32_t index);
void ui_settings_tab_close();
TSUCHI_Result ui_screen_integer_range(TSUCHI_Mod* mod, TSUCHI_Screen screen, const char* id,
    int64_t minimum, int64_t maximum, int64_t value);
TSUCHI_Result ui_screen_add_number(TSUCHI_Mod* mod, TSUCHI_Screen screen,
    const char* id, const char* label, double minimum, double maximum,
    double step, double initial, void (*format)(float, char*));
TSUCHI_Result ui_screen_number(TSUCHI_Mod* mod, TSUCHI_Screen screen, const char* id, double* out);
TSUCHI_Result ui_screen_set_number(TSUCHI_Mod* mod, TSUCHI_Screen screen, const char* id, double value);
// Declared settings may include editable text.
void ui_screen_resettable(TSUCHI_Screen screen);
} // namespace tsuchi
