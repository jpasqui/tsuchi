#define P4M_DOWNLOADER 1
#include "ui_document_literal.h"

#if defined(P4M_UI_DOCUMENT_PROBE) || defined(P4M_DOWNLOADER)
#include "native_access.h"
namespace tsuchi { namespace browser { namespace native { namespace ui_document_literal { namespace {
// UTextBlock and UWidget setters, main-relative, for the 1.1.1 executable. The
// two shared with other components come from build_111.h.
#define kConstructObject (reinterpret_cast<uint64_t>(t::fn::ui_construct) - game::main_base())
#define kTextFromString (reinterpret_cast<uint64_t>(t::fn::text_from_string) - game::main_base())
#define kSetFont t::data::browser_address_4c61ee0                // (text block, const FSlateFontInfo*)
#define kSetColor t::data::browser_address_4c61a20               // (text block, const FSlateColor*)
#define kSetJustification t::data::browser_address_4c62370       // (text block, u8 ETextJustify)
#define kSetShadowOffset t::data::browser_address_4c61dd0        // (text block, two floats)
#define kSetShadowColor t::data::browser_address_4c61cb0         // (text block, four floats)
#define kSetRenderTransform t::data::browser_address_4c6b5c0     // (widget, const seven floats*)
#define kSetRenderTransformPivot (reinterpret_cast<uint64_t>(t::fn::ui_widget_pivot) - game::main_base()) // (widget, two floats)
#define kSetAutoWrap t::data::browser_address_4c62570            // (text block, bool)
#define kSetText t::data::browser_address_4c64530                // (text block, const FText*)
struct GameString {char16_t* data;int32_t num,max;};
struct GameText {uint64_t words[3];};
struct SlateColor {float rgba[4];uint8_t rule,padding[7];uint64_t shared[2];};
struct Vector2 {float x,y;};
struct LinearColor {float rgba[4];};
struct Transform {float values[7];};
static_assert(sizeof(GameString)==16,"Build111 FString layout");
static_assert(sizeof(GameText)==24,"Build111 FText layout");
static_assert(sizeof(SlateColor)==40&&__builtin_offsetof(SlateColor,rule)==16&&
    __builtin_offsetof(SlateColor,shared)==24,"Build111 specified FSlateColor layout");
#ifdef P4M_UI_DOCUMENT_LITERAL_TEST
extern "C" void* p4u_literal_test_call(uint64_t offset);
#endif
template<class T> T original(const Backend& b,uint64_t offset){
#ifdef P4M_UI_DOCUMENT_LITERAL_TEST
    (void)b;return reinterpret_cast<T>(p4u_literal_test_call(offset));
#else
    return reinterpret_cast<T>(b.main_base+offset);
#endif
}
bool text_valid(const char16_t* text,uint32_t units){
    if(units>4096||(!text&&units))return false;
    if(!text)return true;
    if(text[units])return false;
    for(uint32_t i=0;i<units;++i){
        const uint16_t c=text[i];if(!c)return false;
        if(c>=0xD800&&c<=0xDBFF){
            if(++i>=units||text[i]<0xDC00||text[i]>0xDFFF)return false;
        }else if(c>=0xDC00&&c<=0xDFFF)return false;
    }return true;
}
bool finite(float value){return value==value&&value<=3.402823466e38f&&value>=-3.402823466e38f;}
bool shadow_valid(const uint8_t* rich){
    const auto* values=reinterpret_cast<const float*>(rich+t::browser_layout::shadow_offset);
    for(uint32_t i=0;i<6;++i)if(!finite(values[i]))return false;
    return true;
}
void release_text(const GameText& text){
    auto* owner=reinterpret_cast<uint8_t*>(text.words[1]);if(!owner)return;
    auto* strong=reinterpret_cast<uint32_t*>(owner+t::shared_count::holders);
    auto* weak=reinterpret_cast<uint32_t*>(owner+t::shared_count::watchers);
    if(__atomic_fetch_sub(strong,1,__ATOMIC_ACQ_REL)==1){
        method<void(*)(void*)>(owner,t::shared_count_table::destroy_object)(owner);
        if(__atomic_fetch_sub(weak,1,__ATOMIC_ACQ_REL)==1){
            method<void(*)(void*)>(owner,t::shared_count_table::destroy_self)(owner);
        }
    }
}
} // namespace
void* create(const Backend& b,void* tree,void* rich){
    if(!b.main_base||!b.find_exact_class||!b.validate_created||!tree||!rich)return nullptr;
    void* cls=b.find_exact_class(b.context,"TextBlock");if(!cls)return nullptr;
    const uint64_t args[8]{reinterpret_cast<uint64_t>(cls),reinterpret_cast<uint64_t>(tree)};
    void* widget=original<void*(*)(const void*)>(b,kConstructObject)(args);
    if(!widget||!b.validate_created(b.context,widget))return nullptr;
    if(!set_style(b,widget,rich))return nullptr;
    return widget;
}
bool set_style(const Backend& b,void* widget,void* rich){
    if(!b.main_base||!widget||!rich)return false;
    const auto* source=reinterpret_cast<const uint8_t*>(rich);
    if(source[t::browser_layout::justification]>2||!shadow_valid(source))return false;
    original<void(*)(void*,const void*)>(b,kSetFont)(
        widget,reinterpret_cast<const uint8_t*>(rich)+t::browser_layout::font_or_brush_resource);
    // Reflection gives UseColor_Specified=0. The original UTextBlock VM
    // wrapper constructs that rule with zero shared refs before its setter.
    // Rich color is only 16 bytes; never pass it as a 40-byte SlateColor.
    SlateColor color{};
    const auto* rgba=reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(rich)+t::browser_layout::text_color);
    for(uint32_t i=0;i<4;++i)color.rgba[i]=rgba[i];
    original<void(*)(void*,const SlateColor*)>(b,kSetColor)(widget,&color);
    set_justification(b,widget,source[t::browser_layout::justification]);
    set_shadow_style(b,widget,rich);
    return true;
}
bool set_justification(const Backend& b,void* widget,uint8_t value){
    if(!b.main_base||!widget||value>2)return false;
    original<void(*)(void*,uint8_t)>(b,kSetJustification)(widget,value);return true;
}
bool set_shadow_style(const Backend& b,void* widget,void* rich){
    if(!b.main_base||!widget||!rich)return false;
    const auto* source=reinterpret_cast<const uint8_t*>(rich);
    if(!shadow_valid(source))return false;
    const auto* offset=reinterpret_cast<const float*>(source+t::browser_layout::shadow_offset);
    const auto* rgba=reinterpret_cast<const float*>(source+t::browser_layout::shadow_color);
    const Vector2 copied_offset{offset[0],offset[1]};LinearColor copied_color{};
    for(uint32_t i=0;i<4;++i)copied_color.rgba[i]=rgba[i];
    original<void(*)(void*,Vector2)>(b,kSetShadowOffset)(widget,copied_offset);
    original<void(*)(void*,LinearColor)>(b,kSetShadowColor)(widget,copied_color);return true;
}
bool set_render_transform(const Backend& b,void* widget,const float* values,const float* pivot){
    if(!b.main_base||!widget||!values||!pivot)return false;
    Transform copied{};
    for(uint32_t i=0;i<7;++i){if(!finite(values[i]))return false;copied.values[i]=values[i];}
    if(!finite(pivot[0])||!finite(pivot[1]))return false;
    const Vector2 copied_pivot{pivot[0],pivot[1]};
    original<void(*)(void*,const Transform*)>(b,kSetRenderTransform)(widget,&copied);
    original<void(*)(void*,Vector2)>(b,kSetRenderTransformPivot)(widget,copied_pivot);return true;
}
bool set_auto_wrap(const Backend& b,void* widget,bool enabled){
    if(!b.main_base||!widget)return false;
    original<void(*)(void*,bool)>(b,kSetAutoWrap)(widget,enabled);return true;
}
bool set_font_scale(const Backend& b,void* widget,void* rich,float scale){
    if(!b.main_base||!widget||!rich||!(scale>=0.25f&&scale<=2.f))return false;
    // Reflected SlateFontInfo:88 bytes; int32 Size at +72. This raw stack
    // value borrows the live font refs; original SetFont copies/retains them.
    alignas(8) uint8_t font[88];const auto* source=reinterpret_cast<const uint8_t*>(rich)+t::browser_layout::font_or_brush_resource;
    for(uint32_t i=0;i<sizeof font;++i)font[i]=source[i];
    auto* size=reinterpret_cast<int32_t*>(font+t::browser_layout::font_size);if(*size<1||*size>256)return false;
    *size=int32_t(float(*size)*scale+0.5f);if(*size<1)*size=1;
    original<void(*)(void*,const void*)>(b,kSetFont)(widget,font);return true;
}
bool set_text(const Backend& b,void* widget,const char16_t* text,uint32_t units){
    if(!b.main_base||!widget||!text_valid(text,units))return false;
    const char16_t empty=0;
    GameString string{const_cast<char16_t*>(text?text:&empty),int32_t(units+1),int32_t(units+1)};
    const GameText value=original<GameText(*)(const GameString*)>(b,kTextFromString)(&string);
    original<void(*)(void*,const GameText*)>(b,kSetText)(widget,&value);
    release_text(value);return true;
}
} } } }
#else
namespace tsuchi { namespace browser { namespace native { namespace ui_document_literal {
void* create(const Backend&,void*,void*){return nullptr;}
bool set_style(const Backend&,void*,void*){return false;}
bool set_justification(const Backend&,void*,uint8_t){return false;}
bool set_shadow_style(const Backend&,void*,void*){return false;}
bool set_render_transform(const Backend&,void*,const float*,const float*){return false;}
bool set_font_scale(const Backend&,void*,void*,float){return false;}
bool set_auto_wrap(const Backend&,void*,bool){return false;}
bool set_text(const Backend&,void*,const char16_t*,uint32_t){return false;}
} } } }
#endif
