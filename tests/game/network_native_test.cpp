#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "network_transport.h"
#include "game_table.h"
namespace tsuchi {const NetBackend* native=nullptr;void set_network_transport(const NetBackend* value){native=value;}}
namespace {
uint16_t bound=0;bool busy=false;int resets=0,last_errno=0;int closed=0;bool empty=false;
bool system_online=false,fail_socket=false,fail_route=false,fail_name=false,bad_family=false;
uint8_t route_ip=0;
uint16_t swap(uint16_t n){return uint16_t((n>>8)|(n<<8));}
int32_t socket(int32_t family,int32_t kind,int32_t protocol){assert(family==2&&kind==2&&protocol==17);return fail_socket?-1:5;}
int32_t connect(int32_t fd,const void* address,uint32_t size){assert(fd==5&&size==16);const auto* a=static_cast<const uint8_t*>(address);assert(a[4]==192&&a[5]==0&&a[6]==2&&a[7]==1);return fail_route?-1:0;}
int32_t close(int32_t fd){assert(fd==5);++closed;return 0;}
int32_t option(int32_t,int32_t,int32_t,const void*,uint32_t){return 0;}
int32_t bind(int32_t,const void* input,uint32_t bytes){assert(bytes==16);const auto* address=static_cast<const uint8_t*>(input);memcpy(&bound,address+2,2);bound=swap(bound);return 0;}
int32_t name(int32_t,void* output,void* length){assert(*static_cast<uint32_t*>(length)==256);if(fail_name)return -1;memset(output,0xaa,256);auto* address=static_cast<uint8_t*>(output);address[0]=16;address[1]=bad_family?1:2;const uint16_t port=swap(busy?0:bound);memcpy(address+2,&port,2);memset(address+4,0,4);address[4]=route_ip;*static_cast<uint32_t*>(length)=256;return 0;}
int64_t send(int32_t,const void*,uint64_t bytes,int32_t,const void* address,uint32_t size){assert(size==16&&static_cast<const uint8_t*>(address)[1]==2);return int64_t(bytes);}
int64_t receive(int32_t,void* output,uint64_t capacity,int32_t flags,void* address,void* length){assert(flags==0x80&&*static_cast<uint32_t*>(length)==256);if(resets){--resets;last_errno=104;return -1;}if(empty){last_errno=35;return -1;}memset(address,0xbb,256);auto* from=static_cast<uint8_t*>(address);from[0]=16;from[1]=2;const uint16_t port=swap(47700);memcpy(from+2,&port,2);from[4]=127;from[5]=0;from[6]=0;from[7]=1;*static_cast<uint32_t*>(length)=256;const uint32_t bytes=capacity<7?uint32_t(capacity):7;memcpy(output,"captain",bytes);return bytes;}
int32_t error(){return last_errno;}bool online(){return system_online;}
}
int main(){
 using namespace tsuchi;namespace f=game::table::fn;f::socket_socket=socket;f::socket_close=close;f::socket_set_sock_opt=option;f::socket_bind=bind;f::socket_get_sock_name=name;f::socket_send_to=send;f::socket_recv_from=receive;f::socket_errno=error;f::socket_connect=connect;f::nifm_available=online;network_native_start();
 assert(native&&native->available()&&!native->connected()&&closed==1);
 route_ip=192;assert(native->connected()&&closed==2); // Eden's false nifm report
 route_ip=0;assert(!native->connected()&&closed==3); // disconnect is not latched
 route_ip=127;assert(!native->connected()&&closed==4); // loopback alone
 route_ip=224;assert(!native->connected()&&closed==5);
 route_ip=192;fail_route=true;assert(!native->connected()&&closed==6);fail_route=false;
 fail_name=true;assert(!native->connected()&&closed==7);fail_name=false;
 bad_family=true;assert(!native->connected()&&closed==8);bad_family=false;
 fail_socket=true;assert(!native->connected()&&closed==8);fail_socket=false;
 system_online=true;assert(native->connected()&&closed==8);system_online=false;closed=0;
 busy=true;assert(native->udp_open(47700,false)==-3&&closed==1);busy=false;assert(native->udp_open(47700,true)==5);uint8_t ip[4]{};uint16_t port=0;assert(native->local(5,ip,&port)==0&&port==47700);char output[16]{};resets=2;assert(native->receive_from(5,ip,&port,output,sizeof(output))==7&&memcmp(output,"captain",7)==0&&ip[0]==127);empty=true;assert(native->receive_from(5,ip,&port,output,sizeof(output))==-2);resets=32;assert(native->receive_from(5,ip,&port,output,sizeof(output))==-2);assert(native->send_to(5,ip,47700,"captain",7)==0);native->close(5);assert(closed==2);
 puts("PASS Eden addresses, busy bind, reset skip, fresh route status and descriptor cleanup");
}
