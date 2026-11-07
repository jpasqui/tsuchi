// The log's file on the SD card, with the player's save as a fallback.
//
// What mods write with `mod.log`, and what the framework writes of its own, is
// kept on the SD at atmosphere/contents/<program ID>/tsuchi.log, falling back
// to save:/tsuchi/tsuchi.log if mounting or the first write is refused. Each line
// begins with whose it is. The file holds one run. Its first write replaces what
// the run before left, and every later write adds to the end.
//
// Lines wait in memory until the selected destination can be written, at most
// once in two seconds, so that a mod that logs every frame costs the save one
// write in sixty frames. A run writes a bounded amount: a save has little room,
// and a console's storage wears with every write.

#include <string.h>

#include "frame.h"
#include "game.h"
#include "modules.h"
#include "registry.h"
#include "sd_card.h"
#include "services.h"

namespace tsuchi {

namespace {

constexpr char kLogFile[] = "save:/tsuchi/tsuchi.log";
constexpr uint32_t kLogBytes = 256 * 1024;     // what a run writes to the file at most, and what waits at most
constexpr uint32_t kLineBytes = 1000;          // of one line's text
constexpr uint32_t kFramesBetweenWrites = 60;  // two seconds
constexpr uint32_t kFramesBetweenTries = 300;  // after the save would not take a write
constexpr char kFull[] = "[tsuchi] the log is full: what is written after this is not in the file\n";

char* waiting = nullptr; // the lines still to write, one after another
uint32_t waiting_used = 0, waiting_room = 0;
uint32_t taken = 0;       // bytes this run has taken for the file, written or waiting
uint32_t wait_frames = 0; // until the file is written to again
bool begun = false;       // true once this run has written to the file
bool full = false;
LogFileCounts counts{};

namespace t = game::table;
struct {
    bool selected;
} sd{};
bool destination_chosen = false;
using FsScope = SdScope;

// The log's path on the SD, in the folder of the program this build of the game is.
void sd_log_path(char (&path)[64]) {
    constexpr char kBefore[] = "tsuchisd:/atmosphere/contents/", kAfter[] = "/tsuchi.log";
    memcpy(path, kBefore, sizeof kBefore - 1);
    char* at = path + sizeof kBefore - 1;
    const uint64_t program = t::constant::program_id;
    for (int shift = 60; shift >= 0; shift -= 4) *at++ = "0123456789ABCDEF"[(program >> shift) & 15];
    memcpy(at, kAfter, sizeof kAfter);
}

bool mount_sd() {
    if (!t::bound(t::fn::fs_entry_kind) || !t::bound(t::fn::fs_create_folder) || !t::bound(t::fn::fs_create_file) || !t::bound(t::fn::fs_open_file) ||
        !t::bound(t::fn::fs_close_file) || !t::bound(t::fn::fs_file_size) || !t::bound(t::fn::fs_set_file_size) || !t::bound(t::fn::fs_write_file) || !t::bound(t::fn::fs_delete_file))
        return false;
    return sd_card_mounted();
}

// Unlike savedata this filesystem needs no save session or commit. The context makes denied permissions and
// missing paths return errors instead of aborting the game. Every file is closed before that context is restored.
TSUCHI_Result append_sd(const void* data, uint64_t size, bool from_start) {
    FsScope scope;
    char path[64];
    sd_log_path(path);
    for (char* at = path + strlen("tsuchisd:/"); *at; ++at) {
        if (*at != '/') continue;
        *at = 0;
        int32_t kind = -1;
        const uint32_t found = t::fn::fs_entry_kind(&kind, path);
        const bool there = !found ? uint32_t(kind) == t::constant::fs_entry_folder
                                 : found == t::constant::fs_no_such_path && t::fn::fs_create_folder(path) == 0;
        *at = '/';
        if (!there) return TSUCHI_IO;
    }
    int32_t kind = -1;
    const uint32_t found = t::fn::fs_entry_kind(&kind, path);
    const bool made = found == t::constant::fs_no_such_path;
    if ((!made && found) || (!found && uint32_t(kind) == t::constant::fs_entry_folder) ||
        (made && t::fn::fs_create_file(path, int64_t(size)) != 0))
        return TSUCHI_IO;
    void* file = nullptr;
    if (t::fn::fs_open_file(&file, path, int32_t(t::constant::fs_write)) != 0) {
        if (made) t::fn::fs_delete_file(path);
        return TSUCHI_IO;
    }
    int64_t before = 0;
    const bool placed = t::fn::fs_file_size(&before, file) == 0 && before >= 0;
    const int64_t at = from_start ? 0 : before;
    const bool grown = placed && t::fn::fs_set_file_size(file, at + int64_t(size)) == 0;
    uint32_t how = uint32_t(t::constant::fs_write_through);
    const bool whole = grown && (!size || t::fn::fs_write_file(file, at, const_cast<void*>(data), size, &how) == 0);
    if (!whole && grown) t::fn::fs_set_file_size(file, before);
    t::fn::fs_close_file(file);
    if (!whole && made) t::fn::fs_delete_file(path);
    return whole ? TSUCHI_OK : TSUCHI_IO;
}

// Room for `bytes` more of waiting lines. False when there is no memory.
bool make_room(uint32_t bytes) {
    if (waiting_used + bytes <= waiting_room) return true;
    uint32_t next = waiting_room ? waiting_room * 2 : 4096;
    while (next < waiting_used + bytes) next *= 2;
    auto* grown = static_cast<char*>(allocate(next));
    if (!grown) return false;
    if (waiting_used) memcpy(grown, waiting, waiting_used);
    release(waiting);
    waiting = grown, waiting_room = next;
    return true;
}

void add(const char* bytes, uint32_t count) {
    memcpy(waiting + waiting_used, bytes, count);
    waiting_used += count, taken += count;
}

void step() {
    counts.waiting_bytes = waiting_used;
    if (wait_frames) return void(--wait_frames);
    if (!waiting_used) return;
    if (!destination_chosen) sd.selected = mount_sd(), destination_chosen = true;
    TSUCHI_Result result = sd.selected ? append_sd(waiting, waiting_used, !begun)
                                      : append_save_file(kLogFile, waiting, waiting_used, !begun);
    // Keep every queued line if the SD mount exists but the first write is denied. Once a run has written to
    // the SD, later failures stay on that destination and retry; moving only the tail would split the run.
    if (sd.selected && !begun && result != TSUCHI_OK) {
        sd.selected = false;
        result = append_save_file(kLogFile, waiting, waiting_used, true);
    }
    if (result == TSUCHI_IO) wait_frames = kFramesBetweenTries, ++counts.refused;
    if (result != TSUCHI_OK) return; // not yet, or the game is busy with a save: tried again next frame
    begun = true;
    counts.written_bytes += waiting_used, ++counts.writes;
    waiting_used = counts.waiting_bytes = 0;
    wait_frames = kFramesBetweenWrites;
}

} // namespace

void log_file_line(const char* mod_id, const char* text) {
    ++counts.lines;
    // As much of the text as a line takes, and no part of a character cut in two.
    size_t length = strlen(text);
    if (length > kLineBytes) {
        length = kLineBytes;
        while (length && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) --length;
    }
    const size_t id_length = strlen(mod_id), bytes = id_length + length + 4; // the brackets, the space, and the line's end
    if (full || taken + bytes + (sizeof kFull - 1) > kLogBytes) {
        // The file's last line says that there is more than it holds. Room for it was kept all along.
        if (!full && make_room(sizeof kFull - 1)) add(kFull, sizeof kFull - 1);
        full = true;
        ++counts.dropped;
        return;
    }
    if (!make_room(uint32_t(bytes))) return void(++counts.dropped);
    add("[", 1), add(mod_id, uint32_t(id_length)), add("] ", 2);
    char* const line = waiting + waiting_used;
    add(text, uint32_t(length));
    for (size_t i = 0; i < length; ++i) // one call is one line
        if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';
    add("\n", 1);
}

void log_file_start() { each_frame(step); }

LogFileCounts log_file_counts() { return counts; }

} // namespace tsuchi
