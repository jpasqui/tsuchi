// The "set" of a changes file: a value of one of the game's actors is given another.
//
//   {"levels": {"Cave_Light": {"set": {"Lamp_2": {"Intensity": 4.5, "bHidden": false, "Light.Label": "warm"}}}}}
//
// A value is named by a path as the reflect service takes one, from the actor, and every read and write is the
// mod's own, through that service. A path may lead through a reference into another object, such as an asset
// that several maps use and that stays loaded between them. So a say is kept by where its path ends, the last
// object along it and the path from there: that is what tells two says on one value apart from two values, and
// what is still there to be put back once the actor is gone.
//
// Several mods may have a say on a value. The say of the mod started last is in force, and when that mod stops
// the next one's is. What the value was before any say is remembered, and put back when no say is left or the
// world is left. A mod that is stopping can ask for nothing more, so a value put back as its last say goes is
// written by the framework itself.
#include "map_values.h"

#include <string.h>

#include "actor_events.h"
#include "host.h"
#include "json.h"
#include "list.h"
#include "map_tools.h"
#include "objects.h"
#include "object_retention.h"
#include "reflect.h"
#include "reflection.h"
#include "registry.h"
#include "text.h"

namespace tsuchi {
namespace {

using maps::join;
using maps::tell;

// A mod's say on one value.
struct Say {
    uint64_t part;
    TSUCHI_Mod* mod;
    uint64_t source; // the actor whose entry made this say
    uint64_t object; // where its path ends: the last object along it
    Text path;       // and the path from there
    const Json* value, *actor, *level; // as the changes file has them, which is the part's own
    bool fresh;      // said since the values were last settled
    bool counted, told;
    TSUCHI_Value wanted;
    bool text;
};
// What a value was before any mod's say.
struct Original {
    uint64_t object;
    Text path;
    bool was_text, changed;
    bool retained;
    TSUCHI_Value before;
    Text before_text;
};
List<Say> says;
List<Original> originals;
ValueCounts counts{};

// Field names ignore case, so two spellings of a path are one value.
bool same_path(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char x = *a >= 'A' && *a <= 'Z' ? char(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? char(*b + 32) : *b;
        if (x != y) return false;
        if (!x) return true;
    }
}

// ---- Reads and writes, for the mod that asks ----------------------------------------------------------
//
// With no mod that can ask, the framework reads and writes itself.

TSUCHI_Result read(TSUCHI_Mod* mod, uint64_t object, const char* path, TSUCHI_Value& out) {
    if (mod_usable(mod)) return host.reflection->get(mod, TSUCHI_Object{object}, path, &out);
    const uint64_t address = game::from_handle(object);
    return address ? read_at(game::type_of(address), address, path, &out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result read_text(TSUCHI_Mod* mod, uint64_t object, const char* path, Text& out, uint32_t bytes) {
    uint64_t needed = 0;
    if (!out.reserve(bytes ? bytes : 1)) return TSUCHI_NO_MEMORY;
    if (mod_usable(mod)) return host.reflection->get_text(mod, TSUCHI_Object{object}, path, out.data, out.capacity, &needed);
    const uint64_t address = game::from_handle(object);
    return address ? text_at(game::type_of(address), address, path, out.data, out.capacity, &needed) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result write(TSUCHI_Mod* mod, uint64_t object, const char* path, const TSUCHI_Value& value) {
    if (mod_usable(mod)) return host.reflection->set(mod, TSUCHI_Object{object}, path, &value);
    const uint64_t address = game::from_handle(object);
    return address ? write_at(game::type_of(address), address, path, &value) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result write_text(TSUCHI_Mod* mod, uint64_t object, const char* path, const char* text) {
    if (mod_usable(mod)) return host.reflection->set_text(mod, TSUCHI_Object{object}, path, text);
    const uint64_t address = game::from_handle(object);
    return address ? write_text_at(game::type_of(address), address, path, text) : TSUCHI_NOT_FOUND;
}

// Where a path from an actor ends: the last object a reference along it leads to, and the path from there.
// False when the path leads nowhere.
bool land(TSUCHI_Mod* mod, uint64_t actor, const char* path, uint64_t& object, const char*& rest) {
    object = actor, rest = path;
    char before[game::kNameBytes];
    for (size_t i = 0; rest[i]; ++i) {
        if (rest[i] != '.') continue;
        if (i >= sizeof before) return false;
        memcpy(before, rest, i);
        before[i] = 0;
        TSUCHI_Value at{};
        if (host.reflection->get(mod, TSUCHI_Object{object}, before, &at) != TSUCHI_OK) return false;
        if (at.kind != TSUCHI_VALUE_OBJECT) continue;
        object = at.as.object.id, rest += i + 1, i = 0;
        if (!rest[0]) return false;
    }
    return true;
}

// ---- Says and what is in force ------------------------------------------------------------------------

// The say in force on a value: that of the mod started last, and of two of one mod, the one made last.
Say* in_force(uint64_t object, const char* path) {
    Say* last = nullptr;
    for (uint32_t i = 0; i < says.count; ++i)
        if (says[i].object == object && same_path(says[i].path.get(), path) && (!last || mod_rank(says[i].mod) >= mod_rank(last->mod))) last = &says[i];
    return last;
}

Original* original_of(uint64_t object, const char* path) {
    for (uint32_t i = 0; i < originals.count; ++i)
        if (originals[i].object == object && same_path(originals[i].path.get(), path)) return &originals[i];
    return nullptr;
}

void forget(Say* say) {
    say->path.clear();
    says.remove(say);
}

void forget(Original* original) {
    if (original->retained) release_game_object(original->before.as.object.id);
    original->path.clear(), original->before_text.clear();
    originals.remove(original);
}

// Remembers what a value holds, before the first say on it is written.
TSUCHI_Result remember(TSUCHI_Mod* mod, uint64_t object, const char* path) {
    Original original{object, {}, false, false, false, {}, {}};
    TSUCHI_Result result = read(mod, object, path, original.before);
    if (result != TSUCHI_OK) return result;
    if (original.before.kind == TSUCHI_VALUE_STRUCT || original.before.kind == TSUCHI_VALUE_ARRAY) return TSUCHI_UNSUPPORTED; // its parts are set one by one
    if (original.before.kind == TSUCHI_VALUE_TEXT) {
        original.was_text = true;
        result = read_text(mod, object, path, original.before_text, original.before.count);
    }
    if (result == TSUCHI_OK && original.before.kind == TSUCHI_VALUE_OBJECT) {
        original.retained = retain_game_object(original.before.as.object.id);
        if (!original.retained) result = TSUCHI_NO_MEMORY;
    }
    if (result == TSUCHI_OK && !original.path.set(path)) result = TSUCHI_NO_MEMORY;
    if (result == TSUCHI_OK && !originals.add(original)) result = TSUCHI_NO_MEMORY;
    if (result != TSUCHI_OK) {
        if (original.retained) release_game_object(original.before.as.object.id);
        original.path.clear(), original.before_text.clear();
    }
    return result;
}

TSUCHI_Result write_say(const Say& say) {
    return say.text ? write_text(say.mod, say.object, say.path.get(), say.value->text)
                    : write(say.mod, say.object, say.path.get(), say.wanted);
}

// Why a value was not set, in words for the mod's author.
const char* why(TSUCHI_Result result) {
    switch (result) {
    case TSUCHI_NOT_FOUND: return "the actor has no such value";
    case TSUCHI_INVALID: return "the value is of the wrong kind for it";
    case TSUCHI_UNSUPPORTED: return "it is of a kind that does not take such a value";
    case TSUCHI_NO_MEMORY: return "there is no memory for it";
    default: return "the game refused it";
    }
}

// A line about one value in the log of the mod whose changes file names it.
void tell_of(TSUCHI_Mod* mod, const Json& value, const Json& actor, const Json& level, const char* what, const char* more = "", const char* end = "") {
    Text which;
    if (join(which, value.name, " of ", actor.name, " in ", level.name)) tell(mod, "maps: ", which.get(), what, more, end);
    which.clear();
}

// Writes what a value was, where anything else was written there and what holds it is still there, and forgets it.
void put_back(Original* original, TSUCHI_Mod* mod) {
    if (original->changed && game::from_handle(original->object)) {
        const TSUCHI_Result result = original->was_text ? write_text(mod, original->object, original->path.get(), original->before_text.get())
                                                        : write(mod, original->object, original->path.get(), original->before);
        if (result == TSUCHI_OK) ++counts.put_back;
    }
    forget(original);
}

// Puts in force what is in force for a value: the say of the mod started last, or with no say left, what the
// value was. A say the game will not take is told in its mod's log and dropped, and the next one is tried.
void settle(uint64_t object, const char* path) {
    for (;;) {
        Say* const last = in_force(object, path);
        Original* original = original_of(object, path);
        if (!last) {
            if (original) put_back(original, nullptr);
            return;
        }
        TSUCHI_Result result = original ? TSUCHI_OK : remember(last->mod, object, path);
        if (result == TSUCHI_OK) result = write_say(*last);
        if (result == TSUCHI_OK) {
            original_of(object, path)->changed = true;
            if (!last->counted) {
                ++counts.set;
                tell_of(last->mod, *last->value, *last->actor, *last->level, " was set for this map.");
            }
            last->counted = true;
            for (uint32_t i = 0; i < says.count; ++i) {
                Say& other = says[i];
                if (&other == last || other.told || other.object != object || !same_path(other.path.get(), path)) continue;
                other.told = true;
                tell_of(other.mod, *other.value, *other.actor, *other.level, " is set by ", mod_id(last->mod), ", not by this mod");
            }
            return;
        }
        ++counts.refused;
        tell_of(last->mod, *last->value, *last->actor, *last->level, " could not be set: ", why(result));
        forget(last);
    }
}

void say_value(uint64_t part, TSUCHI_Mod* mod, uint64_t source, const Json& level, const Json& actor, const Json& value, const char* path) {
    uint64_t object = 0;
    const char* rest = nullptr;
    TSUCHI_Value held{}, wanted{};
    const char* problem = nullptr;
    TSUCHI_Result result = land(mod, source, path, object, rest) ? read(mod, object, rest, held) : TSUCHI_NOT_FOUND;
    if (result == TSUCHI_OK && value.kind == Json::Kind::object && held.kind == TSUCHI_VALUE_STRUCT) {
        for (const Json* field = value.first; field; field = field->next) {
            Text child;
            if (join(child, path, ".", field->name)) say_value(part, mod, source, level, actor, *field, child.get());
            else { ++counts.refused; tell_of(mod, *field, actor, level, " could not be set: there is no memory for its path"); }
            child.clear();
        }
        return;
    }
    bool text = value.kind == Json::Kind::text && held.kind == TSUCHI_VALUE_TEXT;
    if (result == TSUCHI_OK) {
        const uint64_t address = game::from_handle(object);
        game::Place place;
        result = address ? game::find_place(game::type_of(address), address, rest, place) : TSUCHI_NOT_FOUND;
        const Json* asset = value.kind == Json::Kind::object && value.count == 1 ? value.find("asset") : nullptr;
        if (result == TSUCHI_OK && (asset || (value.kind == Json::Kind::text && game::object_property(place)))) {
            const Json& path_value = asset ? *asset : value;
            if (path_value.kind != Json::Kind::text || path_value.text[0] != '/') result = TSUCHI_INVALID;
            else {
                wanted.kind = TSUCHI_VALUE_OBJECT;
                result = host.asset->load(mod, path_value.text, &wanted.as.object);
                if (result != TSUCHI_OK) problem = "the asset path could not be loaded";
            }
            text = false;
        } else if (result == TSUCHI_OK && value.kind == Json::Kind::text && held.kind == TSUCHI_VALUE_INTEGER) {
            wanted.kind = TSUCHI_VALUE_INTEGER;
            result = game::enum_number(place, value.text, wanted.as.integer);
            if (result != TSUCHI_OK) problem = "the field has no enum member with that name";
        } else if (result == TSUCHI_OK && value.kind == Json::Kind::boolean) {
            wanted.kind = TSUCHI_VALUE_BOOLEAN, wanted.as.boolean = value.boolean;
        } else if (result == TSUCHI_OK && value.kind == Json::Kind::number) {
            wanted.kind = value.whole ? TSUCHI_VALUE_INTEGER : TSUCHI_VALUE_NUMBER;
            if (value.whole) wanted.as.integer = value.integer;
            else wanted.as.number = value.number;
        } else if (result == TSUCHI_OK && !text) {
            result = TSUCHI_UNSUPPORTED;
            problem = "use a scalar, an asset path, an enum name, or fields of a struct";
        }
    }
    for (uint32_t i = says.count; result == TSUCHI_OK && i-- > 0;)
        if (says[i].part == part && says[i].source == source && says[i].object == object && same_path(says[i].path.get(), rest)) forget(&says[i]);
    if (result == TSUCHI_OK) {
        Say say{part, mod, source, object, {}, &value, &actor, &level, true, false, false, wanted, text};
        if (!say.path.set(rest) || !says.add(say)) { say.path.clear(); result = TSUCHI_NO_MEMORY; }
    }
    if (result != TSUCHI_OK) {
        ++counts.refused;
        Text description;
        if (join(description, path, " of ", actor.name, " in ", level.name))
            tell(mod, "maps: ", description.get(), " could not be set: ", problem ? problem : why(result));
        description.clear();
    }
}

// Remove the source actor's says while its referenced assets and fields still resolve.
void left(uint64_t, uint64_t handle, ActorLeft) {
    for (uint32_t i = says.count; i-- > 0;) {
        if (i >= says.count || says[i].source != handle) continue;
        const uint64_t object = says[i].object;
        Text path = says[i].path;
        says[i].path = Text{};
        says.remove(&says[i]);
        settle(object, path.get());
        path.clear();
        i = says.count;
    }
}

} // namespace

bool map_value_well_formed(const Json& value) {
    if (value.kind == Json::Kind::number || value.kind == Json::Kind::boolean) return true;
    if (value.kind == Json::Kind::text) return strlen(value.text) == value.bytes;
    if (value.kind != Json::Kind::object || !value.count) return false;
    for (const Json* field = value.first; field; field = field->next)
        if (!field->name[0] || !map_value_well_formed(*field)) return false;
    return true;
}

void values_say(uint64_t part, TSUCHI_Mod* mod, uint64_t actor, const Json& level, const Json& values) {
    static bool watching = false;
    if (!watching) watching = on_actor_left(left);
    if (!watching) {
        for (const Json* value = values.first; value; value = value->next) {
            ++counts.refused;
            tell_of(mod, *value, values, level, " could not be set: ", why(TSUCHI_NO_MEMORY));
        }
        return;
    }
    for (const Json* value = values.first; value; value = value->next)
        say_value(part, mod, actor, level, values, *value, value->name);
}

void values_settle() {
    // What held a value may be gone, with a sublevel the game let go of: so are its says.
    for (uint32_t i = says.count; i-- > 0;)
        if (!game::from_handle(says[i].object)) forget(&says[i]);
    for (uint32_t i = originals.count; i-- > 0;)
        if (!game::from_handle(originals[i].object)) forget(&originals[i]);
    for (;;) {
        Say* fresh = nullptr;
        for (uint32_t i = 0; i < says.count && !fresh; ++i)
            if (says[i].fresh) fresh = &says[i];
        if (!fresh) return;
        const uint64_t object = fresh->object;
        Text path;
        const bool kept = path.set(fresh->path.get());
        for (uint32_t i = 0; i < says.count; ++i)
            if (says[i].object == object && same_path(says[i].path.get(), fresh->path.get())) says[i].fresh = false;
        if (kept) settle(object, path.get());
        path.clear();
    }
}

void values_end(uint64_t part) {
    for (uint32_t i = says.count; i-- > 0;) {
        if (i >= says.count || says[i].part != part) continue; // settling may have dropped others
        const uint64_t object = says[i].object;
        Text path = says[i].path; // kept past the say itself, for the look at what is left
        says[i].path = Text{};
        says.remove(&says[i]);
        settle(object, path.get());
        path.clear();
        i = says.count; // the list has moved: from its end again
    }
}

void values_put_back() {
    for (uint32_t i = originals.count; i-- > 0;) {
        const Say* const last = in_force(originals[i].object, originals[i].path.get());
        put_back(&originals[i], last ? last->mod : nullptr);
    }
    for (uint32_t i = says.count; i-- > 0;) forget(&says[i]);
    says.clear(), originals.clear();
}

ValueCounts value_counts() { return counts; }

} // namespace tsuchi
