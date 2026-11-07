// The game side of the tables service: rows read and changed through the same
// paths reflection uses, with a row's memory in place of an object's.

#include <string.h>

#include "game.h"
#include "list.h"
#include "objects.h"
#include "reflect.h"
#include "reflection.h"
#include "registry.h"
#include "services.h"
#include "datatable_service.h"
#include "text.h"

namespace tsuchi {
namespace {

namespace t = game::table;

const Kind kChange{"tables.change"};
game::ClassRef table_class{t::name::data_table_class};

// A mod's say on a field: what it wants the field to hold. Several mods can have a say on the same field. The say
// of the mod started last is in force, and when that mod takes its say back the next one's is.
struct Change {
    uint64_t id;
    TSUCHI_Mod* mod;
    uint64_t table; // a handle
    uint64_t row;   // the row's name, as the game keeps it
    Text path;
    bool is_text;
    TSUCHI_Value wanted;
    Text wanted_text;
};
// What a field held before any mod had a say on it.
struct Original {
    uint64_t table, row;
    Text path;
    bool was_text;
    TSUCHI_Value before;
    Text before_text;
};
List<Change> changes;
List<Original> originals;
uint64_t next_change = 1;

// Field names ignore case, so two spellings of a path are one field.
bool same_path(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char x = *a >= 'A' && *a <= 'Z' ? char(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? char(*b + 32) : *b;
        if (x != y) return false;
        if (!x) return true;
    }
}

// A mod's say on a field, or with no mod given, the say that is in force: that of the mod started last.
Change* find(TSUCHI_Mod* mod, uint64_t table, uint64_t row, const char* path) {
    Change* found = nullptr;
    for (uint32_t i = 0; i < changes.count; ++i) {
        Change& each = changes[i];
        if (each.table != table || each.row != row || !same_path(each.path.get(), path)) continue;
        if (mod ? each.mod == mod : !found || mod_rank(each.mod) > mod_rank(found->mod)) found = &each;
    }
    return found;
}

Original* find_original(uint64_t table, uint64_t row, const char* path) {
    for (uint32_t i = 0; i < originals.count; ++i)
        if (originals[i].table == table && originals[i].row == row && same_path(originals[i].path.get(), path)) return &originals[i];
    return nullptr;
}

// Where a row is and what describes it.
struct Row {
    uint64_t name = 0, type = 0, base = 0;
};
TSUCHI_Result find_row(TSUCHI_Object handle, const char* row, Row& out) {
    const uint64_t table = game::from_handle(handle.id);
    if (!table || !game::is_a(table, table_class)) return TSUCHI_NOT_FOUND;
    game::Name name{};
    const TSUCHI_Result named = game::name_from_utf8(row, name);
    if (named != TSUCHI_OK) return named == TSUCHI_CAPACITY ? TSUCHI_NOT_FOUND : named; // a name the game cannot store names no row
    if (!t::bound(t::fn::find_table_row)) return TSUCHI_NOT_READY;
    memcpy(&out.name, &name, sizeof name);
    out.type = game::read_or<uint64_t>(table + t::data_table::row_struct);
    out.base = out.name ? reinterpret_cast<uint64_t>(t::fn::find_table_row(reinterpret_cast<void*>(table), out.name)) : 0;
    return out.type && game::live(out.type) && out.base ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}

TSUCHI_Result get(TSUCHI_Object table, const char* row, const char* path, TSUCHI_Value* out) {
    Row at;
    const TSUCHI_Result found = find_row(table, row, at);
    return found == TSUCHI_OK ? read_at(at.type, at.base, path, out) : found;
}

TSUCHI_Result get_text(TSUCHI_Object table, const char* row, const char* path, char* buffer, uint64_t capacity, uint64_t* out) {
    Row at;
    const TSUCHI_Result found = find_row(table, row, at);
    return found == TSUCHI_OK ? text_at(at.type, at.base, path, buffer, capacity, out) : found;
}

TSUCHI_Result field_at(TSUCHI_Object table, const char* row, const char* path, uint32_t index, char* buffer, uint64_t capacity, uint64_t* out) {
    Row at;
    const TSUCHI_Result found = find_row(table, row, at);
    return found == TSUCHI_OK ? field_name_at(at.type, at.base, path, index, true, buffer, capacity, out) : found;
}

// Writes into the row what is in force for a field: the say of the mod started last, or with no say left, what
// the field held at first, which is then forgotten. Nothing is written when the table or the row is gone.
void put_in_force(uint64_t handle, uint64_t row, const char* path) {
    const Change* const last = find(nullptr, handle, row, path);
    Original* const original = find_original(handle, row, path);
    const uint64_t table = game::from_handle(handle);
    const uint64_t type = table ? game::read_or<uint64_t>(table + t::data_table::row_struct) : 0;
    const auto base = table && t::bound(t::fn::find_table_row) ? reinterpret_cast<uint64_t>(t::fn::find_table_row(reinterpret_cast<void*>(table), row)) : 0;
    if (type && base) {
        if (last) {
            if (last->is_text) write_text_at(type, base, path, last->wanted_text.get());
            else write_at(type, base, path, &last->wanted);
        } else if (original) {
            if (original->was_text) write_text_at(type, base, path, original->before_text.get());
            else write_at(type, base, path, &original->before);
        }
    }
    if (!last && original) {
        original->path.clear(), original->before_text.clear();
        originals.remove(original);
    }
}

// Takes a mod's say out, and puts in force what is in force without it.
void drop(Change* change) {
    const uint64_t table = change->table, row = change->row;
    Text path = change->path; // kept past the change itself, for the look at what is left
    change->path = Text{};
    change->wanted_text.clear();
    changes.remove(change);
    put_in_force(table, row, path.get());
    path.clear();
}

// The mod stopped while it had a say on the field.
void undo(void*, uint64_t id) {
    for (uint32_t i = 0; i < changes.count; ++i) {
        if (changes[i].id != id) continue;
        drop(&changes[i]);
        return;
    }
}

// A compound write prepares desired values, originals, and ownership before publishing any of them.
struct PendingChange {
    Change change{};
    Original original{};
    bool new_change = false, new_original = false;
};
struct PendingChanges {
    List<PendingChange> items;
    ~PendingChanges() {
        for (uint32_t i = 0; i < items.count; ++i) {
            items[i].change.path.clear(), items[i].change.wanted_text.clear();
            items[i].original.path.clear(), items[i].original.before_text.clear();
        }
        items.clear();
    }
};
TSUCHI_Result stage_original(TSUCHI_Object table, const Row& row, const char* path, const game::Place& place, Original& original) {
    original.table = table.id, original.row = row.name;
    game::Value before;
    TSUCHI_Result result = game::read_value(place, before);
    if (result != TSUCHI_OK) return result;
    original.before.kind = uint32_t(before.kind), original.before.count = before.count;
    switch (before.kind) {
    case game::ValueKind::boolean: original.before.as.boolean = before.boolean; break;
    case game::ValueKind::integer: original.before.as.integer = before.integer; break;
    case game::ValueKind::number: original.before.as.number = before.number; break;
    case game::ValueKind::object: original.before.as.object.id = before.object; break;
    default: break;
    }
    if (original.before.kind == TSUCHI_VALUE_STRUCT || original.before.kind == TSUCHI_VALUE_ARRAY) return TSUCHI_UNSUPPORTED;
    if (original.before.kind == TSUCHI_VALUE_TEXT) {
        size_t needed = 0;
        original.was_text = true;
        if (!original.before_text.reserve(original.before.count ? original.before.count : 1)) return TSUCHI_NO_MEMORY;
        result = game::read_text(place, original.before_text.data, original.before_text.capacity, &needed);
    }
    return result == TSUCHI_OK && !original.path.set(path) ? TSUCHI_NO_MEMORY : result;
}
struct ScriptRank {
    TSUCHI_Mod* mod;
    uint64_t table, row;
};
TSUCHI_Result stage_rank(void* context, game::WritePlan& plan, uint32_t index) {
    const ScriptRank& rank = *static_cast<ScriptRank*>(context);
    const Change* last = find(nullptr, rank.table, rank.row, plan.fields[index].path.get());
    if (!last || last->mod == rank.mod || mod_rank(last->mod) <= mod_rank(rank.mod)) return TSUCHI_OK;
    ScriptWrite effective;
    effective.kind = last->is_text ? ScriptWriteKind::text : ScriptWriteKind::value;
    effective.value = last->wanted;
    effective.text.data = last->wanted_text.data;
    return plan.replace_field(index, effective);
}
TSUCHI_Result script_override(TSUCHI_Mod* mod, const ScriptTarget& target, const char* path, const ScriptWrite& value) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    Row row;
    TSUCHI_Result result = find_row(target.object, target.row, row);
    if (result != TSUCHI_OK) return result;
    if (value.kind == ScriptWriteKind::value || value.kind == ScriptWriteKind::text) {
        TSUCHI_Value before{};
        result = read_at(row.type, row.base, path, &before);
        // An existing original may describe a field now deliberately cleared to an unreadable name.
        // Fresh originals still require a successful snapshot after native preparation.
        if (result == TSUCHI_OK && (before.kind == TSUCHI_VALUE_STRUCT || before.kind == TSUCHI_VALUE_ARRAY)) return TSUCHI_UNSUPPORTED;
    }
    game::WritePlan plan;
    ScriptRank rank{mod, target.object.id, row.name};
    plan.field_ready = stage_rank, plan.field_context = &rank;
    if ((result = plan.prepare(row.type, row.base, path, value, false)) != TSUCHI_OK) return result;
    PendingChanges pending;
    uint32_t fresh_changes = 0, fresh_originals = 0;
    for (uint32_t i = 0; i < plan.fields.count; ++i) {
        const game::PreparedField& field = plan.fields[i];
        PendingChange each;
        each.change.mod = mod, each.change.table = target.object.id, each.change.row = row.name;
        each.change.is_text = field.wanted->kind == ScriptWriteKind::text;
        each.change.wanted = field.wanted->value;
        if (!each.change.path.set(field.path.get()) || (each.change.is_text && !each.change.wanted_text.set(field.wanted->text.get()))) {
            each.change.path.clear(), each.change.wanted_text.clear();
            return TSUCHI_NO_MEMORY;
        }
        const Change* existing = find(mod, target.object.id, row.name, field.path.get());
        if (existing && !ownership_update_allowed(mod, kChange, existing->id)) {
            each.change.path.clear(), each.change.wanted_text.clear();
            return TSUCHI_BUSY;
        }
        each.new_change = !existing;
        each.change.id = existing ? existing->id : next_change + fresh_changes++;
        each.new_original = !find_original(target.object.id, row.name, field.path.get());
        if (each.new_original) {
            result = stage_original(target.object, row, field.path.get(), field.destination, each.original);
            if (result != TSUCHI_OK) {
                each.change.path.clear(), each.change.wanted_text.clear();
                each.original.path.clear(), each.original.before_text.clear();
                return result;
            }
            ++fresh_originals;
        }
        if (!pending.items.add(each)) {
            each.change.path.clear(), each.change.wanted_text.clear();
            each.original.path.clear(), each.original.before_text.clear();
            return TSUCHI_NO_MEMORY;
        }
    }
    if (fresh_changes > UINT32_MAX - changes.count || fresh_originals > UINT32_MAX - originals.count) return TSUCHI_CAPACITY;
    if (!changes.reserve(changes.count + fresh_changes) || !originals.reserve(originals.count + fresh_originals)) return TSUCHI_NO_MEMORY;
    if ((result = reserve_owned(mod, fresh_changes)) != TSUCHI_OK) return result;
    // Capacity is reserved and no native or script callback runs while records are installed.
    for (uint32_t i = 0; i < pending.items.count; ++i) {
        PendingChange& each = pending.items[i];
        if (each.new_original) { originals.add(each.original); each.original = Original{}; }
        Change* previous = find(mod, target.object.id, row.name, each.change.path.get());
        if (previous) {
            each.change.path.clear();
            each.change.path = previous->path; // retain the established spelling
            previous->wanted_text.clear();
            *previous = each.change;
        } else {
            changes.add(each.change);
            own(mod, kChange, each.change.id, undo, nullptr);
        }
        each.change = Change{};
    }
    next_change += fresh_changes;
    plan.commit();
    return TSUCHI_OK;
}
TSUCHI_Result set(TSUCHI_Mod* mod, TSUCHI_Object table, const char* row, const char* path, const TSUCHI_Value* value) {
    ScriptWrite wanted;
    wanted.value = *value;
    return script_override(mod, {table, {}, row}, path, wanted);
}
TSUCHI_Result set_text(TSUCHI_Mod* mod, TSUCHI_Object table, const char* row, const char* path, const char* text) {
    if (!text) return TSUCHI_INVALID;
    ScriptWrite wanted;
    wanted.kind = ScriptWriteKind::text;
    wanted.text.data = const_cast<char*>(text); // borrowed for this synchronous preparation
    return script_override(mod, {table, {}, row}, path, wanted);
}

TSUCHI_Result restore(TSUCHI_Mod* mod, TSUCHI_Object table, const char* row, const char* path) {
    game::Name name{};
    uint64_t packed = 0;
    if (game::name_from_utf8(row, name) != TSUCHI_OK) return TSUCHI_NOT_FOUND;
    memcpy(&packed, &name, sizeof name);
    Change* mine = find(mod, table.id, packed, path);
    if (!mine) return table_restore_length(mod, table, row, path); // a field has a change or a length, never both: an array is not changed whole
    disown(mod, kChange, mine->id);
    drop(mine);
    return TSUCHI_OK;
}

// True for a table that keeps its rows the plain way: its own functions for
// adding and removing rows are the engine's.
bool plain_table(uint64_t table) {
    namespace f = t::data_table_table;
    const uint64_t functions = game::read_or<uint64_t>(table);
    const auto is = [&](uint64_t slot, auto* function) {
        return function && game::read_or<uint64_t>(functions + slot) == reinterpret_cast<uint64_t>(function);
    };
    return functions && is(f::add_row, t::fn::add_table_row) && is(f::remove_row, t::fn::remove_table_row) &&
           is(f::add_row_internal, t::fn::add_table_row_internal);
}

TSUCHI_Result add_row(TSUCHI_Object handle, const char* row, const char* from) {
    Row source;
    const TSUCHI_Result found = find_row(handle, from, source);
    if (found != TSUCHI_OK) return found;
    const uint64_t table = game::from_handle(handle.id);
    game::Name name{};
    uint64_t packed = 0;
    const TSUCHI_Result named = game::name_from_utf8(row, name);
    if (named != TSUCHI_OK) return named == TSUCHI_CAPACITY ? TSUCHI_INVALID : named;
    memcpy(&packed, &name, sizeof name);
    if (!packed || t::fn::find_table_row(reinterpret_cast<void*>(table), packed)) return TSUCHI_INVALID;
    if (!plain_table(table)) return TSUCHI_UNSUPPORTED;
    t::fn::add_table_row(reinterpret_cast<void*>(table), packed, reinterpret_cast<void*>(source.base));
    const auto made = reinterpret_cast<uint64_t>(t::fn::find_table_row(reinterpret_cast<void*>(table), packed));
    return made && made != source.base ? TSUCHI_OK : TSUCHI_NOT_READY;
}

const datatable::Backend service{get, get_text, field_at, set, set_text, restore, add_row, table_resize};

// ---- How long an array in a row is -------------------------------------------------------------------
//
// A mod's say on the length of an array, which is a hold like a change to a field: the say of the mod started
// last is in force, and with none left the row has the array it had at first. That array is set aside whole
// while any say stands, so nothing of it is lost. Meanwhile the row has an array of the framework's making, in
// the engine's memory: copies of the first array's elements up to the length in force, new elements past them,
// and on top the says mods have on single elements.

const Kind kLength{"tables.length"};
struct Length {
    uint64_t id;
    TSUCHI_Mod* mod;
    uint64_t table, row; // as in a change
    Text path;
    uint32_t count;
};
// The array a row had before any mod had a say on its length.
struct Aside {
    uint64_t table, row;
    Text path;
    game::ArrayMemory first;
};
List<Length> lengths;
List<Aside> asides;
uint64_t next_length = 1;

Length* find_length(TSUCHI_Mod* mod, uint64_t table, uint64_t row, const char* path) {
    Length* found = nullptr;
    for (uint32_t i = 0; i < lengths.count; ++i) {
        Length& each = lengths[i];
        if (each.table != table || each.row != row || !same_path(each.path.get(), path)) continue;
        if (mod ? each.mod == mod : !found || mod_rank(each.mod) > mod_rank(found->mod)) found = &each;
    }
    return found;
}

Aside* find_aside(uint64_t table, uint64_t row, const char* path) {
    for (uint32_t i = 0; i < asides.count; ++i)
        if (asides[i].table == table && asides[i].row == row && same_path(asides[i].path.get(), path)) return &asides[i];
    return nullptr;
}

// True for the path of something in the array at `array`: one of its elements, or a part of one.
bool inside(const char* array, const char* path) {
    for (; *array; ++array, ++path) {
        const char x = *array >= 'A' && *array <= 'Z' ? char(*array + 32) : *array, y = *path >= 'A' && *path <= 'Z' ? char(*path + 32) : *path;
        if (x != y) return false;
    }
    return *path == '[';
}

// Stage element values through a detached array view. No header or live element is changed here.
TSUCHI_Result prepare_says(game::WritePlan& plan, uint64_t handle, uint64_t row, const char* array,
                          uint64_t type, uint64_t base, const game::Place& place, const game::ArrayMemory& view, bool original) {
    const uint32_t count = original ? originals.count : changes.count;
    for (uint32_t i = 0; i < count; ++i) {
        const char* path;
        ScriptWrite wanted;
        if (original) {
            const Original& first = originals[i];
            if (first.table != handle || first.row != row || !inside(array, first.path.get())) continue;
            path = first.path.get();
            wanted.kind = first.was_text ? ScriptWriteKind::text : ScriptWriteKind::value;
            wanted.value = first.before;
            wanted.text.data = first.before_text.data;
        } else {
            const Change& say = changes[i];
            if (say.table != handle || say.row != row || !inside(array, say.path.get()) || find(nullptr, handle, row, say.path.get()) != &say) continue;
            path = say.path.get();
            wanted.kind = say.is_text ? ScriptWriteKind::text : ScriptWriteKind::value;
            wanted.value = say.wanted;
            wanted.text.data = say.wanted_text.data;
        }
        game::Place member;
        TSUCHI_Result result = game::find_place_view(type, base, path, place.address, &view, member, &plan);
        if (result == TSUCHI_NOT_FOUND) continue; // an override past the active length stays recorded
        if (result != TSUCHI_OK) return result;
        // An object reached through an array reference keeps its own values, independently of array storage.
        if (original && (member.address < view.data || member.address >= view.data + uint64_t(view.count) * uint64_t(game::read_or<int32_t>(game::read_or<uint64_t>(place.property + t::property::inner) + t::property::element_bytes)))) continue;
        result = plan.prepare_field(member, path, wanted);
        if (result != TSUCHI_OK) return result;
    }
    return TSUCHI_OK;
}

// Replacement storage, originals and element overrides are all ready before publication.
TSUCHI_Result put_length_in_force(uint64_t handle, uint64_t row, const char* path, const Length* omitted = nullptr) {
    const Length* last = nullptr;
    for (uint32_t i = 0; i < lengths.count; ++i) {
        const Length& each = lengths[i];
        if (&each == omitted || each.table != handle || each.row != row || !same_path(each.path.get(), path)) continue;
        if (!last || mod_rank(each.mod) > mod_rank(last->mod)) last = &each;
    }
    Aside* aside = find_aside(handle, row, path);
    const uint64_t table = game::from_handle(handle);
    const uint64_t type = table ? game::read_or<uint64_t>(table + t::data_table::row_struct) : 0;
    const auto base = table && t::bound(t::fn::find_table_row) ? reinterpret_cast<uint64_t>(t::fn::find_table_row(reinterpret_cast<void*>(table), row)) : 0;
    game::Place place;
    if (!type || !base || game::find_place(type, base, path, place) != TSUCHI_OK) {
        if (!last && aside) { aside->path.clear(); asides.remove(aside); }
        return TSUCHI_NOT_FOUND;
    }
    if (!last && !aside) return TSUCHI_OK;
    if (!game::writable(place.address, t::array::size)) return TSUCHI_NOT_READY;
    game::ArrayMemory before;
    TSUCHI_Result result = game::array_of(place, before);
    if (result != TSUCHI_OK) return result;
    Aside made{handle, row, {}, before};
    const bool first = last && !aside;
    if (first && (!made.path.set(path) || !asides.reserve(asides.count + 1))) {
        made.path.clear(); return TSUCHI_NO_MEMORY;
    }
    game::ArrayMemory after = aside ? aside->first : before;
    const bool copied = last != nullptr;
    if (copied && (result = game::copy_array(place, after, last->count, after)) != TSUCHI_OK) {
        made.path.clear(); return result;
    }
    game::WritePlan normalize, effective;
    if (first) result = prepare_says(normalize, handle, row, path, type, base, place, before, true);
    if (result == TSUCHI_OK) result = prepare_says(effective, handle, row, path, type, base, place, after, false);
    if (result != TSUCHI_OK) {
        if (copied) game::free_array(place, after);
        made.path.clear(); return result;
    }
    normalize.commit();
    effective.commit();
    game::put_array(place, after);
    if (first) { asides.add(made); made.path = Text{}; }
    else {
        game::free_array(place, before);
        if (!last) { aside->path.clear(); asides.remove(aside); }
    }
    made.path.clear();
    return TSUCHI_OK;
}

// Takes a mod's say out, and puts in force what is in force without it.
void drop_length(Length* length) {
    const uint64_t table = length->table, row = length->row;
    Text path = length->path;
    length->path = Text{};
    lengths.remove(length);
    put_length_in_force(table, row, path.get());
    path.clear();
}

// The mod stopped while it had a say on the array's length.
void undo_length(void*, uint64_t id) {
    for (uint32_t i = 0; i < lengths.count; ++i) {
        if (lengths[i].id != id) continue;
        drop_length(&lengths[i]);
        return;
    }
}

} // namespace

