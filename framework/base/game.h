// Reaching the game's memory. Everything that reads or writes the game goes
// through here, and takes every address and offset from game_table.h.
//
// Four functions differ between the real game and a test, so they are only
// declared here: the game build defines them one way, a test another.
#pragma once
#include <stdint.h>
#include <string.h>

#include "game_table.h"

namespace tsuchi::game {

uint64_t main_base();                             // where the game's code starts
bool readable(uint64_t address, uint64_t bytes);  // mapped and readable right now
bool writable(uint64_t address, uint64_t bytes);  // mapped and writable right now
bool executable(uint64_t address);                // in mapped code

// Copies a value out of the game. False, with `out` untouched, when it is not readable.
template <class T>
bool read(uint64_t address, T& out) {
    if (!readable(address, sizeof(T))) return false;
    memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
    return true;
}

// The same for a size only known as the framework runs, as a structure's in a buffer sized by `most`.
inline bool read_bytes(uint64_t address, void* out, uint64_t bytes) {
    if (!readable(address, bytes)) return false;
    memcpy(out, reinterpret_cast<const void*>(address), bytes);
    return true;
}
inline bool write_bytes(uint64_t address, const void* value, uint64_t bytes) {
    if (!writable(address, bytes)) return false;
    memcpy(reinterpret_cast<void*>(address), value, bytes);
    return true;
}

// For chains of reads where a missing value and a zero are handled the same way.
template <class T>
T read_or(uint64_t address, T fallback = T{}) {
    T value;
    return read(address, value) ? value : fallback;
}

template <class T>
bool write(uint64_t address, const T& value) {
    if (!writable(address, sizeof(T))) return false;
    memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
    return true;
}
// A number of the table says what to write only once it has a width: write a uint32_t(t::...) and so on.
template <class T>
bool write(uint64_t address, const table::Cell<T>& value) = delete;

// A function an object is reached through, from the object's own function table: the entry the game table
// names, as the function the table says is there. Null when the entry cannot be read or does not lead to code.
template <class F>
F method(uint64_t object, table::Entry<F> entry) {
    const uint64_t functions = read_or<uint64_t>(object);
    const uint64_t function = functions ? read_or<uint64_t>(functions + entry.offset) : 0;
    return function && executable(function) ? reinterpret_cast<F>(function) : nullptr;
}

// A global of the game, by its address in the table.
template <class T>
T global(uint64_t table_address, T fallback = T{}) {
    return read_or<T>(main_base() + table_address, fallback);
}

// An indirect function stored in a global, or null when it cannot be read as code.
template <class F>
F global_function(table::DataFunction<F> entry) {
    const uint64_t function = global<uint64_t>(uint64_t(entry.address));
    return function && executable(function) ? reinterpret_cast<F>(function) : nullptr;
}

} // namespace tsuchi::game
