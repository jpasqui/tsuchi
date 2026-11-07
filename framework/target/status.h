// The status block: what the framework publishes about itself, for a tool on a PC to read.
// A build made for checking the framework in the game fills in the rest of it; the build
// people run reports its state and retained acceptance counters without running those checks.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "game.h"
#include "host.h"
#include "hooks.h"
#include "native_hooks.h"
#include "registry.h"

namespace tsuchi::target {

enum State : uint32_t {
    kStarting = 0,
    kRunning = 1,
    kNoGame = 2,           // the game in memory is no build the table describes
    kStatusWordsTaken = 3, // something else uses the words the status is published in
    kTickUnexpected = 4,   // the engine's per-frame entry could not be hooked
};

// What came of telling the player the game is a build the table does not describe.
enum Notice : uint32_t {
    kNoticeNone = 0,
    kNoticeArmed = 1,   // it will show when the game next reads a controller, after its start-up
    kNoticeShown = 2,
    kNoticeNoDialog = 3, // no loaded program offers the error dialog
    kNoticeNoCall = 4,   // the game imports no call this can show it from
};

struct Status {
    uint64_t magic; // kMagic
    uint32_t version, size;
    uint64_t game_base, module_base;
    uint32_t state;
    uint32_t frames;         // frames since start-up
    float last_seconds;      // length of the latest frame
    uint32_t frames_heard;   // frames delivered to a subscriber through the time service
    uint32_t tick_was_stock; // 1 when the per-frame entry held the game's own function

    // A check of the base layer against the running game, filled in after
    // kCheckFrames frames and again later.
    uint32_t checks;        // how many times it has run
    int32_t object_count;   // the game's own count of objects
    uint32_t live_objects;  // objects visited
    uint32_t classes;       // of those, classes
    uint32_t actors;        // of those, actors
    uint32_t handles_tried; // handles made for a sample of objects
    uint32_t handles_ok;    // of those, how many led back to their object
    uint32_t reserved;
    uint64_t root_handle;
    char root_name[32];    // the name of the class every class belongs to
    char actor_name[32];   // the name of the first actor found
    char game_version[16]; // from the game table

    // A check of hooks: a second hook is put on the per-frame entry through the
    // public service for kHookTestFrames frames, then its mod is stopped.
    uint32_t hook_test;       // 0 not yet, 1 hooked, 2 its mod stopped, 3 could not hook
    uint32_t hook_test_calls; // times the second hook ran
    uint32_t tick_hooks;      // hooks on the per-frame entry right now
    int32_t hook_test_result; // what the service returned

    // Code sites. The patch's block reads `site_jump` to find the framework: 0 unless
    // the patch in the running game is the one this build was made with. The check is
    // the one above again, on a function the game calls directly.
    uint64_t site_jump;
    uint32_t sites_patched;   // code sites that can be hooked: 0 when the patch is missing or another build's
    uint32_t site_test;       // 0 not yet, 1 hooked, 2 its mod stopped, 3 could not hook
    uint32_t site_test_calls; // times that hook ran
    int32_t site_test_result; // what the service returned

    // A check of reflection through the public service, on two actors in the
    // loaded level: fields read and written, and the game's functions called.
    uint32_t reflect_test;   // 0 not yet, 1 began, 2 finished, 3 found no actors to use
    uint32_t reflect_step;   // the last step begun
    uint32_t reflect_passed; // one bit per step that did what it should
    int32_t reflect_result;  // what the first call that should have worked and did not returned
    float reflect_field_location[3]; // the first actor's place, read from its fields
    float reflect_call_location[3];  // and as the game's function reports it
    float reflect_distance;          // from the first actor to the second, as the game's function reports it
    float reflect_distance_squared;  // the same, worked out from the two places
    float reflect_tick_interval;     // a field read through a struct and its parent struct
    float reflect_abs;               // what a function of a class made of -2.5
    int32_t reflect_sum;             // what another made of 40 and 2
    int32_t reflect_role;            // a field that holds one of a list of named values
    char reflect_actor[32];
    char reflect_other[32];
    uint32_t reflect_left_open; // things the check still owned when it finished
    uint32_t reserved2;
    float reflect_other_field_location[3]; // the second actor's place, read from its fields
    float reflect_other_call_location[3];  // and as the game's function reports it
    char reflect_text[32];       // the first actor's name, as a function of the game returns it in a string
    char reflect_name_field[32]; // a field of the first actor that holds a name
    char reflect_joined[32];     // what a function of the game made of a list of strings
    int32_t reflect_length;      // how long the game found a string to be
    float reflect_average;       // the game's average of a list of vectors

