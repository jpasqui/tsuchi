#include "objects.h"

#include "game.h"
#include "registry.h"

namespace tsuchi::game {
namespace {

namespace t = table;

// A handle is [index + 1 : 24 bits][life : 16 bits][serial : 24 bits].
constexpr int kIndexShift = 40, kLifeShift = 24;
constexpr uint64_t kSerialMask = 0xFFFFFF, kLifeMask = 0xFFFF;
static_assert(t::most::max_objects < (1u << 24) - 1, "an object index must fit in a handle");
constexpr int kMaxClassDepth = 64;

// How many times each object has been reused, by its index. Grows as needed.
uint16_t* lives = nullptr;
uint32_t lives_capacity = 0;

int32_t object_count() {
    const int32_t count = global<int32_t>(t::data::object_count);
    return count < 0 || uint64_t(count) > t::constant::max_objects ? 0 : count;
}

// The table entry for an index, or 0.
uint64_t item_of(int32_t index) {
    if (index < 0 || index >= object_count()) return 0;
    const uint64_t chunks = global<uint64_t>(t::data::object_chunks);
    const uint64_t chunk = read_or<uint64_t>(chunks + 8 * (uint64_t(index) / t::constant::objects_per_chunk));
    if (!chunk) return 0;
    const uint64_t item = chunk + t::object_item::size * (uint64_t(index) % t::constant::objects_per_chunk);
    return readable(item, t::object_item::size) ? item : 0;
}

bool reachable(uint64_t item) { return !(read_or<uint32_t>(item + t::object_item::flags) & t::constant::unreachable_flags); }

int32_t index_of(uint64_t object) { return read_or<int32_t>(object + t::object::index, -1); }

// The entry's serial number. The game gives an object one the first time
// something refers to it weakly; this does the same when it has none yet, so
// the number never changes under a handle.
int32_t serial_of(uint64_t item) {
    const uint64_t serial_at = item + t::object_item::serial;
    const uint64_t counter_at = main_base() + t::data::object_serial_counter;
    auto* serial = reinterpret_cast<int32_t*>(serial_at);
    int32_t current = __atomic_load_n(serial, __ATOMIC_ACQUIRE);
    if (current) return current;
    if (!writable(serial_at, 4) || !writable(counter_at, 4)) return 0;
    const int32_t fresh = __atomic_add_fetch(reinterpret_cast<int32_t*>(counter_at), 1, __ATOMIC_SEQ_CST);
    if (__atomic_compare_exchange_n(serial, &current, fresh, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return fresh;
    return current; // someone else numbered it first
}

uint16_t life_of(int32_t index) { return uint32_t(index) < lives_capacity ? lives[index] : 0; }

uint64_t super_of(uint64_t type) { return read_or<uint64_t>(type + t::struct_type::super); }

// The class every class is an instance of: the one object that is its own class.
uint64_t class_root() {
    static uint64_t root = 0, handle = 0;
    if (root && from_handle(handle) == root) return root;
    root = 0;
    each_object(
        [](void*, uint64_t object) {
            if (class_of(object) != object) return true;
            root = object;
            return false;
        },
        nullptr);
    handle = to_handle(root);
    return root;
}

} // namespace

bool is_class(uint64_t object) {
    const uint64_t root = class_root();
    uint64_t type = class_of(object);
    for (int depth = 0; root && type && depth < kMaxClassDepth; ++depth) {
        if (type == root) return true;
        type = super_of(type);
    }
    return false;
}

bool live(uint64_t object) {
    if (!object || !readable(object, t::object::size)) return false;
    const uint64_t item = item_of(index_of(object));
    return item && read_or<uint64_t>(item + t::object_item::object) == object && reachable(item);
}

uint64_t class_of(uint64_t object) { return read_or<uint64_t>(object + t::object::class_); }
uint64_t outer_of(uint64_t object) { return read_or<uint64_t>(object + t::object::outer); }
Name name_of(uint64_t object) { return read_or<Name>(object + t::object::name); }

void each_object(bool (*visit)(void*, uint64_t), void* context) {
    const int32_t count = object_count();
    for (int32_t index = 0; index < count; ++index) {
        const uint64_t item = item_of(index);
        if (!item || !reachable(item)) continue;
        const uint64_t object = read_or<uint64_t>(item + t::object_item::object);
        if (object && !visit(context, object)) return;
    }
}

bool each_object_from(int32_t& next, bool (*visit)(void*, uint64_t), void* context) {
    const int32_t count = object_count();
    if (next < 0) next = 0;
    while (next < count) {
        const uint64_t item = item_of(next++);
        if (!item || !reachable(item)) continue;
        const uint64_t object = read_or<uint64_t>(item + t::object_item::object);
        if (object && !visit(context, object)) return next >= count;
    }
    return true;
}

uint64_t to_handle(uint64_t object) {
    if (!live(object)) return 0;
    const int32_t index = index_of(object);
    const int32_t serial = serial_of(item_of(index));
    if (!serial) return 0;
    return (uint64_t(index) + 1) << kIndexShift | uint64_t(life_of(index)) << kLifeShift | (uint64_t(serial) & kSerialMask);
}

uint64_t from_handle(uint64_t handle) {
    const uint64_t slot = handle >> kIndexShift;
    if (!slot) return 0;
    const int32_t index = int32_t(slot - 1);
    const uint64_t item = item_of(index);
    if (!item || !reachable(item)) return 0;
    const uint64_t object = read_or<uint64_t>(item + t::object_item::object);
    if (!live(object)) return 0;
    const uint64_t serial = uint64_t(read_or<int32_t>(item + t::object_item::serial));
    if (!serial || (serial & kSerialMask) != (handle & kSerialMask)) return 0;
    return ((handle >> kLifeShift) & kLifeMask) == life_of(index) ? object : 0;
}

void end_life(uint64_t object) {
    if (!live(object)) return;
    const uint32_t index = uint32_t(index_of(object));
    if (index >= lives_capacity) {
        uint32_t next = lives_capacity ? lives_capacity : 1024;
        while (next <= index) next *= 2;
        auto* grown = static_cast<uint16_t*>(allocate(sizeof(uint16_t) * next));
        if (!grown) return;
        if (lives) memcpy(grown, lives, sizeof(uint16_t) * lives_capacity);
        memset(grown + lives_capacity, 0, sizeof(uint16_t) * (next - lives_capacity));
        if (lives) release(lives);
        lives = grown;
        lives_capacity = next;
    }
    ++lives[index];
}

uint64_t from_weak_reference(uint64_t at) {
    return from_weak_value(uint64_t(uint32_t(read_or<int32_t>(at, -1))) | uint64_t(uint32_t(read_or<int32_t>(at + 4))) << 32);
}

uint64_t from_weak_value(uint64_t packed) {
    const int32_t index = int32_t(uint32_t(packed)), serial = int32_t(uint32_t(packed >> 32));
    const uint64_t item = index >= 0 && serial ? item_of(index) : 0;
    if (!item || read_or<int32_t>(item + t::object_item::serial) != serial) return 0;
    const uint64_t object = read_or<uint64_t>(item + t::object_item::object);
    return live(object) ? object : 0;
}

TSUCHI_Result object_path(uint64_t object, char* buffer, uint64_t capacity, uint64_t* needed) {
    uint32_t depth = 0;
    for (uint64_t at = object; at; at = outer_of(at))
        if (++depth > 4096) return TSUCHI_NOT_FOUND; // outers that lead back into themselves
    for (int pass = 0; pass < 2; ++pass) {
        uint64_t used = 0;
        for (uint32_t level = depth; level--;) { // outermost first
            uint64_t at = object;
            for (uint32_t up = 0; up < level; ++up) at = outer_of(at);
            static char part[kNameBytes];
            if (!name_text(name_of(at), part, sizeof part)) return TSUCHI_NOT_FOUND;
            const uint64_t length = strlen(part), joined = level + 1 < depth ? 1 : 0;
            if (pass) {
                if (joined) buffer[used] = depth - level == 3 ? ':' : '.';
                memcpy(buffer + used + joined, part, length);
            }
            used += joined + length;
        }
        if (pass) {
            buffer[used] = 0;
            break;
        }
        *needed = used + 1;
        if (used + 1 > capacity) return TSUCHI_CAPACITY;
    }
    return TSUCHI_OK;
}

bool keep_object(uint64_t object, bool keep) {
    const uint64_t item = live(object) ? item_of(index_of(object)) : 0;
    const uint64_t at = item + t::object_item::flags;
    if (!item || !writable(at, 4)) return false;
    // The engine's own threads change these flags too.
    auto* flags = reinterpret_cast<uint32_t*>(at);
    const uint32_t bit = uint32_t(t::constant::kept_flag);
    const uint32_t before = keep ? __atomic_fetch_or(flags, bit, __ATOMIC_SEQ_CST) : __atomic_fetch_and(flags, ~bit, __ATOMIC_SEQ_CST);
    return ((before & bit) != 0) != keep;
}

bool object_kept(uint64_t object) {
    const uint64_t item = live(object) ? item_of(index_of(object)) : 0;
    return item && (read_or<uint32_t>(item + t::object_item::flags) & t::constant::kept_flag);
}

uint64_t to_weak_value(uint64_t object) {
    const int32_t index = live(object) ? index_of(object) : -1;
    const uint64_t item = index >= 0 ? item_of(index) : 0;
    const int32_t serial = item ? serial_of(item) : 0;
    return serial ? uint64_t(uint32_t(index)) | uint64_t(uint32_t(serial)) << 32 : 0xFFFFFFFFull;
}

uint64_t find_class(ClassRef& ref) {
    if (ref.found && from_handle(ref.handle) == ref.found) return ref.found;
    ref.found = ref.handle = 0;
    each_object(
        [](void* context, uint64_t object) {
            auto& wanted = *static_cast<ClassRef*>(context);
            if (!name_is(name_of(object), wanted.name) || !is_class(object)) return true;
            wanted.found = object;
            return false;
        },
        &ref);
    ref.handle = to_handle(ref.found);
    return ref.handle ? ref.found : (ref.found = 0);
}

bool is_a(uint64_t object, ClassRef& ref) {
    const uint64_t wanted = find_class(ref);
    if (!wanted || !live(object)) return false;
    uint64_t type = class_of(object);
    for (int depth = 0; type && depth < kMaxClassDepth; ++depth) {
        if (type == wanted) return true;
        type = super_of(type);
    }
    return false;
}

uint64_t member_object(uint64_t object, const char* name) {
    if (!live(object)) return 0;
    uint64_t type = class_of(object);
    for (int depth = 0; type && depth < kMaxClassDepth; ++depth, type = super_of(type)) {
        uint64_t field = read_or<uint64_t>(type + t::struct_type::properties);
        for (uint32_t count = 0; field && count < 4096; ++count, field = read_or<uint64_t>(field + t::property::next)) {
            if (!name_is(read_or<Name>(field + t::property::name), name)) continue;
            const uint64_t kind = read_or<uint64_t>(field + t::property::kind);
            if (!kind || !name_is(read_or<Name>(kind), "ObjectProperty")) return 0;
            return read_or<uint64_t>(object + read_or<int32_t>(field + t::property::offset));
        }
    }
    return 0;
}

} // namespace tsuchi::game
