#pragma once
#include "controller.h"

// Game-thread only. The backend owns the texture root until release, and clears
// registered stock UImage brushes before relinquishing that root.
namespace tsuchi { namespace browser { namespace native {
tsuchi::browser::ImageBackend image_backend();
bool bind_image(void* image,tsuchi::browser::TextureToken texture);
void unbind_image(void* image);
void release_images();
struct Diagnostics {
    unsigned abi=3,size=sizeof(Diagnostics);
    unsigned uploads=0,failed=0,releases=0,binds=0,stale=0;
    unsigned texture_count=0,binding_count=0,resident_bytes=0;
    unsigned last_width=0,last_height=0,last_format=0;
    unsigned ui_stage=0,host_builds=0,host_failures=0,host_cards=0;
    unsigned title_attempts=0,title_inserted=0,ui_screen=0,reserved=0;
    unsigned long long title=0,browse_host=0,detail_host=0,scroll=0;
    struct CardImage {unsigned id=0,generation=0,state=0,width=0,height=0,reserved=0;
        unsigned long long texture=0,image=0,brush=0;} cards[12]{};
    unsigned long long profile_host=0;
    unsigned author_id=0,author_state=0,browse_count=0,browse_error=0,browse_generation=0,search_calls=0,search_result=0,icon_materials=0;
    // Microseconds. upload: decoding one image into a texture. host: building
    // the pages' widgets. ui: this module's whole tick. frame: tick to tick
    // while Mods is open; slow_frames counts those over 50 ms. The frame
    // and ui maxima and both counts restart each time Mods is opened.
    unsigned upload_us_last=0,upload_us_max=0,host_us_total=0,host_us_step_max=0;
    unsigned ui_us_last=0,ui_us_max=0,frame_us_last=0,frame_us_max=0,slow_frames=0,frames=0;
    unsigned images_unsettled=0,images_failed=0,image_bytes=0,detail_state=0;
    // decode: decoding one image, which is most of upload_*; the rest is
    // making the texture. Both run on the game thread (see upload()).
    unsigned decode_us_last=0,decode_us_max=0,decodes=0,reserved2=0;
    // entropy: https_platform::entropy_diagnostic(). guard_*: the installed
    // mod guard (p4u_mod_files_diagnostic 0..3).
    unsigned entropy=0,guard_state=0,guard_seen=0,guard_admitted=0,guard_refused=0,reserved3=0;
    unsigned entropy_detail[8]{};   // https_platform::entropy_detail_diagnostic
};
const Diagnostics& diagnostics();

void start(TSUCHI_Mod*);
} } }