    // A check of the objects service on the actors in play.
    uint32_t objects_test;   // 0 not yet, 1 began, 2 finished
    uint32_t objects_passed; // one bit per step that did what it should
    uint32_t objects_count;  // actors in play
    uint32_t objects_placed; // of those, how many a level placed
    int32_t objects_result;  // what the first call that should have worked and did not returned
    int32_t objects_team;    // the first actor's team
    float objects_location[3]; // where the first actor is
    float objects_rotation[3]; // and which way it faces
    char objects_kind[32];     // what kind of thing it is
    // The check runs on the title screen, and again once a level with many actors has settled.
    uint32_t objects_title_passed; // the first run's steps
    uint32_t objects_title_count;  // and how many actors it saw
    uint32_t actors_now;           // actors in play, counted every half second
    uint32_t objects_level_frame;  // the frame the second run happened on, or 0
    uint32_t objects_compared;     // actors whose place was also asked of the game's own function
    uint32_t objects_differed;     // of those, how many answers differed

    // Actors entering and leaving play, from the wrapped game functions.
    uint32_t events_tables;     // actor function tables wrapped
    uint32_t events_pool_sites; // of the two pool functions, how many are hooked
    uint32_t events_entered, events_destroyed, events_put_away, events_in_play; // running counts
    uint32_t events_heard_spawned, events_heard_destroyed;                      // events delivered through the service
    uint32_t events_unseen;     // at the last objects check: listed actors never seen entering
    uint32_t events_unlisted;   // and actors seen entering that the list does not have
    char events_last_left[32];  // the kind of the last actor heard leaving
    char notes[1024];           // what the last objects check saw: each kind with how many, then actors seen but not listed

    // A check of spawning and removing, in a level: a copy of a pot and of a creature are made beside the
    // ones the level has, watched for a second, removed, and watched go.
    uint32_t spawn_test;           // 0 not yet, 1 spawned, 2 removal asked, 3 finished
    uint32_t spawn_passed;         // residual bits 3/7/9 allocations and counts, 10..12 pooled lifetime; retired bits stay zero
    int32_t spawn_result;          // what the first call that should have worked and did not returned
    uint32_t spawn_left_pool;      // of the two, how many left by going back to a pool
    uint32_t spawn_left_destroyed; // and how many by being destroyed
    uint32_t spawn_frames;         // frames from asking for removal to all being gone
    // The same with one of the level's own Pikmin, which the game keeps in a pool.
    uint32_t pool_made;            // 1 when the Pikmin says it belongs to a pool
    uint32_t pool_object_lives;    // 1 when, after removal, the object was still there though its handle was over

    // A check of suspending, on the same creature: its decisions and its movement are stopped for two
    // seconds and given back. The three switches are read through the engine's own functions: bit 0 the
    // actor's update, bit 1 the part that decides for it, bit 2 the part that moves it.
    uint32_t suspend_test;     // 0 not yet, 1 suspended, 2 resumed, 3 finished
    uint32_t suspend_passed;   // one bit per step that did what it should
    int32_t suspend_result;    // what the first call that should have worked and did not returned
    uint32_t suspend_before, suspend_during, suspend_after; // the switches at each stage
    uint32_t suspend_messages; // messages taken from it while its decisions were stopped
    float suspend_drift;       // how far it moved while suspended, in centimeters
    float suspend_control;     // how far it moved in the same length of time just before
    float suspend_resumed;     // and just after

