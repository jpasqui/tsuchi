#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "registry.h"
#include "frame.h"
#include "holds.h"
#include "actor_events.h"
#include "net_service.h"
#include "network_transport.h"
#include "https_jobs.h"
#include "session_service.h"
namespace tsuchi {
void net_start();void session_start();void session_set_clock(double(*)());
bool on_actor_left(void(*)(uint64_t,uint64_t,ActorLeft)){return true;}
void ordinary_lifecycle_listener(void(*)(uint64_t,uint64_t,bool)){}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t){return TSUCHI_OK;}
uint64_t ordinary_lifecycle_restore_part(uint64_t a,uint64_t){return a;}
const char* result_name(TSUCHI_Result value){return value==TSUCHI_IO?"io":value==TSUCHI_NO_MEMORY?"no_memory":value==TSUCHI_CAPACITY?"capacity":"ok";}
namespace game {bool busy_with_saves_or_scenes(){return false;}uint64_t from_handle(uint64_t h){return h;}uint64_t part_owner(uint64_t h){return h;}bool is_game_actor(uint64_t){return true;}uint64_t to_handle(uint64_t h){return h;}}
}
using namespace tsuchi;
namespace {
struct Socket {bool open;uint16_t port;char bytes[2048];uint32_t size;};
Socket sockets[32]{};uint32_t closes=0;bool fail_open=false;
bool yes(){return true;}bool offline(){return false;}
int32_t open(uint16_t port,bool){if(fail_open)return -1;for(auto& s:sockets)if(s.open&&s.port==port&&port)return -3;for(uint32_t i=0;i<32;++i)if(!sockets[i].open){sockets[i]={true,uint16_t(port?port:51000+i),{},0};return int32_t(i);}return -1;}
int32_t send(int32_t,const uint8_t[4],uint16_t port,const void* data,uint32_t size){if(size>2048)return -1;for(auto& s:sockets)if(s.open&&s.port==port){memcpy(s.bytes,data,size);s.size=size;}return 0;}
int32_t receive(int32_t fd,uint8_t ip[4],uint16_t* port,void* out,uint32_t capacity){auto& s=sockets[fd];if(!s.size)return -2;uint32_t size=s.size<capacity?s.size:capacity;memcpy(out,s.bytes,size);s.size=0;ip[0]=127;ip[1]=0;ip[2]=0;ip[3]=1;*port=47700;return int32_t(size);}
int32_t local(int32_t fd,uint8_t ip[4],uint16_t* port){memset(ip,0,4);*port=sockets[fd].port;return 0;}
void close(int32_t fd){assert(sockets[fd].open);sockets[fd].open=false;++closes;}
int32_t route(uint8_t ip[4]){ip[0]=192,ip[1]=168,ip[2]=1,ip[3]=125;return 0;}
const NetBackend transport{yes,offline,open,send,receive,local,close,nullptr,route};
const NetBackend no_route{yes,offline,open,send,receive,local,close,nullptr};
uint32_t starts=0,wakes=0;bool refuse_start=false;
bool worker_start(){++starts;return !refuse_start;}
void wake(){++wakes;auto* job=https::https_take_job();assert(job&&job->url[0]&&job->max_bytes);job->state.store(https::HTTPS_QUEUED);}
uint32_t disposed=0;
void dispose(https::HttpsJob*){++disposed;}
const https::WorkerBackend worker{worker_start,dispose,wake};
uint32_t completion_a=0,completion_b=0;bool stop_on_completion=false;
TSUCHI_Mod* active=nullptr;
void completion(void* context,const TSUCHI_NetCompleted*){++*static_cast<uint32_t*>(context);if(stop_on_completion)mod_stop(active);}
void joined(void* context,const TSUCHI_SessionJoined*){++*static_cast<uint32_t*>(context);}
void ended(void* context,const TSUCHI_SessionEnded*){++*static_cast<uint32_t*>(context);}
uint64_t token=1;uint64_t thread(){return token;}
double clock_value=0;double clock(){return clock_value;}
}
int main(){
 set_network_transport(&transport);https::set_worker_backend(&worker);net_start();session_start();session_set_clock(clock);
 auto* a=mod_create("online.alpha");auto* b=mod_create("online.beta");assert(a&&b);
 const auto& n=net::table;const auto& s=session::table;
 TSUCHI_NetworkStatus status{};assert(n.status(a,&status)==TSUCHI_OK&&status.available&&!status.connected);
 // This machine's address on its network; a transport that can't say is refused.
 {TSUCHI_NetAddress here{};assert(n.route_address(a,&here)==TSUCHI_OK&&here.ipv4==0xC0A8017Du&&here.port==0);
  set_network_transport(&no_route);assert(n.route_address(a,&here)==TSUCHI_UNSUPPORTED);set_network_transport(&transport);}
 TSUCHI_NetAddress address{};assert(n.resolve(a,"127.0.0.1",47700,&address)==TSUCHI_OK&&address.ipv4==0x7f000001);assert(n.resolve(a,"example.org",443,&address)==TSUCHI_UNSUPPORTED);assert(n.resolve(a,"256.0.0.1",443,&address)==TSUCHI_INVALID);assert(n.resolve(a,"1.2.3",0,&address)==TSUCHI_INVALID);
 TSUCHI_Socket socket{};assert(n.udp_open(a,47700,false,&socket)==TSUCHI_OK);const auto stale=socket;
 TSUCHI_Socket other{};assert(n.udp_open(b,47700,false,&other)==TSUCHI_BUSY);assert(n.udp_open(b,0,true,&other)==TSUCHI_OK);
 assert(n.sender(a,socket,&address)==TSUCHI_NOT_READY);assert(n.local_address(a,socket,&address)==TSUCHI_OK&&address.port==47700);
 char data[16]{};uint64_t size=0;address={0x7f000001,47700};assert(n.send(b,socket,&address,"a",1)==TSUCHI_DENIED);assert(n.receive(a,socket,data,sizeof(data),&size)==TSUCHI_NOT_READY);
 assert(n.send(a,socket,&address,"captain",7)==TSUCHI_OK);assert(n.receive(a,socket,data,3,&size)==TSUCHI_OK&&size==3&&memcmp(data,"cap",3)==0);assert(n.sender(a,socket,&address)==TSUCHI_OK&&address.ipv4==0x7f000001);
 assert(n.close(a,socket)==TSUCHI_OK);assert(n.close(a,stale)==TSUCHI_NOT_FOUND);assert(n.udp_open(a,47700,false,&socket)==TSUCHI_OK&&socket.id!=stale.id);
 fail_open=true;TSUCHI_Socket refused{};assert(n.udp_open(a,0,false,&refused)==TSUCHI_IO);fail_open=false;
 TSUCHI_Subscription ca{},cb{};assert(n.on_completed(a,completion,&completion_a,&ca)==TSUCHI_OK);assert(n.on_completed(b,completion,&completion_b,&cb)==TSUCHI_OK);
 assert(starts==0&&wakes==0); // registration, status, numeric lookup and UDP are lazy
 TSUCHI_Request request{};
 assert(n.get(a,"http://invalid/",32,1000,&request)==TSUCHI_INVALID&&starts==0);
 // Failed startup returns its reserved job slot; repeated retries cannot exhaust it.
 refuse_start=true;for(unsigned i=0;i<80;++i)assert(n.get(a,"https://example.org/",32,1000,&request)==TSUCHI_UNSUPPORTED);
 assert(!https::https_take_job()&&wakes==0);refuse_start=false;
 assert(n.get(a,"https://example.org/small",32,1000,&request)==TSUCHI_OK&&wakes==1);TSUCHI_RequestInfo info{};assert(n.request_info(a,request,&info)==TSUCHI_OK&&info.state==TSUCHI_REQUEST_STATE_QUEUED);assert(n.request_info(b,request,&info)==TSUCHI_DENIED);assert(n.read(a,request,0,data,sizeof(data),&size)==TSUCHI_NOT_READY);
 auto* job=https::https_take_job();assert(job);job->body.chunks[0]=const_cast<char*>("captain");job->body.allocated=7;job->received.store(7);job->http_status.store(200);https::https_finish(*job,0);frame(.03f);frame(.03f);assert(completion_a==1&&completion_b==0);assert(n.read(a,request,2,data,3,&size)==TSUCHI_OK&&size==3&&memcmp(data,"pta",3)==0);assert(n.request_info(a,request,&info)==TSUCHI_OK&&strcmp(info.failure,"")==0);assert(n.release_request(a,request)==TSUCHI_OK);assert(n.request_info(a,request,&info)==TSUCHI_NOT_FOUND);
 assert(n.get(a,"http://example.org/",10,100,&request)==TSUCHI_INVALID);assert(n.get(a,"https://example.org/",10,100,&request)==TSUCHI_OK);assert(n.cancel_request(a,request)==TSUCHI_OK);frame(.03f);assert(completion_a==2);assert(n.request_info(a,request,&info)==TSUCHI_OK&&info.state==TSUCHI_REQUEST_STATE_CANCELLED);assert(n.release_request(a,request)==TSUCHI_OK);
 assert(n.get(a,"https://cancel.example/",10,100,&request)==TSUCHI_OK);job=https::https_take_job();assert(job);assert(n.cancel_request(a,request)==TSUCHI_OK&&https::https_stopped(*job));assert(n.request_info(a,request,&info)==TSUCHI_OK&&info.state==TSUCHI_REQUEST_STATE_RUNNING);https::https_finish(*job,0);frame(.03f);assert(n.request_info(a,request,&info)==TSUCHI_OK&&info.state==TSUCHI_REQUEST_STATE_CANCELLED);assert(n.release_request(a,request)==TSUCHI_OK);completion_a=2;
 assert(n.get(a,"https://wrong.example/",10,100,&request)==TSUCHI_OK);job=https::https_take_job();assert(job);https::https_finish(*job,https::HTTPS_ERROR_TLS);frame(.03f);assert(completion_a==3);assert(n.request_info(a,request,&info)==TSUCHI_OK&&strcmp(info.failure,"io")==0);assert(n.release_request(a,request)==TSUCHI_OK);
 uint32_t joined_a=0,joined_b=0,ended_a=0,ended_b=0;TSUCHI_Subscription ja{},jb{},ea{},eb{};
 assert(s.on_joined(a,joined,&joined_a,&ja)==TSUCHI_OK);assert(s.on_joined(b,joined,&joined_b,&jb)==TSUCHI_OK);assert(s.on_ended(a,ended,&ended_a,&ea)==TSUCHI_OK);assert(s.on_ended(b,ended,&ended_b,&eb)==TSUCHI_OK);
 assert(s.open_channel(b,7)==TSUCHI_OK);assert(s.open_channel(a,3)==TSUCHI_OK);assert(s.open_channel(a,3)==TSUCHI_BUSY);assert(s.host(a,47801,4,"Olimar")==TSUCHI_OK);frame(.03f);assert(joined_a==1&&joined_b==1);TSUCHI_SessionInfo session_info{};assert(s.info(a,&session_info)==TSUCHI_OK&&session_info.state==TSUCHI_SESSION_STATE_HOSTING);uint32_t version=0;assert(s.channel_version(a,0,"online.alpha",&version)==TSUCHI_OK&&version==3);TSUCHI_PeerInfo peer{};assert(s.peer(a,0,&peer)==TSUCHI_OK&&strcmp(peer.name,"Olimar")==0);assert(s.peer(a,3,&peer)==TSUCHI_NOT_FOUND);assert(s.send(a,255,"x",1,true)==TSUCHI_NOT_READY);assert(s.read_message(a,data,sizeof(data),&size)==TSUCHI_NOT_READY);assert(s.leave(b)==TSUCHI_NOT_FOUND);assert(s.leave(a)==TSUCHI_OK);frame(.03f);assert(ended_a==1&&ended_b==1);assert(s.close_channel(a)==TSUCHI_OK);assert(s.close_channel(a)==TSUCHI_NOT_FOUND);
 assert(s.join(a,"127.0.0.1",47801,"Oatchi")==TSUCHI_OK);assert(s.info(a,&session_info)==TSUCHI_OK&&session_info.state==TSUCHI_SESSION_STATE_JOINING);assert(s.leave(a)==TSUCHI_OK);
 set_thread_probe(thread);note_game_thread();token=2;const uint32_t before=closes;assert(n.close(a,socket)==TSUCHI_INVALID);assert(s.open_channel(a,1)==TSUCHI_INVALID);assert(closes==before);token=1;
 // Terminal work that finishes after stop must release storage without events.
 assert(n.get(a,"https://late.example/",10,100,&request)==TSUCHI_OK);job=https::https_take_job();assert(job);const uint32_t previous=disposed;mod_stop(a);assert(https::https_stopped(*job));assert(closes==before+1);https::https_finish(*job,0);frame(.03f);assert(disposed==previous+1&&completion_a==3);assert(n.status(a,&status)==TSUCHI_INVALID);
 mod_stop(b);assert(closes==before+2);
 // Every per-mod counted socket and request limit is restored on stop.
 auto* c=mod_create("capacity.mod");for(uint32_t i=0;i<16;++i)assert(n.udp_open(c,0,false,&socket)==TSUCHI_OK);assert(n.udp_open(c,0,false,&socket)==TSUCHI_CAPACITY);for(uint32_t i=0;i<8;++i)assert(n.get(c,"https://example.org/",10,100,&request)==TSUCHI_OK);assert(n.get(c,"https://example.org/",10,100,&request)==TSUCHI_CAPACITY);assert(s.open_channel(c,1)==TSUCHI_OK);assert(s.host(c,47809,2,"Captain")==TSUCHI_OK);mod_stop(c);assert(s.info(mod_create("observer.mod"),&session_info)==TSUCHI_OK&&session_info.state==TSUCHI_SESSION_STATE_NONE);
 auto* d=mod_create("completion.stop");active=d;stop_on_completion=true;uint32_t stopped_events=0;TSUCHI_Subscription stopped_subscription{};assert(n.on_completed(d,completion,&stopped_events,&stopped_subscription)==TSUCHI_OK);assert(n.get(d,"https://example.org/",10,100,&request)==TSUCHI_OK);job=https::https_take_job();assert(job);https::https_finish(*job,0);const uint32_t before_dispose=disposed;frame(.03f);frame(.03f);assert(stopped_events==1&&disposed==before_dispose+1&&!mod_usable(d));stop_on_completion=false;
 puts("PASS net and session service calls, ownership, stop, request completion, and limits");
}
