#pragma once
#include <stdint.h>

// Private Build111 candidate. The caller validates exact UObject identities,
// WidgetTree ownership and the live stock rich-text style source before calls.
// It owns the returned plain widget, its slot, containment and GC retention.
namespace tsuchi { namespace browser { namespace native { namespace ui_document_literal {
struct Backend {
    uint64_t main_base;
    void* context;
    void* (*find_exact_class)(void* context,const char* name);
    bool (*validate_created)(void* context,void* exact_text_block);
};
// Creates exact UMG TextBlock and borrows/copies the stock rich font at +0x160
// through the original SetFont. It does not create a slot or mount a host.
void* create(const Backend&,void* widget_tree,void* stock_rich_text);
// Reapply stock font, specified RGBA, justification and default-style shadow
// after source NativeConstruct. Both inputs are caller-validated.
bool set_style(const Backend&,void* exact_text_block,void* stock_rich_text);
// ETextJustify: Left=0, Center=1, Right=2. Invalid values make no engine call.
bool set_justification(const Backend&,void* exact_text_block,uint8_t);
bool set_shadow_style(const Backend&,void* exact_text_block,void* stock_rich_text);
// Caller-validated widget; copies seven transform floats and two pivot floats
// before original setters. Non-finite or missing values make no engine call.
bool set_render_transform(const Backend&,void* widget,const float* transform7,const float* pivot2);
bool set_font_scale(const Backend&,void* exact_text_block,void* stock_rich_text,float scale);
bool set_auto_wrap(const Backend&,void* exact_text_block,bool enabled);
// UTF-16 units exclude the required trailing NUL; at most 4096 units.
// Embedded NUL and malformed surrogate sequences fail before engine calls.
// Punctuation is passed unchanged to plain UTextBlock::SetText.
bool set_text(const Backend&,void* exact_text_block,const char16_t*,uint32_t units);
} } } }
