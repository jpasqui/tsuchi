#include <string.h>
#include "flow.h"
#include "frame.h"
#include "holds.h"
#include "network_transport.h"
#include "session_service.h"
#include "session_wire.h"
#include "services.h"
namespace tsuchi {
namespace {
namespace wire=session_wire;
double (*clock_now)()=nullptr;
double now(){return clock_now?clock_now():0;}
wire::Session machine{network_transport};
TSUCHI_Mod* message_mod=nullptr;
const void* message_data=nullptr;
uint64_t message_size=0;
char peer_name[wire::WIRE_SESSION_NAME_BYTES]{};
struct ChannelKept {uint32_t subscription;};
struct ChannelWanted {uint32_t version;};
void on_event(void* context,const wire::WireEvent* event){
    auto* mod=static_cast<TSUCHI_Mod*>(context);
    switch(event->type){
    case wire::WIRE_SESSION_EVENT_STARTED:session::emit_joined_to(mod,{event->peer});break;
    case wire::WIRE_SESSION_EVENT_PEER_JOINED:session::emit_peer_joined_to(mod,{event->peer});break;
    case wire::WIRE_SESSION_EVENT_PEER_LEFT:session::emit_peer_left_to(mod,{event->peer,event->reason});break;
    case wire::WIRE_SESSION_EVENT_ENDED:session::emit_ended_to(mod,{event->reason});break;
    case wire::WIRE_SESSION_EVENT_MESSAGE:{
        TSUCHI_Mod* previous=message_mod;const void* previous_data=message_data;const uint64_t previous_size=message_size;
        message_mod=mod;message_data=event->data;message_size=event->data_size;
        session::emit_message_to(mod,{event->peer,bool(event->reason&wire::WIRE_SESSION_SEND_RELIABLE),event->data_size});
        message_mod=previous;message_data=previous_data;message_size=previous_size;break;
    }
    default:break;
    }
}
TSUCHI_Result channel_apply(const Hold& hold,ChannelKept& kept,const ChannelWanted& wanted,bool fresh){
    if(!fresh)return TSUCHI_BUSY;
    return machine.subscribe(mod_id(hold.mod),wanted.version,on_event,hold.mod,&kept.subscription);
}
bool channel_keep(const Hold&,ChannelKept&){return true;}
void channel_restore(const Hold&,ChannelKept& kept,HoldEnd){machine.stop_channel(kept.subscription);}
Holds<ChannelKept,ChannelWanted,channel_apply,channel_keep,channel_restore> channels{"session.channel",HoldOn::nothing,false};
struct SessionKept {bool started;};
struct SessionWanted {bool host;uint32_t port,max_peers;const char* name;TSUCHI_NetAddress target;};
TSUCHI_Result session_apply(const Hold& hold,SessionKept& kept,const SessionWanted& wanted,bool fresh){
    if(!fresh)return TSUCHI_BUSY;
    if(!clock_now||!network_transport())return TSUCHI_UNSUPPORTED;
    wire::WireOptions options{};options.size=sizeof(options);options.port=uint16_t(wanted.port);options.max_peers=uint16_t(wanted.max_peers);memcpy(options.name,wanted.name,strlen(wanted.name)+1);
    TSUCHI_Result result=TSUCHI_OK;
    if(wanted.host)result=machine.host(mod_id(hold.mod),&options,now());
    else {wire::WireAddress host{};host.size=sizeof(host);host.port=uint16_t(wanted.target.port);for(uint32_t i=0;i<4;++i)host.ip[i]=uint8_t(wanted.target.ipv4>>(24-i*8));result=machine.join(mod_id(hold.mod),&host,&options,now());}
    kept.started=result==TSUCHI_OK;return result;
}
bool session_keep(const Hold&,SessionKept&){return true;}
void session_restore(const Hold&,SessionKept& kept,HoldEnd){if(kept.started)machine.stop();}
Holds<SessionKept,SessionWanted,session_apply,session_keep,session_restore> sessions{"session.active",HoldOn::nothing,true};
TSUCHI_Result host(TSUCHI_Mod* mod,uint32_t port,uint32_t max_peers,const char* name){
    if(!port||port>65535||max_peers<2||max_peers>4||strlen(name)>31)return TSUCHI_INVALID;
    return sessions.take(mod,0,0,{true,port,max_peers,name,{}});
}
TSUCHI_Result join(TSUCHI_Mod* mod,const char* host,uint32_t port,const char* name){
    if(!port||port>65535||strlen(name)>31)return TSUCHI_INVALID;
    TSUCHI_NetAddress address{};const TSUCHI_Result found=network_numeric_address(host,port,&address);if(found!=TSUCHI_OK)return found;
    return sessions.take(mod,0,0,{false,port,4,name,address});
}
TSUCHI_Result leave(TSUCHI_Mod* mod){
    if(!sessions.find(mod,0,0))return TSUCHI_NOT_FOUND;
    const TSUCHI_Result result=machine.leave(mod_id(mod),now());
    return result==TSUCHI_OK?sessions.release(mod,0,0):result;
}
TSUCHI_Result info(TSUCHI_SessionInfo* out){wire::WireInfo in{};in.size=sizeof(in);const TSUCHI_Result result=machine.info(&in,now());if(result==TSUCHI_OK)*out={in.state,in.self,in.peers,in.time,in.port,in.refused};return result;}
TSUCHI_Result peer(uint32_t number,TSUCHI_PeerInfo* out){wire::WirePeer in{};in.size=sizeof(in);const TSUCHI_Result result=machine.peer(number,&in,now());if(result==TSUCHI_OK){memcpy(peer_name,in.name,sizeof(peer_name));*out={peer_name,in.round_trip,in.heard,in.pending};}return result;}
TSUCHI_Result version(uint32_t peer,const char* mod,uint32_t* out){return machine.peer_channel(peer,mod,out);}
TSUCHI_Result open_channel(TSUCHI_Mod* mod,uint32_t version){return channels.take(mod,0,0,{version});}
TSUCHI_Result close_channel(TSUCHI_Mod* mod){if(machine.dispatching())return TSUCHI_BUSY;return channels.release(mod,0,0);}
TSUCHI_Result send(TSUCHI_Mod* mod,uint32_t to,const void* data,uint64_t size,bool reliable){if(size>1024)return TSUCHI_INVALID;return machine.send(mod_id(mod),to,data,uint32_t(size),reliable?wire::WIRE_SESSION_SEND_RELIABLE:0,now());}
TSUCHI_Result read_message(TSUCHI_Mod* mod,void* buffer,uint64_t capacity,uint64_t* out){if(message_mod!=mod)return TSUCHI_NOT_READY;*out=capacity<message_size?capacity:message_size;if(*out)memcpy(buffer,message_data,size_t(*out));return TSUCHI_OK;}
// Every engine frame, also where the world does not step (menus, loading screens), with the game's own word
// for whether it is about to hold its thread for a scene change or a save.
void poll(){if(clock_now)machine.poll(now(),game::busy_with_saves_or_scenes());}
const session::Backend service{host,join,leave,info,peer,version,open_channel,close_channel,send,read_message};
}
namespace session_wire {
bool session_owner_active(const char* id){
    if(sessions.count()){const Hold& hold=sessions.hold_at(0);if(mod_usable(hold.mod)&&strcmp(mod_id(hold.mod),id)==0)return true;}
    for(uint32_t i=0;i<channels.count();++i){const Hold& hold=channels.hold_at(i);if(mod_usable(hold.mod)&&strcmp(mod_id(hold.mod),id)==0)return true;}
    return false;
}
}
void session_set_clock(double (*clock)()){clock_now=clock;}
void session_start(){channels.start();sessions.start();each_engine_frame(poll);session::set_backend(&service);}
}
