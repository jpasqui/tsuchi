// The howlout dictionary assembler against vectors from its Python reference, all made-up data.
#include <stdio.h>
#include <string.h>

#include "howlout_dictionary.h"
#include "howlout_vectors.h"   // written by tests/test_howlout.py

using namespace tsuchi::howlout;

static int failures = 0;
#define CHECK(what) do { if (!(what)) { printf("FAILED line %d: %s\n", __LINE__, #what); ++failures; } } while (0)

int main() {
    static uint8_t out[1 << 16];
    uint64_t size = 0, mark = 0;

    CHECK(fingerprint(stock, sizeof stock, &mark) && mark == expected_fingerprint);
    CHECK(!fingerprint(stock, sizeof stock - 1, &mark));

    CHECK(assemble(stock, sizeof stock, rows, sizeof rows, phrases, phrase_count, out, sizeof out, &size) == Built::ok);
    CHECK(size == sizeof expected && !memcmp(out, expected, sizeof expected));

    // Too small a buffer says how much is needed and writes nothing.
    memset(out, 0xEE, sizeof out);
    size = 0;
    CHECK(assemble(stock, sizeof stock, rows, sizeof rows, phrases, phrase_count, out, sizeof expected - 1, &size) == Built::capacity);
    CHECK(size == sizeof expected && out[0] == 0xEE);

    // Rows trained for another network: one bit of the feature block differs.
    static uint8_t other[sizeof stock];
    memcpy(other, stock, sizeof stock);
    other[0x80 + 0x30] ^= 1;
    CHECK(assemble(other, sizeof other, rows, sizeof rows, phrases, phrase_count, out, sizeof out, &size) == Built::mismatch);

    // Files that are not what they should be.
    CHECK(assemble(stock, sizeof stock - 1, rows, sizeof rows, phrases, phrase_count, out, sizeof out, &size) == Built::stock);
    CHECK(assemble(stock, sizeof stock, rows, sizeof rows - 1, phrases, phrase_count, out, sizeof out, &size) == Built::rows);

    // A sound the rows have nothing for, and what a phrase and a dictionary can hold.
    const Phrase unknown[] = {{"K T", 300}};
    CHECK(assemble(stock, sizeof stock, rows, sizeof rows, unknown, 1, out, sizeof out, &size) == Built::sounds);
    const Phrase loud[] = {{"K AH0", 1001}};
    CHECK(assemble(stock, sizeof stock, rows, sizeof rows, loud, 1, out, sizeof out, &size) == Built::limits);
    CHECK(assemble(stock, sizeof stock, rows, sizeof rows, phrases, 0, out, sizeof out, &size) == Built::limits);
    static Phrase many[kMostPhrases + 1];
    for (Phrase& phrase : many) phrase = {"K AH0", 300};
    CHECK(assemble(stock, sizeof stock, rows, sizeof rows, many, kMostPhrases + 1, out, sizeof out, &size) == Built::limits);
    CHECK(assemble(stock, sizeof stock, rows, sizeof rows, many, kMostPhrases, out, sizeof out, &size) == Built::ok);
    static char longest[4 * 43 + 1];
    for (int i = 0; i < 43; ++i) memcpy(longest + 4 * i, i % 2 ? "AH0 " : "K   ", 4);
    const Phrase lengthy[] = {{longest, 300}};
    CHECK(assemble(stock, sizeof stock, rows, sizeof rows, lengthy, 1, out, sizeof out, &size) == Built::limits);
    longest[4 * 42] = 0;                                    // 42 sounds fit
    CHECK(assemble(stock, sizeof stock, rows, sizeof rows, lengthy, 1, out, sizeof out, &size) == Built::ok);

    if (!failures) printf("howlout dictionary: ok\n");
    return failures ? 1 : 0;
}
