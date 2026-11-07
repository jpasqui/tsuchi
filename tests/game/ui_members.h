// The members the widget Blueprints declare, at places of these tests' own. The framework finds them by name, as it
// does in the game, where each cooking of a Blueprint places them anew.
#pragma once
#include <string>

#include "fake_engine.h"

namespace ui_members {
namespace t = tsuchi::game::table;
constexpr int32_t settings = 0x648, footer = 0x610, footer_text = 0x530, ingame_text = 0x540;
constexpr int32_t tab_label = 0x540, tab_underline = 0x548, tab_selected = 0x530, tab_idle = 0x538;

inline void declare(uint64_t type, const char* name, int32_t offset) {
    std::u16string wide;
    while (*name) wide.push_back(char16_t(*name++));
    fake::field(type, wide, u"ObjectProperty", offset, 8);
}
inline void option_owner(uint64_t type) {
    declare(type, t::name::ui_option_settings_member, settings);
    declare(type, t::name::ui_option_footer_member, footer);
}
inline void footers(uint64_t footer_type, uint64_t ingame_type) {
    declare(footer_type, t::name::ui_footer_text_member, footer_text);
    declare(ingame_type, t::name::ui_footer_ingame_text_member, ingame_text);
}
inline void tab(uint64_t type) {
    declare(type, t::name::ui_mod_list_tab_label_member, tab_label);
    declare(type, t::name::ui_mod_list_tab_underline_member, tab_underline);
    declare(type, t::name::ui_mod_list_tab_selected_member, tab_selected);
    declare(type, t::name::ui_mod_list_tab_idle_member, tab_idle);
}
} // namespace ui_members
