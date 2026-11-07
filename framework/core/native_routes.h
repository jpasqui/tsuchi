// Native routes: places inside a game function where the patch hands the framework every register, so a
// handler can read and change what the function is working with (tools/gen/stubs.py). A route belongs to the
// framework itself. SDK inline hooks use separate owned dynamic gateways and share the register capture.
// Its handler runs on whatever thread runs the game's function.
#pragma once
#include <stdint.h>

namespace tsuchi {

// Every register at the route, in the order the entry code keeps them. A handler may change any of them but
// `sp` and `resume`; the game's function goes on with the rest.
struct NativeFrame {
    uint64_t x[31]; // x0 to x30
    uint64_t sp;
    uint8_t q[32][16];
    uint64_t fpcr, fpsr, nzcv;
    uint64_t resume; // where the route goes on
};
static_assert(sizeof(NativeFrame) == 0x320, "start.S lays the frame out");

using NativeHandler = void (*)(NativeFrame& frame);
// Puts `handler` on a route of the game table, by its number. False for a number past the table's.
bool native_route(unsigned route, NativeHandler handler);
// How many times the game passed a route since the last sample, handled or not; the count starts again. For a
// test's log.
uint32_t native_route_sample(unsigned route);

// Dynamic SDK gateways share this entry's register capture. Their dispatcher
// recognizes its own returned addresses, after the fixed routes are checked.
using NativeDispatch = void (*)(NativeFrame*, uint64_t returned);
void set_native_dispatch(NativeDispatch dispatcher);

} // namespace tsuchi

// Called by the entry code with the frame and the route's `returned` address.
extern "C" void tsuchi_native_dispatch(tsuchi::NativeFrame* frame, uint64_t returned);