    char suspend_notes[1024];  // drift and body state while suspended and after release

    // For whoever drives the game from outside: whether it is paused, and its own clock.
    uint32_t game_paused;      // 1 while the engine says the game is paused
    float world_seconds;       // the level's own time, which stands still while paused

    // Residual native suspension allocation and largest-collider checks. The SDK
    // covers body behavior; its former measurements remain unused in this layout.
    uint32_t body_test;        // 0 not yet, 1 suspended, 2 given back, 3 residual checks finished
    uint32_t body_passed;      // residual bits 7 allocation count and 8 largest collider; retired bits stay zero
    int32_t body_result;       // what the first call that should have worked and did not returned
    uint32_t body_simulating;  // what the engine says of its body being moved by physics: bit 0 before, 1 during, 2 after
    uint32_t body_collision;   // how its body collides, as the engine numbers it: before, during << 8, after << 16
    uint32_t body_parts;       // its parts that can collide
    float body_hover;          // how far above where it stood it still was after hanging for a second and a half
    float body_fall;           // how far it came down, at the lowest, in the three seconds after being given back

    // The framework's memory, and a check on whichever actor in the level has the most parts that
    // can collide: its solidity is suspended and given back at once.
    uint32_t memory_fixed;     // bytes used of the fixed block, which serves until the engine has an allocator
    uint32_t memory_engine;    // blocks the framework holds from the engine's allocator now
    uint32_t memory_returned;  // blocks it has given back
    uint32_t most_parts;       // how many parts that actor has
    uint32_t most_blocking;    // how many of them block: before, during << 10, after << 20
    int32_t most_result;       // what suspending and giving back returned, the first that was not OK
    char most_kind[32];        // what kind of actor it is

    // A check of the components service on the same creature: its parts are listed, and its mesh is
    // held hidden for eight seconds with what is attached below it, then shown again.
    uint32_t parts_test;       // 0 not yet, 1 hidden, 2 shown again with another material, 3 its own material back, 4 finished
    uint32_t parts_passed;     // one bit per step that did what it should
    int32_t parts_result;      // what the first call that should have worked and did not returned
    uint32_t parts_count;      // the parts the creature has
    uint32_t parts_scene;      // of them, those with a place in the world
    uint32_t parts_hidden;     // the parts the hold covered
    uint32_t parts_visible;    // what the engine says of the part being drawn: bit 0 before, 1 while held, 2 at the end of the hold, 3 after
    uint32_t parts_frames;     // the frames it was held hidden
    char parts_name[32];       // the part that was hidden

    // Parts of the framework's own added to the creature: one under its root, one at a bone of its
    // mesh. The first is removed again; the second is left for the creature's removal to take with it.
    uint32_t added_passed;     // one bit per step that did what it should
    int32_t added_result;      // what the first call that should have worked and did not returned
    uint32_t added_listed;     // the creature's parts once two were added
    float added_miss;          // how far the part added at a bone is from that bone
    char added_socket[32];     // the bone it was added at

    // A material held for eight seconds in the first slot of the creature's mesh that has one: a
    // material taken from another actor's mesh. Then the slot is given back.
    uint32_t paint_passed;     // one bit per step that did what it should
    int32_t paint_result;      // what the first call that should have worked and did not returned
    uint32_t paint_slots;      // the slots the mesh has, as the engine counts them
    uint32_t paint_slot;       // the slot; 99 when no slot has a material
    char paint_own[32];        // the material the slot had
    char paint_with[32];       // the material held in it

