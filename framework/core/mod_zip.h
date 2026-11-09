// Bounded ZIP reader and streaming DEFLATE decoder for downloaded packages.
// No engine calls. Only the supplied worker IO touches disk; commit means extraction finished,
// not that the loader may discover the package. Manifest validation and publication follow.
#pragma once
#include <stdint.h>
namespace tsuchi::mod_zip {
struct Cancellation { bool (*stopped)(void*); void* user; uint32_t load() const { return stopped && stopped(user); } };
constexpr uint32_t max_entries=256, max_name=512;
constexpr uint64_t max_archive=512ull<<20, max_expanded=1ull<<30;
constexpr uint32_t conflict_result=0xfffffffe,limit_result=0xfffffffd;
enum class Error { None, Storage, Zip, Unsupported, Path, Limit, Crc, Exists, Cancelled, Conflict };
enum class State { Validating, Extracting, Committing };
// derived_id: the mod ID a cooked container or cheat files are installed under, made from the package's
// source and key. Empty refuses both: only Tsuchi mods are admitted then.
struct Request {char cache_key[128];char derived_id[48]{};uint64_t program_id=0;};
struct Status {State state;uint32_t entry_count,completed_files;uint64_t compressed_done,compressed_total,expanded_done,expanded_total;char group[96],current_file[max_name];bool assets=false,cheats=false;};
struct Outcome {Error error;uint32_t result;bool committed;};
struct Platform {
    void* user;
    void* (*allocate)(uint64_t);void (*free)(void*);
    uint32_t (*input_open)(void*,const char*,void**,uint64_t*);
    uint32_t (*input_read)(void*,void*,uint64_t,void*,uint32_t,uint32_t*);
    void (*input_close)(void*,void*);
    uint32_t (*stage_begin)(void*,const char*,void**);
    uint32_t (*output_open)(void*,void*,const char*,uint64_t,void**);
    uint32_t (*output_write)(void*,void*,uint64_t,const void*,uint32_t);
    uint32_t (*output_finish)(void*,void*);
    uint32_t (*commit)(void*,void*);
    uint32_t (*abort)(void*,void*);
    uint32_t (*input_remove)(void*,const char*)=nullptr;
};
Outcome extract_zip(const Platform&,const Request&,const Cancellation&,Status&,void(*progress)(void*),void*);
uint64_t workspace_bytes();
}
