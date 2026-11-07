#include "names.h"

#include "game.h"

namespace tsuchi::game {
namespace {

namespace t = table;

// Where a name's stored text is, how long it is, and whether it is 16-bit.
struct Stored {
    uint64_t text = 0;
    uint32_t length = 0;
    bool wide = false;
};

bool find(Name name, Stored& out) {
    const uint32_t block_index = name.index >> t::constant::names_per_block_shift;
    if (block_index >= t::constant::name_block_count) return false;
    const uint64_t block = global<uint64_t>(t::data::name_blocks + 8 * uint64_t(block_index));
    if (!block) return false;
    const uint32_t place = name.index & ((1u << t::constant::names_per_block_shift) - 1);
    const uint64_t entry = block + t::constant::name_stride * uint64_t(place);
    uint16_t header;
    if (!read(entry + t::name_entry::header, header)) return false;
    out.wide = (header & t::constant::name_wide_bit) != 0;
    out.length = header >> t::constant::name_length_shift;
    out.text = entry + t::name_entry::text;
    return out.length && readable(out.text, uint64_t(out.length) * (out.wide ? 2 : 1));
}

// Collects UTF-8 into a buffer and keeps counting past its end.
struct Writer {
    char* out;
    size_t capacity, count = 0;
    void put(char c) {
        if (count < capacity) out[count] = c;
        ++count;
    }
    void put_code_point(uint32_t c) {
        if (c < 0x80) {
            put(char(c));
        } else if (c < 0x800) {
            put(char(0xC0 | (c >> 6)));
            put(char(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            put(char(0xE0 | (c >> 12)));
            put(char(0x80 | ((c >> 6) & 0x3F)));
            put(char(0x80 | (c & 0x3F)));
        } else {
            put(char(0xF0 | (c >> 18)));
            put(char(0x80 | ((c >> 12) & 0x3F)));
            put(char(0x80 | ((c >> 6) & 0x3F)));
            put(char(0x80 | (c & 0x3F)));
        }
    }
};

// Writes the name's text, its number and a terminator.
void put_name(const Stored& stored, Name name, Writer& writer) {
    for (uint32_t i = 0; i < stored.length; ++i) {
        uint32_t c = stored.wide ? read_or<uint16_t>(stored.text + 2 * uint64_t(i)) : read_or<uint8_t>(stored.text + i);
        if (stored.wide && c >= 0xD800 && c < 0xDC00 && i + 1 < stored.length) {
            const uint32_t low = read_or<uint16_t>(stored.text + 2 * uint64_t(i + 1));
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        writer.put_code_point(c);
    }
    if (name.number) {
        char digits[10];
        uint32_t count = 0, value = name.number - 1;
        do {
            digits[count++] = char('0' + value % 10);
            value /= 10;
        } while (value);
        writer.put('_');
        while (count) writer.put(digits[--count]);
    }
    writer.put('\0');
}

} // namespace

bool name_text(Name name, char* out, size_t capacity, size_t* needed) {
    if (needed) *needed = 0;
    Stored stored;
    if (!find(name, stored)) return false;
    // Measured first, so a buffer that is too small is left as it was.
    Writer measure{nullptr, 0};
    put_name(stored, name, measure);
    if (!out || measure.count > capacity) {
        if (needed) *needed = measure.count;
        return false;
    }
    Writer writer{out, capacity};
    put_name(stored, name, writer);
    return true;
}

TSUCHI_Result name_into(Name name, char* buffer, uint64_t capacity, uint64_t* needed) {
    size_t bytes = 0;
    if (name_text(name, buffer, size_t(capacity), &bytes)) {
        *needed = strlen(buffer) + 1;
        return TSUCHI_OK;
    }
    if (!bytes) return TSUCHI_NOT_FOUND;
    *needed = bytes;
    return TSUCHI_CAPACITY;
}

bool name_is(Name name, const char* text) {
    if (!text) return false;
    char buffer[kNameBytes];
    return name_text(name, buffer, sizeof buffer) && !strcmp(buffer, text);
}

} // namespace tsuchi::game
