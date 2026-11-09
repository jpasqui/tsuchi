#include "mods.h"
#include "mod_choices.h"
#include "files.h"
#include "file_service.h"
#include "mod_service.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
using namespace tsuchi;
namespace tsuchi {
const char* result_name(TSUCHI_Result){return "test refusal";}
namespace mod {void set_backend(const Backend*){} void emit_restarted(const TSUCHI_ModRestarted&){} }
namespace file {void set_backend(const Backend*){} }
}
namespace {
std::map<std::string,std::string> disk;
std::string manifest(const char* id,const char* extra="") {return std::string("{\"id\":\"")+id+"\",\"name\":\"Mod\",\"version\":\"1.0.0\""+extra+"}";}
TSUCHI_Result installed(Texts& out){return out.add("tsuchisd:/tsuchi/downloads/abc/mods/author.mod")?TSUCHI_OK:TSUCHI_NO_MEMORY;}
void add(const char* id,const char* extra=""){disk[std::string("stage/")+id+"/mod.json"]=manifest(id,extra);}
}
namespace tsuchi::game {
bool files_known(){return true;}
bool path_stays_within(const char* p){return *p&&*p!='/'&&!strstr(p,"..")&&!strchr(p,':');}
TSUCHI_Result file_size(const char* p,uint64_t& n){auto it=disk.find(p);if(it==disk.end())return TSUCHI_NOT_FOUND;n=it->second.size();return TSUCHI_OK;}
TSUCHI_Result read_whole_file(const char* p,char*& out,uint64_t& n){if(file_size(p,n)!=TSUCHI_OK)return TSUCHI_NOT_FOUND;out=static_cast<char*>(allocate(size_t(n)));if(!out)return TSUCHI_NO_MEMORY;memcpy(out,disk[p].data(),size_t(n));return TSUCHI_OK;}
TSUCHI_Result read_file_part(const char* p,uint64_t off,void* out,uint64_t room,uint64_t& n){uint64_t size=0;if(file_size(p,size)!=TSUCHI_OK)return TSUCHI_NOT_FOUND;n=off>=size?0:std::min(room,size-off);if(n)memcpy(out,disk[p].data()+off,size_t(n));return TSUCHI_OK;}
TSUCHI_Result list_folder(const char* p,Texts& files,Texts& folders){std::string prefix=std::string(p)+"/";std::set<std::string> dirs;bool any=false;for(const auto& f:disk)if(f.first.rfind(prefix,0)==0){any=true;auto rest=f.first.substr(prefix.size());auto slash=rest.find('/');if(slash==std::string::npos){if(!files.add(rest.c_str()))return TSUCHI_NO_MEMORY;}else dirs.insert(rest.substr(0,slash));}for(const auto& d:dirs)if(!folders.add(d.c_str()))return TSUCHI_NO_MEMORY;return any?TSUCHI_OK:TSUCHI_NOT_FOUND;}
}
int main(){
    set_allocator({std::malloc,std::free});Texts incoming,existing;Text problem;
    incoming.add("stage/author.mod");add("author.mod");
    assert(validate_mod_installation(incoming,existing,problem)==TSUCHI_OK);
    add("author.mod",",\"requires\":{\"author.lib\":\"1.0.0\"}");
    assert(validate_mod_installation(incoming,existing,problem)==TSUCHI_NOT_FOUND&&!strcmp(problem.get(),"author.lib"));
    existing.add("stage/author.lib");add("author.lib");
    assert(validate_mod_installation(incoming,existing,problem)==TSUCHI_OK);
    existing.add("stage/author.mod");assert(validate_mod_installation(incoming,existing,problem)==TSUCHI_INVALID);existing.clear();
    incoming.add("stage/author.lib");add("author.lib",",\"requires\":{\"author.mod\":\"*\"}");
    assert(validate_mod_installation(incoming,existing,problem)==TSUCHI_INVALID);
    add("author.lib",",\"conflicts\":[\"author.mod\"]");assert(validate_mod_installation(incoming,existing,problem)==TSUCHI_INVALID);
    add("author.lib",",\"game\":\"99.0.0\"");assert(validate_mod_installation(incoming,existing,problem)==TSUCHI_INVALID);
    add("author.lib",",\"framework\":4294967295");assert(validate_mod_installation(incoming,existing,problem)==TSUCHI_INVALID);
    // Profiles are not part of this runtime's manifest contract. Refuse them
    // rather than installing a package whose requested settings would be ignored.
    add("author.lib");
    add("author.mod",",\"settings_overrides\":{\"author.lib\":{\"amount\":4}}");
    assert(validate_mod_installation(incoming,existing,problem)==TSUCHI_INVALID);
    incoming.clear();existing.clear();problem.clear();disk.clear();
    disk["tsuchisd:/tsuchi/downloads/abc/mods/author.mod/mod.json"]=manifest("author.mod");
    disk["tsuchisd:/tsuchi/downloads/abc/mods/author.mod/main.lua"]="downloaded";
    set_installed_folders(installed);assert(find_mods()==TSUCHI_OK);auto* record=find_mod("author.mod");assert(record&&record->manifest_valid&&record->folder[0]);
    Texts path;assert(mod_folder_path(path,"author.mod","mod.nro")&&!strcmp(path.data,"tsuchisd:/tsuchi/downloads/abc/mods/author.mod/mod.nro"));path.clear();
    char* bytes=nullptr;uint64_t count=0;assert(read_mod_file("author.mod","main.lua",bytes,count)==TSUCHI_OK&&std::string(bytes,size_t(count))=="downloaded");release(bytes);
    // Only the exact owned predecessor folder may be replaced; a matching ID
    // alone must not authorize replacement of a manually installed mod.
    add("author.mod");incoming.add("stage/author.mod");Texts replaced;
    assert(validate_mod_installation(incoming,existing,problem)==TSUCHI_INVALID);
    replaced.add("wrong/author.mod");assert(validate_mod_installation(incoming,existing,problem,&replaced)==TSUCHI_INVALID);replaced.clear();
    replaced.add(record->folder);assert(validate_mod_installation(incoming,existing,problem,&replaced)==TSUCHI_OK);
    replaced.clear();incoming.clear();problem.clear();
    disk["rom:/tsuchi/mods/author.mod/mod.json"]=manifest("author.mod");
    assert(find_mods()==TSUCHI_OK&&mod_count()==2);for(uint32_t i=0;i<mod_count();++i)assert(mod_at(i)->state==ModState::blocked);
    forget_mods();set_installed_folders(nullptr);return 0;
}
