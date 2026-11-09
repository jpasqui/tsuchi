// Port of the existing Mods frontend; layout and navigation are preserved.
#include "native_view.h"
#include "native_access.h"
#include "ui_document_literal.h"
#include "ui_layout.h"
#include "card_presentation.h"
#include "detail_presentation.h"
#include "bridge.h"
#include "get_mods_ui.h"
#include "ui_title.h"
#include "ui_screens.h"
#include "ui_service.h"
#include "presenter.h"
#include "frame.h"
#include "mod_catalog.h"
#include "mods.h"
#include "mod_manager.h"
#include "mod_download.h"
#include "mod_choices_game.h"
#include "ui_settings.h"
#include "ui_cheats.h"
#include <string.h>
namespace tsuchi { namespace browser { namespace native {
namespace {
using namespace tsuchi::browser;
#define kRoot u32(t::constant::kept_flag)
#define kSetBrush t::data::browser_address_4c39d10
struct Lease { TextureToken token{};u32 bytes=0;bool added_root=false; };
struct Binding { TextureToken image{},texture{}; };
Lease leases[Images::SLOTS]{};
// One entry per stock UImage currently showing one of our textures. A texture
// can be shown by several images; the image cache bounds the textures.
Binding bindings[64]{};
Diagnostics diagnostic;
bool attached=false;
tsuchi::browser::ui::Layout layout;
u32 kColumns=3;
float kPitchX=440,kPitchY=320,kViewHeight=690;

void* ptr(u64 p){return reinterpret_cast<void*>(p);}
bool equal(TextureToken a,TextureToken b){return a.object==b.object&&a.index==b.index&&a.serial==b.serial;}
void* resolve(TextureToken t){
    if(!t.object||!t.serial||t.index>=u32(kMaxObjects))return nullptr;
    void* item=object_entry(s32(t.index));
    return item&&object_serial(item)==t.serial&&at<void*>(item,t::object_item::object)==ptr(t.object)?ptr(t.object):nullptr;
}
bool derives(void* type,const char* name){
    for(;type;type=at<void*>(type,t::browser_layout::class_super)){
        if(!object_entry_for(type))return false;
        if(named(type,name))return true;
    }
    return false;
}
// Class checks run thousands of times a frame. The answer for a class and a
// name does not change while that class is alive, so it is remembered instead
// of decoding names up the class chain on every call.
struct ClassMemo {void* type=nullptr;const char* name=nullptr;u32 index=0;bool result=false;};
ClassMemo class_memo[512]{};
bool is(void* object,const char* name){
    if(!object_entry_for(object))return false;
    void* type=at<void*>(object,t::browser_layout::object_class);if(!object_entry_for(type))return false;
    auto& memo=class_memo[((reinterpret_cast<u64>(type)>>4)*31+(reinterpret_cast<u64>(name)>>1))&511];
    const u32 index=u32(at<s32>(type,t::browser_layout::object_index));
    if(memo.type!=type||memo.name!=name||memo.index!=index)memo={type,name,index,derives(type,name)};
    return memo.result;
}
TextureToken token(void* object){const auto weak=game::to_weak_value(reinterpret_cast<u64>(object));return weak?TextureToken{reinterpret_cast<u64>(object),u32(weak),u32(weak>>32)}:TextureToken{};}
Lease* owned(TextureToken t){for(auto& l:leases)if(t.object&&equal(l.token,t))return &l;return nullptr;}
TextureToken stock_classes[8]{};
void* create_stock(u32 which,void* owner){
    if(which>7||!is(owner,"UserWidget"))return nullptr;
    static constexpr const char16_t* paths[]={
        u"/Game/Carrot4/UI/OutGame/Information/WBP_ui_Information.WBP_ui_Information_C",
        u"/Game/Carrot4/UI/OutGame/Information/Parts/WBP_ui_Info_PA_Picture.WBP_ui_Info_PA_Picture_C",
        u"/Game/Carrot4/UI/OutGame/Title/Parts/WBP_ui_PA_Title_Button.WBP_ui_PA_Title_Button_C",
        u"/Game/Carrot4/UI/OutGame/Title/Parts/WBP_ui_PA_Title_Gallery_Button.WBP_ui_PA_Title_Gallery_Button_C",
        u"/Game/Carrot4/UI/OutGame/Information/Parts/WBP_ui_Info_PA_Text.WBP_ui_Info_PA_Text_C",
        u"/Game/Carrot4/UI/InGame/Tablet/Parts/WBP_ui_Tablet_PA_Button_Menu.WBP_ui_Tablet_PA_Button_Menu_C",
        u"/Game/Carrot4/UI/Common/Loading/WBP_ui_LoadingLong.WBP_ui_LoadingLong_C",
        // The thin line the game's Information pages put under a block of text.
        u"/Game/Carrot4/UI/OutGame/Information/Parts/WBP_ui_Info_PA_Line.WBP_ui_Info_PA_Line_C"};
    // The soft-path load is needed once per class, not once per widget.
    void* type=resolve(stock_classes[which]);
    if(!type){
        struct Soft {s32 index=-1,serial=0;u32 tag=0,pad=0;u64 asset=0,subpath=0;s32 num=0,max=0;} soft;
        u32 length=0;while(paths[which][length])++length;
        soft.asset=fn<u64(*)(const char16_t*,u64,u64)>((reinterpret_cast<uint64_t>(t::fn::name_from_text) - game::main_base()))(paths[which],length,1);
        type=fn<void*(*)(Soft*)>((reinterpret_cast<uint64_t>(t::fn::load_soft_object) - game::main_base()))(&soft);
        if(!is(type,"Class"))return nullptr;
        stock_classes[which]=token(type);
    }
    void* object=fn<void*(*)(void*,void*,void*)>((reinterpret_cast<uint64_t>(t::fn::ui_create_widget) - game::main_base()))(owner,type,nullptr);
    return is(object,"UserWidget")?object:nullptr;
}
struct Rect {float left,top,width,height;};
Rect bounds(const ui::Rect& r){return {r.x,r.y,r.width,r.height};}
[[maybe_unused]] Rect detail_bounds(const ui::Rect& r){return {r.x-layout.details.viewport.x,r.y-layout.details.viewport.y,r.width,r.height};}
Rect detail_content_bounds(const ui::Rect& r){return {r.x-layout.details.gallery.x,r.y-layout.details.gallery.y,r.width,r.height};}
// The strip of small screenshots is centered under the large one, however
// many it holds. Width of one, and where the first starts (content
// coordinates, which begin at the large screenshot's left edge).
float strip_width(){const auto& strip=layout.details.preview_strip;return (strip.rect.width-(strip.count-1)*strip.gap)/strip.count;}
float strip_span(u32 previews){const auto& strip=layout.details.preview_strip;const u32 slots=previews<strip.count?previews:strip.count;
    return slots?float(slots)*(strip_width()+strip.gap)-strip.gap:0;}
float strip_left(u32 previews){return (layout.details.gallery.width-strip_span(previews))*0.5f;}
struct Size {float width,height;};
u64 clock_ticks(){register u64 ticks asm("x0");asm volatile("svc #0x1e":"=r"(ticks)::"memory");return ticks;}
struct ObjectRoot {TextureToken token{};bool added=false;};
bool root(ObjectRoot& out,void* object){
    const auto t=token(object);if(!t.object)return false;
    void* item=object_entry_for(object);out={t,!(__atomic_fetch_or(&at<u32>(item,t::object_item::flags),kRoot,__ATOMIC_SEQ_CST)&kRoot)};return true;
}
void unroot(ObjectRoot& r){if(void* o=resolve(r.token))if(r.added)__atomic_fetch_and(&at<u32>(object_entry_for(o),t::object_item::flags),~kRoot,__ATOMIC_SEQ_CST);r={};}
void visible(void* o,u32 value){if(is(o,"Widget"))method<void(*)(void*,u32)>(o,t::browser_layout::virtual_288)(o,value);}
// Scans of the object array compare the FName index first; decoding every
// object's name is what made them slow. The decoded name stays the definition.
u32 name_index(const char* name){
    char16_t wide[64];s32 n=0;while(name[n]&&n<63){wide[n]=char16_t(name[n]);++n;}wide[n]=0;
    return u32(fn<u64(*)(const char16_t*,s32,u32)>((reinterpret_cast<uint64_t>(t::fn::name_from_text) - game::main_base()))(wide,n,1));
}
bool has_name(void* o,u32 index,const char* name){return at<u32>(o,t::browser_layout::object_name)==index&&at<u32>(o,t::browser_layout::object_name_number)==0&&named(o,name);}
bool same_name(const char* a,const char* b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
struct ClassEntry {const char* name=nullptr;TextureToken token{};};
ClassEntry class_cache[16]{};
void* class_named(const char* name){
    for(auto& c:class_cache)if(c.name&&same_name(c.name,name)){if(void* o=resolve(c.token))return o;c={};}
    const s32 count=global<s32>(kObjectCount);if(count<1||count>kMaxObjects)return nullptr;
    const u32 index=name_index(name);void* found=nullptr;
    for(s32 i=0;i<count&&!found;++i){void* e=object_entry(i);void* o=e?at<void*>(e,t::object_item::object):nullptr;
        if(o&&has_name(o,index,name)&&is(o,"Class"))found=o;}
    for(s32 i=0;i<count&&!found;++i){void* e=object_entry(i);void* o=e?at<void*>(e,t::object_item::object):nullptr;
        if(o&&named(o,name)&&is(o,"Class"))found=o;}
    if(found)for(auto& c:class_cache)if(!c.name){c={name,token(found)};break;}
    return found;
}
bool inside(void* o,void* owner){
    void* outer=at<void*>(o,t::browser_layout::object_outer);
    for(u32 depth=0;outer&&depth<8;++depth){
        if(outer==owner)return true;if(!object_entry_for(outer))return false;outer=at<void*>(outer,t::browser_layout::object_outer);
    }
    return false;
}
// Up to eight named widgets under one owner in a single pass.
void children_named(void* owner,const char* const* names,void** out,u32 n){
    u32 index[8]{};if(n>8)n=8;for(u32 k=0;k<n;++k){out[k]=nullptr;index[k]=name_index(names[k]);}
    const s32 count=global<s32>(kObjectCount);if(count<1||count>kMaxObjects)return;
    u32 missing=n;
    for(s32 i=0;i<count&&missing;++i){void* e=object_entry(i);void* o=e?at<void*>(e,t::object_item::object):nullptr;if(!o)continue;
        for(u32 k=0;k<n;++k)if(!out[k]&&has_name(o,index[k],names[k])&&inside(o,owner)){out[k]=o;--missing;}}
    for(s32 i=0;i<count&&missing;++i){void* e=object_entry(i);void* o=e?at<void*>(e,t::object_item::object):nullptr;if(!o)continue;
        for(u32 k=0;k<n;++k)if(!out[k]&&named(o,names[k])&&inside(o,owner)){out[k]=o;--missing;}}
}
[[maybe_unused]] void* child_named(void* owner,const char* name){void* out=nullptr;children_named(owner,&name,&out,1);return out;}
void* construct(const char* name,void* tree){
    void* type=class_named(name);if(!type||!is(tree,"WidgetTree"))return nullptr;
    u64 parameters[8]{reinterpret_cast<u64>(type),reinterpret_cast<u64>(tree)};
    return fn<void*(*)(const void*)>((reinterpret_cast<uint64_t>(t::fn::ui_construct) - game::main_base()))(parameters);
}
void* add(void* panel,void* child){return is(panel,"PanelWidget")&&is(child,"Widget")?fn<void*(*)(void*,void*)>((reinterpret_cast<uint64_t>(t::fn::ui_add_child) - game::main_base()))(panel,child):nullptr;}
// The slot setters are only called when the value changes. Layout runs every
// frame for every visible widget, and these calls reach into Slate each time.
bool same_rect(const Rect& a,const Rect& b){return a.left==b.left&&a.top==b.top&&a.width==b.width&&a.height==b.height;}
void rect(void* slot,Rect r){if(is(slot,"CanvasPanelSlot")&&!same_rect(at<Rect>(slot,t::browser_layout::field_38),r))fn<void(*)(void*,Rect)>((reinterpret_cast<uint64_t>(t::fn::ui_slot_offsets) - game::main_base()))(slot,r);}
void fitted_slot(void* slot,Rect r){if(!is(slot,"CanvasPanelSlot"))return;
    // The original CanvasPanelSlot setters receive their four scalars in S0-S3.
    // Keep the asset's alignment, compensating the new top-left anchor for it.
    // AutoSize otherwise substitutes the stock desired width (about 850px),
    // defeating the smaller pool geometry and its alignment compensation.
    if(at<u8>(slot,t::browser_layout::field_60))fn<void(*)(void*,bool)>((reinterpret_cast<uint64_t>(t::fn::ui_slot_auto_size) - game::main_base()))(slot,false);
    if(!same_rect(at<Rect>(slot,t::browser_layout::field_48),{0,0,0,0}))fn<void(*)(void*,Rect)>((reinterpret_cast<uint64_t>(t::fn::ui_slot_anchors) - game::main_base()))(slot,{0,0,0,0});
    r.left+=at<float>(slot,t::browser_layout::field_58)*r.width;r.top+=at<float>(slot,t::browser_layout::field_5c)*r.height;rect(slot,r);
}
void fitted_text(void* text,s32 width,s32 lines=1,u8 justification=0){if(!is(text,"CarrotRichTextBlock"))return;
    if(at<s32>(text,t::browser_layout::field_4d8)==width&&at<s32>(text,t::browser_layout::field_4dc)==lines&&at<float>(text,t::browser_layout::wrap_width)==float(width)&&
       !(at<u8>(text,t::browser_layout::auto_wrap)&1)&&at<u8>(text,t::browser_layout::justification)==justification)return;
    at<s32>(text,t::browser_layout::field_4d8)=width;at<s32>(text,t::browser_layout::field_4dc)=lines;
    at<float>(text,t::browser_layout::wrap_width)=float(width);at<u8>(text,t::browser_layout::auto_wrap)&=0xFE;
    at<u8>(text,t::browser_layout::justification)=justification; // original TextLayoutWidget justification
    if(at<void*>(text,t::browser_layout::rich_slate))fn<void(*)(void*)>((reinterpret_cast<uint64_t>(t::fn::ui_rich_text_sync) - game::main_base()))(text);
}
// caption: where the label sits inside the button; null keeps the stock inset.
// Every caller states its final caption geometry here, so nothing is set to
// one value and then to another within a frame.
void fit_button(void* button,float width,float height,const Rect* caption=nullptr,float scale=1.f,s32 lines=0,u8 justification=1){
    if(!is(button,"WBP_ui_PA_Title_Gallery_Button_C"))return;
    void* base=at<void*>(button,t::browser_layout::gallery_base),*text=at<void*>(button,t::browser_layout::gallery_caption);
    if(is(base,"WBP_ui_PA_Button_Base_C")){
        at<Size>(base,t::browser_layout::field_4dc)={width,height};
        fitted_slot(at<void*>(base,t::browser_layout::widget_slot),{0,0,width,height});
        // Live hierarchy proves this nested canvas retains the stock 854px
        // centered width even when its own images and outer button are fitted.
        void* base_image=at<void*>(base,t::browser_layout::field_5d8),*image_slot=is(base_image,"Image")?at<void*>(base_image,t::browser_layout::widget_slot):nullptr;
        void* canvas=is(image_slot,"CanvasPanelSlot")?at<void*>(image_slot,t::browser_layout::widget_slot):nullptr;
        if(is(canvas,"CanvasPanel")&&named(canvas,"N_Cvs_Button"))fitted_slot(at<void*>(canvas,t::browser_layout::widget_slot),{0,0,width,height});
        // Fit the real stock base/focus/press images as well as the outer slot.
        const u64 offsets[]={t::browser_layout::field_5d8,t::browser_layout::field_5f0,t::browser_layout::field_5f8};
        for(u64 offset:offsets){void* image=at<void*>(base,offset);
            if(is(image,"Image"))fitted_slot(at<void*>(image,t::browser_layout::widget_slot),{0,0,width,height});}
    }
    if(text){const Rect area=caption?*caption:Rect{18,4,width-36,height-8};
        fitted_slot(at<void*>(text,t::browser_layout::widget_slot),area);
        // Keep the stock face and outline. The literal sibling only reduces
        // its size when the actual wrapped text exceeds this button.
        at<float>(text,t::browser_layout::text_scale)=scale;
        fitted_text(text,s32(area.width),lines?lines:height>70?2:1,justification);}
}
void fit_image(void* image,const ImageView& view,float width,float height,bool cover=false){if(!is(image,"Image")||!view.size.width||!view.size.height)return;
    float w=width,h=width*float(view.size.height)/float(view.size.width);
    if(cover?h<height:h>height){h=height;w=height*float(view.size.width)/float(view.size.height);}
    fitted_slot(at<void*>(image,t::browser_layout::widget_slot),{(width-w)*0.5f,(height-h)*0.5f,w,h});
}
void fit_card(void* button,const ui::Card& shape){
    // One native Gallery button owns the image and caption, including its
    // complete focus frame. Caption geometry is local to that full button.
    const Rect caption{0,0,shape.width-36,42};
    fit_button(button,shape.width,shape.height,&caption,0.7f,2,0);
}
void rich_label(void* text,const char16_t* source){
    if(!is(text,"CarrotRichTextBlock")||!source)return;s32 length=1;while(source[length-1]&&length<=4096)++length;
    if(source[length-1])return;
    GameString s{const_cast<char16_t*>(source),length,length};
    auto t=fn<GameText(*)(const GameString*)>((reinterpret_cast<uint64_t>(t::fn::text_from_string) - game::main_base()))(&s);fn<void(*)(void*,const GameText*)>((reinterpret_cast<uint64_t>(t::fn::rich_text_set) - game::main_base()))(text,&t);
    void* owner=ptr(t.words[1]);if(owner&&__atomic_fetch_sub(&at<u32>(owner,t::shared_count::holders),1,__ATOMIC_ACQ_REL)==1){
        method<void(*)(void*)>(owner,t::shared_count_table::destroy_object)(owner);if(__atomic_fetch_sub(&at<u32>(owner,t::shared_count::watchers),1,__ATOMIC_ACQ_REL)==1)method<void(*)(void*)>(owner,t::shared_count_table::destroy_self)(owner);
    }
}
// Literal text uses the qualified original TextBlock setters and borrows only
// the stock widget's font/color. No provider or authored decorator is executed.
// fit_line: a single line that is reduced until it fits this width (0: off).
struct PlainLabel {TextureToken source{},text{},slot{};u64 hash=0,style_hash=0,motion_hash=0,color_hash=0;u32 units=0;
    bool fixed=false,button=false,rich_mode=false,uniform_font=false;Rect position{};float fit_scale=1.f,fit_width=0,fit_line=0;u32 fit_pending=0;};
PlainLabel plain_labels[160]{};
void* literal_class(void*,const char* name){return class_named(name);}
bool literal_created(void*,void* object){return is(object,"TextBlock")&&named(at<void*>(object,t::browser_layout::object_class),"TextBlock");}
ui_document_literal::Backend literal_backend(){return {game::main_base(),nullptr,literal_class,literal_created};}
PlainLabel* plain_for(void* rich){
    if(!is(rich,"CarrotRichTextBlock"))return nullptr;
    for(auto& l:plain_labels)if(l.source.object==reinterpret_cast<u64>(rich)&&resolve(l.source)==rich&&resolve(l.text))return &l;
    PlainLabel* empty=nullptr;
    for(auto& l:plain_labels)if(!resolve(l.source)||!resolve(l.text)){l={};if(!empty)empty=&l;}
    if(!empty)return nullptr;
    void* source_slot=at<void*>(rich,t::browser_layout::widget_slot),*panel=is(source_slot,"PanelSlot")?at<void*>(source_slot,t::browser_layout::widget_slot):nullptr;
    if(!is(panel,"PanelWidget"))return nullptr;
    void* tree=at<void*>(rich,t::browser_layout::object_outer);for(u32 depth=0;tree&&!is(tree,"WidgetTree")&&depth<8;++depth)tree=object_entry_for(tree)?at<void*>(tree,t::browser_layout::object_outer):nullptr;
    if(!is(tree,"WidgetTree"))return nullptr;
    void* text=ui_document_literal::create(literal_backend(),tree,rich);
    if(text)at<u8>(text,t::browser_layout::clipping)=1; // reflected ClipToBounds before Slate creation
    void* slot=text?add(panel,text):nullptr;
    if(!is(slot,"PanelSlot"))return nullptr;
    *empty={token(rich),token(text),token(slot)};
    for(void* outer=tree;outer;outer=object_entry_for(outer)?at<void*>(outer,t::browser_layout::object_outer):nullptr){
        if(is(outer,"WBP_ui_PA_Title_Gallery_Button_C")||is(outer,"WBP_ui_Tablet_PA_Button_Menu_C")){empty->button=true;break;}
        if(is(outer,"UserWidget"))break;
    }
    ui_document_literal::set_auto_wrap(literal_backend(),text,true);return empty;
}
u64 style_bytes(u64 hash,const void* source,u32 count){const auto* bytes=static_cast<const u8*>(source);
    for(u32 i=0;i<count;++i){hash^=bytes[i];hash*=1099511628211ull;}return hash;}
void sync_plain_style(PlainLabel& l,void* rich,void* text){
    float scale=at<float>(rich,t::browser_layout::text_scale);if(!(scale>=0.25f&&scale<=2.f))scale=1.f;
    scale*=l.fit_scale;if(scale<0.25f)scale=0.25f;if(scale>2.f)scale=2.f;
    u64 hash=style_bytes(1469598103934665603ull,&at<u8>(rich,t::browser_layout::font_or_brush_resource),88);
    hash=style_bytes(hash,&at<u8>(rich,t::browser_layout::text_color),16);
    hash=style_bytes(hash,&at<u8>(rich,t::browser_layout::shadow_offset),24);
    hash=style_bytes(hash,&at<u8>(rich,t::browser_layout::justification),1);hash=style_bytes(hash,&scale,sizeof scale);
    if(hash!=l.style_hash&&ui_document_literal::set_style(literal_backend(),text,rich)&&
       ui_document_literal::set_font_scale(literal_backend(),text,rich,scale))l.style_hash=hash;
    hash=style_bytes(1469598103934665603ull,&at<u8>(rich,t::browser_layout::render_transform),36);
    hash=style_bytes(hash,&at<u8>(rich,t::browser_layout::opacity),4);
    if(hash!=l.motion_hash){
        // Original setters update the live Slate widget as well as storage.
        if(ui_document_literal::set_render_transform(literal_backend(),text,&at<float>(rich,t::browser_layout::render_transform),&at<float>(rich,t::browser_layout::render_pivot))){
            fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_set_render_opacity) - game::main_base()))(text,at<float>(rich,t::browser_layout::opacity));l.motion_hash=hash;}
    }
}
void sync_plain(){for(auto& l:plain_labels){void* rich=resolve(l.source),*text=resolve(l.text),*slot=resolve(l.slot);
    if(!rich||!text||!slot)continue;
    if(l.rich_mode){visible(rich,3);visible(text,1);if(l.fixed)fitted_slot(at<void*>(rich,t::browser_layout::widget_slot),l.position);continue;}
    visible(rich,1);visible(text,3);
    sync_plain_style(l,rich,text);
    if(l.fixed){fitted_slot(slot,l.position);continue;}
    if(l.fit_line>0){
        // Wait for a real Slate prepass after each change, as the buttons do.
        const Size desired=fn<Size(*)(void*)>(t::data::browser_address_4c6f4f0)(text);
        if(l.fit_pending)--l.fit_pending;
        else if(desired.width>l.fit_line+1&&desired.width<10000&&l.fit_scale>0.5f){
            l.fit_scale*=l.fit_line/desired.width;if(l.fit_scale<0.5f)l.fit_scale=0.5f;
            l.fit_pending=2;sync_plain_style(l,rich,text);
        }
    }
    void* original_slot=at<void*>(rich,t::browser_layout::widget_slot);
    if(is(original_slot,"GridSlot")&&is(slot,"GridSlot")){
        // Title-button live capture proves its text occupies column2/layer5.
        // Verified original setters copy reflected fields and update Slate.
        fn<void(*)(void*,Rect)>(t::data::browser_address_4c35c50)(slot,at<Rect>(original_slot,t::browser_layout::field_38));
        fn<void(*)(void*,u32)>(t::data::browser_address_4c360d0)(slot,at<u8>(original_slot,t::browser_layout::field_48));
        fn<void(*)(void*,u32)>(t::data::browser_address_4c360f0)(slot,at<u8>(original_slot,t::browser_layout::field_49));
        const u64 setters[]={t::data::browser_address_4c353c0,t::data::browser_address_4c35e30,t::data::browser_address_4c353e0,t::data::browser_address_4c35f60,t::data::browser_address_4c35f80};
        for(u32 i=0;i<5;++i)fn<void(*)(void*,s32)>(setters[i])(slot,at<s32>(original_slot,t::browser_layout::field_4c+i*4));
        fn<void(*)(void*,Size)>(t::data::browser_address_4c360b0)(slot,at<Size>(original_slot,t::browser_layout::field_60));continue;
    }
    if(!is(original_slot,"CanvasPanelSlot")||!is(slot,"CanvasPanelSlot"))continue;
    Rect r=at<Rect>(original_slot,t::browser_layout::field_38);r.left-=at<float>(original_slot,t::browser_layout::field_58)*r.width;r.top-=at<float>(original_slot,t::browser_layout::field_5c)*r.height;
    const Rect anchors=at<Rect>(original_slot,t::browser_layout::field_48);
    if(anchors.left||anchors.top||anchors.width||anchors.height||at<u8>(original_slot,t::browser_layout::field_60)){
        // Verified VM04D53680 -> original04C1D670: X0/S0/S1; also
        // synchronizes the live Slate slot, unlike writing reflected storage.
        fn<void(*)(void*,Size)>((reinterpret_cast<uint64_t>(t::fn::ui_slot_alignment) - game::main_base()))(slot,at<Size>(original_slot,t::browser_layout::field_58));
        fn<void(*)(void*,Rect)>((reinterpret_cast<uint64_t>(t::fn::ui_slot_anchors) - game::main_base()))(slot,anchors);
        fn<void(*)(void*,bool)>((reinterpret_cast<uint64_t>(t::fn::ui_slot_auto_size) - game::main_base()))(slot,at<u8>(original_slot,t::browser_layout::field_60)!=0);
        rect(slot,at<Rect>(original_slot,t::browser_layout::field_38));
    }else {
        if(l.button){
            if(l.fit_width!=r.width){l.fit_width=r.width;l.fit_scale=1.f;l.fit_pending=2;}
            const Size desired=fn<Size(*)(void*)>(t::data::browser_address_4c6f4f0)(text);
            if(l.fit_pending)--l.fit_pending;
            else if(!l.uniform_font&&desired.height>r.height+1&&desired.height<10000&&l.fit_scale>0.65f){
                l.fit_scale*=r.height/desired.height;if(l.fit_scale<0.65f)l.fit_scale=0.65f;
                l.fit_pending=2;sync_plain_style(l,rich,text);
            }
            if(!l.uniform_font&&desired.height>0&&desired.height<r.height){r.top+=(r.height-desired.height)*0.5f;r.height=desired.height;}
        }
        fitted_slot(slot,r);
    }
    visible(rich,1);visible(text,3);
}}
void forget_plain(void* owner){for(auto& l:plain_labels){void* rich=resolve(l.source),*outer=rich;
    for(u32 depth=0;outer&&depth<12;++depth){if(outer==owner){l={};break;}outer=object_entry_for(outer)?at<void*>(outer,t::browser_layout::object_outer):nullptr;}
}}
void label_mode(void* rich,const char16_t* source,bool rich_mode){
    auto* l=plain_for(rich);if(!l)return;u32 units=0;u64 hash=1469598103934665603ull;
    while(source[units]&&units<4096){hash^=uint16_t(source[units++]);hash*=1099511628211ull;}
    if(source[units])return;
    if(hash!=l->hash||units!=l->units||l->rich_mode!=rich_mode){
        bool applied=true;
        if(rich_mode)rich_label(rich,source);
        else applied=ui_document_literal::set_text(literal_backend(),resolve(l->text),source,units);
        if(applied){l->rich_mode=rich_mode;l->hash=hash;l->units=units;l->fit_scale=1.f;l->fit_pending=2;}}
}
void label(void* rich,const char16_t* source){label_mode(rich,source,false);}
// UTF-8 conversion preserves the source; each role explicitly chooses whether
// the result is literal text or native Carrot markup.
template<u32 N> void utf16(const char* source,char16_t (&out)[N]){
    u32 n=0;const auto* p=reinterpret_cast<const u8*>(source);
    while(*p&&n<N-2){u32 c=*p++;u32 extra=0;
        if(c>=0xF0&&c<=0xF4){c&=7;extra=3;}else if(c>=0xE0&&c<=0xEF){c&=15;extra=2;}else if(c>=0xC2&&c<=0xDF){c&=31;extra=1;}else if(c>=128)c=0xFFFD;
        for(u32 i=0;i<extra;++i){if((*p&0xC0)!=0x80){c=0xFFFD;break;}c=(c<<6)|(*p++&63);}
        if(c>0x10FFFF||(c>=0xD800&&c<=0xDFFF))c=0xFFFD;
        if(c>=0x10000){c-=0x10000;out[n++]=char16_t(0xD800+(c>>10));out[n++]=char16_t(0xDC00+(c&1023));}else out[n++]=char16_t(c);
    }out[n]=0;
}
void label_utf8(void* text,const char* source){char16_t buffer[256]{};utf16(source,buffer);label(text,buffer);}
constexpr u32 kCards=12,kStats=3,kInfoRows=4;
u32 kFileRows=2;
// clip_slot: the turned clip that cuts a picture's corners (see clipped_picture).
// revision: the controller revision the stat row was last written for.
struct Card {TextureToken picture{},image{},button{},placeholder{},loader{},title_canvas{},metadata{},stat_icons[kStats]{},stat_text[kStats]{};void* stat_slots[kStats]{};void* stat_icon_slots[kStats]{};void* picture_slot=nullptr;void* clip_slot=nullptr;void* button_slot=nullptr;void* placeholder_slot=nullptr;void* loader_slot=nullptr;void* title_slot=nullptr;void* metadata_slot=nullptr;u64 marquee_start=0;u32 id=0,generation=0,revision=0;bool focus=false,waiting=false,fallback=false;};
u32 hovered_card=0;
// The stock loading-screen widget with everything but its spinning icon hidden.
void loading_icon(Card& c,const Rect& area,bool waiting){
    void* loader=resolve(c.loader);if(!loader)return;
    const float least=area.width<area.height?area.width:area.height,size=least<64?least:64;
    fitted_slot(c.loader_slot,{area.left+(area.width-size)*0.5f,area.top+(area.height-size)*0.5f,size,size});
    const u64 hidden[]{t::browser_layout::field_528,t::browser_layout::field_530,t::browser_layout::field_540};for(u64 field:hidden)visible(at<void*>(loader,field),1);
    void* icon=at<void*>(loader,t::browser_layout::field_538);fitted_slot(icon?at<void*>(icon,t::browser_layout::widget_slot):nullptr,{0,0,size,size});visible(icon,3);
    if(waiting&&!c.waiting){void* animation=at<void*>(loader,t::browser_layout::field_508);
        if(is(animation,"WidgetAnimation"))fn<void*(*)(void*,void*,float,s32,u32,float,bool)>((reinterpret_cast<uint64_t>(t::fn::ui_mod_list_play_animation) - game::main_base()))(loader,animation,0,0,0,1,false);}
    c.waiting=waiting;visible(loader,waiting?3:1);
    if(waiting)fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_set_render_opacity) - game::main_base()))(loader,1.f);
}
enum class HostKind : u8 { Browse, Detail };
// stage: build steps done so far (see build_host). view: where the scrolling
// content is on screen now. The *_width/_height fields remember the last value
// given to a size setter so it is only called on a change. avatar and profile
// are the author's picture and details, shown above the grid on an author's page.
struct Host {ObjectRoot root{};TextureToken scroll{},canvas{},sizer{},description{},description_scroll{},description_sizer{},pinned{},metadata{},status{},header_widget{},header{},footer{},sidebar{},sidebar_buttons[ui::max_sidebar_items]{};TextureToken sidebar_slots[ui::max_sidebar_items]{};
    TextureToken panel{},information{},pager{},notice{},profile{};
    // The mod page's stat row (likes, downloads, date): text cells and their slots.
    // The mod page's details card: a tagline, then one line per figure, each
    // led by its icon with the game's thin Information line under it.
    TextureToken info_text[kInfoRows]{},info_line[kInfoRows]{},tagline{};
    void* info_slots[kInfoRows]{};void* info_icon_slots[kInfoRows]{};void* info_line_slots[kInfoRows]{};void* tagline_slot=nullptr;
    // The L and R prompts at the ends of the mod page's screenshot strip.
    TextureToken strip_prompts[2]{};void* strip_prompt_slots[2]{};
    void* scroll_slot=nullptr;void* sidebar_slot=nullptr;void* extent_slot=nullptr;void* description_slot=nullptr;void* metadata_slot=nullptr;void* status_slot=nullptr;void* notice_slot=nullptr;void* profile_slot=nullptr;
    Card cards[kCards]{};Card avatar{};u32 revision=0,anchor_revision=0,detail_id=0,detail_state=0,stage=0;
    float offset=0,description_extent=0,sizer_width=-1,sizer_height=-1,description_height=-1;Rect view{};
    bool shown=false,description_seen=false,built=false;HostKind kind=HostKind::Browse;};
