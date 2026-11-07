// Checks of the JSON reader. Exits with the number of failures.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "json.h"
#include "registry.h"

namespace {

int failures = 0;
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

// The reader's memory, watched: which blocks are out, and after how many more there is none.
std::map<void*, size_t> blocks_out;
int allocations_left = -1; // below 0: as many as are asked for
void* watched_allocate(size_t bytes) {
    if (allocations_left == 0) return nullptr;
    if (allocations_left > 0) --allocations_left;
    void* block = std::malloc(bytes);
    std::memset(block, 0xA5, bytes); // nothing may count on what new memory holds
    blocks_out[block] = bytes;
    return block;
}
void watched_release(void* block) {
    if (!block) return;
    blocks_out.erase(block);
    std::free(block);
}

const tsuchi::Json* read(const std::string& text, tsuchi::JsonError* error = nullptr) { return tsuchi::read_json(text.data(), text.size(), error); }

// Whether the text is refused, at the line and column given, for a reason that says the word given.
bool refused(const std::string& text, uint32_t line, uint32_t column, const char* word) {
    tsuchi::JsonError error{1, 1, "old failure", true};
    const tsuchi::Json* root = read(text, &error);
    const bool right = !root && !error.allocation_failure && error.what && std::strstr(error.what, word) &&
        error.line == line && error.column == column && blocks_out.empty();
    if (!right) std::printf("  [%s]: %s at %u:%u\n", text.c_str(), root ? "read" : error.what ? error.what : "<no reason>", error.line, error.column);
    tsuchi::release_json(root);
    return right;
}

void values() {
    using Kind = tsuchi::Json::Kind;
    // Each kind of value on its own, with space around it.
    const tsuchi::Json* value = read(" \t\r\n null \n");
    CHECK(value && value->kind == Kind::null && !value->first && !value->next && !value->name);
    tsuchi::release_json(value);
    value = read("true");
    CHECK(value && value->kind == Kind::boolean && value->boolean);
    tsuchi::release_json(value);
    value = read("false");
    CHECK(value && value->kind == Kind::boolean && !value->boolean);
    tsuchi::release_json(value);
    CHECK(blocks_out.empty());

    // Numbers: whole ones exactly, as far as they go; the rest as the nearest there is.
    const auto number = [](const char* text, double expected, bool whole = false, int64_t integer = 0) {
        const tsuchi::Json* value = read(text);
        const bool right = value && value->kind == Kind::number && value->number == expected && value->whole == whole && value->integer == integer;
        if (!right) std::printf("  [%s]: %.17g %d %lld\n", text, value ? value->number : NAN, value ? int(value->whole) : -1, value ? static_cast<long long>(value->integer) : 0);
        tsuchi::release_json(value);
        return right;
    };
    CHECK(number("0", 0, true, 0) && number("-0", -0.0, true, 0) && number("7", 7, true, 7) && number("-12", -12, true, -12) && number("1234567890123", 1234567890123.0, true, 1234567890123));
    CHECK(number("9223372036854775807", 9223372036854775807.0, true, INT64_MAX) && number("-9223372036854775808", -9223372036854775808.0, true, INT64_MIN));
    CHECK(number("9223372036854775808", 9223372036854775808.0) && number("-9223372036854775809", -9223372036854775808.0) && number("18446744073709551615", 18446744073709551615.0));
    CHECK(number("18446744073709551616", 18446744073709551616.0) && number("123456789012345678901234567890", 1.2345678901234568e29));
    CHECK(number("0.1", 0.1) && number("2.5", 2.5) && number("-0.75", -0.75) && number("3.14159", 3.14159) && number("1.0", 1.0) && number("100.001", 100.001));
    CHECK(number("1e3", 1000) && number("1E3", 1000) && number("1e+3", 1000) && number("25e-1", 2.5) && number("1.5e2", 150) && number("0e5", 0) && number("5e-324", 5e-324));
    CHECK(number("1e22", 1e22) && number("1e23", 1e23) && number("1e-400", 0) && number("0e99999999999", 0) && number("1e-99999999999", 0));
    // Far from one, a number is read to within a few parts in ten to the sixteenth.
    const tsuchi::Json* large = read("1.7976931348623157e308"), *small = read("2.2250738585072014e-308");
    CHECK(large && std::fabs(large->number / 1.7976931348623157e308 - 1) < 1e-15 && small && std::fabs(small->number / 2.2250738585072014e-308 - 1) < 1e-15);
    tsuchi::release_json(large), tsuchi::release_json(small);
    CHECK(number("0.30000000000000004", 0.30000000000000004) && number("123456789.123456789", 123456789.123456789));

    // Texts: as written, with what a backslash stands for, in UTF-8.
    const auto text = [](const std::string& written, const std::string& expected) {
        const tsuchi::Json* value = read(written);
        const bool right = value && value->kind == Kind::text && std::string(value->text, value->bytes) == expected && value->text[value->bytes] == 0;
        if (!right) std::printf("  [%s]: [%s]\n", written.c_str(), value && value->kind == Kind::text ? value->text : "<none>");
        tsuchi::release_json(value);
        return right;
    };
    CHECK(text("\"\"", "") && text("\"plain\"", "plain") && text("\"a b\"", "a b") && text("\"caf\xC3\xA9 \xF0\x9F\x8C\xB1\"", "caf\xC3\xA9 \xF0\x9F\x8C\xB1"));
    CHECK(text(R"("\"\\\/\b\f\n\r\t")", "\"\\/\b\f\n\r\t") && text(R"("\u0041\u00e9\u20AC")", "A\xC3\xA9\xE2\x82\xAC") && text(R"("\ud83c\udf31")", "\xF0\x9F\x8C\xB1"));
    CHECK(text(std::string("\"a\\u0000b\""), std::string("a\0b", 3)));
    CHECK(text("\"" + std::string(10000, 'x') + "\"", std::string(10000, 'x'))); // longer than one block of the reader's memory
    CHECK(blocks_out.empty());
}

