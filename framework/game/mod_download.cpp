// Framework-owned SD installs. Workers only stream and extract into a fresh directory;
// the game thread validates manifests and publishes the receipt after the worker ends.
#include "mod_download.h"
#include "mod_zip.h"
#include "cheat_vm.h"
#include "mods.h"
#include "files.h"
#include "frame.h"
#include "sd_card.h"
#include "modules.h"
#include "containers.h"
#include "mod_choices.h"
#include "cost_clock.h"
#include "https_jobs.h"
#include <string.h>
namespace tsuchi { namespace {
namespace t=game::table;
constexpr char root[]="tsuchisd:/tsuchi/downloads";
using DeleteTree=uint32_t(*)(const char*);
DeleteTree delete_tree=nullptr;
bool ready=false, active=false;
List<catalog::Receipt> packages;
https::HttpsJob job;
https::HttpsAtomic<uint32_t> phase{},archive_error{};
https::HttpsAtomic<uint64_t> expanded{},expanded_total{};
DownloadStatus status{};
catalog::Receipt receipt{};
catalog::File chosen{};
char directory[160]{}, archive[192]{};
void* spool=nullptr;
void* workspace=nullptr;
Sha256 hash;
uint64_t counter=0;
bool fresh=false;
char update_target[48]{};

bool append(char* out,size_t room,const char* part){size_t a=strlen(out),b=strlen(part);if(a+b>=room)return false;memcpy(out+a,part,b+1);return true;}
bool path(char* out,size_t room,const char* folder,const char* tail){out[0]=0;return append(out,room,root)&&append(out,room,"/")&&append(out,room,folder)&&append(out,room,tail);}
void hex(const uint8_t* bytes,size_t size,char* out){constexpr char digits[]="0123456789abcdef";for(size_t i=0;i<size;++i){out[2*i]=digits[bytes[i]>>4];out[2*i+1]=digits[bytes[i]&15];}out[2*size]=0;}
bool ours(const char* p){return strlen(p)>sizeof(root)&&!memcmp(p,root,sizeof(root)-1)&&p[sizeof(root)-1]=='/';}
const void* enter(const char* p){return ready&&ours(p)?sd_enter():nullptr;}
void leave(const char* p,const void* previous){if(ready&&ours(p))sd_leave(previous);}
bool initialize(){
    if(ready)return true;
    if(!game::files_known()||!t::bound(t::fn::fs_entry_kind)||!t::bound(t::fn::fs_create_folder)||!t::bound(t::fn::fs_create_file)||!t::bound(t::fn::fs_write_file)||!t::bound(t::fn::fs_set_file_size)||!t::bound(t::fn::fs_delete_file)||!sd_card_mounted())return false;
    const uint64_t library=game::module_containing(game::global<uint64_t>(t::data::system_library_anchor));
    delete_tree=reinterpret_cast<DeleteTree>(game::module_symbol(library,"_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc"));
    game::file_context_enter=enter;game::file_context_leave=leave;
    ready=true;return true;
}
bool folders(char* file){
    for(char* p=file+strlen("tsuchisd:/");*p;++p)if(*p=='/'){
        *p=0;int32_t kind=-1;uint32_t found=t::fn::fs_entry_kind(&kind,file);
        bool ok=(!found&&uint32_t(kind)==t::constant::fs_entry_folder)||(found==t::constant::fs_no_such_path&&!t::fn::fs_create_folder(file));*p='/';if(!ok)return false;
    }
    return true;
}
bool create(char* file,uint64_t size,void** out){return folders(file)&&!t::fn::fs_create_file(file,int64_t(size))&&!t::fn::fs_open_file(out,file,int32_t(t::constant::fs_write));}
bool write(void* f,uint64_t offset,const void* bytes,uint64_t size){uint32_t how=uint32_t(t::constant::fs_write_through);return !t::fn::fs_write_file(f,int64_t(offset),const_cast<void*>(bytes),size,&how);}
void cleanup(){if(spool){t::fn::fs_close_file(spool);spool=nullptr;}if(fresh&&delete_tree&&ours(directory)&&strlen(directory)==strlen(root)+33)delete_tree(directory);fresh=false;}
bool stream_open(const https::HttpsJob&,void** out){SdScope scope;hash=Sha256{};if(!create(archive,0,&spool)){cleanup();return false;}*out=&job;return true;}
bool stream_write(void*,uint64_t offset,const void* bytes,uint64_t size){SdScope scope;
    if(offset>chosen.bytes||size>chosen.bytes-offset||t::fn::fs_set_file_size(spool,int64_t(offset+size))||!write(spool,offset,bytes,size))return false;
    hash.add(bytes,size_t(size));return true;
}
void* zip_allocate(uint64_t bytes){return bytes<=mod_zip::workspace_bytes()?workspace:nullptr;}
void zip_free(void*){}
uint32_t input_open(void*,const char*,void** out,uint64_t* size){uint32_t r=t::fn::fs_open_file(out,archive,int32_t(t::constant::fs_read));int64_t n=0;if(!r&&t::fn::fs_file_size(&n,*out)){t::fn::fs_close_file(*out);*out=nullptr;return 1;}*size=uint64_t(n);return r;}
uint32_t input_read(void*,void* f,uint64_t off,void* bytes,uint32_t size,uint32_t* got){uint64_t n=0;auto r=t::fn::fs_read_file(&n,f,int64_t(off),bytes,size);*got=uint32_t(n);return r;}
void input_close(void*,void* f){t::fn::fs_close_file(f);}
uint32_t stage_begin(void*,const char*,void** out){*out=&job;return 0;}
uint32_t output_open(void*,void*,const char* name,uint64_t size,void** out){char file[768]{};if(!append(file,sizeof file,directory)||!append(file,sizeof file,"/mods/")||!append(file,sizeof file,name))return 1;return create(file,size,out)?0:1;}
uint32_t output_write(void*,void* f,uint64_t off,const void* bytes,uint32_t size){return write(f,off,bytes,size)?0:1;}
uint32_t output_finish(void*,void* f){t::fn::fs_close_file(f);return 0;}
uint32_t extracted(void*,void*){return 0;}
bool cancelled(void*){return job.cancelled.load()!=0;}
mod_zip::Status extraction{};
void progress(void*){expanded.store(extraction.expanded_done);expanded_total.store(extraction.expanded_total);}
bool stream_commit(void*){
    SdScope scope;t::fn::fs_close_file(spool);spool=nullptr;
    if(job.received.load()!=chosen.bytes)return false;
    uint8_t digest[32];hash.finish(digest);hex(digest,32,receipt.digest);
    if(chosen.digest[0]&&strcmp(chosen.digest,receipt.digest))return false;
    phase.store(uint32_t(DownloadPhase::extracting));extraction={};
    const mod_zip::Platform io{nullptr,zip_allocate,zip_free,input_open,input_read,input_close,stage_begin,output_open,output_write,output_finish,extracted,extracted};
    mod_zip::Request request{};request.program_id=t::constant::program_id;uint8_t identity[32];Sha256 key;key.add(&receipt.source,sizeof receipt.source);key.add(receipt.key,strlen(receipt.key));key.finish(identity);
    memcpy(request.legacy_prefix,"external.",9);hex(identity,12,request.legacy_prefix+9);
    const auto result=mod_zip::extract_zip(io,request, {cancelled,nullptr},extraction,progress,nullptr);
    if(!result.committed){archive_error.store(uint32_t(result.error));return false;}
    t::fn::fs_delete_file(archive);
    return true;
}
void stream_abort(void*){SdScope scope;cleanup();}
const https::FileSink sink{stream_open,stream_write,stream_commit,stream_abort};

TSUCHI_Result validate_cheat_folder(const char* folder,Text& why){
    Texts files,dirs;auto result=game::list_folder(folder,files,dirs);
    if(result==TSUCHI_OK&&(!files.first()||dirs.first()))result=TSUCHI_INVALID;
    auto* parsed=static_cast<cheat::File*>(allocate(sizeof(cheat::File)));
    if(!parsed)result=TSUCHI_NO_MEMORY;
    for(const char* name=files.first();result==TSUCHI_OK&&name;name=files.after(name)){
        char file[512]{};append(file,sizeof file,folder);append(file,sizeof file,"/");append(file,sizeof file,name);
        uint64_t bytes=0,got=0;result=game::file_size(file,bytes);
        if(result==TSUCHI_OK&&(!bytes||bytes>=cheat::kFileBytes))result=TSUCHI_INVALID;
        char* text=result==TSUCHI_OK?static_cast<char*>(allocate(size_t(bytes))):nullptr;
        if(result==TSUCHI_OK&&!text)result=TSUCHI_NO_MEMORY;
        if(result==TSUCHI_OK)result=game::read_file_part(file,0,text,bytes,got);
        if(result==TSUCHI_OK&&got!=bytes)result=TSUCHI_IO;
        char problem[160]{};
        if(result==TSUCHI_OK)result=cheat::parse(text,bytes,*parsed,problem,sizeof problem);
        if(result!=TSUCHI_OK)why.set(problem[0]?problem:"The cheat file could not be read or is too large.");
        release(text);
    }
    release(parsed);files.clear();dirs.clear();return result;
}

TSUCHI_Result scan(List<catalog::Receipt>& found,bool removed=false,bool history=false){
    if(!initialize())return TSUCHI_UNSUPPORTED;
    SdScope scope;Texts files,dirs;auto result=game::list_folder(root,files,dirs);files.clear();
    if(result==TSUCHI_NOT_FOUND)result=TSUCHI_OK;
    uint32_t examined=0;
    for(const char* name=dirs.first();result==TSUCHI_OK&&name;name=dirs.after(name)){
        if(!catalog::folder_valid(name))continue;
        if(++examined>256){result=TSUCHI_CAPACITY;break;}
        char file[192];path(file,sizeof file,name,"/receipt");catalog::Receipt r{};uint64_t bytes=0,got=0;
        const auto measured=game::file_size(file,bytes);
        if(measured!=TSUCHI_OK||(bytes!=sizeof r&&bytes!=sizeof(catalog::ReceiptV1)))continue;
        uint8_t raw[sizeof r]{};if(game::read_file_part(file,0,raw,bytes,got)!=TSUCHI_OK||got!=bytes||!catalog::read_receipt(raw,size_t(bytes),r)||strcmp(r.folder,name))continue;
        if(!found.add(r)){result=TSUCHI_NO_MEMORY;break;}
    }dirs.clear();
    // A receipt is the commit point. Keep predecessors on disk but expose only
    // the newest complete revision; a removed newest revision never resurrects one.
    if(history)return result;
    bool hidden[256]{};
    for(uint32_t i=0;i<found.count;++i){
        for(uint32_t j=0;j<found.count;++j)if(i!=j&&catalog::same_mods(found[i],found[j])&&found[j].revision>found[i].revision)hidden[i]=true;
        char marker[192];path(marker,sizeof marker,found[i].folder,"/removed");uint64_t n=0;if(!removed&&game::file_size(marker,n)==TSUCHI_OK)hidden[i]=true;
    }
    uint32_t kept=0;for(uint32_t i=0;i<found.count;++i)if(!hidden[i])found[kept++]=found[i];found.count=kept;
    return result;
}
bool add_folders(const catalog::Receipt& r,Texts& out){
    char base[192];path(base,sizeof base,r.folder,"/mods/");
    for(uint32_t i=0;i<r.count;++i)if(!out.add(base,r.mods[i]))return false;return true;
}
TSUCHI_Result installed(Texts& out){
    List<catalog::Receipt> found;auto result=scan(found);
    if(result==TSUCHI_UNSUPPORTED)return TSUCHI_OK; // ROM mods still work on builds without SD support.
    for(uint32_t i=0;result==TSUCHI_OK&&i<found.count;++i)if(!add_folders(found[i],out))result=TSUCHI_NO_MEMORY;
    found.clear();return result;
}
void fail(const char* message){status.phase=DownloadPhase::failed;catalog::plain(message,status.message,sizeof status.message);}
void tick(){
    if(!active)return;
    const uint32_t state=job.state.load();
    status.phase=DownloadPhase(phase.load());
    status.done=status.phase==DownloadPhase::extracting?expanded.load():job.received.load();
    status.total=status.phase==DownloadPhase::extracting?expanded_total.load():chosen.bytes;
    if(state<https::HTTPS_SUCCEEDED||state>https::HTTPS_CANCELLED)return;
    active=false;release(workspace);workspace=nullptr;
    if(state!=https::HTTPS_SUCCEEDED){
        if(state==https::HTTPS_CANCELLED){status.phase=DownloadPhase::cancelled;catalog::plain("Download cancelled.",status.message,sizeof status.message);}
        else if(archive_error.load()==uint32_t(mod_zip::Error::Unsupported)||archive_error.load()==uint32_t(mod_zip::Error::Path))fail("This ZIP contains unsupported or incomplete mod files. Use a Tsuchi package, cooked asset pack or build-ID cheat files.");
        else fail(job.error.load()==https::HTTPS_ERROR_HTTP?"The server refused the download. Retry later.":"Download or package validation failed. Check the release ZIP and retry.");
        SdScope scope;cleanup();return;
    }
    if(job.cancelled.load()){status.phase=DownloadPhase::cancelled;SdScope scope;cleanup();return;}
    status.phase=DownloadPhase::validating;receipt.legacy_assets=extraction.legacy_assets?1:extraction.legacy_cheats?2:0;
    Texts files,mods,incoming,existing;Text why;SdScope scope;
    char folder[192]{};append(folder,sizeof folder,directory);append(folder,sizeof folder,"/mods");
    auto result=game::list_folder(folder,files,mods);files.clear();
    for(const char* id=mods.first();result==TSUCHI_OK&&id;id=mods.after(id)) {
        if(!valid_mod_id(id)||receipt.count==catalog::kPackageMods){result=TSUCHI_CAPACITY;break;}
        if(extraction.legacy_assets||extraction.legacy_cheats){
            char manifest[768]="{\"id\":\"";append(manifest,sizeof manifest,id);append(manifest,sizeof manifest,"\",\"name\":\"");
            // JSON quotes and backslashes in source titles are escaped, never interpreted.
            for(const char* p=receipt.title;*p;++p){char ch[3]={*p,0,0};if(*p=='"'||*p=='\\'){ch[0]='\\';ch[1]=*p;}append(manifest,sizeof manifest,ch);}
            append(manifest,sizeof manifest,"\",\"version\":\"1.0.0\"}");
            char file[384]{};append(file,sizeof file,folder);append(file,sizeof file,"/");append(file,sizeof file,id);append(file,sizeof file,"/mod.json");void* handle=nullptr;
            bool saved=create(file,strlen(manifest),&handle);if(saved){saved=write(handle,0,manifest,strlen(manifest));t::fn::fs_close_file(handle);}if(!saved){result=TSUCHI_IO;break;}
        }
        memcpy(receipt.mods[receipt.count++],id,strlen(id)+1);
        if(!incoming.add(folder,"/",id))result=TSUCHI_NO_MEMORY;
        if(result==TSUCHI_OK&&extraction.legacy_assets){char asset_folder[384]{};append(asset_folder,sizeof asset_folder,folder);append(asset_folder,sizeof asset_folder,"/");append(asset_folder,sizeof asset_folder,id);result=validate_downloaded_assets(id,asset_folder,why);}
        if(result==TSUCHI_OK&&extraction.legacy_cheats){char cheat_folder[384]{};append(cheat_folder,sizeof cheat_folder,folder);append(cheat_folder,sizeof cheat_folder,"/");append(cheat_folder,sizeof cheat_folder,id);append(cheat_folder,sizeof cheat_folder,"/cheats");result=validate_cheat_folder(cheat_folder,why);}
    }
    Texts replaced;
    if(result==TSUCHI_OK){List<catalog::Receipt> current;result=scan(current,true);
        for(uint32_t i=0;result==TSUCHI_OK&&i<current.count;++i){const auto& old=current[i];
            if(catalog::same_mods(receipt,old)){
                if(receipt.previous[0]||old.revision==UINT64_MAX){result=TSUCHI_INVALID;break;}
                memcpy(receipt.previous,old.folder,sizeof receipt.previous);receipt.revision=old.revision+1;
                if(!add_folders(old,replaced))result=TSUCHI_NO_MEMORY;
            }else {char marker[192];path(marker,sizeof marker,old.folder,"/removed");uint64_t n=0;if(game::file_size(marker,n)!=TSUCHI_OK&&!add_folders(old,existing))result=TSUCHI_NO_MEMORY;}
        }current.clear();
    }
    if(result==TSUCHI_OK&&receipt.previous[0]){List<catalog::Receipt> history;result=scan(history,true,true);
        for(uint32_t i=0;result==TSUCHI_OK&&i<history.count;++i)if(catalog::same_mods(receipt,history[i])&&!add_folders(history[i],replaced))result=TSUCHI_NO_MEMORY;
        history.clear();
    }
    if(result==TSUCHI_OK&&update_target[0]){bool found=false;for(uint32_t i=0;i<receipt.count;++i)if(!strcmp(receipt.mods[i],update_target))found=true;
        if(!receipt.previous[0]||!found){why.set("This file contains different mods or a different variant. The installed package was kept.");result=TSUCHI_INVALID;}}
    if(result==TSUCHI_OK)result=validate_mod_installation(incoming,existing,why,&replaced);
    replaced.clear();
    files.clear();mods.clear();incoming.clear();existing.clear();
    if(result==TSUCHI_NOT_FOUND){char message[321]="Install the required mod first: ";append(message,sizeof message,why.get());fail(message);}
    else if(result!=TSUCHI_OK)fail(why.get()[0]?why.get():"The package could not be installed.");
    else {
        catalog::seal(receipt);char file[192];path(file,sizeof file,receipt.folder,"/receipt");void* handle=nullptr;
        bool saved=create(file,sizeof receipt,&handle);if(saved){saved=write(handle,0,&receipt,sizeof receipt);t::fn::fs_close_file(handle);}
        catalog::Receipt verify{};uint64_t got=0;
        saved=saved&&game::read_file_part(file,0,&verify,sizeof verify,got)==TSUCHI_OK&&got==sizeof verify&&!memcmp(&verify,&receipt,sizeof verify)&&catalog::receipt_valid(verify);
        if(saved){fresh=false;status.phase=DownloadPhase::installed;catalog::plain("Installed. Restart Pikmin 4 to load these mods.",status.message,sizeof status.message);refresh_downloaded_packages();
            if(receipt.legacy_assets==2)catalog::plain("Installed. Restart to load this cheat package, then use Installed > Cheats to toggle its cheats live.",status.message,sizeof status.message);
            if(receipt.legacy_assets==1){bool mounted=true;Text problem;
                for(uint32_t i=0;i<receipt.count;++i)if(mod_choices().enabled(receipt.mods[i])&&activate_downloaded_assets(receipt.mods[i],problem)!=TSUCHI_OK)mounted=false;
                catalog::plain(mounted?"Installed. Enabled content is available to new loads; reload affected maps or objects.":"Installed. Live activation was unavailable; restart to load this version.",status.message,sizeof status.message);problem.clear();}
        }
        else {t::fn::fs_delete_file(file);fail("The installation receipt could not be saved.");}
    }
    why.clear();if(fresh)cleanup();
}
}
void mod_download_start(){set_installed_folders(installed);if(!each_engine_frame(tick))each_frame(tick);}
TSUCHI_Result mod_download_begin(catalog::Source source,const catalog::Item& item,const catalog::File& file){
    if(active)return TSUCHI_BUSY;
    if(!initialize())return TSUCHI_UNSUPPORTED;
    if(!file.bytes||file.bytes>mod_zip::max_archive||strlen(file.url)>=sizeof job.url||strlen(file.url)<8||memcmp(file.url,"https://",8))return TSUCHI_INVALID;
    workspace=allocate(size_t(mod_zip::workspace_bytes()));if(!workspace)return TSUCHI_NO_MEMORY;
    memcpy(update_target,item.replacing,sizeof update_target);chosen=file;receipt={};catalog::plain(file.name,receipt.filename,sizeof receipt.filename);receipt.updated=file.updated;receipt.source=uint32_t(source);memcpy(receipt.key,item.key,sizeof receipt.key);memcpy(receipt.title,item.name,sizeof receipt.title);memcpy(receipt.asset,file.identity,sizeof receipt.asset);memcpy(receipt.release,file.release,sizeof receipt.release);
    uint64_t stamp[2]={cost_now(),++counter};uint8_t sum[32];sha256(stamp,sizeof stamp,sum);hex(sum,16,receipt.folder);
    path(directory,sizeof directory,receipt.folder,"");path(archive,sizeof archive,receipt.folder,"/package.zip");
    {SdScope scope;char parents[192];path(parents,sizeof parents,receipt.folder,"/x");
        // Never reuse an existing directory, even after a clock collision or interrupted run.
        int32_t kind=0;auto exists=t::fn::fs_entry_kind(&kind,directory);
        if(exists!=t::constant::fs_no_such_path||!folders(parents)){release(workspace);workspace=nullptr;return TSUCHI_IO;}
    }
    archive_error.store(0);fresh=true;status={DownloadPhase::downloading,0,file.bytes,{}};phase.store(uint32_t(DownloadPhase::downloading));expanded.store(0);expanded_total.store(0);
    memcpy(job.url,file.url,strlen(file.url)+1);job.kind=https::HTTPS_FILE;job.max_bytes=file.bytes;job.timeout_ms=1800000;job.connect_timeout_ms=60000;job.sink=&sink;
    job.cancelled.store(0);job.error.store(0);job.http_status.store(0);job.received.store(0);job.total.store(0);
    if(!https::https_queue_file(&job)){SdScope scope;cleanup();release(workspace);workspace=nullptr;fail("The download service is unavailable. Retry later.");return TSUCHI_UNSUPPORTED;}
    active=true;return TSUCHI_OK;
}
void mod_download_cancel(){if(active)job.cancelled.store(1);}
DownloadStatus mod_download_status(){return status;}
TSUCHI_Result refresh_downloaded_packages(){List<catalog::Receipt> fresh_packages;auto result=scan(fresh_packages);if(result==TSUCHI_OK){packages.clear();packages=fresh_packages;}else fresh_packages.clear();return result;}
uint32_t downloaded_package_count(){return packages.count;}
const catalog::Receipt* downloaded_package(uint32_t i){return i<packages.count?&packages[i]:nullptr;}
bool downloaded_assets_active(const char* id){return container_mounted_for(id);}
TSUCHI_Result activate_downloaded_assets(const char* id,Text& problem){
    for(uint32_t i=0;i<packages.count;++i){const auto& p=packages[i];if(p.legacy_assets!=1)continue;
        for(uint32_t n=0;n<p.count;++n)if(!strcmp(id,p.mods[n])){char folder[384];path(folder,sizeof folder,p.folder,"/mods/");append(folder,sizeof folder,id);SdScope scope;
            return mount_downloaded_assets(id,folder,problem);}
    }return TSUCHI_UNSUPPORTED;
}
TSUCHI_Result remove_downloaded_package(uint32_t i){
    if(active)return TSUCHI_BUSY;const auto* r=downloaded_package(i);if(!r||!initialize())return TSUCHI_NOT_FOUND;
    char file[192];path(file,sizeof file,r->folder,"/removed");SdScope scope;void* marker=nullptr;
    if(!create(file,0,&marker))return TSUCHI_IO;t::fn::fs_close_file(marker);
    return refresh_downloaded_packages();
}
}