Host browse_host,detail_host;
void command_search();
bool transfer_active();
// category_mode: the left panel lists the categories instead of the sections.
bool sidebar_focus=true,category_mode=false;
u32 sidebar_selected=0,category_cursor=0,category_first=0;
float sidebar_progress=0;
u64 sidebar_time=0;
// Entry 0 is every category; the provider's categories follow.
u32 category_entries(){return controller().categories().count+1;}
u32 category_id(u32 entry){return entry&&entry<category_entries()?controller().categories().records[entry-1].id:0;}
const char* category_name(u32 entry){return entry&&entry<category_entries()?controller().categories().records[entry-1].name:"All categories";}
void size_width(void* sizer,float& last,float value){if(sizer&&last!=value){fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_size_box_width) - game::main_base()))(sizer,value);last=value;}}
void size_height(void* sizer,float& last,float value){if(sizer&&last!=value){fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_size_box_height) - game::main_base()))(sizer,value);last=value;}}
bool author_page();
constexpr float kProfileScale=0.72f;
void sidebar_query(){const auto& item=layout.browse.sidebar.items[sidebar_selected];if(item.action==ui::SidebarAction::Search)return;
    if(item.action==ui::SidebarAction::Installed){controller().open_installed();return;}
    // A section is a sort. The category chosen with Y stays until it is changed.
    Query q{};q.sort=item.sort;q.category_id=item.category?item.category:controller().query().category_id;controller().change_query(q);}
