// A clock for what the framework's own work costs in a step, in microseconds. The module sets it to the
// system's tick counter as it starts; on a PC, where the framework is checked, there is none and it reads 0.
#pragma once
#include <stdint.h>

namespace tsuchi {

inline uint64_t (*cost_clock)() = nullptr;
inline uint64_t cost_now() { return cost_clock ? cost_clock() : 0; }

// The time a piece of work took, added to a total and kept as the longest so far.
struct CostTotal {
    uint32_t runs, total_us, longest_us;
    void add(uint64_t began) {
        const uint64_t took = cost_now() - began;
        ++runs, total_us += uint32_t(took);
        if (took > longest_us) longest_us = uint32_t(took);
    }
};

} // namespace tsuchi
