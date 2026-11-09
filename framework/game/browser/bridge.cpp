// Runtime boundaries for the ported Mods frontend. All entry points are game-thread only.
#include "bridge.h"
#include "net_service.h"
#include "mod_download.h"
#include "registry.h"
#include "mods.h"
#include "mod_choices.h"
#include <string.h>
namespace tsuchi::browser { namespace {
TSUCHI_Mod* owner=nullptr;
struct Transfer {uint32_t id;TSUCHI_Request request;};
Transfer transfers[16]{};uint32_t next_transfer=0;
Transfer* transfer(uint32_t id){for(auto& t:transfers)if(id&&t.id==id)return &t;return nullptr;}
TSUCHI_Result capabilities(uint32_t* out){*out=P4M_HTTPS_ASYNC|P4M_HTTPS_VERIFIED_TLS|P4M_HTTPS_MEMORY_RESULTS;return TSUCHI_OK;}
TSUCHI_Result get(const char*,const P4M_HttpsRequest* in,uint32_t* out){
 if(!owner||!in||!out||in->kind!=P4M_HTTPS_MEMORY)return TSUCHI_INVALID;
 for(auto& t:transfers)if(!t.id){TSUCHI_Request r{};auto result=net::table.get(owner,in->url,in->max_bytes,in->timeout_ms,&r);if(result!=TSUCHI_OK)return result;
  if(++next_transfer==0)++next_transfer;t={next_transfer,r};*out=t.id;return TSUCHI_OK;}
 return TSUCHI_CAPACITY;
}
TSUCHI_Result status(uint32_t id,P4M_HttpsStatus* out){auto* t=transfer(id);if(!t)return TSUCHI_NOT_FOUND;TSUCHI_RequestInfo i{};
 auto r=net::table.request_info(owner,t->request,&i);if(r!=TSUCHI_OK)return r;
 *out={};out->size=sizeof(*out);out->state=i.state<TSUCHI_REQUEST_STATE_SUCCEEDED?P4M_HTTPS_RUNNING:i.state==TSUCHI_REQUEST_STATE_SUCCEEDED?P4M_HTTPS_SUCCEEDED:i.state==TSUCHI_REQUEST_STATE_CANCELLED?P4M_HTTPS_CANCELLED:P4M_HTTPS_FAILED;
 out->received=i.received;out->total=i.total;out->http_status=i.http_status;out->error=out->state==P4M_HTTPS_FAILED?(i.http_status>=400?P4M_HTTPS_ERROR_HTTP:P4M_HTTPS_ERROR_NETWORK):0;return TSUCHI_OK;
}
TSUCHI_Result read(uint32_t id,uint64_t offset,void* out,uint32_t capacity,uint32_t* got){auto* t=transfer(id);if(!t)return TSUCHI_NOT_FOUND;uint64_t bytes=0;auto r=net::table.read(owner,t->request,offset,out,capacity,&bytes);*got=uint32_t(bytes);return r;}
TSUCHI_Result cancel(uint32_t id){auto* t=transfer(id);return t?net::table.cancel_request(owner,t->request):TSUCHI_NOT_FOUND;}
TSUCHI_Result release_transfer(uint32_t id){auto* t=transfer(id);if(!t)return TSUCHI_NOT_FOUND;auto r=net::table.release_request(owner,t->request);*t={};return r;}
const P4M_HttpsApi api{sizeof(P4M_HttpsApi),1,capabilities,get,status,read,cancel,release_transfer};
struct Identity {uint32_t source,id;bool file;char key[128];};
Identity identities[1024]{};uint32_t identity_count=0;
uint32_t identity(uint32_t source,const char* key,bool file){
 if(source==1){uint64_t n=0;const char* p=key;for(;*p>='0'&&*p<='9';++p){n=n*10+uint32_t(*p-'0');if(n>=0x80000000)return 0;}if(p!=key&&!*p)return uint32_t(n);}
 for(uint32_t i=0;i<identity_count;++i)if(identities[i].source==source&&identities[i].file==file&&!strcmp(identities[i].key,key))return identities[i].id;
 if(identity_count==1024||strlen(key)>=128)return 0;auto& i=identities[identity_count++];i.source=source;i.file=file;i.id=0x80000000+identity_count;memcpy(i.key,key,strlen(key)+1);return i.id;
}
void decimal(uint64_t value,char* out){char digits[24];uint32_t n=0;do{digits[n++]=char('0'+value%10);value/=10;}while(value);uint32_t at=0;while(n)out[at++]=digits[--n];out[at]=0;}
install::Status installation{};install::Library library{};
void poll_install(){auto s=mod_download_status();using I=install::State;
 catalog::plain(s.message,installation.message,sizeof installation.message);
 installation.compressed_done=s.done;installation.compressed_total=s.total;
 switch(s.phase){case DownloadPhase::idle:installation.state=I::Idle;break;case DownloadPhase::downloading:installation.state=I::Idle;break;
 case DownloadPhase::extracting:installation.state=I::Extracting;break;case DownloadPhase::validating:installation.state=I::Validating;break;
 case DownloadPhase::installed:installation.state=I::Installed;break;case DownloadPhase::failed:installation.state=I::Failed;installation.error=install::Error::Zip;break;
 case DownloadPhase::cancelled:installation.state=I::Cancelled;installation.error=install::Error::Cancelled;break;}
}
}
void bridge_start(TSUCHI_Mod* mod){owner=mod;}
const P4M_HttpsApi* transport(){return &api;}
uint32_t source_id(uint32_t source,const char* key){return identity(source,key,false);}
const char* source_key(uint32_t id){for(uint32_t i=0;i<identity_count;++i)if(!identities[i].file&&identities[i].id==id)return identities[i].key;return nullptr;}
uint32_t file_id(uint32_t source,const char* key){return identity(source,key,true);}
P4M_Result begin_install(const Listing& listing,const File& file,const char* replacing){
 catalog::Item item{};catalog::File asset{};if(replacing)catalog::plain(replacing,item.replacing,sizeof item.replacing);const auto source=catalog::Source(listing.source);
 if(listing.source_key[0])catalog::plain(listing.source_key,item.key,sizeof item.key);else decimal(listing.id,item.key);
 catalog::plain(listing.title,item.name,sizeof item.name);catalog::plain(listing.author,item.author,sizeof item.author);
 catalog::plain(file.name,asset.name,sizeof asset.name);catalog::plain(file.url,asset.url,sizeof asset.url);catalog::plain(file.version,asset.release,sizeof asset.release);
 if(file.identity[0])catalog::plain(file.identity,asset.identity,sizeof asset.identity);else decimal(file.id,asset.identity);
 catalog::plain(file.digest,asset.digest,sizeof asset.digest);asset.bytes=file.size;asset.updated=file.updated;
 const auto r=mod_download_begin(source,item,asset);if(r!=TSUCHI_OK)return r;
 installation={};installation.submission_id=listing.id;installation.file_id=file.id;return r;
}
void install_download_status(Download& download){
 const auto s=mod_download_status();auto& op=download.operation;op.transfer.received=s.done;op.transfer.total=s.total;
 op.state=s.phase==DownloadPhase::failed?State::Failed:s.phase==DownloadPhase::cancelled?State::Cancelled:s.phase==DownloadPhase::downloading?State::Loading:State::Ready;
}
void cancel_install(){mod_download_cancel();}
namespace install {
Installer& installer(){static Installer value;return value;}
bool Installer::status(Status& out)const{poll_install();out=installation;return true;}
void Installer::cancel(){cancel_install();}
P4M_Result Installer::refresh_library(){
 const auto result=refresh_downloaded_packages();++browser::library.revision;browser::library.state=result==TSUCHI_OK?LibraryState::Ready:LibraryState::Failed;
 browser::library.count=0;if(result!=TSUCHI_OK)return result;
 const auto add=[&](const char* id,const char* title,const catalog::Receipt* p,bool pending){
  if(browser::library.count==max_installed)return;
  auto& out=browser::library.records[browser::library.count++];out={};out.abi=2;
  catalog::plain(id,out.mod_id,sizeof out.mod_id);catalog::plain(title,out.title,sizeof out.title);
  out.enabled=mod_choices().enabled(id);out.pending=pending;
  if(p){out.source=p->source;catalog::plain(p->key,out.source_key,sizeof out.source_key);
   out.submission_id=source_id(p->source,p->key);out.file_id=browser::file_id(p->source,p->asset);out.asset_count=p->count;out.legacy_assets=p->legacy_assets==1;out.legacy_cheats=p->legacy_assets==2;out.live_assets=downloaded_assets_active(id);out.updated=p->updated;catalog::plain(p->digest,out.digest,sizeof out.digest);catalog::plain(p->filename,out.archive_name,sizeof out.archive_name);
   catalog::plain(p->release,out.filename,sizeof out.filename);}
 };
 for(uint32_t i=0;i<mod_count();++i){const auto* mod=mod_at(i);const catalog::Receipt* package=nullptr;
  for(uint32_t j=0;j<downloaded_package_count();++j){const auto* p=downloaded_package(j);for(uint32_t n=0;n<p->count;++n)if(!strcmp(mod->id,p->mods[n]))package=p;}
  add(mod->id,mod->name.get(),package,package&&(!mod->folder[0]||strlen(mod->folder)<strlen("tsuchisd:/tsuchi/downloads/")+32||memcmp(mod->folder+strlen("tsuchisd:/tsuchi/downloads/"),package->folder,32)));
 }
 for(uint32_t i=0;i<downloaded_package_count();++i){const auto* p=downloaded_package(i);
  for(uint32_t n=0;n<p->count;++n)if(!find_mod(p->mods[n]))add(p->mods[n],p->title,p,true);
 }return TSUCHI_OK;
}
P4M_Result Installer::uninstall(uint32_t submission,uint32_t file){
 for(uint32_t i=0;i<downloaded_package_count();++i){const auto* p=downloaded_package(i);if(p&&source_id(p->source,p->key)==submission&&browser::file_id(p->source,p->asset)==file){
  auto result=remove_downloaded_package(i);if(result!=TSUCHI_OK)return result;refresh_library();browser::library.removed_submission=submission;browser::library.removed_file=file;return TSUCHI_OK;
 }}return TSUCHI_NOT_FOUND;
}
bool Installer::library(Library& out)const{out=browser::library;return true;}
}
}