void sidebar_tick(){auto& h=browse_host;if(!resolve(h.sidebar))return;
    const u64 now=clock_ticks();float dt=sidebar_time?float(now-sidebar_time)/19200000.f:0;sidebar_time=now;if(dt>0.05f)dt=0.05f;
    const float step=dt*1000.f/layout.browse.sidebar.duration_ms;
    sidebar_progress+=sidebar_focus?-step:step;if(sidebar_progress<0)sidebar_progress=0;if(sidebar_progress>1)sidebar_progress=1;
    // An author's page has the author in the left column: the sections are
    // away and the grid stays where it is when they are on show.
    const bool author=author_page();
    const float t=sidebar_progress*sidebar_progress*(3-2*sidebar_progress),side=author?1.f:t,wide=author?0.f:t;const auto& s=layout.browse.sidebar;
    fitted_slot(h.sidebar_slot,{s.x+(s.hidden_x-s.x)*side,s.y,s.width,s.height});
    const auto& a=layout.browse.viewport;const auto& b=layout.browse.hidden_viewport;
    h.view={a.x+(b.x-a.x)*wide,a.y+(b.y-a.y)*wide,a.width+(b.width-a.width)*wide,a.height+(b.height-a.height)*wide};
    fitted_slot(h.scroll_slot,h.view);
    size_width(resolve(h.sizer),h.sizer_width,h.view.width);
    if(category_mode){if(category_cursor>=category_entries())category_cursor=0;
        category_first=detail_presentation::window(category_first,category_cursor,category_entries(),s.count);}
    for(u32 i=0;i<s.count;++i){void* button=resolve(h.sidebar_buttons[i]);if(!button)continue;
        const u32 entry=category_first+i;const bool shown=!category_mode||entry<category_entries();
        visible(button,shown?0:1);if(!shown)continue;
        void* text=at<void*>(button,t::browser_layout::field_520),*text_slot=is(text,"Widget")?at<void*>(text,t::browser_layout::widget_slot):nullptr;
        void* list=is(text_slot,"CanvasPanelSlot")?at<void*>(text_slot,t::browser_layout::widget_slot):nullptr;
        visible(list,0);fitted_slot(list?at<void*>(list,t::browser_layout::widget_slot):nullptr,{0,0,s.width,64});
        fitted_slot(text_slot,{22,12,s.width-44,48});fitted_text(text,s32(s.width-44),1);
        if(is(text,"CarrotRichTextBlock"))at<float>(text,t::browser_layout::text_scale)=0.8f;label_utf8(text,category_mode?category_name(entry):s.items[i].label);
        auto* caption=plain_for(text);float label_width=s.width-62;
        if(caption){caption->uniform_font=true;caption->fit_scale=1.f;
            const Size desired=fn<Size(*)(void*)>(t::data::browser_address_4c6f4f0)(resolve(caption->text));
            if(desired.width>0&&desired.width<label_width)label_width=desired.width;}
        visible(at<void*>(button,t::browser_layout::field_538),1);visible(at<void*>(button,t::browser_layout::field_540),1);
        const bool on=sidebar_focus&&(category_mode?entry==category_cursor:i==sidebar_selected);
        void* lt=at<void*>(button,t::browser_layout::field_528),*rb=at<void*>(button,t::browser_layout::field_530);
        fitted_slot(lt?at<void*>(lt,t::browser_layout::widget_slot):nullptr,{0,8,24,24});fitted_slot(rb?at<void*>(rb,t::browser_layout::widget_slot):nullptr,{label_width+30,40,24,24});
        visible(lt,on?3:1);visible(rb,on?3:1);
    }
}
u32 detail_file=0,detail_file_first=0,detail_selection=0,detail_preview=0,detail_preview_first=0;
// Which side of the mod page has the focus (detail_presentation::move).
detail_presentation::Pane detail_pane=detail_presentation::Pane::Actions;
constexpr float kStripPrompt=64,kStripPromptGap=14,kStripPromptScale=0.6f;   // an L or R prompt beside the screenshot strip
// Between the parts of a header trail ("Skins > Moss Drip"): a single right angle quote.
constexpr const char* kTrailSeparator="  \xE2\x80\xBA  ";
// When the screenshot on show last changed (a new mod, another screenshot).
u64 detail_shown_since=0;u64 detail_shown_key=0;
bool detail_focus_pending=false,prebuild_failed=false;
u64 ui_held=0;bool suppress_back=false;
u64 input_buttons();
template<u32 N> void append16(char16_t (&out)[N],const char16_t* text){u32 n=0;while(n<N-1&&out[n])++n;while(*text&&n<N-1)out[n++]=*text++;out[n]=0;}
constexpr const char16_t* kRetryPrompt=u"    [PictureFont:IconData key=\"X_Button\" scale=\"0.76\"/] Retry";
// A screen's footer from the manifest; `retry` adds the prompt while something
// on it has failed, so the prompt is only there when pressing it does something.
template<u32 N> bool footer_text(const ui::Footer& footer,bool retry,char16_t (&out)[N]){
    utf16(footer.text,out);const bool rich=footer.text_mode==ui::TextMode::Rich;
    if(retry)append16(out,rich?kRetryPrompt:u"    X Retry");
    return rich;
}
// Header and footer are given their final text once per frame.
// fit: the width the title may take (0: the whole header line).
void chrome(Host& h,const char* title,const char16_t* footer_text,bool rich,float fit=0){
    void* header_widget=resolve(h.header_widget);visible(header_widget,0);
    if(header_widget){
        fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_set_render_opacity) - game::main_base()))(header_widget,1.f);
        void* tree=at<void*>(header_widget,t::browser_layout::widget_tree),*root_widget=tree?at<void*>(tree,t::browser_layout::widget_slot):nullptr;
        if(is(root_widget,"Widget")){visible(root_widget,0);fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_set_render_opacity) - game::main_base()))(root_widget,1.f);}
        void* text=resolve(h.header);if(is(text,"Widget"))fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_set_render_opacity) - game::main_base()))(text,1.f);
    }
    void* header=resolve(h.header);label_utf8(header,title);
    // A mod's name is the page title, so the header is one line that shrinks to fit.
    const float room=fit>0?fit:layout.header.width;
    if(auto* line=plain_for(header))if(line->fit_line!=room){
        line->fit_line=room;line->fit_scale=1.f;line->fit_pending=2;ui_document_literal::set_auto_wrap(literal_backend(),resolve(line->text),false);}
    void* rich_footer=resolve(h.footer);
    // Preserve the stock Footer widget, cyan background, rich-text slot,
    // dark font color and right alignment. Only its prompt string changes.
    label_mode(rich_footer,footer_text,rich);
    if(auto* footer=plain_for(rich_footer)){struct Color {float rgba[4];u64 rule=0,shared[2]{};} color{};
        for(u32 i=0;i<4;++i)color.rgba[i]=at<float>(rich_footer,t::browser_layout::text_color+i*4);
        const u64 hash=style_bytes(1469598103934665603ull,color.rgba,sizeof color.rgba);
        if(hash!=footer->color_hash){fn<void(*)(void*,const Color*)>(t::data::browser_address_4c61a20)(resolve(footer->text),&color);footer->color_hash=hash;}}
}
void focus(Card& card,bool on){
    if(card.focus==on)return;card.focus=on;void* b=resolve(card.button);if(!b)return;
    void* a=at<void*>(b,on?t::browser_layout::field_5d0:t::browser_layout::field_5c8);if(is(a,"WidgetAnimation"))fn<void*(*)(void*,void*,float,s32,u32,float,bool)>((reinterpret_cast<uint64_t>(t::fn::ui_mod_list_play_animation) - game::main_base()))(b,a,0,1,0,1,false);
    void* base=at<void*>(b,t::browser_layout::gallery_base);if(is(base,"WBP_ui_PA_Button_Base_C")){
        void* focus_image=at<void*>(base,t::browser_layout::field_5f0);visible(focus_image,on?0:1);
        if(on&&is(focus_image,"Image"))fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_set_render_opacity) - game::main_base()))(focus_image,1.f);
        void* animation=at<void*>(base,on?t::browser_layout::field_5b8:t::browser_layout::field_5b0);
        if(is(animation,"WidgetAnimation"))fn<void*(*)(void*,void*,float,s32,u32,float,bool)>((reinterpret_cast<uint64_t>(t::fn::ui_mod_list_play_animation) - game::main_base()))(base,animation,0,1,0,1,false);
    }
}
void hide_host(Host& h){if(void* o=resolve(h.root.token))if(h.shown)fn<void(*)(void*)>(t::data::browser_address_21b9940)(o);h.shown=false;}
void drop_host(Host& h){hide_host(h);for(auto& c:h.cards)if(void* image=resolve(c.image))unbind_image(image);
    if(void* image=resolve(h.avatar.image))unbind_image(image);
    forget_plain(resolve(h.root.token));unroot(h.root);h={};}
