// The engine and game services. Each start function gives its service the code
// that does its work, and is called once when the framework starts.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

void net_start();
void session_start();
void session_set_clock(double (*clock)());

void actors_start();
void entities_start();
void components_start();
void collision_start();
void behavior_start();
void gimmicks_start();
void battle_start(TSUCHI_Mod* self);
void combat_start();
void game_start();
void players_start();
void camera_start();
void captains_start();
void pikmin_start();
void squad_start();
void creatures_start();
void assets_start();
void tables_start();
void text_start();
// The manifest's "tables" and "text": changes a mod declares, made as it starts. After `mods_start`.
void declared_start();
// The manifest's "maps": a mod's levels loaded into the game's maps, and the game's actors there removed,
// moved, or given other values. After `mods_start`.
void maps_start();
// The shared level initialization hook belongs to the framework.
void map_changes_start(TSUCHI_Mod* self);
// How many entries of running mods' "maps" there are.
uint32_t map_parts();
// Whether a map part can contribute values in this world.
bool map_part_ready(uint64_t id);
bool map_part_failed(uint64_t id);
// What has been done for them since the framework started.
struct MapCounts {
    uint32_t levels_asked, levels_refused;   // levels of mods' own the engine took, and would not take
    uint32_t nested_asked, nested_refused;   // levels those brought with them: the same two
    uint32_t entries_taken, entries_refused; // sublevel entries whose actors were all there, and were not
    uint32_t removed, moved;                 // actors of the game's
    uint32_t values_set, values_refused;     // says on a value of an actor's put in force, and says the game would not take
    uint32_t values_put_back;                // values put back as they were before any mod's say
};
MapCounts map_counts();
// The manifest's "content": new treasures and their series, and new creatures, made as a mod starts. After
// `mods_start`.
void content_start();
void content_service_start();
void content_squad_start(TSUCHI_Mod* self);
// What has become of them: the treasures of the mods that run now, and since the framework started, treasures
// made, rows added to the game's tables, rows entered in a loaded world's lists and lists that could not be
// entered in, mods blocked by what their "content" declares, numbers given to IDs that had none, and times the
// record of those numbers could not be read from the save or written to it.
struct ContentCounts {
    uint32_t treasures, declared, rows_added, entered, not_entered, refused, numbers_given, records_unread, records_unwritten;
};
ContentCounts content_counts();
// Parameters, classes, icons, treasure catalog, creatures and creature catalog, in that order.
struct ContentEntryCounts {
    struct Entry { uint32_t no_table, keeper_not_ready, other_table, no_insert, bad_list, already, no_row; } tables[6];
};
ContentEntryCounts content_entry_counts();
// The same for the creatures a manifest's "content" declares: the creatures of the mods that run now, and since
// the framework started, creatures made, rows added to the game's tables, rows of the Piklopedia's table taken out
// again, such rows that wait now for a world to go, rows entered in a loaded world's lists and lists that could
// not be entered in, mods blocked by a creature they declare, numbers given, and times the record of those
// numbers could not be read from the save or written to it.
struct CreatureCounts {
    uint32_t creatures, declared, rows_added, rows_removed, rows_left, entered, not_entered, refused, numbers_given, records_unread, records_unwritten;
};
CreatureCounts creature_counts();
// The manifest's "shaders": a mod's own materials drawn with the shaders the mod brings. After `mods_start`.
// What has been done for them is in shader_materials.h.
void shaders_start();
// What has been done to navmeshes for mods' navigation since the framework started.
struct NavigationCounts;
NavigationCounts map_navigation_counts();
// What has been done with mods' placement lists since the framework started.
struct PlacementCounts;
PlacementCounts map_placement_counts();
// The manifest's AI values for creatures the game places: the context that owns the wraps of an AI part's
// start, which are put on the first time a mod brings such values. And what has been done with them.
void map_ai_start(TSUCHI_Mod* self);
struct AiCounts;
AiCounts map_ai_counts();
void hud_start();
void audio_start();
void effects_start(TSUCHI_Mod* self);
void world_start();
void anim_start();
void story_start();
// Wraps the two calls in which the game sets up a dialog, for the framework's own context. The first is entered
// through `setup_entry`, which hands on the request the game is setting up from: see `tsuchi_dialog_on_setup`.
void dialogs_start(TSUCHI_Mod* self, void* setup_entry);
// How many dialogs mods have asked for that the game has not let go of yet.
unsigned dialogs_waiting();
void storage_start();
// For what the framework itself keeps in the save, beside mods' data. Each opens the save around itself and closes
// it again, and is refused as `storage` is while the game is busy with a save. A file is written whole and the save
// told to keep it; one that is read is the caller's to release.
TSUCHI_Result write_save_file(const char* path, const void* data, uint64_t size);
TSUCHI_Result read_save_file(const char* path, char*& data, uint64_t& size);
// Adds bytes to the end of a file in the save, or with `from_start` puts them in place of all it held. A file that
// is not there is made.
TSUCHI_Result append_save_file(const char* path, const void* data, uint64_t size, bool from_start);
// One run's log on the SD card, atmosphere/contents/<program ID>/tsuchi.log, with save:/tsuchi/tsuchi.log
// as a fallback. Lines wait in memory and the file is written as frames go by.
void log_file_start();
void log_file_line(const char* mod_id, const char* text);
// What has become of the log's lines since the framework started: lines taken and lines dropped once the log was
// full, bytes written to the file and bytes that wait, times the file was written, and writes the save refused.
struct LogFileCounts {
    uint32_t lines, dropped, written_bytes, waiting_bytes, writes, refused;
};
LogFileCounts log_file_counts();
void settings_start();
// What has become of mods' settings: mods whose values were read from the save, and whose file could not be read or
// was no JSON; files written, and writes the save would not take; values that changed.
struct SettingsCounts {
    uint32_t read, unread, written, unwritten, changes;
};
SettingsCounts settings_counts();
// Wraps the save system's functions for writing and deleting a file, and those for asking after one and reading it,
// for the framework's own context.
void saves_start(TSUCHI_Mod* self);
// What has come through them: how many writes and deletions went on to the game, how many were held back, the last
// file held back, and how many files the game has read from a stand-in.
struct SaveCounts {
    uint32_t passed, held_back;
    char last[56];
    uint32_t stand_in_reads;
};
SaveCounts save_counts();
// How many fields of table rows mods have changed now.
unsigned table_changes();
// How many texts mods have replaced now.
unsigned text_replacements();
// How many pauses mods hold now.
unsigned game_pause_holds();
// How many actors the behavior service is keeping an eye on: none while no mod is listening.
unsigned behavior_watched();
// A mod's say on how long an array in a row of a table is, which is a hold as a change to a field is: the say of
// the mod started last is in force, and with none left the row has the array it had at first, untouched. While a
// say is in force the row has an array of that length: copies of the first array's elements, new elements past
// them, and what mods have changed in single elements. TSUCHI_NOT_FOUND for a table, a row or a path that is not
// there, TSUCHI_INVALID for a field that is no array, TSUCHI_UNSUPPORTED for an array of structs or of another
// kind that cannot be copied, TSUCHI_CAPACITY past the longest array the framework makes.
TSUCHI_Result table_resize(TSUCHI_Mod* mod, TSUCHI_Object table, const char* row, const char* path, uint32_t count);
// Takes the mod's say back. TSUCHI_NOT_FOUND when it has none on that array.
TSUCHI_Result table_restore_length(TSUCHI_Mod* mod, TSUCHI_Object table, const char* row, const char* path);
// How many such says mods have now.
unsigned table_lengths();

// The carry service: carried objects, their carriers and destination, and collection credit.
void carry_start(TSUCHI_Mod* self);

// The input service: controllers read once per world step, buttons pressed and hidden, and the keyboard.
void input_start(TSUCHI_Mod* self);

} // namespace tsuchi

// The game sets up a dialog's widget: what the first of those calls is led to, with the request as a fourth argument.
extern "C" void tsuchi_dialog_on_setup(void* widget, uint32_t kind, int32_t layout, void* request);
