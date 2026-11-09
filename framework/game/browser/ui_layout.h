#pragma once
#include "provider.h"

// Builtin Mods visual manifest, not generic ui_json v1 dynamic bindings.
// Game-thread parse into caller-owned copied storage; no input is retained.
namespace tsuchi { namespace browser { namespace ui {
constexpr uint32_t max_json_bytes=16384,max_sidebar_items=6;
struct Rect {float x=0,y=0,width=0,height=0;};
struct Entry {char label[128]{};};
enum class ImageFit : uint32_t { Contain=0,Cover=1 };
struct Card {float width=0,height=0,image_height=0,label_height=0,gap_x=0,gap_y=0;ImageFit image_fit=ImageFit::Contain;};
enum class SidebarAction : uint32_t { Query=0,Search=1,Installed=2 };
struct SidebarItem {char id[48]{},label[128]{};Sort sort=Sort::Latest;uint32_t category=0;SidebarAction action=SidebarAction::Query;};
struct Sidebar {
    float x=0,y=0,width=0,height=0,hidden_x=0,item_pitch=0;
    uint32_t duration_ms=0,count=0;
    SidebarItem items[max_sidebar_items]{};
};
enum class TextMode : uint32_t { Literal=0,Rich=1 };
struct Footer {char text[512]{};TextMode text_mode=TextMode::Literal;};
struct Browse {
    char title[128]{};uint32_t columns=0;
    Rect viewport{},hidden_viewport{};Card card{};Sidebar sidebar{};Footer footer{};
};
struct ActionRect {Rect rect{};char label[128]{};};
struct Files {Rect rect{};uint32_t rows=0;float pitch=0;};
struct PreviewStrip {Rect rect{};uint32_t count=0;float gap=0;};
struct Details {
    char title[128]{};
    Rect title_rect{},viewport{},gallery{},metadata{},description{};
    TextMode description_mode=TextMode::Literal;char description_hint[512]{};
    ActionRect download{},cancel{},author{};Files files{};PreviewStrip preview_strip{};Footer footer{};
};
struct Profile {char title[128]{};Rect avatar{},info{};ActionRect mods{},back{};Footer footer{};bool enabled=false;};
struct Layout {
    char title[128]{};Entry entry{};Rect header{},status{};Browse browse{};Details details{};Profile profile{};
};
struct Diagnostic {P4M_Result code=P4M_OK;uint32_t offset=0;char message[128]{};};
// Requires every fixed visual role. Unsupported fields/adapters never fall back
// to a partial layout. Failure leaves out byte-for-byte unchanged.
bool parse(const char* input,uint32_t bytes,Layout& out,Diagnostic* diagnostic=nullptr);
} } }
