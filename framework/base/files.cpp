#include "files.h"

#include "registry.h"

namespace tsuchi::game {

namespace t = table;

bool files_known() {
    return t::bound(t::fn::fs_open_file) && t::bound(t::fn::fs_close_file) && t::bound(t::fn::fs_file_size) && t::bound(t::fn::fs_read_file) && t::bound(t::fn::fs_open_folder) && t::fn::fs_read_folder &&
           t::fn::fs_close_folder;
}

bool path_stays_within(const char* path) {
    if (!*path || *path == '/') return false;
    for (const char* at = path; *at; ++at) {
        if (*at == ':' || *at == '\\') return false;
        if (*at == '.' && at[1] == '.' && (at == path || at[-1] == '/') && (!at[2] || at[2] == '/')) return false;
    }
    return true;
}

TSUCHI_Result file_size(const char* path, uint64_t& size) {
    return with_file(path, [&](void*, uint64_t bytes) {
        size = bytes;
        return TSUCHI_OK;
    });
}

TSUCHI_Result read_file_part(const char* path, uint64_t offset, void* buffer, uint64_t room, uint64_t& got) {
    return with_file(path, [&](void* file, uint64_t size) {
        got = 0;
        if (offset >= size || !room) return TSUCHI_OK;
        const uint64_t wanted = size - offset < room ? size - offset : room;
        uint64_t read = 0;
        if (t::fn::fs_read_file(&read, file, int64_t(offset), buffer, wanted) != 0) return TSUCHI_IO;
        got = read;
        return TSUCHI_OK;
    });
}

TSUCHI_Result read_whole_file(const char* path, char*& data, uint64_t& size) {
    return with_file(path, [&](void* file, uint64_t bytes) {
        auto* made = static_cast<char*>(allocate(bytes ? bytes : 1));
        if (!made) return TSUCHI_NO_MEMORY;
        uint64_t read = 0;
        if (bytes && (t::fn::fs_read_file(&read, file, 0, made, bytes) != 0 || read != bytes)) {
            release(made);
            return TSUCHI_IO;
        }
        data = made, size = bytes;
        return TSUCHI_OK;
    });
}

namespace {
TSUCHI_Result list_into(const char* path, Texts& files, Texts& folders, List<uint64_t>* sizes) {
    FileScope scope(path);
    auto* entry = static_cast<uint8_t*>(allocate(t::fs_entry::size));
    if (!entry) return TSUCHI_NO_MEMORY;
    void* folder = nullptr;
    // Only a path with nothing at it is a missing folder. Any other refusal is a failed read.
    if (const uint32_t opened = t::fn::fs_open_folder(&folder, const_cast<char*>(path), int32_t(t::constant::fs_list_all))) {
        release(entry);
        return opened == t::constant::fs_no_such_path ? TSUCHI_NOT_FOUND : TSUCHI_IO;
    }
    bool memory = true, failed = false;
    int64_t got = 0;
    while (memory) { // one entry at a time, until the folder has no more
        failed = t::fn::fs_read_folder(&got, entry, folder, 1) != 0;
        if (failed || got != 1) break;
        const bool folder_entry = entry[t::fs_entry::kind] == t::constant::fs_entry_folder;
        Texts& list = folder_entry ? folders : files;
        memory = list.add(reinterpret_cast<const char*>(entry + t::fs_entry::name));
        if (memory && sizes && !folder_entry) {
            int64_t bytes = -1;
            if (t::present(t::fs_entry::bytes)) memcpy(&bytes, entry + t::fs_entry::bytes, sizeof bytes);
            memory = sizes->add(bytes >= 0 ? uint64_t(bytes) : kSizeUnknown) != nullptr;
        }
    }
    t::fn::fs_close_folder(folder);
    release(entry);
    // A listing cut short is not the folder's contents.
    if (failed) return TSUCHI_IO;
    return memory ? TSUCHI_OK : TSUCHI_NO_MEMORY;
}
} // namespace

TSUCHI_Result list_folder(const char* path, Texts& files, Texts& folders) { return list_into(path, files, folders, nullptr); }

TSUCHI_Result list_folder(const char* path, Texts& files, Texts& folders, List<uint64_t>& sizes) {
    return list_into(path, files, folders, &sizes);
}

} // namespace tsuchi::game
