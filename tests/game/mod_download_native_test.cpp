// The production coordinator and file callbacks, with SDK filesystem calls replaced by a memory disk.
#include "../../framework/game/mod_download.cpp"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace {
std::map<std::string,std::string> disk;
std::set<std::string> dirs;
struct Handle{std::string name;};
int depth=0;bool refuse_manifest=false,fail_receipt=false,refuse_queue=false;
tsuchi::https::HttpsJob* queued=nullptr;
TSUCHI_Result (*listing)(tsuchi::Texts&)=nullptr;
std::vector<void(*)()> steps;
const uint32_t missing=tsuchi::game::table::constant::fs_no_such_path;
uint32_t kind(void* out,void* p){assert(depth);std::string path=static_cast<char*>(p);if(!dirs.count(path)&&!disk.count(path))return missing;*static_cast<int32_t*>(out)=dirs.count(path)?0:1;return 0;}
uint32_t make_dir(void* p){assert(depth);return dirs.insert(static_cast<char*>(p)).second?0:1;}
uint32_t make_file(void* p,int64_t n){assert(depth);std::string path=static_cast<char*>(p);if(disk.count(path))return 1;disk[path]=std::string(size_t(n),0);return 0;}
uint32_t open_file(void* out,void* p,int32_t){assert(depth);std::string path=static_cast<char*>(p);if(!disk.count(path))return missing;*static_cast<void**>(out)=new Handle{path};return 0;}
void close_file(void* p){assert(depth);delete static_cast<Handle*>(p);}
uint32_t size_file(void* out,void* f){assert(depth);*static_cast<int64_t*>(out)=int64_t(disk[static_cast<Handle*>(f)->name].size());return 0;}
uint32_t resize_file(void* f,int64_t n){assert(depth);disk[static_cast<Handle*>(f)->name].resize(size_t(n));return 0;}
uint32_t write_file(void* f,int64_t offset,void* bytes,uint64_t n,void*){assert(depth);auto& h=*static_cast<Handle*>(f);if(fail_receipt&&h.name.find("/receipt")!=std::string::npos)return 1;auto& s=disk[h.name];if(uint64_t(offset)>s.size()||n>s.size()-uint64_t(offset))return 1;memcpy(s.data()+offset,bytes,size_t(n));return 0;}
uint32_t read_file(void* out,void* f,int64_t offset,void* bytes,uint64_t n){assert(depth);auto& s=disk[static_cast<Handle*>(f)->name];if(uint64_t(offset)>s.size())return 1;size_t got=std::min(size_t(n),s.size()-size_t(offset));memcpy(bytes,s.data()+offset,got);*static_cast<uint64_t*>(out)=got;return 0;}
uint32_t delete_file(void* p){assert(depth);return disk.erase(static_cast<char*>(p))?0:missing;}
uint32_t remove_tree(const char* p){assert(depth);std::string prefix=std::string(p)+"/";assert(prefix.rfind("tsuchisd:/tsuchi/downloads/",0)==0&&prefix.size()==strlen("tsuchisd:/tsuchi/downloads")+34);for(auto i=disk.begin();i!=disk.end();)if(i->first.rfind(prefix,0)==0)i=disk.erase(i);else ++i;for(auto i=dirs.begin();i!=dirs.end();)if(*i==p||i->rfind(prefix,0)==0)i=dirs.erase(i);else ++i;return 0;}
}
namespace tsuchi {
void* allocate(size_t n){return std::malloc(n);}void release(void* p){std::free(p);}
bool container_mounted_for(const char*){return false;}
TSUCHI_Result validate_downloaded_assets(const char*,const char*,Text&){return TSUCHI_OK;}
TSUCHI_Result mount_downloaded_assets(const char*,const char*,Text&){return TSUCHI_UNSUPPORTED;}
const ModChoices& mod_choices(){static ModChoices c;return c;}
bool ModChoices::enabled(const char*) const{return true;}
bool each_engine_frame(void(*fn)()){steps.push_back(fn);return true;}bool each_frame(void(*)()){assert(false);return false;}
const void* sd_enter(){++depth;return nullptr;}void sd_leave(const void*){assert(depth>0);--depth;}bool sd_card_mounted(){return true;}
void set_installed_folders(TSUCHI_Result(*fn)(Texts&)){listing=fn;}
bool valid_mod_id(const char* p){return *p&&!strchr(p,'/');}
TSUCHI_Result validate_mod_installation(const Texts& incoming,const Texts&,Text& problem,const Texts*){assert(incoming.first());assert(std::string(incoming.first()).find("/mods/author.mod")!=std::string::npos||std::string(incoming.first()).find(".cheats")!=std::string::npos);if(refuse_manifest){problem.set("Incompatible test manifest.");return TSUCHI_INVALID;}return TSUCHI_OK;}
namespace https {bool https_queue_file(HttpsJob* j){assert(!queued);if(refuse_queue)return false;queued=j;j->state.store(HTTPS_RUNNING);return true;}}
namespace game {
uint64_t main_base(){return 0;}bool readable(uint64_t,uint64_t){return false;}uint64_t module_containing(uint64_t){return 0;}uint64_t module_symbol(uint64_t,const char*){return 0;}
bool files_known(){return true;}
TSUCHI_Result file_size(const char* p,uint64_t& n){assert(depth);auto it=disk.find(p);if(it==disk.end())return TSUCHI_NOT_FOUND;n=it->second.size();return TSUCHI_OK;}
TSUCHI_Result read_file_part(const char* p,uint64_t off,void* out,uint64_t cap,uint64_t& n){uint64_t size=0;if(file_size(p,size)!=TSUCHI_OK)return TSUCHI_NOT_FOUND;n=off>=size?0:std::min(cap,size-off);memcpy(out,disk[p].data()+off,size_t(n));return TSUCHI_OK;}
TSUCHI_Result list_folder(const char* p,Texts& files,Texts& folders){assert(depth);if(!dirs.count(p))return TSUCHI_NOT_FOUND;std::string prefix=std::string(p)+"/";for(const auto& d:dirs)if(d.rfind(prefix,0)==0&&d.find('/',prefix.size())==std::string::npos)folders.add(d.c_str()+prefix.size());for(const auto& f:disk)if(f.first.rfind(prefix,0)==0&&f.first.find('/',prefix.size())==std::string::npos)files.add(f.first.c_str()+prefix.size());return TSUCHI_OK;}
}
}
int main(int argc,char** argv){
    using namespace tsuchi;assert(argc==3||argc==4);std::ifstream input(argv[1],std::ios::binary);std::string data((std::istreambuf_iterator<char>(input)),{});std::string mode=argv[2];
    ready=true;delete_tree=remove_tree;auto& f=t::fn::fs_open_file;f=open_file;t::fn::fs_close_file=close_file;t::fn::fs_create_file=make_file;t::fn::fs_create_folder=make_dir;t::fn::fs_entry_kind=kind;t::fn::fs_file_size=size_file;t::fn::fs_set_file_size=resize_file;t::fn::fs_write_file=write_file;t::fn::fs_read_file=read_file;t::fn::fs_delete_file=delete_file;
    mod_download_start();catalog::Item item{};memcpy(item.key,"author/mod",11);memcpy(item.name,"Example",8);catalog::File file{};memcpy(file.url,"https://github.com/author/mod/releases/download/v1/mod.zip",57);file.bytes=data.size();
    if(mode=="digest"){memset(file.digest,'f',64);}
    refuse_manifest=mode=="manifest";fail_receipt=mode=="receipt";
    if(mode=="queue"){
        refuse_queue=true;assert(mod_download_begin(catalog::Source::github,item,file)==TSUCHI_UNSUPPORTED);
        assert(mod_download_status().phase==DownloadPhase::failed&&!active&&!workspace&&!depth&&disk.empty());return 0;
    }
    assert(mod_download_begin(catalog::Source::github,item,file)==TSUCHI_OK);assert(queued);Texts before;assert(listing(before)==TSUCHI_OK&&!before.used);before.clear();
    void* stream=nullptr;assert(queued->sink->open(*queued,&stream));
    for(size_t off=0;off<data.size();off+=777){size_t size=std::min(size_t(777),data.size()-off);assert(queued->sink->write(stream,off,data.data()+off,size));queued->received.store(off+size);}
    if(mode=="cancel")mod_download_cancel();
    bool success=!queued->cancelled.load()&&queued->sink->commit(stream);
    if(!success)queued->sink->abort(stream);
    queued->state.store(queued->cancelled.load()?https::HTTPS_CANCELLED:success?https::HTTPS_SUCCEEDED:https::HTTPS_FAILED);queued=nullptr;
    assert(listing(before)==TSUCHI_OK&&!before.used);before.clear(); // extraction alone cannot publish
    for(auto fn:steps)fn();assert(depth==0);
    assert(refresh_downloaded_packages()==TSUCHI_OK);
    if(mode=="update"||mode=="update-failure"||mode=="cheat-update"||mode=="cheat-update-invalid"){
        assert(downloaded_package_count()==1);const auto first=*downloaded_package(0);memcpy(item.replacing,first.mods[0],sizeof item.replacing);
        if(argc==4){std::ifstream replacement(argv[3],std::ios::binary);data.assign(std::istreambuf_iterator<char>(replacement),{});file.bytes=data.size();}
        auto run_update=[&](bool fail){
            fail_receipt=fail;memcpy(file.identity,"updated",8);assert(mod_download_begin(catalog::Source::github,item,file)==TSUCHI_OK);
            void* next=nullptr;assert(queued->sink->open(*queued,&next));
            assert(queued->sink->write(next,0,data.data(),data.size()));queued->received.store(data.size());
            assert(queued->sink->commit(next));queued->state.store(https::HTTPS_SUCCEEDED);queued=nullptr;
            Texts still_old;assert(listing(still_old)==TSUCHI_OK&&std::string(still_old.first()).find(first.folder)!=std::string::npos);still_old.clear();
            for(auto fn:steps)fn();assert(refresh_downloaded_packages()==TSUCHI_OK&&downloaded_package_count()==1);
        };
        run_update(mode=="update-failure");
        if(mode=="update-failure"||mode=="cheat-update-invalid")assert(!strcmp(downloaded_package(0)->folder,first.folder));
        else {const auto newer=*downloaded_package(0);assert(newer.revision==1&&!strcmp(newer.previous,first.folder));assert(strcmp(newer.folder,first.folder));
            assert(disk.count(std::string(root)+"/"+first.folder+"/receipt"));
            assert(remove_downloaded_package(0)==TSUCHI_OK&&downloaded_package_count()==0);
            assert(listing(before)==TSUCHI_OK&&!before.used); // removal must not resurrect v1
            // Reinstall after removal advances the revision beyond the tombstone.
            fail_receipt=false;assert(mod_download_begin(catalog::Source::github,item,file)==TSUCHI_OK);
            void* next=nullptr;assert(queued->sink->open(*queued,&next));assert(queued->sink->write(next,0,data.data(),data.size()));queued->received.store(data.size());assert(queued->sink->commit(next));queued->state.store(https::HTTPS_SUCCEEDED);queued=nullptr;
            for(auto fn:steps)fn();assert(downloaded_package_count()==1&&downloaded_package(0)->revision==2);
        }
        return 0;
    }
    if(mode=="cheat-good"){
        assert(mod_download_status().phase==DownloadPhase::installed&&downloaded_package_count()==1);
        const auto* p=downloaded_package(0);assert(p->legacy_assets==2&&strstr(p->mods[0],".cheats"));
        const auto payload=std::string(directory)+"/mods/"+p->mods[0]+"/cheats/060F827BD2B38703.txt";
        assert(disk.at(payload)=="[Original name]\n400F0000 00000000 00000001\n");
        assert(disk.count(std::string(directory)+"/mods/"+p->mods[0]+"/mod.json"));return 0;
    }
    if(mode=="good"){
        assert(mod_download_status().phase==DownloadPhase::installed&&downloaded_package_count()==1);
        assert(listing(before)==TSUCHI_OK&&before.first());before.clear();
        const std::string payload=std::string(directory)+"/mods/author.mod/main.lua";assert(disk.count(payload));
        assert(remove_downloaded_package(0)==TSUCHI_OK&&downloaded_package_count()==0&&disk.count(payload));assert(listing(before)==TSUCHI_OK&&!before.used);
    }else assert(downloaded_package_count()==0&&mod_download_status().phase!=DownloadPhase::installed);
    before.clear();return 0;
}
