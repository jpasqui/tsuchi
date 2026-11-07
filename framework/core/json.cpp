#include "json.h"

#include <string.h>

#include "registry.h"

namespace tsuchi {
namespace {

// How deep lists and objects may sit inside one another. Each level takes room on the stack, which is not endless.
constexpr uint32_t kDeepest = 64;

// The tree's memory: blocks handed out from the front, kept in a chain so they can all be given back.
struct Block {
    Block* next;
    size_t used, capacity;
};
struct Document {
    Block* blocks;
    Json root;
};

struct Reader {
    const char* text;
    size_t bytes, at = 0;
    Document* document = nullptr;
    const char* failure = nullptr;
    bool starved = false;

    void* take(size_t wanted) {
        wanted = (wanted + 7) & ~size_t(7);
        Block* block = document->blocks;
        if (!block || block->capacity - block->used < wanted) {
            const size_t capacity = wanted > 4096 ? wanted : 4096;
            block = static_cast<Block*>(allocate(sizeof(Block) + capacity));
            if (!block) {
                starved = true;
                return nullptr;
            }
            *block = Block{document->blocks, 0, capacity};
            document->blocks = block;
        }
        void* given = reinterpret_cast<char*>(block + 1) + block->used;
        block->used += wanted;
        return given;
    }

    bool fail(const char* why) {
        failure = why;
        return false;
    }
    bool more() const { return at < bytes; }
    char peek() const { return text[at]; }
    void skip_space() {
        while (more() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) ++at;
    }
    bool word(const char* expected) {
        const size_t length = strlen(expected);
        if (bytes - at < length || memcmp(text + at, expected, length)) return false;
        at += length;
        return true;
    }

    // Four hexadecimal digits. A text's closing quote is none, so this stops at it.
    bool hex4(uint32_t& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[at + i];
            const uint32_t digit = c >= '0' && c <= '9' ? uint32_t(c - '0') : c >= 'a' && c <= 'f' ? uint32_t(c - 'a' + 10) : c >= 'A' && c <= 'F' ? uint32_t(c - 'A' + 10) : 16;
            if (digit == 16) return false;
            out = out * 16 + digit;
        }
        at += 4;
        return true;
    }

    // How many bytes a UTF-8 character that starts here takes, or 0 when it is not one. Within a text: its closing
    // quote is no part of a character, so this stops at it.
    size_t character() const {
        const auto byte = [&](size_t i) { return uint8_t(text[at + i]); };
        const uint8_t lead = byte(0);
        const size_t length = lead < 0x80 ? 1 : lead >= 0xC2 && lead <= 0xDF ? 2 : lead >= 0xE0 && lead <= 0xEF ? 3 : lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
        if (!length) return 0;
        for (size_t i = 1; i < length; ++i)
            if ((byte(i) & 0xC0) != 0x80) return 0;
        if (lead == 0xE0 && byte(1) < 0xA0) return 0;  // written longer than it needs to be
        if (lead == 0xED && byte(1) > 0x9F) return 0;  // half of a pair, which is no character
        if (lead == 0xF0 && byte(1) < 0x90) return 0;
        if (lead == 0xF4 && byte(1) > 0x8F) return 0;  // past the last character there is
        return length;
    }

