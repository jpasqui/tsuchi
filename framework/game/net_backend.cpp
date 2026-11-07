#include <string.h>
#include "holds.h"
#include "net_service.h"
#include "network_transport.h"
#include "network_requests.h"
#include "services.h"
namespace tsuchi {
namespace {
const NetBackend* transport=nullptr;
uint64_t next_socket=1;
struct SocketKept {const NetBackend* transport;int32_t descriptor;TSUCHI_NetAddress sender;bool heard;};
struct SocketWanted {uint16_t port;bool broadcast;};
TSUCHI_Result socket_apply(const Hold&,SocketKept& kept,const SocketWanted& wanted,bool fresh){
    if(!fresh)return TSUCHI_BUSY;
    if(!transport||!transport->available())return TSUCHI_UNSUPPORTED;
    const int32_t fd=transport->udp_open(wanted.port,wanted.broadcast);
    if(fd<0)return fd==-3?TSUCHI_BUSY:TSUCHI_IO;
    kept.transport=transport;kept.descriptor=fd;
    return TSUCHI_OK;
}
bool socket_keep(const Hold&,SocketKept&){return true;}
void socket_restore(const Hold&,SocketKept& kept,HoldEnd){kept.transport->close(kept.descriptor);}
Holds<SocketKept,SocketWanted,socket_apply,socket_keep,socket_restore> sockets{"net.socket",HoldOn::nothing,false};
TSUCHI_Result socket_of(TSUCHI_Mod* mod,TSUCHI_Socket socket,SocketKept*& out){
    out=sockets.find(mod,0,socket.id);
    return out?TSUCHI_OK:sockets.find(0,socket.id)?TSUCHI_DENIED:TSUCHI_NOT_FOUND;
}
void octets(uint32_t address,uint8_t ip[4]){for(uint32_t i=0;i<4;++i)ip[i]=uint8_t(address>>(24-i*8));}
uint32_t packed(const uint8_t ip[4]){return(uint32_t(ip[0])<<24)|(uint32_t(ip[1])<<16)|(uint32_t(ip[2])<<8)|ip[3];}
TSUCHI_Result status(TSUCHI_NetworkStatus* out){out->available=transport&&transport->available();out->connected=transport&&transport->connected();return TSUCHI_OK;}
TSUCHI_Result open(TSUCHI_Mod* mod,uint32_t port,bool broadcast,TSUCHI_Socket* out){
    if(port>65535)return TSUCHI_INVALID;
    uint32_t count=0;for(uint32_t i=0;i<sockets.count();++i)count+=sockets.hold_at(i).mod==mod;
    if(count>=16)return TSUCHI_CAPACITY;
    if(!next_socket)return TSUCHI_CAPACITY;
    const uint64_t id=next_socket++;
    const TSUCHI_Result result=sockets.take(mod,0,id,{uint16_t(port),broadcast});
    if(result==TSUCHI_OK)out->id=id;
    return result;
}
TSUCHI_Result send(TSUCHI_Mod* mod,TSUCHI_Socket socket,const TSUCHI_NetAddress* to,const void* data,uint64_t size){
    if(!to->port||to->port>65535||!size||size>65507)return TSUCHI_INVALID;
    SocketKept* held=nullptr;const TSUCHI_Result found=socket_of(mod,socket,held);if(found!=TSUCHI_OK)return found;
    uint8_t ip[4];octets(to->ipv4,ip);
    return held->transport->send_to(held->descriptor,ip,uint16_t(to->port),data,uint32_t(size))==0?TSUCHI_OK:TSUCHI_IO;
}
TSUCHI_Result receive(TSUCHI_Mod* mod,TSUCHI_Socket socket,void* buffer,uint64_t capacity,uint64_t* out){
    if(!capacity||capacity>65507)return TSUCHI_INVALID;
    SocketKept* held=nullptr;const TSUCHI_Result found=socket_of(mod,socket,held);if(found!=TSUCHI_OK)return found;
    uint8_t ip[4]{};uint16_t port=0;
    const int32_t size=held->transport->receive_from(held->descriptor,ip,&port,buffer,uint32_t(capacity));
    if(size<0)return size==-2?TSUCHI_NOT_READY:TSUCHI_IO;
    held->sender={packed(ip),port};held->heard=true;*out=uint64_t(size);return TSUCHI_OK;
}
TSUCHI_Result sender(TSUCHI_Mod* mod,TSUCHI_Socket socket,TSUCHI_NetAddress* out){
    SocketKept* held=nullptr;const TSUCHI_Result found=socket_of(mod,socket,held);if(found!=TSUCHI_OK)return found;
    if(!held->heard)return TSUCHI_NOT_READY;*out=held->sender;return TSUCHI_OK;
}
TSUCHI_Result local(TSUCHI_Mod* mod,TSUCHI_Socket socket,TSUCHI_NetAddress* out){
    SocketKept* held=nullptr;const TSUCHI_Result found=socket_of(mod,socket,held);if(found!=TSUCHI_OK)return found;
    uint8_t ip[4]{};uint16_t port=0;if(held->transport->local(held->descriptor,ip,&port)!=0)return TSUCHI_IO;
    *out={packed(ip),port};return TSUCHI_OK;
}
TSUCHI_Result close(TSUCHI_Mod* mod,TSUCHI_Socket socket){SocketKept* held=nullptr;const TSUCHI_Result found=socket_of(mod,socket,held);return found==TSUCHI_OK?sockets.release(mod,0,socket.id):found;}
const net::Backend service{status,open,send,receive,sender,local,close,network_numeric_address,network_request_get,network_request_info,network_request_read,network_request_cancel,network_request_release};
}
void set_network_transport(const NetBackend* backend){transport=backend;}
const NetBackend* network_transport(){return transport;}
TSUCHI_Result network_numeric_address(const char* host,uint32_t port,TSUCHI_NetAddress* out){
    if(!host||!*host||port>65535)return TSUCHI_INVALID;
    bool numeric=true;for(const char* p=host;*p;++p)numeric=numeric&&((*p>='0'&&*p<='9')||*p=='.');
    if(!numeric)return TSUCHI_UNSUPPORTED;
    uint32_t address=0;const char* p=host;
    for(uint32_t i=0;i<4;++i){uint32_t n=0,digits=0;while(*p>='0'&&*p<='9'){n=n*10+uint32_t(*p++-'0');if(++digits>3||n>255)return TSUCHI_INVALID;}
        if(!digits)return TSUCHI_INVALID;address=(address<<8)|n;if(i<3){if(*p++!='.')return TSUCHI_INVALID;}else if(*p)return TSUCHI_INVALID;}
    *out={address,port};return TSUCHI_OK;
}
void net_start(){sockets.start();network_requests_start();net::set_backend(&service);}
}
