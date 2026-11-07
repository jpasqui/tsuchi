// Checks of the creatures a manifest adds to the game under "content". Included by objects_test.cpp after
// content_test.h: these use the made-up game's table of classes and its table of icons from there, which
// creatures and treasures share, and add the creatures' own two tables and what keeps their rows in a world.

namespace {

struct Zoo {
    uint64_t parameters = 0, catalog = 0;   // the creatures' table, and the Piklopedia's table of them
    uint64_t keeper = 0, icons_keeper = 0; // what a loaded world keeps their rows by name in
    int entered = 0, removed = 0;
    std::vector<uint64_t> freed;
} zoo;
// In a row of the Piklopedia's table: the actors it stages by day, and those at night. Each is a name and a number.
constexpr uint32_t kDayActorsAt = 0x0, kNightActorsAt = 0x10, kStagedBytes = 0x10;

// The engine's way of adding a row: a copy under the name, and of the two lists a row of the Piklopedia's has in it.
void zoo_add_row(void* table, uint64_t name, void* from) {
    if (reinterpret_cast<uint64_t>(table) != zoo.catalog) return trove_add_row(table, name, from);
    const uint64_t copy = fake::memory(kRowBytes);
    std::memcpy(reinterpret_cast<void*>(copy), from, kRowBytes);
    for (uint32_t at : {kDayActorsAt, kNightActorsAt}) {
        const int32_t count = fake::get<int32_t>(copy + at + t::array::count);
        const uint64_t data = fake::memory(size_t(kStagedBytes) * size_t(count ? count : 1));
        if (count) std::memcpy(reinterpret_cast<void*>(data), reinterpret_cast<void*>(fake::get<uint64_t>(copy + at + t::array::data)), size_t(kStagedBytes) * size_t(count));
        fake::put(copy + at + t::array::data, data);
    }
    sheets.rows[{zoo.catalog, name}] = copy;
}
void zoo_enter(void* table, uint64_t name, void* row) {
    trove_enter(table, name, row);
    ++zoo.entered;
}
// The engine's way of taking a row out, which frees it.
void zoo_remove_row(void* table, uint64_t name) {
    const auto key=std::make_pair(reinterpret_cast<uint64_t>(table),name);
    const auto found=sheets.rows.find(key);
    if(found==sheets.rows.end()) return;
    const uint64_t row=found->second;
    memset(reinterpret_cast<void*>(row),0xdd,kRowBytes);
    zoo.freed.push_back(row);
    sheets.rows.erase(found), ++zoo.removed;
}

} // namespace

