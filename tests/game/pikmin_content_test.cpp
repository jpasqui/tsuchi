// Manifest admission through the production Pikmin content code and ID registry.
// Engine containers and tables below are stand-ins. This proves no game behavior.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "../../framework/game/pikmin_content.cpp"
#include "services.h"

namespace fixture {
namespace t = tsuchi::game::table;
int failures = 0, unexpected_callbacks = 0, enum_calls = 0, repairs = 0, row_copies = 0;
#define EXPECT(x) do { if (!(x)) { std::printf("%d: %s\n", __LINE__, #x); ++fixture::failures; } } while (0)

// These controlled native callbacks exercise the production wrappers, not game behavior.
alignas(16) unsigned char finish_actors[2][131072]{};
alignas(16) unsigned char finish_parts[2][1024]{};
uint32_t finish_generation[2], part_generation[2];
bool finish_alive[2], finish_handles[2], finish_flags_readable[2];
uint64_t finish_thread=10;
uint64_t thread_now() { return finish_thread; }
enum class FinishMode { live, dead, reuse, nested, dying, flags_unreadable, part_reuse, part_replace, foreign, unknown, post_foreign, post_unknown, reused_nested, part_late };
FinishMode finish_mode[2];
int finish_calls[2], component_calls[2], memory_reads=0;
uint8_t finish_entry_color[2], component_entry_color[2];
int finish_index(uint64_t object) {
    for (int i=0;i<2;++i) if(object==reinterpret_cast<uint64_t>(finish_actors[i])) return i;
    return -1;
}
int part_index(uint64_t object) {
    for (int i=0;i<2;++i) if(object==reinterpret_cast<uint64_t>(finish_parts[i])) return i;
    return -1;
}
// The game instance's pointer and the story objects it can lead to.
alignas(16) uint64_t instance_global = 0;
alignas(16) unsigned char instance_block[0x400]{};
alignas(16) unsigned char stories[2][64]{};
int repair_result = TSUCHI_OK;
bool starting = false;
int hooks_placed = 0, hud_start_result = TSUCHI_OK, birth_start_result = TSUCHI_OK;
bool hud_on = false, counts_on = false;
void set_story(int i) {
    instance_global = i < 0 ? 0 : reinterpret_cast<uint64_t>(instance_block);
    const uint64_t story = i < 0 ? 0 : reinterpret_cast<uint64_t>(stories[i]);
    memcpy(instance_block + t::game_instance::story_save, &story, 8);
}
bool inside(uint64_t address, uint64_t bytes, const void* block, uint64_t size) {
    const uint64_t begin = reinterpret_cast<uint64_t>(block);
    return address >= begin && address - begin <= size && bytes <= size - (address - begin);
}
bool finish_mapped(uint64_t address,uint64_t bytes) {
    ++memory_reads;
    if (inside(address, bytes, &instance_global, sizeof instance_global) || inside(address, bytes, instance_block, sizeof instance_block)) return true;
    for(int i=0;i<2;++i) {
        uint64_t begin=reinterpret_cast<uint64_t>(finish_actors[i]);
        if(address==begin+t::engine_actor::play_state && bytes==1 && !finish_flags_readable[i]) return false;
        if(address>=begin && address-begin<=sizeof finish_actors[i] && bytes<=sizeof finish_actors[i]-(address-begin)) return true;
        begin=reinterpret_cast<uint64_t>(finish_parts[i]);
        if(address>=begin && address-begin<=sizeof finish_parts[i] && bytes<=sizeof finish_parts[i]-(address-begin)) return true;
    }
    return false;
}
void finish_request(uint8_t color,int i) {
    tsuchi::PikminBirthContexts::Scope scope(tsuchi::birth_contexts,{tsuchi::known_game_thread_token(),0,0,color});
    tsuchi::finish(finish_actors[i],nullptr,false,nullptr);
}
void component_original(void* object) {
    int i=part_index(reinterpret_cast<uint64_t>(object)); EXPECT(i>=0); if(i<0)return;
    ++component_calls[i]; component_entry_color[i]=finish_actors[i][t::game_actor::color];
}
void finish_original(void* object,void*,bool,void*) {
    int i=finish_index(reinterpret_cast<uint64_t>(object)); EXPECT(i>=0); if(i<0)return;
    ++finish_calls[i]; finish_entry_color[i]=finish_actors[i][t::game_actor::color];
    finish_actors[i][t::game_actor::color]=165;
    if(finish_mode[i]==FinishMode::dead) finish_alive[i]=false;
    if(finish_mode[i]==FinishMode::reuse || finish_mode[i]==FinishMode::reused_nested) ++finish_generation[i];
    if(finish_mode[i]==FinishMode::dying) finish_actors[i][t::engine_actor::play_state]|=t::constant::actor_destroying_mask;
    if(finish_mode[i]==FinishMode::flags_unreadable) finish_flags_readable[i]=false;
    if(finish_mode[i]==FinishMode::part_reuse) ++part_generation[i];
    if(finish_mode[i]==FinishMode::part_late) { // the game gives the actor its Pikmin component while the spawn finishes
        uint64_t own=reinterpret_cast<uint64_t>(finish_parts[i]);
        memcpy(finish_actors[i]+t::game_actor::pikmin,&own,8);
    }
    if(finish_mode[i]==FinishMode::part_replace) {
        uint64_t other=reinterpret_cast<uint64_t>(finish_parts[1-i]);
        memcpy(finish_actors[i]+t::game_actor::pikmin,&other,8);
    }
    if(finish_mode[i]==FinishMode::nested) {
        finish_request(21,1);
        EXPECT(tsuchi::birth_contexts.snapshot(10).owner==tsuchi::game::to_handle(reinterpret_cast<uint64_t>(object)));
        { tsuchi::PikminBirthContexts::Scope stock(tsuchi::birth_contexts,{10,0,0,0});
          EXPECT(tsuchi::birth_contexts.snapshot(10).color==0); }
        EXPECT(tsuchi::birth_contexts.snapshot(10).color==19);
    }
    if(finish_mode[i]==FinishMode::reused_nested) {
        tsuchi::finish(finish_actors[1],nullptr,false,nullptr);
        EXPECT(finish_entry_color[1]==0 && component_entry_color[1]==165);
    }
    if(finish_mode[i]==FinishMode::foreign) finish_thread=20;
    if(finish_mode[i]==FinishMode::unknown) tsuchi::set_thread_probe(nullptr);
    int reads=memory_reads;
    tsuchi::begin_component(finish_parts[i]);
    if(finish_mode[i]==FinishMode::foreign || finish_mode[i]==FinishMode::unknown) EXPECT(memory_reads==reads);
    finish_thread=10; tsuchi::set_thread_probe(thread_now); tsuchi::note_game_thread();
    if(finish_mode[i]==FinishMode::post_foreign || finish_mode[i]==FinishMode::post_unknown)
        finish_actors[i][t::game_actor::color]=165;
    if(finish_mode[i]==FinishMode::post_foreign) finish_thread=20;
    if(finish_mode[i]==FinishMode::post_unknown) tsuchi::set_thread_probe(nullptr);
}
void reset_finish() {
    for(int i=0;i<2;++i) {
        memset(finish_actors[i],0,sizeof finish_actors[i]); memset(finish_parts[i],0,sizeof finish_parts[i]);
        uint64_t part=reinterpret_cast<uint64_t>(finish_parts[i]), actor=reinterpret_cast<uint64_t>(finish_actors[i]);
        memcpy(finish_actors[i]+t::game_actor::pikmin,&part,8);
        memcpy(finish_parts[i]+t::pikmin_native::owner,&actor,8);
        finish_generation[i]=part_generation[i]=1;
        finish_alive[i]=finish_handles[i]=finish_flags_readable[i]=true;
        finish_calls[i]=component_calls[i]=0; finish_entry_color[i]=component_entry_color[i]=0;
        finish_mode[i]=FinishMode::live;
    }
    finish_thread=10; tsuchi::set_thread_probe(thread_now); tsuchi::note_game_thread();
    tsuchi::finish_before=finish_original; tsuchi::component_before=component_original;
    EXPECT(!tsuchi::birth_contexts.snapshot(10).present);
}
int eject_calls=0;
void eject_original(void*,void*,int32_t,int32_t,int32_t,uint64_t,bool,int32_t) {
    ++eject_calls; tsuchi::finish(finish_actors[0],nullptr,false,nullptr);
}
void overflow(unsigned left) {
    tsuchi::PikminBirthContexts::Scope scope(tsuchi::birth_contexts,{10,0,0,19});
    if(left) overflow(left-1);
    else {
        EXPECT(!scope.allowed(10) && tsuchi::birth_contexts.snapshot(10).blocked);
        tsuchi::finish(finish_actors[0],nullptr,false,nullptr);
        EXPECT(finish_entry_color[0]==0 && component_entry_color[0]==165);
    }
    EXPECT(!scope.allowed(10));
    EXPECT(tsuchi::birth_contexts.snapshot(10).blocked);
}
void finish_lifetimes() {
    reset_finish(); finish_request(19,0);
    EXPECT(finish_calls[0]==1 && component_calls[0]==1 && component_entry_color[0]==19 && finish_actors[0][t::game_actor::color]==19);
    EXPECT(!tsuchi::birth_contexts.snapshot(10).present);
    for(auto mode:{FinishMode::dead,FinishMode::reuse,FinishMode::dying,FinishMode::flags_unreadable,FinishMode::part_reuse,FinishMode::part_replace}) {
        reset_finish(); finish_mode[0]=mode;
        uint64_t identity=tsuchi::game::to_handle(reinterpret_cast<uint64_t>(finish_actors[0]));
        finish_request(19,0);
        EXPECT(finish_calls[0]==1 && component_calls[0]==1 && component_entry_color[0]==165);
        EXPECT(finish_actors[0][t::game_actor::color]==165 && !tsuchi::birth_contexts.snapshot(10).present);
        if(mode==FinishMode::dying) EXPECT(tsuchi::game::from_handle(identity)==reinterpret_cast<uint64_t>(finish_actors[0]));
    }
    // A new Pikmin from the game's own spawn has no component when its finish begins. Its actor
    // still takes the added color, and its component sees that color when it begins play.
    reset_finish(); finish_mode[0]=FinishMode::part_late; memset(finish_actors[0]+t::game_actor::pikmin,0,8);
    finish_request(19,0);
    EXPECT(finish_calls[0]==1 && finish_entry_color[0]==19 && component_calls[0]==1 && component_entry_color[0]==19);
    EXPECT(finish_actors[0][t::game_actor::color]==19 && !tsuchi::birth_contexts.snapshot(10).present);
    reset_finish(); finish_mode[0]=FinishMode::reused_nested; finish_request(19,0);
    EXPECT(finish_calls[0]==1 && finish_calls[1]==1 && finish_actors[0][t::game_actor::color]==165);
    for(auto mode:{FinishMode::post_foreign,FinishMode::post_unknown}) {
        reset_finish(); finish_mode[0]=mode; finish_request(19,0);
        // Its component was admitted before the witness was lost; poison again
        // to distinguish a post-return write from that admitted pre-return write.
        EXPECT(finish_calls[0]==1 && component_calls[0]==1);
        EXPECT(tsuchi::known_game_thread_token()==0 && finish_actors[0][t::game_actor::color]==165);
    }
    reset_finish(); finish_mode[0]=FinishMode::nested; finish_mode[1]=FinishMode::reuse;
    finish_request(19,0);
    EXPECT(finish_calls[0]==1 && finish_calls[1]==1 && component_calls[0]==1 && component_calls[1]==1);
    EXPECT(component_entry_color[0]==19 && component_entry_color[1]==165);
    EXPECT(finish_actors[0][t::game_actor::color]==19 && finish_actors[1][t::game_actor::color]==165);
    reset_finish(); finish_mode[0]=FinishMode::nested; finish_request(19,0);
    EXPECT(component_entry_color[0]==19 && component_entry_color[1]==21);
    for(auto mode:{FinishMode::foreign,FinishMode::unknown}) {
        reset_finish(); finish_mode[0]=mode; finish_request(19,0);
        EXPECT(component_calls[0]==1 && component_entry_color[0]==165 && finish_calls[0]==1);
    }
    for(int choice=0;choice<2;++choice) {
        reset_finish(); tsuchi::PikminBirthContexts::Scope outer(tsuchi::birth_contexts,{10,0,0,19});
        if(choice) finish_thread=20; else tsuchi::set_thread_probe(nullptr);
        int reads=memory_reads;
        tsuchi::finish(finish_actors[0],nullptr,false,nullptr);
        EXPECT(memory_reads==reads && finish_calls[0]==1 && finish_entry_color[0]==0 && component_entry_color[0]==165);
    }
    reset_finish(); finish_actors[0][t::engine_actor::play_state]=t::constant::actor_destroying_mask;
    finish_request(19,0); EXPECT(finish_entry_color[0]==0 && component_entry_color[0]==165);
    reset_finish(); finish_handles[0]=false; finish_request(19,0); EXPECT(finish_actors[0][t::game_actor::color]==165);
    reset_finish(); finish_flags_readable[0]=false; finish_request(19,0); EXPECT(finish_entry_color[0]==0 && component_entry_color[0]==165);
    reset_finish(); finish_actors[0][t::engine_actor::play_state]=0x50; finish_request(19,0); EXPECT(component_entry_color[0]==19);
    reset_finish(); finish_request(0,0); EXPECT(finish_entry_color[0]==0 && finish_actors[0][t::game_actor::color]==165);
    reset_finish(); overflow(tsuchi::PikminBirthContexts::capacity);
    EXPECT(!tsuchi::birth_contexts.snapshot(10).present && finish_calls[0]==1 && component_calls[0]==1);
    finish_request(19,1); EXPECT(component_entry_color[1]==19);
    // Native-safe stock input forwards once on known, foreign and unknown paths.
    tsuchi::eject_before=eject_original;
    for(int choice=0;choice<3;++choice) {
        reset_finish(); eject_calls=0;
        if(choice==1) finish_thread=20;
        if(choice==2) tsuchi::set_thread_probe(nullptr);
        tsuchi::eject(nullptr,nullptr,0,0,1,0,false,0);
        EXPECT(eject_calls==1 && finish_calls[0]==1 && finish_entry_color[0]==0);
    }
    reset_finish(); eject_calls=0;
    tsuchi::eject(nullptr,nullptr,t::constant::pikmin_color_first,0,1,0,false,0);
    EXPECT(eject_calls==0); // Existing custom-ready refusal remains explicit.
    // Interleaved witnesses select and remove only their own synchronous frame.
    tsuchi::PikminBirthContexts contexts;
    { tsuchi::PikminBirthContexts::Scope a(contexts,{10,1,2,19});
      { tsuchi::PikminBirthContexts::Scope b(contexts,{20,3,4,21});
        EXPECT(contexts.snapshot(10).owner==1 && contexts.snapshot(20).owner==3);
        EXPECT(!a.allowed(20) && !contexts.snapshot(0).present); }
      EXPECT(contexts.snapshot(10).color==19 && !contexts.snapshot(20).present); }
    EXPECT(!contexts.snapshot(10).present);
}

constexpr uint64_t framework = 100, properties = 200, color_enum = 300;
std::map<uint8_t, uint64_t> classes{{0, 1000}, {1, 1001}};
std::map<std::string, std::map<std::string, int64_t>> rows;
std::map<std::string, uint32_t> names;
std::map<std::string, std::string> saved;
bool refuse_write = false;
const tsuchi::Kind parameter_kind{"fixture.parameter"};
struct Change { std::string row, field; int64_t before; };
std::vector<Change> changes;
const tsuchi::Kind icon_kind{"fixture.pikmin.icon"};
std::map<uint8_t, uint64_t> icons;
void restore_icon(void*, uint64_t color) { icons.erase(uint8_t(color)); }
void restore(void*, uint64_t key) {
    const Change& c = changes.at(size_t(key));
    rows[c.row][c.field] = c.before;
}
TSUCHI_Result asset_load(TSUCHI_Mod* mod, const char* path, TSUCHI_Object* out) {
    if (!tsuchi::mod_usable(mod)) return TSUCHI_INVALID;
    if (!strcmp(path, t::asset::pikmin_color_enum)) out->id = color_enum;
    else if (!strcmp(path, t::asset::pikmin_property_table)) out->id = properties;
    else if (!strcmp(path, "/Game/Mods/Icons.T_Amber")) out->id = 3000;
    else if (!strcmp(path, "/Game/Mods/Icons.Bad")) out->id = 3001;
    else return TSUCHI_NOT_FOUND;
    return TSUCHI_OK;
}
TSUCHI_Result table_get(TSUCHI_Mod* mod, TSUCHI_Object object, const char* row, const char* field, TSUCHI_Value* out) {
    if (!tsuchi::mod_usable(mod) || object.id != properties) return TSUCHI_INVALID;
    auto found = rows.find(row);
    if (found == rows.end() || !found->second.count(field)) return TSUCHI_NOT_FOUND;
    out->kind = TSUCHI_VALUE_INTEGER; out->as.integer = found->second.at(field);
    return TSUCHI_OK;
}
TSUCHI_Result table_override(TSUCHI_Mod* mod, TSUCHI_Object object, const char* row, const char* field, const TSUCHI_Value* value) {
    if (!tsuchi::mod_usable(mod) || object.id != properties || value->kind != TSUCHI_VALUE_INTEGER) return TSUCHI_INVALID;
    auto found = rows.find(row);
    if (found == rows.end() || !found->second.count(field)) return TSUCHI_NOT_FOUND;
    const uint64_t key = changes.size();
    changes.push_back(Change{row, field, found->second.at(field)});
    const TSUCHI_Result result = tsuchi::own(mod, parameter_kind, key, restore, nullptr);
    if (result != TSUCHI_OK) { changes.pop_back(); return result; }
    found->second[field] = value->as.integer;
    return TSUCHI_OK;
}
TSUCHI_Result table_text(TSUCHI_Mod*, TSUCHI_Object, const char*, const char*, const char*) {
    ++unexpected_callbacks; return TSUCHI_UNSUPPORTED;
}
TSUCHI_Result add_row(TSUCHI_Mod* mod, TSUCHI_Object object, const char* row, const char* from) {
    if (!tsuchi::mod_usable(mod) || object.id != properties) return TSUCHI_INVALID;
    auto found = rows.find(from);
    if (found == rows.end()) return TSUCHI_NOT_FOUND;
    if (rows.count(row)) return TSUCHI_INVALID;
    rows[row] = found->second; ++row_copies;
    return TSUCHI_OK;
}
void* find_row(void* object, uint64_t packed) {
    if (reinterpret_cast<uint64_t>(object) != properties) return nullptr;
    tsuchi::game::Name name{}; memcpy(&name, &packed, sizeof name);
    for (const auto& n : names)
        if (n.second == name.index && rows.count(n.first)) return reinterpret_cast<void*>(properties);
    return nullptr;
}
const TSUCHI_AssetService asset_table = [] {
    TSUCHI_AssetService out{}; out.table_size = sizeof out; out.load = asset_load; return out;
}();
const TSUCHI_DatatableService table = [] {
    TSUCHI_DatatableService out{}; out.table_size = sizeof out;
    out.get = table_get; out.override = table_override; out.override_text = table_text; out.add_row = add_row; return out;
}();
uint32_t lookup(const char* id, TSUCHI_Result wanted = TSUCHI_OK) {
    uint32_t value = 0xDEADBEEF;
    EXPECT(tsuchi::id_value(tsuchi::pikmin::PikminType_vocabulary, id, &value) == wanted);
    return value;
}
void stock_unchanged() {
    const char* const stock[] = {"red", "blue", "yellow", "rock", "wing", "purple", "white", "ice"};
    for (uint32_t i = 0; i < 8; ++i) EXPECT(lookup(stock[i]) == i);
    EXPECT(lookup("glow") == 9);
    EXPECT(rows.at(t::row::pikmin_properties[0]).at("PikminColor") == 0);
    EXPECT(rows.at(t::row::pikmin_properties[1]).at("PikminColor") == 1);
    EXPECT(rows.at(t::row::pikmin_properties[0]).at("LeafSpeed") == 100);
    EXPECT(rows.at(t::row::pikmin_properties[1]).at("LeafSpeed") == 101);
    EXPECT(classes.at(0) == 1000 && classes.at(1) == 1001);
}
// The game's own functions behind each hook, counted.
int originals = 0;
void* enum_object() { return reinterpret_cast<void*>(color_enum); }
void count_one(void*) { ++originals; }
void count_reset(void*, uint32_t) { ++originals; }
void count_none() { ++originals; }
void count_snapshot(uint64_t, uint32_t) { ++originals; }
void count_archive(void*, void*) { ++originals; }
void count_eject(void*, void*, int32_t, int32_t, int32_t, uint64_t, bool, int32_t) { ++originals; }
void count_finish(void*, void*, bool, void*) { ++originals; }
void hook_originals() {
    tsuchi::construct_before = count_one; tsuchi::reset_before = count_reset;
    tsuchi::follow_clear_before = count_none; tsuchi::follow_snapshot_before = count_snapshot;
    tsuchi::summary_reset_before = count_one; tsuchi::result_reset_before = count_one;
    tsuchi::serialize_before = count_archive; tsuchi::structured_before = count_archive;
    tsuchi::eject_before = count_eject; tsuchi::finish_before = count_finish; tsuchi::component_before = count_one;
}
// Every hook with no color in the record: each calls the game's function once,
// and nothing repairs the story, extends the enum, or touches the HUD.
void call_hooks() {
    uint64_t initializer = 0;
    tsuchi::construct(&initializer);
    tsuchi::reset(nullptr, 0);
    tsuchi::clear_follow();
    tsuchi::snapshot_follow(1, 0);
    tsuchi::reset_summary(nullptr);
    tsuchi::reset_result(nullptr);
    tsuchi::serialize(nullptr, nullptr);
    tsuchi::structured(nullptr, nullptr);
    tsuchi::eject(nullptr, nullptr, 0, 0, 1, 0, false, 0);
    tsuchi::finish(finish_actors[0], nullptr, false, nullptr);
    tsuchi::begin_component(finish_parts[0]);
    tsuchi::step();
}
void stock_hooks() {
    hook_originals();
    originals = 0;
    const int repairs_before = repairs, enums_before = enum_calls, unexpected_before = unexpected_callbacks;
    call_hooks();
    EXPECT(originals == 11);
    EXPECT(repairs == repairs_before && enum_calls == enums_before && unexpected_callbacks == unexpected_before);
    EXPECT(tsuchi::given_count == 0 && saved.empty() && tsuchi::record.writes == 0);
}
const tsuchi::Json* manifest(const char* source) {
    const tsuchi::Json* root = tsuchi::read_json(source, strlen(source));
    EXPECT(root && root->find("content"));
    return root;
}
}

