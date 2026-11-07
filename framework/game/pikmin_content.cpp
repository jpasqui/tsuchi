#include "pikmin_content.h"
#include "pikmin_birth.h"
#include "pikmin_birth_context.h"
#include "pikmin_counts.h"
#include <string.h>
#include "actors.h"
#include "content_path.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "host.h"
#include "ids.h"
#include "mods.h"
#include "objects.h"
#include "pikmin_hud.h"
#include "pikmin_native.h"
#include "pikmin_save.h"
#include "pikmin_snapshot.h"
#include "pikmin_service.h"
#include "records.h"
#include "reflect.h"
#include "registry.h"
namespace tsuchi {
namespace {
namespace t = game::table;
struct Given {
    char id[kIdBytes]{};
    uint8_t color = 0, donor = 0;
    Written others{};
    uint64_t framework = 0, table = 0, donor_class = 0, active_class = 0;
};
Given given[t::most::pikmin_type_capacity];
uint32_t given_count = 0;
TSUCHI_Mod* self = nullptr;
TSUCHI_Result hooks_ready = TSUCHI_NOT_READY;
// Set once the whole hook set is in place. Until then every hook passes
// straight to the game, and no type is installed.
bool active = false;
uint64_t summary_type = 0, result_type = 0;
PikminBirthContexts birth_contexts;
const Kind kClass{"pikmin.class"};

bool text(const Json* v) { return v && v->kind == Json::Kind::text && v->bytes && strlen(v->text) == v->bytes; }
bool tell(Text& out, const char* a, const char* b = "", const char* c = "") {
    const size_t x = strlen(a), y = strlen(b), z = strlen(c);
    if (!out.reserve(x + y + z + 1)) return false;
    memcpy(out.data, a, x); memcpy(out.data + x, b, y); memcpy(out.data + x + y, c, z + 1);
    return false;
}
bool stock(const char* id, uint8_t& donor) {
    uint32_t value = 0;
    if (id_form(id) != IdForm::plain || id_value(pikmin::PikminType_vocabulary, id, &value) != TSUCHI_OK || value > 7) return false;
    donor = uint8_t(value);
    return true;
}
const char* row_name(uint8_t color) {
    return color == t::constant::pikmin_color_first ? t::name::pikmin_row_first : t::name::pikmin_row_second;
}
Given* color_entry(uint8_t color) {
    for (uint32_t i = 0; i < given_count; ++i) if (given[i].color == color) return &given[i];
    return nullptr;
}
Given* id_entry(const char* id) {
    for (uint32_t i = 0; i < given_count; ++i) if (!strcmp(given[i].id, id)) return &given[i];
    return nullptr;
}
void empty() {
    for (auto& entry : given) { entry.others.clear(); entry = Given{}; }
    given_count = 0;
}
bool take(const Json& root, const char* source) {
    const Json* all = root.find("types");
    if (!all) return true;
    if (all->kind != Json::Kind::object || all->count > t::constant::pikmin_type_capacity) return false;
    const char* types_text = nullptr;
    for (Members members(root, source); members.next();) if (!strcmp(members.value->name, "types")) types_text = members.value_text;
    if (!types_text) return false;
    for (Members members(*all, types_text); members.next();) {
        const Json& value = *members.value;
        const Json* color = value.find("color"), *donor = value.find("from");
        if (id_form(value.name) != IdForm::added || value.kind != Json::Kind::object || !color || !donor ||
            color->kind != Json::Kind::number || !color->whole || donor->kind != Json::Kind::number || !donor->whole || donor->integer < 0 || donor->integer > 7 ||
            (color->integer != int64_t(t::constant::pikmin_color_first) && color->integer != int64_t(t::constant::pikmin_color_second)) ||
            color_entry(uint8_t(color->integer))) return false;
        Given& entry = given[given_count++];
        memcpy(entry.id, value.name, strlen(value.name) + 1);
        entry.color = uint8_t(color->integer), entry.donor = uint8_t(donor->integer);
        for (Members unknown(value, members.value_text); unknown.next();) {
            if (!strcmp(unknown.value->name, "color") || !strcmp(unknown.value->name, "from")) continue;
            entry.others.add(", ").add(unknown.text, unknown.bytes);
        }
        if (!entry.others.whole) return false;
    }
    return true;
}
void put(Written& out) {
    out.add(", \"types\": {");
    for (uint32_t i = 0; i < given_count; ++i) {
        const Given& entry = given[i];
        out.add(i ? ", \"" : "\"").add(entry.id).add("\": {\"color\": ").number(entry.color).add(", \"from\": ").number(entry.donor);
        if (entry.others.used) out.add(entry.others.data, entry.others.used);
        out.add("}");
    }
    out.add("}");
}
const char* const known[] = {"types"};
Record record{"pikmin", nullptr, known, 1, take, put, empty};
// True once the save's record holds a color. Until then every hook leaves the
// game, its saves, its enum and its tables as they are. The record outlives the
// mod that allocated a color, so a save keeps its rows with that mod removed.
bool allocated() {
    if (!active) return false;
    if (on_game_thread()) read_record(record);
    return given_count != 0;
}

uint64_t story() {
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    const uint64_t object = instance ? game::read_or<uint64_t>(instance + t::game_instance::story_save) : 0;
    return object && game::live(object) ? object : 0;
}
void cache_types(uint64_t object) {
    game::Place at;
    uint64_t base = 0;
    if (!object || game::find_place(game::type_of(object), object, "StorySummary", at) != TSUCHI_OK ||
        game::struct_at(at, summary_type, base) != TSUCHI_OK) return;
    if (game::find_place(summary_type, base, "TotalStoryResultInfo", at) == TSUCHI_OK) game::struct_at(at, result_type, base);
}
TSUCHI_Result repair(uint64_t object, bool clear = false) {
    cache_types(object);
    return pikmin_save_story(object, clear);
}
// The story object the frame step last repaired. The construct, reset, and
// serialize hooks repair the story whenever the game rebuilds or reads it, so
// a frame repairs only a story object it has not seen, or after a failure.
uint64_t repaired = 0;
TSUCHI_Result keep_story(bool fresh) {
    const uint64_t object = story();
    if (!fresh && object && object == repaired) return TSUCHI_OK;
    const TSUCHI_Result r = repair(object);
    repaired = r == TSUCHI_OK ? object : 0;
    return r;
}
void repair_hooked(uint64_t object, bool clear = false) {
    if (repair(object, clear) != TSUCHI_OK) repaired = 0;
}
TSUCHI_Result load(const char* path, TSUCHI_Object& out) { return self ? host.asset->load(self, path, &out) : TSUCHI_NOT_READY; }

// The donor each added color's row was copied from in this run, plus one. The
// row stays in the table for the run, so another donor cannot take it over.
uint8_t row_copied_from[2]{};
// Admission installs a type before its record is written, and leaves the story
// alone until the record holds it.
TSUCHI_Result install(Given& entry, bool need_presentation = true, bool repair_story = true) {
    if (!self || !active) return TSUCHI_NOT_READY;
    const uint64_t framework = game::game_framework();
    if (!framework) return TSUCHI_NOT_READY;
    const uint64_t identity = game::to_handle(framework);
    const uint64_t table = game::from_handle(entry.table);
    if (entry.framework == identity && table && (!need_presentation || game::pikmin_hud_ready(entry.color))) {
        const uint64_t expected = game::from_handle(entry.active_class ? entry.active_class : entry.donor_class);
        uint64_t actual = 0;
        TSUCHI_Value color{};
        if (expected && native_map_get(framework, "PikminClass", &entry.color, sizeof entry.color, &actual, sizeof actual) == TSUCHI_OK && actual == expected &&
            host.datatable->get(self, {entry.table}, row_name(entry.color), "PikminColor", &color) == TSUCHI_OK && color.kind == TSUCHI_VALUE_INTEGER && color.as.integer == entry.color)
            return repair_story ? keep_story(false) : TSUCHI_OK;
    }
    TSUCHI_Object colors{}, properties{};
    TSUCHI_Result r = load(t::asset::pikmin_color_enum, colors);
    if (r != TSUCHI_OK || (r = native_pikmin_enum(game::from_handle(colors.id))) != TSUCHI_OK) return r;
    if ((r = load(t::asset::pikmin_property_table, properties)) != TSUCHI_OK) return r;
    uint64_t donor_class = 0;
    if ((r = native_map_get(framework, "PikminClass", &entry.donor, sizeof entry.donor, &donor_class, sizeof donor_class)) != TSUCHI_OK) return r;
    if (!game::is_class(donor_class)) return TSUCHI_NOT_READY;
    entry.donor_class = game::to_handle(donor_class);
    uint64_t cls = entry.active_class ? game::from_handle(entry.active_class) : 0;
    if (!cls) cls = donor_class;
    if ((r = native_map_put(framework, "PikminClass", &entry.color, sizeof entry.color, &cls, sizeof cls)) != TSUCHI_OK) return r;
    uint64_t inserted = 0;
    if ((r = native_map_get(framework, "PikminClass", &entry.color, sizeof entry.color, &inserted, sizeof inserted)) != TSUCHI_OK || inserted != cls)
        return r == TSUCHI_OK ? TSUCHI_NOT_READY : r;
    const uint64_t object = game::from_handle(properties.id);
    game::Name name{};
    if ((r = game::name_from_utf8(row_name(entry.color), name)) != TSUCHI_OK) return r;
    uint64_t packed = 0; memcpy(&packed, &name, sizeof name);
    if (!t::bound(t::fn::find_table_row)) return TSUCHI_NOT_READY;
    uint8_t& copied = row_copied_from[entry.color == t::constant::pikmin_color_first ? 0 : 1];
    if (!t::fn::find_table_row(reinterpret_cast<void*>(object), packed)) {
        if ((r = host.datatable->add_row(self, properties, row_name(entry.color), t::row::pikmin_properties[entry.donor])) != TSUCHI_OK) return r;
        copied = uint8_t(entry.donor + 1);
    } else if (copied && copied != entry.donor + 1) {
        return TSUCHI_BUSY;
    }
    TSUCHI_Value own_color{}; own_color.kind = TSUCHI_VALUE_INTEGER; own_color.as.integer = entry.color;
    if ((r = host.datatable->override(self, properties, row_name(entry.color), "PikminColor", &own_color)) != TSUCHI_OK) return r;
    if (repair_story && (r = keep_story(true)) != TSUCHI_OK) return r;
    entry.table = properties.id, entry.framework = identity;
    if (need_presentation && (r = game::pikmin_hud_register(entry.color, entry.donor)) != TSUCHI_OK) return r == TSUCHI_NOT_FOUND ? TSUCHI_NOT_READY : r;
    return TSUCHI_OK;
}
void stopped(void*, uint64_t color) {
    Given* entry = color_entry(uint8_t(color));
    if (!entry) return;
    entry->active_class = 0;
    const uint64_t framework = game::game_framework(), donor = game::from_handle(entry->donor_class);
    if (framework && donor) native_map_put(framework, "PikminClass", &entry->color, sizeof entry->color, &donor, sizeof donor);
    // Parameter overrides are undone by their owner's normal registry cleanup.
    // The copied row and its permanent native color remain for live borrowers.
}
TSUCHI_Result prepare_enum() {
    if (!t::bound(t::fn::pikmin_enum_get)) return TSUCHI_NOT_READY;
    return native_pikmin_enum(reinterpret_cast<uint64_t>(t::fn::pikmin_enum_get()));
}
void step() {
    const bool any = self && active && read_record(record) == TSUCHI_OK && given_count;
    game::pikmin_counts_enable(any);
    if (!any) return;
    prepare_enum();
    game::pikmin_hud_refresh();
    for (uint32_t i = 0; i < given_count; ++i) install(given[i], true, false);
    keep_story(false);
}

using Eject = t::SiteOf<void (*)(void*, void*, int32_t, int32_t, int32_t, uint64_t, bool, int32_t)>::Function;
Eject eject_before = nullptr;
void (*finish_before)(void*, void*, bool, void*) = nullptr;
void (*component_before)(void*) = nullptr;
void (*construct_before)(void*) = nullptr;
void (*reset_before)(void*, uint32_t) = nullptr;
void (*follow_clear_before)() = nullptr;
void (*follow_snapshot_before)(uint64_t, uint32_t) = nullptr;
TSUCHI_Result snapshot_state = TSUCHI_NOT_FOUND;
void (*summary_reset_before)(void*) = nullptr;
void (*result_reset_before)(void*) = nullptr;
void (*serialize_before)(void*, void*) = nullptr;
void (*structured_before)(void*, void*) = nullptr;
void eject(void* leader, void* at, int32_t color, int32_t leaf, int32_t count, uint64_t level, bool alone, int32_t decoration) {
    uint8_t added = 0;
    // Existing explicit admission refusal remains separate from context custody.
    if (color == int32_t(t::constant::pikmin_color_first) || color == int32_t(t::constant::pikmin_color_second)) {
        if (added_pikmin_native_ready(uint8_t(color)) != TSUCHI_OK) return;
        added = uint8_t(color);
    } else if (!allocated()) {
        eject_before(leader, at, color, leaf, count, level, alone, decoration);
        return;
    }
    PikminBirthContexts::Scope context(birth_contexts, {known_game_thread_token(), 0, 0, added});
    eject_before(leader, at, color, leaf, count, level, alone, decoration);
}
bool finish_live(uint64_t actor) {
    uint8_t play = 0;
    return game::read(actor + t::engine_actor::play_state, play) &&
        !(play & t::constant::actor_destroying_mask);
}
// The game gives a new Pikmin its Pikmin component while the spawn finishes, so a birth
// without a member yet is bound by the same live game actor alone.
bool finish_binding(uint64_t owner, uint64_t member, uint64_t& actor, uint64_t& part) {
    actor = game::from_handle(owner), part = member ? game::from_handle(member) : 0;
    if (!actor || !game::is_game_actor(actor) || !finish_live(actor)) return false;
    return !member || (part && game::read_or<uint64_t>(actor + t::game_actor::pikmin) == part);
}
void finish(void* object, void* transform, bool at_default, void* data) {
    const uint64_t thread = known_game_thread_token();
    if (!thread || !allocated()) { finish_before(object, transform, at_default, data); return; }
    const auto parent = birth_contexts.snapshot(thread);
    if (parent.blocked || !parent.color) { finish_before(object, transform, at_default, data); return; }
    if (parent.owner) {
        uint64_t enclosing_actor = 0, enclosing_part = 0;
        if (!finish_binding(parent.owner, parent.part, enclosing_actor, enclosing_part)) {
            finish_before(object, transform, at_default, data); return;
        }
    }
    const uint64_t actor = reinterpret_cast<uint64_t>(object);
    const uint64_t part = game::is_game_actor(actor) && finish_live(actor) ?
        game::read_or<uint64_t>(actor + t::game_actor::pikmin) : 0;
    const uint64_t owner = game::to_handle(actor), member = part ? game::to_handle(part) : 0;
    uint64_t joined_actor = 0, joined_part = 0;
    if (!owner || (part && !member) || !finish_binding(owner, member, joined_actor, joined_part) || joined_actor != actor) {
        finish_before(object, transform, at_default, data); return;
    }
    PikminBirthContexts::Scope context(birth_contexts, {thread, owner, member, parent.color});
    if (context.allowed(known_game_thread_token())) game::write(actor + t::game_actor::color, parent.color);
    finish_before(object, transform, at_default, data);
    if (context.allowed(known_game_thread_token()) && finish_binding(owner, member, joined_actor, joined_part) && joined_actor == actor)
        game::write(actor + t::game_actor::color, parent.color);
}
void begin_component(void* object) {
    const uint64_t thread = known_game_thread_token();
    if (!thread || !allocated()) { component_before(object); return; }
    const auto context = birth_contexts.snapshot(thread);
    if (context.blocked) { component_before(object); return; }
    const uint64_t part = reinterpret_cast<uint64_t>(object);
    uint64_t actor = 0, joined_part = 0;
    if (context.owner) {
        if (!finish_binding(context.owner, context.part, actor, joined_part) ||
            (context.part ? joined_part != part : game::read_or<uint64_t>(actor + t::game_actor::pikmin) != part)) {
            component_before(object); return;
        }
        if (context.color) game::write(actor + t::game_actor::color, context.color);
    } else {
        const uint64_t member = game::to_handle(part);
        const uint64_t candidate = member ? game::read_or<uint64_t>(part + t::pikmin_native::owner) : 0;
        const uint64_t owner = game::to_handle(candidate);
        if (!owner || !finish_binding(owner, member, actor, joined_part) || joined_part != part) {
            component_before(object); return;
        }
    }
    const uint8_t current = game::read_or<uint8_t>(actor + t::game_actor::color);
    if (current == t::constant::pikmin_color_first || current == t::constant::pikmin_color_second) added_pikmin_native_ready(current);
    component_before(object);
}
void construct(void* initializer) {
    const uint64_t object = game::read_or<uint64_t>(reinterpret_cast<uint64_t>(initializer));
    construct_before(initializer);
    if (allocated()) repair_hooked(object);
}
void reset(void* object, uint32_t flags) {
    reset_before(object, flags);
    if (allocated()) repair_hooked(reinterpret_cast<uint64_t>(object), true);
}
void clear_follow() { follow_clear_before(); if (allocated()) pikmin_save_clear_follow(story()); }
void snapshot_follow(uint64_t include_loose, uint32_t include_photon) {
    follow_snapshot_before(include_loose, include_photon);
    if (!allocated()) return;
    const TSUCHI_Result result = pikmin_snapshot_follow(story(), include_loose, include_photon);
    if (result != snapshot_state && self) {
        host.mod->log(self, result == TSUCHI_OK ? "new Pikmin follower snapshot ready" : "new Pikmin follower snapshot refused: native classification is not ready");
    }
    snapshot_state = result;
}
void reset_summary(void* object) {
    summary_reset_before(object);
    if (summary_type && allocated()) pikmin_save_summary(summary_type, reinterpret_cast<uint64_t>(object), true);
}
void reset_result(void* object) {
    result_reset_before(object);
    if (result_type && allocated()) pikmin_save_result(result_type, reinterpret_cast<uint64_t>(object), true);
}
// The enum gains its added names before the game reads a save that holds them.
void serialize(void* object, void* archive) {
    if (!allocated()) { serialize_before(object, archive); return; }
    prepare_enum(); repair_hooked(reinterpret_cast<uint64_t>(object));
    serialize_before(object, archive);
    repair_hooked(reinterpret_cast<uint64_t>(object));
}
void structured(void* object, void* archive) {
    if (!allocated()) { structured_before(object, archive); return; }
    prepare_enum(); repair_hooked(reinterpret_cast<uint64_t>(object));
    structured_before(object, archive);
    repair_hooked(reinterpret_cast<uint64_t>(object));
}
}

bool pikmin_content_valid(const Json& content, Text& wrong) {
    const Json* all = content.find("pikmin");
    if (!all) return true;
    if (all->kind != Json::Kind::object) return tell(wrong, "\"content\": \"pikmin\" is an object: a short name for each type");
    for (const Json* each = all->first; each; each = each->next) {
        if (!valid_mod_id(each->name) || each->kind != Json::Kind::object) return tell(wrong, "\"content\": Pikmin ", each->name, ": it needs a short name and an object");
        for (const Json* key = each->first; key; key = key->next)
            if (strcmp(key->name, "from") && strcmp(key->name, "class") && strcmp(key->name, "values") && strcmp(key->name, "icon") && strcmp(key->name, "placement_values"))
                return tell(wrong, "\"content\": Pikmin has a key the framework does not know: ", key->name);
        uint8_t donor = 0;
        const Json* from = each->find("from");
        if (!text(from) || !stock(from->text, donor)) return tell(wrong, "\"content\": Pikmin ", each->name, ": \"from\" names a stock type from red through ice");
        const Json* cls = each->find("class");
        if (cls && (!text(cls) || content_path::wrong(cls->text, cls->bytes, content_path::Names::object)))
            return tell(wrong, "\"content\": Pikmin ", each->name, ": \"class\" is the path of a Pikmin class");
        const Json* icon = each->find("icon");
        if (icon && (!text(icon) || content_path::wrong(icon->text, icon->bytes, content_path::Names::object)))
            return tell(wrong, "\"content\": Pikmin ", each->name, ": \"icon\" is the path of a texture");
        const Json* values = each->find("values");
        if (values && values->kind != Json::Kind::object) return tell(wrong, "\"content\": Pikmin values is an object of fields");
        for (const Json* field = values ? values->first : nullptr; field; field = field->next) {
            // The manifest may tune the row, but cannot change its allocated identity.
            size_t root_bytes = 0;
            while (field->name[root_bytes] && field->name[root_bytes] != '[' && field->name[root_bytes] != '.') ++root_bytes;
            if (content_path::same(field->name, root_bytes, "PikminColor", strlen("PikminColor")) || (field->kind != Json::Kind::number && field->kind != Json::Kind::boolean && !text(field)))
                return tell(wrong, "\"content\": Pikmin field cannot be set: ", field->name);
        }
    }
    return true;
}
namespace {
// A field of the donor's row takes a manifest value of the same kind. A whole
// number fits an integer or a decimal field.
bool takes(const Json& field, const TSUCHI_Value& now) {
    if (field.kind == Json::Kind::boolean) return now.kind == TSUCHI_VALUE_BOOLEAN;
    if (field.kind == Json::Kind::text) return now.kind == TSUCHI_VALUE_TEXT;
    return now.kind == TSUCHI_VALUE_NUMBER || (now.kind == TSUCHI_VALUE_INTEGER && field.whole);
}
// What a declared type needs that does not depend on its color: a class that
// loads and has a Pikmin component, and donor row fields for its values.
TSUCHI_Result check(TSUCHI_Mod* mod, const Json& each, uint8_t donor, TSUCHI_Object properties, uint64_t& cls, uint64_t& icon, Text& problem) {
    cls = icon = 0;
    TSUCHI_Result r = TSUCHI_OK;
    if (const Json* path = each.find("class")) {
        TSUCHI_Object loaded{};
        if ((r = host.asset->load(mod, path->text, &loaded)) != TSUCHI_OK) { tell(problem, "\"content\": the Pikmin class could not be loaded: ", path->text); return r; }
        const uint64_t object = game::from_handle(loaded.id), pattern = object ? game::default_object(object) : 0;
        const uint64_t part = pattern ? game::read_or<uint64_t>(pattern + t::game_actor::pikmin) : 0;
        static game::ClassRef pikmin_part{"PikminComponent"};
        if (!game::is_class(object) || !game::is_game_actor(pattern) || !game::live(part) || !game::is_a(part, pikmin_part)) { tell(problem, "\"content\": the class has no Pikmin component: ", path->text); return TSUCHI_INVALID; }
        cls = loaded.id;
    }
    if (const Json* path = each.find("icon")) {
        TSUCHI_Object loaded{};
        if ((r = host.asset->load(mod, path->text, &loaded)) != TSUCHI_OK) {
            tell(problem, "\"content\": the Pikmin icon could not be loaded: ", path->text); return r;
        }
        static game::ClassRef texture{"Texture2D"};
        if (!game::is_a(game::from_handle(loaded.id), texture)) {
            tell(problem, "\"content\": the Pikmin icon is not a texture: ", path->text); return TSUCHI_INVALID;
        }
        icon = loaded.id;
    }
    const Json* values = each.find("values");
    for (const Json* field = values ? values->first : nullptr; field; field = field->next) {
        TSUCHI_Value now{};
        r = host.datatable->get(mod, properties, t::row::pikmin_properties[donor], field->name, &now);
        if (r == TSUCHI_NOT_FOUND || (r == TSUCHI_OK && !takes(*field, now))) { tell(problem, "\"content\": the Pikmin row has no such field for this value: ", field->name); return TSUCHI_INVALID; }
        if (r != TSUCHI_OK) { tell(problem, "\"content\": the Pikmin row could not be read: ", field->name); return r; }
    }
    return TSUCHI_OK;
}
// Gives a type its native class, row values, and ID. Allocation is recorded by
// the caller once every declared type has passed.
TSUCHI_Result admit(TSUCHI_Mod* mod, const Json& each, Given& entry, uint64_t cls, uint64_t icon, Text& problem) {
    TSUCHI_Result r = own(mod, kClass, entry.color, stopped, nullptr);
    if (r != TSUCHI_OK) return r;
    if ((r = install(entry, false, false)) != TSUCHI_OK) {
        tell(problem, r == TSUCHI_BUSY ? "\"content\": the Pikmin row of a refused type stays until the game restarts: " :
                                         "\"content\": Pikmin native admission is not ready: ", entry.id);
        return r;
    }
    if (cls) {
        entry.active_class = cls;
        uint64_t native_class = game::from_handle(cls);
        if ((r = native_map_put(game::game_framework(), "PikminClass", &entry.color, sizeof entry.color, &native_class, sizeof native_class)) != TSUCHI_OK) return r;
    }
    const Json* values = each.find("values");
    for (const Json* field = values ? values->first : nullptr; field; field = field->next) {
        if (field->kind == Json::Kind::text) r = host.datatable->override_text(mod, {entry.table}, row_name(entry.color), field->name, field->text);
        else {
            TSUCHI_Value value{};
            if (field->kind == Json::Kind::boolean) value.kind = TSUCHI_VALUE_BOOLEAN, value.as.boolean = field->boolean;
            else if (field->whole) value.kind = TSUCHI_VALUE_INTEGER, value.as.integer = field->integer;
            else value.kind = TSUCHI_VALUE_NUMBER, value.as.number = field->number;
            r = host.datatable->override(mod, {entry.table}, row_name(entry.color), field->name, &value);
        }
        if (r != TSUCHI_OK) { tell(problem, "\"content\": Pikmin parameter could not be set: ", field->name); return r; }
    }
    if (icon && (r = game::pikmin_hud_icon(mod, entry.color, icon)) != TSUCHI_OK) {
        tell(problem, "\"content\": the Pikmin icon could not be declared: ", each.name); return r;
    }
    return id_add(pikmin::PikminType_vocabulary, mod, each.name, entry.color);
}
// Takes back the colors this admission gave out, which no record holds yet.
void forget_fresh(TSUCHI_Mod* mod, uint32_t before) {
    const size_t prefix = strlen(mod_id(mod)) + 1;
    while (given_count > before) {
        Given& entry = given[given_count - 1];
        game::pikmin_hud_icon_release(mod, entry.color);
        if (disown(mod, kClass, entry.color) == TSUCHI_OK) stopped(nullptr, entry.color);
        id_remove(pikmin::PikminType_vocabulary, mod, entry.id + prefix);
        entry.others.clear();
        entry = Given{};
        --given_count;
    }
}
} // namespace
TSUCHI_Result pikmin_content_begin(TSUCHI_Mod* mod, const Json& content, Text& problem) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    const Json* all = content.find("pikmin");
    if (!all || !all->first) return TSUCHI_OK;
    TSUCHI_Result r = read_record(record);
    if (r != TSUCHI_OK) { tell(problem, "\"content\": the saved Pikmin type record could not be read"); return r; }
    // Every declared type is checked before any color is given out.
    uint32_t wanted = 0;
    for (const Json* each = all->first; each; each = each->next) {
        char id[kIdBytes]; uint8_t donor = 0;
        if (!id_join(mod_id(mod), each->name, id) || !stock(each->find("from")->text, donor)) return TSUCHI_INVALID;
        const Given* entry = id_entry(id);
        if (entry && entry->donor != donor) { tell(problem, "\"content\": a saved Pikmin type cannot change its stock donor: ", id); return TSUCHI_INVALID; }
        wanted += !entry;
    }
    if (given_count + wanted > t::constant::pikmin_type_capacity) {
        tell(problem, "\"content\": this save has allocated both supported added Pikmin colors");
        return TSUCHI_CAPACITY;
    }
    TSUCHI_Object properties{};
    if ((r = load(t::asset::pikmin_property_table, properties)) != TSUCHI_OK) { tell(problem, "\"content\": the Pikmin parameter table could not be loaded"); return r; }
    uint64_t classes[t::most::pikmin_type_capacity]{}, icons[t::most::pikmin_type_capacity]{};
    uint32_t index = 0;
    for (const Json* each = all->first; each; each = each->next, ++index) {
        uint8_t donor = 0;
        if (index == t::constant::pikmin_type_capacity) return TSUCHI_CAPACITY;
        stock(each->find("from")->text, donor);
        if ((r = check(mod, *each, donor, properties, classes[index], icons[index], problem)) != TSUCHI_OK) return r;
    }
    // A color given out here is recorded only once every type is admitted.
    const uint32_t before = given_count;
    index = 0;
    for (const Json* each = all->first; each; each = each->next, ++index) {
        char id[kIdBytes]; uint8_t donor = 0;
        id_join(mod_id(mod), each->name, id);
        stock(each->find("from")->text, donor);
        Given* entry = id_entry(id);
        if (!entry) {
            entry = &given[given_count++]; memcpy(entry->id, id, strlen(id) + 1); entry->donor = donor;
            entry->color = color_entry(uint8_t(t::constant::pikmin_color_first)) ? uint8_t(t::constant::pikmin_color_second) : uint8_t(t::constant::pikmin_color_first);
        }
        if ((r = admit(mod, *each, *entry, classes[index], icons[index], problem)) != TSUCHI_OK) { forget_fresh(mod, before); return r; }
    }
    if (given_count == before) return TSUCHI_OK;
    if ((r = write_record(record)) != TSUCHI_OK) {
        forget_fresh(mod, before);
        tell(problem, "\"content\": the save refused the Pikmin type allocation");
        return r;
    }
    keep_story(true);
    return TSUCHI_OK;
}
TSUCHI_Result added_pikmin_ready_impl(uint8_t color, bool presentation) {
    if (color != t::constant::pikmin_color_first && color != t::constant::pikmin_color_second) return TSUCHI_OK;
    if (!active) return TSUCHI_NOT_READY;
    const uint64_t context = game::global<uint64_t>(t::data::game_context);
    if (!context || !game::from_weak_reference(context + t::game_context::snapshot_owner)) return TSUCHI_NOT_READY;
    if (game::read_or<uint8_t>(context + t::game_context::rule) != t::constant::rule_story) return TSUCHI_UNSUPPORTED;
    TSUCHI_Result r = read_record(record);
    if (r != TSUCHI_OK) return r;
    Given* entry = color_entry(color);
    return entry ? install(*entry, presentation) : TSUCHI_NOT_FOUND;
}
TSUCHI_Result added_pikmin_ready(uint8_t color) { return added_pikmin_ready_impl(color, true); }
TSUCHI_Result added_pikmin_native_ready(uint8_t color) { return added_pikmin_ready_impl(color, false); }
void pikmin_content_start(TSUCHI_Mod* owner) {
    self = owner;
    active = false;
    uint64_t id = 0;
    // A hook in place before a later one fails stays in place, and passes every
    // call to the game while `active` is false.
    hooks_ready = hook_site(self, t::code_site::pikmin_eject_types, &eject, &eject_before, &id);
    if (hooks_ready == TSUCHI_OK) hooks_ready = hook_site(self, t::code_site::pikmin_finish_spawn, &finish, &finish_before, &id);
    if (hooks_ready == TSUCHI_OK) hooks_ready = hook_slot(self, t::slot::pikmin_begin_play, &begin_component, &component_before, kHookNeedsStock, &id);
    if (hooks_ready == TSUCHI_OK) hooks_ready = hook_site(self, t::code_site::pikmin_story_construct, &construct, &construct_before, &id);
    if (hooks_ready == TSUCHI_OK) hooks_ready = hook_site(self, t::code_site::pikmin_story_reset, &reset, &reset_before, &id);
    if (hooks_ready == TSUCHI_OK) hooks_ready = hook_site(self, t::code_site::pikmin_follow_clear, &clear_follow, &follow_clear_before, &id);
    if (hooks_ready == TSUCHI_OK) hooks_ready = hook_site(self, t::code_site::pikmin_follow_snapshot, &snapshot_follow, &follow_snapshot_before, &id);
    if (hooks_ready == TSUCHI_OK) hooks_ready = hook_site(self, t::code_site::pikmin_summary_reset, &reset_summary, &summary_reset_before, &id);
    if (hooks_ready == TSUCHI_OK) hooks_ready = hook_site(self, t::code_site::pikmin_result_reset, &reset_result, &result_reset_before, &id);
    if (hooks_ready == TSUCHI_OK) hooks_ready = hook_slot(self, t::slot::pikmin_story_serialize, &serialize, &serialize_before, kHookNeedsStock, &id);
    if (hooks_ready == TSUCHI_OK) hooks_ready = hook_slot(self, t::slot::pikmin_story_structured_serialize, &structured, &structured_before, kHookNeedsStock, &id);
    if (hooks_ready == TSUCHI_OK) hooks_ready = game::pikmin_hud_start(self);
    if (hooks_ready == TSUCHI_OK) hooks_ready = game::pikmin_birth_start(self);
    if (hooks_ready == TSUCHI_OK) hooks_ready = game::pikmin_counts_start(self);
    active = hooks_ready == TSUCHI_OK;
    each_frame(step);
}
}