void trees() {
    using Kind = tsuchi::Json::Kind;
    const tsuchi::Json* empty = read("[]"), *nothing = read(" { } ");
    CHECK(empty && empty->kind == Kind::list && empty->count == 0 && !empty->first && !empty->at(0));
    CHECK(nothing && nothing->kind == Kind::object && nothing->count == 0 && !nothing->first && !nothing->find("a"));
    tsuchi::release_json(empty), tsuchi::release_json(nothing);

    const tsuchi::Json* root = read(R"({"id": "check.files", "version" : "1.2.0", "requires": [ "a.b", {"id": "c", "at least": 2} ],
        "on": true, "weight": 2.5, "nothing": null, "": "an empty name", "deep": {"deeper": {"deepest": [[1], [2, 3]]}}})");
    CHECK(root && root->kind == Kind::object && root->count == 8);
    if (root) {
        const tsuchi::Json* id = root->find("id"), *needs = root->find("requires"), *deep = root->find("deep");
        CHECK(id && id->kind == Kind::text && !std::strcmp(id->text, "check.files") && !std::strcmp(id->name, "id") && id == root->first && id == root->at(0));
        CHECK(root->at(1) == root->find("version") && id->next == root->at(1) && !root->at(8) && !root->find("ID") && !root->find("i") && !root->find("idx"));
        CHECK(root->find("on")->boolean && root->find("weight")->number == 2.5 && root->find("nothing")->kind == Kind::null && !std::strcmp(root->find("")->text, "an empty name"));
        CHECK(needs && needs->kind == Kind::list && needs->count == 2 && needs->at(0)->kind == Kind::text && !needs->at(0)->name && needs->at(1)->kind == Kind::object &&
              needs->at(1)->find("at least")->integer == 2 && !needs->at(2) && !needs->find("id") && !id->at(0) && !id->find("x"));
        const tsuchi::Json* deepest = deep ? deep->find("deeper")->find("deepest") : nullptr;
        CHECK(deepest && deepest->count == 2 && deepest->at(1)->count == 2 && deepest->at(1)->at(1)->integer == 3 && deepest->at(0)->at(0)->integer == 1);
    }
    tsuchi::release_json(root);
    // Many values: more than one block of the reader's memory.
    std::string many = "[";
    for (int i = 0; i < 2000; ++i) many += (i ? "," : "") + std::to_string(i);
    root = read(many + "]");
    CHECK(root && root->count == 2000 && root->at(1999)->integer == 1999 && root->at(1000)->integer == 1000 && blocks_out.size() > 2);
    tsuchi::release_json(root);
    // As deep as it goes, and one deeper.
    root = read(std::string(64, '[') + std::string(64, ']'));
    CHECK(root && root->kind == Kind::list);
    tsuchi::release_json(root);
    tsuchi::release_json(nullptr);
    CHECK(blocks_out.empty());
    CHECK(refused(std::string(65, '[') + std::string(65, ']'), 1, 65, "too deep") && refused(std::string(100000, '['), 1, 65, "too deep"));
}