void show_host(Host& h){if(void* o=resolve(h.root.token))if(!h.shown){
    // The fourth original argument selects cache-only construction when true.
    // A fresh independent host must take the false viewport-attachment branch.
    fn<void(*)(void*,void*,s32,bool)>(t::data::browser_address_4c94fb0)(o,nullptr,500,false);
    // NativeConstruct initializes the footer to its stock Close prompt.
    // Reapply our copied prompt text after mounting, keeping its native slot.
    if(auto* footer=plain_for(resolve(h.footer)))footer->units=~u32(0);
    fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_set_render_opacity) - game::main_base()))(o,1.f);
    visible(o,0);visible(resolve(h.information),0);visible(resolve(h.panel),0);
    visible(at<void*>(o,t::browser_layout::information_back),1);visible(at<void*>(o,t::browser_layout::information_header),1);visible(at<void*>(o,t::browser_layout::information_pager),1);visible(resolve(h.pager),1);
    h.shown=true;
    if(void* header=resolve(h.header_widget))visible(header,0);
}}
struct TextBuffer {char16_t data[512]{};u32 count=0;
    void text(const char16_t* s){while(*s&&count<511)data[count++]=*s++;data[count]=0;}
    void utf8(const char* s){char16_t converted[256]{};utf16(s,converted);text(converted);}
    void number(u64 n){char16_t digits[24]{};u32 k=0;do{digits[k++]=char16_t('0'+n%10);n/=10;}while(n&&k<24);while(k&&count<511)data[count++]=digits[--k];data[count]=0;}
    // Whole kilobytes below a megabyte, then megabytes to one decimal.
    void size(u64 bytes){
        if(bytes<(1u<<20)){number((bytes+1023)/1024);text(u" KB");return;}
        const u64 tenths=bytes*10/(1u<<20);number(tenths/10);text(u".");number(tenths%10);text(u" MB");
    }
    void compact(u64 n){card_presentation::Text value;value.count(n);utf8(value.data);}
};
TextureToken stat_materials[6]{};
// UScrollBox::ScrollBarVisibility (ESlateVisibility), from the captured
// reflection data. A scroll box made here has no style, so its bar is a plain
// white strip; collapsing it before the Slate widget is built leaves it out.
#define kScrollBarVisibility t::browser_layout::scrollbar_visibility
// UWidget::RenderTransform.Angle, in degrees clockwise. The transform starts
// at +0x90 (translation, scale, shear, angle), as the literal text adapter
// already copies it. Written before the Slate widget is built.
#define kRenderAngle t::browser_layout::render_angle
// Glyphs from the game's icon sheets: 0 star, 1 speech bubble, 2 stopwatch,
// 3 profile, 4 check mark, 5 arrow (it points left on the sheet).
bool stock_stat_icon(void* image,u32 which){
    if(!is(image,"Image")||which>5)return false;
    static constexpr const char16_t* paths[]{
        u"/Game/Carrot4/UI/Common/Icon/UMG/MI_ui_Icon_System_Result.MI_ui_Icon_System_Result",
        u"/Game/Carrot4/UI/Common/Icon/UMG/MI_ui_Icon_RadarMap_L_000.MI_ui_Icon_RadarMap_L_000",
        u"/Game/Carrot4/UI/Common/Icon/UMG/MI_ui_Icon_Common_Basic.MI_ui_Icon_Common_Basic",
        u"/Game/Carrot4/UI/Common/Icon/UMG/MI_ui_Icon_Option.MI_ui_Icon_Option",
        u"/Game/Carrot4/UI/Common/Icon/UMG/MI_ui_Icon_NpcMessage.MI_ui_Icon_NpcMessage",
        u"/Game/Carrot4/UI/Common/Icon/UMG/MI_ui_Icon_Option.MI_ui_Icon_Option"};
    void* parent=resolve(stat_materials[which]);
    if(!parent){
        struct Soft {s32 index=-1,serial=0;u32 tag=0,pad=0;u64 asset=0,subpath=0;s32 num=0,max=0;} soft;
        u32 size=0;while(paths[which][size])++size;
        soft.asset=fn<u64(*)(const char16_t*,u64,u64)>((reinterpret_cast<uint64_t>(t::fn::name_from_text) - game::main_base()))(paths[which],size,1);
        parent=fn<void*(*)(Soft*)>((reinterpret_cast<uint64_t>(t::fn::load_soft_object) - game::main_base()))(&soft);
        if(!is(parent,"MaterialInterface")||is(parent,"MaterialInstanceDynamic"))return false;
        stat_materials[which]=token(parent);
    }
    fn<void(*)(void*,void*)>(t::data::browser_address_4c3a2e0)(image,parent);
    void* material=fn<void*(*)(void*)>(t::data::browser_address_4c3bb20)(image);
    if(!is(material,"MaterialInstanceDynamic"))return false;
    const u64 index=fn<u64(*)(const char16_t*,s32,u32)>((reinterpret_cast<uint64_t>(t::fn::name_from_text) - game::main_base()))(u"IconIndex",9,1);
    const float tiles[]{10.f,67.f,15.f,4.f,13.f,5.f};fn<void(*)(void*,u64,float)>(t::data::browser_address_5642e40)(material,index,tiles[which]);
    ++diagnostic.icon_materials;return true;
}
// A stat row: likes and downloads on the left, the date against the right
// edge, each an icon with its figure. The glyphs fill their atlas tiles
// differently (the star is the smallest, the stopwatch the largest), so each
// tile is sized to bring the glyphs to about the same height, and centered on
// the middle of the digits.
// gap: from the end of the likes figure to the downloads icon. text: from an
// icon's left edge to its figure. date: room for the date, set flush right.
struct StatRow {float gap,text,center,scale,font,date;};
constexpr StatRow kCardStat{14,26,12.5f,1.f,0.46f,96};
// The details card's lines: likes, downloads, comments, last update. Only
// figures the game has a fitting icon for (there is none for views or a
// release date).
constexpr u32 kInfoGlyph[kInfoRows]{0,5,1,2};
constexpr float kInfoPitch=58,kInfoText=52,kInfoFont=0.62f,kTaglineFont=0.58f;
constexpr u32 kStatGlyph[kStats]{0,5,2};
// The width a figure takes ("32", "1.3k"), from the game font's digit widths.
// The text block's own measure arrives a frame late; this one is exact enough
// to set the next icon a steady distance after the figure.
float stat_text_width(const char* value,const StatRow& row){
    float width=0;for(const char* p=value;*p;++p)width+=*p=='.'?5.f:*p=='1'?10.5f:12.5f;
    return width*row.font/0.46f;
}
// One figure of a stat row: places its icon at left and writes its text.
// Returns where the next figure's icon starts.
float stat_value(void* icon_slot,void* slot,void* cell,u32 n,const StatRow& row,float width,float left,const char* value){
    void* text=cell?at<void*>(cell,t::browser_layout::field_4e0):nullptr;const bool date=n+1==kStats;
    const float date_left=width-row.date-row.text;
    if(date)left=date_left;
    const float size=(n==0?30.f:n==1?22.f:21.f)*row.scale;
    fitted_slot(icon_slot,{left+(24*row.scale-size)*0.5f,row.center-size*0.5f,size,size});
    const float figure=stat_text_width(value,row),x=date?width-row.date:left+row.text;
    float w=date?row.date:n==0?figure+row.gap-2:date_left-x-4;if(w<8)w=8;
    const float h=32*row.scale;
    fitted_slot(slot,{x,0,w,h});label_utf8(text,value);
    fitted_text(text,s32(w),1,date?2:0);fitted_slot(text?at<void*>(text,t::browser_layout::widget_slot):nullptr,{0,0,w,h});
    if(auto* plain=plain_for(text)){plain->uniform_font=true;plain->fit_scale=1.f;}
    return x+figure+row.gap;
}
// The wrap width of a Carrot text block is measured before its font scale is
// applied. Given the box width itself, the description wrapped at 80% of it.
constexpr float kDescriptionScale=0.68f;
s32 description_wrap(){return s32((layout.details.description.width-20)/kDescriptionScale);}
bool card_has_picture(HostKind kind,u32 i){return kind==HostKind::Browse||i==0||i==1||(i>=4&&i<=7)||i==11;}
bool card_has_button(HostKind kind,u32 i){return kind==HostKind::Browse||i==0||i==2||i==3||(i>=8&&i<=10);}
// The stock button has its top-right and bottom-left corners cut at 45
// degrees, and a rectangular picture inside it pokes through the cut. The
// picture therefore sits in a clip that is turned 45 degrees, whose long
// edges run parallel to those cuts, and is itself turned back by the same
// angle. `area` is where the picture shows, in its parent's coordinates;
// top_right and bottom_left are how much of each corner to cut (0: none).
void clipped_picture(Card& c,const Rect& area,float top_right,float bottom_left){
    constexpr float diagonal=0.70710678f;
    const float along=(area.width+area.height)*diagonal,across=(area.width+area.height-top_right-bottom_left)*diagonal;
    // An uneven cut moves the middle of the clip off the middle of the picture.
    const float shift=(bottom_left-top_right)*0.25f;
    const float cx=area.left+area.width*0.5f+shift,cy=area.top+area.height*0.5f-shift;
    fitted_slot(c.clip_slot,{cx-along*0.5f,cy-across*0.5f,along,across});
    const float middle=across*0.5f-(top_right-bottom_left)*0.5f*diagonal;
    fitted_slot(c.picture_slot,{(along-area.width)*0.5f,middle-area.height*0.5f,area.width,area.height});
}
// How far the picture's corner is cut so the gap to the button's cut edge
// matches the gap along its straight edges.
constexpr float kCornerCut=14;
bool host_base(Host& h,void* owner,HostKind kind){
    diagnostic.ui_stage=1;++diagnostic.host_builds;
    void* widget=create_stock(0,owner);if(!widget||!root(h.root,widget))return false;
    h.kind=kind;const bool detail=kind!=HostKind::Browse;
    void* stock_header=at<void*>(widget,t::browser_layout::information_header),*footer=at<void*>(widget,t::browser_layout::information_footer);
    void* header=is(stock_header,"WBP_ui_PA_Header_Text_C")?fn<void*(*)(void*,void*,void*)>((reinterpret_cast<uint64_t>(t::fn::ui_create_widget) - game::main_base()))(widget,at<void*>(stock_header,t::browser_layout::object_class),nullptr):nullptr;
    visible(stock_header,1);
    if(is(header,"WBP_ui_PA_Header_Text_C")){h.header_widget=token(header);h.header=token(at<void*>(header,t::browser_layout::field_538));}
    if(is(footer,"WBP_ui_Cmn_Footer_C"))h.footer=token(at<void*>(footer,t::browser_layout::field_530));
    // Keep original background, header/footer and clipped native panel alive;
    // hide only this independent instance's fixed article and visible pager.
    visible(at<void*>(widget,t::browser_layout::information_back),1);visible(at<void*>(widget,t::browser_layout::information_pager),1);
    const char* names[]{"N_Cvs_eshop","N_Cvs_Scroll","N_Hrz_Pager_000","N_Cvs_Information"};void* found[4]{};
    children_named(widget,names,found,4);
    void* tree=at<void*>(widget,t::browser_layout::widget_tree),*panel=found[0];
    h.panel=token(panel);h.pager=token(found[2]);h.information=token(found[3]);
    visible(found[2],1);visible(found[1],1);
    void* scroll=construct("ScrollBox",tree);void* canvas=construct("CanvasPanel",tree);
    void* sizer=construct("SizeBox",tree);
    if(!is(panel,"CanvasPanel")||!is(scroll,"ScrollBox")||!is(canvas,"CanvasPanel")||!is(sizer,"SizeBox"))return false;
    at<u8>(scroll,kScrollBarVisibility)=1;
    if(is(header,"WBP_ui_PA_Header_Text_C")){
        // The reused native header gets our explicit canvas/layer bounds.
        void* header_slot=add(panel,header);
        if(!is(header_slot,"CanvasPanelSlot"))return false;
        fitted_slot(header_slot,bounds(layout.header));fn<void(*)(void*,s32)>(t::data::browser_address_4c1d980)(header_slot,20);
        void* header_tree=at<void*>(header,t::browser_layout::widget_tree);visible(header_tree?at<void*>(header_tree,t::browser_layout::widget_slot):nullptr,0);
        void* text=resolve(h.header);fitted_slot(text?at<void*>(text,t::browser_layout::widget_slot):nullptr,{0,0,layout.header.width,70});fitted_text(text,s32(layout.header.width),1);
        void* line=at<void*>(header,t::browser_layout::field_540);fitted_slot(line?at<void*>(line,t::browser_layout::widget_slot):nullptr,{0,78,layout.header.width,2});visible(line,0);
    }
    void* slot=add(panel,scroll);if(!slot||!add(scroll,sizer)||!add(sizer,canvas))return false;
    const auto viewport=detail?layout.details.viewport:layout.browse.viewport;h.view=bounds(viewport);
    h.scroll_slot=slot;fitted_slot(slot,h.view);h.scroll=token(scroll);h.canvas=token(canvas);h.sizer=token(sizer);
    fn<void(*)(void*,s32)>(t::data::browser_address_4c1d980)(slot,10);
    size_width(sizer,h.sizer_width,viewport.width);size_height(sizer,h.sizer_height,viewport.height);
    void* status=create_stock(4,widget);h.status_slot=status?add(panel,status):nullptr;
    if(!h.status_slot)return false;
    h.status=token(status);fitted_slot(h.status_slot,bounds(layout.status));
    fn<void(*)(void*,s32)>(t::data::browser_address_4c1d980)(h.status_slot,11);
    void* status_text=at<void*>(status,t::browser_layout::field_4e0);if(is(status_text,"CarrotRichTextBlock"))at<float>(status_text,t::browser_layout::text_scale)=0.65f;fitted_text(status_text,s32(layout.status.width),2);
    if(is(status_text,"CarrotRichTextBlock"))fitted_slot(at<void*>(status_text,t::browser_layout::widget_slot),{0,0,layout.status.width,layout.status.height});
    // One centered message for a page with nothing to list: an error, an empty
    // result. It sits over the scrolling area, which is otherwise blank then.
    void* notice=create_stock(4,widget);h.notice_slot=notice?add(panel,notice):nullptr;
    if(!h.notice_slot)return false;
    h.notice=token(notice);fn<void(*)(void*,s32)>(t::data::browser_address_4c1d980)(h.notice_slot,12);visible(notice,1);
    void* notice_text=at<void*>(notice,t::browser_layout::field_4e0);if(is(notice_text,"CarrotRichTextBlock"))at<float>(notice_text,t::browser_layout::text_scale)=0.8f;
    void* extent=construct("Spacer",tree);h.extent_slot=extent?add(canvas,extent):nullptr;
    if(!h.extent_slot)return false;
    visible(extent,3);return true;
}
bool host_card(Host& h,u32 i){
    void* widget=resolve(h.root.token),*canvas=resolve(h.canvas);if(!widget||!canvas)return false;
    const bool detail=h.kind!=HostKind::Browse;auto& c=h.cards[i];
    const bool with_picture=card_has_picture(h.kind,i),with_button=card_has_button(h.kind,i);
    void* picture=with_picture?create_stock(1,widget):nullptr;void* button=with_button?create_stock(3,widget):nullptr;
    if((with_picture&&!picture)||(with_button&&!button))return false;
    void* caption=nullptr,*card_canvas=nullptr;
    if(button){
        c.button_slot=add(canvas,button);if(!c.button_slot)return false;
        c.button=token(button);caption=at<void*>(button,t::browser_layout::gallery_caption);
        void* caption_slot=is(caption,"Widget")?at<void*>(caption,t::browser_layout::widget_slot):nullptr;
        card_canvas=is(caption_slot,"CanvasPanelSlot")?at<void*>(caption_slot,t::browser_layout::widget_slot):nullptr;
        if(!detail&&!is(card_canvas,"CanvasPanel"))return false;
    }
    void* parent=detail?canvas:card_canvas;
    void* tree=detail?at<void*>(widget,t::browser_layout::widget_tree):at<void*>(button,t::browser_layout::widget_tree);
    if(picture){
        // A card's picture and the mod page's screenshot are framed by a stock
        // button, so they get the turned clip (see clipped_picture).
        void* holder=parent;
        if(!detail||i==0){
            void* clip=construct("CanvasPanel",tree);if(!clip)return false;
            at<u8>(clip,t::browser_layout::clipping)=1;at<float>(clip,kRenderAngle)=45.f;at<float>(picture,kRenderAngle)=-45.f;
            c.clip_slot=add(parent,clip);if(!c.clip_slot)return false;
            holder=clip;
        }
        c.picture_slot=add(holder,picture);
        if(!detail){at<u8>(picture,t::browser_layout::clipping)=1; // qualified Widget ClipToBounds, before Slate creation
            void* picture_tree=at<void*>(picture,t::browser_layout::widget_tree),*picture_canvas=picture_tree?at<void*>(picture_tree,t::browser_layout::widget_slot):nullptr;
            if(is(picture_canvas,"Widget"))at<u8>(picture_canvas,t::browser_layout::clipping)=1;}
        void* image=at<void*>(picture,t::browser_layout::picture_image);if(!c.picture_slot||!is(image,"Image"))return false;
        c.picture=token(picture);c.image=token(image);
        void* loader=create_stock(6,widget);c.loader_slot=loader?add(parent,loader):nullptr;
        if(!c.loader_slot||!is(at<void*>(loader,t::browser_layout::field_538),"Image")||!is(at<void*>(loader,t::browser_layout::field_508),"WidgetAnimation"))return false;
        c.loader=token(loader);at<u8>(loader,t::browser_layout::clipping)=1;visible(loader,1);
        rect(at<void*>(image,t::browser_layout::widget_slot),{0,0,420,230});
    }
    if(!detail){
        void* title_canvas=construct("CanvasPanel",tree);
        c.title_slot=title_canvas?add(card_canvas,title_canvas):nullptr;
        if(!c.title_slot||!add(title_canvas,caption))return false;
        at<u8>(title_canvas,t::browser_layout::clipping)=1;c.title_canvas=token(title_canvas);
        void* metadata=construct("CanvasPanel",tree);c.metadata_slot=metadata?add(card_canvas,metadata):nullptr;
        if(!c.metadata_slot)return false;
        c.metadata=token(metadata);
        for(u32 n=0;n<kStats;++n){void* icon=construct("Image",tree),*cell=create_stock(4,button);
            // The sheet's arrow points left; a quarter turn makes it point down, for downloads.
            if(icon&&kStatGlyph[n]==5)at<float>(icon,kRenderAngle)=-90.f;
            void* icon_slot=icon?add(metadata,icon):nullptr;void* cell_slot=cell?add(metadata,cell):nullptr;
            if(!icon_slot||!cell_slot||!stock_stat_icon(icon,kStatGlyph[n]))return false;
            c.stat_icons[n]=token(icon);c.stat_text[n]=token(cell);c.stat_slots[n]=cell_slot;c.stat_icon_slots[n]=icon_slot;
            void* text=at<void*>(cell,t::browser_layout::field_4e0);if(is(text,"CarrotRichTextBlock")){at<float>(text,t::browser_layout::text_scale)=kCardStat.font;at<u8>(text,t::browser_layout::justification)=0;}
        }
    }
    if(picture){
        void* placeholder=create_stock(4,widget);c.placeholder_slot=placeholder?add(parent,placeholder):nullptr;
        if(!c.placeholder_slot)return false;
        c.placeholder=token(placeholder);visible(placeholder,1);
        void* placeholder_text=at<void*>(placeholder,t::browser_layout::field_4e0);if(is(placeholder_text,"CarrotRichTextBlock")){
            at<float>(placeholder_text,t::browser_layout::text_scale)=0.65f;fitted_text(placeholder_text,390,2);fitted_slot(at<void*>(placeholder_text,t::browser_layout::widget_slot),{0,0,390,70});}
    }
    if(button){
        // Retain the actual R026 stock font and its focus animation. TextWidth
        // is reflected storage, set before original Carrot SetText re-layout.
        fit_button(button,420,76);
        visible(at<void*>(button,t::browser_layout::field_5f0),1); // stock "new" badge belongs to game news
    }
    return true;
}
bool host_finish(Host& h){
    void* widget=resolve(h.root.token),*canvas=resolve(h.canvas),*panel=resolve(h.panel);
    void* tree=widget?at<void*>(widget,t::browser_layout::widget_tree):nullptr;if(!tree||!canvas||!panel)return false;
    if(h.kind==HostKind::Browse){
        void* sidebar=construct("CanvasPanel",tree);h.sidebar_slot=sidebar?add(panel,sidebar):nullptr;
        if(!h.sidebar_slot)return false;
        h.sidebar=token(sidebar);fn<void(*)(void*,s32)>(t::data::browser_address_4c1d980)(h.sidebar_slot,15);
        for(u32 i=0;i<layout.browse.sidebar.count;++i){void* button=create_stock(5,widget),*button_slot=button?add(sidebar,button):nullptr;
            if(!button_slot)return false;
            h.sidebar_buttons[i]=token(button);h.sidebar_slots[i]=token(button_slot);
            fitted_slot(button_slot,{0,float(i)*layout.browse.sidebar.item_pitch,layout.browse.sidebar.width,82});}
        // An author's page is this grid with the author's picture and details
        // in the left column, where the sections are on the other pages. They
        // stay put while the grid scrolls.
        void* picture=create_stock(1,widget),*info=create_stock(4,widget);
        h.avatar.picture_slot=picture?add(panel,picture):nullptr;h.profile_slot=info?add(panel,info):nullptr;
        void* image=picture?at<void*>(picture,t::browser_layout::picture_image):nullptr;
        if(!h.avatar.picture_slot||!h.profile_slot||!is(image,"Image"))return false;
        h.avatar.picture=token(picture);h.avatar.image=token(image);h.profile=token(info);
        visible(picture,1);visible(info,1);
        void* text=at<void*>(info,t::browser_layout::field_4e0);if(!is(text,"CarrotRichTextBlock"))return false;
        at<float>(text,t::browser_layout::text_scale)=kProfileScale;fitted_text(text,s32(layout.profile.info.width/kProfileScale),8);
        fitted_slot(at<void*>(text,t::browser_layout::widget_slot),{0,0,layout.profile.info.width,layout.profile.info.height});
        return true;
    }
    void* description=create_stock(4,widget),*metadata=construct("CanvasPanel",tree);
    if(!description||!metadata)return false;
    void* desc_scroll=construct("ScrollBox",tree),*desc_size=construct("SizeBox",tree),*pinned=construct("CanvasPanel",tree);
    void* pinned_slot=pinned?add(panel,pinned):nullptr;
    if(!pinned_slot)return false;
    h.pinned=token(pinned);
    fitted_slot(pinned_slot,{0,0,1920,1080});fn<void(*)(void*,s32)>(t::data::browser_address_4c1d980)(pinned_slot,15);visible(pinned,4);
    // Original AddChild reparents the stock widgets. Their WidgetTree and
    // rooted Information host retain ownership; only the scrolling parent
    // changes. The controls on the right stay put while the content scrolls;
    // the screenshot frame (card 0) stays in the content.
    const u32 fixed_cards[]{2,3,8,9,10};
    for(u32 i:fixed_cards){auto& c=h.cards[i];c.button_slot=add(pinned,resolve(c.button));if(!c.button_slot)return false;}
    if(!desc_scroll||!desc_size)return false;
    at<u8>(desc_scroll,kScrollBarVisibility)=1;
    if(!add(desc_scroll,desc_size)||!add(desc_size,description))return false;
    fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_size_box_width) - game::main_base()))(desc_size,layout.details.description.width);
    size_height(desc_size,h.description_height,2400);
    h.description_slot=add(canvas,desc_scroll);h.description_scroll=token(desc_scroll);h.description_sizer=token(desc_size);
    h.metadata_slot=add(pinned,metadata);
    if(!h.metadata_slot)return false;
    for(u32 n=0;n<2;++n){void* cell=create_stock(4,widget);h.strip_prompt_slots[n]=cell?add(canvas,cell):nullptr;
        if(!h.strip_prompt_slots[n])return false;
        h.strip_prompts[n]=token(cell);visible(cell,1);}
    at<u8>(metadata,t::browser_layout::clipping)=1;   // clip: the stock line is as wide as an Information page
    {void* cell=create_stock(4,widget);h.tagline_slot=cell?add(metadata,cell):nullptr;if(!h.tagline_slot)return false;
        h.tagline=token(cell);void* text=at<void*>(cell,t::browser_layout::field_4e0);if(is(text,"CarrotRichTextBlock")){at<float>(text,t::browser_layout::text_scale)=kTaglineFont;at<u8>(text,t::browser_layout::justification)=0;}}
    for(u32 n=0;n<kInfoRows;++n){void* icon=construct("Image",tree),*cell=create_stock(4,widget),*line=create_stock(7,widget);
        if(icon&&kInfoGlyph[n]==5)at<float>(icon,kRenderAngle)=-90.f;
        h.info_icon_slots[n]=icon?add(metadata,icon):nullptr;h.info_slots[n]=cell?add(metadata,cell):nullptr;h.info_line_slots[n]=line?add(metadata,line):nullptr;
        if(!h.info_icon_slots[n]||!h.info_slots[n]||!h.info_line_slots[n]||!stock_stat_icon(icon,kInfoGlyph[n]))return false;
        h.info_text[n]=token(cell);h.info_line[n]=token(line);
        void* text=at<void*>(cell,t::browser_layout::field_4e0);if(is(text,"CarrotRichTextBlock")){at<float>(text,t::browser_layout::text_scale)=kInfoFont;at<u8>(text,t::browser_layout::justification)=0;}
    }
    // The author's avatar is drawn over the author row, which was added before it.
    auto& avatar=h.cards[11];avatar.picture_slot=add(pinned,resolve(avatar.picture));
    if(!h.description_slot||!h.metadata_slot||!avatar.picture_slot)return false;
    for(u32 i=2;i<2+kFileRows;++i){auto& c=h.cards[i];void* button=resolve(c.button),*caption=button?at<void*>(button,t::browser_layout::gallery_caption):nullptr;
        void* caption_slot=caption?at<void*>(caption,t::browser_layout::widget_slot):nullptr,*card_canvas=caption_slot?at<void*>(caption_slot,t::browser_layout::widget_slot):nullptr;
        void* card_tree=button?at<void*>(button,t::browser_layout::widget_tree):nullptr,*clip=construct("CanvasPanel",card_tree),*size=create_stock(4,button);
        c.title_slot=clip?add(card_canvas,clip):nullptr;c.stat_slots[0]=size?add(card_canvas,size):nullptr;
        if(!c.title_slot||!c.stat_slots[0]||!add(clip,caption))return false;
        at<u8>(clip,t::browser_layout::clipping)=1;c.title_canvas=token(clip);c.stat_text[0]=token(size);
        // The mark on the file that Install will install.
        void* check=construct("Image",card_tree);c.stat_icon_slots[0]=check?add(card_canvas,check):nullptr;
        if(!c.stat_icon_slots[0]||!stock_stat_icon(check,4))return false;
        c.stat_icons[0]=token(check);visible(check,1);
    }
    h.description=token(description);h.metadata=token(metadata);
    // Publish wrapping before the original Slate rebuild as well as on
    // later refreshes; the descriptor never becomes a worker-owned widget.
    void* description_text=at<void*>(description,t::browser_layout::field_4e0);if(is(description_text,"CarrotRichTextBlock")){
        at<float>(description_text,t::browser_layout::text_scale)=kDescriptionScale;at<u8>(description_text,t::browser_layout::text_format)=0;fitted_text(description_text,description_wrap(),0);
        fitted_slot(at<void*>(description_text,t::browser_layout::widget_slot),{0,0,layout.details.description.width,2400});}
    return true;
}
// A host is about a hundred stock widgets. Built in one frame that is a
// visible freeze, so each call does one step: the frame, then one card per
// call, then the page's own parts. `finish` completes it now, for a page that
// is needed this frame. False when a step failed; the host is then dropped.
bool build_host(Host& h,void* owner,HostKind kind,bool finish){
    if(h.stage&&!resolve(h.root.token))drop_host(h);
    while(!h.built){
        const u64 begin=clock_ticks();
        const bool ok=h.stage==0?host_base(h,owner,kind):h.stage<=kCards?host_card(h,h.stage-1):host_finish(h);
        const u32 us=u32((clock_ticks()-begin)*10/192);
        diagnostic.host_us_total+=us;if(us>diagnostic.host_us_step_max)diagnostic.host_us_step_max=us;
        if(!ok){diagnostic.ui_stage=2;++diagnostic.host_failures;drop_host(h);return false;}
        if(++h.stage==kCards+2){h.built=true;diagnostic.ui_stage=3;diagnostic.host_cards=kCards;}
        if(!finish)break;
    }
    return true;
}
void notice(Host& h,const char16_t* text){
    void* widget=resolve(h.notice);visible(widget,text?3:1);if(!widget||!text)return;
    const Rect area{h.view.left,h.view.top+h.view.height*0.5f-80,h.view.width,160};
    fitted_slot(h.notice_slot,area);void* label_text=at<void*>(widget,t::browser_layout::field_4e0);
    fitted_text(label_text,s32(area.width),3,1);fitted_slot(label_text?at<void*>(label_text,t::browser_layout::widget_slot):nullptr,{0,0,area.width,area.height});
    label(label_text,text);
}
// A profile picture, or the stock profile glyph when there is none to show.
void avatar(Card& c,const Rect& area,u64 id,const char* url){
    void* image=resolve(c.image);rect(c.picture_slot,area);visible(resolve(c.picture),0);if(!image)return;
    const auto view=url[0]?controller().image(id,url,ImageKind::Thumbnail):ImageView{};
    if(view.state==ImageState::Ready){c.fallback=false;fit_image(image,view,area.width,area.height);bind_image(image,view.texture);visible(image,0);return;}
    unbind_image(image);
    if(!c.fallback)c.fallback=stock_stat_icon(image,3);
    if(c.fallback)fitted_slot(at<void*>(image,t::browser_layout::widget_slot),{area.width*0.15f,area.height*0.15f,area.width*0.7f,area.height*0.7f});
    visible(image,c.fallback?0:1);
}
template<u32 N> void append8(char (&out)[N],const char* text){u32 n=0;while(n<N-1&&out[n])++n;while(*text&&n<N-1)out[n++]=*text++;out[n]=0;}
// On an author's page the grid starts below the author's picture and details.
bool author_page(){return controller().screen()==Screen::Browse&&controller().query().author_id&&layout.profile.enabled;}
// Rows of cards start at the top of the grid on every page.
float browse_top(){return 0;}
// Cards given new content per frame. Writing a card's text is costly, and a
// whole page arriving at once was a visible hitch; the rest follow on the
// next frames and show their spinner until then.
constexpr u32 kCardsPerFrame=3;
void refresh_browse(){
    auto& h=browse_host;void* scroll=resolve(h.scroll);if(!scroll)return;
    hide_host(detail_host);show_host(h);
    const auto snapshot=controller().browse();
    const bool local=controller().screen()==Screen::Installed,author=author_page();
    const auto& query=controller().query();
    if(author||local)category_mode=false;
    if(author)sidebar_focus=false;
    // The line above the footer is only for something that needs saying:
    // an uninstall waiting for confirmation, or its result.
    char16_t status[512]{};u32 used=0;
    auto append=[&](const char16_t* s){while(*s&&used<511)status[used++]=*s++;status[used]=0;};
    if(snapshot.failed_sources)append(u"Some mod sources could not load. ZR retries. Results from other sources are shown.");
    if(local){const auto& library=controller().installed();const auto*r=controller().installed_record(snapshot.selected_id);
        if(!snapshot.loading&&snapshot.error==Error::None&&snapshot.count&&transfer_active())append(u"Wait for the current download or installation to finish before uninstalling.");
        if(controller().uninstall_armed()&&r){used=0;status[0]=0;append(u"Uninstall ");char16_t name[256]{};utf16(r->title,name);append(name);append(u" / ");utf16(r->filename,name);append(name);append(u"? A confirms. B cancels.");}
        else if(library.removed_file&&library.state==install::LibraryState::Ready){used=0;status[0]=0;append(u"Uninstalled. Restart the game to disable the mod.");}}
    void* status_widget=resolve(h.status);visible(status_widget,used?3:1);
    if(status_widget&&used){void* text=at<void*>(status_widget,t::browser_layout::field_4e0);fitted_text(text,s32(layout.status.width),1);label(text,status);}
    // The header says what is listed: the section's page, a search, a category, an author.
    char title[256]{};
    if(local)append8(title,"Installed mods");
    else if(author)append8(title,controller().author().name[0]?controller().author().name:layout.profile.title);
    else if(query.search[0]){append8(title,"Search: ");append8(title,query.search);}
    else {append8(title,layout.browse.title);
        if(query.category_id)for(u32 i=0;i<controller().categories().count;++i)if(controller().categories().records[i].id==query.category_id){
            append8(title,"  /  ");append8(title,controller().categories().records[i].name);}}
    char16_t footer[768]{};bool rich=true;
    if(category_mode)append16(footer,u"[PictureFont:IconData key=\"A_Button\" scale=\"0.76\"/] Choose    [PictureFont:IconData key=\"B_Button\" scale=\"0.76\"/] Cancel");
    else if(local)append16(footer,controller().uninstall_armed()?
        u"[PictureFont:IconData key=\"A_Button\" scale=\"0.76\"/] Confirm uninstall    [PictureFont:IconData key=\"B_Button\" scale=\"0.76\"/] Cancel":
        snapshot.count?u"[PictureFont:IconData key=\"A_Button\" scale=\"0.76\"/] Manage    [PictureFont:IconData key=\"B_Button\" scale=\"0.76\"/] Back    [PictureFont:IconData key=\"X_Button\" scale=\"0.76\"/] Refresh":
        u"[PictureFont:IconData key=\"B_Button\" scale=\"0.76\"/] Back    [PictureFont:IconData key=\"X_Button\" scale=\"0.76\"/] Refresh");
    else if(author)append16(footer,u"[PictureFont:IconData key=\"A_Button\" scale=\"0.76\"/] Details    [PictureFont:IconData key=\"B_Button\" scale=\"0.76\"/] Back    [PictureFont:IconData key=\"ZR_Button\" scale=\"0.76\"/] Refresh");
    else if(query.search[0])append16(footer,u"[PictureFont:IconData key=\"A_Button\" scale=\"0.76\"/] Details    [PictureFont:IconData key=\"B_Button\" scale=\"0.76\"/] Back    [PictureFont:IconData key=\"X_Button\" scale=\"0.76\"/] New search    [PictureFont:IconData key=\"ZR_Button\" scale=\"0.76\"/] Refresh");
    else rich=footer_text(layout.browse.footer,false,footer);
    chrome(h,title,footer,rich);
    // Nothing listed yet: while the first page loads, the grid shows empty
    // cards with spinners; afterwards a message says why it is empty.
    const bool skeleton=!snapshot.count&&snapshot.loading&&snapshot.error==Error::None;
    notice(h,snapshot.count||skeleton?nullptr:snapshot.error!=Error::None?
        (local?u"Could not read the installed mods.\nPress X to try again.":u"Could not load mods.\nCheck the connection, then press ZR to try again."):
        local?u"No mods installed yet.":query.search[0]?u"No mods match this search.":author?u"This author has no Pikmin 4 mods listed.":u"No mods here yet.");
    // An author's picture and details are in the left column.
    const float top=browse_top();
    controller().begin_images();
    void* profile=resolve(h.profile);visible(profile,author?3:1);
    if(author){const auto& a=controller().author();const auto& ar=layout.profile.avatar;const auto& ir=layout.profile.info;
        avatar(h.avatar,bounds(ar),(1ull<<32)|a.id,a.avatar_url);
        rect(h.profile_slot,bounds(ir));
        TextBuffer info;const auto state=controller().author_state();
        if(a.title[0]){info.utf8(a.title);info.text(u"\n");}
        if(a.has_joined){card_presentation::Text date;card_presentation::date(date,a.joined);info.text(u"Joined ");info.utf8(date.data);info.text(u"\n");}
        if(a.has_subscribers){info.compact(a.subscribers);info.text(a.subscribers==1?u" subscriber\n":u" subscribers\n");}
        if(snapshot.count&&!snapshot.has_after){info.number(snapshot.count);info.text(snapshot.count==1?u" mod":u" mods");}
        else if(state==State::Loading)info.text(u"Loading...");
        if(profile)label(at<void*>(profile,t::browser_layout::field_4e0),info.data);
    }else {visible(resolve(h.avatar.picture),1);if(void* image=resolve(h.avatar.image))unbind_image(image);}
    const u32 rows=(snapshot.count+kColumns-1)/kColumns;
    {float height=top+float(rows)*kPitchY;if(height<kViewHeight)height=kViewHeight;size_height(resolve(h.sizer),h.sizer_height,height);}
    rect(h.extent_slot,{0,top+float(rows)*kPitchY,1,1});
    float offset=fn<float(*)(void*)>(t::data::browser_address_4c58150)(scroll);if(!(offset>=0&&offset<100000))offset=0;
    if(h.anchor_revision!=snapshot.anchor_revision){
        for(u32 i=0;i<snapshot.count;++i){const Listing* item=controller().listing(i);if(item&&item->id==snapshot.anchor_id){
            // The first row at rest is the top of the page, profile included.
            offset=i<kColumns&&snapshot.row_offset==0?0:top+(i/kColumns+snapshot.row_offset)*kPitchY;break;}}
        fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_scroll_offset) - game::main_base()))(scroll,offset);h.anchor_revision=snapshot.anchor_revision;
    }
    h.offset=offset;const float into=offset>top?offset-top:0;
    const u32 first_row=u32(into/kPitchY),first=first_row*kColumns;
    u32 first_id=0,last_id=0,written=0;
    const auto& shape=layout.browse.card;
    const float grid_width=kColumns*shape.width+(kColumns-1)*shape.gap_x;
    const Rect image_area{12,12,shape.width-24,shape.image_height-12};
    const u32 ghosts=kColumns*3<kCards?kColumns*3:kCards;
    for(u32 i=0;i<kCards;++i){auto& c=h.cards[i];const u32 index=first+i;const Listing* item=index<snapshot.count?controller().listing(index):nullptr;
        void* picture=resolve(c.picture),*button=resolve(c.button),*image=resolve(c.image);
        void* placeholder=resolve(c.placeholder);
        const bool fresh=item&&(c.id!=item->id||c.generation!=snapshot.generation);
        // An empty card with a spinner: while the first page loads, and for a
        // card whose turn to be written has not come this frame.
        const bool ghost=item?fresh&&written>=kCardsPerFrame:skeleton&&i<ghosts;
        visible(picture,item||ghost?0:1);visible(button,item||ghost?0:1);
        if(!item&&!ghost){visible(placeholder,1);visible(resolve(c.metadata),1);loading_icon(c,{0,0,0,0},false);if(image)unbind_image(image);c.id=0;continue;}
        const float x=(h.view.width-grid_width)*0.5f+(index%kColumns)*kPitchX,y=top+(index/kColumns)*kPitchY;
        fitted_slot(c.button_slot,{x,y,shape.width,shape.height});
        clipped_picture(c,image_area,kCornerCut,0);
        // Original NativeConstruct restores BaseSize from stock854x82 data.
        // This runs after AddToScreen on the bounded visible pool, so its
        // actual nested canvas and images receive the final card geometry.
        fit_card(button,shape);
        fitted_slot(c.title_slot,{18,shape.image_height+4,shape.width-36,42});
        fitted_slot(c.metadata_slot,{18,shape.image_height+50,shape.width-36,32});
        fitted_slot(c.placeholder_slot,{18,shape.image_height*0.5f-35,shape.width-36,70});
        if(item){if(!first_id)first_id=item->id;last_id=item->id;}
        if(ghost){
            if(image)unbind_image(image);c.id=0;c.marquee_start=0;
            if(button)label_utf8(at<void*>(button,t::browser_layout::gallery_caption),"");
            visible(resolve(c.metadata),1);visible(image,1);visible(placeholder,1);focus(c,false);loading_icon(c,image_area,true);continue;
        }
        visible(resolve(c.metadata),0);
        const auto* package=local?controller().installed_record(item->id):nullptr;
        if(fresh){c.id=item->id;c.generation=snapshot.generation;c.revision=0;c.marquee_start=0;++written;
            if(button)label_utf8(at<void*>(button,t::browser_layout::gallery_caption),item->title);}
        // The download count arrives after the listing, under a new revision.
        if(c.revision!=snapshot.revision){c.revision=snapshot.revision;
            float stat_left=0;
            for(u32 n=0;n<kStats;++n){card_presentation::Text value;
                // GameBanana leaves a count out of a listing when it is zero.
                if(n==0)value.count(item->likes);
                else if(n==1){if(item->statistics&Downloads)value.count(item->downloads);else value.add("--");}
                else card_presentation::date(value,item->updated);
                stat_left=stat_value(c.stat_icon_slots[n],c.stat_slots[n],resolve(c.stat_text[n]),n,kCardStat,shape.width-36,stat_left,value.data);
            }}
        for(u32 n=0;n<kStats;++n){visible(resolve(c.stat_icons[n]),local?1:0);visible(resolve(c.stat_text[n]),local&&n?1:0);}
        if(package){void* cell=resolve(c.stat_text[0]),*text=cell?at<void*>(cell,t::browser_layout::field_4e0):nullptr;
            fitted_slot(c.stat_slots[0],{0,0,shape.width-36,32});fitted_text(text,s32(shape.width-36),1);fitted_slot(text?at<void*>(text,t::browser_layout::widget_slot):nullptr,{0,0,shape.width-36,32});label_utf8(text,package->filename);}
        if(button){auto* caption=plain_for(at<void*>(button,t::browser_layout::gallery_caption));if(caption){
            if(!caption->fixed){caption->uniform_font=true;caption->fit_scale=1.f;caption->fixed=true;
                ui_document_literal::set_auto_wrap(literal_backend(),resolve(caption->text),false);}
            const Size desired=fn<Size(*)(void*)>(t::data::browser_address_4c6f4f0)(resolve(caption->text));const float width=shape.width-36;
            float full=desired.width>width&&desired.width<10000?desired.width:width;
            const bool active=(!sidebar_focus&&item->id==snapshot.selected_id)||hovered_card==item->id;
            const u64 now=clock_ticks();if(!active)c.marquee_start=0;else if(!c.marquee_start)c.marquee_start=now;
            const float elapsed=c.marquee_start?float(now-c.marquee_start)/19200000.f:0;
            caption->position={-card_presentation::title_offset(elapsed,full-width,active),0,full,42};}}
        focus(c,!sidebar_focus&&item->id==snapshot.selected_id);
        // The card-sized copy where the listing has one; an installed package only kept the small one.
        const bool sharp=item->image.card[0]!=0;
        const auto view=controller().image(item->id,sharp?item->image.card:item->image.thumbnail,sharp?ImageKind::Card:ImageKind::Thumbnail);
        const bool pending=view.state==ImageState::Loading||view.state==ImageState::Waiting;
        visible(image,view.state==ImageState::Ready?0:1);visible(placeholder,view.state==ImageState::Ready||pending?1:0);
        loading_icon(c,image_area,pending);
        if(placeholder&&view.state!=ImageState::Ready&&!pending)label(at<void*>(placeholder,t::browser_layout::field_4e0),package?(package->enabled?u"Enabled":u"Disabled"):u"No preview");
        if(view.state==ImageState::Ready&&image){fit_image(image,view,image_area.width,image_area.height,shape.image_fit==ui::ImageFit::Cover);bind_image(image,view.texture);}
        else if(image)unbind_image(image);
        diagnostic.cards[i]={item->id,snapshot.generation,u32(view.state),view.size.width,view.size.height,0,
            view.texture.object,reinterpret_cast<u64>(image),image?at<u64>(image,t::browser_layout::font_or_brush_resource):0};
    }
    // The rest of the list, nearest rows first, so scrolling does not wait.
    auto ahead=[&](u32 n){if(const Listing* item=controller().listing(n)){const bool sharp=item->image.card[0]!=0;
        controller().prefetch_image(item->id,sharp?item->image.card:item->image.thumbnail,sharp?ImageKind::Card:ImageKind::Thumbnail);}};
    for(u32 n=first+kCards;n<snapshot.count;++n)ahead(n);
    for(u32 n=first;n-->0;)ahead(n);
    // The stock Spacer supplies a bounded resident-window extent independently
    // of the 12 recycled visual cards. Remote provider pages remain invisible.
    if(first_id)controller().viewport(first_id,last_id,into/kPitchY-first_row,snapshot.generation);
    if(!local&&first<=6&&snapshot.has_before&&!snapshot.loading)controller().demand(true,snapshot.generation);
    if(!local&&first+kCards+6>=snapshot.count&&snapshot.has_after&&!snapshot.loading)controller().demand(false,snapshot.generation);
    controller().pump_images();h.revision=snapshot.revision;
}
TextureToken title_widget{};
TSUCHI_Mod* framework=nullptr;
TSUCHI_Screen host_screen{},management_screen{},mod_settings_screen{};
bool suspended=false,resuming=false,return_to_browser=false,manage_requested=false;
uint32_t managed_local=0,page_requested=0;
ModManager installed_manager;
CheatMenu cheat_menu;
void open_installed_actions();
void restore_browser();
bool close_requested=false, keyboard_pending=false;
void close_browser(){close_requested=true;}
void drop_hosts(){drop_host(detail_host);drop_host(browse_host);prebuild_failed=false;}
void leave_title(){controller().close();drop_hosts();controller().release_images();for(auto& memo:class_memo)memo={};}
P4M_Result open_mod(u32 id,u32 generation){const auto r=controller().open_detail(id,generation);if(r==P4M_OK)detail_host.description_seen=false;return r;}
void title_tick(){
 void* widget=reinterpret_cast<void*>(ui_title_view());
 if(resolve(title_widget)!=widget){leave_title();title_widget=token(widget);}
 if(widget&&controller().screen()==Screen::Closed&&!prebuild_failed){
  if(!browse_host.built)prebuild_failed=!build_host(browse_host,widget,HostKind::Browse,false);
  else if(!detail_host.built)prebuild_failed=!build_host(detail_host,widget,HostKind::Detail,false);
 }
}
bool transfer_active(){
    using I=install::State;const auto state=controller().install_status().state;
    return controller().download_status().operation.state==State::Loading||controller().install_pending()||state==I::Queued||state==I::Validating||state==I::Extracting||state==I::Committing;
}
bool detail_file_installed(){
    const auto& d=controller().detail();
    return detail_file<d.file_count&&controller().file_current(d.files[detail_file]);
}
bool selected_zip(){const auto& d=controller().detail();if(detail_file>=d.file_count)return false;const char* name=d.files[detail_file].name;const auto n=strlen(name);return n>=4&&name[n-4]=='.'&&(name[n-3]=='z'||name[n-3]=='Z')&&(name[n-2]=='i'||name[n-2]=='I')&&(name[n-1]=='p'||name[n-1]=='P');}
detail_presentation::Order detail_order(){
    static_assert(max_files<=detail_presentation::max_files,"Detail navigation must include every provider file");
    const auto& d=controller().detail();const bool ready=controller().detail_state()==State::Ready;
    const float y[]{layout.details.files.rect.y,layout.details.download.rect.y,layout.details.cancel.rect.y,layout.details.author.rect.y};
    const bool retired=detail_file<d.file_count&&controller().file_retired(d.listing.id,d.files[detail_file].id);
    return detail_presentation::order(ready?d.file_count:0,ready&&layout.profile.enabled&&d.listing.author_id,ready&&d.file_count&&selected_zip()&&!transfer_active()&&!detail_file_installed()&&!retired,transfer_active(),y);
}
void scroll_detail(float delta){
    auto& h=detail_host;void* outer=resolve(h.scroll),*inner=resolve(h.description_scroll);if(!outer||!inner)return;
    const float viewport_height=layout.details.viewport.y+layout.details.viewport.height-layout.details.gallery.y;
    const float content_height=layout.details.description.y-layout.details.gallery.y+layout.details.description.height;
    auto at=detail_presentation::scroll({fn<float(*)(void*)>(t::data::browser_address_4c58150)(outer),fn<float(*)(void*)>(t::data::browser_address_4c58150)(inner)},
        content_height-viewport_height,h.description_extent-layout.details.description.height,delta);
    fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_scroll_offset) - game::main_base()))(outer,at.outer);fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_scroll_offset) - game::main_base()))(inner,at.inner);
}
[[maybe_unused]] bool detail_at_top(){void* outer=resolve(detail_host.scroll);return !outer||fn<float(*)(void*)>(t::data::browser_address_4c58150)(outer)<1.f;}
// The download or installation of the mod on screen failed in a way that
// trying again can fix (not an unsupported archive or an existing install).
bool transfer_retryable(){
    const auto& d=controller().detail();const auto& download=controller().download_status();const auto& s=controller().install_status();
    const bool terminal=s.error==install::Error::Unsupported||s.error==install::Error::Exists||s.error==install::Error::Limit||s.error==install::Error::Path||s.error==install::Error::Conflict;
    return controller().screen()==Screen::Detail&&controller().detail_state()==State::Ready&&
       download.submission_id==d.listing.id&&(download.operation.state==State::Failed||(!terminal&&s.state==install::State::Failed&&s.submission_id==d.listing.id));
}
void command_retry(){
    const auto& d=controller().detail();const auto& download=controller().download_status();
    if(transfer_retryable()){
        for(u32 i=0;i<d.file_count;++i)if(d.files[i].id==download.file_id){detail_file=i;controller().download(download.file_id);return;}
    }
    controller().retry();
}
const char16_t* transfer_text(bool different_submission){
    const auto& s=controller().install_status();using I=install::State;
    const auto& download=controller().download_status();
    // Unsupported archives have a manual-install result from the outset, but
    // must not say "Downloaded" while their transfer is still in progress.
    if(s.error==install::Error::Unsupported){
        if(download.operation.state==State::Loading)return u"Downloading...";
        if(download.operation.state==State::Failed)return different_submission?u"Download failed.":u"Download failed. Select Download and install to retry.";
        if(download.operation.state==State::Cancelled)return u"Download cancelled.";
    }
    switch(s.state){
    case I::Queued:return u"Checking archive...";
    case I::Validating:return u"Checking archive...";
    case I::Extracting:return u"Installing...";
    case I::Committing:return u"Finishing installation...";
    case I::Installed:if(s.message[0]){static char16_t message[642];utf16(s.message,message);return message;}return u"Installed.";
    case I::Failed:
        if(s.message[0]){static char16_t message[642];utf16(s.message,message);return message;}
        if(s.error==install::Error::Conflict)return u"This package conflicts with an installed download. Uninstall the conflicting package, then try again.";
        if(s.error==install::Error::Exists)return u"This file is already installed. Restart the game to enable it.";
        if(s.error==install::Error::Limit)return u"Archive exceeds installation limits. No mod was installed.";
        if(s.error==install::Error::Path)return u"Archive contains unsupported paths. No mod was installed.";
        return s.error==install::Error::Unsupported?u"This file is not a supported Tsuchi mod ZIP.":different_submission?u"Installation failed.":u"Installation failed. Select Download and install to retry.";
    case I::Cancelled:return u"Installation cancelled.";
    default:break;
    }
    switch(download.operation.state){
    case State::Loading:return u"Downloading...";
    case State::Ready:return controller().install_pending()?u"Checking archive...":u"Downloaded.";
    case State::Failed:return different_submission?u"Download failed.":u"Download failed. Select Download and install to retry.";
    case State::Cancelled:return u"Download cancelled.";
    default:return u"";
    }
}
// The line above the footer on a mod's page: empty unless a download or an
// installation is under way or has just ended.
bool transfer_progress(TextBuffer& out){
    const auto& install=controller().install_status();const auto& download=controller().download_status();
    const u32 submission=install.state!=install::State::Idle?install.submission_id:download.submission_id;
    const bool different=submission&&submission!=controller().detail().listing.id;
    const char16_t* text=transfer_text(different);if(!text[0]){const char* update=controller().update_status();if(!*update)return false;char16_t message[512]{};utf16(update,message);out.text(message);return true;}
    if(different)out.text(u"Another mod: ");
    out.text(text);
    if(download.operation.state==State::Loading){out.text(u"  ");out.size(download.operation.transfer.received);
        if(download.operation.transfer.total){out.text(u" of ");out.size(download.operation.transfer.total);}}
    if(install.state==install::State::Validating||install.state==install::State::Extracting||install.state==install::State::Committing){
        out.text(u"  ");out.number(install.completed_files);out.text(u" of ");out.number(install.entry_count);out.text(u" files");}
    return true;
}
void refresh_detail(){
    using detail_presentation::Pane;
    auto& h=detail_host;
    if(!h.built){void* owner=resolve(title_widget);if(!owner||!build_host(h,owner,HostKind::Detail,true))return;}
    if(!resolve(h.root.token))return;
    hide_host(browse_host);show_host(h);
    const auto& detail=controller().detail();const auto state=controller().detail_state();const bool ready=state==State::Ready;
    // The mod's name is the page title; the retry prompt is only shown while
    // something on the page has failed.
    char16_t footer[768]{};
    const bool rich=footer_text(layout.details.footer,state==State::Failed||controller().images_failed()||transfer_retryable(),footer);
    // The header is a trail: the mod's category, then its name.
    char trail[sizeof(detail.listing.category)+sizeof(detail.listing.title)+16];
    {u32 n=0;auto put=[&](const char* text){while(*text&&n+1<sizeof(trail))trail[n++]=*text++;};
        if(detail.listing.category[0]&&detail.listing.title[0]){put(detail.listing.category);put(kTrailSeparator);}
        put(detail.listing.title[0]?detail.listing.title:layout.details.title);trail[n]=0;}
    // The author sits at the right end of the header line; the title stops short of it.
    const bool author_shown=detail.listing.author[0]!=0&&layout.details.author.rect.width>0;
    chrome(h,trail,footer,rich,author_shown?layout.details.author.rect.x-layout.header.x-28:0);notice(h,nullptr);
    if(detail_file>=detail.file_count)detail_file=0;
    if(detail_preview>=detail.preview_count)detail_preview=0;
    void* description=resolve(h.description),*status=resolve(h.status);
    // L and R step through the screenshots; their prompts sit at the two ends
    // of the strip of small screenshots, which is only there for several.
    {const auto& strip=layout.details.preview_strip;const bool shown=state==State::Ready&&detail.preview_count>1;
        const float first=strip_left(detail.preview_count),span=strip_span(detail.preview_count),width=kStripPrompt;
        const Rect base=detail_content_bounds(strip.rect);
        for(u32 n=0;n<2;++n){void* cell=resolve(h.strip_prompts[n]);visible(cell,shown?3:1);if(!shown||!cell)continue;
            const float left=n?first+span+kStripPromptGap:first-kStripPromptGap-width;
            rect(h.strip_prompt_slots[n],{left,base.top+(base.height-36)*0.5f,width,36});
            void* text=at<void*>(cell,t::browser_layout::field_4e0);if(is(text,"CarrotRichTextBlock"))at<float>(text,t::browser_layout::text_scale)=kStripPromptScale;
            fitted_text(text,s32(width/kStripPromptScale),1,n?0:2);fitted_slot(text?at<void*>(text,t::browser_layout::widget_slot):nullptr,{0,0,width,36});
            label_mode(text,n?u"[PictureFont:IconData key=\"R_Button\" scale=\"0.76\"/] \u203A":u"\u2039 [PictureFont:IconData key=\"L_Button\" scale=\"0.76\"/]",true);}}
    // The details card: the author's tagline, then likes, downloads, comments
    // and the last update. The listing supplies most at once; downloads come
    // with the mod's own record.
    rect(h.metadata_slot,bounds(layout.details.metadata));
    {const auto& item=detail.listing;const float width=layout.details.metadata.width;float top=0;
        void* tagline=resolve(h.tagline);const bool said=state==State::Ready&&detail.tagline[0]!=0;
        visible(tagline,said?3:1);
        if(said&&tagline){rect(h.tagline_slot,{0,0,width,44});void* text=at<void*>(tagline,t::browser_layout::field_4e0);
            fitted_text(text,s32(width/kTaglineFont),1);fitted_slot(text?at<void*>(text,t::browser_layout::widget_slot):nullptr,{0,0,width,44});label_utf8(text,detail.tagline);
            if(auto* plain=plain_for(text))plain->fit_line=width;
            top=58;}
        for(u32 n=0;n<kInfoRows;++n,top+=kInfoPitch){
            TextBuffer line;card_presentation::Text date;
            if(n==0){line.compact(item.likes);line.text(item.source==0?(item.likes==1?u" star":u" stars"):(item.likes==1?u" like":u" likes"));}
            else if(n==1){if(item.statistics&Downloads){line.compact(item.downloads);line.text(item.downloads==1?u" download":u" downloads");}else line.text(u"Downloads unavailable");}
            else if(n==2){if(item.statistics&Comments){line.compact(item.comments);line.text(item.comments==1?u" comment":u" comments");}else line.text(u"Comments unavailable");}
            else {card_presentation::date(date,item.updated);line.text(u"Updated ");line.utf8(date.data);}
            const float size=(n==0?30.f:n==1?22.f:n==2?24.f:21.f)*1.35f;
            fitted_slot(h.info_icon_slots[n],{(40-size)*0.5f,top+(40-size)*0.5f,size,size});
            void* cell=resolve(h.info_text[n]),*text=cell?at<void*>(cell,t::browser_layout::field_4e0):nullptr;const float w=width-kInfoText;
            fitted_slot(h.info_slots[n],{kInfoText,top,w,40});label(text,line.data);
            fitted_text(text,s32(w/kInfoFont),1);fitted_slot(text?at<void*>(text,t::browser_layout::widget_slot):nullptr,{0,0,w,40});
            if(auto* plain=plain_for(text)){plain->uniform_font=true;plain->fit_scale=1.f;}
            fitted_slot(h.info_line_slots[n],{0,top+44,width,8});
        }
    }
    void* desc_text=description?at<void*>(description,t::browser_layout::field_4e0):nullptr,*status_text=status?at<void*>(status,t::browser_layout::field_4e0):nullptr;
    if(!h.description_seen||h.detail_id!=detail.listing.id||h.detail_state!=u32(state)){
        const bool changed=!h.description_seen||h.detail_id!=detail.listing.id;h.detail_id=detail.listing.id;h.detail_state=u32(state);h.description_seen=true;
        if(changed){detail_file=detail_file_first=detail_selection=detail_preview=detail_preview_first=0;h.description_extent=0;
            detail_pane=Pane::Actions;detail_focus_pending=true;
            if(void* scroll=resolve(h.scroll))fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_scroll_offset) - game::main_base()))(scroll,0.f);
            if(void* scroll=resolve(h.description_scroll))fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_scroll_offset) - game::main_base()))(scroll,0.f);}
        if(is(desc_text,"CarrotRichTextBlock")){fitted_text(desc_text,description_wrap(),0);at<u8>(desc_text,t::browser_layout::text_format)=0;
            fitted_slot(at<void*>(desc_text,t::browser_layout::widget_slot),{0,0,layout.details.description.width,2400});
            if(ready){
                char16_t text[4098]{},body[4098]{};utf16(layout.details.description_hint,text);utf16(detail.description,body);
                u32 n=0;while(text[n])++n;if(n&&n+2<4096){text[n++]=u'\n';text[n++]=u'\n';}
                u32 body_units=0;while(body[body_units])++body_units;
                if(detail.description_truncated||detail.files_truncated||detail.previews_truncated||body_units>4096-n){
                    const char16_t* notice=u"Some description, files or previews are omitted. See the original mod page for the complete listing.\n\n";
                    while(*notice&&n<4096)text[n++]=*notice++;}
                u32 b=0;while(body[b]&&n<4096)text[n++]=body[b++];
                if(!n){const char16_t* none=u"The author has not written a description.";while(*none)text[n++]=*none++;}
                if(n&&text[n-1]>=0xD800&&text[n-1]<=0xDBFF)--n;text[n]=0;
                label_mode(desc_text,text,detail.listing.source==1&&layout.details.description_mode==ui::TextMode::Rich);
            }else label(desc_text,state==State::Failed?u"Could not load this mod. Press X to try again.":u"Loading...");}}
    if(is(desc_text,"CarrotRichTextBlock")){
        // Original Widget GetDesiredSize returns its two floats in S0/S1, as
        // confirmed by the reflected wrapper's separate result stores. Wait
        // for a real Slate prepass and size the independent text viewport to
        // all wrapped lines, including unusually newline-heavy descriptions.
        const auto* plain=plain_for(desc_text);const Size desired=fn<Size(*)(void*)>(t::data::browser_address_4c6f4f0)(plain&&!plain->rich_mode?resolve(plain->text):desc_text);
        if(desired.height>0&&desired.height<200000){const float height=desired.height+40;h.description_extent=height;
            fitted_slot(at<void*>(desc_text,t::browser_layout::widget_slot),{0,0,layout.details.description.width,height});
            size_height(resolve(h.description_sizer),h.description_height,height);}
    }
    const auto& gallery=layout.details.gallery;
    h.view={gallery.x,gallery.y,gallery.width,layout.details.viewport.y+layout.details.viewport.height-gallery.y};
    fitted_slot(h.scroll_slot,h.view);
    const float detail_height=layout.details.description.y-gallery.y+layout.details.description.height;
    size_width(resolve(h.sizer),h.sizer_width,gallery.width);size_height(resolve(h.sizer),h.sizer_height,detail_height);
    rect(h.description_slot,detail_content_bounds(layout.details.description));fitted_slot(h.status_slot,bounds(layout.status));
    {TextBuffer progress;const bool shown=transfer_progress(progress);visible(status,shown?3:1);
        if(shown&&is(status_text,"CarrotRichTextBlock")){fitted_text(status_text,s32(layout.status.width),1);fitted_slot(at<void*>(status_text,t::browser_layout::widget_slot),{0,0,layout.status.width,layout.status.height});label(status_text,progress.data);}}
    rect(h.extent_slot,{0,detail_height,1,1});controller().begin_images();
    const auto order=detail_order();
    if(detail_focus_pending&&ready){const auto start=detail_presentation::initial(order,detail.file_count);
        detail_pane=start.pane;detail_selection=start.action;detail_focus_pending=false;}
    if(!order.count)detail_pane=Pane::Content;
    else if(!order.contains(detail_selection)){
        const u32 cancel=detail.file_count+1;
        detail_selection=transfer_active()&&order.contains(cancel)?cancel:order.contains(detail.file_count)?detail.file_count:order.move(detail_selection,0);
    }
    const u64 mod=detail.listing.id;const bool actions=detail_pane==Pane::Actions;
    bool screenshot_shown=false;
    // The rows show the file the focus is on, or failing that the chosen file.
    detail_file_first=detail_presentation::window(detail_file_first,actions&&detail_selection<detail.file_count?detail_selection:detail_file,detail.file_count,kFileRows);
    detail_preview_first=detail_presentation::window(detail_preview_first,detail_preview,detail.preview_count,layout.details.preview_strip.count);
    // Install sits directly under the last file row shown, and Cancel under it.
    const u32 file_rows=ready?(detail.file_count<kFileRows?detail.file_count:kFileRows):0;
    // The controls are stacked upward from the bottom of the page: Install,
    // the files above it, and Cancel above the files while a transfer runs.
    const float install_top=layout.details.viewport.y+layout.details.viewport.height-layout.details.download.rect.height;
    const float files_top=install_top-float(file_rows)*layout.details.files.pitch;
    const float cancel_top=files_top-layout.details.cancel.rect.height-12;
    for(u32 i=0;i<kCards;++i){auto& c=h.cards[i];void* picture=resolve(c.picture),*image=resolve(c.image),*button=resolve(c.button),*placeholder=resolve(c.placeholder);
        const int strip_slot=i==1?0:i>=4&&i<=7?int(i)-3:-1;
        if(i==0){
            // The screenshot, inside a stock frame that lights up while this
            // side of the page has the focus. Inset: the scroll box clips at
            // its edge, and the focus animation grows the frame a little.
            const Rect frame{6,6,gallery.width-12,gallery.height-12},area{18,18,gallery.width-36,gallery.height-36};
            rect(c.button_slot,frame);fit_button(button,frame.width,frame.height);visible(button,0);
            if(button)label_utf8(at<void*>(button,t::browser_layout::gallery_caption),"");
            focus(c,!actions);clipped_picture(c,area,kCornerCut,kCornerCut);visible(picture,0);
            // The listing already has the first screenshot, so it is shown
            // before the mod's full record arrives. A small copy that is
            // already loaded stands in until the screenshot itself is ready.
            const Preview& shown=ready&&detail.preview_count?detail.previews[detail_preview]:detail.listing.image;
            // Decoding a full-size screenshot stalls a frame (see upload()), so
            // it is only asked for once this screenshot has been on show for a
            // moment. Moving through the screenshots or opening a page stays
            // smooth, and the stall falls where nothing is moving.
            const u64 now=clock_ticks(),key=(u64(mod)<<8)^(ready?detail_preview+1:0);
            if(key!=detail_shown_key){detail_shown_key=key;detail_shown_since=now;}
            ImageView full=controller().cached_image(shown.url,ImageKind::Preview),small{};
            const bool at_rest=now-detail_shown_since>19200000ull*45/100;
            if(shown.url[0]&&(at_rest||full.state!=ImageState::Missing))full=controller().image(mod,shown.url,ImageKind::Preview);
            if(full.state!=ImageState::Ready){
                if(shown.card[0])small=controller().image(mod,shown.card,ImageKind::Card);
                if(small.state!=ImageState::Ready&&shown.tile[0])small=controller().image(mod,shown.tile,ImageKind::Thumbnail);
            }
            const ImageView& view=full.state==ImageState::Ready?full:small;
            const bool pending=shown.url[0]?full.state!=ImageState::Ready&&full.state!=ImageState::Failed:!ready&&state!=State::Failed;
            if(view.state==ImageState::Ready&&image){fit_image(image,view,area.width,area.height);bind_image(image,view.texture);visible(image,0);}
            else {if(image)unbind_image(image);visible(image,1);}
            screenshot_shown=full.state==ImageState::Ready&&at_rest;
            loading_icon(c,area,pending);
            const bool none=!pending&&view.state!=ImageState::Ready;visible(placeholder,none?3:1);
            if(none&&placeholder){rect(c.placeholder_slot,{area.left+15,area.top+area.height/2-35,area.width-30,70});
                void* text=at<void*>(placeholder,t::browser_layout::field_4e0);fitted_text(text,s32(area.width-30),2,1);fitted_slot(text?at<void*>(text,t::browser_layout::widget_slot):nullptr,{0,0,area.width-30,70});
                label(text,full.state==ImageState::Failed?u"This screenshot could not be loaded.":u"No screenshots");}
        }else if(strip_slot>=0){
            // Small copies of every screenshot; the strip is hidden for a single one.
            const auto& strip=layout.details.preview_strip;const u32 n=detail_preview_first+u32(strip_slot);
            const bool exists=ready&&detail.preview_count>1&&n<detail.preview_count&&u32(strip_slot)<strip.count;
            visible(picture,exists?0:1);visible(placeholder,1);
            if(!exists){loading_icon(c,{0,0,0,0},false);if(image)unbind_image(image);continue;}
            const float w=strip_width();
            Rect r=detail_content_bounds(strip.rect);r.left=strip_left(detail.preview_count)+u32(strip_slot)*(w+strip.gap);r.width=w;
            rect(c.picture_slot,r);
            const auto& p=detail.previews[n];const auto view=controller().image(mod,p.tile,ImageKind::Thumbnail);
            visible(image,view.state==ImageState::Ready?0:1);
            if(image)fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_set_render_opacity) - game::main_base()))(image,n==detail_preview?1.f:0.45f);
            loading_icon(c,r,view.state==ImageState::Loading||view.state==ImageState::Waiting);
            if(view.state==ImageState::Ready&&image){fit_image(image,view,r.width,r.height);bind_image(image,view.texture);}
            else if(image)unbind_image(image);
        }else if(i>=2&&i<2+kFileRows){const u32 index=detail_file_first+i-2;const bool exists=ready&&index<detail.file_count;
            Rect r=bounds(layout.details.files.rect);r.top=files_top+(i-2)*layout.details.files.pitch;
            rect(c.button_slot,r);visible(button,exists?0:1);visible(resolve(c.stat_text[0]),exists?0:1);
            // The check mark is the file Install will install. It moves when a
            // file is chosen with A, not when the focus passes over a row.
            // With one file there is nothing to choose, so it is not shown.
            const bool chosen=exists&&detail.file_count>1&&index==detail_file;
            void* check=resolve(c.stat_icons[0]);visible(check,chosen?3:1);
            fitted_slot(c.stat_icon_slots[0],{r.width-66,(r.height-52)*0.5f,52,52});
            if(button&&exists){const float width=r.width-36-(detail.file_count>1?56:0);void* caption=at<void*>(button,t::browser_layout::gallery_caption);
                const Rect line{0,0,width,32};fit_button(button,r.width,r.height,&line,0.55f,1,0);
                fitted_slot(c.title_slot,{18,8,width,32});label_utf8(caption,detail.files[index].name);
                if(c.id!=detail.files[index].id){c.id=detail.files[index].id;c.marquee_start=0;}
                if(auto* plain=plain_for(caption)){
                    if(!plain->fixed){plain->uniform_font=true;plain->fit_scale=1.f;plain->fixed=true;
                        ui_document_literal::set_auto_wrap(literal_backend(),resolve(plain->text),false);}
                    const Size desired=fn<Size(*)(void*)>(t::data::browser_address_4c6f4f0)(resolve(plain->text));const float full=desired.width>width&&desired.width<10000?desired.width:width;
                    const bool active=actions&&index==detail_selection;const u64 now=clock_ticks();if(!active)c.marquee_start=0;else if(!c.marquee_start)c.marquee_start=now;
                    const float elapsed=c.marquee_start?float(now-c.marquee_start)/19200000.f:0;
                    plain->position={-card_presentation::title_offset(elapsed,full-width,active),0,full,32};}
                void* size=resolve(c.stat_text[0]),*size_text=size?at<void*>(size,t::browser_layout::field_4e0):nullptr;
                fitted_slot(c.stat_slots[0],{18,44,width,28});if(size_text){at<float>(size_text,t::browser_layout::text_scale)=0.46f;fitted_text(size_text,s32(width),1);fitted_slot(at<void*>(size_text,t::browser_layout::widget_slot),{0,0,width,28});}
                TextBuffer text;text.size(detail.files[index].size);
                if(detail.files[index].version[0]){text.text(u"  /  ");text.utf8(detail.files[index].version);}
                if(controller().file_current(detail.files[index]))text.text(u"  /  Installed");
                else if(controller().file_retired(detail.listing.id,detail.files[index].id))text.text(u"  /  Removal pending");
                if(detail.file_count>kFileRows){text.text(u"  /  File ");text.number(index+1);text.text(u" of ");text.number(detail.file_count);}
                label(size_text,text.data);
                if(auto* plain=plain_for(size_text)){plain->uniform_font=true;plain->fit_scale=1.f;}
            }focus(c,actions&&exists&&index==detail_selection);
        }else if(i==8||i==9){const auto& action=i==8?layout.details.download:layout.details.cancel;Rect r=bounds(action.rect);
            r.top=i==8?install_top:cancel_top;
            const bool shown=i==8?ready&&detail.file_count:transfer_active(),enabled=i==8?order.contains(detail.file_count):shown;
            rect(c.button_slot,r);fit_button(button,r.width,r.height);visible(button,shown?0:1);
            if(button){const bool retired=detail_file<detail.file_count&&controller().file_retired(detail.listing.id,detail.files[detail_file].id);
                const bool zip=selected_zip();
                // One file needs no "selected": the button just installs it.
                const bool several=detail.file_count>1;
                label_utf8(at<void*>(button,t::browser_layout::gallery_caption),i==8&&retired?"Restart required":i==8&&detail_file_installed()?"Installed":
                    i==8&&!zip?"ZIP required":i==8&&controller().source_installed(detail.listing.id)?"Update / change version":i==8&&!several?"Install":action.label);
                fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_set_render_opacity) - game::main_base()))(button,enabled?1.f:0.55f);}
            focus(c,actions&&enabled&&detail_selection==detail.file_count+(i==9));
        }else if(i==10){
            // The author: picture and name, selected like any other control.
            const auto r=bounds(layout.details.author.rect);const bool shown=detail.listing.author[0]!=0;
            rect(c.button_slot,r);visible(button,shown?0:1);
            const Rect name{r.height+4,6,r.width-r.height-22,r.height-12};fit_button(button,r.width,r.height,&name,0.72f,1,0);
            if(button&&shown)label_utf8(at<void*>(button,t::browser_layout::gallery_caption),detail.listing.author);
            focus(c,actions&&order.contains(detail.file_count+2)&&detail_selection==detail.file_count+2);
        }else if(i==11){const auto r=bounds(layout.details.author.rect);const float size=r.height-20;
            if(detail.listing.author[0])avatar(c,{r.left+14,r.top+10,size,size},(1ull<<32)|detail.listing.author_id,detail.listing.author_avatar_url);
            else {visible(picture,1);if(image)unbind_image(image);}
        }
    }
    // The next screenshot and the rest of the strip, after what is on screen.
    // The next full-size screenshot only once this one is up and the page is at rest.
    if(ready){if(screenshot_shown&&detail_preview+1<detail.preview_count)controller().prefetch_image(mod,detail.previews[detail_preview+1].url,ImageKind::Preview);
        if(detail.preview_count>1)for(u32 n=0;n<detail.preview_count;++n)controller().prefetch_image(mod,detail.previews[n].tile,ImageKind::Thumbnail);}
    controller().pump_images();
}
void command_search(){
 if(keyboard_pending)return;
 keyboard_pending=tsuchi::ui::table.ask_text(framework,"Search mods",controller().query().search,100,TSUCHI_TEXT_KIND_TEXT)==TSUCHI_OK;
}
void apply_category(){auto q=controller().query();q.category_id=category_id(category_cursor);category_mode=false;controller().change_query(q);}
// An author's page: their profile with their mods below, in the browse host.
void open_author_page(){
    const auto& d=controller().detail();if(!layout.profile.enabled||!d.listing.author_id)return;
    if(controller().open_author(d.listing.author_id,controller().browse().generation)==P4M_OK){sidebar_focus=false;category_mode=false;}
}
void command_confirm(){
    const auto screen=controller().screen();
    if(screen==Screen::Installed){if(sidebar_focus){if(layout.browse.sidebar.items[sidebar_selected].action==ui::SidebarAction::Search)command_search();else sidebar_focus=false;}
        else manage_requested=true;return;}
    if(screen==Screen::Browse){
        if(sidebar_focus){
            if(category_mode)apply_category();
            else if(layout.browse.sidebar.items[sidebar_selected].action==ui::SidebarAction::Search)command_search();
            else sidebar_focus=false;
            return;}
        const auto s=controller().browse();if(s.error!=Error::None)controller().retry();else open_mod(s.selected_id,s.generation);}
    else if(screen==Screen::Detail&&controller().detail_state()==State::Ready){const auto& d=controller().detail();
        // Nothing on the content side is activated; A acts on the controls.
        if(detail_pane!=detail_presentation::Pane::Actions)return;
        if(detail_selection==d.file_count&&detail_file<d.file_count&&detail_order().contains(detail_selection))controller().download(d.files[detail_file].id);
        else if(detail_selection==d.file_count+1&&transfer_active())controller().cancel_download();
        else if(detail_selection==d.file_count+2)open_author_page();
        else if(detail_selection<d.file_count)detail_file=detail_selection;}
}
void command_back(){suppress_back=true;const auto screen=controller().screen();
    if(category_mode){category_mode=false;return;}
    if(screen==Screen::Installed&&controller().cancel_uninstall())return;
    if(screen==Screen::Detail||(screen==Screen::Browse&&controller().query().author_id)){
        controller().back();if(controller().screen()==Screen::Closed)close_browser();
    }else if(!sidebar_focus)sidebar_focus=true;else close_browser();}
