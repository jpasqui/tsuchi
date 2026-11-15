#include "reflect.h"

#include <string.h>

#include "game.h"
#include "names.h"
#include "objects.h"

namespace tsuchi::game {
namespace {

namespace t = table;
static_assert(t::most::max_array_count <= kMaxScriptArrayCount, "native fixtures may tighten the script array bound");

enum class PropertyKind : uint8_t {
    unknown, boolean, int8, int16, int32, int64, uint8, uint16, uint32, uint64, float32, float64,
    name, object, class_reference, weak_object, text, structure, array, enumeration, soft_object, soft_class, engine_text, other,
};
bool is_soft(PropertyKind kind) { return kind == PropertyKind::soft_object || kind == PropertyKind::soft_class; }
// A reference to an object, kept as the object itself or, weakly, as its number and serial.
bool is_reference(PropertyKind kind) { return kind == PropertyKind::object || kind == PropertyKind::class_reference || kind == PropertyKind::weak_object; }
uint64_t referred(PropertyKind kind, uint64_t at) { return kind == PropertyKind::weak_object ? from_weak_reference(at) : read_or<uint64_t>(at); }
struct KindName {
    const char* name;
    PropertyKind kind;
};
constexpr KindName kKinds[] = {
    {"BoolProperty", PropertyKind::boolean},   {"Int8Property", PropertyKind::int8},     {"Int16Property", PropertyKind::int16},
    {"IntProperty", PropertyKind::int32},      {"Int64Property", PropertyKind::int64},   {"ByteProperty", PropertyKind::uint8},
    {"UInt16Property", PropertyKind::uint16},  {"UInt32Property", PropertyKind::uint32}, {"UInt64Property", PropertyKind::uint64},
    {"FloatProperty", PropertyKind::float32},  {"DoubleProperty", PropertyKind::float64}, {"NameProperty", PropertyKind::name},
    {"ObjectProperty", PropertyKind::object},  {"ClassProperty", PropertyKind::class_reference},  {"StrProperty", PropertyKind::text},
    {"StructProperty", PropertyKind::structure}, {"ArrayProperty", PropertyKind::array}, {"EnumProperty", PropertyKind::enumeration},
    {"SoftObjectProperty", PropertyKind::soft_object}, {"SoftClassProperty", PropertyKind::soft_class},
    {"WeakObjectProperty", PropertyKind::weak_object}, {"TextProperty", PropertyKind::engine_text},
};

// A property's kind is the name of a small record it points at. The engine has
// a few dozen of those records, so the answer for each is remembered.
struct KnownKind {
    uint64_t record;
    PropertyKind kind;
};
KnownKind known_kinds[64];
unsigned known_kind_count = 0;

PropertyKind kind_of(uint64_t property) {
    const uint64_t record = read_or<uint64_t>(property + t::property::kind);
    if (!record) return PropertyKind::unknown;
    for (unsigned i = 0; i < known_kind_count; ++i)
        if (known_kinds[i].record == record) return known_kinds[i].kind;
    Name name;
    if (!read(record, name)) return PropertyKind::unknown;
    PropertyKind kind = PropertyKind::other;
    for (const KindName& known : kKinds)
        if (name_is(name, known.name)) kind = known.kind;
    if (known_kind_count < 64) known_kinds[known_kind_count++] = {record, kind};
    return kind;
}

int32_t count_of(uint64_t property) { return read_or<int32_t>(property + t::property::count); }
int32_t element_bytes(uint64_t property) { return read_or<int32_t>(property + t::property::element_bytes); }

bool same_ignoring_case(const char* a, const char* b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = char(x + 32);
        if (y >= 'A' && y <= 'Z') y = char(y + 32);
        if (x != y) return false;
        if (!x) return true;
    }
}

bool to_units(const char* text, char16_t* out, size_t capacity, size_t& count);

bool same_name(Name a, Name b) { return a.index == b.index && a.number == b.number; }

// The engine's own name for a field's, found without adding one. False when there is none or the engine's
// lookup isn't there; the field is then looked for by its text. The engine's names compare without case, as
// the text below does.
bool engine_name_of(const char* text, Name& out) {
    size_t units = 0;
    if (!text || !*text || !t::bound(t::fn::name_from_text) || !to_units(text, nullptr, 0, units) || units > t::constant::max_name_units)
        return false;
    char16_t wide[t::most::max_name_units + 1];
    to_units(text, wide, units, units);
    wide[units] = 0;
    const uint64_t found = t::fn::name_from_text(wide, units, 0);
    if (!found) return false;
    memcpy(&out, &found, sizeof out);
    return true;
}

// Fields found lately by their engine name. A script that writes the same few fields every step would
// otherwise walk every field of a large class, and its parents', each time: a character's animation blueprint
// took about 6 ms a write on Eden. An entry is used only while the struct it was found in still heads the same
// list of fields, is still the type or one of its parents, and the field still has that name.
struct FoundField {
    uint64_t type, owner, head, property;
    Name name;
};
constexpr unsigned kFoundFields = 64;
FoundField found_fields[kFoundFields];
unsigned next_found_field = 0;

uint64_t remembered_field(uint64_t type, Name name) {
    for (const FoundField& found : found_fields) {
        if (found.type != type || !found.property || !same_name(found.name, name)) continue;
        if (read_or<uint64_t>(found.owner + t::struct_type::properties) != found.head) return 0;
        if (!same_name(read_or<Name>(found.property + t::property::name), name)) return 0;
        uint64_t at = type;
        for (int depth = 0; at && depth < 64 && at != found.owner; ++depth) at = read_or<uint64_t>(at + t::struct_type::super);
        return at == found.owner ? found.property : 0;
    }
    return 0;
}

// The walk by engine name: two numbers compared for each field instead of its text.
uint64_t field_by_engine_name(uint64_t type, Name name) {
    if (const uint64_t known = remembered_field(type, name)) return known;
    const uint64_t first = type;
    for (int depth = 0; type && depth < 64; ++depth, type = read_or<uint64_t>(type + t::struct_type::super)) {
        const uint64_t head = read_or<uint64_t>(type + t::struct_type::properties);
        uint64_t property = head;
        for (int guard = 0; property && guard < 8192; ++guard, property = read_or<uint64_t>(property + t::property::next)) {
            if (!same_name(read_or<Name>(property + t::property::name), name)) continue;
            found_fields[next_found_field] = {first, type, head, property, name};
            next_found_field = (next_found_field + 1) % kFoundFields;
            return property;
        }
    }
    return 0;
}

// A field of a type or of one of its parents.
uint64_t find_field(uint64_t type, const char* name) {
    Name engine{};
    if (engine_name_of(name, engine))
        if (const uint64_t property = field_by_engine_name(type, engine)) return property;
    // By text: no engine name, or none of the fields has it (a field whose name the engine keeps apart).
    for (int depth = 0; type && depth < 64; ++depth, type = read_or<uint64_t>(type + t::struct_type::super)) {
        uint64_t property = read_or<uint64_t>(type + t::struct_type::properties);
        for (int guard = 0; property && guard < 8192; ++guard, property = read_or<uint64_t>(property + t::property::next)) {
            char text[kNameBytes];
            if (name_text(read_or<Name>(property + t::property::name), text, sizeof text) && same_ignoring_case(text, name)) return property;
        }
    }
    return 0;
}

// Reads "[123]" at path[i]. False when it is not a well-formed index.
bool read_index(const char* path, size_t& i, int64_t& index) {
    ++i;
    index = 0;
    int digits = 0;
    for (; path[i] >= '0' && path[i] <= '9'; ++i) {
        index = index * 10 + (path[i] - '0');
        if (++digits > 9) return false;
    }
    if (!digits || path[i] != ']') return false;
    ++i;
    return true;
}

bool is_integer(PropertyKind kind) { return kind >= PropertyKind::int8 && kind <= PropertyKind::uint64; }

// The kind an enumeration is stored as: the kind of the plain number inside it.
PropertyKind stored_kind(uint64_t property) {
    const PropertyKind kind = kind_of(property);
    if (kind != PropertyKind::enumeration) return kind;
    const uint64_t inner = read_or<uint64_t>(property + t::property::inner);
    const PropertyKind under = inner ? kind_of(inner) : PropertyKind::unknown;
    return is_integer(under) ? under : PropertyKind::other;
}

struct Writer {
    char* out;
    size_t capacity, count = 0;
    void put(char c) {
        if (count < capacity) out[count] = c;
        ++count;
    }
};

// Writes a string's 16-bit units as UTF-8 with a terminator.
void convert(uint64_t data, int32_t units, Writer& writer) {
    for (int32_t i = 0; i < units; ++i) {
        uint32_t c = read_or<uint16_t>(data + 2 * uint64_t(i));
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units) {
            const uint32_t low = read_or<uint16_t>(data + 2 * uint64_t(i + 1));
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (c < 0x80) {
            writer.put(char(c));
        } else if (c < 0x800) {
            writer.put(char(0xC0 | (c >> 6)));
            writer.put(char(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            writer.put(char(0xE0 | (c >> 12)));
            writer.put(char(0x80 | ((c >> 6) & 0x3F)));
            writer.put(char(0x80 | (c & 0x3F)));
        } else {
            writer.put(char(0xF0 | (c >> 18)));
            writer.put(char(0x80 | ((c >> 12) & 0x3F)));
            writer.put(char(0x80 | ((c >> 6) & 0x3F)));
            writer.put(char(0x80 | (c & 0x3F)));
        }
    }
    writer.put('\0');
}

ClassRef function_class{t::name::function_class};
ClassRef class_class{t::name::class_class};

} // namespace

uint64_t type_of(uint64_t object) { return class_of(object); }

bool derives_from(uint64_t type, uint64_t wanted) {
    for (int depth = 0; type && wanted && depth < 64; ++depth, type = read_or<uint64_t>(type + t::struct_type::super))
        if (type == wanted) return true;
    return false;
}

TSUCHI_Result find_place_view(uint64_t type, uint64_t base, const char* path, uint64_t replaced, const ArrayMemory* view, Place& out, const WritePlan* pending) {
    if (!path || !*path) return TSUCHI_INVALID;
    size_t i = 0;
    for (;;) {
        char name[kNameBytes];
        size_t length = 0;
        while (path[i] && path[i] != '.' && path[i] != '[') {
            if (length + 1 >= sizeof name) return TSUCHI_INVALID;
            name[length++] = path[i++];
        }
        name[length] = 0;
        if (!length) return TSUCHI_INVALID;
        uint64_t property = type ? find_field(type, name) : 0;
        if (!property) return TSUCHI_NOT_FOUND;
        uint64_t at = base + uint64_t(read_or<int32_t>(property + t::property::offset));
        bool element = false;
        while (path[i] == '[') {
            int64_t index;
            if (!read_index(path, i, index)) return TSUCHI_INVALID;
            const int32_t count = count_of(property);
            if (!element && count > 1) { // a fixed number of values side by side
                if (index >= count) return TSUCHI_NOT_FOUND;
                at += uint64_t(index) * uint64_t(element_bytes(property));
                element = true;
            } else if (kind_of(property) == PropertyKind::array) {
                const uint64_t inner = read_or<uint64_t>(property + t::property::inner);
                const bool staged = view && at == replaced;
                const int32_t length_now = staged ? view->count : read_or<int32_t>(at + t::array::count);
                if (!inner || index >= length_now) return TSUCHI_NOT_FOUND;
                at = (staged ? view->data : read_or<uint64_t>(at + t::array::data)) + uint64_t(index) * uint64_t(element_bytes(inner));
                property = inner;
                element = false;
            } else {
                return TSUCHI_INVALID;
            }
        }
        if (!path[i]) {
            const int32_t bytes = element_bytes(property), count = count_of(property);
            if (bytes <= 0 || count <= 0 || !readable(at, uint64_t(bytes) * uint64_t(element ? 1 : count))) return TSUCHI_NOT_FOUND;
            out = Place{at, property, element};
            return TSUCHI_OK;
        }
        if (path[i] != '.') return TSUCHI_INVALID;
        ++i;
        if (!element && count_of(property) > 1) return TSUCHI_INVALID; // pick an element first
        const PropertyKind kind = kind_of(property);
        if (kind == PropertyKind::structure) {
            type = read_or<uint64_t>(property + t::property::inner);
            base = at;
        } else if (is_reference(kind)) {
            uint64_t reference_at = at;
            if (pending) for (uint32_t j = 0; j < pending->fields.count; ++j) {
                const PreparedField& staged = pending->fields[j];
                if (staged.destination.address == at && staged.destination.property == property) reference_at = staged.scratch;
            }
            const uint64_t object = referred(kind, reference_at);
            if (!live(object)) return TSUCHI_NOT_FOUND;
            type = class_of(object);
            base = object;
        } else {
            return TSUCHI_INVALID;
        }
    }
}

TSUCHI_Result find_place(uint64_t type, uint64_t base, const char* path, Place& out) {
    return find_place_view(type, base, path, 0, nullptr, out);
}

TSUCHI_Result read_value(const Place& place, Value& out) {
    const uint64_t at = place.address;
    out = Value{};
    if (!place.element && count_of(place.property) > 1) {
        out.kind = ValueKind::array;
        out.count = uint32_t(count_of(place.property));
        return TSUCHI_OK;
    }
    const PropertyKind kind = stored_kind(place.property);
    switch (kind) {
    case PropertyKind::boolean: {
        const uint8_t byte = read_or<uint8_t>(place.property + t::property::bool_byte);
        const uint8_t mask = read_or<uint8_t>(place.property + t::property::bool_field_mask);
        out.kind = ValueKind::boolean;
        out.boolean = (read_or<uint8_t>(at + byte) & mask) != 0;
        return TSUCHI_OK;
    }
    case PropertyKind::int8: out.integer = read_or<int8_t>(at); break;
    case PropertyKind::int16: out.integer = read_or<int16_t>(at); break;
    case PropertyKind::int32: out.integer = read_or<int32_t>(at); break;
    case PropertyKind::int64: out.integer = read_or<int64_t>(at); break;
    case PropertyKind::uint8: out.integer = read_or<uint8_t>(at); break;
    case PropertyKind::uint16: out.integer = read_or<uint16_t>(at); break;
    case PropertyKind::uint32: out.integer = read_or<uint32_t>(at); break;
    case PropertyKind::uint64: out.integer = int64_t(read_or<uint64_t>(at)); break;
    case PropertyKind::float32:
        out.kind = ValueKind::number;
        out.number = read_or<float>(at);
        return TSUCHI_OK;
    case PropertyKind::float64:
        out.kind = ValueKind::number;
        out.number = read_or<double>(at);
        return TSUCHI_OK;
    case PropertyKind::object:
    case PropertyKind::class_reference:
    case PropertyKind::weak_object: {
        const uint64_t object = referred(kind, at);
        out.object = to_handle(object);
        out.kind = out.object ? ValueKind::object : ValueKind::none;
        return TSUCHI_OK;
    }
    case PropertyKind::name:
    case PropertyKind::soft_object:
    case PropertyKind::soft_class:
    case PropertyKind::text: {
        size_t needed = 0;
        const TSUCHI_Result result = read_text(place, nullptr, 0, &needed);
        if (result != TSUCHI_OK && result != TSUCHI_CAPACITY) return result;
        out.kind = ValueKind::text;
        out.count = uint32_t(needed);
        return TSUCHI_OK;
    }
    case PropertyKind::structure:
        out.kind = ValueKind::structure;
        return TSUCHI_OK;
    case PropertyKind::array:
        out.kind = ValueKind::array;
        out.count = uint32_t(read_or<int32_t>(at + t::array::count));
        return TSUCHI_OK;
    default:
        return TSUCHI_UNSUPPORTED;
    }
    out.kind = ValueKind::integer;
    return TSUCHI_OK;
}

bool object_property(const Place& place) { return is_reference(kind_of(place.property)); }

TSUCHI_Result enum_number(const Place& place, const char* name, int64_t& out) {
    const PropertyKind kind = kind_of(place.property);
    if (kind != PropertyKind::enumeration && kind != PropertyKind::uint8) return TSUCHI_INVALID;
    const uint64_t type = read_or<uint64_t>(place.property + (kind == PropertyKind::enumeration ? t::property::meta_class : t::property::inner));
    if (!type) return TSUCHI_INVALID;
    const uint64_t data = read_or<uint64_t>(type + t::enum_type::names + t::array::data);
    const int32_t count = read_or<int32_t>(type + t::enum_type::names + t::array::count, -1);
    if (count < 0 || count > 4096 || (count && !readable(data, uint64_t(count) * t::enum_entry::size))) return TSUCHI_NOT_READY;
    for (int32_t i = 0; i < count; ++i) {
        const uint64_t at = data + uint64_t(i) * t::enum_entry::size;
        char text[kNameBytes];
        if (!name_text(read_or<Name>(at), text, sizeof text)) return TSUCHI_NOT_READY;
        const char* suffix = text;
        for (const char* c = text; *c; ++c) if (c[0] == ':' && c[1] == ':') suffix = c + 2;
        if (same_ignoring_case(name, text) || same_ignoring_case(name, suffix))
            return read(at + t::enum_entry::value, out) ? TSUCHI_OK : TSUCHI_NOT_READY;
    }
    return TSUCHI_NOT_FOUND;
}

TSUCHI_Result write_value(const Place& place, const Value& value) {
    const uint64_t at = place.address;
    if (!place.element && count_of(place.property) > 1) return TSUCHI_INVALID;
    const PropertyKind kind = stored_kind(place.property);
    if (kind == PropertyKind::boolean) {
        if (value.kind != ValueKind::boolean) return TSUCHI_INVALID;
        const uint8_t byte = read_or<uint8_t>(place.property + t::property::bool_byte);
        const uint8_t mask = read_or<uint8_t>(place.property + t::property::bool_field_mask);
        uint8_t now;
        if (!read(at + byte, now)) return TSUCHI_NOT_FOUND;
        return write(at + byte, uint8_t(value.boolean ? now | mask : now & ~mask)) ? TSUCHI_OK : TSUCHI_NOT_READY;
    }
    if (is_integer(kind)) {
        if (value.kind != ValueKind::integer) return TSUCHI_INVALID;
        const int64_t v = value.integer;
        bool fits = true;
        switch (kind) {
        case PropertyKind::int8: fits = v >= INT8_MIN && v <= INT8_MAX; break;
        case PropertyKind::int16: fits = v >= INT16_MIN && v <= INT16_MAX; break;
        case PropertyKind::int32: fits = v >= INT32_MIN && v <= INT32_MAX; break;
        case PropertyKind::uint8: fits = v >= 0 && v <= UINT8_MAX; break;
        case PropertyKind::uint16: fits = v >= 0 && v <= UINT16_MAX; break;
        case PropertyKind::uint32: fits = v >= 0 && v <= int64_t(UINT32_MAX); break;
        default: break; // 64 bits either way
        }
        if (!fits) return TSUCHI_INVALID;
        bool done = false;
        switch (kind) {
        case PropertyKind::int8: done = write(at, int8_t(v)); break;
        case PropertyKind::int16: done = write(at, int16_t(v)); break;
        case PropertyKind::int32: done = write(at, int32_t(v)); break;
        case PropertyKind::uint8: done = write(at, uint8_t(v)); break;
        case PropertyKind::uint16: done = write(at, uint16_t(v)); break;
        case PropertyKind::uint32: done = write(at, uint32_t(v)); break;
        default: done = write(at, v); break;
        }
        return done ? TSUCHI_OK : TSUCHI_NOT_READY;
    }
    if (kind == PropertyKind::float32 || kind == PropertyKind::float64) {
        // A whole number is welcome where a decimal is wanted.
        if (value.kind != ValueKind::number && value.kind != ValueKind::integer) return TSUCHI_INVALID;
        const double v = value.kind == ValueKind::number ? value.number : double(value.integer);
        const bool done = kind == PropertyKind::float32 ? write(at, float(v)) : write(at, v);
        return done ? TSUCHI_OK : TSUCHI_NOT_READY;
    }
    if (is_soft(kind)) {
        if (value.kind == ValueKind::none) return write_text(place, "");
        if (value.kind != ValueKind::object) return TSUCHI_INVALID;
        const uint64_t object = from_handle(value.object);
        if (!object) return TSUCHI_NOT_FOUND;
        // The field says which class it takes: of the object, or to be made from when it refers to a class.
        const bool to_class = kind == PropertyKind::soft_class;
        const uint64_t wanted = read_or<uint64_t>(place.property + (to_class ? t::property::meta_class : t::property::inner));
        if (to_class ? !is_class(object) || (wanted && !derives_from(object, wanted)) : wanted && !derives_from(class_of(object), wanted)) return TSUCHI_INVALID;
        static char path[kNameBytes];
        uint64_t needed = 0;
        const TSUCHI_Result named = object_path(object, path, sizeof path, &needed);
        return named == TSUCHI_OK ? write_text(place, path) : named == TSUCHI_CAPACITY ? TSUCHI_UNSUPPORTED : named;
    }
    if (is_reference(kind)) {
        uint64_t object = 0;
        if (value.kind == ValueKind::object) {
            object = from_handle(value.object);
            if (!object) return TSUCHI_NOT_FOUND;
            // The field says which class it takes; anything else would mislead the game.
            const bool to_class = kind == PropertyKind::class_reference;
            const uint64_t wanted = read_or<uint64_t>(place.property + (to_class ? t::property::meta_class : t::property::inner));
            if (to_class ? !is_class(object) || (wanted && !derives_from(object, wanted)) : wanted && !derives_from(class_of(object), wanted)) return TSUCHI_INVALID;
        } else if (value.kind != ValueKind::none) {
            return TSUCHI_INVALID;
        }
        return write(at, kind == PropertyKind::weak_object ? to_weak_value(object) : object) ? TSUCHI_OK : TSUCHI_NOT_READY;
    }
    return kind == PropertyKind::unknown ? TSUCHI_NOT_FOUND : TSUCHI_UNSUPPORTED;
}

TSUCHI_Result read_text(const Place& place, char* out, size_t capacity, size_t* needed) {
    if (needed) *needed = 0;
    const PropertyKind kind = kind_of(place.property);
    if (kind == PropertyKind::name || is_soft(kind)) {
        // A reference to an asset by its path keeps the path as one name.
        const uint64_t at = place.address + (kind == PropertyKind::name ? 0 : t::soft_object::path);
        size_t bytes = 0;
        // A reference that holds no path reads as empty text, which is what writing empty text leaves there.
        if (is_soft(kind) && read_or<uint64_t>(at, 1) == 0) {
            if (needed) *needed = 1;
            if (!out || !capacity) return TSUCHI_CAPACITY;
            out[0] = 0;
            return TSUCHI_OK;
        }
        if (name_text(read_or<Name>(at), out, capacity, &bytes)) {
            if (needed) *needed = strlen(out) + 1;
            return TSUCHI_OK;
        }
        if (!bytes) return TSUCHI_NOT_FOUND;
        if (needed) *needed = bytes;
        return TSUCHI_CAPACITY;
    }
    if (kind != PropertyKind::text) return TSUCHI_INVALID;
    // A string is an array of 16-bit units, ending with a zero.
    const uint64_t data = read_or<uint64_t>(place.address + t::array::data);
    int32_t units = read_or<int32_t>(place.address + t::array::count);
    if (units < 0 || (units && !readable(data, uint64_t(units) * 2))) return TSUCHI_NOT_FOUND;
    Writer measure{nullptr, 0};
    convert(data, units, measure);
    if (needed) *needed = measure.count;
    if (!out || measure.count > capacity) return TSUCHI_CAPACITY;
    Writer writer{out, capacity};
    convert(data, units, writer);
    return TSUCHI_OK;
}

TSUCHI_Result read_engine_text(const Place& place, char* out, size_t capacity, size_t* needed) {
    if (needed) *needed = 0;
    if (!(place.element || count_of(place.property) == 1) || kind_of(place.property) != PropertyKind::engine_text) return TSUCHI_INVALID;
    if (!t::bound(t::fn::text_display_string)) return TSUCHI_NOT_READY;
    // A text shares what it shows with every other use of the same text. One that shares nothing was never made.
    if (!read_or<uint64_t>(place.address)) return TSUCHI_NOT_FOUND;
    const auto string = reinterpret_cast<uint64_t>(t::fn::text_display_string(reinterpret_cast<void*>(place.address)));
    if (!string || !readable(string, t::array::size)) return TSUCHI_NOT_FOUND;
    return units_to_utf8(read_or<uint64_t>(string + t::array::data), read_or<int32_t>(string + t::array::count), out, capacity, needed);
}

// ---- The engine's memory ------------------------------------------------------

namespace {

// Memory from the engine's own allocator, which is what the engine will free it
// with. Null when the allocator cannot be reached or has none to give.
void* engine_allocate(uint64_t bytes, uint32_t alignment) {
    const uint64_t allocator = global<uint64_t>(t::data::allocator);
    const auto allocate = allocator ? method(allocator, t::allocator_vtable::malloc) : nullptr;
    if (!allocate || !method(allocator, t::allocator_vtable::free)) return nullptr;
    return allocate(reinterpret_cast<void*>(allocator), bytes, alignment);
}

void engine_release(uint64_t block) {
    const uint64_t allocator = global<uint64_t>(t::data::allocator);
    const auto release = allocator && block ? method(allocator, t::allocator_vtable::free) : nullptr;
    if (release) release(reinterpret_cast<void*>(allocator), reinterpret_cast<void*>(block));
}

bool engine_memory_ready() {
    const uint64_t allocator = global<uint64_t>(t::data::allocator);
    return allocator && method(allocator, t::allocator_vtable::malloc) && method(allocator, t::allocator_vtable::free);
}

// UTF-8 to 16-bit units. Counts every unit and writes those that fit. False
// for text that is not well-formed UTF-8.
bool to_units(const char* text, char16_t* out, size_t capacity, size_t& count) {
    count = 0;
    const auto put = [&](uint32_t unit) {
        if (count < capacity) out[count] = char16_t(unit);
        ++count;
    };
    for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p;) {
        uint32_t c = *p++;
        int more = 0;
        uint32_t least = 0;
        if (c >= 0xF0) {
            if (c >= 0xF5) return false;
            c &= 0x07, more = 3, least = 0x10000;
        } else if (c >= 0xE0) {
            c &= 0x0F, more = 2, least = 0x800;
        } else if (c >= 0xC0) {
            c &= 0x1F, more = 1, least = 0x80;
        } else if (c >= 0x80) {
            return false;
        }
        for (; more; --more, ++p) {
            if ((*p & 0xC0) != 0x80) return false;
            c = c << 6 | (*p & 0x3F);
        }
        if (c < least || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) return false;
        if (c >= 0x10000) {
            put(0xD800 + ((c - 0x10000) >> 10));
            put(0xDC00 + ((c - 0x10000) & 0x3FF));
        } else {
            put(c);
        }
    }
    return true;
}

bool one_value(const Place& place) { return place.element || count_of(place.property) == 1; }

} // namespace

bool utf8_to_units(const char* text, char16_t* out, size_t capacity, size_t& count) { return to_units(text, out, capacity, count); }

TSUCHI_Result units_to_utf8(uint64_t data, int32_t units, char* out, size_t capacity, size_t* needed) {
    if (units < 0 || (units && !readable(data, uint64_t(units) * 2))) return TSUCHI_NOT_FOUND;
    Writer measure{nullptr, 0};
    convert(data, units, measure);
    if (needed) *needed = measure.count;
    if (!out || measure.count > capacity) return TSUCHI_CAPACITY;
    Writer writer{out, capacity};
    convert(data, units, writer);
    return TSUCHI_OK;
}

namespace {
// The game's name constructor in one of its two modes: 1 adds the name, 0 only finds it.
TSUCHI_Result text_name(const char* text, Name& out, uint64_t add) {
    size_t units = 0;
    if (!text || !to_units(text, nullptr, 0, units)) return TSUCHI_INVALID;
    if (units > t::constant::max_name_units) return TSUCHI_CAPACITY;
    uint64_t name = 0; // the name of nothing, for empty text
    if (units) {
        if (!t::bound(t::fn::name_from_text)) return TSUCHI_NOT_READY;
        char16_t wide[t::most::max_name_units + 1];
        to_units(text, wide, units, units);
        wide[units] = 0;
        name = t::fn::name_from_text(wide, units, add);
        if (!name && !add) return TSUCHI_NOT_FOUND;
    }
    memcpy(&out, &name, sizeof out);
    return TSUCHI_OK;
}
} // namespace

TSUCHI_Result name_from_utf8(const char* text, Name& out) { return text_name(text, out, 1); }

TSUCHI_Result existing_name_from_utf8(const char* text, Name& out) { return text_name(text, out, 0); }

TSUCHI_Result write_text(const Place& place, const char* text) {
    if (!text || !one_value(place)) return TSUCHI_INVALID;
    const PropertyKind kind = kind_of(place.property);
    if (is_soft(kind)) {
        // The path as one name, no object found for it yet, and no path inside the asset.
        namespace s = t::soft_object;
        Name name{};
        const TSUCHI_Result named = name_from_utf8(text, name);
        if (named != TSUCHI_OK) return named;
        const uint64_t at = place.address, inside = at + s::sub_path;
        if (!writable(at, s::size)) return TSUCHI_NOT_READY;
        const uint64_t old = read_or<uint64_t>(inside + t::array::data);
        write(at + s::index, int32_t(-1)), write(at + s::serial, int32_t(0)), write(at + s::path, name);
        write(inside + t::array::data, uint64_t(0)), write(inside + t::array::count, int32_t(0)), write(inside + t::array::max, int32_t(0));
        engine_release(old);
        return TSUCHI_OK;
    }
    if (kind != PropertyKind::name && kind != PropertyKind::text) return kind == PropertyKind::unknown ? TSUCHI_NOT_FOUND : TSUCHI_INVALID;
    size_t units = 0;
    if (!to_units(text, nullptr, 0, units)) return TSUCHI_INVALID;

    if (kind == PropertyKind::name) {
        if (units > t::constant::max_name_units) return TSUCHI_CAPACITY;
        uint64_t name = 0; // the name of nothing, for empty text
        if (units) {
            if (!t::bound(t::fn::name_from_text)) return TSUCHI_NOT_READY;
            char16_t wide[t::most::max_name_units + 1];
            to_units(text, wide, units, units);
            wide[units] = 0;
            name = t::fn::name_from_text(wide, units, 1);
        }
        return write(place.address, name) ? TSUCHI_OK : TSUCHI_NOT_READY;
    }

    const uint64_t header = place.address;
    if (!writable(header, t::array::size)) return TSUCHI_NOT_READY;
    if (units > t::constant::max_array_count) return TSUCHI_CAPACITY;
    const uint64_t old = read_or<uint64_t>(header + t::array::data);
    uint64_t data = 0;
    if (units) {
        if (!engine_memory_ready()) return TSUCHI_NOT_READY;
        auto* block = static_cast<char16_t*>(engine_allocate((units + 1) * 2, 0));
        if (!block) return TSUCHI_NO_MEMORY;
        to_units(text, block, units, units);
        block[units] = 0;
        data = reinterpret_cast<uint64_t>(block);
    }
    const int32_t stored = units ? int32_t(units + 1) : 0; // the terminator counts
    write(header + t::array::data, data);
    write(header + t::array::count, stored);
    write(header + t::array::max, stored);
    engine_release(old);
    return TSUCHI_OK;
}

namespace {

// What an array's elements are, for making, copying and destroying them.
struct Elements {
    PropertyKind kind = PropertyKind::unknown;
    int32_t bytes = 0;
    uint64_t type = 0; // of a struct, which is made and destroyed by its own type
    decltype(t::struct_vtable::initialize)::Function initialize = nullptr;
    decltype(t::struct_vtable::destroy)::Function destroy = nullptr;
};

// TSUCHI_OK for an array whose elements this can make and destroy.
TSUCHI_Result elements_of(const Place& place, Elements& out) {
    if (!one_value(place) || kind_of(place.property) != PropertyKind::array) return TSUCHI_INVALID;
    const uint64_t inner = read_or<uint64_t>(place.property + t::property::inner);
    out.bytes = inner ? element_bytes(inner) : 0;
    if (out.bytes <= 0) return TSUCHI_NOT_FOUND;
    out.kind = stored_kind(inner);
    const bool plain = out.kind == PropertyKind::boolean || is_integer(out.kind) || out.kind == PropertyKind::float32 ||
                       out.kind == PropertyKind::float64 || out.kind == PropertyKind::name || is_reference(out.kind);
    if (is_soft(out.kind) && uint32_t(out.bytes) < t::soft_object::size) return TSUCHI_UNSUPPORTED;
    if (!plain && out.kind != PropertyKind::text && out.kind != PropertyKind::structure && !is_soft(out.kind)) return TSUCHI_UNSUPPORTED;
    if (out.kind == PropertyKind::structure) {
        out.type = read_or<uint64_t>(inner + t::property::inner);
        out.initialize = out.type ? method(out.type, t::struct_vtable::initialize) : nullptr;
        out.destroy = out.type ? method(out.type, t::struct_vtable::destroy) : nullptr;
        if (!out.initialize || !out.destroy) return TSUCHI_NOT_READY;
    }
    return TSUCHI_OK;
}

// New elements in memory of the framework's asking: zero, a reference by path to nothing, or as the engine makes
// a new struct.
void make_elements(const Elements& elements, uint8_t* first, uint32_t count) {
    if (!count) return;
    memset(first, 0, size_t(count) * size_t(elements.bytes));
    if (is_soft(elements.kind)) {
        const int32_t none = -1;
        for (uint32_t i = 0; i < count; ++i) memcpy(first + size_t(i) * size_t(elements.bytes) + t::soft_object::index, &none, sizeof none);
    }
    if (elements.initialize)
        elements.initialize(reinterpret_cast<void*>(elements.type), first, int32_t(count));
}

// Where in an element the string it owns is, or -1 for an element that owns none: a string is one, and a
// reference by path has the path inside the asset.
int32_t owned_string(const Elements& elements) {
    return elements.kind == PropertyKind::text ? 0 : is_soft(elements.kind) ? int32_t(t::soft_object::sub_path) : -1;
}

// Elements that are cut off or thrown away, destroyed where they are.
void destroy_elements(const Elements& elements, uint64_t first, uint32_t count) {
    if (!count) return;
    if (elements.destroy) {
        elements.destroy(reinterpret_cast<void*>(elements.type), reinterpret_cast<void*>(first), int32_t(count));
        return;
    }
    const int32_t string = owned_string(elements);
    for (uint32_t i = 0; string >= 0 && i < count; ++i)
        engine_release(read_or<uint64_t>(first + uint64_t(i) * uint64_t(elements.bytes) + uint64_t(string) + t::array::data));
}

} // namespace

TSUCHI_Result resize(const Place& place, uint32_t count) {
    Elements elements;
    const TSUCHI_Result known = elements_of(place, elements);
    if (known != TSUCHI_OK) return known;
    const int32_t bytes = elements.bytes;

    const uint64_t header = place.address;
    if (!writable(header, t::array::size)) return TSUCHI_NOT_READY;
    const uint64_t old = read_or<uint64_t>(header + t::array::data);
    const int32_t length = read_or<int32_t>(header + t::array::count);
    if (length < 0 || (length && !readable(old, uint64_t(length) * uint64_t(bytes)))) return TSUCHI_NOT_FOUND;
    if (count > t::constant::max_array_count) return TSUCHI_CAPACITY;
    if (count == uint32_t(length)) return TSUCHI_OK;

    // The new memory first: if there is none, nothing has changed yet.
    uint8_t* block = nullptr;
    if (count) {
        if (!engine_memory_ready()) return TSUCHI_NOT_READY;
        block = static_cast<uint8_t*>(engine_allocate(uint64_t(count) * uint64_t(bytes), 0));
        if (!block) return TSUCHI_NO_MEMORY;
    }
    const uint32_t kept = count < uint32_t(length) ? count : uint32_t(length);
    if (kept) memcpy(block, reinterpret_cast<const void*>(old), size_t(kept) * size_t(bytes));
    if (count > kept) make_elements(elements, block + size_t(kept) * size_t(bytes), count - kept);
    // What is cut off is destroyed where it was.
    if (uint32_t(length) > kept) destroy_elements(elements, old + uint64_t(kept) * uint64_t(bytes), uint32_t(length) - kept);
    write(header + t::array::data, reinterpret_cast<uint64_t>(block));
    write(header + t::array::count, int32_t(count));
    write(header + t::array::max, int32_t(count));
    engine_release(old);
    return TSUCHI_OK;
}

TSUCHI_Result array_of(const Place& place, ArrayMemory& out) {
    Elements elements;
    const TSUCHI_Result known = elements_of(place, elements);
    if (known != TSUCHI_OK) return known;
    out.data = read_or<uint64_t>(place.address + t::array::data);
    out.count = read_or<int32_t>(place.address + t::array::count, -1);
    out.max = read_or<int32_t>(place.address + t::array::max, -1);
    return out.count >= 0 && out.max >= out.count && (!out.count || readable(out.data, uint64_t(out.count) * uint64_t(elements.bytes))) ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}

TSUCHI_Result put_array(const Place& place, const ArrayMemory& array) {
    if (!one_value(place) || kind_of(place.property) != PropertyKind::array) return TSUCHI_INVALID;
    if (!writable(place.address, t::array::size)) return TSUCHI_NOT_READY;
    write(place.address + t::array::data, array.data), write(place.address + t::array::count, array.count), write(place.address + t::array::max, array.max);
    return TSUCHI_OK;
}

TSUCHI_Result copy_array(const Place& place, const ArrayMemory& source, uint32_t count, ArrayMemory& copied) {
    Elements elements;
    TSUCHI_Result result = elements_of(place, elements);
    if (result != TSUCHI_OK) return result;
    if (elements.kind == PropertyKind::structure) return TSUCHI_UNSUPPORTED; // a struct is copied by its own type, which this does not ask
    const size_t bytes = size_t(elements.bytes);
    if (!writable(place.address, t::array::size)) return TSUCHI_NOT_READY;
    if (source.count < 0 || (source.count && !readable(source.data, uint64_t(source.count) * bytes))) return TSUCHI_NOT_FOUND;
    if (count > t::constant::max_array_count) return TSUCHI_CAPACITY;
    uint8_t* block = nullptr;
    if (count) {
        if (!engine_memory_ready()) return TSUCHI_NOT_READY;
        block = static_cast<uint8_t*>(engine_allocate(uint64_t(count) * bytes, 0));
        if (!block) return TSUCHI_NO_MEMORY;
    }
    const uint32_t kept = count < uint32_t(source.count) ? count : uint32_t(source.count);
    if (kept) memcpy(block, reinterpret_cast<const void*>(source.data), size_t(kept) * bytes);
    // A string a copied element owns is copied too, so that each array frees its own.
    const int32_t string = owned_string(elements);
    for (uint32_t i = 0; string >= 0 && i < kept; ++i) {
        uint8_t* const at = block + size_t(i) * bytes + size_t(string);
        uint64_t data = 0;
        int32_t units = 0;
        memcpy(&data, at + t::array::data, 8), memcpy(&units, at + t::array::count, 4);
        uint64_t copy = 0;
        if (units > 0 && readable(data, uint64_t(units) * 2)) {
            copy = reinterpret_cast<uint64_t>(engine_allocate(uint64_t(units) * 2, 0));
            if (!copy) { // nothing has changed yet: the copies made so far go back
                for (uint32_t j = 0; j < i; ++j) {
                    memcpy(&data, block + size_t(j) * bytes + size_t(string) + t::array::data, 8);
                    engine_release(data);
                }
                engine_release(reinterpret_cast<uint64_t>(block));
                return TSUCHI_NO_MEMORY;
            }
            memcpy(reinterpret_cast<void*>(copy), reinterpret_cast<const void*>(data), size_t(units) * 2);
        } else {
            units = 0;
        }
        memcpy(at + t::array::data, &copy, 8), memcpy(at + t::array::count, &units, 4), memcpy(at + t::array::max, &units, 4);
    }
    if (count > kept) make_elements(elements, block + size_t(kept) * bytes, count - kept);
    copied = ArrayMemory{reinterpret_cast<uint64_t>(block), int32_t(count), int32_t(count)};
    return TSUCHI_OK;
}

TSUCHI_Result replace_array(const Place& place, const ArrayMemory& source, uint32_t count, ArrayMemory& before) {
    TSUCHI_Result result = array_of(place, before);
    if (result != TSUCHI_OK) return result;
    ArrayMemory copied;
    result = copy_array(place, source, count, copied);
    if (result == TSUCHI_OK) put_array(place, copied);
    return result;
}

void free_array(const Place& place, const ArrayMemory& array) {
    Elements elements;
    if (elements_of(place, elements) != TSUCHI_OK || array.count < 0 || (array.count && !readable(array.data, uint64_t(array.count) * uint64_t(elements.bytes)))) return;
    destroy_elements(elements, array.data, uint32_t(array.count));
    engine_release(array.data);
}

namespace {
struct WritePath {
    char text[4096]; size_t size = 0;
    bool start(const char* path) {
        if (!path || strlen(path) >= sizeof text) return false;
        size = strlen(path); memcpy(text, path, size + 1); return true;
    }
    bool append(const char* field) {
        const size_t bytes = strlen(field);
        if (!bytes || size + bytes + (size ? 1 : 0) >= sizeof text) return false;
        if (size) text[size++] = '.';
        memcpy(text + size, field, bytes + 1); size += bytes; return true;
    }
    bool index(uint32_t index) {
        char digits[16]; unsigned count = 0;
        do { digits[count++] = char('0' + index % 10); } while (index /= 10);
        if (size + count + 2 >= sizeof text) return false;
        text[size++] = '[';
        while (count) text[size++] = digits[--count];
        text[size++] = ']'; text[size] = 0; return true;
    }
    void back(size_t before) { text[size = before] = 0; }
};
bool overlaps(uint64_t a, uint64_t bytes_a, uint64_t b, uint64_t bytes_b) {
    return bytes_a && bytes_b && (a <= b ? b - a < bytes_a : a - b < bytes_b);
}
int32_t field_string(const Place& place) {
    const PropertyKind kind = stored_kind(place.property);
    return kind == PropertyKind::text ? 0 : is_soft(kind) ? int32_t(t::soft_object::sub_path) : -1;
}
TSUCHI_Result stage_representation(PreparedField& field, const ScriptWrite& wanted) {
    const Place scratch{field.scratch, field.destination.property, field.destination.element};
    if (wanted.kind == ScriptWriteKind::text) return write_text(scratch, wanted.text.get());
    if (wanted.kind != ScriptWriteKind::value) return TSUCHI_INVALID;
    Value value; value.kind = ValueKind(wanted.value.kind);
    switch (wanted.value.kind) {
    case TSUCHI_VALUE_NONE: break;
    case TSUCHI_VALUE_BOOLEAN: value.boolean = wanted.value.as.boolean; break;
    case TSUCHI_VALUE_INTEGER: value.integer = wanted.value.as.integer; break;
    case TSUCHI_VALUE_NUMBER:
        value.number = wanted.value.as.number;
        if (value.number - value.number != 0) return TSUCHI_INVALID;
        break;
    case TSUCHI_VALUE_OBJECT:
        value.object = wanted.value.as.object.id;
        if (!value.object) return TSUCHI_INVALID;
        break;
    default: return TSUCHI_INVALID;
    }
    return write_value(scratch, value);
}
void discard_field(PreparedField& field, bool installed) {
    const int32_t string = field_string(field.destination);
    if (!installed && field.scratch && string >= 0)
        engine_release(read_or<uint64_t>(field.scratch + uint64_t(string) + t::array::data));
    engine_release(field.scratch);
    field.path.clear();
}
TSUCHI_Result stage_field(WritePlan& plan, const Place& place, const ScriptWrite& wanted, const char* path) {
    PreparedField field; field.destination = place; field.wanted = &wanted;
    const int32_t bytes = element_bytes(place.property);
    if (bytes <= 0) return TSUCHI_NOT_FOUND;
    field.bytes = uint32_t(bytes);
    field.boolean = stored_kind(place.property) == PropertyKind::boolean;
    if (field.boolean) {
        field.bool_byte = read_or<uint8_t>(place.property + t::property::bool_byte);
        field.bool_mask = read_or<uint8_t>(place.property + t::property::bool_field_mask);
        if (field.bytes <= field.bool_byte) field.bytes = uint32_t(field.bool_byte) + 1;
    }
    if (!writable(place.address, field.bytes)) return TSUCHI_NOT_READY;
    // Aliases cannot retire an owning header twice or override a staged array through its old storage.
    for (uint32_t i = 0; i < plan.fields.count; ++i) {
        const PreparedField& other = plan.fields[i];
        const uint64_t at = place.address + (field.boolean ? field.bool_byte : 0);
        const uint64_t other_at = other.destination.address + (other.boolean ? other.bool_byte : 0);
        if (!overlaps(at, field.boolean ? 1 : field.bytes, other_at, other.boolean ? 1 : other.bytes)) continue;
        if (field.boolean && other.boolean && place.address + field.bool_byte == other.destination.address + other.bool_byte && !(field.bool_mask & other.bool_mask)) continue;
        return TSUCHI_INVALID;
    }
    for (uint32_t i = 0; i < plan.arrays.count; ++i) {
        const PreparedArray& other = plan.arrays[i];
        Elements elements;
        if (elements_of(other.destination, elements) != TSUCHI_OK) return TSUCHI_NOT_READY;
        if (overlaps(place.address, field.bytes, other.destination.address, t::array::size) ||
            overlaps(place.address, field.bytes, other.before_data, uint64_t(other.before_max) * uint64_t(elements.bytes))) return TSUCHI_INVALID;
    }
    const int32_t string = field_string(place);
    if (string >= 0) {
        field.old_owned = read_or<uint64_t>(place.address + uint64_t(string) + t::array::data);
        for (uint32_t i = 0; field.old_owned && i < plan.fields.count; ++i)
            if (plan.fields[i].old_owned == field.old_owned) return TSUCHI_INVALID;
        for (uint32_t i = 0; field.old_owned && i < plan.arrays.count; ++i)
            if (plan.arrays[i].before_data == field.old_owned) return TSUCHI_INVALID;
    }
    if (!field.path.set(path)) return TSUCHI_NO_MEMORY;
    field.scratch = reinterpret_cast<uint64_t>(engine_memory(field.bytes));
    if (!field.scratch) { field.path.clear(); return engine_memory_ready() ? TSUCHI_NO_MEMORY : TSUCHI_NOT_READY; }
    memcpy(reinterpret_cast<void*>(field.scratch), reinterpret_cast<const void*>(place.address), field.bytes);
    if (string >= 0) memset(reinterpret_cast<void*>(field.scratch + uint64_t(string)), 0, t::array::size);
    TSUCHI_Result result = stage_representation(field, wanted);
    if (result != TSUCHI_OK) { discard_field(field, false); return result; }
    if (!plan.fields.add(field)) { discard_field(field, false); return TSUCHI_NO_MEMORY; }
    return plan.field_ready ? plan.field_ready(plan.field_context, plan, plan.fields.count - 1) : TSUCHI_OK;
}
TSUCHI_Result stage_tree(WritePlan& plan, const Place& place, const ScriptWrite& wanted, WritePath& path, bool can_resize, unsigned depth) {
    if (depth > 16) return TSUCHI_UNSUPPORTED;
    if (wanted.kind == ScriptWriteKind::value || wanted.kind == ScriptWriteKind::text)
        return stage_field(plan, place, wanted, path.text);
    Value current;
    TSUCHI_Result result = read_value(place, current);
    if (result != TSUCHI_OK) return result;
    const size_t here = path.size;
    if (current.kind == ValueKind::structure) {
        if (wanted.kind != ScriptWriteKind::structure && wanted.kind != ScriptWriteKind::empty) return TSUCHI_INVALID;
        uint64_t type = 0, base = 0;
        if ((result = struct_at(place, type, base)) != TSUCHI_OK) return result;
        for (uint32_t i = 0; i < wanted.children.count; ++i) {
            const ScriptWrite& child = *wanted.children[i];
            Place member;
            result = find_place_view(type, base, child.name.get(), 0, nullptr, member, &plan);
            if (result == TSUCHI_OK) result = path.append(child.name.get()) ? stage_tree(plan, member, child, path, can_resize, depth + 1) : TSUCHI_INVALID;
            path.back(here);
            if (result != TSUCHI_OK) return result;
        }
        return TSUCHI_OK;
    }
    if (current.kind != ValueKind::array || (wanted.kind != ScriptWriteKind::array && wanted.kind != ScriptWriteKind::empty)) return TSUCHI_INVALID;
    const uint32_t count = wanted.children.count;
    uint64_t data = place.address, property = place.property;
    int32_t stride = element_bytes(place.property);
    if (one_value(place) && kind_of(place.property) == PropertyKind::array) {
        Elements elements;
        if ((result = elements_of(place, elements)) != TSUCHI_OK) return result;
        ArrayMemory before;
        if ((result = array_of(place, before)) != TSUCHI_OK) return result;
        stride = elements.bytes;
        property = read_or<uint64_t>(place.property + t::property::inner);
        data = before.data;
        if (!can_resize) {
            if (count != uint32_t(before.count)) return TSUCHI_INVALID;
        } else {
            if (count > t::constant::max_array_count) return TSUCHI_CAPACITY;
            if (!writable(place.address, t::array::size)) return TSUCHI_NOT_READY;
            for (uint32_t i = 0; i < plan.fields.count; ++i) {
                const PreparedField& field = plan.fields[i];
                if (overlaps(place.address, t::array::size, field.destination.address, field.bytes) ||
                    overlaps(before.data, uint64_t(before.max) * uint64_t(stride), field.destination.address, field.bytes) ||
                    (before.data && field.old_owned == before.data)) return TSUCHI_INVALID;
            }
            for (uint32_t i = 0; i < plan.arrays.count; ++i) {
                const PreparedArray& array = plan.arrays[i];
                Elements other;
                if (elements_of(array.destination, other) != TSUCHI_OK) return TSUCHI_NOT_READY;
                if (overlaps(place.address, t::array::size, array.destination.address, t::array::size) ||
                    (before.data && before.data == array.before_data) ||
                    overlaps(before.data, uint64_t(before.max) * uint64_t(stride), array.before_data, uint64_t(array.before_max) * uint64_t(other.bytes)) ||
                    overlaps(place.address, t::array::size, array.before_data, uint64_t(array.before_max) * uint64_t(other.bytes)) ||
                    overlaps(before.data, uint64_t(before.max) * uint64_t(stride), array.destination.address, t::array::size)) return TSUCHI_INVALID;
            }
            const uint32_t kept = count < uint32_t(before.count) ? count : uint32_t(before.count);
            PreparedArray after{place, before.data, 0, uint32_t(before.count), uint32_t(before.max), count, kept};
            if (count) {
                after.after_data = reinterpret_cast<uint64_t>(engine_memory(uint64_t(count) * uint64_t(stride)));
                if (!after.after_data) return engine_memory_ready() ? TSUCHI_NO_MEMORY : TSUCHI_NOT_READY;
                if (kept) memcpy(reinterpret_cast<void*>(after.after_data), reinterpret_cast<const void*>(before.data), size_t(kept) * size_t(stride));
                make_elements(elements, reinterpret_cast<uint8_t*>(after.after_data) + size_t(kept) * size_t(stride), count - kept);
            }
            if (!plan.arrays.add(after)) {
                if (count > kept) destroy_elements(elements, after.after_data + uint64_t(kept) * uint64_t(stride), count - kept);
                engine_release(after.after_data); return TSUCHI_NO_MEMORY;
            }
            data = after.after_data;
        }
    } else if (count != current.count) return TSUCHI_INVALID;
    for (uint32_t i = 0; i < count; ++i) {
        const Place element{data + uint64_t(i) * uint64_t(stride), property, true};
        result = path.index(i) ? stage_tree(plan, element, *wanted.children[i], path, can_resize, depth + 1) : TSUCHI_INVALID;
        path.back(here);
        if (result != TSUCHI_OK) return result;
    }
    return TSUCHI_OK;
}
} // namespace

TSUCHI_Result WritePlan::prepare(uint64_t type, uint64_t base, const char* path, const ScriptWrite& value, bool resize_arrays) {
    Place place;
    TSUCHI_Result result = find_place(type, base, path, place);
    if (result != TSUCHI_OK) return result;
    if (value.kind == ScriptWriteKind::value || value.kind == ScriptWriteKind::text) return stage_field(*this, place, value, path);
    WritePath named;
    if (!named.start(path)) return TSUCHI_INVALID;
    return stage_tree(*this, place, value, named, resize_arrays, 0);
}
TSUCHI_Result WritePlan::prepare_field(const Place& place, const char* path, const ScriptWrite& value) {
    return stage_field(*this, place, value, path);
}
TSUCHI_Result WritePlan::replace_field(uint32_t index, const ScriptWrite& value) {
    return index < fields.count ? stage_representation(fields[index], value) : TSUCHI_INVALID;
}
void WritePlan::commit() {
    // All destinations and allocations have been checked. Installation runs no script conversion.
    for (uint32_t i = 0; i < fields.count; ++i) {
        const PreparedField& field = fields[i];
        if (field.boolean) {
            const uint64_t at = field.destination.address + field.bool_byte;
            const uint8_t wanted = read_or<uint8_t>(field.scratch + field.bool_byte);
            const uint8_t before = read_or<uint8_t>(at);
            const uint8_t after = uint8_t((before & ~field.bool_mask) | (wanted & field.bool_mask));
            memcpy(reinterpret_cast<void*>(at), &after, sizeof after);
        } else memcpy(reinterpret_cast<void*>(field.destination.address), reinterpret_cast<const void*>(field.scratch), field.bytes);
    }
    // Children publish into their parents' staged buffers before those buffers are installed.
    for (uint32_t i = arrays.count; i; --i) {
        const PreparedArray& array = arrays[i - 1];
        const ArrayMemory after{array.after_data, int32_t(array.after_count), int32_t(array.after_count)};
        memcpy(reinterpret_cast<void*>(array.destination.address), &after, t::array::size);
    }
    committed = true;
    // Retire old resources after the complete result is visible. Kept elements transfer their ownership.
    for (uint32_t i = 0; i < fields.count; ++i) engine_release(fields[i].old_owned);
    for (uint32_t i = arrays.count; i; --i) {
        const PreparedArray& array = arrays[i - 1];
        Elements elements;
        if (elements_of(array.destination, elements) == TSUCHI_OK && array.before_count > array.kept)
            destroy_elements(elements, array.before_data + uint64_t(array.kept) * uint64_t(elements.bytes), array.before_count - array.kept);
        engine_release(array.before_data);
    }
}
WritePlan::~WritePlan() {
    for (uint32_t i = 0; i < fields.count; ++i) discard_field(fields[i], committed);
    fields.clear();
    if (!committed) for (uint32_t i = arrays.count; i; --i) {
        const PreparedArray& array = arrays[i - 1];
        Elements elements;
        if (elements_of(array.destination, elements) == TSUCHI_OK && array.after_count > array.kept)
            destroy_elements(elements, array.after_data + uint64_t(array.kept) * uint64_t(elements.bytes), array.after_count - array.kept);
        engine_release(array.after_data);
    }
    arrays.clear();
}

TSUCHI_Result struct_at(const Place& place, uint64_t& type, uint64_t& base) {
    if (!one_value(place) || kind_of(place.property) != PropertyKind::structure) return TSUCHI_INVALID;
    type = read_or<uint64_t>(place.property + t::property::inner);
    base = place.address;
    return type ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}

uint64_t field_at(uint64_t type, uint32_t index, bool parents) {
    for (int depth = 0; type && depth < 64; ++depth, type = parents ? read_or<uint64_t>(type + t::struct_type::super) : 0) {
        uint64_t property = read_or<uint64_t>(type + t::struct_type::properties);
        for (int guard = 0; property && guard < 8192; ++guard, property = read_or<uint64_t>(property + t::property::next))
            if (!index--) return property;
    }
    return 0;
}

Name field_name(uint64_t property) { return read_or<Name>(property + t::property::name); }

namespace {

// Where the walk is: a field, and its element when it holds more than one value. The path is only spelled
// out for a reference that is cleared, so names are not looked up for the rest.
struct Step {
    uint64_t property;
    int32_t index; // -1 for no element
};
constexpr int kDeepest = 8;
struct Clearing {
    const uint64_t* targets;
    uint32_t target_count;
    void (*cleared)(void*, const char*, uint32_t);
    void* context;
    uint32_t count;
    Step steps[kDeepest + 2];
    int depth;
};

void put_text(char* out, size_t capacity, size_t& at, const char* text) {
    for (; *text && at + 1 < capacity; ++text) out[at++] = *text;
    out[at] = 0;
}

void tell_cleared(Clearing& clearing, uint32_t target) {
    char path[256];
    size_t at = 0;
    path[0] = 0;
    for (int i = 0; i < clearing.depth; ++i) {
        const Step& step = clearing.steps[i];
        if (step.property) {
            char name[kNameBytes];
            if (!name_text(field_name(step.property), name, sizeof name)) name[0] = '?', name[1] = 0;
            if (at) put_text(path, sizeof path, at, ".");
            put_text(path, sizeof path, at, name);
        }
        if (step.index >= 0) {
            char digits[12], text[16];
            size_t d = 0, n = 0;
            uint32_t index = uint32_t(step.index);
            do digits[d++] = char('0' + index % 10); while (index /= 10);
            text[n++] = '[';
            while (d) text[n++] = digits[--d];
            text[n++] = ']', text[n] = 0;
            put_text(path, sizeof path, at, text);
        }
    }
    clearing.cleared(clearing.context, path, target);
}

void clear_in(Clearing& clearing, uint64_t type, uint64_t base);

// One value of a field: a reference to clear, a struct to look into, or an array of either.
void clear_value(Clearing& clearing, uint64_t property, uint64_t at) {
    const PropertyKind kind = kind_of(property);
    if (kind == PropertyKind::object) {
        const uint64_t held = read_or<uint64_t>(at);
        for (uint32_t i = 0; held && i < clearing.target_count; ++i) {
            if (held != clearing.targets[i] || !write(at, uint64_t(0))) continue;
            ++clearing.count;
            if (clearing.cleared) tell_cleared(clearing, i);
            break;
        }
    } else if (kind == PropertyKind::structure) {
        clear_in(clearing, read_or<uint64_t>(property + t::property::inner), at);
    } else if (kind == PropertyKind::array && clearing.depth <= kDeepest) {
        const uint64_t inner = read_or<uint64_t>(property + t::property::inner);
        const PropertyKind inner_kind = inner ? kind_of(inner) : PropertyKind::unknown;
        if (inner_kind != PropertyKind::object && inner_kind != PropertyKind::structure) return;
        const uint64_t data = read_or<uint64_t>(at + t::array::data);
        const int32_t count = read_or<int32_t>(at + t::array::count, -1), bytes = element_bytes(inner);
        if (count <= 0 || bytes <= 0 || uint32_t(count) > t::constant::max_array_count || !readable(data, uint64_t(count) * uint64_t(bytes))) return;
        Step& step = clearing.steps[clearing.depth++];
        step.property = 0;
        for (int32_t i = 0; i < count; ++i) {
            step.index = i;
            clear_value(clearing, inner, data + uint64_t(i) * uint64_t(bytes));
        }
        --clearing.depth;
    }
}

void clear_in(Clearing& clearing, uint64_t type, uint64_t base) {
    if (clearing.depth > kDeepest) return;
    Step& step = clearing.steps[clearing.depth++];
    for (int up = 0; type && up < 64; ++up, type = read_or<uint64_t>(type + t::struct_type::super))
        for (uint64_t property = read_or<uint64_t>(type + t::struct_type::properties), guard = 0; property && guard < 8192;
             ++guard, property = read_or<uint64_t>(property + t::property::next)) {
            const PropertyKind kind = kind_of(property);
            if (kind != PropertyKind::object && kind != PropertyKind::structure && kind != PropertyKind::array) continue;
            const int32_t count = count_of(property), bytes = element_bytes(property);
            if (count <= 0 || bytes <= 0) continue;
            const uint64_t at = base + uint64_t(read_or<int32_t>(property + t::property::offset));
            step.property = property;
            for (int32_t e = 0; e < count; ++e) {
                step.index = count > 1 ? e : -1;
                clear_value(clearing, property, at + uint64_t(e) * uint64_t(bytes));
            }
        }
    --clearing.depth;
}

} // namespace

uint32_t clear_references(uint64_t object, const uint64_t* targets, uint32_t count,
                          void (*cleared)(void* context, const char* path, uint32_t target), void* context) {
    if (!object || !count) return 0;
    Clearing clearing{targets, count, cleared, context, 0, {}, 0};
    clear_in(clearing, class_of(object), object);
    return clearing.count;
}

bool is_output(uint64_t property) {
    const uint64_t flags = read_or<uint64_t>(property + t::property::flags);
    if (!(flags & t::constant::property_param)) return false;
    return (flags & t::constant::property_return) || ((flags & t::constant::property_out) && !(flags & t::constant::property_const));
}

uint64_t find_function(uint64_t type, const char* name) { return find_function_guarded(type, name, {}); }

uint64_t find_function_guarded(uint64_t type, const char* name, CallGuard guard) {
    if (!guard.permits()) return 0;
    if (!live(type) || !name || !t::bound(t::fn::name_from_text) || !t::bound(t::fn::class_find_function)) return 0;
    char16_t wide[kNameLength + 1];
    size_t length = 0;
    for (; name[length]; ++length) {
        if (length >= kNameLength || uint8_t(name[length]) >= 0x80) return 0; // the engine's function names are plain ASCII
        wide[length] = char16_t(uint8_t(name[length]));
    }
    wide[length] = 0;
    if (!guard.permits()) return 0;
    const uint64_t engine_name = t::fn::name_from_text(wide, length, 1);
    if (!guard.permits() || !engine_name || !live(type)) return 0;
    const auto function = reinterpret_cast<uint64_t>(t::fn::class_find_function(reinterpret_cast<void*>(type), engine_name, 1));
    return guard.permits() && is_a(function, function_class) ? function : 0;
}

uint64_t default_object(uint64_t type) {
    if (!is_a(type, class_class)) return 0;
    const uint64_t object = read_or<uint64_t>(type + t::class_type::default_object);
    return live(object) ? object : 0;
}

TSUCHI_Result make_arguments(uint64_t function, uint64_t& memory) { return make_arguments_guarded(function, memory, {}); }

TSUCHI_Result make_arguments_guarded(uint64_t function, uint64_t& memory, CallGuard guard) {
    memory = 0;
    if (!guard.permits()) return TSUCHI_NOT_READY;
    if (!is_a(function, function_class)) return TSUCHI_INVALID;
    const int32_t bytes = read_or<int32_t>(function + t::struct_type::bytes, -1);
    int32_t alignment = read_or<int32_t>(function + t::struct_type::alignment);
    if (bytes < 0 || bytes > 0x10000 || alignment < 0 || alignment > 0x1000 || (alignment & (alignment - 1))) return TSUCHI_NOT_READY;
    if (alignment < 8) alignment = 8;
    const auto initialize = method(function, t::struct_vtable::initialize);
    if (!engine_memory_ready() || !initialize || !method(function, t::struct_vtable::destroy)) return TSUCHI_NOT_READY;
    const uint64_t size = bytes ? (uint64_t(bytes) + uint32_t(alignment) - 1) & ~uint64_t(uint32_t(alignment) - 1) : uint32_t(alignment);
    if (!guard.permits()) return TSUCHI_NOT_READY;
    void* block = engine_allocate(size, uint32_t(alignment));
    if (!block) return guard.permits() ? TSUCHI_NO_MEMORY : TSUCHI_NOT_READY;
    if (!guard.permits()) { engine_release(reinterpret_cast<uint64_t>(block)); return TSUCHI_NOT_READY; }
    memset(block, 0, size_t(size));
    if (!guard.permits()) { engine_release(reinterpret_cast<uint64_t>(block)); return TSUCHI_NOT_READY; }
    initialize(reinterpret_cast<void*>(function), block, 1);
    if (!guard.permits()) {
        // These arguments are initialized and owned, even though their intended
        // object is gone. Their independent destruction must still finish.
        destroy_arguments(function, reinterpret_cast<uint64_t>(block));
        return TSUCHI_NOT_READY;
    }
    memory = reinterpret_cast<uint64_t>(block);
    return TSUCHI_OK;
}

void destroy_arguments(uint64_t function, uint64_t memory) {
    if (!memory) return;
    // Without its function the strings and arrays inside cannot be released, so nothing is.
    if (!is_a(function, function_class)) return;
    if (const auto destroy = method(function, t::struct_vtable::destroy)) destroy(reinterpret_cast<void*>(function), reinterpret_cast<void*>(memory), 1);
    engine_release(memory);
}

TSUCHI_Result call(uint64_t object, uint64_t function, uint64_t arguments) { return call_guarded(object, function, arguments, {}); }

namespace {
TSUCHI_Result call_with(uint64_t object, uint64_t function, uint64_t arguments, CallGuard guard, bool object_dispatch) {
    if (!guard.permits()) return TSUCHI_NOT_READY;
    if (!is_a(function, function_class) || !arguments) return TSUCHI_INVALID;
    if (!live(object)) return TSUCHI_NOT_FOUND;
    const uint64_t owner = outer_of(function); // the class the function belongs to
    uint64_t target = object;
    if (read_or<uint32_t>(function + t::function::flags) & t::constant::function_static) {
        target = default_object(is_a(object, class_class) ? object : owner);
        if (!target) return TSUCHI_NOT_READY;
    }
    if (!derives_from(class_of(target), owner)) return TSUCHI_INVALID;
    auto process = method(target, t::object_vtable::process_event);
    if (object_dispatch) {
        // A function and a class are objects that keep the dispatch every object has. Two of them have to agree.
        const auto plain = method(function, t::object_vtable::process_event);
        if (!plain || plain != method(class_of(target), t::object_vtable::process_event)) return TSUCHI_UNSUPPORTED;
        process = plain;
    }
    if (!process) return TSUCHI_NOT_READY;
    if (!guard.permits()) return TSUCHI_NOT_READY;
    process(reinterpret_cast<void*>(target), reinterpret_cast<void*>(function), reinterpret_cast<void*>(arguments));
    return guard.permits() ? TSUCHI_OK : TSUCHI_NOT_READY;
}
} // namespace

TSUCHI_Result call_guarded(uint64_t object, uint64_t function, uint64_t arguments, CallGuard guard) {
    return call_with(object, function, arguments, guard, false);
}

TSUCHI_Result call_object_dispatch(uint64_t object, uint64_t function, uint64_t arguments) {
    return call_with(object, function, arguments, {}, true);
}

TSUCHI_Result write_argument_guarded(const Place& place, const Value& value, CallGuard guard) {
    if (!guard.permits()) return TSUCHI_NOT_READY;
    const PropertyKind kind = stored_kind(place.property);
    // These fixed restoration parameters do not allocate, invoke native code,
    // or follow soft-reference/string paths while writing argument memory.
    if (kind != PropertyKind::boolean && !is_integer(kind) && kind != PropertyKind::float32 &&
        kind != PropertyKind::float64 && kind != PropertyKind::object) return TSUCHI_UNSUPPORTED;
    if (!guard.permits()) return TSUCHI_NOT_READY;
    return write_value(place, value);
}

void* engine_memory(uint64_t bytes) { return engine_allocate(bytes, 0); }
void* engine_memory_aligned(uint64_t bytes, uint32_t alignment) { return engine_allocate(bytes, alignment); }
bool engine_memory_available() { return engine_memory_ready(); }
void release_engine_memory(void* block) { engine_release(reinterpret_cast<uint64_t>(block)); }

} // namespace tsuchi::game
