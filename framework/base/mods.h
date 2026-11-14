// The mods that are installed: where they are, what their manifests say, which
// of them can run, and in what order they start.
//
// A mod is one folder, `rom:/tsuchi/mods/<id>/`, with a manifest in it, `mod.json`.
// The folder's name is the mod's ID. The manifest is read strictly: a key the
// framework does not know blocks the mod, and the mod list says why.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "json.h"
#include "registry.h"
#include "settings.h"
#include "text.h"
#include "texts.h"

namespace tsuchi {

enum class ModState : uint8_t {
    blocked, // it cannot run, and `problem` says why
    ready,   // it can run, and has its place in the order
    running, // it has its context
    stopped,
    disabled, // the player turned it off before containers were mounted
};

struct ModRecord {
    char id[kModIdBytes]; // its folder's name
    Text name, version;   // as its manifest gives them
    Text problem;         // why it is blocked
    uint32_t parts[3];    // its version, in numbers
    const Json* manifest; // the whole of mod.json, kept for as long as the framework runs; null when it could not be read
    TSUCHI_Mod* context;     // from manifest preparation through running, until stopped
    Settings settings;    // as its manifest declares them, and as they are now
    ModState state;
    uint32_t order;       // its place among those that start, from 0
    uint32_t rank;        // its first context's rank, kept by every restart; 0 until it has had a context
    bool dev_folder;      // its development folder on the SD card was there as it last started
    bool restart_asked;   // a restart waits for the end of the step
    uint64_t dev_print;   // its development folder's files, by path and size, as last seen; 0 for none
    uint64_t dev_content; // their bytes, as last read through
    uint64_t dev_partial; // the bytes read so far of the pass under way, and the file it goes on from
    uint32_t dev_next;
    // For the framework's own use while it works out which can run and in what order.
    uint32_t seen;
    bool circular;
    bool manifest_valid; // read and admitted before activation or dependency decisions
    const char* clash;
    const char* clash_version;
    bool override_active = false;
    bool override_conflict = false;
    bool prepared = false; // the opted-in early declarations began successfully in this context
    bool game_restart = false; // native allocations/effects require a new game process before restarting this mod
};

// Where mods are looked for.
extern const char* const kModsFolder;

// Something that runs a mod's own code: a script language, or compiled mods. A mod is its kind when its folder
// holds the file it starts from.
struct ModRuntime {
    const char* entry; // the file's path within the mod's folder, such as `main.lua`
    // Starts the mod, which has its context. Anything but TSUCHI_OK blocks the mod; `problem` can say why.
    TSUCHI_Result (*start)(TSUCHI_Mod* mod, Text& problem);
    // Compiles the entry of the mod with this ID without running it, before a restart stops the mod. Anything
    // but TSUCHI_OK keeps the mod as it is; `problem` says why. Null for a kind of mod with nothing to compile.
    TSUCHI_Result (*check)(const char* id, Text& problem) = nullptr;
};
// Adds a kind of mod. A mod is started by the first kind added whose file it has. False when there is no memory.
bool add_mod_runtime(const ModRuntime& runtime);
// A part of the manifest that another layer of the framework understands: a key at the top of mod.json.
struct ManifestSection {
    const char* key;
    // Whether the part is written as it should be, looked at as the manifest is read. False blocks the mod;
    // `wrong` says why, and is left empty when there was no memory to say it.
    bool (*well_formed)(const Json& part, Text& wrong);
    // Does what the part declares for a mod that has its context, before the mod's code runs. Anything but
    // TSUCHI_OK blocks the mod, and what was done for it is undone with everything else of its; `problem` can say why.
    TSUCHI_Result (*begin)(TSUCHI_Mod* mod, const Json& part, Text& problem);
    // Internal declarations that must exist before a world loads opt in. Their context is adopted by startup;
    // settings preparation and code still wait for the normal startup boundary. Three-field users stay late.
    bool before_world = false;
};
bool add_manifest_section(const ManifestSection& section);
// A key of the manifest this framework has, whose family this build of the game cannot serve. A mod that uses it is
// blocked for that reason, and not as a key nobody knows.
bool add_manifest_key_off(const char* key, const char* family);
// The same declaration paths are used by the content service. Validation does not change the game.
TSUCHI_Result manifest_part(const char* key, TSUCHI_Mod* mod, const Json& part, Text& problem, bool apply);
// What is done for a mod that has its context, before its code runs.
void set_before_start(void (*prepare)(ModRecord& record));
void set_setting_override_changed(void (*changed)(ModRecord&, const Setting&));
void refresh_setting_overrides();

// Reads one manifest into a record that has its ID. False when the mod is blocked by what its manifest says or
// fails to say; `problem` then tells why.
bool read_manifest(ModRecord& record, const char* text, size_t bytes);

// Finds the mods in the mods folder, reads their manifests, and works out which can run and in what order.
// TSUCHI_NOT_READY while files or saved choices are unavailable, and TSUCHI_NO_MEMORY when memory runs out;
// nothing is published then. Parsed manifests remain private while saved choices are pending.
TSUCHI_Result find_mods();
// Blocks a mod that was found able to run, with a reason of up to four parts, and every mod that needs it. Gives
// false, so a caller can return it.
bool block_mod(ModRecord& record, const char* a, const char* b = "", const char* c = "", const char* d = "");
// After conclusive selection and container/library mounting, create contexts in selected order and begin only
// explicitly early sections. Records stay ready; repeated calls do not repeat completed declarations.
void prepare_mods();
// Starts every mod that can run, in order: each gets its context, and its code is run if it has any. A mod whose
// code fails at once is stopped and blocked, and so is every mod that needs it.
void start_mods();
// Stops every running mod, last started first, and forgets all of them.
void forget_mods();

bool mod_requirement_satisfied(const ModRecord& installed, const char* version);
uint32_t mod_count();
// By ID order.
ModRecord* mod_at(uint32_t index);
ModRecord* find_mod(const char* id);

// ---- Restarts, and the development folder ----------------------------------------------------------------

// Restarts every mod a restart was asked for, with the mods that require it: see DESIGN.md, "Restarting a mod".
// Runs after every mod has heard the step, so that no script is running.
void run_restarts();
// Whether the game is in a state where mods can be stopped and started. Restarts wait while it gives false. With
// none set, they never wait.
void set_restart_gate(bool (*ready)());
// The development folders: `root` is the folder that holds one folder for each mod, as the file functions name
// it. `begin` makes it readable and gives false when it cannot be read; `end` follows a `begin` that gave true.
// Either may be null. With no root, no mod has a development folder.
void set_dev_folder(const char* root, bool (*begin)(), void (*end)());
// Watches the development folders: every `steps` steps, a mod that has had its turn is restarted when its
// development folder was made, taken away, or had a file added, removed or changed since it last started. 0, as
// before the first call, watches nothing. See DESIGN.md, "The development folder".
//
// Each look lists the folder: a file added, removed or of another size is seen at once, and no file is opened for
// it. The files' bytes, 1 MiB of them in all, are read a few at a time, in passes over the folder (kWatchFiles
// files and kWatchBytes bytes a look, at least one file), so a change that keeps a file's size is seen at the end
// of a pass: the one under way when the file was still to be read, the next when it had been read. A mod with a
// few small files has a pass every look.
void set_dev_watch(uint32_t steps);
// Looks at the development folders when the interval set_dev_watch gave has passed. Runs after each step, before
// run_restarts.
void watch_dev_folders();
// The whole of one of a mod's files, as the `file` service reads it: from its development folder first. For a
// runtime's compile check, which runs while the mod may have no context. The caller releases `data`.
TSUCHI_Result read_mod_file(const char* id, const char* path, char*& data, uint64_t& size);

// Where one of a mod's files is, for the file functions. False when the path leads out of the mod's folder (an
// empty path is the folder itself) or there is no memory.
bool mod_file_path(Texts& out, const TSUCHI_Mod* mod, const char* within);

// What mods write with `mod.log` goes to whoever asks for it here. Lines are counted either way.
void set_log_sink(void (*sink)(const char* mod_id, const char* text));
// Writes a line to the log as a mod's, by its ID, whether or not the mod has started.
void mod_note(const char* id, const char* text);
uint32_t log_lines();
// Whose the framework's own lines in the log are: an ID no mod can have.
inline constexpr const char* kFrameworkLogId = "tsuchi";
// Writes to the log what the framework found: how many mods, and for each whether it runs or why it cannot. Called
// once the mods have been started.
void log_mods();

// Gives the `mod` and `files` services their code. Called once when the framework starts.
void mods_start();

} // namespace tsuchi
