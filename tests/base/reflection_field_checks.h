void owned_fields() {
    auto* owner = tsuchi::mod_create("fields.owner");
    auto* other = tsuchi::mod_create("fields.other");
    const auto object = handle(w.red);
    TSUCHI_FieldHold hold{}, duplicate{};
    auto wanted = number(0.1), original = get("Health");
    CHECK(api.hold_field(owner, object, "Health", &wanted, "", &hold) == TSUCHI_OK);
    CHECK(is_number(get("Health"), double(float(0.1)))); // retain stored precision for comparison
    CHECK(api.hold_field(other, object, "hEaLtH", &wanted, "", &duplicate) == TSUCHI_BUSY);
    CHECK(api.update_field(other, hold, &wanted) == TSUCHI_DENIED);
    wanted = number(0.2);
    CHECK(api.update_field(owner, hold, &wanted) == TSUCHI_OK);
    CHECK(api.release_field(other, hold) == TSUCHI_DENIED);
    CHECK(api.release_field(owner, hold) == TSUCHI_OK && is_number(get("Health"), original.as.number));
    CHECK(api.release_field(owner, hold) == TSUCHI_NOT_FOUND);
    wanted = number(7);
    CHECK(api.hold_field(owner, object, "Health", &wanted, "", &hold) == TSUCHI_OK);
    CHECK(set("Health", number(9)) == TSUCHI_OK);
    CHECK(api.update_field(owner, hold, &wanted) == TSUCHI_BUSY);
    CHECK(api.release_field(owner, hold) == TSUCHI_BUSY);
    tsuchi::mod_stop(owner); CHECK(is_number(get("Health"), 9)); // foreign value survives
    CHECK(set("Health", original) == TSUCHI_OK);
    owner = tsuchi::mod_create("fields.owner");
    CHECK(api.hold_field(owner, object, "Root.Scale", &wanted, "", &hold) == TSUCHI_UNSUPPORTED);
    CHECK(api.hold_field(owner, object, "Health", &wanted, "Id", &hold) == TSUCHI_INVALID);
    auto overflow = integer(256);
    CHECK(api.hold_field(owner, object, "Byte", &overflow, "", &hold) == TSUCHI_INVALID);
    CHECK(tsuchi::owned_count(owner) == 0);
    auto too_large = number(1e100);
    CHECK(api.hold_field(owner, object, "Health", &too_large, "", &hold) == TSUCHI_INVALID);
    CHECK(is_number(get("Health"), original.as.number) && !tsuchi::owned_count(owner));
    CHECK(api.hold_field(owner, object, "Health", &wanted, "", &hold) == TSUCHI_OK);
    CHECK(api.update_field(owner, hold, &too_large) == TSUCHI_INVALID);
    CHECK(is_number(get("Health"), 7));
    CHECK(api.release_field(owner, hold) == TSUCHI_OK && is_number(get("Health"), original.as.number));

    // A real reflected array: identity resolution survives reallocation and reorder.
    const auto row_type = fake::memory(0x80);
    fake::put<uint64_t>(row_type, fake::engine().vtable);
    fake::field(row_type, u"Id", u"UInt64Property", 0, 8);
    fake::field(row_type, u"Days", u"IntProperty", 8, 4);
    const auto manager_type = fake::engine_object(u"FieldManager", w.root);
    fake::field(manager_type, u"Rows", u"ArrayProperty", 0x40, 0x10, fake::loose_field(u"StructProperty", 16, row_type));
    const auto manager = fake::engine_object(u"Manager", manager_type, 0x80);
    const auto rows = fake::memory(32), moved = fake::memory(32);
    fake::put<uint64_t>(rows, UINT64_C(9007199254740993)); fake::put<int32_t>(rows + 8, 3);
    fake::put<uint64_t>(rows + 16, 2); fake::put<int32_t>(rows + 24, 4);
    fake::put<uint64_t>(manager + 0x40, rows); fake::put<int32_t>(manager + 0x48, 2); fake::put<int32_t>(manager + 0x4c, 2);
    wanted = integer(7);
    CHECK(api.hold_field(owner, handle(manager), "Rows[0].Days", &wanted, "Id", &hold) == TSUCHI_OK);
    CHECK(api.hold_field(other, handle(manager), "Rows[0].Id", &wanted, "Id", &duplicate) == TSUCHI_INVALID);
    CHECK(api.hold_field(other, handle(manager), "Rows[0].Days", &wanted, "", &duplicate) == TSUCHI_UNSUPPORTED);
    for (uint32_t i = 0; i < 16; ++i) {
        fake::put<uint8_t>(moved + i, fake::get<uint8_t>(rows + 16 + i));
        fake::put<uint8_t>(moved + 16 + i, fake::get<uint8_t>(rows + i));
    }
    fake::put<uint64_t>(manager + 0x40, moved);
    wanted = integer(8);
    CHECK(api.update_field(owner, hold, &wanted) == TSUCHI_OK && fake::get<int32_t>(moved + 24) == 8);
    CHECK(fake::get<int32_t>(moved + 8) == 4);
    CHECK(api.hold_field(other, handle(manager), "Rows[1].Days", &wanted, "Id", &duplicate) == TSUCHI_BUSY);
    fake::put<uint64_t>(moved, UINT64_C(9007199254740993));
    CHECK(api.update_field(owner, hold, &wanted) == TSUCHI_BUSY && api.release_field(owner, hold) == TSUCHI_BUSY);
    fake::put<uint64_t>(moved, 2);
    tsuchi::mod_stop(owner);
    CHECK(fake::get<int32_t>(moved + 24) == 3 && fake::get<int32_t>(moved + 8) == 4);
    owner = tsuchi::mod_create("fields.owner");
    CHECK(api.hold_field(owner, handle(manager), "Rows[1].Days", &wanted, "Id", &hold) == TSUCHI_OK);
    fake::put<int32_t>(manager + 0x40 + fake::t::array::count, 1);
    CHECK(api.update_field(owner, hold, &wanted) == TSUCHI_NOT_FOUND);
    CHECK(api.release_field(owner, hold) == TSUCHI_OK); // deletion doesn't touch the remaining row
    CHECK(fake::get<int32_t>(moved + 8) == 4);
    tsuchi::set_allocator({[](size_t)->void* { return nullptr; }, std::free});
    CHECK(api.hold_field(owner, object, "Health", &wanted, "", &hold) == TSUCHI_NO_MEMORY);
    tsuchi::set_allocator({std::malloc, std::free});
    CHECK(is_number(get("Health"), original.as.number) && !tsuchi::owned_count(owner));
    tsuchi::mod_stop(owner); tsuchi::mod_stop(other);
}
