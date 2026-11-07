#include "object_retention.h"
#include "list.h"
#include "objects.h"
namespace tsuchi {
namespace {
struct Pin { uint64_t handle; uint32_t count; bool told; };
List<Pin> pins;
}
bool retain_game_object(uint64_t handle) {
    if (!handle) return true;
    const uint64_t object = game::from_handle(handle);
    if (!object) return false;
    for (uint32_t i = 0; i < pins.count; ++i) if (pins[i].handle == handle) {
        pins[i].told = game::keep_object(object, true) || pins[i].told;
        ++pins[i].count; return true;
    }
    Pin* added = pins.add(Pin{handle, 1, false});
    if (!added) return false;
    added->told = game::keep_object(object, true);
    if (!game::object_kept(object)) {
        pins.remove(added);
        if (!pins.count) pins.clear();
        return false;
    }
    return true;
}
void release_game_object(uint64_t handle) {
    for (uint32_t i = 0; handle && i < pins.count; ++i) if (pins[i].handle == handle) {
        if (--pins[i].count) return;
        if (pins[i].told) if (const uint64_t object = game::from_handle(handle)) game::keep_object(object, false);
        pins.remove(&pins[i]);
        if (!pins.count) pins.clear();
        return;
    }
}

} // namespace tsuchi
