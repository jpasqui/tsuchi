// Navigation retries against the made-up game. Included by objects_test.cpp after its file fixture builder.
#include "map_navigation.h"

void navigation_retries() {
    using tsuchi::NavigationCounts;
    using tsuchi::NavigationResult;
    using tsuchi::NavigationState;
    namespace nav = tsuchi::nav;
    auto bytes = navigation_fixture;
    nav::Map maps[1];
    nav::Navmesh meshes[1];
    nav::File fixture{bytes.data(), uint32_t(bytes.size()), maps, 1, 0, meshes, 1, 0};
    CHECK(nav::read(&fixture) == nav::Read::ok);
    const size_t mesh_start = meshes[0].name - 2;
    const size_t second_start = bytes.size();
    const auto second = std::vector<uint8_t>(bytes.begin() + mesh_start, bytes.end());
    bytes.insert(bytes.end(), second.begin(), second.end());
    const uint32_t mesh_count = maps[0].path + maps[0].path_length;
    for (uint32_t i = 0; i < 4; ++i) bytes[mesh_count + i] = i ? 0 : 2;
    std::memcpy(bytes.data() + mesh_start + 2, "Nav-LateAA", 10);
    std::memcpy(bytes.data() + second_start + 2, "Nav-LateBB", 10);

    const size_t before = blocks_out.size();
    bool well_formed = false;
    NavigationState state;
    for (int left = 0; left < 2; ++left) {
        allocations_left = left;
        CHECK(!tsuchi::prepare_navigation(state, bytes.data(), bytes.size(), "/G/Cave_P", well_formed));
        allocations_left = -1;
        CHECK(well_formed && !state.parsed && blocks_out.size() == before);
    }
    CHECK(!tsuchi::prepare_navigation(state, bytes.data(), bytes.size(), "/G/Absent_P", well_formed) && well_formed && state.parsed);
    tsuchi::release_navigation(state);
    CHECK(blocks_out.size() == before);
    CHECK(tsuchi::prepare_navigation(state, bytes.data(), bytes.size(), "/g/cAVE_p", well_formed) && well_formed);
    // File validation is a start-time operation. Changing only its mark after preparation must not make a
    // retry parse it again; the retained descriptors and untouched tile payloads still name the same meshes.
    bytes[0] = 'X';

    const uint64_t main = fake::main_image();
    const uint64_t old_allocate = fake::get<uint64_t>(main + t::data::nav_allocate), old_release = fake::get<uint64_t>(main + t::data::nav_release);
    const auto old_remove = t::fn::nav_remove_tile;
    const auto old_add = t::fn::nav_add_tile;
    const auto old_invalidate = t::fn::nav_invalidate_paths;
    const size_t old_code_count = fake::code().size();
    fake::code().push_back(reinterpret_cast<uint64_t>(&game_nav_allocate));
    fake::code().push_back(reinterpret_cast<uint64_t>(&game_nav_release));
    fake::put(main + t::data::nav_allocate, &game_nav_allocate), fake::put(main + t::data::nav_release, &game_nav_release);
    t::fn::nav_remove_tile = game_nav_remove_tile, t::fn::nav_add_tile = game_nav_add_tile, t::fn::nav_invalidate_paths = game_nav_invalidate_paths;
    const uint64_t type = fake::object(u"RecastNavMesh", w.root);
    const uint64_t world = fake::object(u"RetryWorld", w.root, 0x40), level = fake::object(u"PersistentLevel", w.root, 0x40);
    fake::put(level + t::object::outer, world);
    const uint64_t other_world = fake::object(u"OtherRetryWorld", w.root, 0x40), other_level = fake::object(u"PersistentLevel", w.root, 0x40);
    fake::put(other_level + t::object::outer, other_world);
    const auto actor_in = [&](uint64_t at, const std::u16string& name) {
        const uint64_t actor = fake::object(name, type, 0x80);
        fake::put(actor + t::object::outer, at);
        return actor;
    };
    const auto ready = [&](uint64_t actor) {
        const uint64_t implementation = fake::memory(0x20), mesh = fake::memory(0x40), tiles = fake::memory(64 * t::navmesh_tile::size);
        fake::put(actor + t::navmesh_actor::implementation, implementation), fake::put(implementation + t::navmesh_implementation::mesh, mesh);
        for (uint32_t i = 0; i < 20; ++i) fake::put<uint8_t>(mesh + i, uint8_t(1 + i));
        fake::put<int32_t>(mesh + t::navmesh::max_tiles, 64), fake::put(mesh + t::navmesh::tiles, tiles);
        fake::put<uint32_t>(mesh + t::navmesh::tile_bits, 8), fake::put<uint32_t>(mesh + t::navmesh::poly_bits, 20);
        fake::put<uint32_t>(tiles + t::navmesh_tile::salt, 2), fake::put<uint64_t>(tiles + t::navmesh_tile::header, 0x1234);
    };
    uint64_t first = actor_in(level, u"nav-lATEaa");
    ready(first);
    const uint64_t other_first = actor_in(other_level, u"Nav-LateAA"), other_second = actor_in(other_level, u"Nav-LateBB");
    ready(other_first), ready(other_second);
    NavigationCounts counts{};
    nav_scene = NavScene{};
    CHECK(tsuchi::swap_navigation(state, world, counts) == NavigationResult::waiting);
    CHECK(counts.navmeshes == 1 && counts.removed == 1 && counts.added == 2 && nav_scene.taken == 2);
    const auto first_log = nav_scene.log;
    CHECK(tsuchi::swap_navigation(state, world, counts) == NavigationResult::waiting && nav_scene.log == first_log);
    // A completed actor whose life ended is eligible again; a reused slot cannot stand for the earlier life.
    fake::destroy(first);
    first = actor_in(level, u"nav-lATEaa");
    ready(first);
    CHECK(tsuchi::swap_navigation(state, world, counts) == NavigationResult::waiting && counts.navmeshes == 2 && counts.added == 4);
    const uint64_t late = actor_in(level, u"Nav-LateBB");
    const auto second_log = nav_scene.log;
    CHECK(tsuchi::swap_navigation(state, world, counts) == NavigationResult::waiting && nav_scene.log == second_log);
    ready(late);
    CHECK(tsuchi::swap_navigation(state, world, counts) == NavigationResult::done && counts.navmeshes == 3 && counts.added == 6);
    const auto whole_log = nav_scene.log;
    CHECK(tsuchi::swap_navigation(state, world, counts) == NavigationResult::done && nav_scene.log == whole_log);
    CHECK(tsuchi::swap_navigation(state, 0, counts) == NavigationResult::waiting && nav_scene.log == whole_log);
    CHECK(tsuchi::swap_navigation(state, other_world, counts) == NavigationResult::done && counts.navmeshes == 5 && counts.added == 10);
    tsuchi::reset_navigation(state);
    CHECK(tsuchi::swap_navigation(state, other_world, counts) == NavigationResult::done && counts.navmeshes == 7 && counts.added == 14);
    tsuchi::release_navigation(state);
    tsuchi::release_navigation(state);
    CHECK(!state.parsed && blocks_out.size() == before);
    bytes[0] = 'P';
    bytes[7] = '2';
    CHECK(!tsuchi::prepare_navigation(state, bytes.data(), bytes.size(), "/G/Cave_P", well_formed) && !well_formed && !state.parsed);
    CHECK(blocks_out.size() == before);
    t::fn::nav_remove_tile = old_remove, t::fn::nav_add_tile = old_add, t::fn::nav_invalidate_paths = old_invalidate;
    fake::put(main + t::data::nav_allocate, old_allocate), fake::put(main + t::data::nav_release, old_release);
    fake::code().resize(old_code_count);
    fake::destroy(first), fake::destroy(late), fake::destroy(other_first), fake::destroy(other_second), fake::destroy(type);
    fake::destroy(level), fake::destroy(other_level), fake::destroy(world), fake::destroy(other_world);
    nav_scene = NavScene{};
}