namespace tsuchi {
const TSUCHI_Host host = [] {
    TSUCHI_Host out{}; out.api_version = TSUCHI_API_VERSION; out.table_size = sizeof out;
    out.asset = &fixture::asset_table; out.datatable = &fixture::table; return out;
}();
TSUCHI_Result read_save_file(const char* path, char*& data, uint64_t& bytes) {
    auto found = fixture::saved.find(path);
    if (found == fixture::saved.end()) return TSUCHI_NOT_FOUND;
    bytes = found->second.size();
    data = static_cast<char*>(allocate(size_t(bytes) + 1));
    if (!data) return TSUCHI_NO_MEMORY;
    memcpy(data, found->second.c_str(), size_t(bytes) + 1); return TSUCHI_OK;
}
TSUCHI_Result write_save_file(const char* path, const void* data, uint64_t bytes) {
    if (fixture::refuse_write) return TSUCHI_NOT_READY;
    fixture::saved[path] = std::string(static_cast<const char*>(data), size_t(bytes)); return TSUCHI_OK;
}
TSUCHI_Result native_pikmin_enum(uint64_t object) {
    EXPECT(object == fixture::color_enum); ++fixture::enum_calls; return TSUCHI_OK;
}
TSUCHI_Result native_map_get(uint64_t object, const char* path, const void* key, uint32_t kb, void* value, uint32_t vb) {
    EXPECT(object == fixture::framework && !strcmp(path, "PikminClass") && kb == 1 && vb == 8);
    auto found = fixture::classes.find(*static_cast<const uint8_t*>(key));
    if (found == fixture::classes.end()) return TSUCHI_NOT_FOUND;
    memcpy(value, &found->second, 8); return TSUCHI_OK;
}
TSUCHI_Result native_map_put(uint64_t object, const char* path, const void* key, uint32_t kb, const void* value, uint32_t vb) {
    EXPECT(object == fixture::framework && !strcmp(path, "PikminClass") && kb == 1 && vb == 8);
    uint64_t cls = 0; memcpy(&cls, value, 8);
    fixture::classes[*static_cast<const uint8_t*>(key)] = cls; return TSUCHI_OK;
}
bool story_object(uint64_t object) {
    return object == reinterpret_cast<uint64_t>(fixture::stories[0]) || object == reinterpret_cast<uint64_t>(fixture::stories[1]);
}
TSUCHI_Result pikmin_save_story(uint64_t object, bool) {
    EXPECT(object == 0 || story_object(object)); ++fixture::repairs; return TSUCHI_Result(fixture::repair_result);
}
// Unexercised gameplay paths fail if the admission test accidentally reaches them.
TSUCHI_Result pikmin_save_summary(uint64_t, uint64_t, bool) { ++fixture::unexpected_callbacks; return TSUCHI_UNSUPPORTED; }
TSUCHI_Result pikmin_save_result(uint64_t, uint64_t, bool) { ++fixture::unexpected_callbacks; return TSUCHI_UNSUPPORTED; }
TSUCHI_Result pikmin_save_clear_follow(uint64_t) { ++fixture::unexpected_callbacks; return TSUCHI_UNSUPPORTED; }
TSUCHI_Result pikmin_snapshot_follow(uint64_t, uint64_t, uint32_t) { ++fixture::unexpected_callbacks; return TSUCHI_UNSUPPORTED; }
// Start-up is exercised only while `starting` is set: every hook goes in.
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t, uint64_t, void*, uint32_t, uint64_t* id, void** original) {
    if (!fixture::starting) { ++fixture::unexpected_callbacks; return TSUCHI_UNSUPPORTED; }
    *id = ++fixture::hooks_placed; *original = nullptr; return TSUCHI_OK;
}
TSUCHI_Result hook_code_site(TSUCHI_Mod*, uint32_t, void*, uint64_t* id, void** original) {
    if (!fixture::starting) { ++fixture::unexpected_callbacks; return TSUCHI_UNSUPPORTED; }
    *id = ++fixture::hooks_placed; *original = nullptr; return TSUCHI_OK;
}
bool each_frame(void (*)()) { if (!fixture::starting) ++fixture::unexpected_callbacks; return fixture::starting; }
}
namespace tsuchi::game {
uint64_t main_base() { return reinterpret_cast<uint64_t>(&fixture::instance_global) - t::data::game_instance; }
bool readable(uint64_t address, uint64_t bytes) { return fixture::finish_mapped(address, bytes); }
bool writable(uint64_t address, uint64_t bytes) { return fixture::finish_mapped(address, bytes); }
uint64_t game_framework() { return fixture::framework; }
uint64_t to_handle(uint64_t object) {
    int i=fixture::finish_index(object);
    if(i>=0) return fixture::finish_alive[i] && fixture::finish_handles[i] ?
        (uint64_t(0xB7)<<56)|(uint64_t(i+1)<<32)|fixture::finish_generation[i] : 0;
    i=fixture::part_index(object);
    if(i>=0) return (uint64_t(0xB8)<<56)|(uint64_t(i+1)<<32)|fixture::part_generation[i];
    return object;
}
uint64_t from_handle(uint64_t handle) {
    unsigned kind=unsigned(handle>>56);
    if(kind!=0xB7 && kind!=0xB8) return handle;
    uint32_t slot=uint32_t(handle>>32)&0xFFFFFF; if(slot<1||slot>2)return 0;
    int i=int(slot-1);
    if(kind==0xB8) return fixture::part_generation[i]==uint32_t(handle) ? reinterpret_cast<uint64_t>(fixture::finish_parts[i]) : 0;
    return fixture::finish_alive[i] && fixture::finish_generation[i]==uint32_t(handle) ? reinterpret_cast<uint64_t>(fixture::finish_actors[i]) : 0;
}

bool is_class(uint64_t object) { return object == 1000 || object == 1001; }
bool live(uint64_t object) {
    const int i = fixture::finish_index(object);
    return object != 0 && (i < 0 || fixture::finish_alive[i]);
}
uint64_t type_of(uint64_t object) { if (!tsuchi::story_object(object)) ++fixture::unexpected_callbacks; return 0; }
TSUCHI_Result find_place(uint64_t, uint64_t base, const char*, Place&) {
    if (!tsuchi::story_object(base)) ++fixture::unexpected_callbacks;
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result struct_at(const Place&, uint64_t&, uint64_t&) { ++fixture::unexpected_callbacks; return TSUCHI_UNSUPPORTED; }
TSUCHI_Result name_from_utf8(const char* text, Name& out) {
    uint32_t& index = fixture::names[text];
    if (!index) index = uint32_t(fixture::names.size());
    out = Name{index, 0}; return TSUCHI_OK;
}
uint64_t from_weak_reference(uint64_t) { ++fixture::unexpected_callbacks; return 0; }
uint64_t default_object(uint64_t) { ++fixture::unexpected_callbacks; return 0; }
bool is_game_actor(uint64_t object) {
    const int i = fixture::finish_index(object);
    return i >= 0 && fixture::finish_alive[i];
}
bool is_a(uint64_t object, ClassRef& ref) {
    if (!strcmp(ref.name, "Texture2D")) return object == 3000;
    ++fixture::unexpected_callbacks; return false;
}
TSUCHI_Result pikmin_hud_icon(TSUCHI_Mod* mod, uint8_t color, uint64_t texture) {
    if (fixture::icons.count(color)) return TSUCHI_BUSY;
    TSUCHI_Result r = own(mod, fixture::icon_kind, color, fixture::restore_icon, nullptr);
    if (r == TSUCHI_OK) fixture::icons[color] = texture;
    return r;
}
TSUCHI_Result pikmin_hud_icon_release(TSUCHI_Mod* mod, uint8_t color) {
    TSUCHI_Result r = disown(mod, fixture::icon_kind, color);
    if (r == TSUCHI_OK) fixture::restore_icon(nullptr, color);
    return r;
}
bool pikmin_hud_ready(uint8_t) { if (!fixture::hud_on) ++fixture::unexpected_callbacks; return fixture::hud_on; }
TSUCHI_Result pikmin_hud_register(uint8_t, uint8_t) { ++fixture::unexpected_callbacks; return TSUCHI_UNSUPPORTED; }
TSUCHI_Result pikmin_hud_start(TSUCHI_Mod*) {
    if (!fixture::starting) { ++fixture::unexpected_callbacks; return TSUCHI_UNSUPPORTED; }
    return TSUCHI_Result(fixture::hud_start_result);
}
void pikmin_hud_refresh() { if (!fixture::hud_on) ++fixture::unexpected_callbacks; }
TSUCHI_Result pikmin_birth_start(TSUCHI_Mod*) {
    if (!fixture::starting) { ++fixture::unexpected_callbacks; return TSUCHI_UNSUPPORTED; }
    return TSUCHI_Result(fixture::birth_start_result);
}
TSUCHI_Result pikmin_counts_start(TSUCHI_Mod*) {
    if (!fixture::starting) { ++fixture::unexpected_callbacks; return TSUCHI_UNSUPPORTED; }
    return TSUCHI_OK;
}
void pikmin_counts_enable(bool on) { fixture::counts_on = on; }
}

int main() {
    using namespace tsuchi;
    using namespace fixture;
    set_thread_probe(thread_now); note_game_thread();
    for (uint8_t i = 0; i < 8; ++i)
        rows[t::row::pikmin_properties[i]] = {{"PikminColor", i}, {"LeafSpeed", 100 + i}};
    t::fn::find_table_row = find_row;
    tsuchi::self = mod_create("tsuchi.pikmin");
    tsuchi::hooks_ready = TSUCHI_OK;
    tsuchi::active = true;
    EXPECT(tsuchi::self != nullptr);
    const Json* root = manifest(R"({"id":"route.new-pikmin","content":{"pikmin":{"amber":{"from":"red","icon":"/Game/Mods/Icons.T_Amber","values":{"LeafSpeed":117}},"azure":{"from":"blue","values":{"LeafSpeed":223}}}}})");
    if (!root) return 1;
    const Json& content = *root->find("content");
    Text problem;
    EXPECT(pikmin_content_valid(content, problem));
    for (const char* text : {R"({"content":{"pikmin":{"a":{"from":"red","icon":2}}}})",
                            R"({"content":{"pikmin":{"a":{"from":"red","icon":"../bad"}}}})"}) {
        const Json* bad = manifest(text);
        EXPECT(!pikmin_content_valid(*bad->find("content"), problem));
        release_json(bad); problem.clear();
    }
    TSUCHI_Mod* provider = mod_create("route.new-pikmin");
    EXPECT(provider != nullptr);
    finish_thread = 20;
    EXPECT(pikmin_content_begin(provider, content, problem) == TSUCHI_INVALID);
    EXPECT(tsuchi::given_count == 0 && icons.empty() && owned_count(provider) == 0);
    finish_thread = 10;
    EXPECT(id_add(pikmin::PikminType_vocabulary, provider, "route.new-pikmin:amber", 19) == TSUCHI_INVALID);
    EXPECT(pikmin::PikminType_vocabulary.added.count == 0 && owned_count(provider) == 0);
    t::fn::pikmin_enum_get = enum_object;
    stock_hooks();
    // A type that fails a check gives out no color: nothing is installed,
    // registered, or recorded, and both colors stay free.
    const char* const refused[] = {
        R"({"content":{"pikmin":{"amber":{"from":"red","class":"/Game/Mods/Missing.Missing_C"}}}})",
        R"({"content":{"pikmin":{"amber":{"from":"red","icon":"/Game/Mods/Missing.Missing"}}}})",
        R"({"content":{"pikmin":{"amber":{"from":"red","icon":"/Game/Mods/Icons.Bad"}}}})",
        R"({"content":{"pikmin":{"amber":{"from":"red","values":{"NoSuchField":1}}}}})",
        R"({"content":{"pikmin":{"amber":{"from":"red","values":{"LeafSpeed":true}}}}})",
        R"({"content":{"pikmin":{"amber":{"from":"red","values":{"LeafSpeed":1.5}}}}})",
        R"({"content":{"pikmin":{"amber":{"from":"red"},"azure":{"from":"blue","values":{"NoSuchField":1}}}}})",
    };
    for (const char* source : refused) {
        const Json* bad = manifest(source);
        TSUCHI_Mod* checked = mod_create("route.checked");
        problem.clear();
        EXPECT(pikmin_content_begin(checked, *bad->find("content"), problem) != TSUCHI_OK && problem.get()[0]);
        EXPECT(tsuchi::given_count == 0 && pikmin::PikminType_vocabulary.added.count == 0);
        EXPECT(saved.empty() && tsuchi::record.writes == 0 && row_copies == 0 && enum_calls == 0);
        mod_stop(checked); release_json(bad);
    }
    stock_unchanged();
    // A refused record write takes back the colors the admission gave out.
    refuse_write = true; problem.clear();
    EXPECT(pikmin_content_begin(provider, content, problem) == TSUCHI_NOT_READY);
    EXPECT(tsuchi::given_count == 0 && pikmin::PikminType_vocabulary.added.count == 0);
    EXPECT(saved.empty() && classes.at(19) == 1000 && classes.at(21) == 1001);
    refuse_write = false; problem.clear();
    EXPECT(icons.empty());
    const TSUCHI_Result admitted = pikmin_content_begin(provider, content, problem);
    EXPECT(admitted == TSUCHI_OK);
    if (admitted != TSUCHI_OK) { std::printf("Manifest admission refused: %u\n", unsigned(admitted)); return 1; }
    EXPECT(!problem.get()[0]);
    EXPECT(icons.size() == 1 && icons.at(19) == 3000);
    EXPECT(lookup("route.new-pikmin:amber") == 19 && lookup("route.new-pikmin:azure") == 21);
    char id[kIdBytes];
    id_of(pikmin::PikminType_vocabulary, 19, id); EXPECT(!strcmp(id, "route.new-pikmin:amber"));
    id_of(pikmin::PikminType_vocabulary, 21, id); EXPECT(!strcmp(id, "route.new-pikmin:azure"));
    // The refused attempt copied both rows; the admission reuses them, and
    // repairs the story once, after the record holds the colors.
    EXPECT(tsuchi::given_count == 2 && row_copies == 2 && enum_calls == 4 && repairs == 1);
    EXPECT(rows.at(t::name::pikmin_row_first).at("PikminColor") == 19);
    EXPECT(rows.at(t::name::pikmin_row_second).at("PikminColor") == 21);
    EXPECT(rows.at(t::name::pikmin_row_first).at("LeafSpeed") == 117);
    EXPECT(rows.at(t::name::pikmin_row_second).at("LeafSpeed") == 223);
    EXPECT(classes.at(19) == 1000 && classes.at(21) == 1001);
    stock_unchanged();
    const uint32_t writes = tsuchi::record.writes;
    EXPECT(writes == 1 && saved.size() == 1);
    mod_stop(provider);
    EXPECT(icons.empty());
    EXPECT(lookup("route.new-pikmin:amber", TSUCHI_NOT_FOUND) == 0xDEADBEEF);
    EXPECT(lookup("route.new-pikmin:azure", TSUCHI_NOT_FOUND) == 0xDEADBEEF);
    id_of(pikmin::PikminType_vocabulary, 19, id); EXPECT(!strcmp(id, "#19"));
    id_of(pikmin::PikminType_vocabulary, 21, id); EXPECT(!strcmp(id, "#21"));
    EXPECT(rows.at(t::name::pikmin_row_first).at("LeafSpeed") == 100);
    EXPECT(rows.at(t::name::pikmin_row_second).at("LeafSpeed") == 101);
    EXPECT(tsuchi::given_count == 2 && row_copies == 2);
    stock_unchanged();

    // Reload the real saved record before re-admitting the same qualified identities.
    forget_records();
    EXPECT(tsuchi::given_count == 0);
    provider = mod_create("route.new-pikmin");
    problem.clear();
    EXPECT(pikmin_content_begin(provider, content, problem) == TSUCHI_OK);
    EXPECT(lookup("route.new-pikmin:amber") == 19 && lookup("route.new-pikmin:azure") == 21);
    EXPECT(tsuchi::given_count == 2 && row_copies == 2 && tsuchi::record.writes == writes);
    mod_stop(provider);

    // The real registry refuses a conflicting registration after admission has begun.
    provider = mod_create("route.new-pikmin");
    EXPECT(id_add(pikmin::PikminType_vocabulary, provider, "amber", 19) == TSUCHI_OK);
    problem.clear();
    EXPECT(pikmin_content_begin(provider, content, problem) == TSUCHI_BUSY);
    EXPECT(lookup("route.new-pikmin:amber") == 19);
    EXPECT(lookup("route.new-pikmin:azure", TSUCHI_NOT_FOUND) == 0xDEADBEEF);
    mod_stop(provider);
    EXPECT(pikmin::PikminType_vocabulary.added.count == 0);
    EXPECT(rows.at(t::name::pikmin_row_first).at("LeafSpeed") == 100);
    stock_unchanged();

    const Json* third = manifest(R"({"content":{"pikmin":{"third":{"from":"red"}}}})");
    TSUCHI_Mod* other = mod_create("route.other");
    problem.clear();
    EXPECT(pikmin_content_begin(other, *third->find("content"), problem) == TSUCHI_CAPACITY);
    EXPECT(lookup("route.other:third", TSUCHI_NOT_FOUND) == 0xDEADBEEF);
    EXPECT(tsuchi::given_count == 2 && tsuchi::record.writes == writes);
    mod_stop(other);
    EXPECT(unexpected_callbacks == 0);
    // With colors in the record and their mod stopped, the story hooks still
    // repair, and the serializers extend the enum before the game reads.
    {
        hook_originals();
        originals = 0;
        const int repairs_before = repairs, enums_before = enum_calls;
        uint64_t initializer = 0;
        tsuchi::construct(&initializer);
        tsuchi::reset(nullptr, 0);
        tsuchi::serialize(nullptr, nullptr);
        tsuchi::structured(nullptr, nullptr);
        EXPECT(originals == 4 && repairs == repairs_before + 6 && enum_calls == enums_before + 2);
    }
    // A frame repairs a story object it has not seen, and one whose last repair
    // failed. Frames where nothing changed repair nothing.
    {
        hud_on = true;
        set_story(0);
        const int start = repairs;
        counts_on = false;
        tsuchi::step();
        EXPECT(repairs == start + 1 && counts_on);
        for (int frame = 0; frame < 5; ++frame) tsuchi::step();
        EXPECT(repairs == start + 1);
        set_story(1);
        tsuchi::step(); tsuchi::step();
        EXPECT(repairs == start + 2);
        set_story(0); repair_result = TSUCHI_NOT_READY;
        tsuchi::step(); tsuchi::step();
        EXPECT(repairs == start + 4);
        repair_result = TSUCHI_OK;
        tsuchi::step(); tsuchi::step();
        EXPECT(repairs == start + 5);
        // A hook whose repair fails leaves the story to the next frame.
        repair_result = TSUCHI_NOT_READY;
        uint64_t initializer = 0;
        tsuchi::construct(&initializer);
        repair_result = TSUCHI_OK;
        tsuchi::step(); tsuchi::step();
        EXPECT(repairs == start + 7);
        hud_on = false;
        set_story(-1);
    }
    // A hook set that did not all go in leaves every hook passing straight to
    // the game, even with colors in the record.
    {
        starting = true; hud_start_result = TSUCHI_NOT_READY;
        tsuchi::pikmin_content_start(tsuchi::self);
        starting = false;
        EXPECT(!tsuchi::active && hooks_placed == 11 && tsuchi::given_count == 2);
        set_story(0);
        hook_originals();
        originals = 0;
        const int repairs_before = repairs, enums_before = enum_calls, unexpected_before = unexpected_callbacks;
        call_hooks();
        EXPECT(originals == 11 && repairs == repairs_before && enum_calls == enums_before && unexpected_callbacks == unexpected_before);
        EXPECT(tsuchi::added_pikmin_native_ready(19) == TSUCHI_NOT_READY);
        // With the whole set in place the hooks act again.
        starting = true; hud_start_result = TSUCHI_OK; hooks_placed = 0;
        tsuchi::pikmin_content_start(tsuchi::self);
        starting = false;
        EXPECT(tsuchi::active && hooks_placed == 11);
        hook_originals();
        originals = 0;
        uint64_t initializer = 0;
        tsuchi::construct(&initializer);
        tsuchi::serialize(nullptr, nullptr);
        EXPECT(originals == 2 && repairs == repairs_before + 3 && enum_calls == enums_before + 1);
        set_story(-1);
    }
    problem.clear(); release_json(third); release_json(root);
    finish_lifetimes();
    mod_stop(tsuchi::self); forget_records();
    std::printf("Pikmin manifest admission: %d failures, %d unexpected callbacks\n", failures, unexpected_callbacks);
    return failures ? 1 : 0;
}
