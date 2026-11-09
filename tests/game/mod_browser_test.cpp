#include "mods.h"
#include "mod_choices.h"
#include "browser/provider.h"
#include "browser/controller.h"
#include "browser/bridge.h"
#include "net_service.h"
#include "mod_download.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
using namespace tsuchi;
namespace {
struct Request {std::string url,body;uint32_t state=1,http=200;bool cancelled=false,released=false;};
std::map<uint64_t,Request> requests;uint64_t next=0;
bool library_fixture=false;catalog::Receipt library_receipt{};
DownloadStatus download{};catalog::Source installed_source{};catalog::Item installed_item{};catalog::File installed_file{};uint32_t installs=0;
TSUCHI_NetService make_net(){TSUCHI_NetService t{};
 t.get=[](TSUCHI_Mod*,const char* url,uint64_t limit,uint32_t,TSUCHI_Request* out){assert(limit<=1u<<20||limit==2u<<20);*out={++next};requests[next].url=url;return TSUCHI_OK;};
 t.request_info=[](TSUCHI_Mod*,TSUCHI_Request id,TSUCHI_RequestInfo* out){const auto& r=requests.at(id.id);assert(!r.released);*out={r.state,r.http,r.body.size(),r.body.size(),""};return TSUCHI_OK;};
 t.read=[](TSUCHI_Mod*,TSUCHI_Request id,uint64_t off,void* out,uint64_t cap,uint64_t* got){const auto& r=requests.at(id.id);assert(!r.released&&off==0&&cap>=r.body.size());memcpy(out,r.body.data(),r.body.size());*got=r.body.size();return TSUCHI_OK;};
 t.cancel_request=[](TSUCHI_Mod*,TSUCHI_Request id){requests.at(id.id).cancelled=true;return TSUCHI_OK;};
 t.release_request=[](TSUCHI_Mod*,TSUCHI_Request id){requests.at(id.id).released=true;return TSUCHI_OK;};return t;
}
uint64_t latest(const char* part){for(auto it=requests.rbegin();it!=requests.rend();++it)if(!it->second.released&&it->second.url.find(part)!=std::string::npos)return it->first;assert(false);return 0;}
void reply(uint64_t id,const std::string& body,uint32_t status=200){auto& r=requests.at(id);r.body=body;r.http=status;r.state=status==200?2:3;}
const std::string gb_item=R"({"_idRow":42,"_sModelName":"Mod","_aGame":{"_idRow":18712},"_sName":"Banana mod","_aSubmitter":{"_idRow":7,"_sName":"Author"},"_nLikeCount":2,"_bHasFiles":true})";
std::string gb_page(const std::string& item=gb_item){return R"({"_aMetadata":{"_nRecordCount":1,"_nPerpage":24},"_aRecords":[)"+item+"]}";}
const std::string gh_repo=R"({"id":42,"name":"Github mod","full_name":"author/example","private":false,"fork":false,"description":"A mod on GitHub","owner":{"login":"author","avatar_url":"https://avatars.githubusercontent.com/u/1"},"stargazers_count":3,"updated_at":"2026-11-08T12:00:00Z"})";
std::string gh_page(){return R"({"total_count":1,"items":[)"+gh_repo+"]}";}
const std::string releases=R"([{"tag_name":"v1","draft":false,"prerelease":false,"assets":[{"id":4294967300,"name":"example.zip","size":500,"state":"uploaded","browser_download_url":"https://github.com/author/example/releases/download/v1/example.zip"}]}])";
void settle(browser::Provider& p){for(int i=0;i<10;++i)p.pump();}
}
namespace tsuchi {
void* allocate(size_t n){return std::malloc(n);}void release(void* p){std::free(p);}
namespace net {const TSUCHI_NetService table=make_net();}
TSUCHI_Result mod_download_begin(catalog::Source source,const catalog::Item& item,const catalog::File& file){installed_source=source;installed_item=item;installed_file=file;++installs;download.phase=DownloadPhase::downloading;return TSUCHI_OK;}
void mod_download_cancel(){download.phase=DownloadPhase::cancelled;}DownloadStatus mod_download_status(){return download;}
uint32_t downloaded_package_count(){return library_fixture?1:0;}const catalog::Receipt* downloaded_package(uint32_t i){return library_fixture&&!i?&library_receipt:nullptr;}
TSUCHI_Result refresh_downloaded_packages(){return TSUCHI_OK;}TSUCHI_Result remove_downloaded_package(uint32_t){return TSUCHI_NOT_FOUND;}
}
int main(){using namespace browser;bridge_start(reinterpret_cast<TSUCHI_Mod*>(1));static Provider p;static char scratch[json_bytes];assert(p.initialize(transport(),"test",scratch,sizeof scratch)==TSUCHI_OK);
 Query q{};q.sort=Sort::Popular;assert(p.begin_batch(q,1,1)==TSUCHI_OK);auto gb=latest("gamebanana.com"),gh=latest("search/repositories");assert(requests[gh].url.find("topic%3Atsuchi-mod")!=std::string::npos&&requests[gh].url.find("sort=stars")!=std::string::npos);
 reply(gb,gb_page());settle(p);assert(p.batch_operation().state==State::Loading);reply(gh,gh_page());settle(p);assert(p.batch_operation().state==State::Ready&&p.batch().count==2);
 const auto github=p.batch().listings[0],banana=p.batch().listings[1];assert(github.source==0&&banana.source==1&&banana.id==42&&github.id!=42);assert(!strcmp(github.source_key,"author/example"));
 assert(p.begin_detail(github.id,1)==TSUCHI_OK);reply(latest("/repos/author/example"),gh_repo);settle(p);reply(latest("/releases?"),releases);settle(p);assert(p.detail_operation().state==State::Ready&&p.detail().file_count==1);
 assert(!strcmp(p.detail().files[0].identity,"4294967300"));assert(p.begin_download(p.detail().files[0],"unused")==TSUCHI_OK);assert(installs==1&&installed_source==catalog::Source::github&&!strcmp(installed_item.key,"author/example")&&!strcmp(installed_file.identity,"4294967300"));p.cancel_download();assert(download.phase==DownloadPhase::cancelled);
 // One provider failing does not discard the other provider's catalogue.
 assert(p.begin_batch(q,1,2)==TSUCHI_OK);reply(latest("gamebanana.com"),gb_page());reply(latest("search/repositories"),"{}",403);settle(p);assert(p.batch().count==1&&p.batch().failed_sources==1&&p.batch_operation().state==State::Ready);
 assert(p.begin_batch(q,1,3)==TSUCHI_OK);reply(latest("gamebanana.com"),"{}",503);reply(latest("search/repositories"),gh_page());settle(p);assert(p.batch().count==1&&p.batch().failed_sources==2&&p.batch_operation().state==State::Ready);
 assert(p.begin_batch(q,1,4)==TSUCHI_OK);reply(latest("gamebanana.com"),"{}",503);reply(latest("search/repositories"),"{}",403);settle(p);assert(p.batch_operation().state==State::Failed);
 // Full mixed batches fit the original continuous controller, with source-qualified IDs.
 std::string bananas,githubs;for(int i=0;i<24;++i){if(i)bananas+=",";auto item=gb_item;item.replace(item.find("42"),2,std::to_string(100+i));bananas+=item;}
 for(int i=0;i<6;++i){if(i)githubs+=",";auto item=gh_repo;const auto at=item.find("author/example");item.replace(at,14,"author/mod"+std::to_string(i));githubs+=item;}
 assert(p.begin_batch(q,1,7)==TSUCHI_OK);reply(latest("gamebanana.com"),R"({"_aMetadata":{"_nRecordCount":24,"_nPerpage":24},"_aRecords":[)"+bananas+"]}");reply(latest("search/repositories"),R"({"total_count":6,"items":[)"+githubs+"]}");settle(p);assert(p.batch_operation().state==State::Ready&&p.batch().count==30);
 // Author browsing stays scoped to the selected author and provider. A failed
 // GitHub author request cannot look like a successful empty GameBanana page.
 Query author_query{};author_query.author_id=github.author_id;assert(p.begin_batch(author_query,1,8)==TSUCHI_OK);
 assert(requests[latest("search/repositories")].url.find("%20user:author")!=std::string::npos);reply(latest("search/repositories"),"{}",403);settle(p);assert(p.batch_operation().state==State::Failed);
 assert(p.begin_batch(author_query,1,9)==TSUCHI_OK);reply(latest("search/repositories"),gh_page());settle(p);assert(p.batch().count==1&&!p.batch().failed_sources&&p.batch_operation().state==State::Ready);
 assert(p.begin_author(github.author_id,9)==TSUCHI_OK);reply(latest("https://api.github.com/users/author"),R"({"login":"author","name":"The author","followers":4})");settle(p);assert(p.author_operation().state==State::Ready&&!strcmp(p.author().name,"The author"));
 // The detail cap reports omitted files, including a partial final chunk.
 std::string assets;for(int i=0;i<33;++i){if(i)assets+=",";assets+=R"({"id":)"+std::to_string(1000+i)+R"(,"name":"mod.zip","size":50,"state":"uploaded","browser_download_url":"https://github.com/author/example/releases/download/v1/mod.zip"})";}
 assert(p.begin_detail(github.id,10)==TSUCHI_OK);reply(latest("/repos/author/example"),gh_repo);settle(p);reply(latest("/releases?"),R"([{"tag_name":"v1","assets":[)"+assets+"]}]");settle(p);assert(p.detail().file_count==32&&p.detail().files_truncated);
 assert(p.begin_detail(0xffffffff,11)==TSUCHI_INVALID&&p.detail_operation().state==State::Failed);
 assert(p.begin_author(0xffffffff,11)==TSUCHI_INVALID&&p.author_operation().state==State::Failed);
 // A superseded request is released; its late response cannot populate a new query.
 assert(p.begin_batch(q,1,5)==TSUCHI_OK);const auto old=latest("search/repositories");memcpy(q.search,"new",4);q.sort=Sort::Latest;assert(p.begin_batch(q,1,6)==TSUCHI_OK);assert(requests[old].cancelled&&requests[old].released);p.shutdown();
 // Detail Back restores the selected card and scroll anchor in the original controller.
 static Controller c;assert(c.initialize(transport(),{})==TSUCHI_OK);c.attach_installer(&install::installer());q={};assert(c.open(q)==TSUCHI_OK);
 reply(latest("gamebanana.com"),gb_page());reply(latest("search/repositories"),gh_page());for(int i=0;i<12;++i)c.pump();auto state=c.browse();assert(state.count==2&&!state.loading);
 assert(c.select(42,state.generation));assert(c.viewport(42,42,.25f,state.generation));assert(c.open_detail(42,state.generation)==TSUCHI_OK);c.back();state=c.browse();assert(state.selected_id==42&&state.anchor_id==42&&state.row_offset==.25f);
 assert(!c.select(42,state.generation-1));
 q={};q.sort=Sort::Popular;assert(c.change_query(q)==TSUCHI_OK);
 reply(latest("gamebanana.com"),R"({"_aMetadata":{"_nRecordCount":24,"_nPerpage":24},"_aRecords":[)"+bananas+"]}");reply(latest("search/repositories"),R"({"total_count":6,"items":[)"+githubs+"]}");for(int i=0;i<20;++i)c.pump();state=c.browse();assert(state.count==30&&!state.loading);
 library_fixture=true;library_receipt.source=0;library_receipt.count=1;library_receipt.updated=1;
 catalog::plain("author/example",library_receipt.key,sizeof library_receipt.key);catalog::plain("4294967300",library_receipt.asset,sizeof library_receipt.asset);
 catalog::plain("author.example",library_receipt.mods[0],sizeof library_receipt.mods[0]);catalog::plain("Example",library_receipt.title,sizeof library_receipt.title);
 assert(c.open_installed()==TSUCHI_OK);c.pump();assert(c.screen()==Screen::Installed&&c.browse().count==1);
 assert(c.open_installed_page(1)==TSUCHI_OK);assert(strstr(c.update_status(),"Checking"));
 reply(latest("/repos/author/example"),gh_repo);for(int i=0;i<12;++i)c.pump();reply(latest("/releases?"),releases);for(int i=0;i<12;++i)c.pump();
 assert(c.detail_state()==State::Ready&&!c.file_current(c.detail().files[0]));assert(strstr(c.update_status(),"unknown"));
 auto comparable=c.detail().files[0];comparable.updated=1;assert(c.file_current(comparable));comparable.updated=2;assert(!c.file_current(comparable));
 c.back();assert(c.screen()==Screen::Installed&&c.browse().selected_id==1);
 assert(c.open_installed_page(1)==TSUCHI_OK);reply(latest("/repos/author/example"),"{}",503);for(int i=0;i<12;++i)c.pump();assert(strstr(c.update_status(),"failed"));
 c.back();assert(c.open_installed_page(1)==TSUCHI_OK);reply(latest("/repos/author/example"),gh_repo);for(int i=0;i<12;++i)c.pump();
 auto changed=releases;const auto offset=changed.find("\"size\":500");changed.insert(offset,"\"updated_at\":\"2026-11-08T12:00:00Z\",");
 reply(latest("/releases?"),changed);for(int i=0;i<12;++i)c.pump();assert(!c.file_current(c.detail().files[0])&&strstr(c.update_status(),"Newer"));
 c.shutdown();return 0;
}

namespace tsuchi {
bool downloaded_assets_active(const char*){return false;}
uint32_t mod_count(){return 0;} ModRecord* mod_at(uint32_t){return nullptr;} ModRecord* find_mod(const char*){return nullptr;}
const ModChoices& mod_choices(){static ModChoices choices;return choices;}
bool ModChoices::enabled(const char*) const {return true;}
}