    // The behavior service: what the captain and the creature are doing, and every change of any
    // actor's from the moment the creature is made until just before it is removed.
    uint32_t behavior_passed;  // one bit per step that did what it should
    int32_t behavior_result;   // what the first call that should have worked and did not returned
    uint32_t behavior_events;  // changes told, of any actor
    uint32_t behavior_own;     // of them, the creature's
    uint32_t behavior_watched; // the most actors watched at once
    uint32_t behavior_mismatch; // changes told whose actor was by then doing something else again
    char behavior_avatar[64];  // what the captain was doing at the start
    char behavior_creature[64]; // what the creature was doing at the start
    char behavior_trail[256];  // what the creature went on to do, one after another

    // Pikmin deaths the game counted while the checks ran, if any Pikmin died.
    uint32_t died_heard;       // how many were told
    uint32_t died_cause;       // retired native death assertion, unused
    // The patch's native routes read `native_jump` to find the framework, as its code sites read `site_jump`. The
    // place was a retired field's; the rest of it stays unused.
    uint64_t native_jump;
    char died_kind[24];        // retired native death assertion, unused

    // Retired Pikmin death check. The SDK reports it; these layout fields stay unused.
    uint32_t caught_test;      // 0 before its former phase, 2 phase passed without native assertions
    uint32_t caught_passed;    // retired, stays zero
    uint32_t caught_heard;     // how many things it was sent were told
    float caught_amount;       // the amount of the last hit told

    // The remaining physical pause check and existing finish sequence. Fields for retired SDK-covered
    // checks stay in this frozen layout, unused; they do not report successful checks.
    uint32_t game_test;        // 0 not yet, 1 paused, 2 released, 3 asked for the title, 4 at the title
    uint32_t game_passed;      // bit 6 the creature did not move or change behavior during the hold
    int32_t game_result;       // what the first call that should have worked and did not returned
    uint32_t game_mode_title;  // the mode on the title screen
    uint32_t game_mode_level;  // and in the level
    int32_t game_rule;         // the rule in the level
    uint32_t game_held_kinds;  // the kinds paused while the hold was on
    uint32_t game_changes_held; // actors that started doing something else while the hold was on
    float game_moved_held;     // how far the creature moved while the hold was on
    uint32_t game_title_frames; // frames from asking for the title to being there
    char game_stage_title[64]; // the stages on the title screen
    char game_stage_level[64]; // and in the level
    char game_world[32];       // the level's name
    char game_trail[192];      // what the service told, one after another

    // The captains service: the captain's whistle and cursor held, a ride on the dog and off again, and a throw.
    uint32_t captains_test;      // the step it is on; 11 when finished
    uint32_t captains_passed;    // one bit per step that did what it should
    int32_t captains_result;     // what the first call that should have worked and did not returned
    uint32_t captains_whistles;  // whistles begun in 20 frames of a hold on a captain the player steers
    float captains_radius[4];    // the whistle's reach 2 and 20 frames into that hold, and into one on a suspended captain
    uint32_t captains_whistles_suspended; // whistles begun in 20 frames of a hold on a suspended captain
    float captains_cursor_off;   // how far the held cursor was from its point, in centimeters
    int32_t captains_ride;       // what asking for the ride returned
    uint32_t captains_ride_frames; // frames until the captain was on
    uint32_t captains_off_frames;  // frames until it was off again
    int32_t captains_throw;      // what the last throw asked for returned
    uint32_t captains_pikmin;    // glow Pikmin a throw was asked of
    float captains_thrown;       // how far the thrown one was from where it stood two seconds later

