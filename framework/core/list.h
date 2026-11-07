// A list of plain records that grows to whatever it has to hold.
#pragma once
#include <stdint.h>
#include <string.h>

#include "registry.h"

namespace tsuchi {

template <typename T>
struct List {
    T* items = nullptr;
    uint32_t count = 0, capacity = 0;

    T& operator[](uint32_t index) { return items[index]; }
    const T& operator[](uint32_t index) const { return items[index]; }

    // Makes room before a transaction publishes its records. Existing records stay intact.
    bool reserve(uint32_t need) {
        if (need <= capacity) return true;
        uint32_t next = capacity ? capacity : 16;
        while (next < need) {
            if (next > UINT32_MAX / 2) { next = need; break; }
            next *= 2;
        }
        auto* grown = static_cast<T*>(allocate(sizeof(T) * next));
        if (!grown) return false;
        if (items) memcpy(static_cast<void*>(grown), items, sizeof(T) * count);
        release(items);
        items = grown; capacity = next;
        return true;
    }

    // Adds a copy at the end and returns it there. Null when there is no memory for it.
    T* add(const T& item) {
        if (count == UINT32_MAX || !reserve(count + 1)) return nullptr;
        items[count] = item;
        return &items[count++];
    }

    // Takes one out. The last takes its place, so walk the list from its end while removing.
    void remove(T* item) { *item = items[--count]; }

    // Gives the room back.
    void clear() {
        release(items);
        items = nullptr;
        count = capacity = 0;
    }
};

} // namespace tsuchi
