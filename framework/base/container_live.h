// Private IoStore retirement. Reader/file ownership survives outstanding IO; its
// immutable lookup table is detached under the engine lock and can be restored.
#pragma once
#include <stdint.h>
#include <tsuchi/core.h>
namespace tsuchi {
struct ContainerLookup {
    uint64_t package, expected, replacement, slot = 0;
};
struct ContainerReaderState {
    uint64_t reader = 0;
    alignas(8) uint8_t toc[128]{};
    bool detached = false;
};
// Caller drains package loading and checks affected package objects immediately
// before entering; this function invokes no game callback before taking locks.
TSUCHI_Result change_container_reader(uint64_t container, uint64_t package_store,
    ContainerReaderState&, bool enabled, ContainerLookup*, uint32_t count);
}
