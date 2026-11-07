// Independent sound banks. All banks and voices are records in the shared hold tables.
#pragma once
#include <stdint.h>
#include "registry.h"

struct TSUCHI_FileService;

namespace tsuchi {

struct ContentSoundBackend {
    bool (*ready)();
    TSUCHI_Result (*load)(const void*, uint32_t, uint32_t*);
    TSUCHI_Result (*unload)(uint32_t);
    TSUCHI_Result (*play)(uint32_t, const TSUCHI_Vec3&, uint64_t, uint32_t*);
    bool (*active)(uint32_t);
    TSUCHI_Result (*stop)(uint32_t);
};

// The target starts the native backend. Host tests supply their made-up game's backend.
void content_sound_start();
void content_sound_set_backend(const ContentSoundBackend* backend);
TSUCHI_Result content_sound_load(TSUCHI_Mod* mod, const char* id, const void* data, uint32_t count, uint64_t* bank);
// Manifest admission can wait for the sound engine without blocking its mod.
TSUCHI_Result content_sound_declare(TSUCHI_Mod* mod, const char* id, const void* data, uint32_t count);
TSUCHI_Result content_sound_declare_file(TSUCHI_Mod* mod, const char* id, const char* path, const TSUCHI_FileService* files);
TSUCHI_Result content_sound_find(TSUCHI_Mod* mod, const char* id, uint64_t* bank);
// A sequence of terminated event IDs and an extra terminal NUL. Nothing is copied on capacity.
TSUCHI_Result content_sound_events(TSUCHI_Mod* mod, uint64_t bank, char* data, uint64_t capacity, uint64_t* size);
TSUCHI_Result content_sound_play(TSUCHI_Mod* mod, uint64_t bank, const char* event, const TSUCHI_Vec3& location, uint64_t* voice);
TSUCHI_Result content_sound_stop(TSUCHI_Mod* mod, uint64_t voice);
TSUCHI_Result content_sound_unload(TSUCHI_Mod* mod, uint64_t bank);

} // namespace tsuchi
