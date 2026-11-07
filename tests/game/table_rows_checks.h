// Checks of new rows and array lengths that a manifest declares, against the made-up game. Included inside the
// manifest checks of objects_test.cpp, after the block that checks changes to fields, and uses what that function has.
{
    namespace a = t::array;
    const TSUCHI_DatatableService& sheet = *tsuchi::host.datatable;
    // A table whose rows have a number, a list of numbers, a list of strings and a list of structs.
    constexpr int32_t kNumbers = 0x8, kWords = 0x18, kPoints = 0x28;
    const uint64_t row_type = fake::engine_object(u"ListedRow", w.root), point = fake::memory(0x80);
    fake::put<uint64_t>(point, fake::engine().vtable);
    fake::field(point, u"X", u"FloatProperty", 0, 4);
    fake::field(row_type, u"Kind", u"ByteProperty", 0x0, 1);
    fake::field(row_type, u"Numbers", u"ArrayProperty", kNumbers, 0x10, fake::loose_field(u"IntProperty", 4));
    fake::field(row_type, u"Words", u"ArrayProperty", kWords, 0x10, fake::loose_field(u"StrProperty", 0x10));
    fake::field(row_type, u"Points", u"ArrayProperty", kPoints, 0x10, fake::loose_field(u"StructProperty", 4, point));
    const uint64_t lists = fake::object(u"Listed", fake::get<uint64_t>(w.creature_table + t::object::class_), 0x60), first = fake::memory(kRowBytes);
    fake::put(lists, fake::get<uint64_t>(w.creature_table)), fake::put(lists + t::data_table::row_struct, row_type);
    sheets.rows[{lists, row_name(u"FIRST")}] = first;
    const auto engine_block = [](size_t bytes) {
        void* block = tsuchi::game::engine_memory(bytes);
        std::memset(block, 0, bytes);
        return reinterpret_cast<uint64_t>(block);
    };
    const uint64_t numbers = engine_block(12), words = engine_block(0x10), word = engine_block(8);
    for (int i = 0; i < 3; ++i) fake::put<int32_t>(numbers + 4 * uint64_t(i), 10 * (i + 1));
    std::memcpy(reinterpret_cast<void*>(word), u"One", 8);
    fake::put(words + a::data, word), fake::put<int32_t>(words + a::count, 4), fake::put<int32_t>(words + a::max, 4);
    const auto list_in = [&](uint64_t row, int32_t at, uint64_t data, int32_t count) {
        fake::put(row + uint64_t(at) + a::data, data), fake::put(row + uint64_t(at) + a::count, count), fake::put(row + uint64_t(at) + a::max, count);
    };
    list_in(first, kNumbers, numbers, 3), list_in(first, kWords, words, 1);
    fake::put<uint8_t>(first, 2);
    const std::u16string lists_path = u"/G/Listed.Listed", beasts_path = u"/G/Beasts.Beasts", other_path = u"/G/NotTable.NotTable";
    const uint64_t lists_key = fake::engine_name(const_cast<char16_t*>(lists_path.data()), lists_path.size(), 1);
    const uint64_t beasts_key = fake::engine_name(const_cast<char16_t*>(beasts_path.data()), beasts_path.size(), 1);
    const uint64_t other_key = fake::engine_name(const_cast<char16_t*>(other_path.data()), other_path.size(), 1);
    const auto beasts_before = shelf.assets.find(beasts_key);
    const uint64_t beasts_were = beasts_before == shelf.assets.end() ? 0 : beasts_before->second;
    shelf.assets[lists_key] = lists;
    shelf.assets[other_key] = w.idol_asset;
    shelf.assets[beasts_key] = w.creature_table; // the path of the game's table of creatures, in the made-up game
    const TSUCHI_Object lists_of{to_handle(lists)};

    const auto row_of = [&](const char16_t* name) {
        const auto found = sheets.rows.find({lists, row_name(name)});
        return found == sheets.rows.end() ? uint64_t(0) : found->second;
    };
    const auto whole = [&](const char* row, const char* path) {
        TSUCHI_Value got{};
        return sheet.get(mod, lists_of, row, path, &got) == TSUCHI_OK && got.kind == TSUCHI_VALUE_INTEGER ? int(got.as.integer) : -1;
    };
    const auto text_in = [&](const char* row, const char* path) {
        char buffer[64] = "<failed>";
        uint64_t bytes = 0;
        sheet.get_text(mod, lists_of, row, path, buffer, sizeof buffer, &bytes);
        return std::string(buffer);
    };
    const auto count_of = [&](uint64_t row, int32_t at) { return row ? fake::get<int32_t>(row + uint64_t(at) + a::count) : -1; };
    const auto data_of = [&](uint64_t row, int32_t at) { return row ? fake::get<uint64_t>(row + uint64_t(at) + a::data) : 0; };
    const unsigned changes = tsuchi::table_changes(), lengths = tsuchi::table_lengths();
    const int added_before = sheets.added;
    const auto stop = [](const char* id) {
        tsuchi::ModRecord* record = tsuchi::find_mod(id);
        if (record && record->context) tsuchi::mod_stop(record->context);
    };

    // A new row is a copy of the row named by "from", then its fields are set. A length is set before the fields of
    // its row, so the new elements are there to change, and a shorter list keeps its first elements.
    const std::string rows_one = R"(, "tables": {"/G/Listed.Listed": {)"
                                 R"("rows.one:big": {"Numbers[4]": 50, "from": "FIRST", "Kind": 7, "Numbers": {"length": 5}},)"
                                 R"("FIRST": {"Numbers": {"length": 2}, "Words": {"length": 2}, "Words[1]": "Two"}}})";
    plain("rows.one", "1.0.0", rows_one);
    // Started later: its lengths are the ones the game uses.
    plain("rows.two", "1.0.0", R"(, "requires": {"rows.one": "*"}, "tables": {"/G/Listed.Listed": {)"
                               R"("FIRST": {"Numbers": {"length": 4}}, "rows.one:big": {"Numbers": {"length": 1}}}})");
    plain("rows.nodonor", "1.0.0", R"(, "tables": {"/G/Listed.Listed": {"rows.nodonor:x": {"from": "NOBODY"}}})");
    plain("rows.empty", "1.0.0", R"(, "tables": {"/G/Listed.Listed": {"rows.empty:x": {}}})");
    plain("rows.asset", "1.0.0", R"(, "tables": {"/G/NotTable.NotTable": {"rows.asset:x": {"from": "FIRST"}}})");
    plain("rows.missingfrom", "1.0.0", R"(, "tables": {"/G/Listed.Listed": {"rows.missingfrom:x": {"Kind": 7}}})");
    plain("rows.other", "1.0.0", R"(, "tables": {"/G/Listed.Listed": {"rows.one:mine": {"from": "FIRST"}}})");
    plain("rows.taken", "1.0.0", R"(, "tables": {"/G/Listed.Listed": {"rows.taken:x": {"from": "FIRST"}}})");
    plain("rows.content", "1.0.0", R"(, "tables": {"/G/Beasts.Beasts": {"rows.content:x": {"from": "KOCHAPPY"}}})");
    plain("rows.again", "1.0.0", R"(, "tables": {"/G/Listed.Listed": {"FIRST": {"from": "FIRST"}}})");
    plain("rows.nofrom", "1.0.0", R"(, "tables": {"/G/Listed.Listed": {"rows.nofrom:x": {"from": 3}}})");
    plain("rows.badlength", "1.0.0", R"(, "tables": {"/G/Listed.Listed": {"FIRST": {"Numbers": {"length": -1}}}})");
    plain("rows.structs", "1.0.0", R"(, "tables": {"/G/Listed.Listed": {"FIRST": {"Points": {"length": 2}}}})");
    plain("rows.notlist", "1.0.0", R"(, "tables": {"/G/Listed.Listed": {"FIRST": {"Kind": {"length": 2}}}})");
    plain("rows.past", "1.0.0", R"(, "tables": {"/G/Listed.Listed": {"FIRST": {"Numbers[7]": 1}}})");
    const char* const made[] = {"rows.one", "rows.two", "rows.nodonor", "rows.other", "rows.taken", "rows.content", "rows.again",
                                "rows.nofrom", "rows.badlength", "rows.structs", "rows.notlist", "rows.past", "rows.empty", "rows.missingfrom", "rows.asset"};
    CHECK(sheet.add_row(mod, lists_of, "rows.taken:x", "FIRST") == TSUCHI_OK && sheets.added == added_before + 1); // made by code, not by the manifest
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    CHECK(problem("rows.again") == "mod.json: \"tables\": row FIRST: a new row's name is the mod's ID, a colon, and a name, such as example.mod:big_bulborb");
    CHECK(problem("rows.nofrom") == "mod.json: \"tables\": \"from\" is the name of the row that a new row copies");
    CHECK(problem("rows.badlength") == "mod.json: \"tables\": an array's length is {\"length\": n}, where n is a whole number from 0");
    tsuchi::start_mods();
    const uint64_t big = row_of(u"rows.one:big");
    CHECK(problem("rows.one") == "<running>" && problem("rows.two") == "<running>" && big && sheets.added == added_before + 2);
    CHECK(whole("rows.one:big", "Kind") == 7 && whole("FIRST", "Kind") == 2 && count_of(big, kNumbers) == 1 && whole("rows.one:big", "Numbers[0]") == 10);
    CHECK(count_of(first, kNumbers) == 4 && whole("FIRST", "Numbers[1]") == 20 && whole("FIRST", "Numbers[2]") == 30 && whole("FIRST", "Numbers[3]") == 0);
    CHECK(count_of(first, kWords) == 2 && text_in("FIRST", "Words[0]") == "One" && text_in("FIRST", "Words[1]") == "Two");
    CHECK(problem("rows.nodonor") == "mod.json: \"tables\": /G/Listed.Listed, row rows.nodonor:x, field from: the table has no row NOBODY to copy");
    CHECK(problem("rows.empty") == "mod.json: \"tables\": /G/Listed.Listed, row rows.empty:x, field from: a new row requires the name of a row to copy");
    CHECK(problem("rows.asset") == "mod.json: \"tables\": /G/NotTable.NotTable: that asset is no data table");
    CHECK(problem("rows.missingfrom") == "mod.json: \"tables\": /G/Listed.Listed, row rows.missingfrom:x, field from: a new row requires the name of a row to copy");
    CHECK(problem("rows.other") == "mod.json: \"tables\": /G/Listed.Listed, row rows.one:mine: a new row's name begins with the mod's own ID, rows.other, and a colon");
    CHECK(problem("rows.taken") ==
          "mod.json: \"tables\": /G/Listed.Listed, row rows.taken:x, field from: the table already has a row of that name, which another mod added");
    CHECK(problem("rows.content") == "mod.json: \"tables\": /G/Beasts.Beasts, row rows.content:x: a new treasure, creature or Pikmin type is declared under "
                                     "\"content\", which adds its rows to this table");
    CHECK(problem("rows.structs") == "mod.json: \"tables\": /G/Listed.Listed, row FIRST, field Points: its elements are structs, or of a kind whose array cannot change length");
    CHECK(problem("rows.notlist") == "mod.json: \"tables\": /G/Listed.Listed, row FIRST, field Kind: it is no array");
    CHECK(problem("rows.past") == "mod.json: \"tables\": /G/Listed.Listed, row FIRST, field Numbers[7]: the game has none of that name");
    CHECK(!row_of(u"rows.nodonor:x") && !row_of(u"rows.one:mine") && !row_of(u"rows.content:x") && sheets.added == added_before + 2);

    // The mod started later stops: the earlier mod's lengths are the ones the game uses again, with its changes.
    stop("rows.two");
    CHECK(count_of(big, kNumbers) == 5 && whole("rows.one:big", "Numbers[2]") == 30 && whole("rows.one:big", "Numbers[3]") == 0 && whole("rows.one:big", "Numbers[4]") == 50);
    CHECK(count_of(first, kNumbers) == 2 && whole("FIRST", "Numbers[1]") == 20 && data_of(first, kNumbers) != numbers);
    // Every mod stops: the row it added stays, as a copy of the row it was made from, and the first lists are back.
    tsuchi::forget_mods();
    CHECK(row_of(u"rows.one:big") == big && whole("rows.one:big", "Kind") == 2 && count_of(big, kNumbers) == 3 && data_of(big, kNumbers) == numbers);
    CHECK(data_of(first, kNumbers) == numbers && count_of(first, kNumbers) == 3 && data_of(first, kWords) == words && count_of(first, kWords) == 1 &&
          text_in("FIRST", "Words[0]") == "One" && fake::get<uint64_t>(words + a::data) == word);
    CHECK(tsuchi::table_changes() == changes && tsuchi::table_lengths() == lengths);
    // The mod starts again in the same run, and finds its row.
    for (const char* id : made) disk.files.erase(mods_at + id + "/mod.json");
    plain("rows.one", "1.0.0", rows_one);
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(problem("rows.one") == "<running>" && row_of(u"rows.one:big") == big && sheets.added == added_before + 2 && whole("rows.one:big", "Kind") == 7 &&
          count_of(big, kNumbers) == 5 && whole("rows.one:big", "Numbers[4]") == 50);
    tsuchi::forget_mods();
    CHECK(count_of(big, kNumbers) == 3 && whole("rows.one:big", "Kind") == 2 && tsuchi::table_changes() == changes && tsuchi::table_lengths() == lengths);
    disk.files.erase(mods_at + "rows.one/mod.json");
    shelf.assets.erase(lists_key);
    shelf.assets.erase(other_key);
    if (beasts_were) shelf.assets[beasts_key] = beasts_were;
    else shelf.assets.erase(beasts_key);
    logged.lines.clear();
}