void refusals() {
    // What is not JSON, and where it stops being it.
    CHECK(refused("", 1, 1, "missing") && refused("   ", 1, 4, "missing") && refused("nul", 1, 1, "no value") && refused("True", 1, 1, "no value") && refused("'a'", 1, 1, "no value"));
    CHECK(refused("1 2", 1, 3, "follows") && refused("{} x", 1, 4, "follows") && refused("[1],", 1, 4, "follows") && refused("// a note\n1", 1, 1, "no value"));
    CHECK(refused("[1,]", 1, 4, "no value") && refused("[1 2]", 1, 4, "comma") && refused("[1", 1, 3, "comma") && refused("[", 1, 2, "missing") && refused("[,1]", 1, 2, "no value"));
    CHECK(refused("{\"a\":1,}", 1, 8, "name") && refused("{a:1}", 1, 2, "name") && refused("{\"a\" 1}", 1, 6, "colon") && refused("{\"a\":}", 1, 6, "no value") &&
          refused("{\"a\":1 \"b\":2}", 1, 8, "comma") && refused("{\"a\":1", 1, 7, "comma") && refused("{1:2}", 1, 2, "name"));
    CHECK(refused("{\"a\":1,\n \"b\":2,\n \"a\":3}", 3, 5, "twice") && refused("{\"a\\u0000b\":1}", 1, 12, "zero"));
    CHECK(refused("-", 1, 2, "no digits") && refused("01", 1, 1, "zero") && refused("-01", 1, 2, "zero") && refused("1.", 1, 3, "point") && refused(".5", 1, 1, "no value") &&
          refused("1e", 1, 3, "exponent") && refused("1e+", 1, 4, "exponent") && refused("+1", 1, 1, "no value") && refused("1e999", 1, 6, "too large") && refused("1e99999999999", 1, 14, "too large") &&
          refused("0x10", 1, 2, "follows") && refused("1.5.2", 1, 4, "follows"));
    CHECK(refused("\"abc", 1, 2, "not closed") && refused("\"abc\\\"", 1, 2, "not closed") && refused("\"a\nb\"", 1, 3, "control") && refused("\"a\tb\"", 1, 3, "control") &&
          refused("\"\\x41\"", 1, 4, "backslash") && refused("\"\\u12\"", 1, 4, "four") && refused("\"\\u12G4\"", 1, 4, "four"));
    CHECK(refused("\"\\ud83c\"", 1, 8, "second half") && refused("\"\\ud83cx\"", 1, 8, "second half") && refused("\"\\ud83c\\u0041\"", 1, 14, "second half") &&
          refused("\"\\udf31\"", 1, 8, "first half"));
    // Bytes that are not UTF-8: a stray one, one cut short, one written longer than it needs to be, half of a pair, one past the last.
    for (const char* bad : {"\"\x80\"", "\"\xC3\"", "\"\xC0\xAF\"", "\"\xE0\x80\xAF\"", "\"\xED\xA0\x80\"", "\"\xF4\x90\x80\x80\"", "\"\xF8\x88\x80\x80\x80\"", "\"\xE2\x82\"", "\"\xC3\x28\"",
                            "\"\xF5\x80\x80\x80\"", "\"\xF0\x80\x80\x80\"", "\"\xF0\x9F\x8C\""})
        CHECK(refused(bad, 1, 2, "UTF-8"));
    // The mark some editors put first is passed over; anywhere else it is not.
    const tsuchi::Json* marked = read("\xEF\xBB\xBF{\"a\": 1}");
    CHECK(marked && marked->find("a")->integer == 1);
    tsuchi::release_json(marked);
    CHECK(refused("1\xEF\xBB\xBF", 1, 2, "follows"));
    // A text is as long as it is said to be, whatever follows it in memory.
    const char longer[] = "[1, 2] and more";
    const tsuchi::Json* cut = tsuchi::read_json(longer, 6);
    CHECK(cut && cut->count == 2);
    tsuchi::release_json(cut);
    CHECK(!tsuchi::read_json(longer, 5) && !tsuchi::read_json("true", 3) && !tsuchi::read_json("nullify", 3) && !tsuchi::read_json("\"\\u00e", 6) && blocks_out.empty());
}

void starving() {
    // No memory at any point: nothing is read, nothing is left behind, and the reason says so.
    const std::string text = R"({"list": [1, 2, 3, "four"], "name": ")" + std::string(9000, 'n') + "\"}";
    int needed = 0;
    for (;; ++needed) {
        allocations_left = needed;
        tsuchi::JsonError error{};
        const tsuchi::Json* root = read(text, &error);
        allocations_left = -1;
        if (root) {
            CHECK(!error.allocation_failure && !error.what && !error.line && !error.column);
            tsuchi::release_json(root);
            break;
        }
        CHECK(error.allocation_failure && error.what && std::strstr(error.what, "no memory") && blocks_out.empty());
        if (needed > 50) break;
    }
    CHECK(needed >= 3 && needed <= 50 && blocks_out.empty());
}

} // namespace

int main() {
    tsuchi::set_allocator({watched_allocate, watched_release});
    values();
    trees();
    refusals();
    starving();
    if (failures) std::printf("%d failed\n", failures);
    return failures;
}
