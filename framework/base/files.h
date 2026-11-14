// Files, through the file functions of the system the game runs on, which the
// framework reaches by the game's own calls to them. A path names a place the
// game has opened: `rom:/...` for what the game ships with and mods add to,
// `save:/...` for the save while the game's own session for it is open.
//
// Game thread only.
#pragma once
#include <stdint.h>

#include "game.h"
#include "list.h"
#include "tsuchi/core.h"
#include "texts.h"

namespace tsuchi::game {

// Optional context for framework-owned mounts; ordinary RomFS/save calls keep their context.
inline const void* (*file_context_enter)(const char*) = nullptr;
inline void (*file_context_leave)(const char*, const void*) = nullptr;
struct FileScope {
    const char* path; const void* previous;
    explicit FileScope(const char* p) : path(p), previous(file_context_enter ? file_context_enter(p) : nullptr) {}
    ~FileScope() { if (file_context_leave) file_context_leave(path, previous); }
};

// True when the game's calls to the file functions are known.
bool files_known();

// A path that leads nowhere but below where it starts: it is not empty, does not begin at the top, steps back out
// of no folder, and names no other place to the file functions.
bool path_stays_within(const char* path);

// Opens a file to be read and hands it to `use` with its size; closes it afterward.
// TSUCHI_NOT_FOUND when nothing is at the path, TSUCHI_IO when it could not be opened or measured.
template <typename Use>
TSUCHI_Result with_file(const char* path, Use&& use) {
    FileScope scope(path);
    void* file = nullptr;
    const uint32_t opened = table::fn::fs_open_file(&file, const_cast<char*>(path), int32_t(table::constant::fs_read));
    if (opened) return opened == table::constant::fs_no_such_path ? TSUCHI_NOT_FOUND : TSUCHI_IO;
    int64_t size = 0;
    const TSUCHI_Result result = table::fn::fs_file_size(&size, file) == 0 && size >= 0 ? use(file, uint64_t(size)) : TSUCHI_IO;
    table::fn::fs_close_file(file);
    return result;
}

TSUCHI_Result file_size(const char* path, uint64_t& size);
// Up to `room` bytes from `offset` on: fewer at the end of the file, and none past it.
TSUCHI_Result read_file_part(const char* path, uint64_t offset, void* buffer, uint64_t room, uint64_t& got);
// The whole of a file in the framework's memory, to be released by the caller. TSUCHI_NO_MEMORY when there is no
// memory for it.
TSUCHI_Result read_whole_file(const char* path, char*& data, uint64_t& size);

// What one folder holds: the names of its files and of its folders, each added to its list. One folder is open at
// a time. TSUCHI_NOT_FOUND when there is no such folder, TSUCHI_NO_MEMORY when there is no memory for the names.
TSUCHI_Result list_folder(const char* path, Texts& files, Texts& folders);
// The same, with each file's size added to `sizes` in the order of `files`, as the listing gives it: no file is
// opened for it. kSizeUnknown for each file on a build whose listing has no sizes.
constexpr uint64_t kSizeUnknown = UINT64_MAX;
TSUCHI_Result list_folder(const char* path, Texts& files, Texts& folders, List<uint64_t>& sizes);

} // namespace tsuchi::game