    // The pikmin and squad services, and a Pikmin killed through combat: the level's Pikmin listed and
    // described, one leaving the captain's squad and joining again, three new ones, the squad dismissed.
    uint32_t herd_test;          // the step it is on; 6 when finished
    uint32_t herd_passed;        // one bit per step that did what it should
    int32_t herd_result;         // what the first call that should have worked and did not returned
    uint32_t herd_count;         // Pikmin in play
    uint32_t herd_members;       // of them following the captain
    uint32_t herd_type;          // the first one's type
    uint32_t herd_leaf;          // and what it has on its head
    uint32_t herd_added;         // how many more there were ten frames after three were asked for
    uint32_t herd_left;          // following the captain a second and a half after the dismissal
    int32_t herd_dog_join;       // what joining the dog's squad returned
    uint32_t herd_dog_led;       // 1 when the Pikmin then followed the dog
    uint32_t herd_died_cause;    // the cause told for the one that was killed
    uint32_t herd_frames;        // frames until that was told
    uint32_t herd_leave_frames;  // frames until a Pikmin told to leave followed nobody

    // The creatures service: the level's creatures listed and described, and one of them defeated.
    uint32_t wild_test;          // the step it is on; 3 when finished
    uint32_t wild_passed;        // one bit per step that did what it should
    int32_t wild_result;         // what the first call that should have worked and did not returned
    uint32_t wild_count;         // creatures in play
    uint32_t wild_after;         // creatures in play two seconds after one was defeated
    uint32_t wild_defeated;      // of them defeated before the check did anything
    uint32_t wild_frames;        // frames from the blow until the one hit read as defeated
    uint32_t wild_still;         // 1 when the defeated one was still in play then
    char wild_kind[32];          // the kind of the one that was hit

    // More about the squad check's dismissal, which does not always empty the squad at once.
    uint32_t herd_left_frames;   // frames until nobody followed the captain, or 300
    uint32_t herd_straggler;     // which still followed at 45 frames: 0 none, 1 the one that left and joined, 2 a new one, 3 another
    char herd_doing[64];         // what that one was doing then
    char herd_dog_doing[56];     // what the one told to follow the dog was doing ten frames later

    // The hud and audio services: a banner shown for six seconds, and one of the captain's sounds.
    uint32_t hud_test;           // 1 while the banner is up, 2 when finished
    uint32_t hud_passed;         // one bit per step that did what it should
    int32_t hud_result;          // what showing the banner returned
    uint32_t hud_state;          // the banner's state a second after it was asked for
    float hud_wait_before;       // how long the game's own announcements stay
    float hud_wait_after;        // and the same once the banner was done
    int32_t audio_play;          // what playing the captain's whistle sound returned
    int32_t audio_missing;       // what playing a sound it does not have returned

    // The story service, at night: the day, the clock read, set and put back, and travel refused.
    uint32_t story_test;         // 1 when finished
    uint32_t story_passed;       // one bit per step that did what it should
    int32_t story_result;        // what the first call that should have worked and did not returned
    int32_t story_travel;        // what asking for travel to another area returned: at night the game takes none
    int32_t story_leave;         // what asking for the way out of a cave returned, above ground

    // By day the checks above do not run: they are written for one night level. In a level by day,
    // travel to another area is asked for and followed there, and then the game goes back to its title.
    uint32_t day_test;           // 0 not by day, 1 asking, 2 on the way, 3 arrived, 4 leaving
    uint32_t day_passed;         // one bit per step that did what it should
    int32_t day_first;           // what the first request for travel returned
    int32_t day_travel;          // and the last
    uint32_t day_tries;          // how many times it was asked
    uint32_t day_frames;         // frames from the request the game took until the other world had begun
    float day_time;              // the clock on arrival
    uint32_t day_day;            // the day on arrival
    char day_from[32];           // the world it began in
    char day_to[32];             // the world it arrived in
    char day_stage[64];          // the stages the game was in when travel was first asked for

    // The saves service. By night: the slot in use, and a hold taken and let go. By day: saves held
    // through the journey, which the game saves on.
    uint32_t saves_test;         // 1 when the night's part is finished
    uint32_t saves_passed;       // one bit per step that did what it should
    int32_t saves_hold;          // what taking the hold returned
    uint32_t saves_slot;         // the slot in use
    uint32_t day_saves_held;     // writes and deletions held back during the journey
    uint32_t day_saves_passed;   // and those that went on to the game
    char day_saves_last[56];     // the last file held back

