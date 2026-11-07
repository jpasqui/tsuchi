// Checks of what a manifest adds to the game under "content": new treasures and their series. Included by
// objects_test.cpp, which has the made-up game these run against.
#include "content.h"

namespace {

// The made-up game's treasures: its four tables, and what a loaded world keeps their rows by name in.
struct Trove {
    uint64_t parameters = 0, classes = 0, icons = 0, catalog = 0;
    uint64_t rows_keeper = 0, icons_keeper = 0;
    uint64_t functions = 0, icons_keeper_class = 0; // what its tables do, and the class of what keeps the icons: the creatures' checks use both
    int entered = 0;
} trove;
constexpr uint32_t kStagingsAt = 0x10, kStagingBytes = 0x10; // in a row of the catalog: the list of where the Piklopedia shows it

// The engine's way of adding a row: a copy under the name, and of the list a row of the catalog has in it.
void trove_add_row(void* table, uint64_t name, void* from) {
    const uint64_t copy = fake::memory(kRowBytes);
    std::memcpy(reinterpret_cast<void*>(copy), from, kRowBytes);
    if (reinterpret_cast<uint64_t>(table) == trove.catalog) {
        const int32_t count = fake::get<int32_t>(copy + kStagingsAt + t::array::count);
        const uint64_t data = fake::memory(size_t(kStagingBytes) * size_t(count ? count : 1));
        std::memcpy(reinterpret_cast<void*>(data), reinterpret_cast<void*>(fake::get<uint64_t>(copy + kStagingsAt + t::array::data)), size_t(kStagingBytes) * size_t(count));
        fake::put(copy + kStagingsAt + t::array::data, data);
    }
    sheets.rows[{reinterpret_cast<uint64_t>(table), name}] = copy;
}
// A list of rows by name: the row in the slot in use that has the name, or 0.
uint64_t trove_listed(uint64_t list, uint64_t name) {
    namespace l = t::row_list;
    namespace e = t::table_row_entry;
    const uint64_t slots = fake::get<uint64_t>(list + l::slots), far = fake::get<uint64_t>(list + l::used_bits_far);
    for (int32_t i = 0; i < fake::get<int32_t>(list + l::slot_count); ++i) {
        const uint64_t bits = far ? far : list + l::used_bits;
        if (!(fake::get<uint32_t>(bits + 4 * uint64_t(i >> 5)) >> (i & 31) & 1)) continue;
        if (fake::get<uint64_t>(slots + uint64_t(i) * e::size + e::name) == name) return fake::get<uint64_t>(slots + uint64_t(i) * e::size + e::row);
    }
    return 0;
}
// The engine's way of putting a row in a table's list, which is what it is handed's list wherever a table has one.
void trove_enter(void* table, uint64_t name, void* row) {
    namespace l = t::row_list;
    namespace e = t::table_row_entry;
    const uint64_t list = reinterpret_cast<uint64_t>(table) + t::data_table::rows;
    const int32_t count = fake::get<int32_t>(list + l::slot_count);
    const uint64_t slots = fake::memory(size_t(e::size) * size_t(count + 1));
    if (count) std::memcpy(reinterpret_cast<void*>(slots), reinterpret_cast<void*>(fake::get<uint64_t>(list + l::slots)), size_t(e::size) * size_t(count));
    fake::put(slots + uint64_t(count) * e::size + e::name, name), fake::put(slots + uint64_t(count) * e::size + e::row, reinterpret_cast<uint64_t>(row));
    const uint64_t word = list + l::used_bits + 4 * uint64_t(count >> 5);
    fake::put<uint32_t>(word, fake::get<uint32_t>(word) | 1u << (count & 31));
    fake::put(list + l::slots, slots), fake::put<int32_t>(list + l::slot_count, count + 1), fake::put<int32_t>(list + l::bit_count, count + 1);
    ++trove.entered;
}
// The game looks a treasure's row up in the loaded world's list, and nowhere else.
void* trove_find_treasure_row(void* name) {
    return reinterpret_cast<void*>(trove.rows_keeper ? trove_listed(trove.rows_keeper + t::treasure_keeper::rows, fake::get<uint64_t>(reinterpret_cast<uint64_t>(name))) : 0);
}

} // namespace

