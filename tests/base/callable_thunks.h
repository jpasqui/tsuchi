// Hook thunks a host test can really call. In the game a thunk is two
// instructions that jump wherever the framework says; here it is a function
// that asks the same question and calls the answer, passing six arguments
// through untouched. Most checks need whole-number or pointer arguments;
// an engine-update check selects a typed set that also passes its float.
//
// Include in exactly one source file of a test program.
#pragma once
#include <array>
#include <cstdint>
#include <utility>

#include "hooks.h"

namespace thunks {

using Function = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
constexpr unsigned kCount = 256;

// Host equivalent of the assembly return trampoline.
struct Dispatch {
    uint64_t stack;
    TsuchiHookDispatch selected;
    explicit Dispatch(uint32_t thunk) : stack(reinterpret_cast<uint64_t>(this)), selected(tsuchi_hook_enter(thunk, stack, 1)) {}
    ~Dispatch() {
        if (selected.tracked) tsuchi_hook_leave(stack);
    }
};

template <unsigned N>
uint64_t thunk(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    Dispatch dispatch(N);
    return reinterpret_cast<Function>(dispatch.selected.target)(a, b, c, d, e, f);
}
template <size_t... I>
constexpr std::array<Function, sizeof...(I)> make(std::index_sequence<I...>) {
    return {&thunk<I>...};
}
inline const std::array<Function, kCount> all = make(std::make_index_sequence<kCount>());

// The engine's update also takes a floating-point argument. Its original must keep that argument
// in the register the host ABI gives it, rather than sending it through an integer-only thunk.
using EngineTick = void (*)(void*, float, bool);
inline bool engine_ticks = false;
template <unsigned N>
void engine_tick(void* engine, float seconds, bool idle) {
    Dispatch dispatch(N);
    reinterpret_cast<EngineTick>(dispatch.selected.target)(engine, seconds, idle);
}
template <size_t... I>
constexpr std::array<EngineTick, sizeof...(I)> make_engine_ticks(std::index_sequence<I...>) {
    return {&engine_tick<I>...};
}
inline const std::array<EngineTick, kCount> all_engine_ticks = make_engine_ticks(std::make_index_sequence<kCount>());

// The AI ordering proof hooks its actual one-pointer native callee, independently of older generic hooks.
using AiEntry = void (*)(void*);
inline bool ai_entry = false;
template <unsigned N>
void ai_start(void* part) {
    Dispatch dispatch(N);
    reinterpret_cast<AiEntry>(dispatch.selected.target)(part);
}
template <size_t... I>
constexpr std::array<AiEntry, sizeof...(I)> make_ai_starts(std::index_sequence<I...>) {
    return {&ai_start<I>...};
}
inline const std::array<AiEntry, kCount> all_ai_starts = make_ai_starts(std::make_index_sequence<kCount>());

// Calls whatever a function-table entry or a hook's `original` leads to.
inline uint64_t call(uint64_t function, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0, uint64_t f = 0) {
    return reinterpret_cast<Function>(function)(a, b, c, d, e, f);
}

} // namespace thunks

namespace tsuchi {
uint32_t hook_thunk_count() { return thunks::kCount; }
uint64_t hook_thunk_address(uint32_t thunk) {
    return thunks::engine_ticks ? reinterpret_cast<uint64_t>(thunks::all_engine_ticks[thunk]) :
        thunks::ai_entry ? reinterpret_cast<uint64_t>(thunks::all_ai_starts[thunk]) : reinterpret_cast<uint64_t>(thunks::all[thunk]);
}
} // namespace tsuchi
