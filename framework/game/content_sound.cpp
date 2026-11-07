#include "content_sound.h"

#include <string.h>
#include "content_sound_format.h"
#include "frame.h"
#include "game.h"
#include "holds.h"
#include "ids.h"
#include "tsuchi/file.h"

namespace tsuchi {
namespace {
namespace format = sound_bank_format;
constexpr uint32_t bank_limit = 16, voice_limit = 128;
const ContentSoundBackend* backend = nullptr;
uint64_t next_handle = 1;

struct BankData {
    format::Bank parsed;
    char id[kIdBytes];
    uint32_t calls, pending_bytes;
    TSUCHI_Result load_result;
    const TSUCHI_FileService* pending_file;
};
struct BankKept { BankData* data; uint32_t native; bool retired; uint64_t handle; };
struct BankWanted { BankData* data; const void* bytes; uint32_t count; bool* consumed; uint32_t native; bool retired; uint64_t handle; bool ending; bool deferred = false; };
struct VoiceKept { uint64_t bank; uint32_t native; bool retired; };
struct VoiceWanted { uint64_t bank; uint32_t event; TSUCHI_Vec3 location; uint32_t native; bool retired; bool ending; };

bool ready() {
    return backend && backend->ready && backend->load && backend->unload && backend->play && backend->active && backend->stop && backend->ready();
}
TSUCHI_Result apply_bank(const Hold&, BankKept&, const BankWanted&, bool);
bool keep_bank(const Hold&, BankKept&);
void restore_bank(const Hold&, BankKept&, HoldEnd);
TSUCHI_Result apply_voice(const Hold&, VoiceKept&, const VoiceWanted&, bool);
bool keep_voice(const Hold&, VoiceKept&);
void restore_voice(const Hold&, VoiceKept&, HoldEnd);
Holds<BankKept, BankWanted, apply_bank, keep_bank, restore_bank> banks{"content.bank", HoldOn::nothing, true};
Holds<VoiceKept, VoiceWanted, apply_voice, keep_voice, restore_voice> voices{"content.voice", HoldOn::nothing, true};

// Native calls can stop a mod synchronously through other framework hooks.
// Keep its bank alive until a post or stop that is already in progress returns.
struct VoiceCall {
    BankData* data = nullptr;
    explicit VoiceCall(uint64_t bank) {
        for (uint32_t i = 0; i < banks.count(); ++i) {
            auto& kept = banks.kept_at(i);
            if (kept.handle == bank) { data = kept.data; ++data->calls; break; }
        }
    }
    ~VoiceCall() { if (data) --data->calls; }
};

// The banks that hold or will hold something in the sound engine. A bank whose load failed keeps its record,
// so the mod can still read why, but it no longer takes a place of the shared limit.
uint32_t banks_in_use() {
    uint32_t used = 0;
    for (uint32_t i = 0; i < banks.count(); ++i) {
        const BankKept& kept = banks.kept_at(i);
        if (kept.retired || kept.native || !kept.data || kept.data->pending_file || kept.data->pending_bytes) ++used;
    }
    return used;
}
bool voices_on(uint64_t bank) {
    for (uint32_t i = 0; i < voices.count(); ++i)
        if (voices.kept_at(i).bank == bank) return true;
    return false;
}
void release_voices(uint64_t bank) {
    for (uint32_t i = voices.count(); i--;) {
        if (i >= voices.count() || voices.kept_at(i).bank != bank || voices.kept_at(i).retired) continue;
        const Hold hold = voices.hold_at(i);
        voices.release(hold.mod, hold.object, hold.what);
    }
}

TSUCHI_Result apply_bank(const Hold& hold, BankKept& kept, const BankWanted& wanted, bool fresh) {
    if (!fresh) {
        if (!wanted.ending) return TSUCHI_BUSY;
        if (!ready() || !hold_call_guard(hold).permits()) return TSUCHI_NOT_READY;
        const TSUCHI_Result result = backend->unload(kept.native);
        if (result == TSUCHI_OK) kept.native = 0;
        return result;
    }
    kept = {wanted.data, wanted.native, wanted.retired, wanted.retired ? wanted.handle : hold.object};
    if (wanted.consumed) *wanted.consumed = true;
    if (wanted.retired) return TSUCHI_OK;
    if (wanted.deferred && hold_call_guard(hold).permits()) return TSUCHI_OK;
    if (!ready() || !hold_call_guard(hold).permits()) {
        release(kept.data);
        kept = {};
        return TSUCHI_NOT_READY;
    }
    hold_apply_checkpoint(hold);
    const TSUCHI_Result result = backend->load(wanted.bytes, wanted.count, &kept.native);
    if (result != TSUCHI_OK) {
        // A refused load has not copied a bank. Backends leave the output zero.
        release(kept.data);
        kept = {};
    }
    return result;
}
TSUCHI_Result load_file(const Hold& hold, BankKept& kept) {
    auto& data=*kept.data;
    for (uint32_t i=0;i<banks.count();++i) {
        const auto& other=banks.kept_at(i);
        if (other.retired && other.data && id_same(other.data->id,data.id)) return TSUCHI_NOT_READY;
    }
    const char* path=reinterpret_cast<const char*>(&data+1);
    uint64_t size=0,read=0;
    auto result=data.pending_file->size(hold.mod,path,&size);
    if (!hold_call_guard(hold).permits()) return TSUCHI_NOT_READY;
    if (result!=TSUCHI_OK) return result;
    if (!size) return TSUCHI_INVALID;
    if (size>format::MAX_BYTES) return TSUCHI_CAPACITY;
    void* bytes=allocate(size_t(size));
    if (!bytes) return TSUCHI_NO_MEMORY;
    if (hold_call_guard(hold).permits()) result=data.pending_file->read(hold.mod,path,0,bytes,size,&read);
    else result=TSUCHI_NOT_READY;
    if (!hold_call_guard(hold).permits()) result=TSUCHI_NOT_READY;
    if (result==TSUCHI_OK && read!=size) result=TSUCHI_IO;
    if (result==TSUCHI_OK && !format::parse(bytes,uint32_t(size),data.parsed)) result=TSUCHI_INVALID;
    bool retiring_collision=false;
    if (result==TSUCHI_OK) for (uint32_t i=0;i<banks.count();++i) {
        const auto* other=banks.kept_at(i).data;
        if (other && other!=&data && format::collides(other->parsed,data.parsed)) {
            if (banks.kept_at(i).retired) retiring_collision=true;
            else {result=TSUCHI_BUSY;break;}
        }
    }
    if (result==TSUCHI_OK && retiring_collision) result=TSUCHI_NOT_READY;
    if (result==TSUCHI_OK && hold_call_guard(hold).permits()) result=backend->load(bytes,uint32_t(size),&kept.native);
    release(bytes);
    return result;
}
bool keep_bank(const Hold& hold, BankKept& kept) {
    if (!kept.retired) {
        if (kept.data->pending_file) {
            if (!ready() || !hold_call_guard(hold).permits()) return true;
            const auto result=load_file(hold,kept);
            kept.data->load_result=result;
            if (result!=TSUCHI_NOT_READY && result!=TSUCHI_NO_MEMORY) kept.data->pending_file=nullptr;
            if (!kept.native) kept.data->parsed={};
            return true;
        }
        if (!kept.data->pending_bytes || !ready() || !hold_call_guard(hold).permits()) return true;
        const auto result=backend->load(kept.data+1,kept.data->pending_bytes,&kept.native);
        kept.data->load_result=result;
        if (result!=TSUCHI_NOT_READY) kept.data->pending_bytes=0;
        if (!kept.native && !kept.data->pending_bytes) kept.data->parsed={}; // a failed bank collides with nothing
        return true;
    }
    if (!ready() || !hold_call_guard(hold).permits() || kept.data->calls || voices_on(kept.handle)) return true;
    if (backend->unload(kept.native) != TSUCHI_OK) return true;
    kept.native = 0;
    return false;
}
void restore_bank(const Hold&, BankKept& kept, HoldEnd) {
    if (!kept.data) return;
    if (kept.native) {
        // The stopped mod owns nothing. The shared table keeps the native resource
        // for the framework until sound returns, including its collision IDs.
        const BankWanted retirement{kept.data, nullptr, 0, nullptr, kept.native, true, kept.handle, false};
        banks.take(nullptr, next_handle++, 0, retirement);
        release_voices(kept.handle);
        return;
    }
    release(kept.data);
    kept = {};
}

TSUCHI_Result apply_voice(const Hold& hold, VoiceKept& kept, const VoiceWanted& wanted, bool fresh) {
    if (!fresh) {
        if (!wanted.ending) return TSUCHI_BUSY;
        if (!ready() || !hold_call_guard(hold).permits()) return TSUCHI_NOT_READY;
        VoiceCall call(kept.bank);
        if (!backend->active(kept.native)) { kept.native = 0; return TSUCHI_NOT_FOUND; }
        if (!hold_call_guard(hold).permits()) return TSUCHI_NOT_READY;
        const TSUCHI_Result result = backend->stop(kept.native);
        if (result == TSUCHI_OK) kept.native = 0;
        return result;
    }
    kept = {wanted.bank, wanted.native, wanted.retired};
    if (wanted.retired) return TSUCHI_OK;
    if (!ready() || !hold_call_guard(hold).permits()) return TSUCHI_NOT_READY;
    const BankKept* bank = banks.find(hold.mod, wanted.bank, 0);
    if (!bank || bank->retired) return TSUCHI_NOT_FOUND;
    VoiceCall call(wanted.bank);
    hold_apply_checkpoint(hold);
    return backend->play(wanted.event, wanted.location, 0x5453554300000000ull | hold.object, &kept.native);
}
bool keep_voice(const Hold& hold, VoiceKept& kept) {
    if (!ready()) return true;
    VoiceCall call(kept.bank);
    if (!kept.native || !backend->active(kept.native)) {
        kept.native = 0;
        return false;
    }
    if (!hold_call_guard(hold).permits()) return false;
    if (kept.retired && backend->stop(kept.native) == TSUCHI_OK) {
        kept.native = 0;
        return false;
    }
    return true;
}
void restore_voice(const Hold&, VoiceKept& kept, HoldEnd) {
    if (!kept.native) return;
    VoiceCall call(kept.bank);
    if (!ready() || (backend->active(kept.native) && backend->stop(kept.native) != TSUCHI_OK)) {
        const VoiceWanted retirement{kept.bank, 0, {}, kept.native, true, false};
        voices.take(nullptr, next_handle++, 0, retirement);
    }
    kept.native = 0;
}

#ifndef TSUCHI_CONTENT_SOUND_TEST
namespace t = game::table;
bool native_ready() { return t::bound(t::fn::wwise_initialized) && t::fn::wwise_initialized(); }
TSUCHI_Result native_load(const void* bytes, uint32_t count, uint32_t* out) {
    *out = 0;
    if (!native_ready() || !t::bound(t::fn::wwise_load_bank_copy)) return TSUCHI_NOT_READY;
    const uint32_t result = t::fn::wwise_load_bank_copy(const_cast<void*>(bytes), count, out);
    if (result != 1) { *out = 0; return TSUCHI_UNSUPPORTED; }
    return TSUCHI_OK;
}
TSUCHI_Result native_unload(uint32_t bank) {
    if (!native_ready() || !t::bound(t::fn::wwise_unload_bank)) return TSUCHI_NOT_READY;
    return t::fn::wwise_unload_bank(bank, nullptr) == 1 ? TSUCHI_OK : TSUCHI_UNSUPPORTED;
}
TSUCHI_Result native_play(uint32_t event, const TSUCHI_Vec3& point, uint64_t emitter, uint32_t* out) {
    *out = 0;
    if (!native_ready() || !t::bound(t::fn::wwise_register_object) || !t::bound(t::fn::wwise_unregister_object) || !t::bound(t::fn::wwise_set_position) || !t::bound(t::fn::wwise_post_event))
        return TSUCHI_NOT_READY;
    if (t::fn::wwise_register_object(emitter) != 1) return TSUCHI_UNSUPPORTED;
    const float transform[9]{1, 0, 0, 0, 0, 1, point.x, point.y, point.z};
    const uint32_t positioned = t::fn::wwise_set_position(emitter, const_cast<float*>(transform));
    if (positioned == 1) *out = t::fn::wwise_post_event(event, emitter, 0, nullptr, nullptr, 0, nullptr, 0);
    t::fn::wwise_unregister_object(emitter);
    return positioned != 1 ? TSUCHI_UNSUPPORTED : *out ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}
bool native_active(uint32_t id) { return native_ready() && t::bound(t::fn::wwise_event_from_playing) && t::fn::wwise_event_from_playing(id) != 0; }
TSUCHI_Result native_stop(uint32_t id) {
    if (!native_ready() || !t::bound(t::fn::wwise_stop_playing)) return TSUCHI_NOT_READY;
    t::fn::wwise_stop_playing(id, 0, 4);
    return TSUCHI_OK;
}
const ContentSoundBackend native_backend{native_ready, native_load, native_unload, native_play, native_active, native_stop};
#endif
} // namespace

void content_sound_set_backend(const ContentSoundBackend* value) {
    if (!on_game_thread() || banks.count() || voices.count()) return;
    backend = value;
    banks.start();
    voices.start();
}
void content_sound_start() {
#ifndef TSUCHI_CONTENT_SOUND_TEST
    content_sound_set_backend(&native_backend);
#endif
}

static TSUCHI_Result admit_bank(TSUCHI_Mod* mod, const char* id, const void* data, uint32_t count, uint64_t* out, bool deferred) {
    if (out) *out = 0;
    if (!mod_usable(mod) || !out || !data || !count || count > format::MAX_BYTES) return TSUCHI_INVALID;
    char owner[kModIdBytes];
    const char* name = nullptr;
    if (!id_split(id, owner, &name) || strcmp(owner, mod_id(mod)) != 0) return TSUCHI_INVALID;
    const bool waiting=!ready();
    if (waiting && !deferred) return TSUCHI_NOT_READY;
    auto* parsed = static_cast<BankData*>(allocate(sizeof(BankData)+(waiting?count:0)));
    if (!parsed) return TSUCHI_NO_MEMORY;
    parsed->calls = 0;
    parsed->pending_file=nullptr;
    parsed->pending_bytes=waiting?count:0;
    parsed->load_result=waiting?TSUCHI_NOT_READY:TSUCHI_OK;
    if (waiting) memcpy(parsed+1,data,count);
    if (!format::parse(data, count, parsed->parsed)) { release(parsed); return TSUCHI_INVALID; }
    memcpy(parsed->id, id, strlen(id) + 1);
    for (uint32_t i = 0; i < banks.count(); ++i) {
        const BankData* existing = banks.kept_at(i).data;
        if (existing && (id_same(existing->id, id) || format::collides(existing->parsed, parsed->parsed))) {
            release(parsed);
            return TSUCHI_BUSY;
        }
    }
    if (banks_in_use() >= bank_limit) { release(parsed); return TSUCHI_CAPACITY; }
    // Reserve retirement room before native code can acquire anything.
    if (!banks.reserve(1)) { release(parsed); return TSUCHI_NO_MEMORY; }
    const uint64_t handle = next_handle++;
    bool consumed = false;
    const BankWanted wanted{parsed, data, count, &consumed, 0, false, handle, false, waiting};
    const TSUCHI_Result result = banks.take(mod, handle, 0, wanted);
    if (!consumed) release(parsed);
    if (result == TSUCHI_OK) *out = handle;
    return result;
}
TSUCHI_Result content_sound_load(TSUCHI_Mod* mod, const char* id, const void* data, uint32_t count, uint64_t* out) {
    return admit_bank(mod,id,data,count,out,false);
}
TSUCHI_Result content_sound_declare(TSUCHI_Mod* mod, const char* id, const void* data, uint32_t count) {
    uint64_t bank=0;
    return admit_bank(mod,id,data,count,&bank,true);
}
TSUCHI_Result content_sound_declare_file(TSUCHI_Mod* mod, const char* id, const char* path, const TSUCHI_FileService* files) {
    char owner[kModIdBytes];const char* name=nullptr;
    if (!mod_usable(mod) || !path || !*path || !files || !files->size || !files->read ||
        !id_split(id,owner,&name) || strcmp(owner,mod_id(mod))) return TSUCHI_INVALID;
    const size_t bytes=strlen(path)+1;
    if (bytes>4096) return TSUCHI_CAPACITY;
    bool replacement=false;
    for (uint32_t i=0;i<banks.count();++i) {
        const auto& other=banks.kept_at(i);
        if (!id_same(other.data->id,id)) continue;
        if (!other.retired) return TSUCHI_BUSY;
        replacement=true;
    }
    // The retiring bank already reserves the replacement's native slot.
    if (!replacement && banks_in_use()>=bank_limit) return TSUCHI_CAPACITY;
    auto* data=static_cast<BankData*>(allocate(sizeof(BankData)+bytes));
    if (!data) return TSUCHI_NO_MEMORY;
    *data={};data->pending_file=files;data->load_result=TSUCHI_NOT_READY;
    memcpy(data->id,id,strlen(id)+1);memcpy(data+1,path,bytes);
    if (!banks.reserve(1)) {release(data);return TSUCHI_NO_MEMORY;}
    bool consumed=false;
    const uint64_t handle=next_handle++;
    const BankWanted wanted{data,nullptr,0,&consumed,0,false,handle,false,true};
    const auto result=banks.take(mod,handle,0,wanted);
    if (!consumed) release(data);
    return result;
}
TSUCHI_Result content_sound_find(TSUCHI_Mod* mod, const char* id, uint64_t* out) {
    if (out) *out = 0;
    if (!mod_usable(mod) || !out || !id_valid(id)) return TSUCHI_INVALID;
    for (uint32_t i = 0; i < banks.count(); ++i) {
        const BankKept& kept = banks.kept_at(i);
        if (kept.retired || !kept.data || !id_same(kept.data->id, id)) continue;
        if (banks.hold_at(i).mod != mod) return TSUCHI_DENIED;
        if (!kept.native) return kept.data->load_result;
        *out = kept.handle;
        return TSUCHI_OK;
    }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result content_sound_events(TSUCHI_Mod* mod, uint64_t bank, char* data, uint64_t capacity, uint64_t* size) {
    if (size) *size = 0;
    if (!mod_usable(mod) || !size || (!data && capacity)) return TSUCHI_INVALID;
    if (banks.holder(bank, 0) && banks.holder(bank, 0) != mod) return TSUCHI_DENIED;
    const BankKept* found = banks.find(mod, bank, 0);
    if (!found || found->retired) return TSUCHI_NOT_FOUND;
    uint64_t required = 1;
    for (uint32_t i = 0; i < found->data->parsed.count; ++i) {
        char id[kIdBytes];
        id_placeholder(found->data->parsed.events[i], id);
        required += strlen(id) + 1;
    }
    *size = required;
    if (capacity < required) return TSUCHI_CAPACITY;
    uint64_t at = 0;
    for (uint32_t i = 0; i < found->data->parsed.count; ++i) {
        char id[kIdBytes];
        id_placeholder(found->data->parsed.events[i], id);
        const uint64_t bytes = strlen(id) + 1;
        memcpy(data + at, id, size_t(bytes));
        at += bytes;
    }
    data[at] = 0;
    return TSUCHI_OK;
}
TSUCHI_Result content_sound_play(TSUCHI_Mod* mod, uint64_t bank, const char* event, const TSUCHI_Vec3& location, uint64_t* out) {
    if (out) *out = 0;
    if (!mod_usable(mod) || !out || !finite(&location)) return TSUCHI_INVALID;
    uint32_t number = 0;
    if (id_form(event, nullptr, &number) != IdForm::placeholder) return id_valid(event) ? TSUCHI_NOT_FOUND : TSUCHI_INVALID;
    if (banks.holder(bank, 0) && banks.holder(bank, 0) != mod) return TSUCHI_DENIED;
    const BankKept* found = banks.find(mod, bank, 0);
    if (!found || found->retired || !format::contains(found->data->parsed, number)) return TSUCHI_NOT_FOUND;
    if (!ready()) return TSUCHI_NOT_READY;
    if (voices.count() >= voice_limit) return TSUCHI_CAPACITY;
    if (!voices.reserve(1)) return TSUCHI_NO_MEMORY;
    const uint64_t handle = next_handle++;
    const VoiceWanted wanted{bank, number, location, 0, false, false};
    const TSUCHI_Result result = voices.take(mod, handle, 0, wanted);
    if (result == TSUCHI_OK) *out = handle;
    return result;
}
TSUCHI_Result content_sound_stop(TSUCHI_Mod* mod, uint64_t voice) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    if (voices.holder(voice, 0) && voices.holder(voice, 0) != mod) return TSUCHI_DENIED;
    VoiceKept* kept = voices.find(mod, voice, 0);
    if (!kept || kept->retired) return TSUCHI_NOT_FOUND;
    if (!ready()) return TSUCHI_NOT_READY;
    VoiceWanted ending{};
    ending.ending = true;
    const TSUCHI_Result stopped = voices.take(mod, voice, 0, ending);
    if (stopped == TSUCHI_NOT_FOUND) { voices.release(mod, voice, 0); return stopped; }
    if (stopped != TSUCHI_OK) return stopped;
    return voices.release(mod, voice, 0);
}
TSUCHI_Result content_sound_unload(TSUCHI_Mod* mod, uint64_t bank) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    if (banks.holder(bank, 0) && banks.holder(bank, 0) != mod) return TSUCHI_DENIED;
    BankKept* kept = banks.find(mod, bank, 0);
    if (!kept || kept->retired) return TSUCHI_NOT_FOUND;
    if (!ready()) return TSUCHI_NOT_READY;
    kept = banks.find(mod, bank, 0);
    if (!kept || kept->retired) return TSUCHI_NOT_READY;
    if (kept->data->calls) return TSUCHI_BUSY;
    // Failed declarations hold nothing in the sound engine, so only banks in use count, as they do for a load.
    if (banks_in_use() > bank_limit || voices.count() > voice_limit) return TSUCHI_CAPACITY;
    uint32_t retiring_voices = 0;
    for (uint32_t i = 0; i < voices.count(); ++i)
        if (voices.kept_at(i).bank == bank && !voices.kept_at(i).retired) ++retiring_voices;
    // Reserve the complete cleanup before ending a handle or stopping a voice.
    // Native refusal after this point belongs to the framework's retired holds.
    if (!banks.reserve(1) || !voices.reserve(retiring_voices)) return TSUCHI_NO_MEMORY;
    if (!mod_usable(mod)) return TSUCHI_NOT_READY;
    kept = banks.find(mod, bank, 0);
    if (!kept || kept->retired) return TSUCHI_NOT_READY;
    if (kept->data->calls) return TSUCHI_BUSY;
    return banks.release(mod, bank, 0);
}

} // namespace tsuchi
