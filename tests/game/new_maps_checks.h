// Checks of the manifest's "new_maps" against the made-up game: maps of mods' own, each given one of the game's
// free areas. Part of objects_test.cpp, which includes this inside its own namespace, after everything it uses.

#include "table_length_checks.h"

struct Atlas {
    std::string log;                      // what the game's menu and its story were asked, in order
    std::map<uint64_t, uint64_t> records; // the story's records of areas, by the name of an area's level
    uint64_t flags = 0, rows = 0;         // the story's flags, and memory the menu's rows stand in
    int grown = 0;
    uint64_t hidden_table = 0, hidden_row = 0, refused_table = 0, refused_row = 0, tracked_table = 0, tracked_row = 0;
    int hide_at = 0, found = 0, refused = 0, tracked = 0;
    decltype(t::fn::find_table_row) find_before = nullptr;
    decltype(t::fn::add_table_row) add_before = nullptr;
} atlas;
void* retry_find_row(void* table, uint64_t row) {
    const uint64_t address = reinterpret_cast<uint64_t>(table);
    if (address == atlas.tracked_table && row == atlas.tracked_row) ++atlas.tracked;
    if (address == atlas.hidden_table && row == atlas.hidden_row && ++atlas.found == atlas.hide_at) return nullptr;
    return atlas.find_before(table, row);
}
void retry_add_row(void* table, uint64_t row, void* source) {
    if (reinterpret_cast<uint64_t>(table) == atlas.refused_table && row == atlas.refused_row) { ++sheets.added; ++atlas.refused; return; }
    atlas.add_before(table, row, source);
}
std::string text_of_name(uint64_t packed) {
    Name unpacked;
    char text[64] = "?";
    std::memcpy(&unpacked, &packed, sizeof unpacked);
    name_text(unpacked, text, sizeof text);
    return text;
}
std::u16string wide(const std::string& text) { return std::u16string(text.begin(), text.end()); }
// The game's own way to make its list of scenes longer: the count already includes the new one.
void game_scene_grow(void* array) {
    const auto at = reinterpret_cast<uint64_t>(array);
    const int32_t count = fake::get<int32_t>(at + t::array::count), max = fake::get<int32_t>(at + t::array::max);
    const uint64_t bigger = fake::memory(8 * size_t(count + 2));
    std::memcpy(reinterpret_cast<void*>(bigger), reinterpret_cast<const void*>(fake::get<uint64_t>(at + t::array::data)), 8 * size_t(max));
    fake::put(at + t::array::data, bigger), fake::put<int32_t>(at + t::array::max, count + 2);
    ++atlas.grown;
}
uint64_t game_story_flags(void*) { return atlas.flags; }
void* game_story_find_area(void*, void* name) {
    const auto found = atlas.records.find(fake::get<uint64_t>(reinterpret_cast<uint64_t>(name)));
    return reinterpret_cast<void*>(found == atlas.records.end() ? 0 : found->second);
}
int32_t story_bases_given = -1;
void game_story_add_area(void*, void* name, int32_t bases) {
    story_bases_given = bases;
    atlas.records[fake::get<uint64_t>(reinterpret_cast<uint64_t>(name))] = fake::memory(0x10);
    atlas.log += "add:" + text_of_name(fake::get<uint64_t>(reinterpret_cast<uint64_t>(name))) + " ";
}
bool game_story_link_area(void*, void* name, uint64_t zero) {
    atlas.log += "link:" + text_of_name(fake::get<uint64_t>(reinterpret_cast<uint64_t>(name))) + (zero ? "? " : " ");
    return true;
}
void* game_explore_row(void*, int32_t index) { return reinterpret_cast<void*>(atlas.rows + uint64_t(index)); }
void game_explore_row_label(void* row, void* text) {
    const std::u16string label = announced.made[fake::get<uint64_t>(reinterpret_cast<uint64_t>(text)) - 1];
    atlas.log += "label" + std::to_string(reinterpret_cast<uint64_t>(row) - atlas.rows) + "=" + std::string(label.begin(), label.end()) + " ";
}
// The game's own set-up of its menu of areas: it builds a row for each number in its list.
uint64_t game_explore_setup(uint64_t) {
    const uint64_t at = fake::main_image() + t::data::explore_areas, data = fake::get<uint64_t>(at + t::array::data);
    atlas.log += "rows:";
    for (int32_t i = 0; i < fake::get<int32_t>(at + t::array::count); ++i) atlas.log += (i ? "," : "") + std::to_string(fake::get<int32_t>(data + 4 * uint64_t(i)));
    atlas.log += fake::get<int32_t>(at + t::array::max) >= fake::get<int32_t>(at + t::array::count) ? " " : "?max ";
    return 0;
}
uint64_t game_explore_destruct(uint64_t) {
    atlas.log += "end ";
    return 0;
}

// A count beside the list keeps what it shows as a text: a pointer to its string. The game's own way of giving it
// another text.
constexpr uint64_t kCountText = 0x60;
void put_count(uint64_t block, const std::u16string& text) {
    const uint64_t header = fake::memory(0x10), units = fake::memory(2 * (text.size() + 1));
    std::memcpy(reinterpret_cast<void*>(units), text.data(), 2 * text.size());
    fake::put(header + t::array::data, units), fake::put<int32_t>(header + t::array::count, int32_t(text.size() + 1)), fake::put<int32_t>(header + t::array::max, int32_t(text.size() + 1));
    fake::put(block + kCountText, header);
}
void game_count_set(void* block, void* text) {
    put_count(reinterpret_cast<uint64_t>(block), announced.made[fake::get<uint64_t>(reinterpret_cast<uint64_t>(text)) - 1]);
    atlas.log += "count ";
}


