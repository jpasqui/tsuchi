// Internal native RTPC marker that keeps a controlled bus in the mixing graph.
#pragma once
#include "game.h"
#include <float.h>

namespace tsuchi::audio_bus_binding {
namespace t = game::table;
constexpr uint32_t bus_volume = 5;
struct Point { float x, y; uint32_t interpolation; };
static_assert(sizeof(Point) == 12);
enum class Curve { absent, ours, foreign, unavailable };
class Bus {
    uint64_t node_ = 0;
public:
    explicit Bus(uint32_t id) {
        const uint64_t index = game::global<uint64_t>(t::data::wwise_index);
        if (index) node_ = t::fn::wwise_get_bus(index, id, 1);
    }
    ~Bus() { if (node_) t::fn::wwise_release_bus(node_); }
    explicit operator bool() const { return node_ != 0; }
    Curve curve(uint32_t id) const {
        if (!node_) return Curve::absent;
        const uint64_t manager = game::global<uint64_t>(t::data::wwise_parameter_manager);
        if (!manager) return Curve::unavailable;
        const uint64_t subscription = t::fn::wwise_bus_subscription(manager, node_ + t::wwise_bus_node::subscriber, bus_volume);
        if (!subscription) return Curve::absent;
        if (!game::readable(subscription, t::wwise_bus_subscription::count + 4)) return Curve::unavailable;
        const uint64_t curves = game::read_or<uint64_t>(subscription + t::wwise_bus_subscription::curves);
        const uint32_t count = game::read_or<uint32_t>(subscription + t::wwise_bus_subscription::count);
        if (count > 4096 || (count && (!curves || !game::readable(curves, uint64_t(count) * t::wwise_bus_curve::size)))) return Curve::unavailable;
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t entry = curves + uint64_t(i) * t::wwise_bus_curve::size;
            if (game::read_or<uint32_t>(entry + t::wwise_bus_curve::id) != id) continue;
            if (game::read_or<uint32_t>(entry + t::wwise_bus_curve::parameter) != id ||
                game::read_or<uint32_t>(entry + t::wwise_bus_curve::point_count) != 4 ||
                game::read_or<uint32_t>(entry + t::wwise_bus_curve::scaling) != 0) return Curve::foreign;
            const uint64_t address = game::read_or<uint64_t>(entry + t::wwise_bus_curve::points);
            if (!address || !game::readable(address, 4 * sizeof(Point))) return Curve::unavailable;
            Point points[4]{};
            if (!game::read(address, points)) return Curve::unavailable;
            const float expected_x[4]{-FLT_MAX, 0, 1, FLT_MAX};
            for (uint32_t p = 0; p < 4; ++p)
                if (points[p].x != expected_x[p] || points[p].y != 0 || points[p].interpolation != (p == 0 || p == 3 ? 9u : 4u)) return Curve::foreign;
            return Curve::ours;
        }
        return Curve::absent;
    }
    uint32_t add(uint32_t id) const {
        Point points[2]{{0, 0, 4}, {1, 0, 4}};
        // Game parameter, additive accumulation, BusVolume, no scaling. Native
        // SetRTPC copies the points and reevaluates the bus's mixing graph.
        return t::fn::wwise_bind_bus_curve(node_, id, 0, 2, bus_volume, id, 0, points, 2, true);
    }
    void remove(uint32_t id) const { t::fn::wwise_unbind_bus_curve(node_, bus_volume, id); }
};
}