void command_sort(){if(controller().screen()==Screen::Installed||controller().screen()==Screen::Detail||controller().browse().error!=Error::None)command_retry();
    else{auto q=controller().query();if(q.search[0]){command_search();return;}if(q.author_id||category_mode)return;q.sort=Sort((u32(q.sort)+1)%3);
        // The sections on the left are the sorts; keep the highlighted one in step.
        if(controller().change_query(q)==P4M_OK)for(u32 i=0;i<layout.browse.sidebar.count;++i){const auto& item=layout.browse.sidebar.items[i];
            if(item.action==ui::SidebarAction::Query&&item.sort==q.sort){sidebar_selected=i;break;}}}}
// Y lists the categories where the sections are; A picks one, B leaves it as it was.
void command_category(){if(controller().screen()!=Screen::Browse)return;const auto& q=controller().query();
    if(q.search[0]){command_search();return;}if(q.author_id)return;
    category_mode=!category_mode;if(!category_mode)return;
    sidebar_focus=true;category_cursor=0;
    for(u32 entry=1;entry<category_entries();++entry)if(category_id(entry)==q.category_id)category_cursor=entry;
}
u64 input_buttons(){
 u64 held=0;
 const u32 pads[]={0,uint32_t(t::constant::pad_handheld)};
 for(u32 pad:pads){alignas(8) u8 state[128]{};if(!presenter_sample_input(pad,state))continue;
  held|=at<u64>(state,t::npad_state::buttons);
  const s32 x=at<s32>(state,t::input_state::sticks),y=at<s32>(state,t::input_state::sticks+4);
  if(x < -16000)held|=1ull<<16;if(x>16000)held|=1ull<<18;
  if(y>16000)held|=1ull<<17;if(y < -16000)held|=1ull<<19;
 }
 return held;
}
void ui_input(){
    if(keyboard_pending)return;
    const u64 held=input_buttons();u64 edges=held&~ui_held;ui_held=held;
    if(!(held&2))suppress_back=false;
    static u64 repeated=0,next_repeat=0;static Screen repeat_screen=Screen::Closed;
    const auto screen=controller().screen();if(screen==Screen::Closed){repeated=next_repeat=0;repeat_screen=screen;return;}
    constexpr u64 directions=0xFFull<<12;
    const u64 now=clock_ticks();
    const u64 active=held&directions;
    if(repeat_screen!=screen||active!=repeated){repeated=active;next_repeat=now+5760000;repeat_screen=screen;}
    else if(active&&now>=next_repeat){edges|=active;next_repeat=now+1920000;}
    if(edges&2){command_back();return;}if(edges&1)command_confirm();
    if(edges&(1ull<<2))command_sort();if(edges&(1ull<<3))command_category();if(edges&(1ull<<9))command_retry();
    const bool left=edges&((1ull<<12)|(1ull<<16)),up=edges&((1ull<<13)|(1ull<<17));
    const bool right=edges&((1ull<<14)|(1ull<<18)),down=edges&((1ull<<15)|(1ull<<19));
    if(controller().screen()==Screen::Installed&&(left||right||up||down)&&controller().uninstall_armed())controller().cancel_uninstall();
    if(controller().screen()==Screen::Detail){const auto& d=controller().detail();const auto order=detail_order();
        // Left and right cross between the two sides of the page; see
        // detail_presentation::move. Moving the focus over a file does not
        // choose it; A does.
        using detail_presentation::Move;detail_presentation::Focus at{detail_pane,detail_selection,detail_preview};
        auto go=[&](Move direction){const auto step=detail_presentation::move(at,direction,order,d.preview_count);
            at=step.focus;if(step.scroll)scroll_detail(float(step.scroll)*140.f);};
        if(left)go(Move::Left);if(right)go(Move::Right);if(up)go(Move::Up);if(down)go(Move::Down);
        // L and R: the previous and the next screenshot.
        if(edges&(1ull<<6))go(Move::Previous);if(edges&(1ull<<7))go(Move::Next);
        detail_pane=at.pane;detail_selection=at.action;detail_preview=at.preview;
        return;}
    if(sidebar_focus){
        if(category_mode){if(up&&category_cursor)--category_cursor;if(down&&category_cursor+1<category_entries())++category_cursor;return;}
        if(up&&sidebar_selected){--sidebar_selected;sidebar_query();}if(down&&sidebar_selected+1<layout.browse.sidebar.count){++sidebar_selected;sidebar_query();}if(right)sidebar_focus=false;return;}
    // An author's page has no sections beside it to go back to.
    const bool author=author_page();
    const auto s=controller().browse();s32 index=-1;for(u32 i=0;i<s.count;++i){const Listing* l=controller().listing(i);if(l&&l->id==s.selected_id){index=s32(i);break;}}
    if(index<0){if(left&&!author)sidebar_focus=true;return;}if(left&&u32(index)%kColumns==0){if(!author)sidebar_focus=true;return;}s32 next=index+(left?-1:right?1:up?-s32(kColumns):down?s32(kColumns):0);
    if(next<0){if(s.has_before)controller().demand(true,s.generation);next=index;}
    if(next>=s32(s.count)){if(s.has_after)controller().demand(false,s.generation);
        // Preserve loaded-boundary focus while awaiting more records, but a
        // shorter final row must remain reachable from every column above it.
        next=down&&u32(index)/kColumns<(s.count-1)/kColumns?s32(s.count)-1:index;}
    if(next!=index){const Listing* l=controller().listing(u32(next));if(l)controller().select(l->id,s.generation);
        // The first row is the top of the page, which on an author's page shows the profile above it.
        const u32 row=u32(next)/kColumns;float offset=browse_host.offset;const float y=browse_top()+row*kPitchY;
        if(!row)offset=0;else if(y<offset)offset=y;else if(y+kPitchY>offset+kViewHeight)offset=y+kPitchY-kViewHeight;
        if(void* scroll=resolve(browse_host.scroll))fn<void(*)(void*,float)>((reinterpret_cast<uint64_t>(t::fn::ui_scroll_offset) - game::main_base()))(scroll,offset);}
}
struct SharedWrapper {void* object=nullptr;void* control=nullptr;};
// Nontrivial shared-pointer returns use AAPCS64's indirect result register X8.
// Do not declare the original factory as a trivial two-word C++ return value.
__attribute__((naked)) void create_wrapper(SharedWrapper*,void*,u32,void*){
    __asm__("mov x8,x0\nmov x0,x1\nmov w1,w2\nbr x3");
}
struct Decode {
    struct Raw {const u8* data=nullptr;u64 num=0,max=0;} raw;
    // Original SP+8/+16/+24 storage, virtual+0x20 X3=SP+8 and later FMalloc
    // free at 05524668..688 prove caller-owned TArray64, not wrapper-owned data.
    static_assert(sizeof(Raw)==24);
    SharedWrapper wrapper;
    ~Decode(){
        if(raw.data){
            void* allocator=global<void*>(t::data::allocator);
            if(allocator)method<void(*)(void*,const void*)>(allocator,t::browser_layout::virtual_38)(allocator,raw.data);
        }
        void* owner=wrapper.control;
        if(owner&&__atomic_fetch_sub(&at<u32>(owner,t::shared_count::holders),1,__ATOMIC_ACQ_REL)==1){
            method<void(*)(void*)>(owner,t::shared_count_table::destroy_object)(owner);
            if(__atomic_fetch_sub(&at<u32>(owner,t::shared_count::watchers),1,__ATOMIC_ACQ_REL)==1)method<void(*)(void*)>(owner,t::shared_count_table::destroy_self)(owner);
        }
    }
    // The game's ImageWrapper module. Game thread only; the module stays loaded.
    static void* module(){
        static void* found=nullptr;if(found)return found;
        // Let the audited original initialize FModuleManager using a buffer
        // that DetectImageFormat rejects before any decoder or texture call.
        if(!(global<u8>(t::data::browser_address_9830b10)&1)){
            const u8 invalid=0;struct Bytes {const u8* data;s32 num,max;} empty{&invalid,1,1};
            fn<void*(*)(const Bytes*)>(t::data::browser_address_5524420)(&empty);
        }
        const u64 name=fn<u64(*)(const char16_t*,s32,u32)>((reinterpret_cast<uint64_t>(t::fn::name_from_text) - game::main_base()))(u"ImageWrapper",12,1);
        found=fn<void*(*)(void*,u64)>(t::data::browser_address_3730f00)(ptr(game::main_base()+t::data::browser_address_9830740),name);
        return found;
    }
    bool run(void* module,const u8* data,u32 count,ImageSize size){
        if(!module)return false;
        const u32 format=method<u32(*)(void*,const u8*,u64)>(module,t::browser_layout::virtual_50)(module,data,count);
        if((format&255)==255)return false;
        create_wrapper(&wrapper,module,format,method<void*>(module,t::browser_layout::virtual_48));
        void* decoder=wrapper.object;if(!decoder||!wrapper.control)return false;
        if(!method<bool(*)(void*,const void*,u64)>(decoder,t::browser_layout::decoder_set_compressed)(decoder,data,count))return false;
        if(method<s32(*)(void*)>(decoder,t::browser_layout::virtual_38)(decoder)!=8||
           method<s32(*)(void*)>(decoder,t::browser_layout::virtual_28)(decoder)!=s32(size.width)||
           method<s32(*)(void*)>(decoder,t::browser_layout::virtual_30)(decoder)!=s32(size.height))return false;
        if(!method<bool(*)(void*,u32,s32,Raw*)>(decoder,t::browser_layout::virtual_20)(decoder,1,8,&raw))return false;
        return raw.data&&raw.num==u64(size.width)*size.height*4&&raw.max>=raw.num;
    }
};
// Decodes into `pixels` at the size the image is kept at: a large screenshot
// is decoded whole and kept at a whole fraction of its size, a card picture is
// resampled to the card's width. `pixels` holds stored.width*stored.height*4.
bool decode_into(u8* pixels,void* module,const u8* data,u32 count,ImageSize size,ImageSize stored,u32 factor){
    Decode decoded;if(!decoded.run(module,data,count,size))return false;
    if(!factor||!stored.width||!stored.height||stored.width>size.width||stored.height>size.height)return false;
    const bool whole=u64(stored.width)*factor<=size.width&&u64(stored.height)*factor<=size.height&&(factor>1||(stored.width==size.width&&stored.height==size.height));
    const u64 bytes=u64(stored.width)*stored.height*4;const u8* source=decoded.raw.data;
    if(!whole){
        // Not a whole fraction (a card picture brought to the card's width):
        // each kept pixel blends the four decoded pixels around its center.
        const u64 step_x=(u64(size.width)<<16)/stored.width,step_y=(u64(size.height)<<16)/stored.height;
        for(u32 y=0;y<stored.height;++y){
            u64 fy=y*step_y+step_y/2;fy=fy>32768?fy-32768:0;
            u32 y0=u32(fy>>16),wy=u32(fy&65535)>>8;if(y0>=size.height-1){y0=size.height-1;wy=0;}
            const u8* row0=source+u64(y0)*size.width*4,*row1=row0+(wy?u64(size.width)*4:0);
            u8* out=pixels+u64(y)*stored.width*4;
            for(u32 x=0;x<stored.width;++x,out+=4){
                u64 fx=x*step_x+step_x/2;fx=fx>32768?fx-32768:0;
                u32 x0=u32(fx>>16),wx=u32(fx&65535)>>8;if(x0>=size.width-1){x0=size.width-1;wx=0;}
                const u8* a=row0+u64(x0)*4,*b=a+(wx?4:0),*c=row1+u64(x0)*4,*d=c+(wx?4:0);
                for(u32 k=0;k<4;++k){
                    const u32 top=a[k]*(256-wx)+b[k]*wx,bottom=c[k]*(256-wx)+d[k]*wx;
                    out[k]=u8((top*(256-wy)+bottom*wy)>>16);
                }
            }
        }
    }
    else if(factor==1)for(u64 i=0;i<bytes;++i)pixels[i]=source[i];
    else {
        // Box filter: each kept pixel is the mean of a factor x factor block.
        const u32 samples=factor*factor;
        for(u32 y=0;y<stored.height;++y)for(u32 x=0;x<stored.width;++x){
            u32 sum[4]{};
            for(u32 v=0;v<factor;++v){const u8* p=source+(u64(y*factor+v)*size.width+u64(x)*factor)*4;
                for(u32 u=0;u<factor;++u,p+=4){sum[0]+=p[0];sum[1]+=p[1];sum[2]+=p[2];sum[3]+=p[3];}}
            u8* out=pixels+(u64(y)*stored.width+x)*4;
            out[0]=u8(sum[0]/samples);out[1]=u8(sum[1]/samples);out[2]=u8(sum[2]/samples);out[3]=u8(sum[3]/samples);
        }
    }
    return true;
}
// Game thread: a transient texture holding already decoded pixels.
void* texture_from(const u8* decoded_pixels,ImageSize stored){
    void* texture=fn<void*(*)(s32,s32,u32,u64)>(t::data::browser_address_5ab0820)(stored.width,stored.height,2,0);
    if(!texture)return nullptr;
    void* platform=at<void*>(texture,t::browser_layout::field_190);
    void** mips=platform?at<void**>(platform,t::browser_layout::texture_mips):nullptr;
    if(!mips||at<s32>(platform,t::browser_layout::texture_mip_count)!=1||!mips[0])return nullptr;
    void* bulk=static_cast<u8*>(mips[0])+t::browser_layout::mip_bulk;
    // Check the allocated length before any pixel access.
    const u64 bytes=u64(stored.width)*stored.height*4;
    if(fn<u64(*)(void*)>(t::data::browser_address_381f120)(bulk)!=bytes)return nullptr;
    void* destination=fn<void*(*)(void*,u32)>(t::data::browser_address_3821230)(bulk,2);
    if(!destination)return nullptr;
    auto* pixels=static_cast<u8*>(destination);
    for(u64 i=0;i<bytes;++i)pixels[i]=decoded_pixels[i];
    fn<void(*)(void*)>(t::data::browser_address_3821420)(bulk);
    at<u8>(texture,t::browser_layout::field_85)|=0x10;
    method<void(*)(void*)>(texture,t::browser_layout::virtual_2e0)(texture);
    return texture;
}
// The largest image kept is a 1280x720 screenshot.
constexpr u32 kDecodedBytes=1280u*720u*4u;
alignas(16) u8 decoded_pixels[kDecodedBytes];
void clear(Binding& b){
    if(void* image=resolve(b.image)){
        // Do not erase a brush another owner has replaced since our binding.
        if(at<u64>(image,t::browser_layout::font_or_brush_resource)==b.texture.object)fn<void(*)(void*,void*,bool)>(kSetBrush)(image,nullptr,false);
    }
    b={};if(diagnostic.binding_count)--diagnostic.binding_count;
}
void release(void*,TextureToken t){
    Lease* l=owned(t);if(!l){++diagnostic.stale;return;}
    for(auto& b:bindings)if(b.image.object&&equal(b.texture,t))clear(b);
    if(void* object=resolve(t))if(l->added_root){
        void* item=object_entry_for(object);
        __atomic_fetch_and(&at<u32>(item,t::object_item::flags),~kRoot,__ATOMIC_SEQ_CST);
    }
    diagnostic.resident_bytes-=l->bytes;--diagnostic.texture_count;++diagnostic.releases;*l={};
}
// Game thread: makes the texture for pixels in decoded_pixels and takes a lease on it.
bool keep_texture(ImageSize stored,TextureToken* out){
    Lease* empty=nullptr;for(auto& l:leases)if(!l.token.object){empty=&l;break;}
    if(!empty){++diagnostic.failed;return false;}
    const u64 begin=clock_ticks();
    void* texture=texture_from(decoded_pixels,stored);
    diagnostic.upload_us_last=u32((clock_ticks()-begin)*10/192);
    if(diagnostic.upload_us_last>diagnostic.upload_us_max)diagnostic.upload_us_max=diagnostic.upload_us_last;
    if(!is(texture,"Texture2D")){++diagnostic.failed;return false;}
    void* item=object_entry_for(texture);
    // Pin before any callback or widget publication. Preserve an existing root.
    const bool added=!(__atomic_fetch_or(&at<u32>(item,t::object_item::flags),kRoot,__ATOMIC_SEQ_CST)&kRoot);
    TextureToken t=token(texture);void* platform=at<void*>(texture,t::browser_layout::field_190);
    const u32 w=platform?at<u32>(platform,t::browser_layout::texture_width):0,h=platform?at<u32>(platform,t::browser_layout::texture_height):0,format=platform?at<u32>(platform,t::browser_layout::texture_format):0;
    diagnostic.last_width=w;diagnostic.last_height=h;diagnostic.last_format=format;
    // Our scheduler budgets four bytes per pixel; refuse FloatRGBA and any
    // decoder interpretation differing from the prevalidated PNG/JPEG header.
    if(!t.object||w!=stored.width||h!=stored.height||format!=2){
        if(added)__atomic_fetch_and(&at<u32>(item,t::object_item::flags),~kRoot,__ATOMIC_SEQ_CST);
        ++diagnostic.failed;return false;
    }
    *empty={t,w*h*4,added};*out=t;
    ++diagnostic.uploads;++diagnostic.texture_count;diagnostic.resident_bytes+=empty->bytes;return true;
}
bool checked_sizes(const uint8_t* data,uint32_t count,ImageKind kind,const ImageSize& expected,ImageSize& checked,ImageSize& stored){
    if(!game::main_base()||!image_envelope(data,count,kind,&checked)||checked.width!=expected.width||checked.height!=expected.height)return false;
    stored=image_stored_size(checked,kind);return u64(stored.width)*stored.height*4<=kDecodedBytes;
}
// Decodes on the game thread: 15 to 40 ms for a card picture, up to about
// 240 ms for the largest screenshots, one image per frame. Decoding on a
// worker thread was tried and measured: the game's decoder shares its
// allocator with the game, a worker below the game's priority held those
// locks while it waited for processor time, and frames stalled for two to
// three times the decode itself (0.5 to 0.8 s). So the cost stays here, and
// the mod page puts off a large screenshot until the page is at rest.
bool upload(void*,const uint8_t* data,uint32_t count,ImageKind kind,const ImageSize& expected,TextureToken* out){
    if(out)*out={};ImageSize checked{},stored{};
    if(!out||!checked_sizes(data,count,kind,expected,checked,stored)){++diagnostic.failed;return false;}
    const u64 begin=clock_ticks();
    // A GIF (an avatar) is decoded by our own first-frame decoder; the game's reads PNG and JPEG.
    ImageSize frame{};
    const bool ok=count>=3&&data[0]=='G'&&data[1]=='I'&&data[2]=='F'?
        gif_first_frame(data,count,decoded_pixels,kDecodedBytes,&frame)&&frame.width==stored.width&&frame.height==stored.height:
        decode_into(decoded_pixels,Decode::module(),data,count,checked,stored,image_reduction(checked,kind));
    diagnostic.decode_us_last=u32((clock_ticks()-begin)*10/192);if(diagnostic.decode_us_last>diagnostic.decode_us_max)diagnostic.decode_us_max=diagnostic.decode_us_last;++diagnostic.decodes;
    if(!ok){++diagnostic.failed;return false;}
    return keep_texture(stored,out);
}
}
ImageBackend image_backend(){return {nullptr,upload,release};}
bool bind_image(void* image,TextureToken texture){
    if(!owned(texture)||!resolve(texture)||!is(image,"Image")){++diagnostic.stale;return false;}
    const TextureToken i=token(image);if(!i.object)return false;
    Binding* slot=nullptr;
    for(auto& b:bindings){if(equal(b.image,i)){slot=&b;break;}if(!b.image.object&&!slot)slot=&b;}
    if(!slot){++diagnostic.failed;return false;}
    // Every card binds every frame; only a change reaches the widget.
    if(slot->image.object&&equal(slot->texture,texture)&&at<u64>(image,t::browser_layout::font_or_brush_resource)==texture.object)return true;
    if(!slot->image.object)++diagnostic.binding_count;
    *slot={i,texture};
    // Keep the stock brush's fitted dimensions. Same-resource size matching
    // exits early in the original implementation, so it is not a resize API.
    fn<void(*)(void*,void*,bool)>(kSetBrush)(image,ptr(texture.object),false);
    ++diagnostic.binds;return true;
}
void unbind_image(void* image){
    for(auto& b:bindings)if(b.image.object==reinterpret_cast<u64>(image))clear(b);
}
void release_images(){for(auto& l:leases)if(l.token.object)release(nullptr,l.token);}
const Diagnostics& diagnostics(){return diagnostic;}
void attach(){

    if(!attached){if(!ui::parse(p4m_get_mods_ui_json,p4m_get_mods_ui_json_bytes,layout))return;
        kFileRows=layout.details.files.rows;kColumns=layout.browse.columns;kPitchX=layout.browse.card.width+layout.browse.card.gap_x;kPitchY=layout.browse.card.height+layout.browse.card.gap_y;kViewHeight=layout.browse.viewport.height;
        attached=controller().initialize(transport(),image_backend())==P4M_OK;
        if(attached){controller().set_download_session(clock_ticks());controller().attach_installer(&install::installer());}}
}
void tick(){
    if(!attached)attach();
    const u64 begin=clock_ticks();static u64 last_frame=0;
    const bool open=attached&&controller().screen()!=Screen::Closed;
    if(open&&last_frame){const u32 us=u32((begin-last_frame)*10/192);diagnostic.frame_us_last=us;
        if(us>diagnostic.frame_us_max)diagnostic.frame_us_max=us;if(us>50000)++diagnostic.slow_frames;++diagnostic.frames;}
    last_frame=open?begin:0;
    if(attached){controller().set_time(begin/19200);controller().pump();}
    if(attached)title_tick();
    if(attached)ui_input();
    if(suspended)return; // An Installed action may have handed the screen to the settings host.
    if(attached&&(controller().screen()==Screen::Browse||controller().screen()==Screen::Installed)&&resolve(browse_host.root.token)){sidebar_tick();refresh_browse();}
    if(attached&&controller().screen()==Screen::Detail)refresh_detail();
    sync_plain();
    diagnostic.ui_us_last=u32((clock_ticks()-begin)*10/192);if(open&&diagnostic.ui_us_last>diagnostic.ui_us_max)diagnostic.ui_us_max=diagnostic.ui_us_last;
    diagnostic.images_unsettled=controller().images_unsettled();diagnostic.images_failed=controller().images_failed();diagnostic.image_bytes=controller().image_bytes();
    diagnostic.detail_state=u32(controller().detail_state());
    diagnostic.ui_screen=u32(controller().screen());diagnostic.title=title_widget.object;
    diagnostic.profile_host=0;diagnostic.author_id=controller().author().id;diagnostic.author_state=u32(controller().author_state());
    const auto state=controller().browse();diagnostic.browse_count=state.count;diagnostic.browse_error=u32(state.error);diagnostic.browse_generation=state.generation;
    diagnostic.browse_host=browse_host.root.token.object;diagnostic.detail_host=detail_host.root.token.object;diagnostic.scroll=browse_host.scroll.object;
    // Retire bindings whose pooled widgets were collected/replaced; textures
    // stay under the scheduler's independently bounded visibility leases.
    for(auto& b:bindings)if(b.image.object&&!resolve(b.image))clear(b);
}

