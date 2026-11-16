// The dictionary the howlout service gives its own instance of the game's recognizer: the game's
// network and silence row, and only the phrases mods added. Nothing here touches the game, so the
// host test builds a dictionary from made-up data.
#pragma once
#include <stdint.h>

namespace tsuchi {
namespace howlout {

constexpr uint32_t kMostPhrases = 50;   // the recognizer's own limit
constexpr uint32_t kMostStates = 127;   // in one phrase: 42 sounds of three states

struct Phrase {
    const char* sounds;    // ARPAbet separated by spaces, as "HH EH0 L OW1"; stress digits are optional
    uint32_t threshold;    // 0 to 1000; higher is easier to trigger
};

enum class Built {
    ok,
    stock,      // the game's dictionary is not one this code knows
    rows,       // the sound rows file is not one this code knows
    mismatch,   // the sound rows were trained for another dictionary's network
    sounds,     // a phrase names a sound the rows have nothing for
    limits,     // no phrase, too many, one too long, or a threshold above 1000
    capacity,   // `out` is too small; `size` says what is needed
};

// What trained rows have to match: FNV-1a over the layer sizes below the output, the feature
// block, and the hidden layers' weights and biases. False when `stock` is not a dictionary.
bool fingerprint(const uint8_t* stock, uint64_t size, uint64_t* out);

// What can be said of one phrase with the sound rows alone: ok, rows, sounds or limits.
Built check(const uint8_t* rows, uint64_t rows_size, const Phrase& phrase);

// Writes the dictionary for `phrases` into `out`. `stock` is the game's dictionary for the language
// and `rows` the sound rows file for it. Entry i gets the ID i and the name "H" and three digits.
Built assemble(const uint8_t* stock, uint64_t stock_size, const uint8_t* rows, uint64_t rows_size,
               const Phrase* phrases, uint32_t count, uint8_t* out, uint64_t capacity, uint64_t* size);

} // namespace howlout
} // namespace tsuchi
