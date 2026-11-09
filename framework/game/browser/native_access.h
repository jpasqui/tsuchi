#pragma once
#include "game.h"
#include "objects.h"
#include "names.h"
#include "registry.h"
#include <stdint.h>
namespace tsuchi::browser::native {
namespace t=game::table;
using u8=uint8_t; using u32=uint32_t; using s32=int32_t; using u64=uint64_t;
struct GameString {char16_t* data; s32 size,capacity;};
struct GameText {u64 words[3];};
#define kObjectCount t::data::object_count
#define kObjectChunks t::data::object_chunks
#define kMaxObjects s32(t::constant::max_objects)
#define kUnreachableFlags t::constant::unreachable_flags
template<class T> T& at(void* p,u64 offset){return *reinterpret_cast<T*>(static_cast<u8*>(p)+offset);}
template<class T> T global(u64 offset){return game::global<T>(offset);}
template<class T> T fn(u64 offset){return reinterpret_cast<T>(game::main_base()+offset);}
template<class T> T method(void* p,u64 slot){return *reinterpret_cast<T*>(at<u64>(p,0)+slot);}
inline void* object_entry(s32 index){
 const s32 count=global<s32>(kObjectCount);if(index<0||index>=count||count>kMaxObjects)return nullptr;
 const u64 chunks=global<u64>(kObjectChunks),chunk=game::read_or<u64>(chunks+8*(index>>16));
 const u64 item=chunk+uint64_t(index&65535)*t::object_item::size;
 return chunk&&game::readable(item,t::object_item::size)&&!(game::read_or<u32>(item+t::object_item::flags)&kUnreachableFlags)?reinterpret_cast<void*>(item):nullptr;
}
inline void* object_entry_for(void* p){if(!game::live(reinterpret_cast<u64>(p)))return nullptr;return object_entry(at<s32>(p,t::browser_layout::object_index));}
inline u32 object_serial(void* item){return at<u32>(item,t::object_item::serial);}
inline bool named(void* p,const char* name){return game::live(reinterpret_cast<u64>(p))&&game::name_is(game::name_of(reinterpret_cast<u64>(p)),name);}
}
