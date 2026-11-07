// These allocation failures target each preparation boundary, not a fallible rollback.
int atomic_framework_fail = -1, atomic_framework_live = 0;
void* atomic_allocate(size_t bytes) {
    if (atomic_framework_fail == 0) return nullptr;
    if (atomic_framework_fail > 0) --atomic_framework_fail;
    void* result = std::malloc(bytes);
    if (result) ++atomic_framework_live;
    return result;
}
void atomic_release(void* memory) {
    if (memory) --atomic_framework_live;
    std::free(memory);
}
void atomic_writes() {
    using tsuchi::ScriptWriteTree;
    using tsuchi::ScriptWriteKind;
    auto& engine = fake::engine();
    // Both a string array and an array of structs grow while each allocation is refused in turn.
    for (const bool structs : {false, true}) {
        const uint64_t header = w.red + (structs ? 0xA8 : 0x100);
        const uint64_t before_data = fake::get<uint64_t>(header);
        const int32_t before_count = fake::get<int32_t>(header + fake::t::array::count);
        ScriptWriteTree tree;
        tree.root = tree.add(ScriptWriteKind::array);
        for (int32_t i = 0; i < before_count + 1; ++i) {
            auto* member = tree.add(structs ? ScriptWriteKind::structure : ScriptWriteKind::text);
            CHECK(member && tree.root->children.add(member));
            if (structs) {
                auto* x = tree.add(ScriptWriteKind::value);
                CHECK(x && x->name.set("X") && member->children.add(x));
                x->value = number(80 + i);
            } else CHECK(member->text.set("staged string"));
        }
        for (const bool framework : {false, true}) {
            bool finished = false;
            for (int fail = 0; fail < 64; ++fail) {
                const int allocated = engine.allocated, freed = engine.freed, made = engine.made, unmade = engine.unmade;
                engine.fail_after = framework ? -1 : fail;
                atomic_framework_fail = framework ? fail : -1;
                atomic_framework_live = 0;
                if (framework) tsuchi::set_allocator({atomic_allocate, atomic_release});
                TSUCHI_Result result;
                {
                    tsuchi::game::WritePlan plan;
                    result = plan.prepare(tsuchi::game::type_of(w.red), w.red, structs ? "Points" : "Notes", *tree.root);
                    // Even successful preparation can be abandoned without touching game memory.
                }
                if (framework) tsuchi::set_allocator({std::malloc, std::free});
                engine.fail_after = atomic_framework_fail = -1;
                CHECK(result == TSUCHI_NO_MEMORY || result == TSUCHI_OK);
                CHECK(fake::get<uint64_t>(header) == before_data && fake::get<int32_t>(header + fake::t::array::count) == before_count);
                CHECK(engine.allocated - allocated == engine.freed - freed && engine.made - made == engine.unmade - unmade);
                CHECK(!framework || atomic_framework_live == 0);
                if (result == TSUCHI_OK) { finished = true; break; }
            }
            CHECK(finished);
        }
        CHECK(tsuchi::apply_script_write(mod, {handle(w.red), {}, nullptr}, structs ? "Points" : "Notes", *tree.root) == TSUCHI_OK);
        CHECK(fake::get<int32_t>(header + fake::t::array::count) == before_count + 1);
    }
    // Two dynamic array properties may partially alias the same storage. Abort must never free an interior pointer.
    const uint64_t pair = fake::memory(0x80), type = fake::engine_object(u"AliasedArrayOwner", w.root);
    fake::put<uint64_t>(pair, engine.vtable);
    fake::field(pair, u"Left", u"ArrayProperty", 0, 0x10, fake::loose_field(u"NameProperty", 8));
    fake::field(pair, u"Right", u"ArrayProperty", 0x10, 0x10, fake::loose_field(u"NameProperty", 8));
    fake::field(type, u"Pair", u"StructProperty", 0x40, 0x20, pair);
    const uint64_t object = fake::engine_object(u"Aliased", type, 0x80);
    const uint64_t buffer = reinterpret_cast<uint64_t>(tsuchi::game::engine_memory(24));
    std::memset(reinterpret_cast<void*>(buffer), 0, 24);
    for (int i = 0; i < 2; ++i) {
        fake::put(object + 0x40 + uint64_t(16 * i), buffer + uint64_t(8 * i));
        fake::put<int32_t>(object + 0x48 + uint64_t(16 * i), 2);
        fake::put<int32_t>(object + 0x4C + uint64_t(16 * i), 2);
    }
    ScriptWriteTree alias;
    alias.root = alias.add(ScriptWriteKind::structure);
    for (const char* name : {"Left", "Right"}) {
        auto* array = alias.add(ScriptWriteKind::array);
        CHECK(array && array->name.set(name) && alias.root->children.add(array));
        for (int i = 0; i < 2; ++i) {
            auto* value = alias.add(ScriptWriteKind::text);
            CHECK(value && value->text.set("New") && array->children.add(value));
        }
    }
    const int allocated = engine.allocated, freed = engine.freed;
    CHECK(tsuchi::apply_script_write(mod, {handle(object), {}, nullptr}, "Pair", *alias.root) == TSUCHI_INVALID);
    CHECK(fake::get<uint64_t>(object + 0x40) == buffer && fake::get<uint64_t>(object + 0x50) == buffer + 8);
    CHECK(engine.allocated - allocated == engine.freed - freed && fake::get<uint64_t>(buffer) == 0);
    // Empty arrays still own their capacity. Equal buffers and unused-capacity intersections cannot retire twice.
    ScriptWriteTree capacity_alias;
    capacity_alias.root = capacity_alias.add(ScriptWriteKind::structure);
    for (const char* name : {"Left", "Right"}) {
        auto* array = capacity_alias.add(ScriptWriteKind::array);
        CHECK(array && array->name.set(name) && capacity_alias.root->children.add(array));
    }
    for (const bool empty : {false, true}) {
        fake::put(object + 0x40, buffer);
        fake::put(object + 0x50, empty ? buffer : buffer + 16);
        fake::put<int32_t>(object + 0x40 + fake::t::array::count, empty ? 0 : 1);
        fake::put<int32_t>(object + 0x40 + fake::t::array::max, 3);
        fake::put<int32_t>(object + 0x50 + fake::t::array::count, empty ? 0 : 1);
        fake::put<int32_t>(object + 0x50 + fake::t::array::max, empty ? 3 : 1);
        const int before_allocated = engine.allocated, before_freed = engine.freed;
        CHECK(tsuchi::apply_script_write(mod, {handle(object), {}, nullptr}, "Pair", *capacity_alias.root) == TSUCHI_INVALID);
        CHECK(fake::get<uint64_t>(object + 0x40) == buffer && fake::get<uint64_t>(object + 0x50) == (empty ? buffer : buffer + 16));
        CHECK(fake::get<int32_t>(object + 0x40 + fake::t::array::count) == (empty ? 0 : 1) && fake::get<int32_t>(object + 0x50 + fake::t::array::count) == (empty ? 0 : 1));
        CHECK(engine.allocated - before_allocated == engine.freed - before_freed);
    }
    tsuchi::game::release_engine_memory(reinterpret_cast<void*>(buffer));
}
