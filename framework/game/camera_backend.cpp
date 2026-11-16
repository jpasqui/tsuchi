// The player's follow camera goals and the view after the presenter has blended it.
#include "actor_events.h"
#include "actors.h"
#include "angles.h"
#include "camera_service.h"
#include "captain.h"
#include "game.h"
#include "holds.h"
#include "objects.h"
#include "ordinary_lifecycle.h"
#include "parts.h"
#include "people.h"
#include "presenter.h"

namespace tsuchi {
namespace {
namespace t = game::table;
game::ClassRef follow_class{t::name::camera_follow_class};
bool player_ok(uint32_t p) {
    return p < game::kPlayerSlots;
}
float normalized(float yaw) {
    while (yaw > 180)
        yaw -= 360;
    while (yaw <= -180)
        yaw += 360;
    return yaw;
}
uint64_t controller(uint32_t p) {
    const uint64_t world = game::game_framework();
    return player_ok(p) && world && t::fn::camera_controller
               ? reinterpret_cast<uint64_t>(t::fn::camera_controller(reinterpret_cast<void*>(world), int32_t(p)))
               : 0;
}
uint64_t manager(uint32_t p) {
    const uint64_t pc = controller(p);
    return pc ? game::read_or<uint64_t>(pc + t::player_controller::camera_manager) : 0;
}
uint64_t follow(uint32_t p) {
    const uint64_t actor = game::player_captain(p);
    const uint64_t part = actor ? game::read_or<uint64_t>(actor + t::game_actor::player) : 0;
    const uint64_t camera = part ? game::read_or<uint64_t>(part + t::camera_service::player_camera) : 0;
    return camera && game::live(camera) && game::is_a(camera, follow_class) ? camera : 0;
}
bool paused(uint64_t c) {
    const uint64_t gf = game::game_framework();
    const uint64_t demo = gf ? game::read_or<uint64_t>(gf + t::camera_service::demo_manager) : 0;
    return game::read_or<uint8_t>(c + t::camera_service::mode) != 0 ||
           (demo && game::read_or<uint32_t>(demo + t::camera_service::demo_hold));
}
struct Zoom {
    uint64_t camera;
    TSUCHI_Zoom before, wanted;
};
struct Heading {
    uint64_t camera;
    float before, wanted;
};
struct LookSaved {
    TSUCHI_Vec3 pole, look, offset;
    uint8_t follow;
};
struct Look {
    uint64_t camera;
    TSUCHI_Look wanted;
    LookSaved* saved;
};
void restore_zoom(const Hold&, Zoom& s, HoldEnd) {
    const uint64_t c = game::from_handle(s.camera);
    if (c) {
        game::write(c + t::camera_service::distance, s.before.distance);
        game::write(c + t::camera_service::pitch, s.before.pitch);
    }
}
TSUCHI_Result apply_zoom(const Hold& h, Zoom& s, const TSUCHI_Zoom& wanted, bool fresh) {
    const uint64_t c = follow(uint32_t(h.what));
    if (!c) {
        restore_zoom(h, s, HoldEnd::lost);
        s.camera = 0;
        return TSUCHI_NOT_READY;
    }
    if (fresh || game::from_handle(s.camera) != c) {
        if (!fresh)
            restore_zoom(h, s, HoldEnd::lost);
        s.camera = game::to_handle(c);
        if (!s.camera || !game::read(c + t::camera_service::distance, s.before.distance) ||
            !game::read(c + t::camera_service::pitch, s.before.pitch))
            return TSUCHI_NOT_READY;
    }
    if (!game::writable(c + t::camera_service::distance, 4) || !game::writable(c + t::camera_service::pitch, 4))
        return TSUCHI_NOT_READY;
    s.wanted = wanted;
    hold_apply_checkpoint(h);
    game::write(c + t::camera_service::distance, wanted.distance);
    game::write(c + t::camera_service::pitch, wanted.pitch);
    return TSUCHI_OK;
}
bool keep_zoom(const Hold& h, Zoom& s) {
    return apply_zoom(h, s, s.wanted, false) == TSUCHI_OK || !follow(uint32_t(h.what));
}
void restore_heading(const Hold&, Heading& s, HoldEnd) {
    const uint64_t c = game::from_handle(s.camera);
    if (c)
        game::write(c + t::camera_service::heading, s.before);
}
TSUCHI_Result apply_heading(const Hold& h, Heading& s, const float& wanted, bool fresh) {
    const uint64_t c = follow(uint32_t(h.what));
    if (!c) {
        restore_heading(h, s, HoldEnd::lost);
        s.camera = 0;
        return TSUCHI_NOT_READY;
    }
    if (!game::writable(c + t::camera_service::heading, 4))
        return TSUCHI_NOT_READY;
    if (fresh || game::from_handle(s.camera) != c) {
        if (!fresh)
            restore_heading(h, s, HoldEnd::lost);
        s.camera = game::to_handle(c);
        if (!s.camera || !game::read(c + t::camera_service::heading, s.before))
            return TSUCHI_NOT_READY;
    }
    s.wanted = normalized(wanted);
    hold_apply_checkpoint(h);
    game::write(c + t::camera_service::heading,
                normalized(s.wanted + 180) < 0 ? normalized(s.wanted + 180) + 360 : normalized(s.wanted + 180));
    return TSUCHI_OK;
}
bool keep_heading(const Hold& h, Heading& s) {
    return apply_heading(h, s, s.wanted, false) == TSUCHI_OK || !follow(uint32_t(h.what));
}
void restore_look(const Hold&, Look& s, HoldEnd) {
    const uint64_t c = game::from_handle(s.camera);
    if (c && s.saved) {
        game::write(c + t::camera_service::follow, s.saved->follow);
        game::write(c + t::camera_service::look, s.saved->look);
        game::write(c + t::camera_service::pole, s.saved->pole);
    }
    tsuchi::release(s.saved);
    s.saved = nullptr;
}

bool target(const TSUCHI_Look& wanted, TSUCHI_Vec3& point) {
    point = wanted.point;
    if (!wanted.actor.id)
        return true;
    const uint64_t actor = game::from_handle(wanted.actor.id);
    game::Transform pose;
    if (!actor)
        return false;
    if (game::is_game_actor(actor)) {
        if (!game::get_transform(actor, pose))
            return false;
    } else {
        const uint64_t root = game::read_or<uint64_t>(actor + t::engine_actor::root);
        if (!root || !game::read(root + t::scene_component::location, pose.location))
            return false;
    }
    point.x += pose.location[0];
    point.y += pose.location[1];
    point.z += pose.location[2];
    return true;
}
TSUCHI_Result apply_look(const Hold& h, Look& s, const TSUCHI_Look& wanted, bool fresh) {
    TSUCHI_Vec3 point;
    if (!target(wanted, point))
        return TSUCHI_NOT_FOUND;
    const uint64_t c = follow(uint32_t(h.what));
    if (!c) {
        restore_look(h, s, HoldEnd::lost);
        s.camera = 0;
        return TSUCHI_NOT_READY;
    }
    if (fresh || game::from_handle(s.camera) != c) {
        if (!fresh)
            restore_look(h, s, HoldEnd::lost);
        s.camera = game::to_handle(c);
    }
    s.wanted = wanted;
    if (paused(c)) {
        restore_look(h, s, HoldEnd::lost);
        return TSUCHI_OK;
    }
    // The camera stays at its distance from the captain, or from the dog a captain rides: a rider's own
    // transform stays where it got on.
    const uint64_t rider = game::player_captain(uint32_t(h.what));
    const uint64_t mount = rider ? game::mount_of(rider) : 0;
    game::Transform captain;
    if (!game::get_transform(mount ? mount : rider, captain))
        return TSUCHI_NOT_READY;
    if (!game::writable(c + t::camera_service::follow, 1) || !game::writable(c + t::camera_service::look, sizeof point) ||
        !game::writable(c + t::camera_service::pole, sizeof point))
        return TSUCHI_NOT_READY;
    if (!s.saved) {
        LookSaved before{};
        if (!game::read(c + t::camera_service::follow, before.follow) || !game::read(c + t::camera_service::pole, before.pole) ||
            !game::read(c + t::camera_service::look, before.look))
            return TSUCHI_NOT_READY;
        before.offset = {before.pole.x - captain.location[0], before.pole.y - captain.location[1], before.pole.z - captain.location[2]};
        s.saved = static_cast<LookSaved*>(allocate(sizeof before));
        if (!s.saved)
            return TSUCHI_NO_MEMORY;
        *s.saved = before;
    }
    hold_apply_checkpoint(h);
    const TSUCHI_Vec3 pole{captain.location[0] + s.saved->offset.x, captain.location[1] + s.saved->offset.y,
                           captain.location[2] + s.saved->offset.z};
    game::write(c + t::camera_service::follow, uint8_t(0));
    game::write(c + t::camera_service::look, point);
    game::write(c + t::camera_service::pole, pole);
    return TSUCHI_OK;
}
bool keep_look(const Hold& h, Look& s) {
    const TSUCHI_Result r = apply_look(h, s, s.wanted, false);
    return r != TSUCHI_NOT_FOUND;
}
Holds<Zoom, TSUCHI_Zoom, apply_zoom, keep_zoom, restore_zoom> zooms{"camera.zoom", HoldOn::nothing, true};
Holds<Heading, float, apply_heading, keep_heading, restore_heading> headings{"camera.heading", HoldOn::nothing, true};
Holds<Look, TSUCHI_Look, apply_look, keep_look, restore_look> looks{"camera.look", HoldOn::nothing, true};
// A camera hold belongs to a player, but its optional followed actor still ends it on leaving play.
void target_left(uint64_t handle) {
    for (uint32_t i = looks.count(); i--;) {
        const Hold h = looks.hold_at(i);
        if (looks.kept_at(i).wanted.actor.id == handle)
            looks.release(h.mod, h.object, h.what);
    }
}
void actor_left(uint64_t, uint64_t handle, ActorLeft) {
    target_left(handle);
}
void ordinary_left(uint64_t, uint64_t handle, bool) {
    target_left(handle);
}
bool foreign(TSUCHI_Mod* mod, uint32_t p) {
    const TSUCHI_Mod* owners[] = {zooms.holder(0, p), headings.holder(0, p), looks.holder(0, p)};
    for (const TSUCHI_Mod* owner : owners)
        if (owner && owner != mod)
            return true;
    return false;
}
struct Shown {
    uint64_t player = 0;
    TSUCHI_View view{};
    uint8_t prefix[t::view_info::most]{};
} shown[game::kPlayerSlots];
// ProjectWorldToScreen calls LocalPlayer.GetProjectionData, which calls this same view slot.
// Use the shown prefix once, including projection/aspect fields, and restore its native scratch on return.
struct Projection {
    uint32_t player;
    uint64_t controller, output = 0;
    uint8_t before[t::view_info::most]{};
    bool applied = false;
    bool use_shown = true;
};
Projection* projection = nullptr;

void after_view(uint64_t local, uint64_t output) {
    const uint64_t pc = game::read_or<uint64_t>(local + t::local_player::controller);
    for (uint32_t p = 0; p < game::kPlayerSlots; ++p) {
        if (!pc || pc != controller(p))
            continue;
        if (presenter_limit() == 30)
            shown[p] = {};
        if (projection) {
            if (projection->use_shown && projection->player == p && !projection->applied && shown[p].player == projection->controller &&
                game::read_bytes(output, projection->before, t::view_info::size)) {
                projection->output = output;
                projection->applied = game::write_bytes(output, shown[p].prefix, t::view_info::size);
            }
            return;
        }
        if (presenter_limit() == 30 || !presenter_drawing_view())
            return;
        TSUCHI_Rotation rotation;
        if (!game::read(output + t::view_info::rotation, rotation))
            return;
        // Holds update native camera goals on world steps. Preserve the full
        // blended native POV here: re-aiming at the latest simulation pose
        // would separate its orientation from this picture's orbit and target.
        Shown& s = shown[p];
        s.player = game::to_handle(pc);
        game::read(output + t::view_info::location, s.view.location);
        s.view.rotation = rotation;
        game::read(output + t::view_info::fov, s.view.fov);
        game::read_bytes(output, s.prefix, t::view_info::size);
    }
}
TSUCHI_Result view(uint32_t p, TSUCHI_View* out) {
    if (!player_ok(p))
        return TSUCHI_INVALID;
    if (presenter_limit() == 30)
        shown[p] = {};
    if (shown[p].player && game::from_handle(shown[p].player) == controller(p)) {
        *out = shown[p].view;
        return TSUCHI_OK;
    }
    const uint64_t c = manager(p);
    if (!c || !game::read(c + t::camera_service::pov, *out))
        return TSUCHI_NOT_READY;
    return TSUCHI_OK;
}
TSUCHI_Result zoom(uint32_t p, TSUCHI_Zoom* out) {
    if (!player_ok(p))
        return TSUCHI_INVALID;
    const uint64_t c = follow(p);
    TSUCHI_Zoom value;
    if (!c || !game::read(c + t::camera_service::distance, value.distance) || !game::read(c + t::camera_service::pitch, value.pitch))
        return TSUCHI_NOT_READY;
    *out = value;
    return TSUCHI_OK;
}
TSUCHI_Result heading(uint32_t p, float* out) {
    if (!player_ok(p))
        return TSUCHI_INVALID;
    const uint64_t c = follow(p);
    float goal;
    if (!c || !game::read(c + t::camera_service::heading, goal))
        return TSUCHI_NOT_READY;
    *out = normalized(goal - 180);
    return TSUCHI_OK;
}
TSUCHI_Result hold_zoom(TSUCHI_Mod* m, uint32_t p, const TSUCHI_Zoom* z) {
    if (!player_ok(p) || z->distance < 10 || z->distance > 100000 || z->pitch < -90 || z->pitch > 90)
        return TSUCHI_INVALID;
    return foreign(m, p) ? TSUCHI_BUSY : zooms.take(m, 0, p, *z);
}
TSUCHI_Result hold_heading(TSUCHI_Mod* m, uint32_t p, float y) {
    if (!player_ok(p) || y < -3600 || y > 3600)
        return TSUCHI_INVALID;
    return foreign(m, p) ? TSUCHI_BUSY : headings.take(m, 0, p, y);
}
TSUCHI_Result hold_look(TSUCHI_Mod* m, uint32_t p, const TSUCHI_Look* l) {
    if (!player_ok(p))
        return TSUCHI_INVALID;
    if (l->actor.id) {
        const uint64_t actor = game::from_handle(l->actor.id);
        if (!actor)
            return TSUCHI_NOT_FOUND;
        if (!game::is_engine_actor(actor))
            return TSUCHI_INVALID;
        if (!game::is_game_actor(actor)) {
            const TSUCHI_Result watched = ordinary_lifecycle_watch(actor);
            if (watched != TSUCHI_OK)
                return watched;
        }
    }
    return foreign(m, p) ? TSUCHI_BUSY : looks.take(m, 0, p, *l);
}
TSUCHI_Result release_zoom(TSUCHI_Mod* m, uint32_t p) {
    return player_ok(p) ? zooms.release(m, 0, p) : TSUCHI_INVALID;
}
TSUCHI_Result release_heading(TSUCHI_Mod* m, uint32_t p) {
    return player_ok(p) ? headings.release(m, 0, p) : TSUCHI_INVALID;
}
TSUCHI_Result release_look(TSUCHI_Mod* m, uint32_t p) {
    return player_ok(p) ? looks.release(m, 0, p) : TSUCHI_INVALID;
}
TSUCHI_Result look(uint32_t p, TSUCHI_Look* out) {
    if (!player_ok(p))
        return TSUCHI_INVALID;
    const Look* l = looks.find(0, p);
    *out = l ? l->wanted : TSUCHI_Look{};
    return TSUCHI_OK;
}
TSUCHI_Result shake(uint32_t p, TSUCHI_ShakeKind k) {
    if (!player_ok(p))
        return TSUCHI_INVALID;
    const uint64_t gf = game::game_framework(), mgr = gf ? game::read_or<uint64_t>(gf + t::camera_service::shake_manager) : 0;
    if (!mgr || !t::bound(t::fn::camera_shake) || !game::player_captain(p))
        return TSUCHI_NOT_READY;
    t::fn::camera_shake(reinterpret_cast<void*>(mgr), k, reinterpret_cast<void*>(game::player_captain(p)));
    return TSUCHI_OK;
}
TSUCHI_Result stop_shakes(uint32_t p, bool now) {
    if (!player_ok(p))
        return TSUCHI_INVALID;
    const uint64_t mgr = manager(p);
    const auto stop = mgr ? game::method(mgr, t::camera_service::stop_shakes) : nullptr;
    if (!stop)
        return TSUCHI_NOT_READY;
    stop(reinterpret_cast<void*>(mgr), now);
    return TSUCHI_OK;
}
TSUCHI_Result screen_size(uint32_t p, TSUCHI_ScreenSize* out) {
    if (!player_ok(p))
        return TSUCHI_INVALID;
    const uint64_t pc = controller(p);
    if (!pc || !t::bound(t::fn::camera_screen))
        return TSUCHI_NOT_READY;
    TSUCHI_ScreenSize size{};
    t::fn::camera_screen(reinterpret_cast<void*>(pc), &size.width, &size.height);
    if (size.width <= 0 || size.height <= 0)
        return TSUCHI_NOT_READY;
    *out = size;
    return TSUCHI_OK;
}
TSUCHI_Result world_to_screen(uint32_t p, const TSUCHI_Vec3* point, TSUCHI_ScreenPoint* out) {
    if (!player_ok(p))
        return TSUCHI_INVALID;
    if (projection)
        return TSUCHI_BUSY;
    const uint64_t pc = controller(p), handle = pc ? game::to_handle(pc) : 0;
    const bool blended = presenter_limit() != 30;
    if (!blended)
        shown[p] = {};
    if (!handle || (blended && shown[p].player != handle) || !t::bound(t::fn::camera_project))
        return TSUCHI_NOT_READY;
    Projection call{p, handle};
    call.use_shown = blended;
    projection = &call;
    TSUCHI_ScreenPoint screen{};
    const bool visible = t::fn::camera_project(reinterpret_cast<void*>(pc), point, &screen, false);
    projection = nullptr;
    if (call.applied && game::from_handle(handle) == pc)
        game::write_bytes(call.output, call.before, t::view_info::size);
    if (blended && !call.applied)
        return TSUCHI_UNSUPPORTED;
    if (!visible)
        return TSUCHI_NOT_FOUND;
    *out = screen;
    return TSUCHI_OK;
}

const camera::Backend service{view,      zoom,         heading, hold_zoom, release_zoom, hold_heading,    release_heading,
                              hold_look, release_look, look,    shake,     stop_shakes,  world_to_screen, screen_size};
} // namespace
void camera_start() {
    camera::set_backend(&service);
    zooms.start();
    headings.start();
    looks.start();
    on_actor_left(actor_left);
    ordinary_lifecycle_listener(ordinary_left);
    presenter_camera_after(after_view);
}
} // namespace tsuchi
