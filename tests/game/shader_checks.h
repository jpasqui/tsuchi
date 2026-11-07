// Checks of the manifest's "shaders": a shader set as a file, and a mod's materials drawn with their own
// shaders in the made-up game. Part of objects_test.cpp, which includes this before its `main`.
#pragma once

#include "shader_file.h"
#include "shader_materials.h"

// ---- A shader set, written as the file has it -------------------------------------------------------

struct SetShader {
    uint32_t place;
    std::string payload;
    uint8_t kind = tsuchi::shaders::kPixel;
};
struct SetOperation {
    bool fresh;          // new bytes, or a stretch of the game's description
    uint32_t from, length;
    std::string bytes;
};
struct SetName {
    std::string text;
    uint32_t number;
    std::vector<uint32_t> places;
};
struct SetMaterial {
    std::string material, game_material;
    uint8_t hash_first; // the hash of the shader map is twenty bytes that count up from this
    uint32_t map_shaders;
    std::vector<SetShader> shaders;
    uint32_t old_size = 0, new_size = 0; // with a description of its own
    std::vector<SetOperation> operations;
    std::vector<SetName> names;
};
std::vector<uint8_t> shader_set(const std::vector<SetMaterial>& materials, char format = '3') {
    std::vector<uint8_t> bytes;
    const auto u16 = [&](uint32_t v) { bytes.push_back(uint8_t(v)), bytes.push_back(uint8_t(v >> 8)); };
    const auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(uint8_t(v >> (8 * i)));
    };
    const auto text = [&](const std::string& s) {
        u16(uint32_t(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    };
    const std::string mark = std::string("P4MSHDR") + format;
    bytes.insert(bytes.end(), mark.begin(), mark.end());
    u32(uint32_t(materials.size()));
    for (const SetMaterial& material : materials) {
        text(material.material), text(material.game_material);
        for (uint32_t i = 0; i < tsuchi::shaders::kHashBytes; ++i) bytes.push_back(uint8_t(material.hash_first + i));
        u32(material.map_shaders), u32(uint32_t(material.shaders.size()));
        for (const SetShader& shader : material.shaders) {
            u32(shader.place), bytes.push_back(shader.kind);
            for (uint32_t i = 0; i < tsuchi::shaders::kHashBytes; ++i) bytes.push_back(uint8_t(shader.payload[0] + i)); // a hash that tells whose it is
            u32(uint32_t(shader.payload.size()));
            bytes.insert(bytes.end(), shader.payload.begin(), shader.payload.end());
        }
        u32(material.old_size ? 1 : 0);
        if (!material.old_size) continue;
        u32(material.old_size), u32(material.new_size), u32(uint32_t(material.operations.size()));
        for (const SetOperation& operation : material.operations) {
            bytes.push_back(operation.fresh ? 1 : 0);
            if (!operation.fresh) u32(operation.from), u32(operation.length);
            else u32(uint32_t(operation.bytes.size())), bytes.insert(bytes.end(), operation.bytes.begin(), operation.bytes.end());
        }
        u32(uint32_t(material.names.size()));
        for (const SetName& name : material.names) {
            text(name.text), u32(name.number), u32(uint32_t(name.places.size()));
            for (uint32_t place : name.places) u32(place);
        }
    }
    return bytes;
}

// The two materials the checks below use: one that keeps the game material's description, and one with a
// description of its own, which is the game's sixteen bytes with twelve new ones after the first four, a name in them.
SetMaterial lava_material(const std::string& game_material = "/G/MW", uint8_t hash_first = 0x10, uint32_t map_shaders = 5) {
    return {"/G/Mods/MI_Lava.MI_Lava", game_material, hash_first, map_shaders, {{1, "lava-one"}, {4, "lava-two"}}, 0, 0, {}, {}};
}
SetMaterial ice_material() {
    return {"/G/Mods/MI_Ice.MI_Ice", "/G/MW", 0x10, 5, {{0, "ice"}}, 16, 28, {{false, 0, 4, ""}, {true, 0, 0, "NNNNNNNNNNNN"}, {false, 4, 12, ""}}, {{"Tex", 7, {4}}}};
}

// A file of two materials is read, the second with a description of its own, which is built from the game's.
// A file with anything wrong in it is refused whole.
void shader_files() {
    namespace set = tsuchi::shaders;
    const std::vector<uint8_t> bytes = shader_set({lava_material(), ice_material()});
    set::Material rows[4];
    set::Shader programs[8];
    const auto read = [&](const std::vector<uint8_t>& from, uint32_t row_room = 4, uint32_t program_room = 8) {
        set::File file{from.data(), uint32_t(from.size()), rows, row_room, 0, programs, program_room, 0};
        const set::Read result = set::read(&file);
        set::Counts counts{};
        const set::Read measured = set::measure(from.data(), uint32_t(from.size()), counts);
        if (result == set::Read::ok) return file.material_count == 2 && file.shader_count == 3 && measured == result && counts.materials == 2 && counts.shaders == 3 ? result : set::Read::full;
        // Refused: nothing is counted, and measuring says the same unless it was the tables that had no room.
        if (file.material_count || file.shader_count) return set::Read::ok;
        if (result == set::Read::full) return measured == set::Read::ok ? result : set::Read::ok;
        return measured == result && !counts.materials && !counts.shaders ? result : set::Read::ok;
    };
    const auto text = [&](uint32_t at, uint32_t length) { return std::string(reinterpret_cast<const char*>(bytes.data()) + at, length); };
    CHECK(read(bytes) == set::Read::ok);
    CHECK(text(rows[0].material, rows[0].material_length) == "/G/Mods/MI_Lava.MI_Lava" && text(rows[0].game_material, rows[0].game_material_length) == "/G/MW" &&
          text(rows[0].leaf, rows[0].leaf_length) == "MW" && rows[0].map_shaders == 5 && rows[0].first_shader == 0 && rows[0].shader_count == 2 && rows[0].old_size == 0);
    CHECK(bytes[rows[0].map_hash] == 0x10 && bytes[rows[0].map_hash + 19] == 0x23 && programs[0].place == 1 && programs[1].place == 4 &&
          text(programs[0].payload, programs[0].size) == "lava-one" && text(programs[1].payload, programs[1].size) == "lava-two" && bytes[programs[1].hash] == 'l' &&
          bytes[programs[1].hash + 19] == 'l' + 19);
    CHECK(text(rows[1].material, rows[1].material_length) == "/G/Mods/MI_Ice.MI_Ice" && rows[1].first_shader == 2 && rows[1].shader_count == 1 && programs[2].place == 0 &&
          text(programs[2].payload, programs[2].size) == "ice" && rows[1].old_size == 16 && rows[1].new_size == 28 && rows[1].operation_count == 3 && rows[1].name_count == 1);
    // The description of the second: the game's first four bytes, the new twelve, and the game's last twelve.
    const std::string old = "0123456789abcdef";
    std::string built(28, '?');
    set::build_content(bytes.data(), rows[1], reinterpret_cast<const uint8_t*>(old.data()), reinterpret_cast<uint8_t*>(&built[0]));
    CHECK(built == "0123NNNNNNNNNNNN456789abcdef");
    std::string names;
    set::each_name(bytes.data(), rows[1], [&](const uint8_t* name, uint32_t length, uint32_t number, uint32_t place) {
        names += std::string(reinterpret_cast<const char*>(name), length) + ":" + std::to_string(number) + "@" + std::to_string(place) + " ";
    });
    CHECK(names == "Tex:7@4 ");
    // After what the tables already hold.
    set::File more{bytes.data(), uint32_t(bytes.size()), rows, 3, 2, programs, 8, 3};
    CHECK(set::read(&more) == set::Read::full && more.material_count == 2 && more.shader_count == 3);
    const std::vector<uint8_t> one = shader_set({lava_material()});
    set::File after{one.data(), uint32_t(one.size()), rows, 4, 2, programs, 8, 3};
    CHECK(set::read(&after) == set::Read::ok && after.material_count == 3 && after.shader_count == 5 && rows[2].first_shader == 3 && programs[4].place == 4);
    const std::vector<uint8_t> none = shader_set({});
    set::File empty{none.data(), uint32_t(none.size()), rows, 4, 0, programs, 8, 0};
    CHECK(set::read(&empty) == set::Read::ok && empty.material_count == 0);

    // Refused whole: another mark, or the mark of a kind of file this does not read; a file cut short at any
    // byte; bytes left over; and tables with no room.
    auto wrong = bytes;
    wrong[0] = 'Q';
    CHECK(read(wrong) == set::Read::not_a_set && read(shader_set({lava_material()}, '2')) == set::Read::not_a_set && read(shader_set({lava_material()}, '1')) == set::Read::not_a_set &&
          read(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 11)) == set::Read::not_a_set);
    for (size_t length = 12; length < bytes.size(); ++length) {
        const set::Read cut = read(std::vector<uint8_t>(bytes.begin(), bytes.begin() + ptrdiff_t(length)));
        if (cut != set::Read::cut_short && cut != set::Read::bad_value) std::printf("  cut to %zu bytes: %u\n", length, unsigned(cut));
        CHECK(cut == set::Read::cut_short || cut == set::Read::bad_value);
    }
    wrong = bytes, wrong.push_back(0);
    CHECK(read(wrong) == set::Read::bad_value);
    CHECK(read(bytes, 1, 8) == set::Read::full && read(bytes, 4, 2) == set::Read::full);
    // And anything out of range, each in a file that is otherwise right.
    const auto refused = [&](SetMaterial first, SetMaterial second = ice_material()) { return read(shader_set({first, second})) == set::Read::bad_value; };
    SetMaterial bad = lava_material();
    bad.material = "/G/Mods/MI_Lava"; // a package, where an object is named
    CHECK(refused(bad));
    bad = lava_material(), bad.material = "G/Mods/MI_Lava.MI_Lava";
    CHECK(refused(bad));
    bad = lava_material(), bad.material = "/G/Mods/MI Lava.MI_Lava";
    CHECK(refused(bad));
    bad = lava_material(), bad.material = "/G//MI_Lava.MI_Lava";
    CHECK(refused(bad));
    bad = lava_material(), bad.material = "/G/Mods/MI_Lava.MI_Lava.More";
    CHECK(refused(bad));
    bad = lava_material(), bad.material = "/G/Mods/.MI_Lava";
    CHECK(refused(bad));
    bad = lava_material(), bad.material = "/G/Mods/MI_Lava." + std::string(set::kPathLimit, 'a');
    CHECK(refused(bad));
    bad = lava_material("/G/MW.MW"); // an object, where a package is named
    CHECK(refused(bad));
    bad = lava_material("/G/MW/");
    CHECK(refused(bad));
    bad = lava_material("/G/MW", 0x10, 0); // a shader map with no shaders
    CHECK(refused(bad));
    bad = lava_material("/G/MW", 0x10, set::kMapShaderLimit + 1);
    CHECK(refused(bad));
    bad = lava_material("/G/MW", 0x10, 4); // a place past the end of the shader map
    CHECK(refused(bad));
    bad = lava_material("/G/MW", 0x10, 1); // more shaders than the shader map has
    CHECK(refused(bad));
    bad = lava_material(), bad.shaders.clear(); // no custom program: a donor-only copy is not a shader material
    CHECK(refused(bad));
    bad = lava_material(), bad.shaders[1].place = 1; // two shaders for one place
    CHECK(refused(bad));
    bad = lava_material(), bad.shaders[0].kind = 0; // a vertex shader
    CHECK(refused(bad));
    bad = lava_material(), bad.shaders[0].payload.assign(set::kShaderLimit + 1, 'x');
    CHECK(refused(bad));
    bad = ice_material(), bad.material = "/G/Mods/MI_Lava.MI_Lava"; // two entries for one material
    CHECK(refused(lava_material(), bad));
    bad = ice_material(), bad.new_size = 29; // edits that make fewer bytes than they say
    CHECK(refused(lava_material(), bad));
    bad = ice_material(), bad.new_size = 27;
    CHECK(refused(lava_material(), bad));
    bad = ice_material(), bad.operations[2].length = 13; // a stretch that runs past the end of the game's description
    CHECK(refused(lava_material(), bad));
    bad = ice_material(), bad.old_size = set::kContentLimit + 1;
    CHECK(refused(lava_material(), bad));
    bad = ice_material(), bad.names[0].places[0] = 17; // a name that would run past the end of the description
    CHECK(refused(lava_material(), bad));
    bad = ice_material(), bad.names[0].text = "";
    CHECK(refused(lava_material(), bad));
    bad = ice_material(), bad.names[0].text = "Te\tx";
    CHECK(refused(lava_material(), bad));
    bad = ice_material(), bad.names[0].text.assign(set::kNameLimit + 1, 'n');
    CHECK(refused(lava_material(), bad));
    wrong = bytes;
    for (int i = 0; i < 4; ++i) wrong[8 + i] = 0xFF; // more materials than a set may hold
    CHECK(read(wrong) == set::Read::bad_value);
    wrong = bytes, wrong[wrong.size() - 4 - 4 - 4 - 5 - 4 - 9 - 16 - 9 - 16] = 2; // an edit that is neither kind: the first operation's first byte
    CHECK(read(wrong) == set::Read::bad_value);
}