    // A Pikmin sent to work on the spawned creature, and more about the dismissal that follows.
    int32_t herd_work;           // what sending it returned
    uint32_t herd_work_led;      // 1 when it still followed the captain a second later
    char herd_work_doing[48];    // what it was doing then
    uint32_t herd_redismissed;   // 1 when a second dismissal was asked for, five seconds after the first
    float herd_dog_life;         // the dog's life when the squad was dismissed
    char herd_dog_state[32];     // and what the dog was doing
    int32_t herd_dog_idle_join;  // what joining the dog returned while the dog only followed its captain
    uint32_t herd_dog_idle_led;  // 1 when the Pikmin then followed the dog ten frames later

    // A treasure counted and recorded as collected, through the story service.
    int32_t story_treasure;      // what recording it returned
    uint32_t story_treasure_before; // how many of it the story had
    uint32_t story_treasure_after;  // and afterward
    int32_t story_sparklium_gain;   // how much Sparklium the ship gained
    int32_t story_treasure_worth;   // what the treasure's row says it is worth
    uint32_t story_treasure_tables; // tables looked at to find the treasures' own
    char story_treasure_name[32];   // the treasure
    char story_treasure_table[96];  // the table it was found in

    // By day, after the journey: a creature brought near, defeated, watched until it is a carcass, and brought back.
    uint32_t day_wild_count;     // creatures in the level
    uint32_t day_wild_dormant;   // of them at rest because nothing is near
    uint32_t day_wild_awake;     // 1 when the chosen one was up three seconds after being brought near
    uint32_t day_wild_defeat_frames;  // frames from the blow until it read as defeated
    uint32_t day_wild_carcass_frames; // frames from then until the game marked it a carcass; 0 if it never did
    uint32_t day_wild_listed;    // 1 when it was still listed then
    int32_t day_wild_hit;        // what the blow returned
    int32_t day_wild_revive;     // what bringing it back returned
    uint32_t day_wild_revive_frames;  // frames until there was one more of its kind up and about; 0 if there never was
    uint32_t day_wild_old_alive; // 1 when the first actor was still in play then
    char day_wild_kind[32];      // its kind

    // The dialogs service: a message, two questions, and two more asked for at once, which whoever runs the check answers.
    uint32_t talk_test;          // 1 a message is up, 2 a question to accept, 3 one to back out of, 4 and 5 the two asked for at once, 6 a wait, 7 finished
    uint32_t talk_passed;        // one bit per step that did what it should
    int32_t talk_result;         // what the first call that should have worked and did not returned
    uint32_t talk_heard;         // how many answers were heard
    uint32_t talk_accepted;      // bit n set when the nth dialog asked for was accepted
    uint32_t talk_frames[5];     // frames each waited for its answer
    uint32_t talk_waiting;       // dialogs the service still kept two seconds after the last answer
    uint32_t talk_paused;        // the kinds of pause in force when the story's check began, after the dialogs
    uint32_t talk_busy;          // 1 when the game was busy with a save or a scene then
    uint32_t talk_spare;
    char talk_stage[32];         // the stages the game was in then

    // A slot's files and a stand-in: at the title screen the files of the slot in use are listed, read, and given
    // back as a stand-in for the same slot, which the game then loads the level from.
    uint32_t files_test;         // 1 the stand-in is in use, 2 ended with the level up, 9 the files could not be had
    uint32_t files_passed;       // one bit per step that did what it should
    int32_t files_result;        // what listing the slot's files returned
    uint32_t files_slot;         // the slot in use
    uint32_t files_count;        // how many files it holds
    uint32_t files_bytes;        // and how many bytes they hold together
    uint32_t files_largest;      // the largest of them
    uint32_t files_reads;        // files the game had read from the stand-in when the level was up
    uint32_t files_held;         // writes to the slot held back while the stand-in was in use
    uint32_t files_tries;        // times the listing was tried
    char files_first[48];        // the first path listed

