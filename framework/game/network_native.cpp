// net backend for the Switch/emulator: the game's own nn::socket and
// nn::nifm imports, called through its PLT stubs (native/main_imports.h).
// The game initializes nn::socket at boot (nifm::Initialize, then
// socket::Initialize with a 6 MB pool), so the framework never initializes it.
// Structures follow the SDK's FreeBSD layouts: sockaddr_in carries a length
// byte, and addrinfo keeps ai_canonname before ai_addr (the game reads the
// canonical name at +0x18).
#include "network_transport.h"
#include "game_table.h"
#include "services.h"



#ifndef P4U_NET_IMPORT
#define P4U_NET_IMPORT(type, offset) offset
#endif

namespace {
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using s32=int32_t;
constexpr int AF_INET=2,SOCK_DGRAM=2,IPPROTO_UDP=17,SOL_SOCKET=0xFFFF,SO_BROADCAST=0x20,MSG_DONTWAIT=0x80;
constexpr int EAGAIN_LINUX=11,EAGAIN_BSD=35,ECONNRESET_LINUX=104,ECONNRESET_BSD=54;
struct SockAddrIn {u8 len,family;uint16_t port;u8 addr[4];u8 zero[8];};
static_assert(sizeof(SockAddrIn)==16);
// SDK IPv4 inputs are 16 bytes. Eden 0.2.1's BSD service writes a 0x100-byte
// socket address for GetSockName/RecvFrom, so output storage must accommodate
// either representation. Decode the common IPv4 prefix only.
// https://github.com/eden-emulator/mirror/blob/v0.2.1/src/core/hle/service/sockets/sockets.h
struct SockAddrOutput {SockAddrIn value;u8 padding[240];};
static_assert(sizeof(SockAddrOutput)==256);
bool valid_address(const SockAddrOutput& a,u32 length){
    return length>=sizeof(SockAddrIn)&&length<=sizeof(SockAddrOutput)&&a.value.family==AF_INET;
}
struct AddrInfo {
    s32 flags,family,socktype,protocol;u32 addrlen,pad;
    char* canonname;SockAddrIn* addr;AddrInfo* next;
};
static_assert(sizeof(AddrInfo)==0x30);
uint16_t swap16(uint16_t v){return uint16_t((v>>8)|(v<<8));}
SockAddrIn address(const u8 ip[4],uint16_t port){
    SockAddrIn a{};a.len=sizeof a;a.family=AF_INET;a.port=swap16(port);
    for(int i=0;i<4;++i)a.addr[i]=ip[i];return a;
}

bool available(){return true;}
// UDP connect selects a route without sending a datagram or resolving DNS: the socket's own address is then the one
// this machine sends from toward the internet. Asked again on every query rather than kept after a disconnect.
int32_t route(u8 ip[4]){
    const int fd=P4U_NET_IMPORT(int(*)(int,int,int),tsuchi::game::table::fn::socket_socket)(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(fd<0)return -1;
    const u8 documentation[4]={192,0,2,1};const auto to=address(documentation,9);
    SockAddrOutput self{};u32 length=sizeof self;
    const bool found=P4U_NET_IMPORT(int(*)(int,const SockAddrIn*,u32),tsuchi::game::table::fn::socket_connect)(fd,&to,sizeof to)==0&&
        P4U_NET_IMPORT(int(*)(int,SockAddrIn*,u32*),tsuchi::game::table::fn::socket_get_sock_name)(fd,&self.value,&length)==0&&valid_address(self,length)&&
        self.value.addr[0]!=0&&self.value.addr[0]!=127&&self.value.addr[0]<224;
    P4U_NET_IMPORT(int(*)(int),tsuchi::game::table::fn::socket_close)(fd);
    if(!found)return -1;
    for(int i=0;i<4;++i)ip[i]=self.value.addr[i];
    return 0;
}
bool connected(){
    if(P4U_NET_IMPORT(bool(*)(),tsuchi::game::table::fn::nifm_available)())return true;
    u8 ip[4];
    return route(ip)==0;
}
int32_t udp_open(uint16_t port,bool broadcast){
    const int fd=P4U_NET_IMPORT(int(*)(int,int,int),tsuchi::game::table::fn::socket_socket)(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(fd<0)return -1;
    if(broadcast){
        const int on=1;
        if(P4U_NET_IMPORT(int(*)(int,int,int,const void*,u32),tsuchi::game::table::fn::socket_set_sock_opt)(fd,SOL_SOCKET,SO_BROADCAST,&on,sizeof on)!=0){
            P4U_NET_IMPORT(int(*)(int),tsuchi::game::table::fn::socket_close)(fd);return -1;
        }
    }
    const u8 any[4]{};const SockAddrIn local=address(any,port);
    if(P4U_NET_IMPORT(int(*)(int,const SockAddrIn*,u32),tsuchi::game::table::fn::socket_bind)(fd,&local,sizeof local)!=0){
        P4U_NET_IMPORT(int(*)(int),tsuchi::game::table::fn::socket_close)(fd);return -1;
    }
    // Eden reports a port another program holds (WSAEADDRINUSE) as a successful bind and
    // leaves the socket unbound: check the port the socket really has.
    if(port){
        SockAddrOutput self{};u32 length=sizeof self;
        if(P4U_NET_IMPORT(int(*)(int,SockAddrIn*,u32*),tsuchi::game::table::fn::socket_get_sock_name)(fd,&self.value,&length)!=0||!valid_address(self,length)||swap16(self.value.port)!=port){
            P4U_NET_IMPORT(int(*)(int),tsuchi::game::table::fn::socket_close)(fd);return -3;
        }
    }
    return fd;
}
int32_t send_to(int32_t fd,const uint8_t ip[4],uint16_t port,const void* data,uint32_t size){
    const SockAddrIn to=address(ip,port);
    const long long sent=P4U_NET_IMPORT(long long(*)(int,const void*,u64,int,const SockAddrIn*,u32),tsuchi::game::table::fn::socket_send_to)(fd,data,size,0,&to,sizeof to);
    return sent==(long long)size?0:-1;
}
int32_t receive_from(int32_t fd,uint8_t ip[4],uint16_t* port,void* buffer,uint32_t capacity){
    SockAddrOutput from{};long long got=-1;u32 length=0;
    // A datagram this socket sent earlier that was refused (ICMP port unreachable) comes
    // back as a connection reset on Windows hosts: it says nothing about what waits, so
    // the next datagram is read instead.
    for(int resets=0;resets<32;++resets){
        length=sizeof from;
        got=P4U_NET_IMPORT(long long(*)(int,void*,u64,int,SockAddrIn*,u32*),tsuchi::game::table::fn::socket_recv_from)(fd,buffer,capacity,MSG_DONTWAIT,&from.value,&length);
        if(got>=0)break;
        const int error=P4U_NET_IMPORT(int(*)(),tsuchi::game::table::fn::socket_errno)();
        if(error==EAGAIN_LINUX||error==EAGAIN_BSD)return -2;
        if(error!=ECONNRESET_LINUX&&error!=ECONNRESET_BSD)return -1;
    }
    if(got<0)return -2;
    if(!valid_address(from,length))return -1;
    for(int i=0;i<4;++i)ip[i]=from.value.addr[i];*port=swap16(from.value.port);
    return got>(long long)capacity?int32_t(capacity):int32_t(got);
}
int32_t local(int32_t fd,uint8_t ip[4],uint16_t* port){
    SockAddrOutput self{};u32 length=sizeof self;
    if(P4U_NET_IMPORT(int(*)(int,SockAddrIn*,u32*),tsuchi::game::table::fn::socket_get_sock_name)(fd,&self.value,&length)!=0||!valid_address(self,length))return -1;
    for(int i=0;i<4;++i)ip[i]=self.value.addr[i];*port=swap16(self.value.port);return 0;
}
void close(int32_t fd){P4U_NET_IMPORT(int(*)(int),tsuchi::game::table::fn::socket_close)(fd);}
const tsuchi::NetBackend backend{available,connected,udp_open,send_to,receive_from,local,close,nullptr,route};
}
namespace tsuchi {
void network_native_start(){set_network_transport(&backend);}
}
