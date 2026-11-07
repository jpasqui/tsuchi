// Owned engine UMG widgets. Independent of the generated Settings screen model.
#pragma once
#include "registry.h"
#include "tsuchi/ui.h"
namespace tsuchi {
void ui_widgets_start(TSUCHI_Mod* framework);
TSUCHI_Result ui_create_widget(TSUCHI_Mod*, TSUCHI_Object context, TSUCHI_Object of_class, TSUCHI_Object* out);
TSUCHI_Result ui_destroy_widget(TSUCHI_Mod*, TSUCHI_Object widget);
TSUCHI_Result ui_construct_widget(TSUCHI_Mod*, TSUCHI_Object widget, TSUCHI_Object of_class, TSUCHI_Object* out);
TSUCHI_Result ui_find_widget(TSUCHI_Mod*, TSUCHI_Object widget, const char* name, TSUCHI_Object* out);
TSUCHI_Result ui_set_widget_text(TSUCHI_Mod*, TSUCHI_Object widget, const char* text);
TSUCHI_Result ui_set_root_widget(TSUCHI_Mod*, TSUCHI_Object widget, TSUCHI_Object root);
TSUCHI_Result ui_add_child(TSUCHI_Mod*, TSUCHI_Object panel, TSUCHI_Object child, TSUCHI_Object* out);
TSUCHI_Result ui_hold_widget(TSUCHI_Mod*, TSUCHI_Object widget, int32_t z_order);
TSUCHI_Result ui_release_widget(TSUCHI_Mod*, TSUCHI_Object widget);
TSUCHI_Result ui_hold_button_events(TSUCHI_Mod*, TSUCHI_Object widget);
TSUCHI_Result ui_release_button_events(TSUCHI_Mod*, TSUCHI_Object widget);
}