void mapping() {
    array_lengths(); // what the list of areas shows for a map is built on lists in rows that are made longer
    const TSUCHI_StoryService& tale = *tsuchi::host.story;
    const uint64_t main = fake::main_image();
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/", record_at = "save:/tsuchi/tsuchi.content/new_maps.json";
    namespace f = t::flow_stage;
    const auto problem = [](const char* id) {
        const tsuchi::ModRecord* record = tsuchi::find_mod(id);
        return std::string(!record ? "<no such mod>" : record->state == tsuchi::ModState::blocked ? record->problem.get() : record->state == tsuchi::ModState::ready ? "<ready>" : "<running>");
    };
    const auto log_is = [](const std::string& expected) {
        const bool same = atlas.log == expected;
        if (!same) std::printf("  log: [%s]\n", atlas.log.c_str());
        atlas.log.clear();
        return same;
    };
    const auto steps = [](int count) {
        for (int i = 0; i < count; ++i) tsuchi::frame(0.03f);
    };
    // The level the game knows a map by, as travel and the name of the loaded world use it.
    const auto level_of = [](const char* id) {
        char level[10] = "";
        const TSUCHI_Result result = tsuchi::new_map_level(id, level);
        return std::string(result == TSUCHI_OK ? level : result == TSUCHI_NOT_READY ? "<not ready>" : result == TSUCHI_NOT_FOUND ? "<not found>" : "<?>");
    };
    std::vector<std::string> made; // the mods of these checks
    const auto declare = [&](const std::string& id, const std::string& maps) {
        disk.files[mods_at + id + "/mod.json"] = "{\"id\": \"" + id + "\", \"name\": \"N\", \"version\": \"1.0.0\", \"new_maps\": " + maps + "}";
        if (std::find(made.begin(), made.end(), id) == made.end()) made.push_back(id);
    };
    const auto absent = [&](const std::string& id) { disk.files.erase(mods_at + id + "/mod.json"); };
    // A map's builds: its level and its scene for each number, in a folder of one letter. The made-up game stores
    // names of up to 21 units, so a scene goes by its package's path, which is the shorter way to write it.
    const auto builds = [](char folder, std::initializer_list<int> areas) {
        std::string text;
        for (int area : areas) {
            char one[128];
            std::snprintf(one, sizeof one, "%s{\"world\": \"/%c/Area%03d_P\", \"scene\": \"/%c/SD_Area%03d\"}", text.empty() ? "" : ", ", folder, area, folder, area);
            text += one;
        }
        return "[" + text + "]";
    };
    const auto one_map = [&](const std::string& id, char folder, std::initializer_list<int> areas, const std::string& name = "Course") {
        declare(id, "{\"one\": {\"name\": \"" + name + "\", \"builds\": " + builds(folder, areas) + "}}");
    };

    // ---- The made-up game's side -------------------------------------------------------------------
    // Its keeper of scenes, with a list of two that is full; its story, with a base for each area's number; its
    // table of bases, with the first area's row; its list of areas, and a menu that shows them.
    const uint64_t keeper_type = fake::object(u"GameSceneMgr", w.root), scene_type = fake::object(u"GameSceneData", w.root), tally_type = fake::object(u"ActorPlacedNumAsset", w.root);
    const uint64_t keeper = fake::object(u"Scenes", keeper_type, 0x80), base_scene = fake::object(u"SD_Area", scene_type, 0x80), tally = fake::object(u"APN_Area", tally_type);
    const uint64_t own_scenes = fake::memory(16), scenes_at = keeper + t::scene_manager::scenes;
    fake::put(own_scenes, fake::object(u"SD_Title", scene_type, 0x80)), fake::put(own_scenes + 8, fake::object(u"SD_Area001", scene_type, 0x80));
    fake::put(scenes_at + t::array::data, own_scenes), fake::put<int32_t>(scenes_at + t::array::count, 2), fake::put<int32_t>(scenes_at + t::array::max, 2);
    const auto scene_count = [&] { return fake::get<int32_t>(scenes_at + t::array::count); };
    const auto scene_at = [&](int32_t index) { return fake::get<uint64_t>(fake::get<uint64_t>(scenes_at + t::array::data) + 8 * uint64_t(index)); };
    const uint64_t old_instance = fake::get<uint64_t>(main + t::data::game_instance), old_keeper = fake::get<uint64_t>(running.instance + t::game_instance::scene_manager);
    const uint64_t old_context = fake::get<uint64_t>(main + t::data::game_context), old_story = fake::get<uint64_t>(running.instance + t::game_instance::story_save);
    fake::put(main + t::data::game_instance, running.instance), fake::put(main + t::data::game_context, running.context);
    fake::put(running.instance + t::game_instance::scene_manager, keeper);
    // A scene of a mod's, on the shelf under its path: it names its level, the scene it takes the rest from, and its counts.
    const auto scene_for = [&](char folder, int area, bool whole = true) {
        char leaf[16], path[24], level[40];
        std::snprintf(leaf, sizeof leaf, "SD_Area%03d", area), std::snprintf(path, sizeof path, "/%c/SD_Area%03d", folder, area);
        std::snprintf(level, sizeof level, "/%c/Area%03d_P.Area%03d_P", folder, area, area);
        const uint64_t scene = fake::object(wide(leaf), scene_type, 0x80);
        fake::put(scene + t::scene_data::world, fake::name(wide(level))), fake::put(scene + t::scene_data::parent, whole ? base_scene : 0), fake::put(scene + t::scene_data::counts, tally);
        const std::u16string at = wide(path);
        shelf.assets[fake::engine_name(const_cast<char16_t*>(at.data()), at.size(), 1)] = scene;
        return scene;
    };
    std::map<char, uint64_t> fives; // each folder's scene for area 5
    for (char folder : {'A', 'B', 'C', 'D', 'E'})
        for (int area : {5, 7, 8, 9}) {
            const uint64_t scene = scene_for(folder, area);
            if (area == 5) fives[folder] = scene;
        }
    const auto old_loader = t::fn::load_soft_object;
    const auto old_text_maker = t::fn::text_from_string;
    t::fn::load_soft_object = game_load_soft_object;
    t::fn::text_from_string = &game_text_from_string;
    t::fn::story_flags = game_story_flags, t::fn::story_find_area = game_story_find_area, t::fn::story_add_area = game_story_add_area, t::fn::story_link_area = game_story_link_area;
    t::fn::explore_row = game_explore_row, t::fn::explore_row_label = game_explore_row_label, t::fn::scene_array_grow = game_scene_grow;

    // The table of bases. Its path is longer than a name of the made-up game, so it is found by its own name.
    tsuchi::game::ClassRef table_ref{"DataTable"};
    const uint64_t table_functions = fake::memory(0x40), base_row_type = fake::engine_object(u"BaseRow", w.root);
    fake::field(base_row_type, u"BaseCampId", u"IntProperty", 0x4, 4), fake::field(base_row_type, u"ToLevelName", u"NameProperty", 0x8, 8);
    fake::put(table_functions + t::data_table_table::add_row, &game_add_table_row), fake::put(table_functions + t::data_table_table::remove_row, &game_remove_table_row);
    fake::put(table_functions + t::data_table_table::add_row_internal, &game_add_table_row_internal);
    const uint64_t bases = fake::object(u"DT_AreaBaseCampTransition", find_class(table_ref), 0x60), first_base = fake::memory(kRowBytes);
    fake::put(bases, table_functions), fake::put(bases + t::data_table::row_struct, base_row_type);
    fake::put<int32_t>(first_base + 0x4, 7), fake::put(first_base + 0x8, row_name(u"Area001_P"));
    sheets.rows[{bases, row_name(u"Area001_00")}] = first_base;
    const auto base_row = [&](const char16_t* row) {
        const auto found = sheets.rows.find({bases, row_name(row)});
        return found == sheets.rows.end() ? std::string("<none>") : text_of_name(fake::get<uint64_t>(found->second + 0x8)) + "/" + std::to_string(fake::get<int32_t>(found->second + 0x4));
    };

    // The story the menu lists areas of, and the game's own list of them.
    const uint64_t story = fake::object(u"StoryOfMaps", w.component_class, 0x80), base_states = fake::memory(10 * t::area_bases::size), own_areas = fake::memory(6 * 4);
    fake::put(story + t::story_save::area_bases + t::array::data, base_states), fake::put<int32_t>(story + t::story_save::area_bases + t::array::count, 10);
    const int32_t game_areas[6] = {1, 2, 3, 10, 4, 6};
    std::memcpy(reinterpret_cast<void*>(own_areas), game_areas, sizeof game_areas);
    const uint64_t areas_at = main + t::data::explore_areas;
    fake::put(areas_at + t::array::data, own_areas), fake::put<int32_t>(areas_at + t::array::count, 6), fake::put<int32_t>(areas_at + t::array::max, 6);
    const auto base_state = [&](int area) { return int(fake::get<uint8_t>(base_states + uint64_t(area) * t::area_bases::size + t::area_bases::first_state)); };
    atlas.rows = fake::memory(64);

    // The menu: two lists of zoom values, one entry for each of the game's rows, and a panel that can be drawn smaller.
    const uint64_t kArgument = t::constant::property_param;
    constexpr int32_t kWorldZooms = 0x80, kIconZooms = 0x90;
    const uint64_t pair_type = fake::memory(0x80);
    fake::put<uint64_t>(pair_type, fake::engine().vtable), fake::put<int32_t>(pair_type + t::struct_type::bytes, 8), fake::put<int32_t>(pair_type + t::struct_type::alignment, 4);
    fake::field(pair_type, u"X", u"FloatProperty", 0, 4), fake::field(pair_type, u"Y", u"FloatProperty", 4, 4);
    tsuchi::game::ClassRef widget_ref{"Widget"};
    const uint64_t widget_type = find_class(widget_ref) ? find_class(widget_ref) : fake::object(u"Widget", w.root);
    const uint64_t menu_type = fake::object(u"AreaMenu", w.root), panel_type = fake::object(u"RowPanel", w.root);
    fake::set_super(panel_type, widget_type);
    fake::field(menu_type, u"WorldMapZooms", u"ArrayProperty", kWorldZooms, 0x10, fake::loose_field(u"StructProperty", 8, pair_type));
    fake::field(menu_type, u"AreaIconZooms", u"ArrayProperty", kIconZooms, 0x10, fake::loose_field(u"StructProperty", 8, pair_type));
    const auto two = [](const char* what, uint64_t arguments) {
        char line[64];
        std::snprintf(line, sizeof line, "%s:%.3g,%.3g ", what, fake::get<float>(arguments), fake::get<float>(arguments + 4));
        atlas.log += line;
    };
    const uint64_t set_pivot = fake::function(panel_type, function_type, u"SetRenderTransformPivot", 8, 4, [&](uint64_t, uint64_t arguments) { two("pivot", arguments); });
    const uint64_t set_scale = fake::function(panel_type, function_type, u"SetRenderScale", 8, 4, [&](uint64_t, uint64_t arguments) { two("scale", arguments); });
    fake::flag(fake::field(set_pivot, u"Pivot", u"StructProperty", 0, 8, pair_type), kArgument), fake::flag(fake::field(set_scale, u"Scale", u"StructProperty", 0, 8, pair_type), kArgument);
    const auto menu_of = [&](bool night = false, bool hero = false) {
        const uint64_t menu = fake::engine_object(u"Menu", menu_type, 0x100), panel = fake::engine_object(u"Panel", panel_type);
        fake::put(menu + t::explore_menu::story, story), fake::put(menu + t::explore_menu::panel, panel);
        fake::put<uint8_t>(menu + t::explore_menu::night, night), fake::put<uint8_t>(menu + t::explore_menu::hero, hero);
        for (int32_t list : {kWorldZooms, kIconZooms}) {
            auto* values = static_cast<float*>(tsuchi::game::engine_memory(6 * 8));
            for (int i = 0; i < 12; ++i) values[i] = float(list + i);
            fake::put(menu + uint64_t(list) + t::array::data, values), fake::put<int32_t>(menu + uint64_t(list) + t::array::count, 6), fake::put<int32_t>(menu + uint64_t(list) + t::array::max, 6);
        }
        return menu;
    };
    const auto zooms = [&](uint64_t menu, int32_t list) { // how many entries, and the last one's first value
        const int32_t count = fake::get<int32_t>(menu + uint64_t(list) + t::array::count);
        return std::to_string(count) + ":" + std::to_string(int(fake::get<float>(fake::get<uint64_t>(menu + uint64_t(list) + t::array::data) + 8 * uint64_t(count - 1))));
    };
    // The two entries of the menu's function table, with the game's own functions in them, and the game calling them.
    const uint64_t setup_entry = main + t::slot::explore_setup.table + t::slot::explore_setup.offset, destruct_entry = main + t::slot::explore_destruct.table + t::slot::explore_destruct.offset;
    fake::put(setup_entry, &game_explore_setup), fake::put(destruct_entry, &game_explore_destruct);
    for (auto function : {reinterpret_cast<uint64_t>(&game_explore_setup), reinterpret_cast<uint64_t>(&game_explore_destruct)}) fake::code().push_back(function);
    const auto game_opens = [&](uint64_t menu) { thunks::call(fake::get<uint64_t>(setup_entry), menu); };
    const auto game_ends = [&](uint64_t menu) { thunks::call(fake::get<uint64_t>(destruct_entry), menu); };
    const auto list_is_the_games = [&] { return fake::get<uint64_t>(areas_at + t::array::data) == own_areas && fake::get<int32_t>(areas_at + t::array::count) == 6 && fake::get<int32_t>(areas_at + t::array::max) == 6; };
    // A run of the game from its start: the mods are found and started anew, and the game's list of scenes is its own.
    const auto run = [&] {
        tsuchi::forget_mods();
        fake::put<int32_t>(scenes_at + t::array::count, 2);
        tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
        tsuchi::start_mods();
    };
    const tsuchi::NewMapCounts before = tsuchi::new_map_counts();
    tsuchi::forget_mods();
    disk.files.erase(record_at);
    restart_records(record_at);
    logged.lines.clear();

    // ---- A part that is written wrongly blocks its mod as the manifest is read ------------------------
    {
        const std::string good = "\"builds\": " + builds('A', {5});
        declare("nm.list", "[]");
        declare("nm.name", "{\"Bad Name\": {\"name\": \"N\", " + good + "}}");
        declare("nm.plain", "{\"one\": 5}");
        declare("nm.key", "{\"one\": {\"name\": \"N\", " + good + ", \"slot\": 5}}");
        declare("nm.unnamed", "{\"one\": {" + good + "}}");
        declare("nm.nobuild", "{\"one\": {\"name\": \"N\", \"builds\": []}}");
        declare("nm.five", "{\"one\": {\"name\": \"N\", \"builds\": " + builds('A', {5, 7, 8, 9, 5}) + "}}");
        declare("nm.taken", "{\"one\": {\"name\": \"N\", \"builds\": " + builds('A', {6}) + "}}");
        declare("nm.other", R"({"one": {"name": "N", "builds": [{"world": "/A/Area005_P", "scene": "/A/SD_Area007"}]}})");
        declare("nm.object", R"({"one": {"name": "N", "builds": [{"world": "/A/Area005_P", "scene": "/A/SD_Area005.SD_Area007"}]}})");
        declare("nm.twice", R"({"one": {"name": "N", "builds": [{"world": "/A/Area005_P", "scene": "/A/SD_Area005"}, {"world": "/B/Area005_P", "scene": "/B/SD_Area005"}]}})");
        declare("nm.more", R"({"one": {"name": "N", "builds": [{"world": "/A/Area005_P", "scene": "/A/SD_Area005", "slot": 5}]}})");
        declare("nm.radar", "{\"one\": {\"name\": \"N\", " + good + ", \"radar\": {\"size\": 100}}}");
        declare("nm.limits", "{\"one\": {\"name\": \"N\", " + good + ", \"radar\": {\"texture\": \"/A/T.T\", \"limits\": [5, 5, -5, -5]}}}");
        declare("nm.pikmin", "{\"one\": {\"name\": \"N\", " + good + ", \"pikmin\": [\"red\", \"Red\"]}}");
        declare("nm.totals", "{\"one\": {\"name\": \"N\", " + good + ", \"totals\": {\"treasures\": 10001}}}");
        declare("nm.long", "{\"one\": {\"name\": \"" + std::string(161, 'n') + "\", " + good + "}}");
        // Everything a map can say, written as it should be. The scene's path may say the object's name too.
        declare("nm.whole", R"({"one": {"name": "A whole map", "builds": [{"world": "/A/Area005_P", "scene": "/A/SD_Area005.SD_Area005"}, {"world": "/A/Area009_P", "scene": "/A/SD_Area009"}],
                 "preview": "/A/T.T", "radar": {"texture": "/A/R.R", "offset": [1, -2], "size": 12000, "angle": -90, "scale": 3.75, "limits": [-5, 5, 5, -5]},
                 "pikmin": ["red", "ice", "example.gems:ruby"], "totals": {"treasures": 3, "castaways": 0, "flarlic": 10000}}})");
        tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
        CHECK(problem("nm.list") == "mod.json: \"new_maps\" is an object: each of the mod's maps by a short name");
        CHECK(problem("nm.name") == "mod.json: \"new_maps\": a map's name is spelled as a mod's ID is: Bad Name");
        CHECK(problem("nm.plain") == "mod.json: \"new_maps\": one is an object with \"name\" and \"builds\"");
        CHECK(problem("nm.key") == "mod.json: \"new_maps\": one has a key the framework does not know: slot");
        CHECK(problem("nm.unnamed") == "mod.json: \"new_maps\": one: \"name\" is the area's name, as text of up to 160 characters" && problem("nm.long") == problem("nm.unnamed"));
        CHECK(problem("nm.nobuild") == "mod.json: \"new_maps\": one: \"builds\" is a list of the map's builds, one for each of the game's free areas it is built for" &&
              problem("nm.five") == problem("nm.nobuild"));
        CHECK(problem("nm.taken") == "mod.json: \"new_maps\": one: /A/Area006_P is no level named for one of the game's free areas, as Area005_P is");
        CHECK(problem("nm.other") == "mod.json: \"new_maps\": one: /A/SD_Area007 is no scene named for the area of its level, as SD_Area005 is for Area005_P" &&
              problem("nm.object") == "mod.json: \"new_maps\": one: /A/SD_Area005.SD_Area007 is no scene named for the area of its level, as SD_Area005 is for Area005_P");
        CHECK(problem("nm.twice") == "mod.json: \"new_maps\": one: two builds are for the same area: /B/Area005_P");
        CHECK(problem("nm.more") == "mod.json: \"new_maps\": one: a build is \"world\", the package of the map's level, and \"scene\", the path of its scene");
        CHECK(problem("nm.radar") == "mod.json: \"new_maps\": one: \"radar\" is an object with \"texture\", the path of a texture");
        CHECK(problem("nm.limits") == "mod.json: \"new_maps\": one: the radar's \"limits\" are four numbers: left, top, right and bottom, with left below right and bottom below top");
        CHECK(problem("nm.pikmin") == "mod.json: \"new_maps\": one: \"pikmin\" is a list of types of Pikmin, each by its ID and each once");
        CHECK(problem("nm.totals") == "mod.json: \"new_maps\": one: \"totals\" holds \"treasures\", \"castaways\" and \"flarlic\", each a whole number from 0 to 10000");
        CHECK(problem("nm.whole") == "<ready>");
        tsuchi::forget_mods();
        for (const std::string& id : made) absent(id);
    }

    // ---- One map: given a number as its mod starts, and to the game once the game's list of scenes is built ----
    one_map("nm.a", 'A', {5, 7, 8, 9});
    // A scene the game cannot load, one that is not whole, and one whose name the game already has, each block the mod.
    one_map("nm.lost", 'L', {7});
    one_map("nm.half", 'H', {8});
    one_map("nm.same", 'S', {9});
    scene_for('H', 8, false), scene_for('S', 9);
    disk.mounts = false; // the save cannot be opened: nobody can say which map has which number
    run();
    CHECK(problem("nm.a") == "mod.json: \"new_maps\": which map has which of the game's free areas could not be read from the save" && tsuchi::new_map_count() == 0);
    disk.mounts = true;
    run();
    CHECK(problem("nm.a") == "<running>" && tsuchi::new_map_count() == 4 && tsuchi::new_map_counts().scenes_waiting == 4 && level_of("nm.a:one") == "<not ready>");
    CHECK(tale.travel(mod, "nm.a:one", -1) == TSUCHI_NOT_READY && scene_count() == 2 && tsuchi::new_map_counts().menu_hooks == 2);
    CHECK(saved_record(record_at) == "{\"areas\": {\"5\": {\"map\": \"nm.a:one\", \"used\": false}, \"7\": {\"map\": \"nm.lost:one\", \"used\": false}, \"8\": {\"map\": \"nm.half:one\", \"used\": false}, "
                                   "\"9\": {\"map\": \"nm.same:one\", \"used\": false}}}\n");
    // The list is built, and a world begins: each scene is looked at, and the one that is right goes into the list.
    fake::put<uint8_t>(keeper + t::scene_manager::ready, uint8_t(t::constant::scenes_ready));
    const uint64_t old_world = fake::get<uint64_t>(main + t::data::world), field = fake::object(u"Field", w.component_class, 0x40);
    const uint8_t old_begun = fake::get<uint8_t>(w.framework + t::game_framework::world_begun);
    fake::put(field + t::world::level_name, fake::name(u"Area001_P"));
    fake::put(main + t::data::world, field), fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 1);
    steps(1);
    CHECK(scene_count() == 4 && scene_at(2) == fives['A'] && scene_at(0) == fake::get<uint64_t>(own_scenes) && atlas.grown == 1 && level_of("nm.a:one") == "Area005_P" &&
          level_of("nm.same:one") == "Area009_P" && level_of("nm.lost:one") == "<not found>");
    CHECK(tsuchi::new_map_counts().scenes_added == before.scenes_added + 2 && tsuchi::new_map_counts().scenes_waiting == 0 && tsuchi::new_map_counts().scene_list == 4);
    CHECK(logged.lines.find("[nm.lost] new_maps: one: its scene could not be loaded: /L/SD_Area007") != std::string::npos &&
          logged.lines.find("[nm.half] new_maps: one: /H/SD_Area008 is no scene named SD_Area008 for the level /H/Area008_P") != std::string::npos);
    // Its base has a row in the game's table, a copy of the first area's for its own level, and its name is the game's text for the area.
    CHECK(base_row(u"Area005_00") == "Area005_P/1" && base_row(u"Area001_00") == "Area001_P/7" && base_row(u"Area007_00") == "<none>");
    char text[128] = "";
    uint64_t needed = 0;
    CHECK(tsuchi::host.text->get(mod, "MapName", "Area005_00", text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "Course"));
    // In a run where the list is built as mods start, as it is at the title screen, a scene the game will not take blocks its mod, and the number is nobody's.
    fake::put(fake::get<uint64_t>(scenes_at + t::array::data) + 8, fake::object(u"SD_Area009", scene_type, 0x80)); // the game has a scene of that name
    disk.files.erase(record_at);
    restart_records(record_at);
    run();
    CHECK(problem("nm.a") == "<running>" && level_of("nm.a:one") == "Area005_P" && scene_count() == 3 && tsuchi::new_map_count() == 1);
    CHECK(problem("nm.lost") == "mod.json: \"new_maps\": one: its scene could not be loaded: /L/SD_Area007");
    CHECK(problem("nm.half") == "mod.json: \"new_maps\": one: /H/SD_Area008 is no scene named SD_Area008 for the level /H/Area008_P");
    CHECK(problem("nm.same") == "mod.json: \"new_maps\": one: the game already has a scene named SD_Area009, from another package");
    CHECK(saved_record(record_at) == "{\"areas\": {\"5\": {\"map\": \"nm.a:one\", \"used\": false}}}\n" && base_row(u"Area005_00") == "Area005_P/1");
    fake::put(fake::get<uint64_t>(scenes_at + t::array::data) + 8, fake::get<uint64_t>(own_scenes + 8));
    absent("nm.lost"), absent("nm.half"), absent("nm.same");

    // ---- Travel by the map's ID, and the loaded world told by it ---------------------------------------
    const uint64_t context = running.context, flow_root = fake::get<uint64_t>(context + t::game_context::flow);
    const uint64_t old_top = fake::get<uint64_t>(flow_root + f::child), old_requests = fake::get<uint64_t>(context + t::game_context::requests), old_mode = fake::get<uint64_t>(main + t::data::game_mode);
    const uint64_t old_clock = fake::get<uint64_t>(w.framework + t::game_framework::clock);
    const uint8_t old_rule = fake::get<uint8_t>(context + t::game_context::rule);
    const uint32_t old_paused = fake::get<uint32_t>(running.pauses + t::pause_manager::active);
    const uint64_t story_stage = fake::memory(f::size), play = fake::memory(f::size), queue = fake::memory(0x20), queue_table = fake::memory(0x20), clock = fake::memory(0x20);
    fake::put(story_stage, main + t::data::story_flow_table), fake::put(story_stage + f::name, fake::name(u"Story")), fake::put(story_stage + f::child, play);
    fake::put(play, main + t::data::story_play_table), fake::put(play + f::name, fake::name(u"StoryInGame")), fake::put<int32_t>(play + f::status, 1);
    fake::put(flow_root + f::child, story_stage), fake::put<uint8_t>(context + t::game_context::rule, uint8_t(t::constant::rule_story));
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, 0);
    fake::put(queue, queue_table), fake::put(queue_table + t::request_queue_table::push, &game_push_request), fake::put(context + t::game_context::requests, queue);
    fake::put(main + t::data::game_mode, fake::memory(0x10));
    fake::put<uint64_t>(clock, main + t::data::clock_table), fake::put(w.framework + t::game_framework::clock, clock);
    fake::put(running.instance + t::game_instance::story_save, story);
    const auto old_request_maker = t::fn::travel_request;
    t::fn::travel_request = game_travel_request;
    const size_t posted = journey.posted.size();
    CHECK(tale.travel(mod, "nm.b:one", -1) == TSUCHI_NOT_FOUND && tale.travel(mod, "nm.a:two", -1) == TSUCHI_NOT_FOUND && journey.posted.size() == posted);
    CHECK(tale.travel(mod, "nm.a:one", 1) == TSUCHI_OK && journey.posted.size() == posted + 1);
    if (journey.posted.size() == posted + 1)
        CHECK(text_of_name(fake::get<uint64_t>(journey.posted.back() + t::travel_request::area)) == "Area005_P" && fake::get<int32_t>(journey.posted.back() + t::travel_request::base) == 1);
    CHECK(base_state(5)==int(t::constant::base_open) && saved_record(record_at).find("\"used\": true")!=std::string::npos);
    steps(300); // the request is taken to have come to nothing
    // The world the game loads for it is told by the map's ID; a free area that is no running map's, by its level's name.
    fake::put(field + t::world::level_name, fake::name(u"Area005_P"));
    CHECK(tsuchi::host.game->world(mod, text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "nm.a:one") && needed == 9);
    CHECK(tsuchi::host.game->world(mod, text, 8, &needed) == TSUCHI_CAPACITY && needed == 9);
    fake::put(field + t::world::level_name, fake::name(u"Area007_P"));
    CHECK(tsuchi::host.game->world(mod, text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "Area007_P"));

    // ---- The game's menu of areas ----------------------------------------------------------------------
    // The story and zooms can be prepared before the UI tables load. Until Types is available the game's list
    // stays its own: a custom row must not be published without its AreaType assignment.
    atlas.log.clear();
    const uint64_t menu = menu_of();
    const tsuchi::NewMapCounts closed = tsuchi::new_map_counts();
    game_opens(menu);
    CHECK(log_is("rows:1,2,3,10,4,6 "));
    CHECK(base_state(5) == int(t::constant::base_open) && base_state(7) == 0 && base_state(1) == 0 && zooms(menu, kWorldZooms) == "7:" + std::to_string(kWorldZooms + 10) &&
          zooms(menu, kIconZooms) == "7:" + std::to_string(kIconZooms + 10) && list_is_the_games());
    CHECK(tsuchi::new_map_counts().menus_opened == closed.menus_opened + 1 && tsuchi::new_map_counts().menus_listed == closed.menus_listed &&
          tsuchi::new_map_counts().rows_named == closed.rows_named && tsuchi::new_map_counts().zooms_grown == closed.zooms_grown + 2 &&
          tsuchi::new_map_counts().area_records_made == closed.area_records_made && tsuchi::new_map_counts().bases_opened == closed.bases_opened);
    // From here on a save may hold the map's progress under the number: the record says so at the next frame.
    steps(1);
    CHECK(saved_record(record_at) == "{\"areas\": {\"5\": {\"map\": \"nm.a:one\", \"used\": true}}}\n");
    // The menu ends: the panel has its own size, and the list is the game's again.
    game_ends(menu);
    CHECK(log_is("end ") && list_is_the_games());
    // Opened again, the story has its record already. A menu for a night expedition and one in Olimar's Shipwreck
    // Tale list the game's own areas, and so does any menu the framework could not give its zoom values.
    const uint64_t again = menu_of(), by_night = menu_of(true), olimars = menu_of(false, true), without = menu_of();
    game_opens(again);
    CHECK(log_is("rows:1,2,3,10,4,6 "));
    game_ends(by_night); // another menu's end leaves the list as it is
    CHECK(log_is("end ") && list_is_the_games());
    game_ends(again);
    CHECK(log_is("end ") && list_is_the_games());
    game_opens(by_night), game_ends(by_night), game_opens(olimars), game_ends(olimars);
    CHECK(log_is("rows:1,2,3,10,4,6 end rows:1,2,3,10,4,6 end ") && zooms(by_night, kWorldZooms) == "6:" + std::to_string(kWorldZooms + 10));
    atlas.flags = t::constant::story_of_olimar;
    game_opens(again), game_ends(again);
    atlas.flags = 0;
    fake::put<int32_t>(without + kIconZooms + t::array::count, 0);
    game_opens(without), game_ends(without);
    CHECK(log_is("rows:1,2,3,10,4,6 end rows:1,2,3,10,4,6 end ") && tsuchi::new_map_counts().zooms_failed == closed.zooms_failed + 1 && list_is_the_games());
    // A menu that never ended: the next one starts from the game's own list.
    game_opens(menu_of());
    const uint64_t next = menu_of();
    game_opens(next);
    CHECK(log_is("rows:1,2,3,10,4,6 rows:1,2,3,10,4,6 "));
    game_ends(next);
    CHECK(log_is("end ") && list_is_the_games());

    // ---- Several mods: each map its own number, and one map too many -------------------------------
    one_map("nm.b", 'B', {5, 7, 8, 9}, "Hills"), one_map("nm.c", 'C', {5, 7, 8, 9}), one_map("nm.d", 'D', {5, 7, 8, 9}), one_map("nm.e", 'E', {5, 7, 8, 9}, "Shore");
    run();
    CHECK(level_of("nm.a:one") == "Area005_P" && level_of("nm.b:one") == "Area007_P" && level_of("nm.c:one") == "Area008_P" && level_of("nm.d:one") == "Area009_P" && tsuchi::new_map_count() == 4);
    CHECK(problem("nm.e") == "mod.json: \"new_maps\": one: the game has no free area for it: area 5 is nm.a:one's, area 7 is nm.b:one's, area 8 is nm.c:one's, area 9 is nm.d:one's" &&
          level_of("nm.e:one") == "<not found>" && scene_count() == 6);
    CHECK(saved_record(record_at) == "{\"areas\": {\"5\": {\"map\": \"nm.a:one\", \"used\": true}, \"7\": {\"map\": \"nm.b:one\", \"used\": false}, \"8\": {\"map\": \"nm.c:one\", \"used\": false}, "
                                   "\"9\": {\"map\": \"nm.d:one\", \"used\": false}}}\n");
    CHECK(base_row(u"Area007_00") == "Area007_P/1" && base_row(u"Area009_00") == "Area009_P/1" && tsuchi::host.text->get(mod, "MapName", "Area007_00", text, sizeof text, &needed) == TSUCHI_OK &&
          !std::strcmp(text, "Hills"));
    // The same numbers in the next run. A game that does not lead to its keeper of scenes the usual way has it found among its objects.
    fake::put(running.instance + t::game_instance::scene_manager, old_keeper);
    run();
    CHECK(level_of("nm.a:one") == "Area005_P" && level_of("nm.b:one") == "Area007_P" && level_of("nm.d:one") == "Area009_P" && problem("nm.e").find("no free area") != std::string::npos &&
          scene_count() == 6);
    fake::put(running.instance + t::game_instance::scene_manager, keeper);
    // A map whose mod is gone and that never got as far as a save gives its number up to the next map that needs it,
    // and does not get it back from that map.
    absent("nm.d");
    run();
    CHECK(level_of("nm.e:one") == "Area009_P" && level_of("nm.a:one") == "Area005_P" && level_of("nm.d:one") == "<not found>" && problem("nm.e") == "<running>");
    one_map("nm.d", 'D', {5, 7, 8, 9});
    run();
    CHECK(level_of("nm.e:one") == "Area009_P" && problem("nm.d") == "mod.json: \"new_maps\": one: the game has no free area for it: area 5 is nm.a:one's, area 7 is nm.b:one's, "
                                                                    "area 8 is nm.c:one's, area 9 is nm.e:one's");
    // All four in the menu, in the order they were given their numbers.
    const uint64_t full = menu_of();
    game_opens(full);
    CHECK(log_is("add:Area007_P link:Area007_P add:Area008_P link:Area008_P add:Area009_P link:Area009_P rows:1,2,3,10,4,6 ") && zooms(full, kWorldZooms) == "10:" + std::to_string(kWorldZooms + 10));
    CHECK(story_bases_given == int32_t(t::constant::story_area_bases)); // as many bases as the story gives its own areas
    game_ends(full);
    steps(1);
    CHECK(log_is("end ") && saved_record(record_at).find("\"9\": {\"map\": \"nm.e:one\", \"used\": true}") != std::string::npos);
    // A map whose mod is gone and that a save may hold progress of keeps its number: nobody else gets it, and it
    // has it again when its mod is back.
    absent("nm.a");
    run();
    CHECK(level_of("nm.a:one") == "<not found>" && level_of("nm.b:one") == "Area007_P" && tsuchi::new_map_count() == 3 &&
          problem("nm.d") == "mod.json: \"new_maps\": one: the game has no free area for it: area 5 is kept for nm.a:one, which is not running, with the player's progress there, "
                             "area 7 is nm.b:one's, area 8 is nm.c:one's, area 9 is nm.e:one's");
    CHECK(tale.travel(mod, "nm.a:one", -1) == TSUCHI_NOT_FOUND && saved_record(record_at).find("\"5\": {\"map\": \"nm.a:one\", \"used\": true}") != std::string::npos);
    // With the mod gone the game is given nothing of its map: the menu does not list the area, and the world is told by its level's name.
    const uint64_t fewer = menu_of();
    game_opens(fewer), game_ends(fewer);
    CHECK(log_is("rows:1,2,3,10,4,6 end "));
    fake::put(field + t::world::level_name, fake::name(u"Area005_P"));
    CHECK(tsuchi::host.game->world(mod, text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "Area005_P"));
    one_map("nm.a", 'A', {5, 7, 8, 9});
    run();
    CHECK(level_of("nm.a:one") == "Area005_P" && tsuchi::host.game->world(mod, text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "nm.a:one"));
    // A map that no longer has a build for the number its progress is under blocks its mod: the progress would be left behind.
    one_map("nm.a", 'A', {7, 8});
    run();
    CHECK(problem("nm.a") == "mod.json: \"new_maps\": one: the player's progress in it is kept under the game's area 5, and it has no build for that area");
    one_map("nm.a", 'A', {5, 7, 8, 9});

    // ---- A mod that stops takes its map with it --------------------------------------------------------
    run();
    const std::string kept_before = saved_record(record_at);
    tsuchi::forget_mods();
    const uint64_t after = menu_of();
    game_opens(after), game_ends(after);
    CHECK(tsuchi::new_map_count() == 0 && level_of("nm.a:one") == "<not found>" && tale.travel(mod, "nm.a:one", -1) == TSUCHI_NOT_FOUND && log_is("rows:1,2,3,10,4,6 end ") && list_is_the_games());
    CHECK(tsuchi::host.game->world(mod, text, sizeof text, &needed) == TSUCHI_OK && !std::strcmp(text, "Area005_P") && saved_record(record_at) == kept_before);
    CHECK(scene_count() == 6 && base_row(u"Area005_00") == "Area005_P/1"); // what the game was given stays where the game may hold on to it

    // Neither damaged record may hand another map the number of this map's progress.
    const uint32_t damaged=tsuchi::new_map_counts().record_damaged;
    disk.files[record_at]="nothing that is JSON";
    restart_records(record_at);
    run();
    CHECK(level_of("nm.a:one")=="<not found>" && problem("nm.a").find("damaged")!=std::string::npos);
    CHECK(disk.files[record_at]=="nothing that is JSON" && tsuchi::new_map_counts().record_damaged>damaged);
    disk.files[record_at]=kept_before;
    restart_records(record_at);
    run();

    // ---- What the list of areas and the radar show for a map -----------------------------------------
    // Until here the made-up game had none of the five tables that is kept in, and every map waited for them.
    const tsuchi::NewMapCounts bare = tsuchi::new_map_counts();
    CHECK(bare.ui_waits > before.ui_waits && bare.ui_rows_made == before.ui_rows_made && bare.ui_rows_failed == before.ui_rows_failed && bare.messages_set == before.messages_set);
    namespace so = t::soft_object;
    const TSUCHI_DatatableService& sheet = *tsuchi::host.datatable;
    const uint64_t texture_type = fake::object(u"Texture2D", w.root), margin_type = fake::memory(0x80);
    fake::put<uint64_t>(margin_type, fake::engine().vtable);
    int32_t side_at = 0;
    for (const char16_t* side : {u"Left", u"Top", u"Right", u"Bottom"}) fake::field(margin_type, side, u"FloatProperty", side_at, 4), side_at += 4;
    const uint64_t slides_type = fake::engine_object(u"SlidesRow", w.root), types_type = fake::engine_object(u"TypesRow", w.root), totals_type = fake::engine_object(u"TotalsRow", w.root);
    const uint64_t radar_type = fake::engine_object(u"RadarRow", w.root), notes_type = fake::engine_object(u"NotesRow", w.root);
    fake::field(slides_type, u"ImageTextureList", u"ArrayProperty", 0x0, 0x10, fake::loose_field(u"SoftObjectProperty", int32_t(so::size), texture_type));
    fake::field(types_type, u"AreaType", u"ByteProperty", 0x0, 1);
    fake::field(types_type, u"TargetPikminColor", u"ArrayProperty", 0x8, 0x10, fake::loose_field(u"ByteProperty", 1));
    fake::field(types_type, u"DifficultList", u"ArrayProperty", 0x18, 0x10, fake::loose_field(u"IntProperty", 4));
    fake::field(totals_type, u"AreaName", u"NameProperty", 0x0, 8);
    fake::field(totals_type, u"GroundOtakara", u"IntProperty", 0x8, 4), fake::field(totals_type, u"SURVIVOR", u"IntProperty", 0xC, 4), fake::field(totals_type, u"Wasurena", u"IntProperty", 0x10, 4);
    fake::field(radar_type, u"SceneName", u"NameProperty", 0x0, 8);
    fake::field(radar_type, u"Texture", u"SoftObjectProperty", 0x8, int32_t(so::size), texture_type);
    fake::field(radar_type, u"PositionOffset", u"StructProperty", 0x38, 8, pair_type);
    fake::field(radar_type, u"DefaultAngle", u"FloatProperty", 0x40, 4), fake::field(radar_type, u"BaseScale", u"FloatProperty", 0x44, 4), fake::field(radar_type, u"CaptureSize", u"FloatProperty", 0x48, 4);
    fake::field(radar_type, u"CursorMoveLimit", u"StructProperty", 0x4C, 16, margin_type);
    fake::field(notes_type, u"AreaBitFlags", u"IntProperty", 0x0, 4);
    const auto shelve = [](const std::u16string& path, uint64_t asset) { shelf.assets[fake::engine_name(const_cast<char16_t*>(path.data()), path.size(), 1)] = asset; };
    const auto shown_table = [&](const std::u16string& name, uint64_t row_type) {
        const uint64_t made_table = fake::object(name, find_class(table_ref), 0x60);
        fake::put(made_table, table_functions), fake::put(made_table + t::data_table::row_struct, row_type);
        return made_table;
    };
    const uint64_t slides = shown_table(u"Slides", slides_type), types = shown_table(u"Types", types_type), totals = shown_table(u"Totals", totals_type);
    const uint64_t radars = shown_table(u"Radar", radar_type), notes = shown_table(u"Notes", notes_type);
    const auto row_in = [](uint64_t table, const std::u16string& name) {
        const uint64_t row = fake::memory(kRowBytes);
        sheets.rows[{table, row_name(name)}] = row;
        return row;
    };
    const auto row_at = [](uint64_t table, const std::u16string& name) {
        const auto found = sheets.rows.find({table, row_name(name)});
        return found == sheets.rows.end() ? 0 : found->second;
    };
    const auto path_in = [](uint64_t at, const std::u16string& path) { fake::put<int32_t>(at + so::index, -1), fake::put(at + so::path, row_name(path)); };
    const auto list_in = [](uint64_t at, uint64_t data, int32_t count) { fake::put(at + t::array::data, data), fake::put(at + t::array::count, count), fake::put(at + t::array::max, count); };
    // The first area's rows, which a new area's begin as copies of: one picture, two types, a number for each of
    // three nights, and a radar.
    const uint64_t first_picture = fake::memory(so::size), first_colors = fake::memory(8), first_nights = fake::memory(12), first_types = row_in(types, u"Area001");
    path_in(first_picture, u"/G/Pic.Pic"), list_in(row_in(slides, u"Area001"), first_picture, 1);
    fake::put<uint8_t>(first_colors + 1, 1), list_in(first_types + 0x8, first_colors, 2), list_in(first_types + 0x18, first_nights, 3);
    for (int i = 0; i < 3; ++i) fake::put<int32_t>(first_nights + 4 * uint64_t(i), i + 1);
    const uint64_t first_radar = row_in(radars, u"Area001");
    fake::put(first_radar, row_name(u"Area001")), path_in(first_radar + 0x8, u"/G/Map.Map"), fake::put(first_radar + 0x48, 12000.0f), fake::put(first_radar + 0x40, -90.0f);
    // The game's table of totals has a row for every number, the free ones too; and its table of messages has
    // three that are for all six of its areas.
    for (int area : {5, 7, 8, 9}) {
        char level[12];
        std::snprintf(level, sizeof level, "Area%03d_P", area);
        const uint64_t row = row_in(totals, wide(level));
        fake::put(row, row_name(wide(level)));
        for (int i = 0; i < 3; ++i) fake::put<int32_t>(row + 0x8 + 4 * uint64_t(i), 9);
    }
    for (const char16_t* note : {u"Note_More", u"Note_Base", u"Note_Done"}) fake::put<int32_t>(row_in(notes, note), 1118);
    // Pictures of a mod's own, each under its path; and an asset that is no picture.
    const auto asset = [&](const std::u16string& package, const std::u16string& leaf, uint64_t type) {
        const uint64_t made_asset = fake::object(leaf, type, 0x40);
        fake::put(made_asset + t::object::outer, fake::object(package, w.component_class, 0x40));
        shelve(package + u"." + leaf, made_asset);
        return made_asset;
    };
    const uint64_t preview_picture = asset(u"/A/T", u"T", texture_type), radar_picture = asset(u"/A/R", u"R", texture_type), other_radar = asset(u"/A/M", u"M", texture_type);
    asset(u"/A/N", u"N", w.component_class);
    // A field of a row, as text: a number, a name or a path, or how long a list is.
    const auto cell = [&](uint64_t table, const std::string& row, const char* path) {
        const TSUCHI_Object of{to_handle(table)};
        TSUCHI_Value got{};
        char buffer[64] = "";
        uint64_t bytes = 0;
        if (sheet.get(mod, of, row.c_str(), path, &got) != TSUCHI_OK) return std::string("<none>");
        if (got.kind == TSUCHI_VALUE_INTEGER) return std::to_string(got.as.integer);
        if (got.kind == TSUCHI_VALUE_ARRAY) return "[" + std::to_string(got.count) + "]";
        if (got.kind == TSUCHI_VALUE_NUMBER) std::snprintf(buffer, sizeof buffer, "%g", got.as.number);
        else if (got.kind != TSUCHI_VALUE_TEXT || sheet.get_text(mod, of, row.c_str(), path, buffer, sizeof buffer, &bytes) != TSUCHI_OK) return std::string("?");
        return std::string(buffer);
    };
    const auto cells = [&](uint64_t table, const std::string& row, std::initializer_list<const char*> paths) {
        std::string all;
        for (const char* path : paths) all += (all.empty() ? "" : " ") + cell(table, row, path);
        return all;
    };
    const auto pictures_of = [&](const std::string& area) { return cells(slides, area, {"ImageTextureList", "ImageTextureList[0]"}); };
    const auto types_of = [&](const std::string& area) {
        return cells(types, area, {"AreaType", "TargetPikminColor", "TargetPikminColor[0]", "TargetPikminColor[1]", "DifficultList", "DifficultList[0]", "DifficultList[2]"});
    };
    const auto totals_of = [&](const std::string& level) { return cells(totals, level, {"AreaName", "GroundOtakara", "SURVIVOR", "Wasurena"}); };
    const auto radar_of = [&](const std::string& area) {
        return cells(radars, area, {"Texture", "PositionOffset.X", "PositionOffset.Y", "DefaultAngle", "BaseScale", "CaptureSize", "CursorMoveLimit.Left", "CursorMoveLimit.Top",
                                    "CursorMoveLimit.Right", "CursorMoveLimit.Bottom"});
    };
    // The scene a row of the radar's table is for, which is a name in it; "" for a row that is for none.
    const auto radar_scene = [&](const std::u16string& area) {
        const uint64_t row = row_at(radars, area);
        return !row ? std::string("<none>") : fake::get<uint64_t>(row) ? text_of_name(fake::get<uint64_t>(row)) : std::string();
    };
    const auto note_of = [&](const std::string& row) { return cell(notes, row, "AreaBitFlags"); };
    const std::string empty_types = "255 [0] <none> <none> [3] 0 0", first_types_are = "0 [2] 0 1 [3] 1 3", first_radar_is = "/G/Map.Map 0 0 -90 0 12000 0 0 0 0";

    // The game has the tables from here on. A world begins: each of the four running maps gets its rows, which are
    // the framework's own and empty, and three messages that are for its area.
    shelve(u"/G/Slides.Slides", slides), shelve(u"/G/Types.Types", types), shelve(u"/G/Totals.Totals", totals), shelve(u"/G/Radar.Radar", radars), shelve(u"/G/Notes.Notes", notes);
    const uint64_t meadow = fake::object(u"Meadow", w.component_class, 0x40);
    fake::put(meadow + t::world::level_name, fake::name(u"Area001_P")), fake::put(main + t::data::world, meadow);
    steps(1);
    const tsuchi::NewMapCounts rowed = tsuchi::new_map_counts();
    CHECK(tsuchi::new_map_count() == 4 && rowed.ui_rows_made == bare.ui_rows_made + 4 * 5 && rowed.messages_set == bare.messages_set + 4 * 3 && rowed.ui_rows_failed == bare.ui_rows_failed);
    CHECK(pictures_of("Area005") == "[0] <none>" && pictures_of("Area009") == "[0] <none>" && pictures_of("Area001") == "[1] /G/Pic.Pic" && pictures_of("Area006") == "<none> <none>");
    CHECK(types_of("Area005") == empty_types && types_of("Area007") == empty_types && types_of("Area001") == first_types_are);
    CHECK(note_of("Tsuchi_Area005_Day0") == "32" && note_of("Tsuchi_Area005_Day2") == "32" && note_of("Tsuchi_Area007_Day1") == "128" && note_of("Tsuchi_Area009_Day2") == "512" &&
          note_of("Note_More") == "1118" && note_of("Tsuchi_Area005_Day3") == "<none>" && note_of("Tsuchi_Area006_Day0") == "<none>");
    CHECK(radar_scene(u"Area005") == "<none>" && totals_of("Area005_P") == "Area005_P 9 9 9"); // a map that says nothing of a radar or of totals leaves those tables alone
    // The mods stop: the rows stay, and the messages are for no area.
    tsuchi::forget_mods();
    CHECK(note_of("Tsuchi_Area005_Day0") == "0" && note_of("Tsuchi_Area007_Day2") == "0" && pictures_of("Area005") == "[0] <none>" && types_of("Area007") == empty_types);

    // A map that says everything, and one that says only where its radar's picture is.
    for (const std::string& id : made) absent(id);
    disk.files.erase(record_at);
    restart_records(record_at);
    declare("nm.shown", R"({"one": {"name": "Shown", "builds": [{"world": "/A/Area005_P", "scene": "/A/SD_Area005"}], "preview": "/A/T.T",
             "radar": {"texture": "/A/R.R", "offset": [100, -200], "size": 8000, "angle": 45, "scale": 2.5, "limits": [-10, 20, 30, -40]},
             "pikmin": ["red", "ice"], "totals": {"treasures": 3, "flarlic": 1}}})");
    declare("nm.radar", R"({"one": {"name": "Radar", "builds": [{"world": "/B/Area007_P", "scene": "/B/SD_Area007"}], "radar": {"texture": "/A/M.M"}, "pikmin": []}})");
    const unsigned changes_before = tsuchi::table_changes(), lengths_before = tsuchi::table_lengths();
    run();
    const tsuchi::NewMapCounts told = tsuchi::new_map_counts();
    CHECK(problem("nm.shown") == "<running>" && problem("nm.radar") == "<running>" && told.previews_set == rowed.previews_set + 1 && told.radars_set == rowed.radars_set + 2 &&
          told.pikmin_set == rowed.pikmin_set + 1 && told.totals_set == rowed.totals_set + 1 && told.messages_set == rowed.messages_set + 6 && told.ui_rows_made == rowed.ui_rows_made + 2);
    CHECK(pictures_of("Area005") == "[1] /A/T.T" && pictures_of("Area007") == "[0] <none>" && pictures_of("Area001") == "[1] /G/Pic.Pic");
    CHECK(types_of("Area005") == "255 [2] 0 7 [3] 0 0" && types_of("Area007") == empty_types && types_of("Area001") == first_types_are);
    CHECK(totals_of("Area005_P") == "Area005_P 3 0 1" && totals_of("Area007_P") == "Area007_P 9 9 9");
    CHECK(radar_scene(u"Area005") == "Area005" && radar_of("Area005") == "/A/R.R 100 -200 45 2.5 8000 -10 20 30 -40");
    CHECK(radar_scene(u"Area007") == "Area007" && radar_of("Area007") == "/A/M.M 0 0 -90 3.75 12000 -6000 6000 6000 -6000"); // what a radar is given where the manifest says nothing
    CHECK(radar_scene(u"Area001") == "Area001" && radar_of("Area001") == first_radar_is);
    CHECK(note_of("Tsuchi_Area005_Day1") == "32" && note_of("Tsuchi_Area007_Day1") == "128" && object_kept(preview_picture) && object_kept(radar_picture));
    // The list is opened: each new area's row of types is told its place in the list, which is where the game
    // looks the types up, and is for no place again when the list ends.
    // The menu's small map: a frame that draws a picture, and in a group of its own the game's map of the world
    // with its marks. And its three counts, each a text.
    const uint64_t image_type = fake::object(u"Image", w.root), slot_type = fake::object(u"PanelSlot", w.root), count_type = fake::object(u"CarrotRichTextBlock", w.root);
    const uint64_t brush_type = fake::memory(0x80), world_picture = fake::object(u"T_World", texture_type, 0x40);
    constexpr uint64_t kSlot = 0x40, kShown = 0x48, kBrush = 0x50, kFrameAt = 0xA0, kWorldMapAt = 0xA8, kCountsAt = 0xB0;
    fake::put<uint64_t>(brush_type, fake::engine().vtable);
    fake::set_super(image_type, widget_type), fake::set_super(count_type, widget_type);
    fake::field(brush_type, u"ResourceObject", u"ObjectProperty", 0, 8);
    fake::field(widget_type, u"Slot", u"ObjectProperty", int32_t(kSlot), 8), fake::field(widget_type, u"Visibility", u"ByteProperty", int32_t(kShown), 1);
    fake::field(slot_type, u"Parent", u"ObjectProperty", int32_t(kSlot), 8);
    fake::field(image_type, u"Brush", u"StructProperty", int32_t(kBrush), 8, brush_type);
    fake::field(count_type, u"Text", u"TextProperty", int32_t(kCountText), int32_t(t::text_value::size));
    fake::field(menu_type, u"IM_Base_MapFrame", u"ObjectProperty", int32_t(kFrameAt), 8), fake::field(menu_type, u"IM_WorldMap", u"ObjectProperty", int32_t(kWorldMapAt), 8);
    fake::field(menu_type, u"F_Info_Rate_000", u"ObjectProperty", int32_t(kCountsAt), 8), fake::field(menu_type, u"F_Info_Rate_001", u"ObjectProperty", int32_t(kCountsAt) + 8, 8);
    fake::field(menu_type, u"F_Info_Rate_002", u"ObjectProperty", int32_t(kCountsAt) + 16, 8);
    const uint64_t set_picture = fake::function(image_type, function_type, u"SetBrushResourceObject", 8, 8, [&](uint64_t object, uint64_t arguments) {
        fake::put(object + kBrush, fake::get<uint64_t>(arguments));
        atlas.log += "picture ";
    });
    const uint64_t set_shown = fake::function(widget_type, function_type, u"SetVisibility", 1, 1, [&](uint64_t object, uint64_t arguments) {
        fake::put(object + kShown, fake::get<uint8_t>(arguments));
        atlas.log += "shown" + std::to_string(fake::get<uint8_t>(arguments)) + " ";
    });
    fake::flag(fake::field(set_picture, u"ResourceObject", u"ObjectProperty", 0, 8), kArgument), fake::flag(fake::field(set_shown, u"InVisibility", u"ByteProperty", 0, 1), kArgument);
    struct SmallMap {
        uint64_t frame, group, counts[3];
    };
    const auto small_map_in = [&](uint64_t menu) {
        SmallMap parts{fake::engine_object(u"Frame", image_type), fake::engine_object(u"N_Cvs_Map_Content", widget_type), {}};
        const uint64_t world_map = fake::engine_object(u"WorldMap", image_type), slot = fake::engine_object(u"MapSlot", slot_type);
        fake::put(world_map + kSlot, slot), fake::put(slot + kSlot, parts.group), fake::put(parts.frame + kBrush, world_picture);
        fake::put(menu + kFrameAt, parts.frame), fake::put(menu + kWorldMapAt, world_map);
        for (int i = 0; i < 3; ++i) {
            parts.counts[i] = fake::engine_object(u"Count", count_type);
            put_count(parts.counts[i], u"0/???");
            fake::put(menu + kCountsAt + 8 * uint64_t(i), parts.counts[i]);
        }
        return parts;
    };
    const auto drawn_in = [&](const SmallMap& parts) { return fake::get<uint64_t>(parts.frame + kBrush); };
    const auto shown_as = [&](uint64_t widget) { return int(fake::get<uint8_t>(widget + kShown)); };
    const auto count_in = [&](uint64_t block) {
        const std::u16string text = string_of(reinterpret_cast<const void*>(fake::get<uint64_t>(block + kCountText)));
        return std::string(text.begin(), text.end());
    };
    const auto on_row = [&](uint64_t menu, int32_t row, int frames = 1) {
        fake::put<int32_t>(menu + t::explore_menu::selected, row);
        steps(frames);
    };
    const auto old_count_setter = t::fn::rich_text_set;
    const auto old_display = t::fn::text_display_string;
    t::fn::rich_text_set = game_count_set, t::fn::text_display_string = game_text_display_string;
    const int hidden = int(t::constant::widget_hidden), collapsed = int(t::constant::widget_collapsed);

    const uint64_t listing = menu_of();
    const SmallMap small = small_map_in(listing);
    game_opens(listing);
    CHECK(log_is("rows:1,2,3,10,4,6,7,5 label6=Radar label7=Shown pivot:0.5,0 scale:0.75,0.75 ")); // in the order the maps were given their numbers
    CHECK(cell(types, "Area007", "AreaType") == "6" && cell(types, "Area005", "AreaType") == "7" && cell(types, "Area001", "AreaType") == "0" &&
          tsuchi::new_map_counts().places_set == told.places_set + 2);
    // The player is on a row of the game's own: the small map and the counts are the game's.
    on_row(listing, 2);
    CHECK(log_is("") && drawn_in(small) == world_picture && shown_as(small.group) == 0 && count_in(small.counts[0]) == "0/???");
    // On the row of a map with a radar, the frame draws that radar's picture and the game's own map with its marks is
    // hidden. What the frame drew is kept for it. Declared totals fill only the game's unknown display totals;
    // omitted kinds beside the others mean zero. The native numerator and progress records stay the game's.
    on_row(listing, 7);
    CHECK(log_is("picture shown" + std::to_string(hidden) + " count count count ") && drawn_in(small) == radar_picture && shown_as(small.group) == hidden && object_kept(world_picture) &&
          tsuchi::new_map_counts().insets_shown == told.insets_shown + 1 && tsuchi::new_map_counts().counts_written == told.counts_written + 3);
    CHECK(count_in(small.counts[0]) == "0/0" && count_in(small.counts[1]) == "0/3" && count_in(small.counts[2]) == "0/1");
    steps(9);
    CHECK(log_is("")); // already projected: no duplicate setter calls
    // The game writes the count again, with marks of its own in it: they stay. A count that shows something found, or
    // a total, is left alone.
    put_count(small.counts[0], u"0[Slash w=\"1\"/]/[Tongue/][Hid/]???"), put_count(small.counts[1], u"0/7");
    steps(9);
    CHECK(log_is("count ") && count_in(small.counts[0]) == "0[Slash w=\"1\"/]/[Tongue/][Hid/]0" && count_in(small.counts[1]) == "0/7");
    for (const char16_t* other : {u"1/???", u"0/????", u"0/[open???", u"0/??? ", u"10/???", u"0/???[Late/]"}) {
        put_count(small.counts[0], other);
        steps(9);
        CHECK(log_is("") && count_in(small.counts[0]) == std::string(other, other + std::char_traits<char16_t>::length(other)));
    }
    // Positive totals retain collected values and every native text mark. A contradictory native numerator
    // cannot be presented as found out of a smaller declaration, and malformed/known totals stay native.
    for (const auto& sample : {std::pair<const char16_t*, const char*>{u"1/???", "1/3"},
                             {u"3[Slash/]/[Hid/]???", "3[Slash/]/[Hid/]3"}}) {
        put_count(small.counts[1], sample.first); steps(9);
        CHECK(log_is("count ") && count_in(small.counts[1]) == sample.second);
        steps(9); CHECK(log_is(""));
    }
    for (const char16_t* other : {u"4/???", u"10001/???", u"0/????", u"0/[open???", u"0/??? ", u"0/???[Late/]", u"0/7", u"/???"}) {
        put_count(small.counts[1], other); steps(9);
        CHECK(log_is("") && count_in(small.counts[1]) == std::string(other, other + std::char_traits<char16_t>::length(other)));
    }
    // The other map's row: its own picture, after the game's was put back. It gives no totals, so the counts are the game's.
    put_count(small.counts[0], u"0/???");
    on_row(listing, 6);
    CHECK(log_is("picture shown0 picture shown" + std::to_string(hidden) + " ") && drawn_in(small) == other_radar && shown_as(small.group) == hidden && count_in(small.counts[0]) == "0/???" &&
          tsuchi::new_map_counts().insets_shown == told.insets_shown + 2 && tsuchi::new_map_counts().insets_put_back == told.insets_put_back + 1);
    // The game hides the frame, as it does where the player has not been: everything is the game's again, and stays so.
    fake::put<uint8_t>(small.frame + kShown, uint8_t(collapsed));
    steps(9);
    CHECK(log_is("picture shown0 ") && drawn_in(small) == world_picture && shown_as(small.group) == 0 && !object_kept(world_picture));
    steps(9);
    CHECK(log_is(""));
    fake::put<uint8_t>(small.frame + kShown, 0);
    steps(9);
    CHECK(log_is("picture shown" + std::to_string(hidden) + " ") && drawn_in(small) == other_radar);
    // The game shows its own map again by itself, or draws another picture in the frame: neither is undone.
    fake::put<uint8_t>(small.group + kShown, 0), fake::put(small.frame + kBrush, preview_picture);
    on_row(listing, 0);
    CHECK(log_is("") && drawn_in(small) == preview_picture && shown_as(small.group) == 0 && !object_kept(world_picture));
    fake::put(small.frame + kBrush, world_picture);
    // The menu ends while a radar is drawn: the game has its own back first.
    on_row(listing, 7);
    CHECK(log_is("picture shown" + std::to_string(hidden) + " count ") && drawn_in(small) == radar_picture && count_in(small.counts[0]) == "0/0");
    game_ends(listing);
    CHECK(log_is("picture shown0 pivot:0.5,0 scale:1,1 end ") && drawn_in(small) == world_picture && shown_as(small.group) == 0);
    CHECK(cell(types, "Area005", "AreaType") == "255" && cell(types, "Area007", "AreaType") == "255" && list_is_the_games());
    // A menu whose frame is inside the group that would be hidden, and one whose group is another: left alone.
    const uint64_t nested = menu_of(), renamed = menu_of();
    const SmallMap inside = small_map_in(nested), other_group = small_map_in(renamed);
    const uint64_t frame_slot = fake::engine_object(u"FrameSlot", slot_type);
    fake::put(inside.frame + kSlot, frame_slot), fake::put(frame_slot + kSlot, inside.group);
    fake::put(other_group.group + t::object::name, fake::name(u"N_Cvs_Other"));
    game_opens(nested), on_row(nested, 7), game_ends(nested), game_opens(renamed), on_row(renamed, 7), game_ends(renamed);
    CHECK(atlas.log.find("picture") == std::string::npos && drawn_in(inside) == world_picture && drawn_in(other_group) == world_picture &&
          tsuchi::new_map_counts().counts_written == told.counts_written + 13); // the counts do not wait on the small map
    atlas.log.clear();
    // A map's mod stops while its radar is drawn: the game has its own back.
    const uint64_t late = menu_of();
    const SmallMap last = small_map_in(late);
    game_opens(late), on_row(late, 7);
    CHECK(drawn_in(last) == radar_picture && shown_as(last.group) == hidden);
    steps(1); // the record says that a save may hold progress in both
    atlas.log.clear();
    // The mods stop: every row is as the framework made it, and nothing of the mods' is held.
    tsuchi::forget_mods();
    CHECK(drawn_in(last) == world_picture && shown_as(last.group) == 0 && !object_kept(world_picture));
    game_ends(late);
    atlas.log.clear();
    CHECK(pictures_of("Area005") == "[0] <none>" && types_of("Area005") == empty_types && totals_of("Area005_P") == "Area005_P 9 9 9" && note_of("Tsuchi_Area005_Day1") == "0");
    CHECK(radar_scene(u"Area005").empty() && radar_scene(u"Area007").empty() && radar_of("Area005") == first_radar_is && !object_kept(preview_picture) && !object_kept(radar_picture) && !object_kept(other_radar));
    // Started again in the same run, they find their rows and use them.
    run();
    CHECK(pictures_of("Area005") == "[1] /A/T.T" && types_of("Area005") == "255 [2] 0 7 [3] 0 0" && radar_scene(u"Area007") == "Area007" && totals_of("Area005_P") == "Area005_P 3 0 1" &&
          tsuchi::new_map_counts().ui_rows_made == told.ui_rows_made && tsuchi::new_map_counts().ui_rows_failed == told.ui_rows_failed);
    tsuchi::forget_mods();
    // What is left is the framework's own: two rows of the radar's table that name no scene, and one area's name in the table of totals.
    CHECK(tsuchi::table_changes() == changes_before + 3 && tsuchi::table_lengths() == lengths_before);

    // What the game will not take blocks the mod as it starts, and leaves the rows as the framework made them.
    absent("nm.shown"), absent("nm.radar");
    disk.files.erase(record_at);
    restart_records(record_at);
    const std::string five = R"("builds": [{"world": "/A/Area005_P", "scene": "/A/SD_Area005"}])";
    declare("nm.lost", "{\"one\": {\"name\": \"N\", " + five + ", \"pikmin\": [\"blue\"], \"preview\": \"/A/No.No\"}}");
    run();
    CHECK(problem("nm.lost") == "mod.json: \"new_maps\": one: its preview could not be loaded: /A/No.No" && types_of("Area005") == empty_types && pictures_of("Area005") == "[0] <none>");
    declare("nm.lost", "{\"one\": {\"name\": \"N\", " + five + ", \"preview\": \"/A/N.N\"}}");
    run();
    CHECK(problem("nm.lost") == "mod.json: \"new_maps\": one: its preview is no picture the list of areas can show: /A/N.N" && pictures_of("Area005") == "[0] <none>");
    declare("nm.lost", "{\"one\": {\"name\": \"N\", " + five + ", \"preview\": \"/A/T.T\", \"radar\": {\"texture\": \"/A/N.N\"}}}");
    run();
    CHECK(problem("nm.lost") == "mod.json: \"new_maps\": one: its radar's picture is no picture the radar can draw: /A/N.N" && pictures_of("Area005") == "[0] <none>" &&
          radar_scene(u"Area005").empty() && !object_kept(preview_picture));
    // A type of Pikmin the list has no icon for, one nobody has named, and one no mod has added.
    for (const char* type : {"glow", "#8", "nm.lost:pink"}) {
        declare("nm.lost", "{\"one\": {\"name\": \"N\", " + five + ", \"pikmin\": [\"red\", \"" + type + "\"]}}");
        run();
        CHECK(problem("nm.lost") == "mod.json: \"new_maps\": one: \"pikmin\": " + std::string(type) + " is no type of Pikmin the list of areas shows" && types_of("Area005") == empty_types);
    }
    // A game whose table of totals has no row for the area: one is not added, since the game reads the rows by their place.
    const uint64_t nine_totals = row_at(totals, u"Area009_P");
    sheets.rows.erase({totals, row_name(u"Area009_P")});
    declare("nm.lost", R"({"one": {"name": "N", "builds": [{"world": "/A/Area009_P", "scene": "/A/SD_Area009"}], "totals": {}}})");
    run();
    CHECK(problem("nm.lost") == "mod.json: \"new_maps\": one: the game's table of areas has no row for area 9" && !row_at(totals, u"Area009_P"));
    sheets.rows[{totals, row_name(u"Area009_P")}] = nine_totals;
    run();
    CHECK(problem("nm.lost") == "<running>" && totals_of("Area009_P") == "Area009_P 0 0 0");


    // A blocked mod whose first scene was already added leaves that entry reusable by the next mod.
    absent("nm.lost");
    disk.files.erase(record_at);
    restart_records(record_at);
    declare("nm.a", R"({"one":{"name":"First","builds":[{"world":"/A/Area005_P","scene":"/A/SD_Area005"}]},"two":{"name":"Bad","builds":[{"world":"/L/Area007_P","scene":"/L/SD_Area007"}]}})");
    one_map("nm.b",'B',{5});
    run();
    CHECK(problem("nm.a").find("could not be loaded")!=std::string::npos && problem("nm.b")=="<running>");
    CHECK(level_of("nm.b:one")=="Area005_P" && scene_count()==3 && scene_at(2)==fives['B']);
    CHECK(object_kept(fives['B']));
    // An incomplete base row is repaired by its destination, not mistaken for a completed row.
    const uint64_t incomplete=sheets.rows[{bases,row_name(u"Area005_00")}];
    fake::put(incomplete+0x8,row_name(u"Area001_P"));
    const auto row_counts=tsuchi::new_map_counts();
    run();
    CHECK(base_row(u"Area005_00")=="Area005_P/1" && tsuchi::new_map_counts().base_rows_made>row_counts.base_rows_made);
    // A refused durable allocation cannot make another map use a new number.
    tsuchi::forget_mods();
    absent("nm.a"),absent("nm.b");
    disk.files.erase(record_at);
    restart_records(record_at);
    one_map("nm.a",'A',{5});
    disk.refuse="write";
    run();
    CHECK(problem("nm.a").find("could not be kept")!=std::string::npos && level_of("nm.a:one")=="<not found>");
    disk.refuse.clear();
    run();
    CHECK(problem("nm.a")=="<running>" && level_of("nm.a:one")=="Area005_P");
    // A failed override and a refused create keep only their day rows pending. Completed rows, the preview,
    // and row allocations are left alone when the same world retries.
    tsuchi::forget_mods();
    absent("nm.a");
    declare("nm.retry", R"({"one":{"name":"Retry","builds":[{"world":"/A/Area005_P","scene":"/A/SD_Area005"}],"preview":"/A/T.T"}})");
    const size_t rows_before_retry = sheets.rows.size();
    sheets.rows.erase({notes, row_name(u"Tsuchi_Area005_Day1")});
    atlas.find_before = t::fn::find_table_row, atlas.add_before = t::fn::add_table_row;
    t::fn::find_table_row = retry_find_row, t::fn::add_table_row = retry_add_row;
    fake::put(table_functions+t::data_table_table::add_row, reinterpret_cast<uint64_t>(&retry_add_row));
    atlas.hidden_table = notes, atlas.hidden_row = row_name(u"Tsuchi_Area005_Day0"), atlas.hide_at = 2, atlas.found = 0;
    atlas.refused_table = notes, atlas.refused_row = row_name(u"Tsuchi_Area005_Day1");
    atlas.tracked_table = notes, atlas.tracked_row = row_name(u"Tsuchi_Area005_Day2"), atlas.tracked = 0;
    const auto retry_before = tsuchi::new_map_counts();
    const int adds_before_retry = sheets.added;
    run();
    CHECK(problem("nm.retry")=="<running>" && level_of("nm.retry:one")=="Area005_P");
    CHECK(tsuchi::new_map_counts().messages_set==retry_before.messages_set+1 && tsuchi::new_map_counts().previews_set==retry_before.previews_set+1 && atlas.refused==1);
    const int finished_row_reads = atlas.tracked;
    atlas.log.clear();
    const uint64_t pending_menu = menu_of();
    game_opens(pending_menu), game_ends(pending_menu);
    CHECK(log_is("rows:1,2,3,10,4,6 end ") && list_is_the_games());
    atlas.refused_table = 0, atlas.hide_at = 0;
    steps(31);
    CHECK(tsuchi::new_map_counts().messages_set==retry_before.messages_set+3 && tsuchi::new_map_counts().previews_set==retry_before.previews_set+1);
    CHECK(tsuchi::new_map_counts().ui_rows_made==retry_before.ui_rows_made+1 && sheets.added==adds_before_retry+2 && sheets.rows.size()==rows_before_retry && atlas.tracked==finished_row_reads);
    CHECK(note_of("Tsuchi_Area005_Day0")=="32" && note_of("Tsuchi_Area005_Day1")=="32" && note_of("Tsuchi_Area005_Day2")=="32");
    atlas.tracked_table = 0;
    // Refusing the second AreaType assignment rolls back the first and leaves the game's list unpublished.
    one_map("nm.second", 'B', {7}, "Second");
    run();
    // A saved map remains available when marking another map used cannot be written.
    CHECK(tale.travel(mod, "nm.retry:one", -1) == TSUCHI_OK);
    steps(300);
    const std::string before_refused_menu = saved_record(record_at);
    const auto partial_before = tsuchi::new_map_counts();
    const uint64_t partial_menu = menu_of();
    disk.refuse = "write";
    game_opens(partial_menu);
    const uint64_t partial_list = fake::get<uint64_t>(fake::main_image() + t::data::explore_areas + t::array::data);
    CHECK(!list_is_the_games() && fake::get<int32_t>(fake::main_image() + t::data::explore_areas + t::array::count) == 7 &&
          fake::get<int32_t>(partial_list + 6 * 4) == 5);
    CHECK(tsuchi::new_map_counts().menus_listed == partial_before.menus_listed + 1 && tsuchi::new_map_counts().places_set == partial_before.places_set + 1);
    CHECK(types_of("Area005") == "6 [0] <none> <none> [3] 0 0" && types_of("Area007") == empty_types && saved_record(record_at) == before_refused_menu);
    game_ends(partial_menu);
    CHECK(list_is_the_games());
    disk.refuse.clear();
    const uint64_t after_write = menu_of();
    game_opens(after_write);
    CHECK(!list_is_the_games() && fake::get<int32_t>(fake::main_image() + t::data::explore_areas + t::array::count) == 8);
    CHECK(types_of("Area005") == "6 [0] <none> <none> [3] 0 0" && types_of("Area007") == "7 [0] <none> <none> [3] 0 0");
    game_ends(after_write);
    atlas.log.clear();
    const auto place_before = tsuchi::new_map_counts();
    atlas.hidden_table = types, atlas.hidden_row = row_name(u"Area007"), atlas.hide_at = 1, atlas.found = 0;
    const uint64_t failed_menu = menu_of();
    game_opens(failed_menu);
    atlas.hide_at = 0;
    CHECK(list_is_the_games() && tsuchi::new_map_counts().places_set==place_before.places_set && tsuchi::new_map_counts().menus_listed==place_before.menus_listed);
    CHECK(types_of("Area005")==empty_types && types_of("Area007")==empty_types);
    game_ends(failed_menu);
    atlas.log.clear();
    const uint64_t retried_menu = menu_of();
    game_opens(retried_menu);
    CHECK(!list_is_the_games() && tsuchi::new_map_counts().places_set==place_before.places_set+2 && tsuchi::new_map_counts().menus_listed==place_before.menus_listed+1);
    CHECK(types_of("Area005")=="6 [0] <none> <none> [3] 0 0" && types_of("Area007")=="7 [0] <none> <none> [3] 0 0");
    game_ends(retried_menu);
    CHECK(list_is_the_games() && types_of("Area005")==empty_types && types_of("Area007")==empty_types);
    tsuchi::forget_mods();
    CHECK(note_of("Tsuchi_Area005_Day0")=="0" && note_of("Tsuchi_Area005_Day1")=="0" && note_of("Tsuchi_Area005_Day2")=="0" && pictures_of("Area005")=="[0] <none>");
    t::fn::find_table_row = atlas.find_before, t::fn::add_table_row = atlas.add_before;
    fake::put(table_functions+t::data_table_table::add_row, reinterpret_cast<uint64_t>(atlas.add_before));
    atlas.hidden_table = atlas.refused_table = atlas.tracked_table = 0;
    // What comes after finds the game and the mods as these checks found them.
    tsuchi::forget_mods();
    for (const std::string& id : made) absent(id);
    disk.files.erase(record_at);
    restart_records(record_at);
    fake::put<int32_t>(scenes_at + t::array::count, 2);
    fake::put(main + t::data::world, old_world), fake::put<uint8_t>(w.framework + t::game_framework::world_begun, old_begun);
    fake::put(flow_root + f::child, old_top), fake::put(context + t::game_context::requests, old_requests), fake::put(main + t::data::game_mode, old_mode);
    fake::put(w.framework + t::game_framework::clock, old_clock), fake::put<uint8_t>(context + t::game_context::rule, old_rule);
    fake::put<uint32_t>(running.pauses + t::pause_manager::active, old_paused);
    fake::put(running.instance + t::game_instance::scene_manager, old_keeper), fake::put(running.instance + t::game_instance::story_save, old_story);
    fake::put(main + t::data::game_instance, old_instance), fake::put(main + t::data::game_context, old_context);
    t::fn::load_soft_object = old_loader, t::fn::text_from_string = old_text_maker, t::fn::travel_request = old_request_maker;
    t::fn::story_flags = nullptr, t::fn::story_find_area = nullptr, t::fn::story_add_area = nullptr, t::fn::story_link_area = nullptr;
    t::fn::explore_row = nullptr, t::fn::explore_row_label = nullptr, t::fn::scene_array_grow = nullptr;
    t::fn::rich_text_set = old_count_setter, t::fn::text_display_string = old_display;
    tsuchi::forget_records();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    logged.lines.clear();
    disk.log.clear(), disk.changes.clear();
    atlas.log.clear();
}