    // The remaining trace through `reflect`, using the existing result bits 6 and 7.
    uint32_t clip_test;          // 0 not yet, 4 the trace finished
    uint32_t clip_passed;        // bit 6 hit the ground, bit 7 named the hit actor
    int32_t clip_result;         // what the first call that should have worked and did not returned
    uint32_t trace_hit;          // 1 when a trace down through the captain hit something
    float trace_z;               // how high the hit was
    float trace_from;            // how high the captain is
    float trace_distance;        // how far along the trace the hit was
    char trace_actor[32];        // what the trace hit

    // The mods that are installed: the checks' own, found, read and started at the title screen.
    uint32_t mods_test;          // 1 when the check has run
    uint32_t mods_passed;        // one bit per step that did what it should
    int32_t mods_result;         // what finding the mods returned
    uint32_t mods_found;         // how many folders the mods folder holds
    uint32_t mods_running;       // how many of them were started
    uint32_t mods_blocked;       // and how many cannot run
    uint32_t mods_log_lines;     // lines mods have written to the log
    uint32_t mods_spare;
    char mods_problem[96];       // why the mod with the broken manifest cannot run
    char mods_listed[64];        // what the library mod's folder holds

    // Mods written in Lua: one of the checks' own, run from its main.lua, tells what it finds by what it prints.
    uint32_t script_passed;      // one bit per line it should print: started, helper, language, caught, frames, actors
    uint32_t script_lines;       // how many lines it has printed
    char script_line[64];        // the last of them
    char script_problem[96];     // why the script mod that fails as it starts cannot run
    char script_note[160];       // the first line of the language check that is not as it is on the machine the checks were built on

    // Compiled mods: one of the checks' own runs, and one refuses to start.
    uint32_t compiled_passed;    // one bit per step: started, memory, frames, actors, the refuser ran, and how the three ended up
    uint32_t compiled_loaded;    // how many mods' code has been loaded
    uint32_t compiled_started;   // and how many times a mod's start has been called
    uint32_t compiled_refused;   // what the system's loader last answered when it refused something
    char compiled_line[64];      // the last line the compiled mod wrote
    char compiled_problem[96];   // why the one that refuses cannot run

    // A mod's saved data: the framework's own context keeps a piece, reads it back, replaces it and forgets it.
    uint32_t keep_test;          // 1 done, 9 the first piece could not be kept
    uint32_t keep_passed;        // one bit per step that did what it should
    int32_t keep_result;         // what keeping the first piece returned
    uint32_t keep_tries;         // times it was tried
    char keep_listed[48];        // the first name listed

    // Settings: one of the checks' own mods reads its own and another mod's, changes some, and they are kept.
    uint32_t settings_passed;    // one bit per step: found at their defaults, the library's, the refusals, the changes heard, written, the file as it should be, found as a prepared save holds them
    uint32_t settings_read;      // mods whose values were read from the save
    uint32_t settings_unread;    // and whose file could not be read
    uint32_t settings_written;   // files written
    uint32_t settings_unwritten; // and writes the save would not take
    uint32_t settings_changes;   // values that changed
    char settings_found[64];     // what the mod found as it started
    char settings_file[160];     // what the save holds for it afterward