    // A text, from its opening quote to its closing one. It is never longer than the way it is written.
    bool string(const char*& out, size_t& length) {
        const size_t start = ++at;
        size_t end = start;
        while (end < bytes && text[end] != '"') end += text[end] == '\\' && end + 1 < bytes ? 2 : 1;
        if (end >= bytes) return fail("a text is not closed");
        auto* made = static_cast<char*>(take(end - start + 1));
        if (!made) return false;
        size_t put = 0;
        const auto encode = [&](uint32_t code) {
            if (code < 0x80) {
                made[put++] = char(code);
            } else if (code < 0x800) {
                made[put++] = char(0xC0 | code >> 6), made[put++] = char(0x80 | (code & 0x3F));
            } else if (code < 0x10000) {
                made[put++] = char(0xE0 | code >> 12), made[put++] = char(0x80 | ((code >> 6) & 0x3F)), made[put++] = char(0x80 | (code & 0x3F));
            } else {
                made[put++] = char(0xF0 | code >> 18), made[put++] = char(0x80 | ((code >> 12) & 0x3F));
                made[put++] = char(0x80 | ((code >> 6) & 0x3F)), made[put++] = char(0x80 | (code & 0x3F));
            }
        };
        while (at < end) {
            const char c = text[at];
            if (uint8_t(c) < 0x20) return fail("a text holds a control character, which has to be written with a backslash");
            if (c != '\\') {
                const size_t size = character();
                if (!size) return fail("the text is not UTF-8");
                memcpy(made + put, text + at, size);
                put += size, at += size;
                continue;
            }
            const char escaped = text[at + 1];
            at += 2;
            uint32_t code = 0;
            switch (escaped) {
            case '"': made[put++] = '"'; break;
            case '\\': made[put++] = '\\'; break;
            case '/': made[put++] = '/'; break;
            case 'b': made[put++] = '\b'; break;
            case 'f': made[put++] = '\f'; break;
            case 'n': made[put++] = '\n'; break;
            case 'r': made[put++] = '\r'; break;
            case 't': made[put++] = '\t'; break;
            case 'u': {
                if (!hex4(code)) return fail("\\u is followed by four hexadecimal digits");
                if (code >= 0xDC00 && code <= 0xDFFF) return fail("the second half of a pair with no first half");
                if (code >= 0xD800 && code <= 0xDBFF) { // the first half of a pair: the second has to follow
                    uint32_t low = 0;
                    if (bytes - at < 2 || text[at] != '\\' || text[at + 1] != 'u') return fail("the first half of a pair with no second half");
                    at += 2;
                    if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return fail("the first half of a pair with no second half");
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                encode(code);
                break;
            }
            default: return fail("a backslash is followed by something it cannot be followed by");
            }
        }
        made[put] = 0;
        out = made, length = put;
        ++at; // the closing quote
        return true;
    }

    static bool digit(char c) { return c >= '0' && c <= '9'; }

    bool number(Json& value) {
        // Ten to these powers is exact in a double, so a number of few digits is read exactly.
        static const double kTens[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
        const bool negative = peek() == '-';
        if (negative) ++at;
        if (!more() || !digit(peek())) return fail("a number has no digits");
        if (peek() == '0' && at + 1 < bytes && digit(text[at + 1])) return fail("a number begins with a zero it does not need");
        uint64_t digits = 0; // the number without its point, as far as it fits
        int32_t tens = 0;    // and the power of ten that goes with it
        bool every_digit = true;
        const auto take_digit = [&](bool after_point) {
            const uint64_t one = uint64_t(peek() - '0');
            if (digits <= (UINT64_MAX - one) / 10) {
                digits = digits * 10 + one;
                tens -= after_point;
            } else { // one digit more than can be kept: only its place counts
                every_digit = false;
                tens += !after_point;
            }
        };
        for (; more() && digit(peek()); ++at) take_digit(false);
        bool plain = true;
        if (more() && peek() == '.') {
            plain = false;
            ++at;
            if (!more() || !digit(peek())) return fail("a number has a point and no digits after it");
            for (; more() && digit(peek()); ++at) take_digit(true);
        }
        if (more() && (peek() == 'e' || peek() == 'E')) {
            plain = false;
            ++at;
            const bool down = more() && peek() == '-';
            if (more() && (peek() == '-' || peek() == '+')) ++at;
            if (!more() || !digit(peek())) return fail("a number has an exponent with no digits");
            int32_t exponent = 0;
            for (; more() && digit(peek()); ++at)
                if (exponent < 100000) exponent = exponent * 10 + int32_t(peek() - '0');
            tens += down ? -exponent : exponent;
        }
        double amount = double(digits);
        for (int32_t left = tens; left > 0 && amount != 0; left -= 22) {
            amount *= kTens[left < 22 ? left : 22];
            if (amount * 0 != 0) return fail("a number is too large");
        }
        for (int32_t left = -tens; left > 0 && amount != 0; left -= 22) amount /= kTens[left < 22 ? left : 22];
        const uint64_t limit = negative ? uint64_t(1) << 63 : uint64_t(INT64_MAX);
        value.kind = Json::Kind::number;
        value.number = negative ? -amount : amount;
        value.whole = plain && every_digit && digits <= limit;
        value.integer = !value.whole ? 0 : negative ? int64_t(~digits + 1) : int64_t(digits);
        return true;
    }

    bool value(Json& out, uint32_t depth) {
        skip_space();
        if (!more()) return fail("a value is missing");
        const char c = peek();
        if (c == '"') {
            out.kind = Json::Kind::text;
            return string(out.text, out.bytes);
        }
        if (c == '-' || digit(c)) return number(out);
        if (word("true")) return out.kind = Json::Kind::boolean, out.boolean = true, true;
        if (word("false")) return out.kind = Json::Kind::boolean, true;
        if (word("null")) return true;
        if (c != '[' && c != '{') return fail("this is no value");
        if (depth == kDeepest) return fail("lists and objects are inside one another too deep");
        const bool object = c == '{';
        const char closing = object ? '}' : ']';
        out.kind = object ? Json::Kind::object : Json::Kind::list;
        ++at;
        skip_space();
        if (more() && peek() == closing) return ++at, true;
        Json* last = nullptr;
        for (;;) {
            auto* member = static_cast<Json*>(take(sizeof(Json)));
            if (!member) return false;
            *member = Json{};
            if (object) {
                skip_space();
                if (!more() || peek() != '"') return fail("a name is missing: names are texts");
                size_t length = 0;
                if (!string(member->name, length)) return false;
                if (strlen(member->name) != length) return fail("a name holds a zero");
                if (out.find(member->name)) return fail("the same name is used twice in one object");
                skip_space();
                if (!more() || peek() != ':') return fail("a colon is missing after a name");
                ++at;
            }
            if (!value(*member, depth + 1)) return false;
            (last ? last->next : out.first) = member;
            last = member;
            ++out.count;
            skip_space();
            if (more() && peek() == ',') {
                ++at;
                continue;
            }
            if (more() && peek() == closing) return ++at, true;
            return fail(object ? "a comma or a closing brace is missing" : "a comma or a closing bracket is missing");
        }
    }
};

void give_back(Document* document) {
    for (Block* block = document->blocks; block;) {
        Block* next = block->next;
        release(block);
        block = next;
    }
    release(document);
}

} // namespace

const Json* Json::find(const char* wanted) const {
    if (kind != Kind::object) return nullptr;
    for (const Json* member = first; member; member = member->next)
        if (!strcmp(member->name, wanted)) return member;
    return nullptr;
}

const Json* Json::at(uint32_t index) const {
    const Json* member = first;
    for (; member && index; --index) member = member->next;
    return member;
}

const Json* read_json(const char* text, size_t bytes, JsonError* error) {
    if (error) *error = JsonError{0, 0, nullptr};
    auto* document = static_cast<Document*>(allocate(sizeof(Document)));
    if (!document) {
        if (error) { error->what = "there is no memory to read it into"; error->allocation_failure = true; }
        return nullptr;
    }
    *document = Document{};
    Reader reader{text, bytes};
    reader.document = document;
    if (bytes >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) reader.at = 3; // a mark some editors put first
    bool read = reader.value(document->root, 0);
    if (read) {
        reader.skip_space();
        if (reader.more()) read = reader.fail("something follows the value");
    }
    if (read) return &document->root;
    if (error) {
        error->allocation_failure = reader.starved;
        error->what = reader.starved ? "there is no memory to read it into" : reader.failure;
        error->line = 1, error->column = 1;
        for (size_t i = 0; i < reader.at && i < bytes; ++i) {
            if (text[i] == '\n') ++error->line, error->column = 1;
            else ++error->column;
        }
    }
    give_back(document);
    return nullptr;
}

void release_json(const Json* root) {
    if (!root) return;
    give_back(reinterpret_cast<Document*>(reinterpret_cast<char*>(const_cast<Json*>(root)) - offsetof(Document, root)));
}

} // namespace tsuchi
