{
    using tsuchi::ScriptWriteTree;
    using tsuchi::ScriptWriteKind;
    const uint64_t receipt = fake::memory(0x80), atomic_type = fake::engine_object(u"AtomicRow", w.root);
    fake::put<uint64_t>(receipt, fake::engine().vtable);
    fake::field(receipt, u"Title", u"StrProperty", 0, 0x10);
    fake::field(receipt, u"Count", u"IntProperty", 0x10, 4);
    fake::field(atomic_type, u"Receipt", u"StructProperty", 0, 0x30, receipt);
    fake::field(receipt, u"Label", u"NameProperty", 0x28, 8);
    fake::field(receipt, u"Links", u"ArrayProperty", 0x18, 0x10, fake::loose_field(u"ObjectProperty", 8, w.component_class));
    const uint64_t atomic_table = table_of(u"AtomicTable"), atomic_row = fake::memory(kRowBytes);
    fake::put(atomic_table + t::data_table::row_struct, atomic_type);
    sheets.rows[{atomic_table, row_name(u"FIRST")}] = atomic_row;
    CHECK(tsuchi::write_text_at(atomic_type, atomic_row, "Receipt.Title", "original") == TSUCHI_OK);
    fake::put<int32_t>(atomic_row + 0x10, 1);
    const TSUCHI_Object atomic_of = object(atomic_table);
    const auto title_of = [&]() {
        char text[64]; uint64_t bytes = 0;
        CHECK(sheet.get_text(mod, atomic_of, "FIRST", "Receipt.Title", text, sizeof text, &bytes) == TSUCHI_OK);
        return std::string(text);
    };
    const auto make = [](ScriptWriteTree& tree, const char* title, int64_t count) {
        tree.root = tree.add(ScriptWriteKind::structure);
        auto* text = tree.add(ScriptWriteKind::text);
        auto* number = tree.add(ScriptWriteKind::value);
        CHECK(tree.root && text && number && text->name.set("Title") && text->text.set(title) && number->name.set("Count"));
        number->value.kind = TSUCHI_VALUE_INTEGER; number->value.as.integer = count;
        CHECK(tree.root->children.add(text) && tree.root->children.add(number));
    };
    TSUCHI_Mod* earlier = tsuchi::mod_create("tables.atomic.earlier");
    CHECK(sheet.override_text(earlier, atomic_of, "FIRST", "Receipt.Title", "earlier") == TSUCHI_OK);
    ScriptWriteTree replacement; make(replacement, "replacement", 7);
    for (const bool framework : {false, true}) {
        bool completed = false;
        for (int fail = 0; fail < 64; ++fail) {
            const unsigned before_changes = tsuchi::table_changes();
            const uint32_t before_owned = tsuchi::owned_count(earlier);
            const uint64_t before_string = fake::get<uint64_t>(atomic_row);
            auto& engine = fake::engine();
            const int allocated = engine.allocated, freed = engine.freed;
            engine.fail_after = framework ? -1 : fail;
            watched_fail_after = framework ? fail : -1;
            const TSUCHI_Result result = tsuchi::apply_script_write(earlier, {atomic_of, {}, "FIRST"}, "Receipt", *replacement.root);
            engine.fail_after = watched_fail_after = -1;
            CHECK(result == TSUCHI_NO_MEMORY || result == TSUCHI_OK);
            if (result == TSUCHI_OK) { completed = true; break; }
            CHECK(title_of() == "earlier" && fake::get<uint64_t>(atomic_row) == before_string && fake::get<int32_t>(atomic_row + 0x10) == 1);
            CHECK(tsuchi::table_changes() == before_changes && tsuchi::owned_count(earlier) == before_owned);
            CHECK(engine.allocated - allocated == engine.freed - freed);
        }
        CHECK(completed && title_of() == "replacement" && fake::get<int32_t>(atomic_row + 0x10) == 7);
        CHECK(sheet.restore(earlier, atomic_of, "FIRST", "Receipt.Title") == TSUCHI_OK && title_of() == "original");
        CHECK(sheet.restore(earlier, atomic_of, "FIRST", "Receipt.Count") == TSUCHI_OK && fake::get<int32_t>(atomic_row + 0x10) == 1);
        CHECK(sheet.override_text(earlier, atomic_of, "FIRST", "Receipt.Title", "earlier") == TSUCHI_OK);
    }
    TSUCHI_Mod* later = tsuchi::mod_create("tables.atomic.later");
    CHECK(sheet.override_text(later, atomic_of, "FIRST", "Receipt.Title", "later") == TSUCHI_OK);
    CHECK(tsuchi::apply_script_write(earlier, {atomic_of, {}, "FIRST"}, "Receipt", *replacement.root) == TSUCHI_OK && title_of() == "later");
    tsuchi::mod_stop(later);
    CHECK(title_of() == "replacement");
    tsuchi::mod_stop(earlier);
    CHECK(title_of() == "original" && fake::get<int32_t>(atomic_row + 0x10) == 1);
    TSUCHI_Mod* fresh = tsuchi::mod_create("tables.atomic.fresh");
    ScriptWriteTree invalid; make(invalid, "never published", int64_t(INT32_MAX) + 1);
    const unsigned before_changes = tsuchi::table_changes();
    CHECK(tsuchi::apply_script_write(fresh, {atomic_of, {}, "FIRST"}, "Receipt", *invalid.root) == TSUCHI_INVALID);
    CHECK(tsuchi::owned_count(fresh) == 0 && tsuchi::table_changes() == before_changes && title_of() == "original");
    CHECK(tsuchi::write_text_at(atomic_type, atomic_row, "Receipt.Title", "new game value") == TSUCHI_OK);
    CHECK(sheet.override_text(fresh, atomic_of, "FIRST", "Receipt.Title", "temporary") == TSUCHI_OK);
    tsuchi::mod_stop(fresh);
    CHECK(title_of() == "new game value"); // failed preparation left no orphan original
    // Cleared names are unreadable through the text reader, but a retained original makes replacement possible.
    TSUCHI_Mod* named = tsuchi::mod_create("tables.atomic.named");
    const unsigned before_name_changes = tsuchi::table_changes();
    CHECK(sheet.override_text(named, atomic_of, "FIRST", "Receipt.Label", "fresh unreadable") == TSUCHI_NOT_FOUND);
    CHECK(fake::get<uint64_t>(atomic_row + 0x28) == 0 && tsuchi::owned_count(named) == 0 && tsuchi::table_changes() == before_name_changes);
    const uint64_t original_label = row_name(u"OriginalLabel");
    fake::put(atomic_row + 0x28, original_label);
    CHECK(sheet.override_text(named, atomic_of, "FIRST", "Receipt.Label", "") == TSUCHI_OK && fake::get<uint64_t>(atomic_row + 0x28) == 0);
    TSUCHI_Value wrong_label{TSUCHI_VALUE_INTEGER, 0, {}}; wrong_label.as.integer = 3;
    CHECK(sheet.override(named, atomic_of, "FIRST", "Receipt.Label", &wrong_label) == TSUCHI_UNSUPPORTED && fake::get<uint64_t>(atomic_row + 0x28) == 0);
    fake::engine().fail_after = 0;
    CHECK(sheet.override_text(named, atomic_of, "FIRST", "Receipt.Label", "ReplacedLabel") == TSUCHI_NO_MEMORY && fake::get<uint64_t>(atomic_row + 0x28) == 0);
    fake::engine().fail_after = -1;
    CHECK(sheet.override_text(named, atomic_of, "FIRST", "Receipt.Label", "ReplacedLabel") == TSUCHI_OK);
    char label[64]; uint64_t label_bytes = 0;
    CHECK(sheet.get_text(mod, atomic_of, "FIRST", "Receipt.Label", label, sizeof label, &label_bytes) == TSUCHI_OK && std::string(label) == "ReplacedLabel");
    CHECK(sheet.restore(named, atomic_of, "FIRST", "Receipt.Label") == TSUCHI_OK && fake::get<uint64_t>(atomic_row + 0x28) == original_label);
    tsuchi::mod_stop(named);
    // Later path resolution follows a staged reference, including copies rebuilt from the aside.
    const uint64_t linked_type = fake::engine_object(u"AtomicLinked", w.root);
    fake::set_super(linked_type, w.component_class);
    fake::field(linked_type, u"Value", u"IntProperty", 0x40, 4);
    const uint64_t first_object = fake::engine_object(u"AtomicFirst", linked_type), second_object = fake::engine_object(u"AtomicSecond", linked_type);
    fake::put<int32_t>(first_object + 0x40, 10); fake::put<int32_t>(second_object + 0x40, 20);
    const uint64_t links = reinterpret_cast<uint64_t>(tsuchi::game::engine_memory(8));
    fake::put(links, first_object);
    fake::put(atomic_row + 0x18, links); fake::put<int32_t>(atomic_row + 0x20, 1); fake::put<int32_t>(atomic_row + 0x24, 1);
    const auto linked_write = [&](ScriptWriteTree& tree, bool invalid_count) {
        tree.root = tree.add(ScriptWriteKind::structure);
        auto* link = tree.add(ScriptWriteKind::value);
        auto* number = tree.add(ScriptWriteKind::value);
        CHECK(tree.root && link && number && link->name.set("Links[0]") && number->name.set("Links[0].Value"));
        link->value.kind = TSUCHI_VALUE_OBJECT; link->value.as.object = object(second_object);
        number->value.kind = TSUCHI_VALUE_INTEGER; number->value.as.integer = 33;
        CHECK(tree.root->children.add(link) && tree.root->children.add(number));
        if (invalid_count) {
            auto* overflow = tree.add(ScriptWriteKind::value);
            CHECK(overflow && overflow->name.set("Count"));
            overflow->value.kind = TSUCHI_VALUE_INTEGER; overflow->value.as.integer = int64_t(INT32_MAX) + 1;
            CHECK(tree.root->children.add(overflow));
        }
    };
    TSUCHI_Mod* compound = tsuchi::mod_create("tables.atomic.compound.references");
    ScriptWriteTree failed_links; linked_write(failed_links, true);
    const unsigned before_link_changes = tsuchi::table_changes();
    CHECK(tsuchi::apply_script_write(compound, {atomic_of, {}, "FIRST"}, "Receipt", *failed_links.root) == TSUCHI_INVALID);
    CHECK(tsuchi::owned_count(compound) == 0 && tsuchi::table_changes() == before_link_changes);
    CHECK(fake::get<uint64_t>(links) == first_object && fake::get<int32_t>(first_object + 0x40) == 10 && fake::get<int32_t>(second_object + 0x40) == 20);
    ScriptWriteTree compound_links; linked_write(compound_links, false);
    CHECK(tsuchi::apply_script_write(compound, {atomic_of, {}, "FIRST"}, "Receipt", *compound_links.root) == TSUCHI_OK);
    CHECK(fake::get<uint64_t>(links) == second_object && fake::get<int32_t>(first_object + 0x40) == 10 && fake::get<int32_t>(second_object + 0x40) == 33);
    tsuchi::mod_stop(compound);
    CHECK(fake::get<uint64_t>(links) == first_object && fake::get<int32_t>(second_object + 0x40) == 20);
    // An existing later-started parent's say governs child resolution during preparation.
    TSUCHI_Mod* low = tsuchi::mod_create("tables.atomic.reference.low"), *high = tsuchi::mod_create("tables.atomic.reference.high");
    TSUCHI_Value first_value{TSUCHI_VALUE_OBJECT, 0, {}}; first_value.as.object = object(first_object);
    CHECK(sheet.override(high, atomic_of, "FIRST", "Receipt.Links[0]", &first_value) == TSUCHI_OK);
    CHECK(tsuchi::apply_script_write(low, {atomic_of, {}, "FIRST"}, "Receipt", *compound_links.root) == TSUCHI_OK);
    CHECK(fake::get<uint64_t>(links) == first_object && fake::get<int32_t>(first_object + 0x40) == 33 && fake::get<int32_t>(second_object + 0x40) == 20);
    CHECK(sheet.restore(low, atomic_of, "FIRST", "Receipt.Links[0].Value") == TSUCHI_OK && fake::get<int32_t>(first_object + 0x40) == 10);
    tsuchi::mod_stop(high);
    CHECK(fake::get<uint64_t>(links) == second_object);
    tsuchi::mod_stop(low);
    CHECK(fake::get<uint64_t>(links) == first_object);
    TSUCHI_Mod* references = tsuchi::mod_create("tables.atomic.references");
    TSUCHI_Value second_value{TSUCHI_VALUE_OBJECT, 0, {}};
    second_value.as.object = object(second_object);
    CHECK(sheet.override(references, atomic_of, "FIRST", "Receipt.Links[0]", &second_value) == TSUCHI_OK);
    TSUCHI_Value changed_value{TSUCHI_VALUE_INTEGER, 0, {}}; changed_value.as.integer = 77;
    CHECK(sheet.override(references, atomic_of, "FIRST", "Receipt.Links[0].Value", &changed_value) == TSUCHI_OK);
    CHECK(sheet.resize(references, atomic_of, "FIRST", "Receipt.Links", 2) == TSUCHI_OK);
    CHECK(sheet.resize(references, atomic_of, "FIRST", "Receipt.Links", 3) == TSUCHI_OK);
    CHECK(fake::get<int32_t>(first_object + 0x40) == 10 && fake::get<int32_t>(second_object + 0x40) == 77);
    CHECK(sheet.restore(references, atomic_of, "FIRST", "Receipt.Links") == TSUCHI_OK);
    CHECK(fake::get<uint64_t>(atomic_row + 0x18) == links && fake::get<uint64_t>(links) == second_object);
    tsuchi::mod_stop(references);
    CHECK(fake::get<uint64_t>(links) == first_object && fake::get<int32_t>(first_object + 0x40) == 10 && fake::get<int32_t>(second_object + 0x40) == 20);
}
