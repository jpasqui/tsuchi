#include "presenter.h"
#include "registry.h"
#include "time_service.h"
#include <cassert>
using namespace tsuchi;
namespace {
bool ready = true, held = false;
uint32_t wanted = 30, sync = 0, file_limit = 30, file_sync = 0;
uint64_t thread = 1;
unsigned requests = 0;
bool pose_ready = true, pose_held = false;
uint32_t pose_wanted = 0, file_pose = 0;
unsigned pose_requests = 0;
}
namespace tsuchi {
TSUCHI_Result presenter_request(bool take, uint32_t limit, uint32_t wait) {
    ++requests;
    if (take && !ready) return TSUCHI_NOT_READY;
    held = take; wanted = take ? limit : file_limit; sync = take ? wait : file_sync;
    return TSUCHI_OK;
}
void presenter_policy(uint32_t& limit, uint32_t& wait) { limit=wanted; wait=sync; }
TSUCHI_Result presenter_pose_request(bool take, uint32_t mode) {
    ++pose_requests;
    if (take && !pose_ready) return TSUCHI_NOT_READY;
    pose_held = take; pose_wanted = take ? mode : file_pose;
    return TSUCHI_OK;
}
uint32_t presenter_pose_policy() { return pose_wanted; }
}
int main() {
    set_thread_probe([] { return thread; }); note_game_thread();
    presentation_start();
    auto* a=mod_create("video.settings"); auto* b=mod_create("other.video");
    const auto& api=time::table;
    TSUCHI_PresentationPolicy read{};
    assert(api.presentation(a,&read)==TSUCHI_OK && read.frame_limit==30);
    assert(api.hold_presentation(a,45,TSUCHI_PRESENTATION_SYNC_OFF)==TSUCHI_INVALID);
    assert(api.hold_presentation(a,60,TSUCHI_PresentationSync(7))==TSUCHI_INVALID);
    assert(!owned_count(a) && requests==0);
    ready=false;
    assert(api.hold_presentation(a,60,TSUCHI_PRESENTATION_SYNC_OFF)==TSUCHI_NOT_READY && !owned_count(a));
    ready=true;
    assert(api.hold_presentation(a,60,TSUCHI_PRESENTATION_SYNC_OFF)==TSUCHI_OK && held && owned_count(a)==1);
    assert(api.hold_presentation(b,120,TSUCHI_PRESENTATION_SYNC_ON)==TSUCHI_BUSY && wanted==60);
    assert(api.hold_presentation(a,120,TSUCHI_PRESENTATION_SYNC_ON)==TSUCHI_OK && owned_count(a)==1);
    thread=2;
    assert(api.release_presentation(a)==TSUCHI_INVALID && held); thread=1;
    assert(api.presentation(a,&read)==TSUCHI_OK && read.frame_limit==120 && read.vsync==TSUCHI_PRESENTATION_SYNC_ON);
    assert(api.release_presentation(b)==TSUCHI_NOT_FOUND && held);
    file_limit=60;file_sync=2;
    mod_stop(a);
    assert(!held && wanted==60 && sync==2 && !owned_count(a));
    assert(api.hold_presentation(b,0,TSUCHI_PRESENTATION_SYNC_OFF)==TSUCHI_OK);
    assert(api.release_presentation(b)==TSUCHI_OK && !held && wanted==60);
    assert(api.release_presentation(b)==TSUCHI_NOT_FOUND);
    mod_stop(b);

    // Pose presentation owns its own process policy. A frame-rate owner and a
    // different pose owner coexist; neither family's release removes the other.
    auto* c=mod_create("presentation.owner"); auto* d=mod_create("pose.owner");
    TSUCHI_PosePresentation pose=TSUCHI_POSE_PRESENTATION_SMOOTH;
    assert(api.pose_presentation(c,&pose)==TSUCHI_OK && pose==TSUCHI_POSE_PRESENTATION_SELECTIVE);
    assert(api.pose_presentation(c,nullptr)==TSUCHI_INVALID);
    assert(api.hold_pose_presentation(c,TSUCHI_PosePresentation(2))==TSUCHI_INVALID);
    assert(api.hold_pose_presentation(c,TSUCHI_PosePresentation(-1))==TSUCHI_INVALID);
    assert(!owned_count(c) && pose_requests==0);
    pose_ready=false;
    assert(api.hold_pose_presentation(c,TSUCHI_POSE_PRESENTATION_SMOOTH)==TSUCHI_NOT_READY);
    assert(!owned_count(c) && !pose_held && pose_wanted==0);
    pose_ready=true;
    assert(api.hold_presentation(c,120,TSUCHI_PRESENTATION_SYNC_ON)==TSUCHI_OK);
    assert(api.hold_pose_presentation(d,TSUCHI_POSE_PRESENTATION_SMOOTH)==TSUCHI_OK);
    assert(held && wanted==120 && pose_held && pose_wanted==1);
    assert(owned_count(c)==1 && owned_count(d)==1);
    const unsigned accepted_requests=pose_requests;
    assert(api.hold_pose_presentation(c,TSUCHI_POSE_PRESENTATION_SELECTIVE)==TSUCHI_BUSY);
    assert(pose_requests==accepted_requests && pose_wanted==1);
    assert(api.release_pose_presentation(c)==TSUCHI_NOT_FOUND && pose_held);

    // A failed update by the existing owner retains both its previous request
    // and its ownership. Invalid values never reach the presenter either.
    pose_ready=false;
    assert(api.hold_pose_presentation(d,TSUCHI_POSE_PRESENTATION_SELECTIVE)==TSUCHI_NOT_READY);
    assert(pose_held && pose_wanted==1 && owned_count(d)==1);
    const unsigned failed_requests=pose_requests;
    assert(api.hold_pose_presentation(d,TSUCHI_PosePresentation(7))==TSUCHI_INVALID);
    assert(pose_requests==failed_requests && pose_wanted==1 && owned_count(d)==1);
    assert(api.hold_pose_presentation(c,TSUCHI_POSE_PRESENTATION_SELECTIVE)==TSUCHI_BUSY);
    pose_ready=true;
    thread=2;
    assert(api.hold_pose_presentation(d,TSUCHI_POSE_PRESENTATION_SELECTIVE)==TSUCHI_INVALID);
    assert(api.release_pose_presentation(d)==TSUCHI_INVALID && pose_held);
    assert(api.pose_presentation(d,&pose)==TSUCHI_INVALID);
    thread=1;
    assert(api.pose_presentation(d,&pose)==TSUCHI_OK && pose==TSUCHI_POSE_PRESENTATION_SMOOTH);
    file_pose=1;
    assert(api.release_pose_presentation(d)==TSUCHI_OK && !pose_held && pose_wanted==1);
    assert(held && wanted==120 && owned_count(c)==1 && !owned_count(d));
    assert(api.release_pose_presentation(d)==TSUCHI_NOT_FOUND);

    // The same mod may own both families, and its stop restores the latest file
    // values for each. Release/stop do not require the native hooks to be ready.
    assert(api.hold_pose_presentation(c,TSUCHI_POSE_PRESENTATION_SELECTIVE)==TSUCHI_OK);
    assert(owned_count(c)==2 && pose_wanted==0);
    file_limit=30; file_sync=0; file_pose=1; pose_ready=false;
    mod_stop(c);
    assert(!held && wanted==30 && sync==0 && !pose_held && pose_wanted==1 && !owned_count(c));
    pose_ready=true;
    assert(api.hold_pose_presentation(d,TSUCHI_POSE_PRESENTATION_SMOOTH)==TSUCHI_OK);
    assert(api.hold_pose_presentation(d,TSUCHI_POSE_PRESENTATION_SELECTIVE)==TSUCHI_OK);
    assert(owned_count(d)==1 && pose_wanted==0);
    file_pose=0;
    mod_stop(d);
    assert(!pose_held && pose_wanted==0 && !owned_count(d));
}
