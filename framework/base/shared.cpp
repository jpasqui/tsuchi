#include "shared.h"

namespace tsuchi::game {

namespace t = table;

void drop_shared(uint64_t counter) {
    namespace c = t::shared_count;
    if (!counter || !writable(counter, c::size)) return;
    if (__atomic_sub_fetch(reinterpret_cast<int32_t*>(counter + c::holders), 1, __ATOMIC_SEQ_CST)) return;
    // Read both destruction functions before calling either.
    const auto destroy_object = method(counter, t::shared_count_table::destroy_object);
    const auto destroy_self = method(counter, t::shared_count_table::destroy_self);
    if (destroy_object) destroy_object(reinterpret_cast<void*>(counter));
    if (!__atomic_sub_fetch(reinterpret_cast<int32_t*>(counter + c::watchers), 1, __ATOMIC_SEQ_CST) && destroy_self) destroy_self(reinterpret_cast<void*>(counter));
}

} // namespace tsuchi::game