void adding_content() {
    namespace game = tsuchi::game;
    const TSUCHI_DatatableService& sheet = *tsuchi::host.datatable;
    const TSUCHI_TextService& lines = *tsuchi::host.text;
    const TSUCHI_StoryService& tale = *tsuchi::host.story;
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/", record_at = "save:/tsuchi/tsuchi.content/treasures.json";
    const auto other_files = disk.files;
    disk.files.clear(), disk.folders.clear(), disk.changes.clear();
    tsuchi::set_log_sink(log_sink);
    const auto plain = [&](const std::string& id, const std::string& content, const std::string& more = "") {
        disk.files[mods_at + id + "/mod.json"] = "{\"id\": \"" + id + "\", \"name\": \"The " + id + " mod\", \"version\": \"1.0.0\"" + more + (content.empty() ? "" : ", \"content\": " + content) + "}";
    };
    const auto gone = [&](const std::string& id) { disk.files.erase(mods_at + id + "/mod.json"), disk.files.erase(mods_at + id + "/main.lua"); };
    const auto problem = [](const char* id) {
        const tsuchi::ModRecord* record = tsuchi::find_mod(id);
        return std::string(!record ? "<no such mod>" : record->state == tsuchi::ModState::blocked ? record->problem.get() : record->state == tsuchi::ModState::ready ? "<ready>" : "<running>");
    };
    const auto told = [&](const char* id, const std::string& expected) {
        const std::string said = problem(id);
        if (said != expected) std::printf("  %s: [%s]\n", id, said.c_str());
        return said == expected;
    };
    const auto shelve = [](std::u16string path, uint64_t asset) { shelf.assets[fake::engine_name(path.data(), path.size(), 1)] = asset; };

    // ---- The made-up game's side: four tables with a treasure in them, a class, an icon, and a loaded world ----
    const uint64_t table_class = fake::get<uint64_t>(w.creature_table + t::object::class_), functions = fake::memory(0x40);
    const auto old_add = t::fn::add_table_row;
    const auto old_enter = t::fn::add_table_row_internal;
    const auto old_find = t::fn::find_treasure_row;
    t::fn::add_table_row = trove_add_row, t::fn::add_table_row_internal = trove_enter, t::fn::find_treasure_row = trove_find_treasure_row;
    namespace f = t::data_table_table;
    fake::put(functions + f::add_row, &trove_add_row), fake::put(functions + f::remove_row, &game_remove_table_row), fake::put(functions + f::add_row_internal, &trove_enter);
    const auto struct_type = [] {
        const uint64_t type = fake::memory(0x80);
        fake::put<uint64_t>(type, fake::engine().vtable);
        return type;
    };
    const auto table_of = [&](const std::u16string& name, const std::u16string& path, uint64_t row_type) {
        const uint64_t made = fake::object(name, table_class, 0x90);
        fake::put(made, functions), fake::put(made + t::data_table::row_struct, row_type);
        shelve(path, made);
        return made;
    };
    const uint64_t worth_type = fake::engine_object(u"TreasureRow", w.root), shown_type = fake::engine_object(u"ShownRow", w.root);
    const uint64_t looks_type = fake::engine_object(u"LooksRow", w.root), icon_type = fake::engine_object(u"IconRow", w.root);
    const uint64_t info_type = struct_type(), staging_type = struct_type();
    fake::field(worth_type, u"ActorName", u"NameProperty", 0x0, 8);
    fake::field(worth_type, u"Kira", u"IntProperty", t::treasure_row::sparklium, 4), fake::field(worth_type, u"Poko", u"IntProperty", t::treasure_row::poko, 4);
    fake::field(worth_type, u"CarryWeightMin", u"IntProperty", 0x10, 4), fake::field(worth_type, u"CarryWeightMax", u"IntProperty", 0x14, 4);
    fake::field(worth_type, u"CarryWeightMinVs", u"IntProperty", 0x18, 4), fake::field(worth_type, u"CarryWeightMaxVs", u"IntProperty", 0x1C, 4);
    fake::field(worth_type, u"Shine", u"FloatProperty", 0x20, 4);
    fake::field(info_type, u"ZukanNo", u"IntProperty", 0, 4), fake::field(info_type, u"SeriesID", u"IntProperty", 4, 4);
    fake::field(staging_type, u"ActorName", u"NameProperty", 0, 8), fake::field(staging_type, u"Distance", u"FloatProperty", 8, 4);
    fake::field(shown_type, u"OtakaraZukanIDInfo", u"StructProperty", 0x0, 8, info_type);
    fake::field(shown_type, u"DayParams", u"ArrayProperty", kStagingsAt, 0x10, fake::loose_field(u"StructProperty", kStagingBytes, staging_type));
    fake::field(shown_type, u"TotalPlaceNum", u"IntProperty", 0x20, 4);
    fake::field(looks_type, u"DayCookBlueprint", u"SoftClassProperty", 0x0, t::soft_object::size, w.actor_class);
    fake::field(looks_type, u"NightCookBlueprint", u"SoftClassProperty", 0x30, t::soft_object::size, w.actor_class);
    trove.functions = functions;
    fake::field(icon_type, u"Texture2D", u"SoftObjectProperty", 0x0, t::soft_object::size, w.component_class);
    trove.parameters = table_of(u"Hoard", u"/G/Hoard.Hoard", worth_type), trove.catalog = table_of(u"Shown", u"/G/Shown.Shown", shown_type);
    trove.classes = table_of(u"Looks", u"/G/Looks.Looks", looks_type), trove.icons = table_of(u"Icons", u"/G/Icons.Icons", icon_type);
    const auto row_in = [](uint64_t table, const std::u16string& name) {
        const uint64_t row = fake::memory(kRowBytes);
        sheets.rows[{table, row_name(name)}] = row;
        return row;
    };
    const auto soft = [](uint64_t at, const std::u16string& path) { fake::put<int32_t>(at + t::soft_object::index, -1), fake::put(at + t::soft_object::path, row_name(path)); };
    // The game's own treasure, in all four tables; and a part of the ship, which the Piklopedia does not list.
    const uint64_t eclair = row_in(trove.parameters, u"OTAECLAIR"), eclair_shown = row_in(trove.catalog, u"OTAECLAIR");
    fake::put(eclair, row_name(u"GOtaEclair")), fake::put<int32_t>(eclair + t::treasure_row::sparklium, 30), fake::put<int32_t>(eclair + t::treasure_row::poko, 5);
    for (int i = 0; i < 4; ++i) fake::put<int32_t>(eclair + 0x10 + 4 * i, 2 + i);
    fake::put(eclair + 0x20, 1.5f);
    const uint64_t staged = fake::memory(kStagingBytes);
    fake::put(staged, row_name(u"OTAECLAIR")), fake::put(staged + 8, 40.0f);
    fake::put<int32_t>(eclair_shown, 1), fake::put<int32_t>(eclair_shown + 4, 2); // the second of series 2
    fake::put(eclair_shown + kStagingsAt + t::array::data, staged), fake::put<int32_t>(eclair_shown + kStagingsAt + t::array::count, 1), fake::put<int32_t>(eclair_shown + kStagingsAt + t::array::max, 1);
    fake::put<int32_t>(eclair_shown + 0x20, 1);
    soft(row_in(trove.classes, u"OTAECLAIR"), u"/G/Eclair.Eclair_C"), soft(row_in(trove.icons, u"OTAECLAIR"), u"/G/EclairIcon.Icon");
    const uint64_t part = row_in(trove.parameters, u"OTAPART");
    fake::put<int32_t>(part + t::treasure_row::sparklium, 1);
    soft(row_in(trove.classes, u"OTAPART"), u"/G/Part.Part_C"), soft(row_in(trove.icons, u"OTAPART"), u"/G/PartIcon.Icon");
    // Classes of treasures the mods bring, each with the default object every actor of it is made from, which
    // carries the name of the treasure it was made from. And something that is no class.
    const uint64_t treasure_base = fake::object(u"TreasureBase", w.root, 0x100);
    fake::set_super(treasure_base, w.actor_class);
    fake::field(treasure_base, u"CID", u"NameProperty", t::game_actor::kind, 8);
    std::map<std::string, uint64_t> patterns;
    const auto class_of_treasure = [&](const std::string& name) {
        const std::u16string wide(name.begin(), name.end());
        const uint64_t made = fake::object(wide + u"_C", w.root, 0x100), pattern = fake::object(u"Default__" + wide, made, 0x100);
        fake::set_super(made, treasure_base);
        fake::put(made + t::class_type::default_object, pattern), fake::put(pattern + t::game_actor::kind, row_name(u"OTAECLAIR"));
        fake::put(made + t::object::outer, fake::object(u"/G/" + wide, w.component_class));
        shelve(u"/G/" + wide + u"." + wide + u"_C", made);
        patterns[name] = pattern;
        return made;
    };
    const auto carried = [&](const std::string& name) {
        char text[64] = "";
        name_text(fake::get<Name>(patterns[name] + t::game_actor::kind), text, sizeof text);
        return std::string(text);
    };
    const uint64_t ruby_class = class_of_treasure("Ruby");
    class_of_treasure("Coin"), class_of_treasure("Iron"), class_of_treasure("Late");
    const uint64_t ruby_icon = fake::object(u"RubyIcon", w.component_class), not_a_class = fake::object(u"Plain_C", w.component_class);
    shelve(u"/G/RubyIcon.RubyIcon", ruby_icon), shelve(u"/G/Plain.Plain_C", not_a_class);
    // A loaded world: what keeps the treasures' rows by name and what keeps the icons', each with the lists it
    // made of the game's tables, and the count of the Piklopedia's treasures, which the game has counted.
    const uint64_t rows_keeper_class = fake::object(u"OtakaraParameterMgr", w.root), icons_keeper_class = fake::object(u"ActorTextureMgr", w.root);
    trove.icons_keeper_class = icons_keeper_class;
    const uint64_t old_ui = fake::get<uint64_t>(w.framework + t::game_framework::ui), ui = fake::memory(0x80);
    fake::put(w.framework + t::game_framework::ui, ui);
    const auto counted = [&] { return fake::get<uint8_t>(ui + t::treasure_count::known) != 0; };
    const auto new_world = [&] {
        namespace k = t::treasure_keeper;
        trove.rows_keeper = fake::object(u"Keeper", rows_keeper_class, 0xD0), trove.icons_keeper = fake::object(u"IconKeeper", icons_keeper_class, 0x90);
        fake::put(trove.rows_keeper + k::table, trove.parameters), fake::put(trove.rows_keeper + k::catalog_table, trove.catalog);
        fake::put(trove.icons_keeper + t::icon_keeper::table, trove.icons);
        // As the game builds its lists: from the rows the tables have then.
        for (const auto& [key, row] : sheets.rows) {
            if (key.first == trove.parameters) trove_enter(reinterpret_cast<void*>(trove.rows_keeper + k::rows - t::data_table::rows), key.second, reinterpret_cast<void*>(row));
            if (key.first == trove.catalog) trove_enter(reinterpret_cast<void*>(trove.rows_keeper + k::catalog_rows - t::data_table::rows), key.second, reinterpret_cast<void*>(row));
            if (key.first == trove.icons) trove_enter(reinterpret_cast<void*>(trove.icons_keeper + t::icon_keeper::rows - t::data_table::rows), key.second, reinterpret_cast<void*>(row));
        }
        fake::put(w.framework + t::framework_treasures::rows, trove.rows_keeper), fake::put(w.framework + t::framework_treasures::icons, trove.icons_keeper);
        fake::put<uint8_t>(ui + t::treasure_count::known, 1);
        trove.entered = 0;
    };
    const auto in_list = [&](uint64_t list, const char16_t* name) { return trove_listed(list, row_name(name)); };
    const auto rows_list = [&] { return trove.rows_keeper + t::treasure_keeper::rows; };
    const auto catalog_list = [&] { return trove.rows_keeper + t::treasure_keeper::catalog_rows; };
    const auto icons_list = [&] { return trove.icons_keeper + t::icon_keeper::rows; };
    new_world();
    // A story being played, for the story's record of what was collected.
    const uint64_t old_story = fake::get<uint64_t>(running.instance + t::game_instance::story_save);
    const uint8_t old_rule = fake::get<uint8_t>(running.context + t::game_context::rule);
    fake::put(running.instance + t::game_instance::story_save, hoard.story), fake::put<uint8_t>(running.context + t::game_context::rule, uint8_t(t::constant::rule_story));
    hoard.log.clear();

    const auto whole = [&](uint64_t table, const char* row, const char* path) {
        TSUCHI_Value got{};
        return sheet.get(mod, {to_handle(table)}, row, path, &got) == TSUCHI_OK && got.kind == TSUCHI_VALUE_INTEGER ? got.as.integer : -99;
    };
    const auto text_in = [&](uint64_t table, const char* row, const char* path) {
        char buffer[64] = "<failed>";
        uint64_t bytes = 0;
        sheet.get_text(mod, {to_handle(table)}, row, path, buffer, sizeof buffer, &bytes);
        return std::string(buffer);
    };
    const auto row_at = [&](uint64_t table, const char16_t* row) { return sheets.rows[{table, row_name(row)}]; };
    const auto has_row = [&](uint64_t table, const char16_t* row) { return sheets.rows.count({table, row_name(row)}) != 0; };
    const auto word = [&](const char* name_space, const char* key) {
        char buffer[64] = "<none>";
        uint64_t bytes = 0;
        lines.get(mod, name_space, key, buffer, sizeof buffer, &bytes);
        return std::string(buffer);
    };
    const auto collected = [&](const char* id) {
        uint32_t count = 99;
        const TSUCHI_Result result = tale.treasure_count(mod, id, &count);
        return result == TSUCHI_OK ? int(count) : -int(result);
    };

    // An isolated code declaration cannot replace an older override of the
    // same mod. New targets roll back without disturbing those older values.
    // Existing text restores. New native keys keep their last text by DESIGN.
    words.strings[{u"ContentIsolation", u"Older"}] = shared_string(u"original older");
    words.strings[{u"ContentIsolation", u"Newer"}] = shared_string(u"original newer");
    const unsigned isolated_replacements = tsuchi::text_replacements();
    TSUCHI_Mod* isolated = tsuchi::mod_create("content.isolation");
    CHECK(isolated);
    TSUCHI_Value held_value{};
    held_value.kind = TSUCHI_VALUE_INTEGER;
    held_value.as.integer = 51;
    const TSUCHI_Object isolated_table{to_handle(trove.parameters)};
    CHECK(sheet.override(isolated, isolated_table, "OTAECLAIR", "Kira", &held_value) == TSUCHI_OK);
    CHECK(lines.override(isolated, "ContentIsolation", "Older", "held") == TSUCHI_OK);
    const uint64_t isolated_first = tsuchi::ownership_mark(isolated);
    const uint64_t former_floor = tsuchi::isolate_ownership(isolated, isolated_first);
    held_value.as.integer = 73;
    CHECK(sheet.override(isolated, isolated_table, "OTAECLAIR", "Kira", &held_value) == TSUCHI_BUSY);
    CHECK(whole(trove.parameters, "OTAECLAIR", "Kira") == 51);
    CHECK(lines.override(isolated, "ContentIsolation", "Older", "changed") == TSUCHI_BUSY);
    CHECK(word("ContentIsolation", "Older") == "held");
    CHECK(sheet.override(isolated, isolated_table, "OTAECLAIR", "Poko", &held_value) == TSUCHI_OK);
    CHECK(lines.override(isolated, "ContentIsolation", "Newer", "new") == TSUCHI_OK);
    CHECK(lines.override(isolated, "ContentIsolation", "Added", "retained") == TSUCHI_OK);
    CHECK(whole(trove.parameters, "OTAECLAIR", "Poko") == 73 && word("ContentIsolation", "Newer") == "new");
    const uint64_t isolated_last = tsuchi::ownership_mark(isolated);
    tsuchi::isolate_ownership(isolated, former_floor);
    tsuchi::release_owned_between(isolated, isolated_first, isolated_last);
    CHECK(whole(trove.parameters, "OTAECLAIR", "Kira") == 51 && whole(trove.parameters, "OTAECLAIR", "Poko") == 5);
    CHECK(word("ContentIsolation", "Older") == "held");
    char ended_text[32] = "unchanged";
    uint64_t ended_bytes = 0;
    CHECK(lines.get(mod, "ContentIsolation", "Newer", ended_text, sizeof ended_text, &ended_bytes) == TSUCHI_OK &&
          std::string(ended_text) == "original newer" && tsuchi::text_replacements() == isolated_replacements + 1);
    CHECK(word("ContentIsolation", "Added") == "retained");
    tsuchi::mod_stop(isolated);
    CHECK(whole(trove.parameters, "OTAECLAIR", "Kira") == 30);
    CHECK(lines.get(mod, "ContentIsolation", "Older", ended_text, sizeof ended_text, &ended_bytes) == TSUCHI_OK &&
          std::string(ended_text) == "original older" && tsuchi::text_replacements() == isolated_replacements);
    CHECK(word("ContentIsolation", "Added") == "retained");
    const tsuchi::ContentCounts before = tsuchi::content_counts();
    const unsigned changes = tsuchi::table_changes(), replaced = tsuchi::text_replacements();

    // ---- How it is written: judged as the manifest is read, with the treasure and what is wrong ----
    const std::string ruby_class_path = R"("class": "/G/Ruby.Ruby_C", "from": "OTAECLAIR")", named = R"("text": {"name": "Ruby"})";
    const std::pair<std::string, std::string> wrong[] = {
        {R"({"pikmin": []})", "\"content\": \"pikmin\" is an object: a short name for each type"},
        {R"({"pikmin": {"Amber": {"from":"red"}}})", "\"content\": Pikmin Amber: it needs a short name and an object"},
        {R"({"pikmin": {"amber": {}}})", "\"content\": Pikmin amber: \"from\" names a stock type from red through ice"},
        {R"({"pikmin": {"amber": {"from":"glow"}}})", "\"content\": Pikmin amber: \"from\" names a stock type from red through ice"},
        {R"({"pikmin": {"amber": {"from":"#8"}}})", "\"content\": Pikmin amber: \"from\" names a stock type from red through ice"},
        {R"({"pikmin": {"amber": {"from":"red", "class":"../other"}}})", "\"content\": Pikmin amber: \"class\" is the path of a Pikmin class"},
        {R"({"pikmin": {"amber": {"from":"red", "color":8}}})", "\"content\": Pikmin has a key the framework does not know: color"},
        {R"({"pikmin": {"amber": {"from":"red", "values":[]}}})", "\"content\": Pikmin values is an object of fields"},
        {R"({"pikmin": {"amber": {"from":"red", "values":{"PikminColor":8}}}})", "\"content\": Pikmin field cannot be set: PikminColor"},
        {R"({"pikmin": {"amber": {"from":"red", "values":{"pikmincolor[0]":8}}}})", "\"content\": Pikmin field cannot be set: pikmincolor[0]"},
        {R"({"pikmin": {"amber": {"from":"red", "values":{"LeafSpeed":[1]}}}})", "\"content\": Pikmin field cannot be set: LeafSpeed"},
        {R"([])", "\"content\" is an object: \"treasures\" and \"creatures\", each by a short name, and the treasures' \"treasure_series\""},
        {R"({"pikmins": {}})", "\"content\" has a key the framework does not know: pikmins"},
        {R"({"treasures": []})", "\"content\": \"treasures\" is an object: a short name for each treasure, and what it is"},
        {R"({"treasure_series": 5})", "\"content\": \"treasure_series\" is an object: a short name for each series, and what it is"},
        {R"({"treasure_series": {"Set": {"name": "A"}}})", "\"content\": series Set: its short name is spelled as a mod's ID is"},
        {R"({"treasure_series": {"set": {}}})", "\"content\": series set: it is an object with its \"name\", the name people see"},
        {R"({"treasure_series": {"set": {"name": "A", "bonus": 2}}})", "content: series bonus is a fraction from 0 to 1"},
        {"{\"treasures\": {\"Ruby\": {" + ruby_class_path + ", " + named + "}}}", "\"content\": treasure Ruby: its short name is spelled as a mod's ID is"},
        {R"({"treasures": {"ruby": 7}})", "\"content\": treasure ruby: it is an object: its \"class\", the treasure it starts \"from\", its \"text\", and what else it has of its own"},
        {"{\"treasures\": {\"ruby\": {" + ruby_class_path + ", " + named + ", \"weight\": 3}}}", "\"content\": treasure ruby: it has a key the framework does not know: weight"},
        {"{\"treasures\": {\"ruby\": {\"from\": \"OTAECLAIR\", " + named + "}}}", "\"content\": treasure ruby: \"class\" is the path of its Blueprint class, such as /Game/Mods/example_gems/GOtaRuby.GOtaRuby_C"},
        {"{\"treasures\": {\"ruby\": {\"class\": \"/G/Ruby.Ruby\", \"from\": \"OTAECLAIR\", " + named + "}}}",
         "\"content\": treasure ruby: \"class\" is the path of its Blueprint class, such as /Game/Mods/example_gems/GOtaRuby.GOtaRuby_C"},
        {"{\"treasures\": {\"ruby\": {\"class\": \"/G/Ruby.Ruby_C\", \"from\": \"no such\", " + named + "}}}",
         "\"content\": treasure ruby: \"from\" is the ID of the treasure its rows start as copies of, such as OTAECLAIR"},
        {"{\"treasures\": {\"ruby\": {\"class\": \"/G/Ruby.Ruby_C\", \"from\": \"#3\", " + named + "}}}",
         "\"content\": treasure ruby: \"from\" is the ID of the treasure its rows start as copies of, such as OTAECLAIR"},
        {"{\"treasures\": {\"ruby\": {" + ruby_class_path + "}}}", "\"content\": treasure ruby: \"text\" is an object with its \"name\", the name people see"},
        {"{\"treasures\": {\"ruby\": {" + ruby_class_path + R"(, "text": {"name": ""}}}})", "\"content\": treasure ruby: \"text\" is an object with its \"name\", the name people see"},
        {"{\"treasures\": {\"ruby\": {" + ruby_class_path + R"(, "text": {"name": "Ruby", "motto": "x"}}}})", "\"content\": treasure ruby: \"text\" has a key the framework does not know: motto"},
        {"{\"treasures\": {\"ruby\": {" + ruby_class_path + R"(, "text": {"name": "Ruby", "description": 5}}}})", "\"content\": treasure ruby: \"text\": each of its texts is text"},
        {"{\"treasures\": {\"ruby\": {" + ruby_class_path + ", " + named + R"(, "sparklium": -1}}})", "\"content\": treasure ruby: \"sparklium\" is what it is worth, a whole number from 0"},
        {"{\"treasures\": {\"ruby\": {" + ruby_class_path + ", " + named + R"(, "carriers": [8, 4]}}})",
         "\"content\": treasure ruby: \"carriers\" and \"battle_carriers\" are the fewest and the most Pikmin that carry it: two whole numbers from 1, or one for both"},
        {"{\"treasures\": {\"ruby\": {" + ruby_class_path + ", " + named + R"(, "battle_carriers": 0}}})",
         "\"content\": treasure ruby: \"carriers\" and \"battle_carriers\" are the fewest and the most Pikmin that carry it: two whole numbers from 1, or one for both"},
        {"{\"treasures\": {\"ruby\": {" + ruby_class_path + ", " + named + R"(, "icon": "T_Ruby"}}})", "\"content\": treasure ruby: \"icon\" is the path of a texture, such as /Game/Mods/example_gems/T_Ruby.T_Ruby"},
        {"{\"treasures\": {\"ruby\": {" + ruby_class_path + ", " + named + R"(, "values": {"Shine": [1]}}}})",
         "\"content\": treasure ruby: \"values\" is an object: fields of its row by their paths, each with a number, true or false, or text"},
        {"{\"treasures\": {\"ruby\": {" + ruby_class_path + ", " + named + R"(, "catalog": {}}}})", "\"content\": treasure ruby: \"catalog\" is an object with the \"series\" it is in, by the series' short name"},
        {"{\"treasures\": {\"ruby\": {" + ruby_class_path + ", " + named + R"(, "catalog": {"series": "gold"}}}})",
         "\"content\": treasure ruby: \"catalog\" names a series that \"treasure_series\" does not have: gold"},
        {"{\"treasure_series\": {\"set\": {\"name\": \"A\"}}, \"treasures\": {\"ruby\": {" + ruby_class_path + ", " + named + R"(, "catalog": {"series": "set", "page": 2}}}})",
         "\"content\": treasure ruby: \"catalog\" has a key the framework does not know: page"},
    };
    int number = 0;
    for (const auto& each : wrong) plain("odd" + std::to_string(number++), each.first);
    plain("odd.none", "{}"), plain("odd.empty", R"({"treasures": {}, "treasure_series": {}, "pikmin": {}})");
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    number = 0;
    for (const auto& each : wrong) CHECK(told(("odd" + std::to_string(number++)).c_str(), "mod.json: " + each.second));
    // A mod that declares nothing under it starts, and nothing is asked of the save for it.
    tsuchi::start_mods();
    CHECK(told("odd.none", "<running>") && told("odd.empty", "<running>") && !disk.files.count(record_at) && disk.changes.empty());
    tsuchi::forget_mods();
    for (int i = 0; i < number; ++i) gone("odd" + std::to_string(i));
    gone("odd.none"), gone("odd.empty");

    // ---- A mod with two treasures and a series, and a mod that claims the first one's class ----
    const std::string gems = R"({"treasure_series": {"set": {"name": "Gem Set"}}, "treasures": {
        "ruby": {"class": "/G/Ruby.Ruby_C", "from": "OTAECLAIR", "sparklium": 120, "carriers": [4, 8], "icon": "/G/RubyIcon.RubyIcon",
                 "text": {"name": "Ruby", "result_name": "A Ruby", "description": "A red stone."}, "values": {"Shine": 2.5, "Poko": 9},
                 "catalog": {"series": "set", "values": {"TotalPlaceNum": 3, "OtakaraZukanIDInfo.SeriesID": 2,
                     "OtakaraZukanIDInfo.ZukanNo": 99, "DayParams[0].ActorName": "OTAECLAIR"}}},
        "coin": {"class": "/G/Coin.Coin_C", "from": "gems:ruby", "sparklium": 7, "carriers": 3, "battle_carriers": [1, 2], "text": {"name": "Coin"}}}})";
    plain("gems", gems);
    disk.files[mods_at + "gems/main.lua"] = "print('has', story.treasure_count('gems:ruby'), story.treasure_count('gems:coin'), story.treasure_count('gems:pearl'))\n";
    plain("rival", R"({"treasures": {"gem": {"class": "/G/Ruby.Ruby_C", "from": "OTAECLAIR", "text": {"name": "Gem"}}}})");
    CHECK(collected("gems:ruby") == -int(TSUCHI_NOT_FOUND) && collected("OTATSUCHI0001") == -int(TSUCHI_NOT_FOUND));
    CHECK(tsuchi::find_mods() == TSUCHI_OK && told("gems", "<ready>") && told("rival", "<ready>"));
    // A slot of the world's list that is free, and happens to hold the name still, is no entry.
    {
        namespace l = t::row_list;
        const uint64_t list = rows_list();
        trove_enter(reinterpret_cast<void*>(list - t::data_table::rows), row_name(u"OTATSUCHI0001"), reinterpret_cast<void*>(uint64_t(0x40)));
        const int32_t last = fake::get<int32_t>(list + l::slot_count) - 1;
        const uint64_t at = list + l::used_bits + 4 * uint64_t(last >> 5);
        fake::put<uint32_t>(at, fake::get<uint32_t>(at) & ~(1u << (last & 31))), fake::put<int32_t>(list + l::free_count, 1);
        trove.entered = 0;
    }
    logged.lines.clear(), disk.changes.clear();
    tsuchi::start_mods();
    CHECK(told("gems", "<running>") && closed() && disk.well_formed);
    // The numbers were given in the order the manifest has its things, and are in the save before anything else is done.
    CHECK(saved_record(record_at) == "{\"treasures\": {\"gems:ruby\": 1, \"gems:coin\": 2, \"rival:gem\": 3}, \"series\": {\"gems:set\": 1000}}\n");
    // Its rows: a copy of the treasure it starts from in each table, under the name the framework gave it, with
    // what the manifest says in place of what that treasure has. The treasure it starts from is as it was.
    CHECK(has_row(trove.parameters, u"OTATSUCHI0001") && has_row(trove.classes, u"OTATSUCHI0001") && has_row(trove.icons, u"OTATSUCHI0001") && has_row(trove.catalog, u"OTATSUCHI0001"));
    CHECK(text_in(trove.parameters, "OTATSUCHI0001", "ActorName") == "Ruby" && whole(trove.parameters, "OTATSUCHI0001", "Kira") == 120 && whole(trove.parameters, "OTATSUCHI0001", "Poko") == 9 &&
          whole(trove.parameters, "OTATSUCHI0001", "CarryWeightMin") == 4 && whole(trove.parameters, "OTATSUCHI0001", "CarryWeightMax") == 8 &&
          whole(trove.parameters, "OTATSUCHI0001", "CarryWeightMinVs") == 4 && whole(trove.parameters, "OTATSUCHI0001", "CarryWeightMaxVs") == 8);
    CHECK(fake::get<float>(row_at(trove.parameters, u"OTATSUCHI0001") + 0x20) == 2.5f && fake::get<float>(eclair + 0x20) == 1.5f);
    CHECK(text_in(trove.classes, "OTATSUCHI0001", "DayCookBlueprint") == "/G/Ruby.Ruby_C" && text_in(trove.icons, "OTATSUCHI0001", "Texture2D") == "/G/RubyIcon.RubyIcon");
    // values cannot put a treasure in a native series, change its order, or give the stage another actor.
    CHECK(whole(trove.catalog, "OTATSUCHI0001", "OtakaraZukanIDInfo.SeriesID") == 1000 && whole(trove.catalog, "OTATSUCHI0001", "OtakaraZukanIDInfo.ZukanNo") == 0 &&
          text_in(trove.catalog, "OTATSUCHI0001", "DayParams[0].ActorName") == "OTATSUCHI0001" && whole(trove.catalog, "OTATSUCHI0001", "TotalPlaceNum") == 3);
    CHECK(text_in(trove.parameters, "OTAECLAIR", "ActorName") == "GOtaEclair" && whole(trove.parameters, "OTAECLAIR", "Kira") == 30 && whole(trove.parameters, "OTAECLAIR", "CarryWeightMin") == 2 &&
          whole(trove.catalog, "OTAECLAIR", "OtakaraZukanIDInfo.SeriesID") == 2 && text_in(trove.catalog, "OTAECLAIR", "DayParams[0].ActorName") == "OTAECLAIR" &&
          text_in(trove.classes, "OTAECLAIR", "DayCookBlueprint") == "/G/Eclair.Eclair_C" && text_in(trove.icons, "OTAECLAIR", "Texture2D") == "/G/EclairIcon.Icon");
    // The second starts from the first, a mod's own, and is in no catalog: it has no row there.
    CHECK(text_in(trove.parameters, "OTATSUCHI0002", "ActorName") == "Coin" && whole(trove.parameters, "OTATSUCHI0002", "Kira") == 7 && whole(trove.parameters, "OTATSUCHI0002", "CarryWeightMin") == 3 &&
          whole(trove.parameters, "OTATSUCHI0002", "CarryWeightMax") == 3 && whole(trove.parameters, "OTATSUCHI0002", "CarryWeightMinVs") == 1 && whole(trove.parameters, "OTATSUCHI0002", "CarryWeightMaxVs") == 2 &&
          text_in(trove.classes, "OTATSUCHI0002", "DayCookBlueprint") == "/G/Coin.Coin_C" && text_in(trove.icons, "OTATSUCHI0002", "Texture2D") == "/G/RubyIcon.RubyIcon" &&
          !has_row(trove.catalog, u"OTATSUCHI0002"));
    // Its texts, under its name: a name the manifest gives once is its name everywhere, and a note it does not give is empty.
    CHECK(word("OtakaraName", "OTATSUCHI0001") == "Ruby" && word("OtakaraName_PictureBook", "OTATSUCHI0001") == "Ruby" && word("OtakaraName_Result", "OTATSUCHI0001") == "A Ruby" &&
          word("OtakaraDesc", "OTATSUCHI0001") == "A red stone." && word("OtakaraDescOlimar", "OTATSUCHI0001") == "" && word("OtakaraDescLouie", "OTATSUCHI0001") == "" &&
          word("OtakaraName", "OTATSUCHI0002") == "Coin" && word("OtakaraSeries", "Series1000") == "Gem Set" && word("OtakaraSeries_Result", "Series1000") == "Gem Set");
    // Every actor of its class carries the name, since the class's default object does.
    CHECK(carried("Ruby") == "OTATSUCHI0001" && carried("Coin") == "OTATSUCHI0002" && object_kept(ruby_class) && object_kept(ruby_icon) && object_kept(trove.parameters) && object_kept(trove.catalog));
    // The loaded world's lists have its rows, the slot that was free notwithstanding, and the game is to count
    // the Piklopedia's treasures again.
    CHECK(in_list(rows_list(), u"OTATSUCHI0001") == row_at(trove.parameters, u"OTATSUCHI0001") && in_list(rows_list(), u"OTATSUCHI0002") &&
          in_list(catalog_list(), u"OTATSUCHI0001") == row_at(trove.catalog, u"OTATSUCHI0001") && !in_list(catalog_list(), u"OTATSUCHI0002") &&
          in_list(icons_list(), u"OTATSUCHI0001") && in_list(icons_list(), u"OTATSUCHI0002") && trove.entered == 5 && !counted());
    // The ID is the treasure's, to the mod's own code as it starts and to everyone after: the story's record of it
    // is read and added to by the ID, and the game is asked by the name it knows.
    CHECK(logged.lines.find("[gems] has\t0\t0\tnil\tnot_found") != std::string::npos);
    CHECK(collected("gems:ruby") == 0 && collected("gems:coin") == 0 && collected("gems:pearl") == -int(TSUCHI_NOT_FOUND) && collected("rival:gem") == -int(TSUCHI_NOT_FOUND));
    CHECK(tale.give_treasure(mod, "gems:ruby", true) == TSUCHI_OK && hoard.log == "collect:OTATSUCHI0001 120 9 sparklium:120+shown " && collected("gems:ruby") == 1 && collected("gems:coin") == 0);
    hoard.log.clear();
    // An actor of the kind is told as the ID, and any other as the game names it.
    {
        const Name own_kind = fake::get<Name>(w.captain + t::game_actor::kind);
        char kind[TSUCHI_ID_SIZE] = "";
        uint64_t bytes = 0;
        fake::put(w.captain + t::game_actor::kind, row_name(u"OTATSUCHI0001"));
        CHECK(api.kind(mod, handle(w.captain), kind, sizeof kind, &bytes) == TSUCHI_OK && !std::strcmp(kind, "gems:ruby") && bytes == 10 && api.kind(mod, handle(w.captain), kind, 9, &bytes) == TSUCHI_CAPACITY &&
              bytes == 10);
        fake::put(w.captain + t::game_actor::kind, row_name(u"OTAECLAIR"));
        CHECK(api.kind(mod, handle(w.captain), kind, sizeof kind, &bytes) == TSUCHI_OK && !std::strcmp(kind, "OTAECLAIR"));
        fake::put(w.captain + t::game_actor::kind, own_kind);
    }
    // A class carries one name. The mod that asked second is blocked, with whose the class is, and has nothing.
    CHECK(told("rival", "mod.json: \"content\": treasure gem: its class, /G/Ruby.Ruby_C, is already the treasure gems:ruby's") && !tsuchi::find_mod("rival")->context &&
          !has_row(trove.parameters, u"OTATSUCHI0003") && carried("Ruby") == "OTATSUCHI0001");
    // An ID is its mod's own: no other mod is given it, and it names one thing, as a name of the game's does.
    {
        TSUCHI_Mod* const owner = tsuchi::find_mod("gems")->context;
        Name other{}, ruby{}, found{};
        char id[TSUCHI_ID_SIZE] = "";
        CHECK(name_from_utf8("OTAOTHER", other) == TSUCHI_OK && game::added_name("gems:ruby", ruby) && game::added_id(ruby, id) && !std::strcmp(id, "gems:ruby") && !game::added_id(other, id));
        CHECK(game::add_name(mod, "gems:ruby", other) == TSUCHI_INVALID && game::add_name(mod, "gems:pearl", other) == TSUCHI_INVALID && game::add_name(owner, "OTAOTHER", other) == TSUCHI_INVALID &&
              game::add_name(owner, "gems:pearl", Name{}) == TSUCHI_INVALID);
        CHECK(game::add_name(owner, "gems:ruby", other) == TSUCHI_BUSY && game::add_name(owner, "gems:pearl", ruby) == TSUCHI_BUSY && !game::added_name("gems:pearl", found));
        CHECK(game::add_name(owner, "gems:pearl", other) == TSUCHI_OK && game::added_name("gems:pearl", found) && found.index == other.index && game::added_id(other, id) && !std::strcmp(id, "gems:pearl"));
    }
    tsuchi::ContentCounts now = tsuchi::content_counts();
    CHECK(now.treasures == 2 && now.declared == before.declared + 2 && now.rows_added == before.rows_added + 7 && now.entered == before.entered + 5 && now.refused == before.refused + 1 &&
          now.numbers_given == before.numbers_given + 4 && now.records_unread == before.records_unread && now.records_unwritten == before.records_unwritten);

    // ---- Another world: its lists are its own, made from the tables or not, and get what they lack ----
    new_world(); // the game built these from the tables, which have the rows
    tsuchi::frame(0.03f);
    CHECK(trove.entered == 0 && counted() && collected("gems:ruby") == 1);
    new_world();
    for (uint64_t list : {rows_list(), catalog_list(), icons_list()}) fake::put<int32_t>(list + t::row_list::slot_count, 0); // a world whose lists were made before the rows
    CHECK(collected("gems:ruby") == -int(TSUCHI_NOT_FOUND));
    tsuchi::frame(0.03f);
    CHECK(trove.entered == 5 && !counted() && collected("gems:ruby") == 1 && in_list(catalog_list(), u"OTATSUCHI0001") && in_list(icons_list(), u"OTATSUCHI0002"));
    tsuchi::frame(0.03f);
    CHECK(trove.entered == 5);
    // No world, and then one that keeps another table than the rows are in: nothing is entered there.
    fake::put<uint64_t>(w.framework + t::framework_treasures::rows, 0), fake::put<uint64_t>(w.framework + t::framework_treasures::icons, 0);
    tsuchi::frame(0.03f);
    new_world();
    for (uint64_t list : {rows_list(), catalog_list(), icons_list()}) fake::put<int32_t>(list + t::row_list::slot_count, 0);
    fake::put(trove.rows_keeper + t::treasure_keeper::catalog_table, trove.classes);
    const auto detail_before=tsuchi::content_entry_counts();
    const uint32_t passed = tsuchi::content_counts().not_entered;
    tsuchi::frame(0.03f);
    CHECK(trove.entered == 4 && counted() && !in_list(catalog_list(), u"OTATSUCHI0001") && tsuchi::content_counts().not_entered == passed + 1);
    const auto detail_after=tsuchi::content_entry_counts();
    CHECK(detail_after.tables[3].other_table==detail_before.tables[3].other_table+1);
    CHECK(detail_after.tables[2].other_table==detail_before.tables[2].other_table && detail_after.tables[2].bad_list==detail_before.tables[2].bad_list);
    new_world();
    fake::put<uint64_t>(trove.icons_keeper + t::icon_keeper::table, 0);
    fake::put<int32_t>(icons_list() + t::row_list::slot_count, 0);
    const auto waiting_before = tsuchi::content_entry_counts();
    tsuchi::frame(0.03f);
    CHECK(tsuchi::content_entry_counts().tables[2].keeper_not_ready == waiting_before.tables[2].keeper_not_ready + 2);
    CHECK(!in_list(icons_list(), u"OTATSUCHI0001") && !in_list(icons_list(), u"OTATSUCHI0002"));
    // The keeper stays alive while its table loads. Enter both missing icons in that same keeper.
    const uint64_t waiting_keeper = trove.icons_keeper;
    fake::put(trove.icons_keeper + t::icon_keeper::table, trove.icons);
    tsuchi::frame(0.03f);
    CHECK(trove.icons_keeper == waiting_keeper && in_list(icons_list(), u"OTATSUCHI0001") && in_list(icons_list(), u"OTATSUCHI0002"));
    const int entered_when_ready = trove.entered;
    tsuchi::frame(0.03f);
    CHECK(trove.entered == entered_when_ready);
    // The same keeper and source table can rebuild the list after the first insertion.
    fake::put<int32_t>(icons_list() + t::row_list::slot_count, 0);
    tsuchi::frame(0.03f);
    CHECK(in_list(icons_list(), u"OTATSUCHI0001") && in_list(icons_list(), u"OTATSUCHI0002") && trove.entered == entered_when_ready + 2);
    // If insertion is not available yet, readiness can change without changing either pointer or list header.
    const auto old_insert = t::fn::add_table_row_internal;
    t::fn::add_table_row_internal = nullptr;
    fake::put<int32_t>(icons_list() + t::row_list::slot_count, 0);
    tsuchi::frame(0.03f);
    CHECK(!in_list(icons_list(), u"OTATSUCHI0001"));
    t::fn::add_table_row_internal = old_insert;
    for (int frame = 0; frame < 31; ++frame) tsuchi::frame(0.03f);
    CHECK(in_list(icons_list(), u"OTATSUCHI0001") && in_list(icons_list(), u"OTATSUCHI0002"));
    // An initialization that never completes stops retrying, instead of scanning rows forever.
    t::fn::add_table_row_internal = nullptr;
    fake::put<int32_t>(icons_list() + t::row_list::slot_count, 0);
    tsuchi::frame(0.03f);
    for (int frame = 0; frame < 650; ++frame) tsuchi::frame(0.03f);
    const uint32_t stopped_tries = tsuchi::content_entry_counts().tables[2].no_insert;
    for (int frame = 0; frame < 90; ++frame) tsuchi::frame(0.03f);
    CHECK(tsuchi::content_entry_counts().tables[2].no_insert == stopped_tries);
    t::fn::add_table_row_internal = old_insert;
    new_world();

    // ---- The mod stops ----
    tsuchi::forget_mods();
    now = tsuchi::content_counts();
    // The ID names nothing, the fields of the rows and the holds on what was loaded are as before the mod, and
    // the texts the game had no key for keep what they were last given.
    Name none{};
    CHECK(now.treasures == 0 && collected("gems:ruby") == -int(TSUCHI_NOT_FOUND) && !game::added_name("gems:ruby", none) && !game::added_name("gems:pearl", none) &&
          tsuchi::table_changes() == changes && tsuchi::text_replacements() == replaced && !object_kept(ruby_class) && !object_kept(ruby_icon) && !object_kept(trove.parameters));
    // The rows stay, as copies of the treasure they started from; the one in the Piklopedia's table is in no
    // series, and the game is to count again. The accepted class-default identity stays for the run.
    CHECK(has_row(trove.parameters, u"OTATSUCHI0001") && has_row(trove.catalog, u"OTATSUCHI0001") && whole(trove.parameters, "OTATSUCHI0001", "Kira") == 30 &&
          text_in(trove.parameters, "OTATSUCHI0001", "ActorName") == "GOtaEclair" && whole(trove.catalog, "OTATSUCHI0001", "OtakaraZukanIDInfo.SeriesID") == -1 &&
          whole(trove.catalog, "OTAECLAIR", "OtakaraZukanIDInfo.SeriesID") == 2 && !counted() && carried("Ruby") == "OTATSUCHI0001" && word("OtakaraName", "OTATSUCHI0001") == "Ruby");
    // The story's record is the game's, under the game's name, and stays.
    CHECK(collected("OTATSUCHI0001") == 1);

    // ---- It starts again in the same run: the same numbers from the save, and the rows it left ----
    gone("rival");
    const std::string kept = saved_record(record_at);
    // A parsed newest copy with lost allocation groups cannot hand the older IDs out again.
    disk.files[record_at]="{\"counter\": 1, "+kept.substr(1);
    const std::string newer=record_at.substr(0,record_at.size()-5)+".2.json";
    disk.files[newer]=R"({"counter": 2, "future": {"flag": true}})";
    disk.changes.clear();
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    const tsuchi::ContentCounts again = tsuchi::content_counts();
    CHECK(told("gems", "<running>") && collected("gems:ruby")==1 && carried("Ruby")=="OTATSUCHI0001" && carried("Coin")=="OTATSUCHI0002");
    CHECK(disk.files[newer]==R"({"counter": 2, "future": {"flag": true}})");
    disk.files.erase(newer);
    CHECK(told("gems", "<running>") && saved_record(record_at) == kept && disk.changes.empty() && again.numbers_given == now.numbers_given && again.rows_added == now.rows_added &&
          again.treasures == 2 && trove.entered == 0);
    CHECK(whole(trove.parameters, "OTATSUCHI0001", "Kira") == 120 && whole(trove.catalog, "OTATSUCHI0001", "OtakaraZukanIDInfo.SeriesID") == 1000 && collected("gems:ruby") == 1 &&
          carried("Coin") == "OTATSUCHI0002");
    tsuchi::forget_mods();

    // ---- The mod is absent for a run, and another brings a treasure: it gets a number nobody had ----
    const auto new_run = [&] { // what a run of the game starts with: the tables and the classes as the game ships them, and the save
        for (auto row = sheets.rows.begin(); row != sheets.rows.end();) {
            char name[64] = "";
            Name packed{};
            std::memcpy(&packed, &row->first.second, sizeof packed);
            name_text(packed, name, sizeof name);
            row = !std::strncmp(name, "OTATSUCHI", 9) ? sheets.rows.erase(row) : std::next(row);
        }
        for (const auto& [name, pattern] : patterns) fake::put(pattern + t::game_actor::kind, row_name(u"OTAECLAIR"));
        new_world();
    };
    new_run();
    gone("gems");
    plain("ores", R"({"treasure_series": {"heap": {"name": "Ore Heap"}}, "treasures": {"iron": {"class": "/G/Iron.Iron_C", "from": "OTAECLAIR", "text": {"name": "Iron"}, "catalog": {"series": "heap"}}}})");
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(told("ores", "<running>") && saved_record(record_at) ==
          "{\"treasures\": {\"gems:ruby\": 1, \"gems:coin\": 2, \"rival:gem\": 3, \"ores:iron\": 4}, \"series\": {\"gems:set\": 1000, \"ores:heap\": 1001}}\n");
    CHECK(has_row(trove.parameters, u"OTATSUCHI0004") && !has_row(trove.parameters, u"OTATSUCHI0001") && whole(trove.catalog, "OTATSUCHI0004", "OtakaraZukanIDInfo.SeriesID") == 1001 &&
          word("OtakaraSeries", "Series1001") == "Ore Heap" && carried("Iron") == "OTATSUCHI0004" && collected("ores:iron") == 0 && collected("gems:ruby") == -int(TSUCHI_NOT_FOUND));
    // The save still has the absent mod's treasure, under a name the game has no row for now.
    CHECK(hoard.records.count(row_name(u"OTATSUCHI0001")) && collected("OTATSUCHI0001") == -int(TSUCHI_NOT_FOUND));
    tsuchi::forget_mods();
    // ---- It is back in the run after: its treasures have the names they had, and the story's record with them ----
    new_run();
    plain("gems", gems);
    const std::string both = saved_record(record_at);
    const uint32_t rows_before = tsuchi::content_counts().rows_added;
    disk.changes.clear();
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(told("gems", "<running>") && told("ores", "<running>") && saved_record(record_at) == both && disk.changes.empty() && tsuchi::content_counts().rows_added == rows_before + 11);
    CHECK(carried("Ruby") == "OTATSUCHI0001" && carried("Coin") == "OTATSUCHI0002" && carried("Iron") == "OTATSUCHI0004" && collected("gems:ruby") == 1 && collected("ores:iron") == 0 &&
          whole(trove.catalog, "OTATSUCHI0001", "OtakaraZukanIDInfo.SeriesID") == 1000 && whole(trove.catalog, "OTATSUCHI0004", "OtakaraZukanIDInfo.SeriesID") == 1001 &&
          whole(trove.catalog, "OTATSUCHI0004", "OtakaraZukanIDInfo.ZukanNo") == 0 && tsuchi::content_counts().treasures == 3);
    tsuchi::forget_mods();
    gone("gems"), gone("ores");

    // ---- What the game will not take blocks the mod as it starts, with the treasure and the reason ----
    const auto treasure = [](const std::string& of_class, const std::string& from, const std::string& more = "") {
        return "{\"treasure_series\": {\"set\": {\"name\": \"Set\"}}, \"treasures\": {\"one\": {\"class\": \"" + of_class + "\", \"from\": \"" + from + "\", \"text\": {\"name\": \"One\"}" + more + "}}}";
    };
    plain("no.class", treasure("/G/Nope.Nope_C", "OTAECLAIR"));
    plain("no.cid", treasure("/G/Plain.Plain_C", "OTAECLAIR"));
    plain("no.from", treasure("/G/Late.Late_C", "OTANOPE"));
    plain("no.mod", treasure("/G/Late.Late_C", "gems:ruby"));
    plain("no.shown", treasure("/G/Late.Late_C", "OTAPART", R"(, "catalog": {"series": "set"})"));
    plain("no.field", treasure("/G/Late.Late_C", "OTAECLAIR", R"(, "values": {"Weight": 1})"));
    plain("no.icon", treasure("/G/Late.Late_C", "OTAECLAIR", R"(, "icon": "/G/Nope.Nope")"));
    plain("no.twice", R"({"treasures": {"one": {"class": "/G/Late.Late_C", "from": "OTAECLAIR", "text": {"name": "One"}}, "two": {"class": "/G/Late.Late_C", "from": "OTAECLAIR", "text": {"name": "Two"}}}})");
    const tsuchi::ContentCounts calm = tsuchi::content_counts();
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(told("no.class", "mod.json: \"content\": treasure one: its class, /G/Nope.Nope_C, could not be loaded: the game finds no such asset"));
    CHECK(told("no.cid", "mod.json: \"content\": treasure one: its class, /G/Plain.Plain_C, is no class of a treasure: its default object has no CID"));
    CHECK(told("no.from", "mod.json: \"content\": treasure one: the treasure it starts from, OTANOPE, is no treasure the game has"));
    CHECK(told("no.mod", "mod.json: \"content\": treasure one: the treasure it starts from, gems:ruby, is no treasure of a mod that is running"));
    CHECK(told("no.shown", "mod.json: \"content\": treasure one: the treasure it starts from, OTAPART, has no row in the Piklopedia's table of treasures"));
    CHECK(told("no.field", "mod.json: \"content\": treasure one: field Weight of its row in the treasures' table: the game has none of that name"));
    CHECK(told("no.icon", "mod.json: \"content\": treasure one: its icon, /G/Nope.Nope, could not be loaded: the game finds no such asset"));
    CHECK(told("no.twice", "mod.json: \"content\": treasure two: its class, /G/Late.Late_C, is already the treasure no.twice:one's"));
    // Nothing of a mod that was blocked is left in force: no treasure, no change to a row, no text, no hold.
    now = tsuchi::content_counts();
    CHECK(now.treasures == 0 && now.refused == calm.refused + 8 && tsuchi::table_changes() == changes && tsuchi::text_replacements() == replaced && !object_kept(trove.parameters) &&
          !game::added_name("no.twice:one", none) && carried("Late") == "OTAECLAIR");
    tsuchi::forget_mods();
    for (const char* id : {"no.class", "no.cid", "no.from", "no.mod", "no.shown", "no.field", "no.icon", "no.twice"}) gone(id);

    // ---- The numbers cannot be read or kept: the mod is blocked, and nothing is made with a number that may not last ----
    plain("late", R"({"treasures": {"gift": {"class": "/G/Late.Late_C", "from": "OTAECLAIR", "text": {"name": "Gift"}}}})");
    const std::string sound = saved_record(record_at);
    const auto starts = [&] {
        tsuchi::forget_records();
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        tsuchi::start_mods();
        const std::string said = problem("late");
        tsuchi::forget_mods();
        return said;
    };
    const uint32_t rows_now = tsuchi::content_counts().rows_added, unread = tsuchi::content_counts().records_unread, unwritten = tsuchi::content_counts().records_unwritten;
    for (const char* damaged : {"[]", "{ nope", R"({"treasures": []})", R"({"treasures": {"gems:ruby": 0}})", R"({"treasures": {"gems:ruby": 1, "gems:coin": 1}})", R"({"treasures": {"OTAECLAIR": 1}})",
                                R"({"series": {"gems:set": 999}})", R"({"treasures": {"gems:ruby": 1.5}})"}) {
        disk.files[record_at] = damaged;
        restart_records(record_at);
        CHECK(starts() == "mod.json: \"content\": the numbers the framework has given out could not be read from the save: its record of them is damaged" && saved_record(record_at) == damaged);
    }
    disk.files[record_at] = sound;
    restart_records(record_at);
    disk.mounts = false;
    CHECK(starts() == "mod.json: \"content\": the numbers the framework has given out could not be read from the save: the save could not be opened or read");
    disk.mounts = true;
    disk.refuse = "write";
    CHECK(starts() == "mod.json: \"content\": the numbers the framework gave it could not be kept in the save: the save could not be written");
    disk.refuse.clear();
    now = tsuchi::content_counts();
    CHECK(now.rows_added == rows_now && now.records_unread == unread + 9 && now.records_unwritten == unwritten + 1 && carried("Late") == "OTAECLAIR");
    // With the save as it should be, the mod starts, and its treasure has the number after the highest ever given.
    disk.files[record_at] = sound;
    restart_records(record_at);
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(told("late", "<running>") && saved_record(record_at).find("\"late:gift\": 14") != std::string::npos && collected("late:gift") == 0 && carried("Late") == "OTATSUCHI0014");
    tsuchi::forget_mods();
    gone("late");

    // A dotted package folder cannot be a Blueprint class package.
    const std::string bad_path=mods_at+"dot/mod.json";
    disk.files[bad_path]=R"({"id":"dot", "name":"Dot", "version":"1.0.0", "content":{"treasures":{"gift":{"class":"/G/with.dot/T.T_C","from":"OTAECLAIR","text":{"name":"Gift"}}}}})";
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods()==TSUCHI_OK && problem("dot").find("no content path")!=std::string::npos);
    disk.files.erase(bad_path);

    // As it was, for what comes after.
    t::fn::add_table_row = old_add, t::fn::add_table_row_internal = old_enter, t::fn::find_treasure_row = old_find;
    fake::put(w.framework + t::game_framework::ui, old_ui);
    fake::put<uint64_t>(w.framework + t::framework_treasures::rows, 0), fake::put<uint64_t>(w.framework + t::framework_treasures::icons, 0);
    fake::put(running.instance + t::game_instance::story_save, old_story), fake::put<uint8_t>(running.context + t::game_context::rule, old_rule);
    disk.files = other_files, disk.changes.clear();
    logged.lines.clear(), hoard.log.clear();
    tsuchi::set_log_sink(nullptr);
}