    // What has been done for mods' "maps": told every frame, whatever checks run.
    uint32_t map_parts;           // entries of running mods' "maps"
    uint32_t map_levels_asked;    // levels of mods' own the engine took
    uint32_t map_levels_refused;  // and would not take
    uint32_t map_entries_taken;   // sublevel entries whose actors were all there
    uint32_t map_entries_refused; // and were not
    uint32_t map_removed;         // actors of the game's removed
    uint32_t map_moved;           // and moved
    uint32_t nav_meshes;          // navmeshes whose tiles were swapped whole
    uint32_t nav_removed;         // tiles of the game's taken out
    uint32_t nav_added;           // tiles of mods' put in
    uint32_t nav_refused;         // tiles a navmesh would not take
    uint32_t map_values_refused;  // actor values a map change would not set
    uint32_t newmap_menus_listed; // lists of areas rebuilt with mods' maps
    uint32_t map_values_set;      // actor values successfully set by map changes

    // What mods' shader sets and held material slots cost: told every frame, whatever checks run.
    uint32_t shader_level_requests; // game material packages asked to load as a level, taken or not
    uint32_t shader_copies_asked;   // of those, the engine took
    uint32_t shader_copies_ready;   // copies given the mods' shaders
    uint32_t shader_stand_ins;      // stand-ins made
    uint32_t shader_failed;         // materials left with the game material's shaders
    uint32_t shader_searches;       // full object walks for a material or an awaited copy
    uint32_t shader_parts_asked;    // meshes asked for the materials of their slots
    uint32_t shader_slots_set;      // slots given a stand-in
    uint32_t hold_kept;             // held material slots looked at, a step at a time
    uint32_t hold_changed;          // of those, ones the game had put something else in than was last shown
    uint32_t hold_set;              // setter calls that put the hold's material back
    uint32_t shader_steps;          // shader steps timed, while there were sets
    uint32_t shader_step_us;        // their time together, in microseconds
    uint32_t shader_step_longest_us; // and the longest of them
    uint32_t hold_passes;           // passes over every table of holds
    uint32_t hold_pass_us;          // their time together
    uint32_t hold_pass_longest_us;  // and the longest of them
    uint32_t shader_full_walks;     // materials whose parents reached a mod's instance, so were checked in full

    // Native gateway allocation and the hook engine's shared budgets.
    NativeHookStats native_hooks;
    HookStats hook_budgets;

    // Which build of the game the framework selected: its key, and its place in the table's list.
    char game_build[24];
    uint32_t build_index;
    // What came of telling the player about a build the table does not describe (status.h's Notice).
    uint32_t notice;
    // The API families this build cannot serve: bit N is family N of game/families.yaml.
    uint64_t families_absent;
};
static_assert(sizeof(Status) == 6352, "tools read this block by its layout; raise `version` when it changes");
static_assert(offsetof(Status, site_jump) == game::table::stub::status_site_jump, "the patch reads this field by its place");
static_assert(offsetof(Status, native_jump) == game::table::constant::native_jump_offset, "the patch's native routes read this field by its place");
constexpr uint64_t kMagic = game::table::stub::status_magic;
constexpr uint32_t kCheckFrames = 300, kCheckAgainFrames = 1500, kHookTestStart = 400, kHookTestFrames = 300,
                   kSiteTestStart = 800, kReflectTestFrame = 1200, kObjectsTestFrame = 1300, kLevelActors = 20,
                   kLevelSettleSamples = 10;

extern Status status;
extern uint64_t tick_entry; // where the engine keeps its per-frame function
extern TSUCHI_Mod* self;       // the framework's own context
extern bool mods_started;   // true once the mods have been found and started
// Where a line of the log goes: the system's debug output, and the log's file in the save.
void log_line(const char* mod_id, const char* text);

inline void copy_text(char* to, size_t capacity, const char* from) {
    size_t i = 0;
    for (; from[i] && i + 1 < capacity; ++i) to[i] = from[i];
    to[i] = 0;
}

// The framework's own checks, in a build made with TSUCHI_SELFCHECK. They run in the game, do
// things to it no player asked for, and are not part of the build people run.
void selfcheck_start();
void selfcheck_frame();

// Copies service counters without running a check or changing the game.
void observe_status();

} // namespace tsuchi::target
