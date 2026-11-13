// The small native glyph vocabulary shared by generated and authored UI text.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace tsuchi {
constexpr size_t kUiTextBytes = 640;
constexpr uint32_t kUiGlyphCount = 7;
inline const char* ui_glyph_token(uint32_t button) {
    static const char* const tokens[] = {
        "[PictureFont:IconData key=\"A_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"B_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"X_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"Y_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"L_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"R_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"ZR_Button\" scale=\"0.76\"/]"
    };
    return button < kUiGlyphCount ? tokens[button] : nullptr;
}
// Syntax/size check only. Callers also use the game's UTF-8 converter before
// storing or passing text; this helper is deliberately not another UTF codec.
// With `lines`, a line break is text too, for a rich text block of several lines.
inline bool ui_glyph_text_syntax(const char* text, bool empty = false, bool lines = false) {
    if (!text || (!*text && !empty)) return false;
    const size_t length = strlen(text);
    if (length > kUiTextBytes) return false;
    for (size_t at = 0; at < length;) {
        const unsigned char byte = static_cast<unsigned char>(text[at]);
        if (lines && byte == '\n') { ++at; continue; }
        if (byte < 32 || byte == 127 || byte == ']') return false;
        if (byte != '[') { ++at; continue; }
        size_t matched = 0;
        for (uint32_t i = 0; i < kUiGlyphCount; ++i) {
            const char* token = ui_glyph_token(i);
            const size_t size = strlen(token);
            if (size <= length - at && !memcmp(text + at, token, size)) { matched = size; break; }
        }
        if (!matched) return false;
        at += matched;
    }
    return true;
}
} // namespace tsuchi
