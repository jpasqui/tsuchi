// See howlout_dictionary.h. The layouts are the recognizer's dictionary and the sound rows file.
//
// Dictionary: a 0x80-byte header (phrase count at 0x50, the offsets of the entry table and the
// matrix at 0x54 and 0x58), then the model: its size at +4, the phrase count at +0xC, the layer
// count at +0x14, the output scale at +0x16, the feature count at +0x18, eight layer sizes at
// +0x20, the feature block (68 bytes a feature) at +0x30, every layer's weights as signed bytes,
// every layer's biases as 32-bit numbers, and a 268-byte record for each phrase. After the model:
// a 20-byte entry for each phrase (ID, threshold, name) and a matrix of 16-bit flags.
//
// Sound rows file: see the table in read_rows.
#include "howlout_dictionary.h"

#include <string.h>

namespace tsuchi {
namespace howlout {
namespace {

constexpr uint64_t kHeader = 0x80, kModelHead = 0x30, kPhraseBytes = 268, kEntryBytes = 20;
constexpr uint32_t kStateSlots = 128;

uint16_t u16(const uint8_t* at) { return uint16_t(at[0] | at[1] << 8); }
uint32_t u32(const uint8_t* at) { return uint32_t(u16(at)) | uint32_t(u16(at + 2)) << 16; }
uint64_t u64(const uint8_t* at) { return uint64_t(u32(at)) | uint64_t(u32(at + 4)) << 32; }
void put16(uint8_t* at, uint32_t value) { at[0] = uint8_t(value); at[1] = uint8_t(value >> 8); }
void put32(uint8_t* at, uint32_t value) { put16(at, value); put16(at + 2, value >> 16); }

struct Stock {
    uint32_t layers = 0, inputs = 0;                // `inputs` to the output layer
    uint16_t sizes[8] = {};
    uint64_t weights = 0, hidden_weights = 0;       // where the weights begin, and the bytes below the output layer
    uint64_t biases = 0, hidden_biases = 0;
    uint64_t phrases = 0;                           // where the phrase records begin
};

bool read_stock(const uint8_t* data, uint64_t size, Stock& stock) {
    if (!data || size < kHeader + kModelHead) return false;
    const uint64_t count = u32(data + 0x50), table = u32(data + 0x54), matrix = u32(data + 0x58);
    if (u32(data + 0x5C) != 0x1257BD83 || u32(data + 0x80) != 0x0C08C83C || u32(data + 0x90) != 0x0C02BD3F) return false;
    if (count < 1 || count > kMostPhrases || u16(data + 0x8C) != count) return false;
    if (table != kHeader + u32(data + 0x84) || matrix != table + count * kEntryBytes || size != matrix + count * count * 2) return false;
    stock.layers = u16(data + 0x94);
    const uint64_t features = u16(data + 0x98);
    if (stock.layers < 2 || stock.layers > 8 || features < 1) return false;
    uint64_t weights = 0, biases = 0;
    for (uint32_t i = 0; i < stock.layers; ++i) {
        stock.sizes[i] = u16(data + 0xA0 + 2 * i);
        if (!stock.sizes[i]) return false;
        if (i) {
            if (i + 1 == stock.layers) { stock.hidden_weights = weights; stock.hidden_biases = biases; }
            weights += uint64_t(stock.sizes[i - 1]) * stock.sizes[i];
            biases += 4 * uint64_t(stock.sizes[i]);
        }
    }
    stock.inputs = stock.sizes[stock.layers - 2];
    stock.weights = kHeader + kModelHead + features * 68;
    stock.biases = stock.weights + weights;
    stock.phrases = stock.biases + biases;
    return stock.phrases + count * kPhraseBytes == table;
}

uint64_t fnv(uint64_t value, const uint8_t* data, uint64_t size) {
    for (uint64_t i = 0; i < size; ++i) value = (value ^ data[i]) * 0x100000001B3ull;
    return value;
}

uint64_t fingerprint_of(const uint8_t* data, const Stock& stock) {
    uint64_t value = 0xCBF29CE484222325ull;
    value = fnv(value, data + 0xA0, 2 * uint64_t(stock.layers - 1));
    value = fnv(value, data + kHeader + kModelHead, stock.weights + stock.hidden_weights - kHeader - kModelHead);
    return fnv(value, data + stock.biases, stock.hidden_biases);
}

struct Rows {
    uint32_t inputs = 0, scale = 0, phones = 0, classes = 0, aliases = 0;
    const uint8_t *model = nullptr, *names = nullptr, *group = nullptr, *table = nullptr, *alias = nullptr;
    const uint8_t *weights = nullptr, *biases = nullptr;
    uint64_t mark = 0;
};

// 0x00 "HWLR", version 1 | 0x08 the model ID, 32 bytes | 0x28 the fingerprint | 0x30 inputs a row |
// 0x32 the output scale | 0x34 phones P | 0x36 neighbor groups | 0x38 classes C | 0x3C aliases A |
// 0x40 P names of 4 bytes | P+1 groups, the last for silence | C classes: phone, state, kind, value |
// A aliases: phone, state, class | C rows of weights | C biases.
// Kinds: 0 the plain state, 1 a first state after neighbor `value`, 2 after any neighbor of group
// `value`, 3 a last state before neighbor `value`, 4 before any neighbor of group `value`.
bool read_rows(const uint8_t* data, uint64_t size, Rows& rows) {
    if (!data || size < 0x40 || memcmp(data, "HWLR", 4) || u32(data + 4) != 1) return false;
    rows.model = data + 0x08;
    rows.mark = u64(data + 0x28);
    rows.inputs = u16(data + 0x30);
    rows.scale = u16(data + 0x32);
    rows.phones = u16(data + 0x34);
    rows.classes = u32(data + 0x38);
    rows.aliases = u32(data + 0x3C);
    if (!rows.inputs || !rows.phones || rows.phones > 254 || !rows.classes || rows.classes > 0xFFFF || rows.aliases > 0xFFFF) return false;
    uint64_t at = 0x40;
    rows.names = data + at; at += 4 * uint64_t(rows.phones);
    rows.group = data + at; at += rows.phones + 1;
    rows.table = data + at; at += 4 * uint64_t(rows.classes);
    rows.alias = data + at; at += 4 * uint64_t(rows.aliases);
    rows.weights = data + at; at += uint64_t(rows.classes) * rows.inputs;
    rows.biases = data + at; at += 4 * uint64_t(rows.classes);
    return at == size;
}

int find_class(const Rows& rows, uint32_t phone, uint32_t state, uint32_t kind, uint32_t value) {
    for (uint32_t i = 0; i < rows.classes; ++i) {
        const uint8_t* entry = rows.table + 4 * i;
        if (entry[0] == phone && entry[1] == state && entry[2] == kind && entry[3] == value) return int(i);
    }
    return -1;
}

// A state's class: by its neighbor, then by the neighbor's group, then the plain state, then the alias.
int class_of(const Rows& rows, uint32_t phone, uint32_t state, uint32_t left, uint32_t right) {
    int found = -1;
    if (state == 0) {
        found = find_class(rows, phone, state, 1, left);
        if (found < 0) found = find_class(rows, phone, state, 2, rows.group[left]);
    } else if (state == 2) {
        found = find_class(rows, phone, state, 3, right);
        if (found < 0) found = find_class(rows, phone, state, 4, rows.group[right]);
    }
    if (found < 0) found = find_class(rows, phone, state, 0, 0);
    for (uint32_t i = 0; found < 0 && i < rows.aliases; ++i) {
        const uint8_t* entry = rows.alias + 4 * i;
        if (entry[0] == phone && entry[1] == state && u16(entry + 2) < rows.classes) found = u16(entry + 2);
    }
    return found;
}

// A sound's index. Stress digits are dropped, except that AH0 is the sound AX.
int phone_of(const Rows& rows, const char* text, uint32_t length) {
    char name[4] = {};
    if (length == 3 && !memcmp(text, "AH0", 3)) {
        name[0] = 'A'; name[1] = 'X';
    } else {
        while (length && text[length - 1] >= '0' && text[length - 1] <= '2') --length;
        if (!length || length > 4) return -1;
        memcpy(name, text, length);
    }
    for (uint32_t i = 0; i < rows.phones; ++i)
        if (!memcmp(rows.names + 4 * i, name, 4)) return int(i);
    return -1;
}

bool blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// The classes of a phrase's states, three a sound, with silence on both sides. 0 when a sound is unknown.
uint32_t states_of(const Rows& rows, const char* sounds, uint16_t* states, bool* too_long) {
    uint8_t phones[kMostStates / 3];
    uint32_t count = 0;
    for (const char* at = sounds; *at;) {
        while (blank(*at)) ++at;
        const char* start = at;
        while (*at && !blank(*at)) ++at;
        if (at == start) break;
        const int phone = phone_of(rows, start, uint32_t(at - start));
        if (phone < 0) return 0;
        if (count == kMostStates / 3) { *too_long = true; return 0; }
        phones[count++] = uint8_t(phone);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t left = i ? phones[i - 1] : rows.phones, right = i + 1 < count ? phones[i + 1] : rows.phones;
        for (uint32_t state = 0; state < 3; ++state) {
            const int found = class_of(rows, phones[i], state, left, right);
            if (found < 0) return 0;
            states[3 * i + state] = uint16_t(found);
        }
    }
    return 3 * count;
}

} // namespace

bool fingerprint(const uint8_t* stock, uint64_t size, uint64_t* out) {
    Stock read;
    if (!read_stock(stock, size, read)) return false;
    *out = fingerprint_of(stock, read);
    return true;
}

Built check(const uint8_t* rows_data, uint64_t rows_size, const Phrase& phrase) {
    Rows rows;
    if (!read_rows(rows_data, rows_size, rows)) return Built::rows;
    if (phrase.threshold > 1000 || !phrase.sounds) return Built::limits;
    static uint16_t states[kMostStates];
    bool too_long = false;
    if (states_of(rows, phrase.sounds, states, &too_long)) return Built::ok;
    return too_long ? Built::limits : Built::sounds;
}

Built assemble(const uint8_t* stock_data, uint64_t stock_size, const uint8_t* rows_data, uint64_t rows_size,
               const Phrase* phrases, uint32_t count, uint8_t* out, uint64_t capacity, uint64_t* size) {
    Stock stock;
    Rows rows;
    if (!read_stock(stock_data, stock_size, stock)) return Built::stock;
    if (!read_rows(rows_data, rows_size, rows)) return Built::rows;
    if (memcmp(rows.model, stock_data + 0x28, 32) || rows.mark != fingerprint_of(stock_data, stock) ||
        rows.scale != u16(stock_data + 0x96) || rows.inputs != stock.inputs) return Built::mismatch;
    if (!phrases || count < 1 || count > kMostPhrases) return Built::limits;

    // Every phrase's states as classes, then the classes in use, in order: they follow the silence row.
    static uint16_t states[kMostPhrases][kMostStates], used[kMostPhrases * kMostStates];
    uint32_t lengths[kMostPhrases], used_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        bool too_long = false;
        if (phrases[i].threshold > 1000 || !phrases[i].sounds) return Built::limits;
        lengths[i] = states_of(rows, phrases[i].sounds, states[i], &too_long);
        if (too_long) return Built::limits;
        if (!lengths[i]) return Built::sounds;
        for (uint32_t s = 0; s < lengths[i]; ++s) {
            uint32_t at = 0;
            while (at < used_count && used[at] < states[i][s]) ++at;
            if (at < used_count && used[at] == states[i][s]) continue;
            memmove(used + at + 1, used + at, (used_count - at) * sizeof used[0]);
            used[at] = states[i][s];
            ++used_count;
        }
    }
    const uint64_t outputs = 1 + uint64_t(used_count);
    if (outputs > 32767) return Built::limits;

