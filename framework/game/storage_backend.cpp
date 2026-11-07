// The game side of the storage service.
//
// A mod's saved data is files of its own in the player's save, beside the
// slots' folders: `save:/tsuchi/<id>/data/<name>`. They are read and written
// through the file functions of the system the game runs on, with the save
// opened the way the game opens it, and closed again before the call returns.
//
// A piece is written the way the game writes a file of its own: one that is not
// there is made at its size, one that is there is given its new size, and the
// bytes are written from its start. Then the save is told to keep the change.
// No file is renamed: an emulator has been seen to fail that for a file that
// was just written, and the system's file functions stop the game when it does.

#include <string.h>

#include "files.h"
#include "flow.h"
#include "game.h"
#include "registry.h"
#include "save_session.h"
#include "services.h"
#include "storage_service.h"
#include "texts.h"

namespace tsuchi {

namespace {

namespace t = game::table;

// Where a mod's data is, or one piece of it.
bool place(Texts& out, const TSUCHI_Mod* mod, const char* name) {
    if (!out.add(game::kSavedMods, "/", mod_id(mod), "/data")) return false;
    if (!*name) return true;
    --out.used; // the terminator goes, and the rest follows
    return out.add("/", name);
}

bool writing_known() {
    return t::bound(t::fn::fs_create_file) && t::bound(t::fn::fs_write_file) && t::bound(t::fn::fs_delete_file) && t::bound(t::fn::fs_set_file_size) && t::bound(t::fn::fs_create_folder) && t::fn::fs_entry_kind &&
           t::fn::fs_commit;
}

// A piece's name is a path that stays within the mod's data and names a file: no part of it is empty.
bool names_a_piece(const char* name) {
    if (!game::path_stays_within(name) || name[strlen(name) - 1] == '/') return false;
    for (const char* at = name; *at; ++at)
        if (at[0] == '/' && at[1] == '/') return false;
    return true;
}

// What stands in the way of reaching a mod's data now. `name` is null when no one piece is meant.
TSUCHI_Result refusal(const char* name) {
    if (name && !names_a_piece(name)) return TSUCHI_INVALID;
    if (!game::save_session_known() || !writing_known()) return TSUCHI_NOT_READY;
    return game::busy_with_saves_or_scenes() ? TSUCHI_BUSY : TSUCHI_OK;
}

enum class Found { nothing, file, folder };
Found what_is_at(const char* path) {
    int32_t kind = 0;
    if (t::fn::fs_entry_kind(&kind, const_cast<char*>(path)) != 0) return Found::nothing;
    return uint32_t(kind) == t::constant::fs_entry_folder ? Found::folder : Found::file;
}

// Makes the folders a path's file is in, from the top down, where they are not there yet.
bool make_folders(char* path) {
    for (char* at = path + strlen("save:/"); *at; ++at) {
        if (*at != '/') continue;
        *at = 0;
        const Found found = what_is_at(path);
        const bool there = found == Found::folder || (found == Found::nothing && t::fn::fs_create_folder(path) == 0);
        *at = '/';
        if (!there) return false;
    }
    return true;
}

// Writes a piece whole. False when any of it fails; a piece that was only just made is then taken away again.
bool write_piece(char* path, const void* data, uint64_t size) {
    if (!make_folders(path)) return false;
    const Found found = what_is_at(path);
    if (found == Found::folder || (found == Found::nothing && t::fn::fs_create_file(path, int64_t(size)) != 0)) return false;
    void* file = nullptr;
    bool whole = t::fn::fs_open_file(&file, path, int32_t(t::constant::fs_write)) == 0;
    if (whole) {
        uint32_t how = uint32_t(t::constant::fs_write_through);
        whole = (found == Found::nothing || t::fn::fs_set_file_size(file, int64_t(size)) == 0) &&
                (!size || t::fn::fs_write_file(file, 0, const_cast<void*>(data), size, &how) == 0);
        t::fn::fs_close_file(file);
    }
    if (!whole && found == Found::nothing) t::fn::fs_delete_file(path);
    return whole;
}

// Runs `use` on the path of one of a mod's pieces, with the save open around it.
template <typename Use>
TSUCHI_Result with_piece(TSUCHI_Mod* mod, const char* name, Use&& use) {
    const TSUCHI_Result refused = refusal(name);
    if (refused != TSUCHI_OK) return refused;
    Texts where;
    if (!place(where, mod, name)) return TSUCHI_NO_MEMORY;
    TSUCHI_Result result = TSUCHI_IO;
    {
        game::SaveSession session;
        if (session.open) result = use(where.data);
    }
    where.clear();
    return result;
}

// What the save holds now is what it keeps.
TSUCHI_Result kept() { return t::fn::fs_commit(const_cast<char*>("save")) == 0 ? TSUCHI_OK : TSUCHI_IO; }

TSUCHI_Result save(TSUCHI_Mod* mod, const char* name, const void* data, uint64_t size) {
    return with_piece(mod, name, [&](char* path) { return write_piece(path, data, size) ? kept() : TSUCHI_IO; });
}

TSUCHI_Result load(TSUCHI_Mod* mod, const char* name, uint64_t offset, void* buffer, uint64_t capacity, uint64_t* out) {
    return with_piece(mod, name, [&](char* path) { return game::read_file_part(path, offset, buffer, capacity, *out); });
}

TSUCHI_Result exists(TSUCHI_Mod* mod, const char* name, bool* out) {
    return with_piece(mod, name, [&](char* path) {
        *out = what_is_at(path) == Found::file;
        return TSUCHI_OK;
    });
}

TSUCHI_Result size(TSUCHI_Mod* mod, const char* name, uint64_t* out) {
    return with_piece(mod, name, [&](char* path) { return game::file_size(path, *out); });
}

TSUCHI_Result remove(TSUCHI_Mod* mod, const char* name) {
    return with_piece(mod, name, [&](char* path) {
        if (what_is_at(path) != Found::file) return TSUCHI_NOT_FOUND;
        return t::fn::fs_delete_file(path) == 0 ? kept() : TSUCHI_IO;
    });
}

TSUCHI_Result list(TSUCHI_Mod* mod, char* buffer, uint64_t capacity, uint64_t* out) {
    const TSUCHI_Result refused = refusal(nullptr);
    if (refused != TSUCHI_OK) return refused;
    game::SaveSession session;
    if (!session.open) return TSUCHI_IO;
    Texts found, folders; // the pieces, and the folders still to be read, each by its path within the mod's data: "" is all of it
    bool memory = folders.add("");
    for (size_t at = 0; memory && at < folders.used; at += strlen(folders.data + at) + 1) {
        Texts where, names, inside; // the folders inside this one are read once it is closed
        memory = place(where, mod, folders.data + at) && game::list_folder(where.data, names, inside) != TSUCHI_NO_MEMORY;
        where.clear();
        for (const char* name = names.first(); memory && name; name = names.after(name)) memory = found.add(folders.data + at, folders.data[at] ? "/" : "", name);
        for (const char* name = inside.first(); memory && name; name = inside.after(name)) memory = folders.add(folders.data + at, folders.data[at] ? "/" : "", name);
        names.clear(), inside.clear();
    }
    folders.clear();
    const TSUCHI_Result result = memory ? found.give(buffer, capacity, out) : TSUCHI_NO_MEMORY;
    found.clear();
    return result;
}

const storage::Backend service{save, load, exists, size, remove, list};

} // namespace

void storage_start() { storage::set_backend(&service); }

TSUCHI_Result write_save_file(const char* path, const void* data, uint64_t size) {
    const TSUCHI_Result refused = refusal(nullptr);
    if (refused != TSUCHI_OK) return refused;
    Texts where; // the folders on the way are made by putting an end to the path at each in turn
    if (!where.add(path)) return TSUCHI_NO_MEMORY;
    TSUCHI_Result result = TSUCHI_IO;
    {
        game::SaveSession session;
        if (session.open && write_piece(where.data, data, size)) result = kept();
    }
    where.clear();
    return result;
}

TSUCHI_Result append_save_file(const char* path, const void* data, uint64_t size, bool from_start) {
    const TSUCHI_Result refused = refusal(nullptr);
    if (refused != TSUCHI_OK) return refused;
    Texts where;
    if (!where.add(path)) return TSUCHI_NO_MEMORY;
    TSUCHI_Result result = TSUCHI_IO;
    {
        game::SaveSession session;
        const Found found = session.open && make_folders(where.data) ? what_is_at(where.data) : Found::folder;
        // A file that is not there is made at the size of what is added. One that is there grows by that much,
        // from its start when it is to hold nothing else.
        void* file = nullptr;
        if (found != Found::folder && (found == Found::file || t::fn::fs_create_file(where.data, int64_t(size)) == 0) &&
            t::fn::fs_open_file(&file, where.data, int32_t(t::constant::fs_write)) == 0) {
            int64_t at = 0;
            uint32_t how = uint32_t(t::constant::fs_write_through);
            const bool placed = found == Found::nothing || from_start || (t::fn::fs_file_size(&at, file) == 0 && at >= 0);
            const bool grown = placed && found == Found::file && t::fn::fs_set_file_size(file, at + int64_t(size)) == 0;
            const bool whole = placed && (found == Found::nothing || grown) && (!size || t::fn::fs_write_file(file, at, const_cast<void*>(data), size, &how) == 0);
            // Room that was made and not written to is taken back: an emulator's save keeps what it was never told to keep.
            if (!whole && grown) t::fn::fs_set_file_size(file, at);
            t::fn::fs_close_file(file);
            if (!whole && found == Found::nothing) t::fn::fs_delete_file(where.data);
            if (whole) result = kept();
        }
    }
    where.clear();
    return result;
}

TSUCHI_Result read_save_file(const char* path, char*& data, uint64_t& size) {
    const TSUCHI_Result refused = refusal(nullptr);
    if (refused != TSUCHI_OK) return refused;
    game::SaveSession session;
    return session.open ? game::read_whole_file(path, data, size) : TSUCHI_IO;
}

} // namespace tsuchi
