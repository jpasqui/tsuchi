#include "native_routes.h"

#include "game.h"

namespace tsuchi {
namespace {

namespace t = game::table;
constexpr unsigned kRoutes = t::native_route_count ? t::native_route_count : 1;
NativeHandler handlers[kRoutes] = {};
uint32_t calls[kRoutes] = {}; // since the last sample, on any thread
NativeDispatch dynamic_dispatch = nullptr;

} // namespace

bool native_route(unsigned route, NativeHandler handler) {
    if (route >= t::native_route_count) return false;
    handlers[route] = handler;
    return true;
}

void set_native_dispatch(NativeDispatch dispatcher) { dynamic_dispatch = dispatcher; }

uint32_t native_route_sample(unsigned route) {
    return route < t::native_route_count ? __atomic_exchange_n(&calls[route], 0u, __ATOMIC_RELAXED) : 0;
}

} // namespace tsuchi

extern "C" void tsuchi_native_dispatch(tsuchi::NativeFrame* frame, uint64_t returned) {
    using namespace tsuchi;
    const uint64_t main = game::main_base();
    for (unsigned route = 0; route < game::table::native_route_count; ++route)
        if (main + game::table::native_routes[route].returned == returned) {
            __atomic_fetch_add(&calls[route], 1u, __ATOMIC_RELAXED);
            if (handlers[route]) handlers[route](*frame);
            return;
        }
    if (dynamic_dispatch) dynamic_dispatch(frame, returned);
}