    const uint64_t head = stock.weights + stock.hidden_weights;                 // the header, the features, the hidden weights
    const uint64_t biases = head + outputs * stock.inputs;
    const uint64_t records = biases + stock.hidden_biases + 4 * outputs;
    const uint64_t table = records + uint64_t(count) * kPhraseBytes;
    const uint64_t matrix = table + uint64_t(count) * kEntryBytes;
    *size = matrix + uint64_t(count) * count * 2;
    if (!out || capacity < *size) return Built::capacity;

    memcpy(out, stock_data, head);
    put32(out + 0x50, count);
    put32(out + 0x54, uint32_t(table));
    put32(out + 0x58, uint32_t(matrix));
    put32(out + 0x84, uint32_t(table - kHeader));
    put16(out + 0x8C, count);
    put16(out + 0xA0 + 2 * (stock.layers - 1), uint32_t(outputs));

    uint8_t* at = out + head;
    memcpy(at, stock_data + head, stock.inputs);                                // the game's silence row
    at += stock.inputs;
    for (uint32_t i = 0; i < used_count; ++i, at += stock.inputs) memcpy(at, rows.weights + uint64_t(used[i]) * rows.inputs, stock.inputs);
    memcpy(at, stock_data + stock.biases, stock.hidden_biases + 4);             // the hidden biases, then silence's
    at += stock.hidden_biases + 4;
    for (uint32_t i = 0; i < used_count; ++i, at += 4) memcpy(at, rows.biases + 4 * uint64_t(used[i]), 4);

    for (uint32_t i = 0; i < count; ++i, at += kPhraseBytes) {
        memcpy(at, stock_data + stock.phrases, 12);                             // the six fields of the game's first phrase
        put16(at + 8, lengths[i]);                                              // the fifth is the state count
        for (uint32_t s = 0; s < kStateSlots; ++s) {
            uint32_t value = 0xFFFF;                                            // an unused slot
            for (uint32_t u = 0; s < lengths[i] && u < used_count; ++u)
                if (used[u] == states[i][s]) value = 1 + u;
            put16(at + 12 + 2 * s, value);
        }
    }
    for (uint32_t i = 0; i < count; ++i, at += kEntryBytes) {
        memset(at, 0, kEntryBytes);
        put32(at, i);
        put32(at + 4, phrases[i].threshold);
        at[10] = 'H';
        at[11] = char('0' + i / 100 % 10); at[12] = char('0' + i / 10 % 10); at[13] = char('0' + i % 10);
    }
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < count; ++j, at += 2) put16(at, i == j);
    return Built::ok;
}

} // namespace howlout
} // namespace tsuchi