TSUCHI_Result table_resize(TSUCHI_Mod* mod, TSUCHI_Object table, const char* row, const char* path, uint32_t count) {
    if (!mod_usable(mod) || !row || !path) return TSUCHI_INVALID;
    Row at;
    TSUCHI_Result result = find_row(table, row, at);
    if (result != TSUCHI_OK) return result;
    game::Place place;
    game::ArrayMemory now;
    if ((result = game::find_place(at.type, at.base, path, place)) != TSUCHI_OK || (result = game::array_of(place, now)) != TSUCHI_OK) return result;
    if (count > t::constant::max_array_count) return TSUCHI_CAPACITY;
    Length* mine = find_length(mod, table.id, at.name, path);
    if (mine && !ownership_update_allowed(mod, kLength, mine->id)) return TSUCHI_BUSY;
    const bool fresh = !mine;
    if (fresh) {
        Length made{next_length, mod, table.id, at.name, {}, count};
        if (made.path.set(path)) mine = lengths.add(made);
        result = mine ? own(mod, kLength, made.id, undo_length, nullptr) : TSUCHI_NO_MEMORY;
        if (result != TSUCHI_OK) {
            made.path.clear();
            if (mine) lengths.remove(mine);
            return result;
        }
        ++next_length;
    }
    const uint32_t before = mine->count;
    mine->count = count;
    result = put_length_in_force(table.id, at.name, path);
    if (result != TSUCHI_OK) { // a say that cannot be made leaves the array as it was in force
        if (fresh) {
            disown(mod, kLength, mine->id);
            mine->path.clear();
            lengths.remove(mine);
        } else {
            mine->count = before;
        }
        // Preparation has not touched the active array, so no fallible reapplication is needed.
    }
    return result;
}

TSUCHI_Result table_restore_length(TSUCHI_Mod* mod, TSUCHI_Object table, const char* row, const char* path) {
    game::Name name{};
    uint64_t packed = 0;
    if (!row || !path || game::name_from_utf8(row, name) != TSUCHI_OK) return TSUCHI_NOT_FOUND;
    memcpy(&packed, &name, sizeof name);
    Length* mine = find_length(mod, table.id, packed, path);
    if (!mine) return TSUCHI_NOT_FOUND;
    const TSUCHI_Result result = put_length_in_force(table.id, packed, path, mine);
    if (result != TSUCHI_OK && result != TSUCHI_NOT_FOUND) return result;
    disown(mod, kLength, mine->id);
    mine->path.clear();
    lengths.remove(mine);
    return TSUCHI_OK;
}

void tables_start() {
    datatable::set_backend(&service);
    table_script_writer = script_override;
}
unsigned table_changes() { return changes.count; }
unsigned table_lengths() { return lengths.count; }

} // namespace tsuchi