void shutdown(){leave_title();if(attached)controller().shutdown();release_images();attached=false;}
namespace {
TSUCHI_Result host_open(const UiScreen&,void** out){
 void* widget=reinterpret_cast<void*>(ui_title_view());if(!widget||!attached||!build_host(browse_host,widget,HostKind::Browse,true))return TSUCHI_NOT_READY;
 if(resuming){resuming=false;suspended=false;close_requested=false;ui_held=input_buttons()|2;visible(widget,1);show_host(browse_host);*out=&controller();return TSUCHI_OK;}
 u32 start=0;for(u32 i=0;i<layout.browse.sidebar.count;++i)if(layout.browse.sidebar.items[i].action!=ui::SidebarAction::Search){start=i;break;}
 if(controller().open_installed()!=TSUCHI_OK)return TSUCHI_NOT_READY;
 sidebar_focus=true;category_mode=false;sidebar_selected=start;sidebar_progress=0;sidebar_time=0;ui_held=input_buttons()|1;
 close_requested=false;visible(widget,1);show_host(browse_host);*out=&controller();return TSUCHI_OK;
}
bool host_update(void*,const UiScreen&){return true;}
bool host_live(void*){return resolve(title_widget)&&ui_title_view();}
void host_close(void*){manage_requested=false;if(!suspended)controller().close();hide_host(detail_host);hide_host(browse_host);category_mode=false;visible(resolve(title_widget),0);close_requested=false;}
bool host_action(void*,uint64_t,uint32_t){return true;} // physical navigation owns this view
const UiRenderer renderer{host_open,host_update,host_live,host_close,host_action};
void restore_browser(){
 if(!ui_title_view()){suspended=false;controller().close();return;}
 resuming=true;
 if(tsuchi::ui::table.create(framework,"Mods",&host_screen)!=TSUCHI_OK)return;
 tsuchi::ui::table.add_text(framework,host_screen,"catalogue","Mods","");
 ui_screen_native_renderer(framework,host_screen,&renderer);
 tsuchi::ui::table.hold_open_over(framework,host_screen,TSUCHI_MENU_TITLE);
}
void open_installed_actions(){
 const auto* r=controller().installed_record(controller().browse().selected_id);if(!r)return;
 managed_local=controller().browse().selected_id;
 installed_manager.cleanup();installed_manager.begin(mod_choices());
 suspended=true;const auto old=host_screen;host_screen={};tsuchi::ui::table.destroy(framework,old);
 if(tsuchi::ui::table.create(framework,r->title,&management_screen)!=TSUCHI_OK){return_to_browser=true;return;}
 tsuchi::ui::table.add_text(framework,management_screen,"status","Status",r->pending&&find_mod(r->mod_id)?"Updated version saved; restart pending":r->legacy_assets&&r->live_assets?(r->enabled?"Content mounted; new loads use it":"Off saved; mounted content remains this session"):r->pending?"Installed; activation pending":r->enabled?"Enabled":"Disabled");
 tsuchi::ui::table.add_button(framework,management_screen,"toggle","Toggle enabled");
 if(auto* mod=find_mod(r->mod_id)){if(mod->settings.list.count)tsuchi::ui::table.add_button(framework,management_screen,"settings","Settings");}
 if(r->legacy_cheats||mod_has_cheats(r->mod_id))tsuchi::ui::table.add_button(framework,management_screen,"cheats","Cheats");
 if(r->submission_id){tsuchi::ui::table.add_button(framework,management_screen,"page","View mod page");tsuchi::ui::table.add_button(framework,management_screen,"update","Check for updates");tsuchi::ui::table.add_button(framework,management_screen,"remove","Uninstall package");}
 tsuchi::ui::table.hold_open_over(framework,management_screen,TSUCHI_MENU_TITLE);
}
void management_action(void*,const TSUCHI_UiActivated* e){
 if(e->screen.id==cheat_menu.screen().id){cheat_menu.activate(*e);if(!cheat_menu.screen().id)return_to_browser=true;return;}
 if(e->screen.id!=management_screen.id)return;
 const auto* r=controller().installed_record(managed_local);if(!r)return;
 const auto report=[&](const char* message){tsuchi::ui::table.set_text(framework,management_screen,"status",message);};
 const auto apply_assets=[&](){
  const bool enabled=mod_choices().enabled(r->mod_id);Text why;
  const auto result=enabled?activate_downloaded_assets(r->mod_id,why):TSUCHI_UNSUPPORTED;
  report(enabled?(result==TSUCHI_OK?"Enabled for new loads. Reload affected maps or objects.":"Enabled for next launch; live activation was unavailable."):
      downloaded_assets_active(r->mod_id)?"Disabled for next launch. Already mounted content remains active this session.":"Disabled.");why.clear();
 };
 if(!strcmp(e->control,"toggle")){
  if(r->legacy_assets&&!find_mod(r->mod_id)){
   Text activation,order;ModChoices choices;auto result=mod_choices().activation_text(activation)&&mod_choices().order_text(order)?choices.load(activation.get(),order.get()):TSUCHI_NO_MEMORY;
   const bool enabled=!mod_choices().enabled(r->mod_id);
   if(result==TSUCHI_OK)result=choices.set_enabled(r->mod_id,enabled);
   if(result==TSUCHI_OK)result=save_mod_choices(choices);choices.clear();activation.clear();order.clear();
   if(result!=TSUCHI_OK){report("The choice could not be saved.");return;}
   apply_assets();return;
  }
  if(!find_mod(r->mod_id)){report("Restart to load this mod's settings and dependency information.");return;}
  if(installed_manager.pending()){report("Review and Apply the pending dependency changes, or go Back to cancel.");return;}
  auto result=installed_manager.propose(r->mod_id,!installed_manager.draft().enabled(r->mod_id));
  if(result==TSUCHI_OK){
   if(installed_manager.changes().count>1){report("Dependencies also change. Review the changes below, then Apply.");
    for(uint32_t i=0;i<installed_manager.changes().count;++i){const auto& c=installed_manager.changes()[i];char id[32]="change";char digits[12];u32 v=i,n=0;do{digits[n++]=char('0'+v%10);v/=10;}while(v);u32 k=6;while(n)id[k++]=digits[--n];id[k]=0;
     char label[64]{};const char* action=c.enabled?"Enable ":"Disable ";memcpy(label,action,strlen(action));memcpy(label+strlen(action),c.id,strlen(c.id)+1);tsuchi::ui::table.add_button(framework,management_screen,id,label);}
    tsuchi::ui::table.add_button(framework,management_screen,"apply","Apply changes");return;}
   result=installed_manager.confirm();if(result==TSUCHI_OK)result=installed_manager.save(save_mod_choices);
  }
  if(result==TSUCHI_OK&&r->legacy_assets)apply_assets();
  else report(result==TSUCHI_OK?"Saved. Restart required for this mod's activation.":installed_manager.problem());
 }else if(!strcmp(e->control,"apply")){
  auto result=installed_manager.confirm();if(result==TSUCHI_OK)result=installed_manager.save(save_mod_choices);
  if(result==TSUCHI_OK){auto old=management_screen;management_screen={};tsuchi::ui::table.destroy(framework,old);return_to_browser=true;}
  else report(installed_manager.problem());
 }else if(!strcmp(e->control,"cheats")){
  char id[48];memcpy(id,r->mod_id,sizeof id);auto old=management_screen;management_screen={};tsuchi::ui::table.destroy(framework,old);
  if(cheat_menu.open(framework,id)!=TSUCHI_OK)return_to_browser=true;
 }else if(!strcmp(e->control,"settings")){
  auto* mod=find_mod(r->mod_id);if(!mod)return;
  TSUCHI_Screen settings{};if(ui_mod_settings(*mod,&settings)!=TSUCHI_OK){report("Settings could not be opened.");return;}
  auto old=management_screen;management_screen={};tsuchi::ui::table.destroy(framework,old);mod_settings_screen=settings;
  if(tsuchi::ui::table.hold_open_over(framework,settings,TSUCHI_MENU_TITLE)!=TSUCHI_OK){tsuchi::ui::table.destroy(framework,settings);mod_settings_screen={};return_to_browser=true;}
 }else if(!strcmp(e->control,"page")||!strcmp(e->control,"update")){
  page_requested=managed_local;auto old=management_screen;management_screen={};tsuchi::ui::table.destroy(framework,old);return_to_browser=true;
 }else if(!strcmp(e->control,"remove")){
  tsuchi::ui::table.add_button(framework,management_screen,"confirm_remove","Confirm uninstall package");report("This removes every mod in this downloaded package. Saved preferences are kept.");
 }else if(!strcmp(e->control,"confirm_remove")){
  auto result=install::installer().uninstall(r->submission_id,r->file_id);report(result==TSUCHI_OK?"Uninstalled. Running content remains until it can be released.":"Uninstall failed. The package was retained.");
 }
}
void management_closed(void*,const TSUCHI_UiClosed* e){
 if(e->screen.id==cheat_menu.screen().id){cheat_menu.close();return_to_browser=true;}
 if(e->screen.id==management_screen.id){const auto old=management_screen;management_screen={};tsuchi::ui::table.destroy(framework,old);return_to_browser=true;}
 if(e->screen.id==mod_settings_screen.id){const auto old=mod_settings_screen;mod_settings_screen={};tsuchi::ui::table.destroy(framework,old);return_to_browser=true;}
}
void cheat_changed(void*,const TSUCHI_UiChoiceChanged* e){cheat_menu.change(*e);}
void chosen(void*,const TSUCHI_UiEntryChosen* e){
 if(e->menu!=TSUCHI_MENU_TITLE||strcmp(e->entry,"mods"))return;
 if(host_screen.id)tsuchi::ui::table.destroy(framework,host_screen);
 if(tsuchi::ui::table.create(framework,"Mods",&host_screen)!=TSUCHI_OK)return;
 tsuchi::ui::table.add_text(framework,host_screen,"catalogue","Mods","");
 ui_screen_native_renderer(framework,host_screen,&renderer);
 tsuchi::ui::table.hold_open_over(framework,host_screen,TSUCHI_MENU_TITLE);
}
void text_entered(void*,const TSUCHI_UiTextEntered* e){
 if(!keyboard_pending)return;keyboard_pending=false;ui_held=input_buttons();
 if(!e->accepted)return;Query q{};catalog::plain(e->text,q.search,sizeof q.search);
 if(controller().change_query(q)==TSUCHI_OK){sidebar_focus=false;sidebar_selected=0;}
}
void frame(){
 if(cheat_menu.screen().id){cheat_menu.refresh();if(!cheat_menu.screen().id)return_to_browser=true;}
 // Keep the browser's A press from reaching the first control on a newly opened screen.
 if(manage_requested&&!(input_buttons()&1)){manage_requested=false;open_installed_actions();}
 if(return_to_browser){return_to_browser=false;restore_browser();if(page_requested){controller().open_installed_page(page_requested);page_requested=0;sidebar_focus=false;}else install::installer().refresh_library();}
 if(suspended)return;
 tick();if(close_requested){close_requested=false;ui_action(5);}}
}
void start(TSUCHI_Mod* mod){
 if(strcmp(game::table::build_key,"1.1.1"))return;
 framework=mod;bridge_start(mod);attach();TSUCHI_Subscription sub{};
 tsuchi::ui::table.on_activated(mod,management_action,nullptr,&sub);tsuchi::ui::table.on_closed(mod,management_closed,nullptr,&sub);
 tsuchi::ui::table.on_choice_changed(mod,cheat_changed,nullptr,&sub);
 tsuchi::ui::table.on_entry_chosen(mod,chosen,nullptr,&sub);tsuchi::ui::table.on_text_entered(mod,text_entered,nullptr,&sub);
 tsuchi::ui::table.hold_menu_entry(mod,TSUCHI_MENU_TITLE,"mods","Mods","Browse and install mods.");
 if(!each_engine_frame(frame))each_frame(frame);
}
} } }
namespace tsuchi {void mod_browser_start(TSUCHI_Mod* mod){browser::native::start(mod);}}
