#include "presenter.h"
#include "registry.h"
#include "time_service.h"
namespace tsuchi {
namespace {
const Kind policy_kind{"time.presentation"};
const Kind pose_kind{"time.pose_presentation"};
void stopped(void*, uint64_t) { presenter_request(false, 30, 0); }
TSUCHI_Result policy(TSUCHI_PresentationPolicy* out) {
    uint32_t limit = 30, sync = 0;
    presenter_policy(limit, sync);
    *out = {limit, TSUCHI_PresentationSync(sync)}; return TSUCHI_OK;
}
TSUCHI_Result hold(TSUCHI_Mod* mod, uint32_t limit, TSUCHI_PresentationSync sync) {
    if ((limit != 0 && limit != 30 && limit != 60 && limit != 120) || uint32_t(sync) > 2) return TSUCHI_INVALID;
    TSUCHI_Mod* previous = owner(policy_kind, 0);
    if (previous && previous != mod) return TSUCHI_BUSY;
    const auto recorded = own(mod, policy_kind, 0, stopped, nullptr, true);
    if (recorded != TSUCHI_OK) return recorded;
    const auto result = presenter_request(true, limit, uint32_t(sync));
    if (result != TSUCHI_OK && !previous) disown(mod, policy_kind, 0);
    return result;
}
TSUCHI_Result release_policy(TSUCHI_Mod* mod) {
    const auto result = disown(mod, policy_kind, 0);
    return result == TSUCHI_OK ? presenter_request(false, 30, 0) : result;
}
void pose_stopped(void*, uint64_t) { presenter_pose_request(false, 0); }
TSUCHI_Result pose_policy(TSUCHI_PosePresentation* out) {
    *out = TSUCHI_PosePresentation(presenter_pose_policy()); return TSUCHI_OK;
}
TSUCHI_Result hold_pose(TSUCHI_Mod* mod, TSUCHI_PosePresentation mode) {
    if (uint32_t(mode) > 1) return TSUCHI_INVALID;
    TSUCHI_Mod* previous = owner(pose_kind, 0);
    if (previous && previous != mod) return TSUCHI_BUSY;
    const auto recorded = own(mod, pose_kind, 0, pose_stopped, nullptr, true);
    if (recorded != TSUCHI_OK) return recorded;
    const auto result = presenter_pose_request(true, uint32_t(mode));
    if (result != TSUCHI_OK && !previous) disown(mod, pose_kind, 0);
    return result;
}
TSUCHI_Result release_pose(TSUCHI_Mod* mod) {
    const auto result = disown(mod, pose_kind, 0);
    return result == TSUCHI_OK ? presenter_pose_request(false, 0) : result;
}
const time::Backend backend{policy, hold, release_policy, pose_policy, hold_pose, release_pose};
}
void presentation_start() { time::set_backend(&backend); }
}