// ---- A mod's materials, drawn with their own shaders ---------------------------------------------------

// What the made-up renderer was asked to make, and what it made.
struct Rendered {
    std::string log;
    bool refuses = false;
    std::vector<uint64_t> made;
} rendered;
struct MadeShaderCode {
    const uint8_t* data;
    int32_t count;
};
struct MadeShaderReference {
    uint64_t shader;
    MadeShaderReference() : shader(0) {}
    MadeShaderReference(const MadeShaderReference& other) : shader(other.shader) {}
    ~MadeShaderReference() {}
};
MadeShaderReference game_make_pixel_shader(uint64_t renderer, MadeShaderCode code, const uint8_t* hash) {
    const bool right = fake::get<uint64_t>(renderer) == fake::main_image() + t::data::renderer_table && hash[0] == code.data[0];
    rendered.log += "make:" + std::string(reinterpret_cast<const char*>(code.data), size_t(code.count)) + (right ? " " : "? ");
    MadeShaderReference made;
    if (!rendered.refuses) rendered.made.push_back(made.shader = fake::memory(0x40));
    return made;
}

void shading() {
    const uint64_t kIn = t::constant::property_param, kOut = kIn | t::constant::property_out, kResult = kOut | t::constant::property_return;
    const uint64_t main = fake::main_image();
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/";
    const TSUCHI_ComponentService& parts_api = *tsuchi::host.component;
    const auto plain = [&](const std::string& id, const std::string& more = "") {
        disk.files[mods_at + id + "/mod.json"] = "{\"id\": \"" + id + "\", \"name\": \"The " + id + " mod\", \"version\": \"1.0.0\"" + more + "}";
    };
    const auto set_file = [&](const std::string& id, const std::vector<uint8_t>& bytes) {
        plain(id, R"(, "shaders": "shaders.bin")");
        disk.files[mods_at + id + "/shaders.bin"] = std::string(bytes.begin(), bytes.end());
    };
    const auto problem = [](const char* id) {
        const tsuchi::ModRecord* record = tsuchi::find_mod(id);
        return std::string(!record ? "<no such mod>" : record->state == tsuchi::ModState::blocked ? record->problem.get() : record->state == tsuchi::ModState::ready ? "<ready>" : "<running>");
    };
    const auto steps = [](int count) {
        for (int i = 0; i < count; ++i) tsuchi::frame(0.03f);
    };
    const auto told = [&](const std::string& part) { return logged.lines.find(part) != std::string::npos; };
    const auto text_of = [](uint64_t object) {
        char text[64] = "?";
        tsuchi::game::name_text(tsuchi::game::name_of(object), text, sizeof text);
        return std::string(text);
    };
    // An object the engine knows by its name, as the engine would have named it: one name for one text.
    const auto named = [](uint64_t object, const std::u16string& text) {
        fake::put<uint64_t>(object + t::object::name, fake::engine_name(const_cast<char16_t*>(text.data()), text.size(), 1));
        return object;
    };
    const auto counted = [] { return tsuchi::shader_counts(); };

    // The engine's side: material instances, the function that loads a level into the world, and the two that
    // make a dynamic instance and give it another instance's values. The class a part of the checks before this
    // made for streaming levels is gone, and this one is the engine's.
    struct Scene {
        std::string log;
        uint64_t world = 0;
        int refuse = 0; // how many of the next requests for a level the engine will not take
        std::vector<uint64_t> stand_ins;
    } scene;
    for (tsuchi::game::ClassRef old{"LevelStreamingDynamic"}; const uint64_t type = tsuchi::game::find_class(old);) fake::destroy(type);
    const uint64_t instance_type = fake::object(u"MaterialInstanceConstant", w.root), dynamic_type = fake::object(u"MaterialInstanceDynamic", w.root);
    const uint64_t library_type = fake::object(u"KismetMaterialLibrary", w.root), stream_type = fake::object(u"LevelStreamingDynamic", w.root);
    const uint64_t world_type = fake::object(u"World", w.root);
    fake::set_super(instance_type, material_class), fake::set_super(dynamic_type, material_class);
    fake::field(instance_type, u"Parent", u"ObjectProperty", 0xa0, 8, material_class);
    fake::put<uint64_t>(library_type + t::class_type::default_object, fake::engine_object(u"LibraryDefault", library_type));
    fake::put<uint64_t>(stream_type + t::class_type::default_object, fake::engine_object(u"StreamDefault", stream_type));
    const uint64_t load = fake::function(stream_type, function_type, u"LoadLevelInstance", 0x28, 8, [&](uint64_t, uint64_t arguments) {
        std::string name;
        const uint64_t data = fake::get<uint64_t>(arguments + 8 + t::array::data);
        for (int32_t i = 0; i + 1 < fake::get<int32_t>(arguments + 8 + t::array::count); ++i) name += char(fake::get<uint16_t>(data + 2 * i));
        const bool refused = scene.refuse > 0;
        scene.refuse -= refused;
        scene.log += "copy:" + name + (fake::get<uint64_t>(arguments) == scene.world ? "" : "?world") + (refused ? "? " : " ");
        fake::put<uint8_t>(arguments + 0x18, !refused), fake::put<uint64_t>(arguments + 0x20, 0);
    }, t::constant::function_static);
    fake::flag(fake::field(load, u"WorldContextObject", u"ObjectProperty", 0, 8, world_type), kIn);
    fake::flag(fake::field(load, u"LevelName", u"StrProperty", 8, 0x10), kIn);
    fake::flag(fake::bit_field(load, u"bOutSuccess", 0x18, 0, 0xFF), kOut);
    fake::flag(fake::field(load, u"ReturnValue", u"ObjectProperty", 0x20, 8, stream_type), kResult);
    const uint64_t make = fake::function(library_type, function_type, u"CreateDynamicMaterialInstance", 0x18, 8, [&](uint64_t, uint64_t arguments) {
        const uint64_t parent = fake::get<uint64_t>(arguments + 8);
        const uint64_t made = fake::engine_object(u"Stand" + std::u16string(1, char16_t(u'1' + scene.stand_ins.size())), dynamic_type);
        scene.stand_ins.push_back(made);
        scene.log += "stand-in:" + text_of(tsuchi::game::outer_of(parent)) + (fake::get<uint64_t>(arguments) == scene.world ? " " : "?world ");
        fake::put(arguments + 0x10, made);
    }, t::constant::function_static);
    fake::flag(fake::field(make, u"WorldContextObject", u"ObjectProperty", 0, 8, world_type), kIn);
    fake::flag(fake::field(make, u"Parent", u"ObjectProperty", 8, 8, material_class), kIn);
    fake::flag(fake::field(make, u"ReturnValue", u"ObjectProperty", 0x10, 8, dynamic_type), kResult);
    const uint64_t values = fake::function(dynamic_type, function_type, u"CopyParameterOverrides", 8, 8,
                                           [&](uint64_t object, uint64_t arguments) { scene.log += "values:" + text_of(object) + "<" + text_of(fake::get<uint64_t>(arguments)) + " "; });
    fake::flag(fake::field(values, u"MaterialInstance", u"ObjectProperty", 0, 8, material_class), kIn);

    // The renderer: an object the game points at, known by its function table, with the function that makes a pixel shader in it.
    const uint64_t renderer = fake::memory(0x20);
    fake::put(renderer, main + t::data::renderer_table), fake::put(main + t::data::renderer, renderer);
    fake::put(main + t::data::renderer_table + t::renderer_table::make_pixel_shader, &game_make_pixel_shader);
    fake::code().push_back(reinterpret_cast<uint64_t>(&game_make_pixel_shader));
    rendered = Rendered{};

    // A material of the game's with shaders of its own, or a copy of one: the instance in its package, what it is
    // rendered through, its shader map, and the shader map's list of shaders.
    struct Built {
        uint64_t instance, resource, map, map_resource, list;
    };
    const std::string description = "0123456789abcdef";
    const auto shader_map = [&](uint8_t hash_first, uint32_t shaders) {
        const uint64_t map = fake::memory(t::shader_map::size), map_resource = fake::memory(t::shader_map_resource::size), list = fake::memory(8 * shaders);
        const uint64_t content = fake::memory(description.size());
        std::memcpy(reinterpret_cast<void*>(content), description.data(), description.size());
        std::memset(reinterpret_cast<void*>(map), 0xB7, t::shader_map::size), std::memset(reinterpret_cast<void*>(map_resource), 0xC9, t::shader_map_resource::size);
        fake::put(map + t::shader_map::resource, map_resource), fake::put(map + t::shader_map::content, content);
        fake::put<uint32_t>(map + t::shader_map::content_bytes, uint32_t(description.size())), fake::put<int32_t>(map + t::shader_map::references, 2);
        fake::put<uint8_t>(map + t::shader_map::flags, uint8_t(t::constant::shader_map_registered | 0x10));
        for (uint32_t i = 0; i < tsuchi::shaders::kHashBytes; ++i) fake::put<uint8_t>(map + t::shader_map::id_hash + i, uint8_t(hash_first + i));
        fake::put(map_resource + t::shader_map_resource::shaders, list), fake::put<uint32_t>(map_resource + t::shader_map_resource::shader_count, shaders);
        fake::put<int32_t>(map_resource + t::shader_map_resource::references, 3), fake::put<uint8_t>(map_resource + t::shader_map_resource::preloaded, 1);
        fake::put<uint64_t>(list + 8 * 2, 0x7777); // a shader the game has drawn with already
        return Built{0, 0, map, map_resource, list};
    };
    const auto material_in = [&](const std::u16string& package_name, const std::u16string& name, Built with, int32_t resources = 1) {
        const uint64_t package = fake::object(package_name, w.root, 0x40);
        with.instance = named(fake::engine_object(name, instance_type, 0x100), name);
        with.resource = fake::memory(0x40);
        fake::put(with.instance + t::object::outer, package);
        const uint64_t data = fake::memory(8);
        fake::put(data, with.resource), fake::put(with.instance + t::material_instance::resources + t::array::data, data);
        fake::put<int32_t>(with.instance + t::material_instance::resources + t::array::count, resources);
        fake::put(with.resource + t::material_resource::instance, with.instance);
        fake::put(with.resource + t::material_resource::game_map, with.map), fake::put(with.resource + t::material_resource::render_map, with.map);
        return with;
    };
    const auto on_shelf = [&](const std::u16string& path, uint64_t asset) { shelf.assets[fake::engine_name(const_cast<char16_t*>(path.data()), path.size(), 1)] = asset; };
    t::fn::load_soft_object = game_load_soft_object;
    const Built wall = material_in(u"/G/MW", u"MW", shader_map(0x10, 5));
    const Built bare = material_in(u"/G/MB", u"MB", shader_map(0x10, 5), 0); // an instance that draws with its parent's shaders
    on_shelf(u"/G/MW.MW", wall.instance), on_shelf(u"/G/MB.MB", bare.instance);
    // A copy the engine has loaded, which shares the game material's shader map.
    const auto copy_comes = [&](int number) { return material_in(u"/G/MW_LevelInstance_" + std::u16string(1, char16_t(u'0' + number)), u"MW", Built{0, 0, wall.map, wall.map_resource, wall.list}); };
    // A material instance of a mod's own, once its level has brought it.
    const auto mod_material = [&](const std::u16string& name) {
        const uint64_t package = fake::object(u"/G/Mods/" + name, w.root, 0x40), instance = named(fake::engine_object(name, instance_type, 0x100), name);
        fake::put(instance + t::object::outer, package);
        return instance;
    };
    // A scenery mesh belongs to an ordinary engine Actor, with the synthetic
    // base lifecycle table supplied by this test's factory. It is not a GActor.
    const auto mesh = [&](const std::u16string& name, int32_t slots) {
        const uint64_t owner = fake::engine_object(name + u"Actor", actor_base_class, 0x100);
        const uint64_t part = fake::engine_object(name, mesh_class, 0x100);
        fake::put(part + t::scene_component::owner, owner);
        slots_of[part] = slots;
        return part;
    };
    const auto in_world = [&](const std::u16string& package_path, const std::u16string& name) {
        const uint64_t package = fake::object(package_path, w.root, 0x40), world = fake::object(name, world_type, 0x40);
        fake::put(world + t::object::outer, package);
        return world;
    };
    const std::map<uint64_t, int32_t> other_slots = slots_of; // the meshes of the checks before this have no slots meanwhile
    const uint64_t other_stock = stock_material;
    slots_of.clear();

    // The mods. Two bring sets that fit; the others each have one thing wrong.
    const char* const made[] = {"shade.one", "shade.three", "shade.two", "shade.list", "shade.out", "shade.nofile", "shade.old", "shade.cut", "shade.kind",
                                "shade.hash", "shade.count", "shade.size", "shade.lost", "shade.bare", "shade.empty"};
    SetMaterial moss = lava_material(), reed = lava_material(), rush = lava_material();
    moss.material = "/G/Mods/MI_Moss.MI_Moss", reed.material = "/G/Mods/MI_Reed.MI_Reed", rush.material = "/G/Mods/MI_Rush.MI_Rush";
    moss.shaders = {{3, "moss"}}, reed.shaders = {{0, "reed"}}, rush.shaders = {{0, "rush-one"}, {1, "rush-two"}};
    set_file("shade.one", shader_set({lava_material(), ice_material()}));
    set_file("shade.two", shader_set({moss}));
    set_file("shade.three", shader_set({reed, rush}));
    plain("shade.list", R"(, "shaders": ["shaders.bin"])");
    plain("shade.out", R"(, "shaders": "../shade.one/shaders.bin")");
    plain("shade.nofile", R"(, "shaders": "sets/mine.bin")");
    set_file("shade.old", shader_set({lava_material()}, '2'));
    std::vector<uint8_t> damaged = shader_set({lava_material()});
    damaged.resize(damaged.size() - 9);
    set_file("shade.cut", damaged);
    SetMaterial vertex = lava_material();
    vertex.shaders[1].kind = 0;
    set_file("shade.kind", shader_set({vertex}));
    set_file("shade.hash", shader_set({ice_material(), lava_material("/G/MW", 0x11)}));
    set_file("shade.count", shader_set({lava_material("/G/MW", 0x10, 6)}));
    SetMaterial longer = ice_material();
    longer.old_size = 17, longer.new_size = 29, longer.operations[2].length = 13;
    set_file("shade.size", shader_set({longer}));
    set_file("shade.lost", shader_set({lava_material("/G/Gone")}));
    set_file("shade.bare", shader_set({lava_material("/G/MB")}));
    set_file("shade.empty", shader_set({}));
    const tsuchi::ShaderCounts before = counted();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    CHECK(problem("shade.list") == "mod.json: \"shaders\" is the path of a file in the mod's folder: its shader set" && problem("shade.out") == problem("shade.list") &&
          problem("shade.one") == "<ready>" && problem("shade.hash") == "<ready>");
    logged.lines.clear(), scene.log.clear();
    tsuchi::start_mods();
    CHECK(problem("shade.one") == "<running>" && problem("shade.two") == "<running>" && problem("shade.three") == "<running>" && problem("shade.empty") == "<running>");
    CHECK(problem("shade.nofile") == "mod.json: \"shaders\": sets/mine.bin: the mod has no such file");
    // A file that is no shader set, or is damaged, blocks its mod.
    CHECK(problem("shade.old") == "mod.json: \"shaders\": shaders.bin is no shader set, or one of a kind this framework does not read");
    CHECK(problem("shade.cut") == "mod.json: \"shaders\": shaders.bin is cut short");
    CHECK(problem("shade.kind") == "mod.json: \"shaders\": shaders.bin is damaged: something in it is out of range");
    // A set compiled for other shaders than the game material has here blocks its mod: by the hash of the shader
    // map, by how many shaders it has, and by the size of the description a material's own is made from.
    const std::string other_version = "mod.json: \"shaders\": shaders.bin was made for another version of the game: the shaders of /G/MW are not the ones it was compiled for";
    CHECK(problem("shade.hash") == other_version && problem("shade.count") == other_version && problem("shade.size") == other_version);
    CHECK(problem("shade.lost") == "mod.json: \"shaders\": shaders.bin: the game has no material /G/Gone");
    CHECK(problem("shade.bare") == "mod.json: \"shaders\": shaders.bin: /G/MB is no material of the game's with shaders of its own");
    CHECK(counted().sets == before.sets + 3 && counted().materials == before.materials + 5 && counted().files_refused == before.files_refused + 3 &&
          counted().sets_refused == before.sets_refused + 5);
    // Nothing of the game's is changed as a mod starts, and the game material stays loaded for the mod.
    CHECK(scene.log.empty() && rendered.log.empty() && tsuchi::game::object_kept(wall.instance) && !told("shaders:"));

    // A world is up. Each material's copy is asked for, one at a time, in the order the mods started.
    const uint64_t old_world = fake::get<uint64_t>(main + t::data::world);
    const uint8_t old_begun = fake::get<uint8_t>(w.framework + t::game_framework::world_begun);
    const uint64_t cave = in_world(u"/G/Cave_P", u"Cave_P"), garden = in_world(u"/G/Garden_P", u"Garden_P");
    scene.world = cave;
    fake::put(main + t::data::world, cave), fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 1);
    const auto look = [&] { steps(15); }; // the world is looked at every fifteenth step while it settles
    steps(29);
    CHECK(scene.log.empty());
    steps(1);
    CHECK(scene.log == "copy:/G/MW " && counted().copies_asked == before.copies_asked + 1);
    look(), look();
    CHECK(scene.log == "copy:/G/MW " && rendered.log.empty());
    // The copy comes: it is kept, and at the look after that it has a shader map of its own. The list of
    // shaders is empty but for the mod's two, which the renderer made and which carry their hashes; the lock
    // is one nobody holds; and the engine will never count either down to nothing. The game's own are as they were.
    const Built first = copy_comes(1);
    look();
    CHECK(tsuchi::game::object_kept(first.instance) && rendered.log.empty() && fake::get<uint64_t>(first.resource + t::material_resource::game_map) == wall.map);
    look();
    CHECK(rendered.log == "make:lava-one make:lava-two " && rendered.made.size() == 2 && counted().shaders_made == before.shaders_made + 2 &&
          counted().copies_ready == before.copies_ready + 1);
    const uint64_t own_map = fake::get<uint64_t>(first.resource + t::material_resource::game_map);
    const uint64_t own_resource = fake::get<uint64_t>(own_map + t::shader_map::resource), own_list = fake::get<uint64_t>(own_resource + t::shader_map_resource::shaders);
    CHECK(own_map != wall.map && fake::get<uint64_t>(first.resource + t::material_resource::render_map) == own_map && own_resource != wall.map_resource && own_list != wall.list);
    CHECK(fake::get<int32_t>(own_map + t::shader_map::references) == 1 << 29 && fake::get<uint8_t>(own_map + t::shader_map::flags) == 0x10 &&
          fake::get<uint8_t>(own_map + t::shader_map::id_hash + 19) == 0x23 && fake::get<uint64_t>(own_map + t::shader_map::content) == fake::get<uint64_t>(wall.map + t::shader_map::content) &&
          fake::get<uint8_t>(own_map + t::shader_map::flags + 1) == 0xB7);
    CHECK(fake::get<int32_t>(own_resource + t::shader_map_resource::references) == 1 << 29 && fake::get<uint8_t>(own_resource + t::shader_map_resource::preloaded) == 0 &&
          fake::get<uint32_t>(own_resource + t::shader_map_resource::shader_count) == 5 && fake::get<uint8_t>(own_resource) == 0xC9);
    for (uint32_t i = 0; i < t::system_mutex::size; ++i)
        CHECK(fake::get<uint8_t>(own_resource + t::shader_map_resource::lock + i) == (i == t::system_mutex::state ? t::constant::mutex_ready : i == t::system_mutex::recursive ? 1 : 0));
    CHECK(rendered.made.size() == 2 && fake::get<uint64_t>(own_list + 8 * 1) == rendered.made[0] && fake::get<uint64_t>(own_list + 8 * 4) == rendered.made[1] &&
          !fake::get<uint64_t>(own_list) && !fake::get<uint64_t>(own_list + 8 * 2) && !fake::get<uint64_t>(own_list + 8 * 3));
    CHECK(rendered.made.size() == 2 && fake::get<uint8_t>(rendered.made[1] + t::shader::hash) == 'l' && fake::get<uint8_t>(rendered.made[1] + t::shader::hash + 19) == 'l' + 19);
    CHECK(fake::get<uint64_t>(wall.resource + t::material_resource::game_map) == wall.map && fake::get<uint64_t>(wall.map + t::shader_map::resource) == wall.map_resource &&
          fake::get<int32_t>(wall.map + t::shader_map::references) == 2 && fake::get<uint8_t>(wall.map + t::shader_map::flags) == (t::constant::shader_map_registered | 0x10) &&
          fake::get<uint64_t>(wall.list + 8 * 2) == 0x7777 && fake::get<uint8_t>(wall.map_resource + t::shader_map_resource::preloaded) == 1);
    // In the same look the next material's copy is asked for. The copy that comes for it is not the one the
    // first took, and the engine has not finished with it at first: it is left until it has what it is
    // rendered through. It gets a description of its own: the game's with the set's edits, and the name where
    // the set says, as the engine numbers it.
    CHECK(scene.log == "copy:/G/MW copy:/G/MW ");
    const Built second = copy_comes(2);
    fake::put<int32_t>(second.instance + t::material_instance::resources + t::array::count, 0);
    look(), look(), look();
    CHECK(rendered.log == "make:lava-one make:lava-two " && scene.log == "copy:/G/MW copy:/G/MW " && tsuchi::game::object_kept(second.instance));
    fake::put<int32_t>(second.instance + t::material_instance::resources + t::array::count, 1);
    scene.refuse = 1, scene.log.clear();
    look();
    const uint64_t ice_map = fake::get<uint64_t>(second.resource + t::material_resource::game_map);
    const uint64_t ice_content = fake::get<uint64_t>(ice_map + t::shader_map::content);
    const std::u16string tex = u"Tex";
    const uint32_t tex_index = uint32_t(fake::engine_name(const_cast<char16_t*>(tex.data()), tex.size(), 1));
    CHECK(rendered.log == "make:lava-one make:lava-two make:ice " && ice_map != wall.map && ice_map != own_map && fake::get<uint64_t>(first.resource + t::material_resource::game_map) == own_map);
    CHECK(fake::get<uint32_t>(ice_map + t::shader_map::content_bytes) == 28 && ice_content != fake::get<uint64_t>(wall.map + t::shader_map::content) &&
          !std::memcmp(reinterpret_cast<const void*>(ice_content), "0123", 4) && !std::memcmp(reinterpret_cast<const void*>(ice_content + 16), "456789abcdef", 12) &&
          fake::get<uint32_t>(ice_content + 4 + t::frozen_name::number) == 7 && fake::get<uint32_t>(ice_content + 4 + t::frozen_name::comparison) == tex_index &&
          fake::get<uint32_t>(ice_content + 4 + t::frozen_name::display) == tex_index && counted().descriptions == before.descriptions + 1);
    CHECK(fake::get<uint32_t>(wall.map + t::shader_map::content_bytes) == 16 && !std::memcmp(reinterpret_cast<const void*>(fake::get<uint64_t>(wall.map + t::shader_map::content)), description.data(), 16));
    // The engine will not load a package a second time: that material keeps the game material's shaders in this
    // world, its mod is told, and the next material is asked for. The renderer makes nothing of a shader: that
    // material keeps them for good, and the copy is left as it came.
    CHECK(scene.log == "copy:/G/MW? copy:/G/MW " && counted().failed == before.failed + 1 && counted().last_failure == tsuchi::ShaderFailure::no_copy);
    CHECK(told("[shade.three] shaders: /G/Mods/MI_Reed.MI_Reed is drawn with the shaders of /G/MW: the engine did not load the game material a second time\n"));
    const Built third = copy_comes(3);
    rendered.refuses = true, scene.log.clear();
    look(), look();
    rendered.refuses = false;
    CHECK(rendered.log == "make:lava-one make:lava-two make:ice make:rush-one " && fake::get<uint64_t>(third.resource + t::material_resource::game_map) == wall.map &&
          fake::get<uint64_t>(third.resource + t::material_resource::render_map) == wall.map && counted().failed == before.failed + 2 &&
          counted().last_failure == tsuchi::ShaderFailure::shader_refused && counted().copies_ready == before.copies_ready + 2);
    CHECK(told("[shade.three] shaders: /G/Mods/MI_Rush.MI_Rush is drawn with the shaders of /G/MW: the game's renderer made nothing of one of its shaders\n") &&
          !told("[shade.one]") && !told("[shade.two]"));
    CHECK(scene.log == "copy:/G/MW "); // the last material's, of the mod that started last

    // The mod's own instance is loaded, and meshes are drawn with it: one that has it put in a slot, and two
    // whose own material it is. A stand-in is made from the copy, with the instance's values, and put in each of
    // those slots, the slot it was found put in first.
    scene.log.clear(), part_log.clear();
    const uint64_t lava = mod_material(u"MI_Lava");
    const uint64_t box = mesh(u"Box", 2), crate = mesh(u"Crate", 1);
    put_override(box, 1, lava);
    stock_material = lava;
    look();
    CHECK(scene.log == "stand-in:/G/MW_LevelInstance_1 values:Stand1<MI_Lava " && scene.stand_ins.size() == 1 && counted().stand_ins == before.stand_ins + 1);
    CHECK(part_log == "set:Box[1]=Stand1 set:Crate[0]=Stand1 set:Box[0]=Stand1 " && counted().slots_set == before.slots_set + 3);
    // Done once for a mesh that stays as it is.
    look(), look();
    CHECK(part_log == "set:Box[1]=Stand1 set:Crate[0]=Stand1 set:Box[0]=Stand1 " && scene.log == "stand-in:/G/MW_LevelInstance_1 values:Stand1<MI_Lava ");
    // The game puts the mod's instance in a slot afterward: found at the next look. A mesh that comes later: the same.
    part_log.clear();
    put_override(crate, 0, lava);
    const uint64_t late = mesh(u"Late", 1);
    look();
    CHECK(part_log == "set:Crate[0]=Stand1 set:Late[0]=Stand1 ");
    // An actor of the game's that enters play is looked at on the next frame, with no look at the world. One the
    // game takes from its pool again, with its materials as the game made them, is looked at again.
    part_log.clear();
    const uint64_t totem = tabled(table_a(), u"Totem", u"Totem", 0, 0), face = mesh(u"Face", 1);
    fake::put(face + t::scene_component::owner, totem);
    parts_of[totem].push_back(face);
    registers(totem);
    CHECK(part_log.empty());
    steps(1);
    CHECK(part_log == "set:Face[0]=Stand1 ");
    steps(3);
    pool_return(kPool, totem);
    put_override(face, 0, 0), fake::put<uint32_t>(totem + t::game_actor::rest, 0);
    pool_gives = totem;
    CHECK(pool_take() == totem && part_log == "set:Face[0]=Stand1 set:Face[0]=none ");
    steps(1);
    CHECK(part_log == "set:Face[0]=Stand1 set:Face[0]=none set:Face[0]=Stand1 " && override_in(face, 0) == scene.stand_ins[0]);
    // A slot a mod holds with the instance is drawn with the stand-in, and goes back to what it had.
    stock_material = other_stock;
    part_log.clear();
    const uint64_t shelf_mesh = mesh(u"Shelf", 1);
    const TSUCHI_Component shelf_part{to_handle(shelf_mesh)};
    const uint32_t prior_holds=tsuchi::material_holds();
    CHECK(parts_api.hold_material(mod, shelf_part, 0, TSUCHI_Object{to_handle(lava)}) == TSUCHI_OK && part_log == "set:Shelf[0]=Stand1 " && tsuchi::material_holds() == prior_holds + 1);
    steps(2);
    CHECK(part_log == "set:Shelf[0]=Stand1 " && parts_api.release_material(mod, shelf_part, 0) == TSUCHI_OK && part_log == "set:Shelf[0]=Stand1 set:Shelf[0]=none ");

    // A mod hold on a slot with the framework's stand-in may end without losing that stand-in.
    const TSUCHI_Component fallback_part{to_handle(crate)};
    const uint64_t fallback=override_in(crate,0);
    CHECK(parts_api.hold_material(mod,fallback_part,0,TSUCHI_Object{to_handle(lava)})==TSUCHI_OK);
    CHECK(parts_api.release_material(mod,fallback_part,0)==TSUCHI_OK && override_in(crate,0)==fallback);
    steps(1);
    CHECK(override_in(crate,0)==fallback);

    // A different held material takes precedence over the fallback on every frame. Releasing
    // the fallback while the mod holds the slot leaves that material alone.
    CHECK(parts_api.hold_material(mod, fallback_part, 0, TSUCHI_Object{to_handle(red_material)}) == TSUCHI_OK);
    steps(3);
    CHECK(override_in(crate, 0) == red_material);
    CHECK(tsuchi::restore_material(nullptr, crate, 0) == TSUCHI_OK && override_in(crate, 0) == red_material);
    steps(1);
    CHECK(override_in(crate, 0) == red_material);
    CHECK(parts_api.release_material(mod, fallback_part, 0) == TSUCHI_OK && override_in(crate, 0) == fallback);
    put_override(crate, 0, lava);
    look();
    CHECK(override_in(crate, 0) == fallback);

    // Stopping a mod also unblocks the fallback, so a later game change is taken again.
    TSUCHI_Mod* overlay = tsuchi::mod_create("materials.overlay");
    CHECK(parts_api.hold_material(overlay, fallback_part, 0, TSUCHI_Object{to_handle(red_material)}) == TSUCHI_OK);
    steps(2);
    CHECK(override_in(crate, 0) == red_material);
    tsuchi::mod_stop(overlay);
    CHECK(override_in(crate, 0) == fallback);
    put_override(crate, 0, lava);
    steps(1);
    CHECK(override_in(crate, 0) == fallback);

    // Another world. The copy and its shaders are as they were; a stand-in is made for this world, and the
    // meshes here get it. The copy the engine would not load in the world before is asked for again, and after
    // it the one that was asked for there and did not come.
    for (uint64_t gone : {box, crate, late, face, shelf_mesh}) fake::destroy(gone);
    scene.log.clear(), part_log.clear(), scene.world = garden;
    fake::put(main + t::data::world, garden);
    const uint64_t bed = mesh(u"Bed", 1), row = mesh(u"Row", 1);
    put_override(bed, 0, lava);
    steps(29);
    CHECK(scene.log.empty() && part_log.empty());
    steps(1);
    CHECK(scene.log == "copy:/G/MW stand-in:/G/MW_LevelInstance_1 values:Stand2<MI_Lava " && part_log == "set:Bed[0]=Stand2 " && rendered.log == "make:lava-one make:lava-two make:ice make:rush-one ");
    CHECK(fake::get<uint64_t>(first.resource + t::material_resource::game_map) == own_map);
    const Built fourth = copy_comes(4);
    look(), look();
    CHECK(rendered.log == "make:lava-one make:lava-two make:ice make:rush-one make:reed " && tsuchi::game::object_kept(fourth.instance) &&
          scene.log == "copy:/G/MW stand-in:/G/MW_LevelInstance_1 values:Stand2<MI_Lava copy:/G/MW ");
    const Built fifth = copy_comes(5);
    look(), look();
    CHECK(rendered.log == "make:lava-one make:lava-two make:ice make:rush-one make:reed make:moss " && tsuchi::game::object_kept(fifth.instance) &&
          counted().copies_ready == before.copies_ready + 4);
    // The second mod's material is put in a slot by a hold before the world was looked at again: it is known
    // by its path, its stand-in is made then, from that mod's copy, and the slot gets it.
    scene.log.clear(), part_log.clear();
    const uint64_t moss_material = mod_material(u"MI_Moss"), post = mesh(u"Post", 1);
    const TSUCHI_Component post_part{to_handle(post)};
    CHECK(parts_api.hold_material(mod, post_part, 0, TSUCHI_Object{to_handle(moss_material)}) == TSUCHI_OK);
    CHECK(scene.log == "stand-in:/G/MW_LevelInstance_5 values:Stand3<MI_Moss " && part_log == "set:Post[0]=Stand3 ");
    stock_material = lava;
    look();
    CHECK(part_log == "set:Post[0]=Stand3 set:Row[0]=Stand2 ");

    // Missing map-specific instances cannot require a full material walk on every look.
    const uint32_t searches_before=counted().material_searches;
    steps(240);
    CHECK(counted().material_searches<=searches_before+1);

    // A mod stops: each slot its stand-in was put in gets back what it had, the mod's instance or nothing over
    // the mesh's own. Its copies stay kept, with their shaders. The other mod's material is as it was.
    part_log.clear(), scene.log.clear();
    const tsuchi::ShaderCounts running = counted();
    tsuchi::mod_stop(tsuchi::find_mod("shade.one")->context);
    CHECK(part_log == "set:Bed[0]=MI_Lava set:Row[0]=none " && counted().slots_put_back == running.slots_put_back + 2 && counted().sets == running.sets - 1 &&
          counted().materials == running.materials - 2);
    CHECK(tsuchi::game::object_kept(first.instance) && fake::get<uint64_t>(first.resource + t::material_resource::game_map) == own_map && override_in(post, 0) == scene.stand_ins[2]);
    look(), look();
    CHECK(part_log == "set:Bed[0]=MI_Lava set:Row[0]=none " && scene.log.empty());
    // Every mod stops. A slot that is still held with a material of one of them is drawn with the material itself.
    part_log.clear();
    tsuchi::forget_mods();
    CHECK(counted().sets == before.sets && counted().materials == before.materials && part_log.empty());
    steps(1);
    CHECK(part_log == "set:Post[0]=MI_Moss " && parts_api.release_material(mod, post_part, 0) == TSUCHI_OK && part_log == "set:Post[0]=MI_Moss set:Post[0]=none ");
    look(), look(), look();
    CHECK(scene.log.empty() && part_log == "set:Post[0]=MI_Moss set:Post[0]=none ");

    #include "shader_target_checks.h"

    // A mod that starts again gets copies of its own again: one a mod had before is not taken for the new one.
    // A copy that does not come is given up after a while, and the mod is told. It is asked for again in the
    // next world.
    for (const char* id : made)
        if (std::string(id) != "shade.two") disk.files.erase(mods_at + id + "/mod.json"), disk.files.erase(mods_at + id + "/shaders.bin");
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    logged.lines.clear(), scene.log.clear();
    tsuchi::start_mods();
    steps(30);
    CHECK(problem("shade.two") == "<running>" && scene.log == "copy:/G/MW ");
    steps(900);
    CHECK(!told("[shade.two] shaders:") && rendered.log == "make:lava-one make:lava-two make:ice make:rush-one make:reed make:moss ");
    steps(240);
    CHECK(told("[shade.two] shaders: /G/Mods/MI_Moss.MI_Moss is drawn with the shaders of /G/MW: the engine did not load the game material a second time\n") &&
          rendered.log == "make:lava-one make:lava-two make:ice make:rush-one make:reed make:moss " && scene.log == "copy:/G/MW ");
    scene.world = cave;
    fake::put(main + t::data::world, cave);
    steps(30);
    CHECK(scene.log == "copy:/G/MW copy:/G/MW ");
    // No memory at any point as a mod with a set starts: the mod is blocked, and nothing is left of its set. A
    // mod's context is the one block that stays when it stops.
    const std::map<std::string, std::string> other_files = disk.files;
    disk.files = {{mods_at + "shade.two/mod.json", other_files.at(mods_at + "shade.two/mod.json")}, {mods_at + "shade.two/shaders.bin", other_files.at(mods_at + "shade.two/shaders.bin")}};
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(problem("shade.two") == "<running>" && counted().sets == before.sets + 1);
    tsuchi::forget_mods();
    const size_t set_bytes = other_files.at(mods_at + "shade.two/shaders.bin").size();
    const int of_that_size = blocks_of(set_bytes);
    for (int left = 0;; ++left) {
        const size_t settled = blocks_out.size();
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        allocations_left = left;
        tsuchi::start_mods();
        allocations_left = -1;
        const bool started = problem("shade.two") == "<running>";
        CHECK(counted().sets == before.sets + (started ? 1 : 0));
        tsuchi::forget_mods();
        if (blocks_out.size() > settled + 1) std::printf("  with memory for %d blocks: %zu blocks out, after %zu\n", left, blocks_out.size(), settled);
        CHECK(blocks_out.size() <= settled + 1 && blocks_of(set_bytes) == of_that_size && counted().sets == before.sets && counted().materials == before.materials);
        if (started || left > 400) {
            CHECK(started && left > 5);
            break;
        }
    }
    disk.files = other_files;

    fake::put(main + t::data::world, old_world), fake::put<uint8_t>(w.framework + t::game_framework::world_begun, old_begun);
    for (uint64_t gone : {bed, row, post, lava, moss_material}) fake::destroy(gone);
    slots_of = other_slots, stock_material = other_stock;
    disk.files.erase(mods_at + "shade.two/mod.json"), disk.files.erase(mods_at + "shade.two/shaders.bin");
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    logged.lines.clear(), disk.log.clear();
}