void adding_creatures() {
    namespace game = tsuchi::game;
    namespace k = t::creature_keeper;
    const TSUCHI_DatatableService& sheet = *tsuchi::host.datatable;
    const TSUCHI_TextService& lines = *tsuchi::host.text;
    const TSUCHI_StoryService& tale = *tsuchi::host.story;
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/", record_at = "save:/tsuchi/tsuchi.content/creatures.json", treasures_at = "save:/tsuchi/tsuchi.content/treasures.json";
    const auto other_files = disk.files;
    disk.files.clear(), disk.folders.clear(), disk.changes.clear();
    tsuchi::set_log_sink(log_sink);
    const auto plain = [&](const std::string& id, const std::string& content) {
        disk.files[mods_at + id + "/mod.json"] = "{\"id\": \"" + id + "\", \"name\": \"The " + id + " mod\", \"version\": \"1.0.0\"" + (content.empty() ? "" : ", \"content\": " + content) + "}";
    };
    const auto gone = [&](const std::string& id) { disk.files.erase(mods_at + id + "/mod.json"); };
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

    // ---- The made-up game's side: its two tables of creatures, a creature in all four tables, classes, a world ----
    const auto old_add = t::fn::add_table_row;
    const auto old_enter = t::fn::add_table_row_internal;
    const auto old_remove = t::fn::remove_table_row;
    namespace f = t::data_table_table;
    const uint64_t old_slots[] = {fake::get<uint64_t>(trove.functions + f::add_row), fake::get<uint64_t>(trove.functions + f::remove_row), fake::get<uint64_t>(trove.functions + f::add_row_internal)};
    t::fn::add_table_row = zoo_add_row, t::fn::add_table_row_internal = zoo_enter, t::fn::remove_table_row = zoo_remove_row;
    fake::put(trove.functions + f::add_row, &zoo_add_row), fake::put(trove.functions + f::remove_row, &zoo_remove_row), fake::put(trove.functions + f::add_row_internal, &zoo_enter);
    const uint64_t table_class = fake::get<uint64_t>(w.creature_table + t::object::class_);
    const auto table_of = [&](const std::u16string& name, const std::u16string& path, uint64_t row_type) {
        const uint64_t made = fake::object(name, table_class, 0x90);
        fake::put(made, trove.functions), fake::put(made + t::data_table::row_struct, row_type);
        shelve(path, made);
        return made;
    };
    const uint64_t beast_type = fake::engine_object(u"BeastRow", w.root), staged_type = fake::engine_object(u"StagedRow", w.root), actor_type = fake::memory(0x80);
    fake::put<uint64_t>(actor_type, fake::engine().vtable);
    fake::field(beast_type, u"ActorName", u"NameProperty", 0x0, 8);
    fake::field(beast_type, u"CarryWeightMin", u"IntProperty", 0x8, 4), fake::field(beast_type, u"CarryWeightMax", u"IntProperty", 0xC, 4);
    fake::field(beast_type, u"CarryWeightMinVs", u"IntProperty", 0x10, 4), fake::field(beast_type, u"CarryWeightMaxVs", u"IntProperty", 0x14, 4);
    fake::field(beast_type, u"CarryIncPikmins", u"IntProperty", 0x18, 4), fake::field(beast_type, u"MaxLife", u"FloatProperty", 0x1C, 4);
    fake::field(beast_type, u"Kira", u"IntProperty", 0x20, 4), fake::field(beast_type, u"Poko", u"IntProperty", 0x24, 4);
    fake::field(actor_type, u"ActorName", u"NameProperty", 0, 8), fake::field(actor_type, u"Distance", u"FloatProperty", 8, 4);
    fake::field(staged_type, u"DayParams", u"ArrayProperty", kDayActorsAt, 0x10, fake::loose_field(u"StructProperty", kStagedBytes, actor_type));
    fake::field(staged_type, u"NightParams", u"ArrayProperty", kNightActorsAt, 0x10, fake::loose_field(u"StructProperty", kStagedBytes, actor_type));
    fake::field(staged_type, u"TimeType", u"IntProperty", 0x20, 4);
    zoo.parameters = table_of(u"Beasts", u"/G/Beasts.Beasts", beast_type), zoo.catalog = table_of(u"Staged", u"/G/Staged.Staged", staged_type);
    const auto row_in = [](uint64_t table, const std::u16string& name) {
        const uint64_t row = fake::memory(kRowBytes);
        sheets.rows[{table, row_name(name)}] = row;
        return row;
    };
    const auto soft = [](uint64_t at, const std::u16string& path) { fake::put<int32_t>(at + t::soft_object::index, -1), fake::put(at + t::soft_object::path, row_name(path)); };
    const auto actors = [](uint64_t at, std::initializer_list<const char16_t*> names) {
        const uint64_t data = fake::memory(size_t(kStagedBytes) * (names.size() ? names.size() : 1));
        int32_t count = 0;
        for (const char16_t* name : names) fake::put(data + uint64_t(count) * kStagedBytes, row_name(name)), fake::put(data + uint64_t(count) * kStagedBytes + 8, 10.0f * float(count + 1)), ++count;
        fake::put(at + t::array::data, data), fake::put<int32_t>(at + t::array::count, count), fake::put<int32_t>(at + t::array::max, count);
    };
    // One of the game's own creatures, with its life, its worth and its carriers; the class and the icon the
    // tables give it; and what the Piklopedia stages for it, by day and at night, among other things.
    const auto creature_of_game = [&](const std::u16string& name, const std::u16string& actor_name, float life) {
        const uint64_t row = row_in(zoo.parameters, name);
        fake::put(row, row_name(actor_name));
        const int32_t carriers[] = {2, 5, 1, 4, 4};
        for (int i = 0; i < 5; ++i) fake::put<int32_t>(row + 0x8 + 4 * uint64_t(i), carriers[i]);
        fake::put(row + 0x1C, life), fake::put<int32_t>(row + 0x20, 2), fake::put<int32_t>(row + 0x24, 3);
        const uint64_t looks = row_in(trove.classes, name);
        soft(looks, u"/G/Ko.Ko_C"), soft(looks + 0x30, u"/G/KoN.KoN_C");
        soft(row_in(trove.icons, name), u"/G/KoIcon.Icon");
        return row;
    };
    const uint64_t kochappy = creature_of_game(u"KOCHAPPY", u"GKochappy", 200.0f), kochappy_staged = row_in(zoo.catalog, u"KOCHAPPY");
    actors(kochappy_staged + kDayActorsAt, {u"KOCHAPPY", u"HIDDENBOX"}), actors(kochappy_staged + kNightActorsAt, {u"NIGHTDECOY", u"KOCHAPPY"});
    fake::put<int32_t>(kochappy_staged + 0x20, 1);
    creature_of_game(u"FIRETANK", u"GFireTank", 900.0f); // one the Piklopedia does not list
    creature_of_game(u"ODDONE", u"GOddOne", 5.0f);       // one whose row in the Piklopedia stages other things than itself
    actors(row_in(zoo.catalog, u"ODDONE") + kDayActorsAt, {u"HIDDENBOX"});
    // Classes of creatures the mods bring, each with the default object every actor of it is made from, which
    // carries the name of the creature it was made from.
    const uint64_t creature_base = fake::object(u"CreatureBase", w.root, 0x100);
    fake::set_super(creature_base, w.actor_class);
    fake::field(creature_base, u"CID", u"NameProperty", t::game_actor::kind, 8);
    std::map<std::string, uint64_t> patterns;
    const auto class_of_creature = [&](const std::string& name) {
        const std::u16string wide(name.begin(), name.end());
        const uint64_t made = fake::object(wide + u"_C", w.root, 0x100), pattern = fake::object(u"Default__" + wide, made, 0x100);
        fake::set_super(made, creature_base);
        fake::put(made + t::class_type::default_object, pattern), fake::put(pattern + t::game_actor::kind, row_name(u"KOCHAPPY"));
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
    const uint64_t hog_class = class_of_creature("Hog"), hog_night_class = class_of_creature("HogN");
    class_of_creature("Pup"), class_of_creature("Kit"), class_of_creature("Cow"), class_of_creature("Elk");
    const uint64_t hog_icon = fake::object(u"HogIcon", w.component_class);
    shelve(u"/G/HogIcon.HogIcon", hog_icon);
    // A loaded world: what keeps the creatures' rows by name, with the two lists it made of the game's tables,
    // and what keeps the icons'.
    const uint64_t keeper_class = fake::object(u"TekiParameterMgr", w.root);
    const auto new_world = [&] {
        zoo.keeper = fake::object(u"BeastKeeper", keeper_class, 0xD0), zoo.icons_keeper = fake::object(u"IconKeeper", trove.icons_keeper_class, 0x90);
        fake::put(zoo.keeper + k::table, zoo.parameters), fake::put(zoo.keeper + k::catalog_table, zoo.catalog);
        fake::put(zoo.icons_keeper + t::icon_keeper::table, trove.icons);
        // As the game builds its lists: from the rows the tables have then.
        for (const auto& [key, row] : sheets.rows) {
            if (key.first == zoo.parameters) trove_enter(reinterpret_cast<void*>(zoo.keeper + k::rows - t::data_table::rows), key.second, reinterpret_cast<void*>(row));
            if (key.first == zoo.catalog) trove_enter(reinterpret_cast<void*>(zoo.keeper + k::catalog_rows - t::data_table::rows), key.second, reinterpret_cast<void*>(row));
            if (key.first == trove.icons) trove_enter(reinterpret_cast<void*>(zoo.icons_keeper + t::icon_keeper::rows - t::data_table::rows), key.second, reinterpret_cast<void*>(row));
        }
        fake::put(w.framework + t::framework_creatures::rows, zoo.keeper), fake::put(w.framework + t::framework_treasures::icons, zoo.icons_keeper);
        zoo.entered = 0;
    };
    const auto in_list = [&](uint64_t list, const char16_t* name) { return trove_listed(list, row_name(name)); };
    const auto rows_list = [&] { return zoo.keeper + k::rows; };
    const auto catalog_list = [&] { return zoo.keeper + k::catalog_rows; };
    const auto icons_list = [&] { return zoo.icons_keeper + t::icon_keeper::rows; };
    const auto empty_lists = [&] {
        for (uint64_t list : {rows_list(), catalog_list(), icons_list()}) fake::put<int32_t>(list + t::row_list::slot_count, 0);
    };
    new_world();
    // A story being played, for asking the story about a treasure.
    const uint64_t old_story = fake::get<uint64_t>(running.instance + t::game_instance::story_save);
    const uint8_t old_rule = fake::get<uint8_t>(running.context + t::game_context::rule);
    fake::put(running.instance + t::game_instance::story_save, hoard.story), fake::put<uint8_t>(running.context + t::game_context::rule, uint8_t(t::constant::rule_story));

    const auto whole = [&](uint64_t table, const char* row, const char* path) {
        TSUCHI_Value got{};
        return sheet.get(mod, {to_handle(table)}, row, path, &got) == TSUCHI_OK && got.kind == TSUCHI_VALUE_INTEGER ? got.as.integer : -99;
    };
    const auto real = [&](uint64_t table, const char* row, const char* path) {
        TSUCHI_Value got{};
        return sheet.get(mod, {to_handle(table)}, row, path, &got) == TSUCHI_OK && got.kind == TSUCHI_VALUE_NUMBER ? got.as.number : -99.0;
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
    // The rows a table has under the names the framework gives creatures.
    const auto rows_of_mods = [&](uint64_t table) {
        int count = 0;
        for (const auto& [key, row] : sheets.rows) {
            char name[64] = "";
            Name unpacked{};
            std::memcpy(&unpacked, &key.second, sizeof unpacked);
            name_text(unpacked, name, sizeof name);
            count += key.first == table && !std::strncmp(name, "TEKITSUCHI", 10);
        }
        return count;
    };
    const tsuchi::CreatureCounts before = tsuchi::creature_counts();
    const tsuchi::ContentCounts treasures_before = tsuchi::content_counts();
    const unsigned changes = tsuchi::table_changes(), replaced = tsuchi::text_replacements();

    // ---- How it is written: judged as the manifest is read, with the creature and what is wrong ----
    const std::string hog_class_path = R"("class": "/G/Hog.Hog_C", "from": "KOCHAPPY")", named = R"("text": {"name": "Hog"})";
    const auto one = [&](const std::string& more) { return "{\"creatures\": {\"hog\": {" + hog_class_path + ", " + named + more + "}}}"; };
    const std::string numbers_are = "\"carriers\" and \"battle_carriers\" are the fewest and the most Pikmin that carry its body: two whole numbers from 1, or one for both";
    const std::string values_are = "\"values\" is an object: fields of its row by their paths, each with a number, true or false, or text";
    const std::pair<std::string, std::string> wrong[] = {
        {R"({"creatures": []})", "\"content\": \"creatures\" is an object: a short name for each creature, and what it is"},
        {"{\"creatures\": {\"Hog\": {" + hog_class_path + ", " + named + "}}}", "\"content\": creature Hog: its short name is spelled as a mod's ID is"},
        {R"({"creatures": {"hog": 7}})", "\"content\": creature hog: it is an object: its \"class\", the creature it starts \"from\", its \"text\", and what else it has of its own"},
        {one(R"(, "weight": 3)"), "\"content\": creature hog: it has a key the framework does not know: weight"},
        {R"({"creatures": {"hog": {"from": "KOCHAPPY", )" + named + "}}}", "\"content\": creature hog: \"class\" is the path of its Blueprint class, such as /Game/Mods/example_zoo/GStonehog.GStonehog_C"},
        {R"({"creatures": {"hog": {"class": "/G/Hog.Hog", "from": "KOCHAPPY", )" + named + "}}}",
         "\"content\": creature hog: \"class\" is the path of its Blueprint class, such as /Game/Mods/example_zoo/GStonehog.GStonehog_C"},
        {one(R"(, "night_class": "/G/Hog.Hog_C")"),
         "\"content\": creature hog: \"night_class\" is the path of another Blueprint class, which the Piklopedia shows at night, such as /Game/Mods/example_zoo/GStonehogNight.GStonehogNight_C"},
        {one(R"(, "night_class": "HogN_C")"),
         "\"content\": creature hog: \"night_class\" is the path of another Blueprint class, which the Piklopedia shows at night, such as /Game/Mods/example_zoo/GStonehogNight.GStonehogNight_C"},
        {R"({"creatures": {"hog": {"class": "/G/Hog.Hog_C", "from": "#3", )" + named + "}}}", "\"content\": creature hog: \"from\" is the ID of the creature its rows start as copies of, such as KOCHAPPY"},
        {"{\"creatures\": {\"hog\": {" + hog_class_path + "}}}", "\"content\": creature hog: \"text\" is an object with its \"name\", the name people see"},
        {"{\"creatures\": {\"hog\": {" + hog_class_path + R"(, "text": {"name": "Hog", "catalog_name": "x"}}}})", "\"content\": creature hog: \"text\" has a key the framework does not know: catalog_name"},
        {"{\"creatures\": {\"hog\": {" + hog_class_path + R"(, "text": {"name": "Hog", "family": 5}}}})", "\"content\": creature hog: \"text\": each of its texts is text"},
        {one(R"(, "life": 0)"), "\"content\": creature hog: \"life\" is how much life it has, a number above 0"},
        {one(R"(, "life": "much")"), "\"content\": creature hog: \"life\" is how much life it has, a number above 0"},
        {one(R"(, "sparklium": -1)"), "\"content\": creature hog: \"sparklium\" is what its body is worth, a whole number from 0"},
        {one(R"(, "pikmin": 1.5)"), "\"content\": creature hog: \"pikmin\" is how many Pikmin an Onion makes of its body, a whole number from 0"},
        {one(R"(, "carriers": [8, 4])"), "\"content\": creature hog: " + numbers_are},
        {one(R"(, "battle_carriers": 0)"), "\"content\": creature hog: " + numbers_are},
        {one(R"(, "icon": "T_Hog")"), "\"content\": creature hog: \"icon\" is the path of a texture, such as /Game/Mods/example_zoo/T_Stonehog.T_Stonehog"},
        {one(R"(, "values": {"Poko": [1]})"), "\"content\": creature hog: " + values_are},
        {one(R"(, "night": 5)"), "\"content\": creature hog: \"night\": it is an object: what the creature has other at night than by day"},
        {one(R"(, "night": {"class": "/G/HogN.HogN_C"})"), "\"content\": creature hog: \"night\": it has a key the framework does not know: class"},
        {one(R"(, "night": {"life": -3})"), "\"content\": creature hog: \"night\": \"life\" is how much life it has, a number above 0"},
        {one(R"(, "night": {"carriers": [1, 2, 3]})"), "\"content\": creature hog: \"night\": " + numbers_are},
        {one(R"(, "night": {"values": 4})"), "\"content\": creature hog: \"night\": " + values_are},
        {one(R"(, "catalog": true)"), "\"content\": creature hog: \"catalog\" is an object, which may be empty: that the Piklopedia lists it, and \"values\" of its row there"},
        {one(R"(, "catalog": {"series": "set"})"), "\"content\": creature hog: \"catalog\" has a key the framework does not know: series"},
        {one(R"(, "catalog": {"values": {"TimeType": null}})"), "\"content\": creature hog: \"catalog\": " + values_are},
        {R"({"treasures": {"hog": {"class": "/G/Coin.Coin_C", "from": "OTAECLAIR", "text": {"name": "Hog"}}}, "creatures": {"hog": {)" + hog_class_path + ", " + named + "}}}",
         "\"content\": creature hog: a treasure of the manifest has that short name, and the two would have one ID"},
    };
    int number = 0;
    for (const auto& each : wrong) plain("odd" + std::to_string(number++), each.first);
    plain("odd.empty", R"({"creatures": {}})");
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    number = 0;
    for (const auto& each : wrong) CHECK(told(("odd" + std::to_string(number++)).c_str(), "mod.json: " + each.second));
    // A mod that declares none under it starts, and nothing is asked of the save for it.
    tsuchi::start_mods();
    CHECK(told("odd.empty", "<running>") && !disk.files.count(record_at) && disk.changes.empty());
    tsuchi::forget_mods();
    for (int i = 0; i < number; ++i) gone("odd" + std::to_string(i));
    gone("odd.empty");

    // ---- A mod with three creatures, and a mod that claims the first one's night class ----
    const std::string ark = R"({"creatures": {
        "hog": {"class": "/G/Hog.Hog_C", "night_class": "/G/HogN.HogN_C", "from": "KOCHAPPY", "life": 450.5, "sparklium": 9, "carriers": [3, 7], "pikmin": 6,
                "icon": "/G/HogIcon.HogIcon", "text": {"name": "Hog", "scientific_name": "Sus saxum", "family": "Boar", "description": "Hard to move."},
                "values": {"Poko": 4}, "night": {"life": 600, "carriers": 5, "values": {"Poko": 8}}, "catalog": {"values": {"TimeType": 2}}},
        "pup": {"class": "/G/Pup.Pup_C", "from": "ark:hog", "life": 20, "battle_carriers": [1, 2], "text": {"name": "Pup"}},
        "kit": {"class": "/G/Kit.Kit_C", "from": "ark:hog", "text": {"name": "Kit"}, "catalog": {}}}})";
    plain("ark", ark);
    plain("rival", R"({"creatures": {"cub": {"class": "/G/HogN.HogN_C", "from": "KOCHAPPY", "text": {"name": "Cub"}}}})");
    CHECK(tsuchi::find_mods() == TSUCHI_OK && told("ark", "<ready>") && told("rival", "<ready>"));
    logged.lines.clear(), disk.changes.clear();
    tsuchi::start_mods();
    CHECK(told("ark", "<running>") && closed() && disk.well_formed);
    // The numbers were given in the order the manifest has its creatures, in a record of the creatures' own, and
    // are in the save before anything else is done. Nothing was asked of the treasures' record.
    CHECK(saved_record(record_at) == "{\"creatures\": {\"ark:hog\": 1, \"ark:pup\": 2, \"ark:kit\": 3, \"rival:cub\": 4}}\n" && !disk.files.count(treasures_at));
    // Its rows: a copy of the creature it starts from in each table, under the name the framework gave it, with
    // what the manifest says in place of what that creature has. The creature it starts from is as it was.
    CHECK(has_row(zoo.parameters, u"TEKITSUCHI0001") && has_row(trove.classes, u"TEKITSUCHI0001") && has_row(trove.icons, u"TEKITSUCHI0001") && has_row(zoo.catalog, u"TEKITSUCHI0001"));
    CHECK(text_in(zoo.parameters, "TEKITSUCHI0001", "ActorName") == "Hog" && real(zoo.parameters, "TEKITSUCHI0001", "MaxLife") == 450.5 && whole(zoo.parameters, "TEKITSUCHI0001", "Kira") == 9 &&
          whole(zoo.parameters, "TEKITSUCHI0001", "Poko") == 4 && whole(zoo.parameters, "TEKITSUCHI0001", "CarryWeightMin") == 3 && whole(zoo.parameters, "TEKITSUCHI0001", "CarryWeightMax") == 7 &&
          whole(zoo.parameters, "TEKITSUCHI0001", "CarryWeightMinVs") == 3 && whole(zoo.parameters, "TEKITSUCHI0001", "CarryWeightMaxVs") == 7 && whole(zoo.parameters, "TEKITSUCHI0001", "CarryIncPikmins") == 6);
    CHECK(text_in(zoo.parameters, "KOCHAPPY", "ActorName") == "GKochappy" && fake::get<float>(kochappy + 0x1C) == 200.0f && whole(zoo.parameters, "KOCHAPPY", "Kira") == 2 &&
          whole(zoo.parameters, "KOCHAPPY", "CarryWeightMin") == 2 && whole(zoo.parameters, "KOCHAPPY", "CarryIncPikmins") == 4);
    // At night the game looks for its row under its name and `_NIGHT`: what it has by day, except where "night"
    // says otherwise.
    CHECK(text_in(zoo.parameters, "TEKITSUCHI0001_NIGHT", "ActorName") == "Hog" && real(zoo.parameters, "TEKITSUCHI0001_NIGHT", "MaxLife") == 600.0 && whole(zoo.parameters, "TEKITSUCHI0001_NIGHT", "Kira") == 9 &&
          whole(zoo.parameters, "TEKITSUCHI0001_NIGHT", "Poko") == 8 && whole(zoo.parameters, "TEKITSUCHI0001_NIGHT", "CarryWeightMin") == 5 && whole(zoo.parameters, "TEKITSUCHI0001_NIGHT", "CarryWeightMax") == 5 &&
          whole(zoo.parameters, "TEKITSUCHI0001_NIGHT", "CarryWeightMinVs") == 5 && whole(zoo.parameters, "TEKITSUCHI0001_NIGHT", "CarryWeightMaxVs") == 5 &&
          whole(zoo.parameters, "TEKITSUCHI0001_NIGHT", "CarryIncPikmins") == 6);
    // The Piklopedia is given its class by day and its other class at night, and its icon.
    CHECK(text_in(trove.classes, "TEKITSUCHI0001", "DayCookBlueprint") == "/G/Hog.Hog_C" && text_in(trove.classes, "TEKITSUCHI0001", "NightCookBlueprint") == "/G/HogN.HogN_C" &&
          text_in(trove.icons, "TEKITSUCHI0001", "Texture2D") == "/G/HogIcon.HogIcon" && text_in(trove.classes, "KOCHAPPY", "DayCookBlueprint") == "/G/Ko.Ko_C" &&
          text_in(trove.classes, "KOCHAPPY", "NightCookBlueprint") == "/G/KoN.KoN_C" && text_in(trove.icons, "KOCHAPPY", "Texture2D") == "/G/KoIcon.Icon");
    // On the Piklopedia's stage, the actors that were the creature it starts from are the new creature, by day and
    // at night, and the others are as they were.
    CHECK(text_in(zoo.catalog, "TEKITSUCHI0001", "DayParams[0].ActorName") == "TEKITSUCHI0001" && text_in(zoo.catalog, "TEKITSUCHI0001", "DayParams[1].ActorName") == "HIDDENBOX" &&
          text_in(zoo.catalog, "TEKITSUCHI0001", "NightParams[0].ActorName") == "NIGHTDECOY" && text_in(zoo.catalog, "TEKITSUCHI0001", "NightParams[1].ActorName") == "TEKITSUCHI0001" &&
          whole(zoo.catalog, "TEKITSUCHI0001", "TimeType") == 2);
    CHECK(text_in(zoo.catalog, "KOCHAPPY", "DayParams[0].ActorName") == "KOCHAPPY" && text_in(zoo.catalog, "KOCHAPPY", "NightParams[1].ActorName") == "KOCHAPPY" && whole(zoo.catalog, "KOCHAPPY", "TimeType") == 1);
    // The second starts from the first, a mod's own, has one class and no row for the night, and is in no Piklopedia.
    CHECK(text_in(zoo.parameters, "TEKITSUCHI0002", "ActorName") == "Pup" && real(zoo.parameters, "TEKITSUCHI0002", "MaxLife") == 20.0 && whole(zoo.parameters, "TEKITSUCHI0002", "Kira") == 9 &&
          whole(zoo.parameters, "TEKITSUCHI0002", "CarryWeightMin") == 3 && whole(zoo.parameters, "TEKITSUCHI0002", "CarryWeightMax") == 7 && whole(zoo.parameters, "TEKITSUCHI0002", "CarryWeightMinVs") == 1 &&
          whole(zoo.parameters, "TEKITSUCHI0002", "CarryWeightMaxVs") == 2 && text_in(trove.classes, "TEKITSUCHI0002", "DayCookBlueprint") == "/G/Pup.Pup_C" &&
          text_in(trove.classes, "TEKITSUCHI0002", "NightCookBlueprint") == "/G/Pup.Pup_C" && text_in(trove.icons, "TEKITSUCHI0002", "Texture2D") == "/G/HogIcon.HogIcon" &&
          !has_row(zoo.parameters, u"TEKITSUCHI0002_NIGHT") && !has_row(zoo.catalog, u"TEKITSUCHI0002"));
    // The third starts from the first too, and the Piklopedia stages it where it staged the first.
    CHECK(text_in(zoo.catalog, "TEKITSUCHI0003", "DayParams[0].ActorName") == "TEKITSUCHI0003" && text_in(zoo.catalog, "TEKITSUCHI0003", "DayParams[1].ActorName") == "HIDDENBOX" &&
          text_in(zoo.catalog, "TEKITSUCHI0003", "NightParams[1].ActorName") == "TEKITSUCHI0003" && whole(zoo.catalog, "TEKITSUCHI0003", "TimeType") == 2 &&
          text_in(zoo.catalog, "TEKITSUCHI0001", "DayParams[0].ActorName") == "TEKITSUCHI0001");
    // Its texts, under its name: the scientific name everywhere the game looks for one, and what the manifest
    // does not give empty.
    CHECK(word("GenseiName", "TEKITSUCHI0001") == "Hog" && word("GenseiScientificName", "TEKITSUCHI0001") == "Sus saxum" && word("GenseiScientificName_Picturebook", "TEKITSUCHI0001") == "Sus saxum" &&
          word("GenseiScientificName_Ellipsis", "TEKITSUCHI0001") == "Sus saxum" && word("GenseiFamilyName", "TEKITSUCHI0001") == "Boar" && word("GenseiDesc", "TEKITSUCHI0001") == "Hard to move." &&
          word("GenseiDescOlimar", "TEKITSUCHI0001") == "" && word("GenseiDescLouie", "TEKITSUCHI0001") == "" && word("GenseiName", "TEKITSUCHI0002") == "Pup" &&
          word("GenseiFamilyName", "TEKITSUCHI0002") == "");
    // Every actor of either of its classes carries the name, since each class's default object does.
    CHECK(carried("Hog") == "TEKITSUCHI0001" && carried("HogN") == "TEKITSUCHI0001" && carried("Pup") == "TEKITSUCHI0002" && carried("Kit") == "TEKITSUCHI0003" && object_kept(hog_class) &&
          object_kept(hog_night_class) && object_kept(hog_icon) && object_kept(zoo.parameters) && object_kept(zoo.catalog));
    // The loaded world's lists have its rows: by day and at night, the Piklopedia's, and the icon's.
    CHECK(in_list(rows_list(), u"TEKITSUCHI0001") == row_at(zoo.parameters, u"TEKITSUCHI0001") && in_list(rows_list(), u"TEKITSUCHI0001_NIGHT") && in_list(rows_list(), u"TEKITSUCHI0002") &&
          in_list(catalog_list(), u"TEKITSUCHI0001") == row_at(zoo.catalog, u"TEKITSUCHI0001") && !in_list(catalog_list(), u"TEKITSUCHI0002") && in_list(catalog_list(), u"TEKITSUCHI0003") &&
          in_list(icons_list(), u"TEKITSUCHI0001") && in_list(icons_list(), u"TEKITSUCHI0002") && zoo.entered == 9);
    // An actor of the kind is told as the ID, and any other as the game names it. The ID is no treasure's.
    {
        const Name own_kind = fake::get<Name>(w.captain + t::game_actor::kind);
        char kind[TSUCHI_ID_SIZE] = "";
        uint64_t bytes = 0;
        uint32_t count = 0;
        Name found{};
        fake::put(w.captain + t::game_actor::kind, row_name(u"TEKITSUCHI0001"));
        CHECK(api.kind(mod, handle(w.captain), kind, sizeof kind, &bytes) == TSUCHI_OK && !std::strcmp(kind, "ark:hog") && bytes == 8 && api.kind(mod, handle(w.captain), kind, 7, &bytes) == TSUCHI_CAPACITY);
        fake::put(w.captain + t::game_actor::kind, row_name(u"KOCHAPPY"));
        CHECK(api.kind(mod, handle(w.captain), kind, sizeof kind, &bytes) == TSUCHI_OK && !std::strcmp(kind, "KOCHAPPY"));
        fake::put(w.captain + t::game_actor::kind, own_kind);
        CHECK(game::added_name("ark:hog", found) && game::added_id(found, kind) && !std::strcmp(kind, "ark:hog") && game::added_name("ark:kit", found) && !game::added_name("rival:cub", found));
        CHECK(tale.treasure_count(mod, "ark:hog", &count) == TSUCHI_NOT_FOUND && tale.give_treasure(mod, "ark:hog", true) == TSUCHI_NOT_FOUND);
    }
    // A class carries one name, whether it is a creature's class or the one the Piklopedia shows at night. The mod
    // that asked second is blocked, with whose the class is, and has nothing.
    CHECK(told("rival", "mod.json: \"content\": creature cub: its class, /G/HogN.HogN_C, is already the creature ark:hog's") && !tsuchi::find_mod("rival")->context &&
          !has_row(zoo.parameters, u"TEKITSUCHI0004") && carried("HogN") == "TEKITSUCHI0001");
    tsuchi::CreatureCounts now = tsuchi::creature_counts();
    CHECK(now.creatures == 3 && now.declared == before.declared + 3 && now.rows_added == before.rows_added + 12 && now.entered == before.entered + 9 && now.refused == before.refused + 1 &&
          now.numbers_given == before.numbers_given + 4 && now.rows_removed == before.rows_removed && now.rows_left == 0 && now.records_unread == before.records_unread &&
          now.records_unwritten == before.records_unwritten);
    // Nothing of it is counted among the treasures.
    {
        const tsuchi::ContentCounts treasures_now = tsuchi::content_counts();
        CHECK(treasures_now.treasures == 0 && treasures_now.declared == treasures_before.declared && treasures_now.rows_added == treasures_before.rows_added && treasures_now.entered == treasures_before.entered &&
              treasures_now.refused == treasures_before.refused && treasures_now.numbers_given == treasures_before.numbers_given);
    }

    // ---- Another world: its lists are its own, made from the tables or not, and get what they lack ----
    new_world(); // the game built these from the tables, which have the rows
    tsuchi::frame(0.03f);
    CHECK(zoo.entered == 0);
    new_world();
    empty_lists(); // a world whose lists were made before the rows
    tsuchi::frame(0.03f);
    CHECK(zoo.entered == 9 && in_list(rows_list(), u"TEKITSUCHI0001_NIGHT") && in_list(catalog_list(), u"TEKITSUCHI0003") && in_list(icons_list(), u"TEKITSUCHI0002"));
    tsuchi::frame(0.03f);
    CHECK(zoo.entered == 9);
    // No world, and then one that keeps another table than the Piklopedia's rows are in: nothing is entered there.
    fake::put<uint64_t>(w.framework + t::framework_creatures::rows, 0), fake::put<uint64_t>(w.framework + t::framework_treasures::icons, 0);
    tsuchi::frame(0.03f);
    new_world();
    empty_lists();
    fake::put(zoo.keeper + k::catalog_table, trove.classes);
    const uint32_t passed = tsuchi::creature_counts().not_entered;
    tsuchi::frame(0.03f);
    CHECK(zoo.entered == 7 && !in_list(catalog_list(), u"TEKITSUCHI0001") && tsuchi::creature_counts().not_entered == passed + 2);
    new_world();

    // ---- The mod stops, in a world whose lists have its rows ----
    tsuchi::forget_mods();
    now = tsuchi::creature_counts();
    // The ID names nothing, the fields of the rows and the holds on what was loaded are as before the mod, and
    // the texts the game had no key for keep what they were last given.
    Name none{};
    CHECK(now.creatures == 0 && !game::added_name("ark:hog", none) && tsuchi::table_changes() == changes && tsuchi::text_replacements() == replaced && !object_kept(hog_class) &&
          !object_kept(hog_night_class) && !object_kept(hog_icon) && !object_kept(zoo.parameters));
    // The rows stay, as copies of the creature they started from, and each class's default object keeps the name.
    CHECK(has_row(zoo.parameters, u"TEKITSUCHI0001") && has_row(zoo.parameters, u"TEKITSUCHI0001_NIGHT") && has_row(trove.classes, u"TEKITSUCHI0001") && has_row(trove.icons, u"TEKITSUCHI0001") &&
          whole(zoo.parameters, "TEKITSUCHI0001", "Kira") == 2 && real(zoo.parameters, "TEKITSUCHI0001", "MaxLife") == 200.0 && real(zoo.parameters, "TEKITSUCHI0001_NIGHT", "MaxLife") == 200.0 &&
          text_in(zoo.parameters, "TEKITSUCHI0001", "ActorName") == "GKochappy" && text_in(trove.classes, "TEKITSUCHI0001", "NightCookBlueprint") == "/G/KoN.KoN_C" && carried("Hog") == "TEKITSUCHI0001" &&
          carried("HogN") == "TEKITSUCHI0001" && word("GenseiName", "TEKITSUCHI0001") == "Hog");
    // A row of the Piklopedia's table would go on counting there. The world's list points at it, so it waits, as it
    // was before the mod: staging the creature it started from.
    CHECK(has_row(zoo.catalog, u"TEKITSUCHI0001") && has_row(zoo.catalog, u"TEKITSUCHI0003") && text_in(zoo.catalog, "TEKITSUCHI0001", "DayParams[0].ActorName") == "KOCHAPPY" &&
          whole(zoo.catalog, "TEKITSUCHI0001", "TimeType") == 1 && now.rows_left == 2 && now.rows_removed == before.rows_removed && zoo.removed == 0);
    tsuchi::frame(0.03f);
    CHECK(tsuchi::creature_counts().rows_left == 2 && zoo.removed == 0);
    // Something there that cannot be told for the keeper of the creatures' rows: the rows wait.
    fake::put(w.framework + t::framework_creatures::rows, w.captain);
    tsuchi::frame(0.03f);
    CHECK(tsuchi::creature_counts().rows_left == 2 && zoo.removed == 0);
    // The world is gone, and nothing points at the rows: they are taken out of the table. The other tables keep theirs.
    fake::put<uint64_t>(w.framework + t::framework_creatures::rows, 0);
    tsuchi::frame(0.03f);
    now = tsuchi::creature_counts();
    CHECK(zoo.removed == 2 && !has_row(zoo.catalog, u"TEKITSUCHI0001") && !has_row(zoo.catalog, u"TEKITSUCHI0003") && has_row(zoo.catalog, u"KOCHAPPY") && now.rows_left == 0 &&
          now.rows_removed == before.rows_removed + 2 && has_row(zoo.parameters, u"TEKITSUCHI0001") && has_row(trove.icons, u"TEKITSUCHI0003"));
    tsuchi::frame(0.03f);
    CHECK(zoo.removed == 2);
    for(uint64_t row : zoo.freed) {
        const auto* bytes=reinterpret_cast<const unsigned char*>(row);
        CHECK(std::all_of(bytes,bytes+kRowBytes,[](unsigned char value){ return value==0xdd; }));
    }
    zoo.freed.clear();

    // ---- It starts again in the same run: the same numbers from the save, the rows it left, and the Piklopedia's anew ----
    gone("rival");
    new_world();
    const std::string kept = saved_record(record_at);
    disk.changes.clear();
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    const tsuchi::CreatureCounts again = tsuchi::creature_counts();
    CHECK(told("ark", "<running>") && saved_record(record_at) == kept && disk.changes.empty() && again.numbers_given == now.numbers_given && again.rows_added == now.rows_added + 2 &&
          again.creatures == 3 && zoo.entered == 2);
    CHECK(whole(zoo.parameters, "TEKITSUCHI0001", "Kira") == 9 && real(zoo.parameters, "TEKITSUCHI0001_NIGHT", "MaxLife") == 600.0 && carried("Pup") == "TEKITSUCHI0002" &&
          text_in(zoo.catalog, "TEKITSUCHI0003", "NightParams[1].ActorName") == "TEKITSUCHI0003" && in_list(catalog_list(), u"TEKITSUCHI0001"));
    // It stops with no world loaded: its rows in the Piklopedia's table go at once.
    fake::put<uint64_t>(w.framework + t::framework_creatures::rows, 0), fake::put<uint64_t>(w.framework + t::framework_treasures::icons, 0);
    zoo.removed = 0;
    tsuchi::forget_mods();
    CHECK(zoo.removed == 2 && !rows_of_mods(zoo.catalog) && tsuchi::creature_counts().rows_left == 0 && tsuchi::creature_counts().rows_removed == before.rows_removed + 4);

    // ---- The mod is absent for a run, and another brings a creature: it gets a number nobody had ----
    const auto new_run = [&] { // what a run of the game starts with: the tables and the classes as the game ships them, and the save
        for (auto row = sheets.rows.begin(); row != sheets.rows.end();) {
            char name[64] = "";
            Name unpacked{};
            std::memcpy(&unpacked, &row->first.second, sizeof unpacked);
            name_text(unpacked, name, sizeof name);
            row = !std::strncmp(name, "TEKITSUCHI", 10) ? sheets.rows.erase(row) : std::next(row);
        }
        for (const auto& [name, pattern] : patterns) fake::put(pattern + t::game_actor::kind, row_name(u"KOCHAPPY"));
        new_world();
    };
    new_run();
    gone("ark");
    plain("farm", R"({"creatures": {"cow": {"class": "/G/Cow.Cow_C", "from": "KOCHAPPY", "text": {"name": "Cow"}, "night": {"life": 5}, "catalog": {}}}})");
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(told("farm", "<running>") && saved_record(record_at) == "{\"creatures\": {\"ark:hog\": 1, \"ark:pup\": 2, \"ark:kit\": 3, \"rival:cub\": 4, \"farm:cow\": 5}}\n");
    CHECK(has_row(zoo.parameters, u"TEKITSUCHI0005") && has_row(zoo.parameters, u"TEKITSUCHI0005_NIGHT") && !has_row(zoo.parameters, u"TEKITSUCHI0001") && carried("Cow") == "TEKITSUCHI0005" &&
          real(zoo.parameters, "TEKITSUCHI0005", "MaxLife") == 200.0 && real(zoo.parameters, "TEKITSUCHI0005_NIGHT", "MaxLife") == 5.0 && whole(zoo.parameters, "TEKITSUCHI0005_NIGHT", "Kira") == 2 &&
          text_in(trove.classes, "TEKITSUCHI0005", "NightCookBlueprint") == "/G/Cow.Cow_C" && text_in(zoo.catalog, "TEKITSUCHI0005", "DayParams[0].ActorName") == "TEKITSUCHI0005" &&
          !game::added_name("ark:hog", none) && carried("Hog") == "KOCHAPPY");
    tsuchi::forget_mods();
    // ---- It is back in the run after: its creatures have the names they had ----
    new_run();
    plain("ark", ark);
    const std::string both = saved_record(record_at);
    disk.changes.clear();
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(told("ark", "<running>") && told("farm", "<running>") && saved_record(record_at) == both && disk.changes.empty());
    CHECK(carried("Hog") == "TEKITSUCHI0001" && carried("Kit") == "TEKITSUCHI0003" && carried("Cow") == "TEKITSUCHI0005" && tsuchi::creature_counts().creatures == 4 &&
          real(zoo.parameters, "TEKITSUCHI0001", "MaxLife") == 450.5 && has_row(zoo.catalog, u"TEKITSUCHI0005") && has_row(zoo.catalog, u"TEKITSUCHI0003"));
    tsuchi::forget_mods();
    gone("ark"), gone("farm");

    // ---- What the game will not take blocks the mod as it starts, with the creature and the reason ----
    new_run();
    const auto creature = [](const std::string& of_class, const std::string& from, const std::string& more = "") {
        return "{\"creatures\": {\"one\": {\"class\": \"" + of_class + "\", \"from\": \"" + from + "\", \"text\": {\"name\": \"One\"}" + more + "}}}";
    };
    plain("no.class", creature("/G/Nope.Nope_C", "KOCHAPPY"));
    plain("no.cid", creature("/G/Plain.Plain_C", "KOCHAPPY"));
    plain("no.night", creature("/G/Elk.Elk_C", "KOCHAPPY", R"(, "night_class": "/G/Nope.Nope_C")"));
    plain("no.from", creature("/G/Elk.Elk_C", "NOPE"));
    plain("no.mod", creature("/G/Elk.Elk_C", "ark:hog"));
    plain("no.shown", creature("/G/Elk.Elk_C", "FIRETANK", R"(, "catalog": {})"));
    plain("no.stage", creature("/G/Elk.Elk_C", "ODDONE", R"(, "catalog": {})"));
    plain("no.field", creature("/G/Elk.Elk_C", "KOCHAPPY", R"(, "values": {"Weight": 1})"));
    plain("no.nightly", creature("/G/Elk.Elk_C", "KOCHAPPY", R"(, "night": {"values": {"Weight": 1}})"));
    plain("no.page", creature("/G/Elk.Elk_C", "KOCHAPPY", R"(, "catalog": {"values": {"Page": 1}})"));
    plain("no.icon", creature("/G/Elk.Elk_C", "KOCHAPPY", R"(, "icon": "/G/Nope.Nope")"));
    plain("no.twice", R"({"creatures": {"one": {"class": "/G/Elk.Elk_C", "from": "KOCHAPPY", "text": {"name": "One"}, "catalog": {}},
                                        "two": {"class": "/G/Elk.Elk_C", "from": "KOCHAPPY", "text": {"name": "Two"}}}})");
    plain("no.mix", R"({"treasures": {"gem": {"class": "/G/Coin.Coin_C", "from": "OTAECLAIR", "text": {"name": "Gem"}}},
                        "creatures": {"imp": {"class": "/G/Coin.Coin_C", "from": "KOCHAPPY", "text": {"name": "Imp"}}}})");
    const tsuchi::CreatureCounts calm = tsuchi::creature_counts();
    zoo.removed = 0;
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(told("no.class", "mod.json: \"content\": creature one: its class, /G/Nope.Nope_C, could not be loaded: the game finds no such asset"));
    CHECK(told("no.cid", "mod.json: \"content\": creature one: its class, /G/Plain.Plain_C, is no class of a creature: its default object has no CID"));
    CHECK(told("no.night", "mod.json: \"content\": creature one: its night class, /G/Nope.Nope_C, could not be loaded: the game finds no such asset"));
    CHECK(told("no.from", "mod.json: \"content\": creature one: the creature it starts from, NOPE, is no creature the game has"));
    CHECK(told("no.mod", "mod.json: \"content\": creature one: the creature it starts from, ark:hog, is no creature of a mod that is running"));
    CHECK(told("no.shown", "mod.json: \"content\": creature one: the creature it starts from, FIRETANK, has no row in the Piklopedia's table of creatures"));
    CHECK(told("no.stage", "mod.json: \"content\": creature one: the creature it starts from, ODDONE, is not among the actors its own row in the Piklopedia's table of creatures puts on the stage"));
    CHECK(told("no.field", "mod.json: \"content\": creature one: field Weight of its row in the creatures' table: the game has none of that name"));
    CHECK(told("no.nightly", "mod.json: \"content\": creature one: field Weight of its row in the creatures' table: the game has none of that name"));
    CHECK(told("no.page", "mod.json: \"content\": creature one: field Page of its row in the Piklopedia's table of creatures: the game has none of that name"));
    CHECK(told("no.icon", "mod.json: \"content\": creature one: its icon, /G/Nope.Nope, could not be loaded: the game finds no such asset"));
    CHECK(told("no.twice", "mod.json: \"content\": creature two: its class, /G/Elk.Elk_C, is already the creature no.twice:one's"));
    CHECK(told("no.mix", "mod.json: \"content\": creature imp: its class, /G/Coin.Coin_C, is already the treasure no.mix:gem's"));
    // Nothing of a mod that was blocked is left in force: no creature, no treasure, no change to a row, no text, no
    // hold. And no row of the Piklopedia's table: the three that had been added were taken out again, since the
    // world's list did not have them yet.
    now = tsuchi::creature_counts();
    CHECK(now.creatures == 0 && tsuchi::content_counts().treasures == 0 && now.refused == calm.refused + 13 && tsuchi::table_changes() == changes && tsuchi::text_replacements() == replaced &&
          !object_kept(zoo.parameters) && !game::added_name("no.twice:one", none) && !game::added_name("no.mix:gem", none));
    CHECK(zoo.removed == 3 && !rows_of_mods(zoo.catalog) && now.rows_removed == calm.rows_removed + 3 && now.rows_left == 0 && rows_of_mods(zoo.parameters) > 0);
    tsuchi::forget_mods();
    for (const char* id : {"no.class", "no.cid", "no.night", "no.from", "no.mod", "no.shown", "no.stage", "no.field", "no.nightly", "no.page", "no.icon", "no.twice", "no.mix"}) gone(id);

    // ---- The numbers cannot be read or kept: the mod is blocked, and nothing is made with a number that may not last ----
    plain("late", R"({"creatures": {"elk": {"class": "/G/Elk.Elk_C", "from": "KOCHAPPY", "text": {"name": "Elk"}}}})");
    const std::string sound = saved_record(record_at);
    const auto starts = [&] {
        tsuchi::forget_records();
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        tsuchi::start_mods();
        const std::string said = problem("late");
        tsuchi::forget_mods();
        return said;
    };
    const uint32_t rows_now = tsuchi::creature_counts().rows_added, unread = tsuchi::creature_counts().records_unread, unwritten = tsuchi::creature_counts().records_unwritten;
    const std::string elk_before = carried("Elk");
    for (const char* damaged : {"[]", "{ nope", R"({"creatures": []})", R"({"creatures": {"ark:hog": 0}})", R"({"creatures": {"ark:hog": 1, "ark:pup": 1}})", R"({"creatures": {"KOCHAPPY": 1}})",
                                R"({"creatures": {"ark:hog": 1.5}})"}) {
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
    now = tsuchi::creature_counts();
    CHECK(now.rows_added == rows_now && now.records_unread == unread + 8 && now.records_unwritten == unwritten + 1 && carried("Elk") == elk_before);
    // With the save as it should be, the mod starts, and its creature has the number after the highest ever given.
    disk.files[record_at] = sound;
    restart_records(record_at);
    const std::string highest = sound.substr(sound.rfind(": ") + 2, sound.rfind('}') - 1 - (sound.rfind(": ") + 2));
    const std::string next = std::to_string(std::stoi(highest) + 1), next_name = std::string("TEKITSUCHI") + std::string(4 - std::min<size_t>(4, next.size()), '0') + next;
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(told("late", "<running>") && saved_record(record_at).find("\"late:elk\": " + next + "}") != std::string::npos && carried("Elk") == next_name);
    tsuchi::forget_mods();
    gone("late");

    // ---- A mod with a treasure and a creature: each has its own record and its own name, in the tables they share ----
    plain("both", R"({"treasures": {"gem": {"class": "/G/Coin.Coin_C", "from": "OTAECLAIR", "text": {"name": "Gem"}}},
                      "creatures": {"imp": {"class": "/G/Elk.Elk_C", "from": "KOCHAPPY", "text": {"name": "Imp"}}}})");
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    {
        Name gem{}, imp{};
        char gem_name[64] = "", imp_name[64] = "";
        CHECK(told("both", "<running>") && game::added_name("both:gem", gem) && game::added_name("both:imp", imp));
        name_text(gem, gem_name, sizeof gem_name), name_text(imp, imp_name, sizeof imp_name);
        CHECK(!std::strncmp(gem_name, "OTATSUCHI", 9) && !std::strncmp(imp_name, "TEKITSUCHI", 10) && saved_record(treasures_at).find("\"both:gem\"") != std::string::npos &&
              saved_record(record_at).find("\"both:imp\"") != std::string::npos && saved_record(record_at).find("gem") == std::string::npos);
        CHECK(text_in(trove.classes, gem_name, "DayCookBlueprint") == "/G/Coin.Coin_C" && text_in(trove.classes, imp_name, "DayCookBlueprint") == "/G/Elk.Elk_C" &&
              tsuchi::content_counts().treasures == 1 && tsuchi::creature_counts().creatures == 1);
    }
    tsuchi::forget_mods();
    gone("both");

    // As it was, for what comes after.
    t::fn::add_table_row = old_add, t::fn::add_table_row_internal = old_enter, t::fn::remove_table_row = old_remove;
    fake::put(trove.functions + f::add_row, old_slots[0]), fake::put(trove.functions + f::remove_row, old_slots[1]), fake::put(trove.functions + f::add_row_internal, old_slots[2]);
    fake::put<uint64_t>(w.framework + t::framework_creatures::rows, 0), fake::put<uint64_t>(w.framework + t::framework_treasures::icons, 0);
    fake::put(running.instance + t::game_instance::story_save, old_story), fake::put<uint8_t>(running.context + t::game_context::rule, old_rule);
    disk.files = other_files, disk.changes.clear();
    logged.lines.clear(), hoard.log.clear();
    tsuchi::set_log_sink(nullptr);
}
