// Checks of a mod's say on how long an array in a row of a table is, against the made-up game. Part of
// objects_test.cpp, which includes this inside its own namespace, after everything it uses.

void array_lengths() {
    const TSUCHI_DatatableService& sheet = *tsuchi::host.datatable;
    fake::Engine& engine = fake::engine();
    namespace a = t::array;
    namespace s = t::soft_object;
    const auto object = [](uint64_t address) { return TSUCHI_Object{to_handle(address)}; };
    const auto integer = [](int64_t v) {
        TSUCHI_Value value{TSUCHI_VALUE_INTEGER, 0, {}};
        value.as.integer = v;
        return value;
    };
    const uint32_t owned = tsuchi::owned_count(mod);
    const unsigned changes = tsuchi::table_changes();

    // A table whose rows have a list of numbers, a list of references to assets by path, a list of strings, and
    // a list of structs.
    constexpr int32_t kNumbers = 0x8, kPictures = 0x18, kWords = 0x28, kPoints = 0x38;
    const uint64_t functions = fake::memory(0x40), row_type = fake::engine_object(u"ListRow", w.root), point = fake::memory(0x80);
    fake::put<uint64_t>(point, fake::engine().vtable);
    fake::field(point, u"X", u"FloatProperty", 0, 4);
    fake::field(row_type, u"Kind", u"ByteProperty", 0x0, 1);
    fake::field(row_type, u"Numbers", u"ArrayProperty", kNumbers, 0x10, fake::loose_field(u"IntProperty", 4));
    fake::field(row_type, u"Pictures", u"ArrayProperty", kPictures, 0x10, fake::loose_field(u"SoftObjectProperty", int32_t(s::size), w.component_class));
    fake::field(row_type, u"Words", u"ArrayProperty", kWords, 0x10, fake::loose_field(u"StrProperty", 0x10));
    fake::field(row_type, u"Points", u"ArrayProperty", kPoints, 0x10, fake::loose_field(u"StructProperty", 4, point));
    fake::put(functions + t::data_table_table::add_row, &game_add_table_row), fake::put(functions + t::data_table_table::remove_row, &game_remove_table_row);
    fake::put(functions + t::data_table_table::add_row_internal, &game_add_table_row_internal);
    const uint64_t lists = fake::object(u"Lists", fake::get<uint64_t>(w.creature_table + t::object::class_), 0x60), row = fake::memory(kRowBytes);
    fake::put(lists, functions), fake::put(lists + t::data_table::row_struct, row_type);
    sheets.rows[{lists, row_name(u"FIRST")}] = row;
    const TSUCHI_Object lists_of = object(lists);
    // The row's own lists, in the engine's memory as the game's are: three numbers, one picture with a path inside
    // its asset, and one word.
    const auto engine_block = [](size_t bytes) {
        void* block = tsuchi::game::engine_memory(bytes);
        std::memset(block, 0, bytes);
        return reinterpret_cast<uint64_t>(block);
    };
    const auto list_in = [&](int32_t at, uint64_t data, int32_t count) { fake::put(row + uint64_t(at) + a::data, data), fake::put(row + uint64_t(at) + a::count, count), fake::put(row + uint64_t(at) + a::max, count); };
    const uint64_t numbers = engine_block(12), pictures = engine_block(s::size), words = engine_block(0x10), inside_path = engine_block(8), word = engine_block(8);
    for (int i = 0; i < 3; ++i) fake::put<int32_t>(numbers + 4 * uint64_t(i), 10 * (i + 1));
    fake::put<int32_t>(pictures + s::index, -1), fake::put(pictures + s::path, row_name(u"/G/Old.Old"));
    fake::put(pictures + s::sub_path + a::data, inside_path), fake::put<int32_t>(pictures + s::sub_path + a::count, 4), fake::put<int32_t>(pictures + s::sub_path + a::max, 4);
    std::memcpy(reinterpret_cast<void*>(inside_path), u"Sub", 8), std::memcpy(reinterpret_cast<void*>(word), u"One", 8);
    fake::put(words + a::data, word), fake::put<int32_t>(words + a::count, 4), fake::put<int32_t>(words + a::max, 4);
    list_in(kNumbers, numbers, 3), list_in(kPictures, pictures, 1), list_in(kWords, words, 1);
    const auto count_of = [&](int32_t at) { return fake::get<int32_t>(row + uint64_t(at) + a::count); };
    const auto data_of = [&](int32_t at) { return fake::get<uint64_t>(row + uint64_t(at) + a::data); };
    const auto number_at = [&](int index) {
        TSUCHI_Value got{};
        char path[24];
        std::snprintf(path, sizeof path, "Numbers[%d]", index);
        return sheet.get(mod, lists_of, "FIRST", path, &got) == TSUCHI_OK ? int(got.as.integer) : -1;
    };
    const auto text_of = [&](const char* path) {
        char buffer[64] = "<failed>";
        uint64_t bytes = 0;
        sheet.get_text(mod, lists_of, "FIRST", path, buffer, sizeof buffer, &bytes);
        return std::string(buffer);
    };

    // A say on a list's length: the row has a list of that length, with what the first list held and new places
    // past it. The first list is set aside as it was.
    int allocated = engine.allocated, freed = engine.freed;
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 5) == TSUCHI_OK && count_of(kNumbers) == 5 && data_of(kNumbers) != numbers && number_at(0) == 10 && number_at(2) == 30 &&
          number_at(3) == 0 && number_at(4) == 0 && tsuchi::table_lengths() == 1 && tsuchi::owned_count(mod) == owned + 1 && engine.allocated == allocated + 1 && engine.freed == freed);
    CHECK(fake::get<int32_t>(numbers + 8) == 30); // the first list, untouched
    // Thought again, the same say: shorter, and to nothing.
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "numbers", 2) == TSUCHI_OK && count_of(kNumbers) == 2 && number_at(1) == 20 && number_at(2) == -1 && tsuchi::table_lengths() == 1 &&
          tsuchi::owned_count(mod) == owned + 1 && engine.freed == freed + 1);
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 0) == TSUCHI_OK && count_of(kNumbers) == 0 && data_of(kNumbers) == 0);
    // Taken back: the first list is the row's again.
    CHECK(tsuchi::table_restore_length(mod, lists_of, "FIRST", "NUMBERS") == TSUCHI_OK && data_of(kNumbers) == numbers && count_of(kNumbers) == 3 && number_at(2) == 30 &&
          tsuchi::table_lengths() == 0 && tsuchi::owned_count(mod) == owned && tsuchi::table_restore_length(mod, lists_of, "FIRST", "Numbers") == TSUCHI_NOT_FOUND);
    CHECK(engine.allocated - allocated == engine.freed - freed); // everything the framework asked the engine for went back

    // A change to an element is a change like any other, on top of the length in force, and outlasts another length.
    const TSUCHI_Value ninety = integer(90), seventy = integer(70);
    CHECK(sheet.override(mod, lists_of, "FIRST", "Numbers[1]", &ninety) == TSUCHI_OK && number_at(1) == 90 && fake::get<int32_t>(numbers + 4) == 90);
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 4) == TSUCHI_OK && number_at(1) == 90 && number_at(3) == 0 && fake::get<int32_t>(numbers + 4) == 20); // set aside as the game made it
    CHECK(sheet.override(mod, lists_of, "FIRST", "Numbers[3]", &seventy) == TSUCHI_OK && number_at(3) == 70);
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 6) == TSUCHI_OK && number_at(1) == 90 && number_at(3) == 70 && number_at(5) == 0 && number_at(0) == 10);
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 2) == TSUCHI_OK && number_at(1) == 90 && count_of(kNumbers) == 2); // the say on the fourth has nothing to change now
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 4) == TSUCHI_OK && number_at(3) == 70);
    // The change to an element is taken back while the list is another length: the first list has what it held at first.
    CHECK(sheet.restore(mod, lists_of, "FIRST", "Numbers[1]") == TSUCHI_OK && number_at(1) == 20);
    CHECK(tsuchi::table_restore_length(mod, lists_of, "FIRST", "Numbers") == TSUCHI_OK && data_of(kNumbers) == numbers && number_at(1) == 20 && number_at(3) == -1);
    CHECK(sheet.restore(mod, lists_of, "FIRST", "Numbers[3]") == TSUCHI_OK && count_of(kNumbers) == 3 && tsuchi::table_changes() == changes && tsuchi::owned_count(mod) == owned);
    // A change made first and still standing when the length is taken back is in the first list again.
    CHECK(sheet.override(mod, lists_of, "FIRST", "Numbers[0]", &ninety) == TSUCHI_OK && tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 1) == TSUCHI_OK && number_at(0) == 90 &&
          fake::get<int32_t>(numbers) == 10 && tsuchi::table_restore_length(mod, lists_of, "FIRST", "Numbers") == TSUCHI_OK && number_at(0) == 90 && fake::get<int32_t>(numbers) == 90 &&
          sheet.restore(mod, lists_of, "FIRST", "Numbers[0]") == TSUCHI_OK && fake::get<int32_t>(numbers) == 10);

    // References to assets by path and strings: an element that is copied owns a copy of its string, a new one
    // refers to nothing, and every copy goes back to the engine with its list.
    allocated = engine.allocated, freed = engine.freed;
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Pictures", 2) == TSUCHI_OK && count_of(kPictures) == 2 && text_of("Pictures[0]") == "/G/Old.Old" &&
          fake::get<uint64_t>(data_of(kPictures) + s::sub_path + a::data) != inside_path && fake::get<int32_t>(data_of(kPictures) + s::sub_path + a::count) == 4 &&
          !std::memcmp(reinterpret_cast<void*>(fake::get<uint64_t>(data_of(kPictures) + s::sub_path + a::data)), u"Sub", 8) &&
          fake::get<int32_t>(data_of(kPictures) + s::size + s::index) == -1 && fake::get<uint64_t>(data_of(kPictures) + s::size + s::path) == 0);
    CHECK(sheet.override_text(mod, lists_of, "FIRST", "Pictures[1]", "/G/New.New") == TSUCHI_OK && text_of("Pictures[1]") == "/G/New.New" &&
          sheet.override_text(mod, lists_of, "FIRST", "Pictures[0]", "/G/Mine.Mine") == TSUCHI_OK && text_of("Pictures[0]") == "/G/Mine.Mine");
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Words", 3) == TSUCHI_OK && text_of("Words[0]") == "One" && text_of("Words[2]").empty() &&
          fake::get<uint64_t>(data_of(kWords) + a::data) != word && sheet.override_text(mod, lists_of, "FIRST", "Words[2]", "Three") == TSUCHI_OK && text_of("Words[2]") == "Three");
    CHECK(sheet.restore(mod, lists_of, "FIRST", "Words[2]") == TSUCHI_OK && sheet.restore(mod, lists_of, "FIRST", "Pictures[1]") == TSUCHI_OK &&
          sheet.restore(mod, lists_of, "FIRST", "Pictures[0]") == TSUCHI_OK && text_of("Pictures[0]") == "/G/Old.Old");
    CHECK(tsuchi::table_restore_length(mod, lists_of, "FIRST", "Words") == TSUCHI_OK && tsuchi::table_restore_length(mod, lists_of, "FIRST", "Pictures") == TSUCHI_OK &&
          data_of(kPictures) == pictures && data_of(kWords) == words && text_of("Pictures[0]") == "/G/Old.Old" && text_of("Words[0]") == "One" &&
          fake::get<uint64_t>(pictures + s::sub_path + a::data) == inside_path && fake::get<uint64_t>(words + a::data) == word);
    CHECK(engine.allocated - allocated == engine.freed - freed && tsuchi::owned_count(mod) == owned && tsuchi::table_changes() == changes);

    // Several mods can each have a say. The say of the mod started last is in force; when it is taken back the
    // next one's is; and a say ends with its mod.
    TSUCHI_Mod* rival = tsuchi::mod_create("actors.lengths");
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 1) == TSUCHI_OK && tsuchi::table_resize(rival, lists_of, "FIRST", "Numbers", 5) == TSUCHI_OK && count_of(kNumbers) == 5 &&
          number_at(2) == 30 && tsuchi::table_lengths() == 2);
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 2) == TSUCHI_OK && count_of(kNumbers) == 5); // the earlier mod thinks again: still not in force
    CHECK(sheet.override(rival, lists_of, "FIRST", "Numbers[4]", &seventy) == TSUCHI_OK && number_at(4) == 70);
    CHECK(tsuchi::table_restore_length(mod, lists_of, "FIRST", "Numbers") == TSUCHI_OK && count_of(kNumbers) == 5 && number_at(4) == 70 && tsuchi::table_lengths() == 1);
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 2) == TSUCHI_OK && count_of(kNumbers) == 5);
    tsuchi::mod_stop(rival); // its length and its change go, in whichever order: the earlier mod's length is in force
    CHECK(count_of(kNumbers) == 2 && number_at(1) == 20 && tsuchi::table_lengths() == 1 && tsuchi::table_changes() == changes);
    CHECK(tsuchi::table_restore_length(mod, lists_of, "FIRST", "Numbers") == TSUCHI_OK && data_of(kNumbers) == numbers && count_of(kNumbers) == 3);

    // The same through the service: `resize` holds a length, and `restore` with the array's path takes it back.
    CHECK(sheet.resize(mod, lists_of, "FIRST", "Numbers", 4) == TSUCHI_OK && count_of(kNumbers) == 4 && number_at(3) == 0 && tsuchi::table_lengths() == 1 &&
          tsuchi::owned_count(mod) == owned + 1);
    CHECK(sheet.restore(mod, lists_of, "FIRST", "Numbers") == TSUCHI_OK && data_of(kNumbers) == numbers && count_of(kNumbers) == 3 && tsuchi::table_lengths() == 0 &&
          tsuchi::owned_count(mod) == owned && sheet.restore(mod, lists_of, "FIRST", "Numbers") == TSUCHI_NOT_FOUND);
    CHECK(sheet.resize(mod, lists_of, "FIRST", "Kind", 1) == TSUCHI_INVALID && sheet.resize(mod, lists_of, "FIRST", "Points", 1) == TSUCHI_UNSUPPORTED);

    // An existing string override survives every failed first resize and later resize boundary.
    CHECK(sheet.override_text(mod, lists_of, "FIRST", "Words[0]", "Held") == TSUCHI_OK);
    for (const bool already : {false, true}) {
        if (already) CHECK(sheet.resize(mod, lists_of, "FIRST", "Words", 2) == TSUCHI_OK);
        for (const bool framework : {false, true}) {
            bool completed = false;
            for (int fail = 0; fail < 64; ++fail) {
                const uint64_t before_data = data_of(kWords), before_string = fake::get<uint64_t>(before_data);
                const int32_t before_count = count_of(kWords);
                const uint32_t before_owned = tsuchi::owned_count(mod);
                const unsigned before_lengths = tsuchi::table_lengths();
                allocated = engine.allocated; freed = engine.freed;
                engine.fail_after = framework ? -1 : fail;
                watched_fail_after = framework ? fail : -1;
                const TSUCHI_Result result = sheet.resize(mod, lists_of, "FIRST", "Words", already ? 3 : 2);
                engine.fail_after = watched_fail_after = -1;
                CHECK(result == TSUCHI_NO_MEMORY || result == TSUCHI_OK);
                if (result == TSUCHI_OK) { completed = true; break; }
                CHECK(data_of(kWords) == before_data && count_of(kWords) == before_count && fake::get<uint64_t>(before_data) == before_string && text_of("Words[0]") == "Held");
                CHECK(tsuchi::owned_count(mod) == before_owned && tsuchi::table_lengths() == before_lengths && engine.allocated - allocated == engine.freed - freed);
            }
            CHECK(completed && text_of("Words[0]") == "Held");
            const uint64_t held_data = data_of(kWords), held_string = fake::get<uint64_t>(held_data);
            const int32_t held_count = count_of(kWords);
            const uint32_t held_owned = tsuchi::owned_count(mod);
            const unsigned held_lengths = tsuchi::table_lengths();
            engine.fail_after = 0;
            CHECK(sheet.restore(mod, lists_of, "FIRST", "Words") == TSUCHI_NO_MEMORY);
            engine.fail_after = -1;
            CHECK(data_of(kWords) == held_data && fake::get<uint64_t>(held_data) == held_string && count_of(kWords) == held_count && text_of("Words[0]") == "Held" &&
                  tsuchi::owned_count(mod) == held_owned && tsuchi::table_lengths() == held_lengths);
            CHECK(sheet.restore(mod, lists_of, "FIRST", "Words") == TSUCHI_OK && data_of(kWords) == words && count_of(kWords) == 1 && text_of("Words[0]") == "Held");
            if (already && !framework) CHECK(sheet.resize(mod, lists_of, "FIRST", "Words", 2) == TSUCHI_OK);
        }
    }
    CHECK(sheet.restore(mod, lists_of, "FIRST", "Words[0]") == TSUCHI_OK && text_of("Words[0]") == "One" && tsuchi::owned_count(mod) == owned);

    // What cannot be asked, and what it leaves: nothing.
    allocated = engine.allocated, freed = engine.freed;
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Kind", 1) == TSUCHI_INVALID && tsuchi::table_resize(mod, lists_of, "FIRST", "Points", 1) == TSUCHI_UNSUPPORTED &&
          tsuchi::table_resize(mod, lists_of, "FIRST", "Nope", 1) == TSUCHI_NOT_FOUND && tsuchi::table_resize(mod, lists_of, "NOBODY", "Numbers", 1) == TSUCHI_NOT_FOUND &&
          tsuchi::table_resize(mod, object(w.idol_asset), "FIRST", "Numbers", 1) == TSUCHI_NOT_FOUND && tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers[", 1) == TSUCHI_INVALID &&
          tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 1001) == TSUCHI_CAPACITY && tsuchi::table_resize(nullptr, lists_of, "FIRST", "Numbers", 1) == TSUCHI_INVALID);
    engine.out_of_memory = true;
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 4) == TSUCHI_NO_MEMORY && data_of(kNumbers) == numbers && count_of(kNumbers) == 3);
    engine.out_of_memory = false;
    CHECK(tsuchi::table_lengths() == 0 && tsuchi::owned_count(mod) == owned && engine.allocated == allocated && engine.freed == freed);

    // Native Academy-style copies: independent arrays, owners retained while
    // stashed, all destinations prepared before publication, and late copies.
    static uint64_t mirror_table = 0, mirror_name = 0, current_owner = 0;
    static std::map<uint64_t, uint64_t> mirror_rows;
    mirror_table = lists; mirror_name = row_name(u"FIRST");
    tsuchi::add_row_copies([](uint64_t table, uint64_t name, uint64_t owner) -> tsuchi::TableRowCopy {
        if (table != mirror_table || name != mirror_name) return {};
        const auto handle = owner ? owner : current_owner;
        const auto found = mirror_rows.find(handle);
        return found != mirror_rows.end() && tsuchi::game::from_handle(handle) ? tsuchi::TableRowCopy{found->second, handle} : tsuchi::TableRowCopy{};
    });
    const uint64_t mirror_object = fake::object(u"ArrayCopyOwner", w.component_class, 0x80);
    current_owner = to_handle(mirror_object);
    uint64_t mirror_row = fake::memory(kRowBytes);
    mirror_rows[current_owner] = mirror_row;
    const uint64_t mirror_numbers = engine_block(12);
    for (uint64_t i = 0; i < 3; ++i) fake::put<int32_t>(mirror_numbers + 4 * i, 10 * int32_t(i + 1));
    const auto mirror_header = [&](uint64_t data, int32_t count) {
        fake::put(mirror_row + kNumbers + a::data, data);
        fake::put(mirror_row + kNumbers + a::count, count); fake::put(mirror_row + kNumbers + a::max, count);
    };
    const auto mirror_count = [&] { return fake::get<int32_t>(mirror_row + kNumbers + a::count); };
    const auto mirror_data = [&] { return fake::get<uint64_t>(mirror_row + kNumbers + a::data); };
    const auto mirror_value = [&](uint32_t i) { return fake::get<int32_t>(mirror_data() + 4 * i); };
    mirror_header(mirror_numbers, 3);
    CHECK(!tsuchi::game::object_kept(mirror_object));
    for (bool framework : {false, true}) {
        bool completed = false;
        for (int fail = 0; fail < 96; ++fail) {
            const auto before_data = data_of(kNumbers), before_mirror = mirror_data();
            allocated = engine.allocated; freed = engine.freed;
            engine.fail_after = framework ? -1 : fail;
            watched_fail_after = framework ? fail : -1;
            const auto result = sheet.resize(mod, lists_of, "FIRST", "Numbers", 5);
            engine.fail_after = watched_fail_after = -1;
            CHECK(result == TSUCHI_NO_MEMORY || result == TSUCHI_OK);
            if (result == TSUCHI_OK) { completed = true; break; }
            CHECK(data_of(kNumbers) == before_data && mirror_data() == before_mirror && count_of(kNumbers) == 3 && mirror_count() == 3);
            CHECK(engine.allocated - allocated == engine.freed - freed && !tsuchi::game::object_kept(mirror_object));
        }
        CHECK(completed && count_of(kNumbers) == 5 && mirror_count() == 5 && data_of(kNumbers) != mirror_data());
        CHECK(tsuchi::game::object_kept(mirror_object) && mirror_value(0) == 10 && mirror_value(4) == 0);
        CHECK(sheet.restore(mod, lists_of, "FIRST", "Numbers") == TSUCHI_OK && mirror_data() == mirror_numbers && mirror_count() == 3);
        CHECK(!tsuchi::game::object_kept(mirror_object));
    }
    CHECK(sheet.resize(mod, lists_of, "FIRST", "Numbers", 5) == TSUCHI_OK);
    CHECK(sheet.override(mod, lists_of, "FIRST", "Numbers[4]", &ninety) == TSUCHI_OK && mirror_value(4) == 90);
    TSUCHI_Mod* mirror_rival = tsuchi::mod_create("tables.copy.rival");
    CHECK(sheet.resize(mirror_rival, lists_of, "FIRST", "Numbers", 1) == TSUCHI_OK && mirror_count() == 1);
    CHECK(sheet.resize(mod, lists_of, "FIRST", "Numbers", 6) == TSUCHI_OK && mirror_count() == 1);
    // A map relocation moves the header, not ownership of its elements.
    const auto relocated = fake::memory(kRowBytes);
    std::memcpy(reinterpret_cast<void*>(relocated), reinterpret_cast<void*>(mirror_row), kRowBytes);
    std::memset(reinterpret_cast<void*>(mirror_row), 0, kRowBytes);
    mirror_row = relocated; mirror_rows[current_owner] = mirror_row;
    tsuchi::mod_stop(mirror_rival);
    CHECK(count_of(kNumbers) == 6 && mirror_count() == 6 && mirror_value(4) == 90);
    CHECK(sheet.restore(mod, lists_of, "FIRST", "Numbers[4]") == TSUCHI_OK);
    CHECK(sheet.restore(mod, lists_of, "FIRST", "Numbers") == TSUCHI_OK && mirror_data() == mirror_numbers && mirror_count() == 3);
    CHECK(!tsuchi::game::object_kept(mirror_object));

    current_owner = 0;
    CHECK(sheet.resize(mod, lists_of, "FIRST", "Numbers", 5) == TSUCHI_OK);
    CHECK(sheet.override(mod, lists_of, "FIRST", "Numbers[1]", &ninety) == TSUCHI_OK);
    const uint64_t late_object = fake::object(u"LateArrayCopyOwner", w.component_class, 0x80);
    current_owner = to_handle(late_object);
    mirror_row = fake::memory(kRowBytes); mirror_rows[current_owner] = mirror_row;
    const uint64_t late_native = engine_block(20);
    std::memcpy(reinterpret_cast<void*>(late_native), reinterpret_cast<void*>(data_of(kNumbers)), 20);
    mirror_header(late_native, 5); // the game copied the already modified table
    tsuchi::frame_begin();
    CHECK(mirror_count() == 5 && mirror_value(1) == 90 && mirror_data() != late_native && tsuchi::game::object_kept(late_object));
    const auto stable_data = mirror_data(); allocated = engine.allocated;
    tsuchi::frame_begin(); tsuchi::frame_begin();
    CHECK(mirror_data() == stable_data && engine.allocated == allocated); // no per-frame rebuild
    CHECK(sheet.restore(mod, lists_of, "FIRST", "Numbers") == TSUCHI_OK && mirror_count() == 3 && mirror_value(1) == 90);
    CHECK(sheet.restore(mod, lists_of, "FIRST", "Numbers[1]") == TSUCHI_OK && mirror_value(1) == 20 && !tsuchi::game::object_kept(late_object));

    // String elements own independent copies, including normalized originals
    // and all failure points before either array header is published.
    const uint64_t mirror_words = engine_block(0x10), mirror_word = engine_block(8);
    std::memcpy(reinterpret_cast<void*>(mirror_word), u"One", 8);
    fake::put(mirror_words + a::data, mirror_word); fake::put<int32_t>(mirror_words + a::count, 4); fake::put<int32_t>(mirror_words + a::max, 4);
    fake::put(mirror_row + kWords + a::data, mirror_words); fake::put<int32_t>(mirror_row + kWords + a::count, 1); fake::put<int32_t>(mirror_row + kWords + a::max, 1);
    const auto mirror_text = [&] {
        char buffer[32]; uint64_t bytes = 0;
        CHECK(tsuchi::text_at(row_type, mirror_row, "Words[0]", buffer, sizeof buffer, &bytes) == TSUCHI_OK);
        return std::string(buffer);
    };
    CHECK(sheet.override_text(mod, lists_of, "FIRST", "Words[0]", "Held") == TSUCHI_OK && mirror_text() == "Held");
    for (bool framework : {false, true}) {
        bool completed = false;
        for (int fail = 0; fail < 96; ++fail) {
            const auto before_source = data_of(kWords), before_copy = fake::get<uint64_t>(mirror_row + kWords + a::data);
            allocated = engine.allocated; freed = engine.freed;
            engine.fail_after = framework ? -1 : fail; watched_fail_after = framework ? fail : -1;
            const auto result = sheet.resize(mod, lists_of, "FIRST", "Words", 3);
            engine.fail_after = watched_fail_after = -1;
            CHECK(result == TSUCHI_NO_MEMORY || result == TSUCHI_OK);
            if (result == TSUCHI_OK) { completed = true; break; }
            CHECK(data_of(kWords) == before_source && fake::get<uint64_t>(mirror_row + kWords + a::data) == before_copy);
            CHECK(count_of(kWords) == 1 && fake::get<int32_t>(mirror_row + kWords + a::count) == 1 && mirror_text() == "Held");
            CHECK(engine.allocated - allocated == engine.freed - freed && !tsuchi::game::object_kept(late_object));
        }
        CHECK(completed && mirror_text() == "Held" && fake::get<int32_t>(mirror_row + kWords + a::count) == 3);
        CHECK(sheet.restore(mod, lists_of, "FIRST", "Words") == TSUCHI_OK && fake::get<uint64_t>(mirror_row + kWords + a::data) == mirror_words);
    }
    CHECK(sheet.restore(mod, lists_of, "FIRST", "Words[0]") == TSUCHI_OK && mirror_text() == "One");

    // A removed source row does not strand the surviving native copy's roots.
    const auto saved_mirror_row = mirror_row;
    mirror_name = row_name(u"COPY_REMOVED");
    const auto removed_row = fake::memory(kRowBytes), removed_numbers = engine_block(12), removed_copy = engine_block(12);
    fake::put(removed_row + kNumbers + a::data, removed_numbers); fake::put<int32_t>(removed_row + kNumbers + a::count, 3); fake::put<int32_t>(removed_row + kNumbers + a::max, 3);
    sheets.rows[{lists, mirror_name}] = removed_row;
    mirror_row = fake::memory(kRowBytes); mirror_rows[current_owner] = mirror_row; mirror_header(removed_copy, 3);
    CHECK(sheet.resize(mod, lists_of, "COPY_REMOVED", "Numbers", 5) == TSUCHI_OK && mirror_count() == 5 && tsuchi::game::object_kept(late_object));
    sheets.rows.erase({lists, mirror_name});
    CHECK(sheet.restore(mod, lists_of, "COPY_REMOVED", "Numbers") == TSUCHI_OK && mirror_data() == removed_copy && mirror_count() == 3 && !tsuchi::game::object_kept(late_object));
    mirror_name = row_name(u"FIRST"); mirror_row = saved_mirror_row; mirror_rows[current_owner] = mirror_row;

    // A foreign header survives stop; neither uncertain storage nor the foreign
    // array is freed, and this owner/path cannot be acquired again.
    TSUCHI_Mod* displaced = tsuchi::mod_create("tables.copy.displaced");
    CHECK(sheet.resize(displaced, lists_of, "FIRST", "Numbers", 5) == TSUCHI_OK);
    CHECK(sheet.resize(mod, lists_of, "FIRST", "Numbers", 4) == TSUCHI_OK && count_of(kNumbers) == 5 && mirror_count() == 5);
    const uint64_t foreign = engine_block(4); fake::put<int32_t>(foreign, 777);
    mirror_header(foreign, 1);
    // Giving up a say is not refused over a copy another writer took, as stopping is not.
    CHECK(sheet.restore(mod, lists_of, "FIRST", "Numbers") == TSUCHI_OK && count_of(kNumbers) == 5 && mirror_data() == foreign);
    CHECK(sheet.resize(displaced, lists_of, "FIRST", "Numbers", 6) == TSUCHI_BUSY && count_of(kNumbers) == 5 && mirror_data() == foreign);
    tsuchi::mod_stop(displaced);
    CHECK(data_of(kNumbers) == numbers && count_of(kNumbers) == 3 && mirror_data() == foreign && mirror_value(0) == 777);
    CHECK(!tsuchi::game::object_kept(late_object));
    CHECK(sheet.resize(mod, lists_of, "FIRST", "Numbers", 5) == TSUCHI_BUSY && count_of(kNumbers) == 3 && mirror_data() == foreign);
    fake::destroy(late_object); current_owner = 0; tsuchi::frame_begin();
    mirror_table = 0; mirror_rows.clear();

    // A stop that finds no memory for the length left in force leaves the array as it was. A later frame puts it
    // right, and the frames after that find nothing to do.
    TSUCHI_Mod* starved = tsuchi::mod_create("tables.length.starved");
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 5) == TSUCHI_OK && tsuchi::table_resize(starved, lists_of, "FIRST", "Numbers", 2) == TSUCHI_OK && count_of(kNumbers) == 2);
    engine.fail_after = 0;
    tsuchi::mod_stop(starved);
    engine.fail_after = -1;
    CHECK(count_of(kNumbers) == 2 && tsuchi::table_lengths() == 1);
    tsuchi::frame_begin();
    CHECK(count_of(kNumbers) == 5 && number_at(2) == 30);
    allocated = engine.allocated;
    tsuchi::frame_begin(); tsuchi::frame_begin();
    CHECK(engine.allocated == allocated);
    CHECK(tsuchi::table_restore_length(mod, lists_of, "FIRST", "Numbers") == TSUCHI_OK && data_of(kNumbers) == numbers && count_of(kNumbers) == 3);

    // The game takes the row away meanwhile: the say is forgotten quietly, and nothing is written.
    CHECK(tsuchi::table_resize(mod, lists_of, "FIRST", "Numbers", 1) == TSUCHI_OK && count_of(kNumbers) == 1);
    const uint64_t left = data_of(kNumbers);
    sheets.rows.erase({lists, row_name(u"FIRST")});
    CHECK(tsuchi::table_restore_length(mod, lists_of, "FIRST", "Numbers") == TSUCHI_OK && data_of(kNumbers) == left && tsuchi::table_lengths() == 0 && tsuchi::owned_count(mod) == owned);
}
