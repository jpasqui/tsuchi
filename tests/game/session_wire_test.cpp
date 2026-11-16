// session wire contract tests: several machines (Session instances) on a simulated network
// with loss, reordering and broadcast. Run by tests/test_network.py.
#include "session_wire.h"
#include <cstdio>
#include <cstring>

namespace tsuchi::session_wire {
const char* inactive="stopped.mod";
const char* stopping="";   // an owner that stops later
bool session_owner_active(const char* o){return !same(o,inactive)&&!same(o,stopping);}

}
using namespace tsuchi::session_wire;
using tsuchi::NetBackend;

#define CHECK(x) do{if(!(x)){std::printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)

// ---- the network ------------------------------------------------------------------------
namespace net {
struct Socket {bool open=false;int machine=0;uint16_t port=0;};
struct Datagram {int to;uint8_t from_ip[4];uint16_t from_port;uint32_t size;uint8_t data[1500];};
Socket sockets[32];
Datagram* queue=new Datagram[8192];uint32_t queued=0;
// A router in front of a machine lets a datagram in only from an address and port that the receiving
// socket has sent to (join codes punch through it).
bool behind_router[16]{};
struct Contact {uint8_t ip[4];uint16_t port;};
Contact contacts[32][64];uint32_t contact_count[32]{};
uint32_t blocked=0;
void contact(int32_t fd,const uint8_t ip[4],uint16_t port){
    for(uint32_t i=0;i<contact_count[fd];++i)
        if(contacts[fd][i].port==port&&std::memcmp(contacts[fd][i].ip,ip,4)==0)return;
    if(contact_count[fd]<64){Contact& c=contacts[fd][contact_count[fd]++];std::memcpy(c.ip,ip,4);c.port=port;}
}
bool contacted(int32_t fd,const uint8_t ip[4],uint16_t port){
    for(uint32_t i=0;i<contact_count[fd];++i)
        if(contacts[fd][i].port==port&&std::memcmp(contacts[fd][i].ip,ip,4)==0)return true;
    return false;
}
uint32_t loss_every=0,counter=0;bool reorder=false;
uint32_t damage_every=0,duplicate_every=0,damaged=0,duplicated=0;
const char* watch=nullptr;uint32_t watched=0;   // datagrams that carried this text in the clear
uint8_t ip_of(int machine,int i){const uint8_t base[4]{10,0,0,uint8_t(1+machine)};return base[i];}
int32_t open(int machine,uint16_t port){
    if(port)for(auto& s:sockets)if(s.open&&s.machine==machine&&s.port==port)return -3;
    for(int32_t fd=0;fd<32;++fd)if(!sockets[fd].open){sockets[fd]={true,machine,uint16_t(port?port:50000+fd)};return fd;}
    return -1;
}
void deliver(int fd,int target,const void* data,uint32_t size){
    if(loss_every&&++counter%loss_every==0)return;
    if(queued==8192)return;
    {uint8_t from[4];for(int i=0;i<4;++i)from[i]=ip_of(sockets[fd].machine,i);
     if(behind_router[sockets[target].machine]&&!contacted(target,from,sockets[fd].port)){++blocked;return;}}
    if(watch){const size_t k=std::strlen(watch);
     for(uint32_t i=0;i+k<=size;++i)if(std::memcmp(static_cast<const uint8_t*>(data)+i,watch,k)==0){++watched;break;}}
    Datagram& d=queue[queued++];d.to=target;for(int i=0;i<4;++i)d.from_ip[i]=ip_of(sockets[fd].machine,i);
    d.from_port=sockets[fd].port;d.size=size;std::memcpy(d.data,data,size);
    if(damage_every&&counter%damage_every==1&&size>20){d.data[size-1]^=0x40;++damaged;}   // one bit of its last byte
    if(duplicate_every&&counter%duplicate_every==2&&queued<8192){queue[queued]=d;++queued;++duplicated;}
    if(reorder&&queued>=2&&counter%3==0){Datagram t=queue[queued-1];queue[queued-1]=queue[queued-2];queue[queued-2]=t;}
}
int32_t send(int32_t fd,const uint8_t ip[4],uint16_t port,const void* data,uint32_t size){
    if(size>1500)return -1;
    contact(fd,ip,port);
    const bool broadcast=ip[0]==255&&ip[1]==255&&ip[2]==255&&ip[3]==255;
    for(int t=0;t<32;++t){
        if(!sockets[t].open||t==fd||sockets[t].port!=port)continue;
        const bool here=ip[0]==10&&ip[1]==0&&ip[2]==0&&ip[3]==ip_of(sockets[t].machine,3);
        if(broadcast||here)deliver(fd,t,data,size);
    }
    return 0;
}
int32_t receive(int32_t fd,uint8_t ip[4],uint16_t* port,void* buffer,uint32_t capacity){
    for(uint32_t i=0;i<queued;++i){
        if(queue[i].to!=fd)continue;
        Datagram& d=queue[i];const uint32_t n=d.size<capacity?d.size:capacity;
        std::memcpy(buffer,d.data,n);for(int k=0;k<4;++k)ip[k]=d.from_ip[k];*port=d.from_port;
        std::memmove(&queue[i],&queue[i+1],sizeof(Datagram)*(queued-i-1));--queued;
        return int32_t(n);
    }
    return -2;
}
void close(int32_t fd){sockets[fd].open=false;contact_count[fd]=0;for(uint32_t i=0;i<queued;)if(queue[i].to==fd){std::memmove(&queue[i],&queue[i+1],sizeof(Datagram)*(queued-i-1));--queued;}else ++i;}
template<int M> struct Machine {
    static bool available(){return true;}
    static bool connected(){return true;}
    static int32_t udp_open(uint16_t port,bool){return open(M,port);}
    static int32_t local(int32_t fd,uint8_t ip[4],uint16_t* port){for(int i=0;i<4;++i)ip[i]=0;*port=sockets[fd].port;return 0;}
    static int32_t resolve(const char*,uint8_t*){return 1;}
    static constexpr NetBackend backend{available,connected,udp_open,send,receive,local,close,resolve};
    static const NetBackend* get(){return &backend;}
};
WireAddress address(int machine,uint16_t port){WireAddress a{};a.size=sizeof a;for(int i=0;i<4;++i)a.ip[i]=ip_of(machine,i);a.port=port;return a;}
}

// ---- what each machine's mods saw --------------------------------------------------------
struct Seen {
    uint32_t started=0,self=WIRE_PEER_NONE,joined=0,left=0,ended=0,channels=0,last_left=0,last_reason=0;
    uint32_t messages=0,expected=0,out_of_order=0,unreliable=0,from_mask=0;
    char last[64]{};
};
Seen seen[4];
void on_event(void* user,const WireEvent* e){
    Seen& s=*static_cast<Seen*>(user);
    if(e->type==WIRE_SESSION_EVENT_STARTED){++s.started;s.self=e->peer;}
    else if(e->type==WIRE_SESSION_EVENT_PEER_JOINED){++s.joined;}
    else if(e->type==WIRE_SESSION_EVENT_PEER_LEFT){++s.left;s.last_left=e->peer;s.last_reason=e->reason;}
    else if(e->type==WIRE_SESSION_EVENT_ENDED)++s.ended;
    else if(e->type==WIRE_SESSION_EVENT_CHANNELS)++s.channels;
    else if(e->type==WIRE_SESSION_EVENT_MESSAGE){
        s.from_mask|=1u<<e->peer;
        if(!(e->reason&WIRE_SESSION_SEND_RELIABLE)){++s.unreliable;return;}
        uint32_t n=0;std::memcpy(&n,e->data,4);
        if(e->data_size>=8&&std::memcmp(static_cast<const char*>(e->data)+4,"text",4)==0){
            std::memcpy(s.last,static_cast<const char*>(e->data)+4,e->data_size-4<63?e->data_size-4:63);return;
        }
        if(n!=s.expected)++s.out_of_order;
        s.expected=n+1;++s.messages;
    }
}

Session m0{net::Machine<0>::get},m1{net::Machine<1>::get},m2{net::Machine<2>::get},m3{net::Machine<3>::get};
Session* machines[4]{&m0,&m1,&m2,&m3};
double now=0;
void run(double seconds,int count=4,int paused=-1){
    for(double end=now+seconds;now<end;now+=1.0/60){
        for(int i=0;i<count;++i)if(i!=paused)machines[i]->poll(now);
    }
}
WireOptions options(const char* name,uint16_t port=0,uint16_t max=0){
    WireOptions o{};o.size=sizeof o;o.port=port;o.max_peers=max;for(uint32_t i=0;name[i]&&i+1<sizeof o.name;++i)o.name[i]=name[i];return o;
}

// ---- getting through routers -------------------------------------------------------------------
// A STUN server on machine 9 answers each Binding request with the address it came from.
struct Stun {
    int32_t fd=-1;
    uint32_t answered=0;
    bool wrong_number=false;     // answer with another transaction number, which must not be taken
    static void put16(uint8_t* at,uint16_t v){at[0]=uint8_t(v>>8);at[1]=uint8_t(v);}
    static void put32(uint8_t* at,uint32_t v){for(int i=0;i<4;++i)at[i]=uint8_t(v>>(24-8*i));}
    void serve(){
        uint8_t d[64],ip[4];
        uint16_t port=0;
        for(int32_t got;(got=net::receive(fd,ip,&port,d,sizeof d))>=0;){
            if(got!=20||d[0]!=0||d[1]!=1)continue;
            uint8_t a[32]{};
            put16(a,0x0101);
            put16(a+2,12);
            std::memcpy(a+4,d+4,16);
            if(wrong_number)a[19]^=1;
            put16(a+20,0x0020);
            put16(a+22,8);
            a[25]=1;
            put16(a+26,uint16_t(port^0x2112));
            uint32_t address=(uint32_t(ip[0])<<24)|(uint32_t(ip[1])<<16)|(uint32_t(ip[2])<<8)|ip[3];
            put32(a+28,address^0x2112A442u);
            net::send(fd,ip,port,a,32);
            ++answered;
        }
    }
};

int traversal(){
    Stun stun;
    stun.fd=net::open(9,3478);
    const WireAddress at_stun=net::address(9,3478);
    auto run_with_stun=[&](double seconds){
        for(double end=now+seconds;now<end;now+=1.0/60){
            m0.poll(now);
            m1.poll(now);
            stun.serve();
        }
    };
    net::behind_router[0]=net::behind_router[1]=true;
    const WireOptions host_options=options("Olimar"),guest_options=options("Oatchi");
    // Without a session there is nothing to ask for.
    {WireAddress a{};a.size=sizeof a;CHECK(m0.public_address(&a)==TSUCHI_NOT_FOUND);}
    CHECK(m0.discover("a.mod",&at_stun,now)==TSUCHI_NOT_FOUND);
    CHECK(m0.host("a.mod",&host_options,now)==TSUCHI_OK);
    {WireAddress a{};a.size=sizeof a;CHECK(m0.public_address(&a)==TSUCHI_NOT_FOUND);}
    {WireAddress bad=at_stun;bad.port=0;CHECK(m0.discover("a.mod",&bad,now)==TSUCHI_INVALID);}
    CHECK(m0.discover("b.mod",&at_stun,now)==TSUCHI_NOT_FOUND);
    // An answer with another transaction number is not taken.
    stun.wrong_number=true;
    CHECK(m0.discover("a.mod",&at_stun,now)==TSUCHI_OK);
    run_with_stun(1);
    {WireAddress a{};a.size=sizeof a;CHECK(stun.answered>0&&m0.public_address(&a)==TSUCHI_NOT_READY);}
    // The host learns its public address, from its own socket's port.
    stun.wrong_number=false;
    CHECK(m0.discover("a.mod",&at_stun,now)==TSUCHI_OK);
    run_with_stun(0.5);
    WireAddress host_public{};host_public.size=sizeof host_public;
    CHECK(m0.public_address(&host_public)==TSUCHI_OK&&host_public.ip[3]==1&&host_public.port==WIRE_SESSION_DEFAULT_PORT);
    // Behind the host's router, a guest's joins do not get in.
    const uint32_t blocked_before=net::blocked;
    CHECK(m1.join("a.mod",&host_public,&guest_options,now)==TSUCHI_OK);
    run_with_stun(2);
    {WireInfo i{};i.size=sizeof i;CHECK(m1.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINING&&net::blocked>blocked_before);}
    // The guest learns its own public address; the host punches toward it, and the guest gets in.
    CHECK(m1.discover("a.mod",&at_stun,now)==TSUCHI_OK);
    run_with_stun(0.5);
    WireAddress guest_public{};guest_public.size=sizeof guest_public;
    CHECK(m1.public_address(&guest_public)==TSUCHI_OK&&guest_public.ip[3]==2&&guest_public.port!=0);
    CHECK(m1.punch("a.mod",&host_public,now)==TSUCHI_NOT_FOUND);     // only a host punches
    {WireAddress bad=guest_public;bad.port=0;CHECK(m0.punch("a.mod",&bad,now)==TSUCHI_INVALID);}
    const uint32_t joined=seen[1].started,messages=seen[0].messages;
    CHECK(m0.punch("a.mod",&guest_public,now)==TSUCHI_OK);
    run_with_stun(2);
    {WireInfo i{};i.size=sizeof i;CHECK(m1.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINED&&i.self==1);}
    CHECK(seen[1].started==joined+1);
    // Messages go both ways directly; the STUN server hears nothing more.
    const uint32_t answered=stun.answered;
    for(uint32_t k=0;k<20;++k){
        uint8_t m[4];
        const uint32_t n=seen[0].expected+k;
        std::memcpy(m,&n,4);
        CHECK(m1.send("a.mod",0,m,4,WIRE_SESSION_SEND_RELIABLE,now)==TSUCHI_OK);
    }
    run_with_stun(10);
    CHECK(seen[0].messages==messages+20&&stun.answered==answered);
    {WireInfo i{};i.size=sizeof i;CHECK(m1.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINED);}
    CHECK(m1.leave("a.mod",now)==TSUCHI_OK);
    // A STUN server that never answers is given up on.
    const WireAddress nobody=net::address(8,3478);
    CHECK(m0.discover("a.mod",&nobody,now)==TSUCHI_OK);
    run_with_stun(6);
    {WireAddress a{};a.size=sizeof a;CHECK(m0.public_address(&a)==TSUCHI_IO);}
    CHECK(m0.leave("a.mod",now)==TSUCHI_OK);
    {WireAddress a{};a.size=sizeof a;CHECK(m0.public_address(&a)==TSUCHI_NOT_FOUND);}
    net::behind_router[0]=net::behind_router[1]=false;
    net::close(stun.fd);
    return 0;
}

// ---- what one packet can do ----------------------------------------------------------------------
// A raw JOIN from a socket that never answers: the host does not keep its slot or stream keep-alives at it.
int admission(){
    const WireOptions host_options=options("Olimar");
    CHECK(m0.host("a.mod",&host_options,now)==TSUCHI_OK);
    const int32_t raw=net::open(2,0);
    const WireAddress at_host=net::address(0,WIRE_SESSION_DEFAULT_PORT);
    auto join=[&](uint32_t instance){
        uint8_t d[32]{};const uint32_t magic=SESSION_MAGIC;const uint16_t protocol=SESSION_PROTOCOL;
        std::memcpy(d,&magic,4);std::memcpy(d+4,&protocol,2);d[6]=1;d[7]=255;std::memcpy(d+12,&instance,4);
        d[16]=0;d[17]=0;   // no name, no channels
        net::send(raw,at_host.ip,at_host.port,d,18);
    };
    auto drain=[&](){uint32_t n=0;uint8_t ip[4];uint16_t port;uint8_t b[1500];while(net::receive(raw,ip,&port,b,sizeof b)>=0)++n;return n;};
    auto peers=[&](){WireInfo i{};i.size=sizeof i;m0.info(&i,now);return i.peers;};
    join(77);
    for(double end=now+0.5;now<end;now+=1.0/60)m0.poll(now);
    CHECK(peers()==3u);                                   // it took the guest slot for now
    const uint32_t sent_early=drain();
    CHECK(sent_early>=1&&sent_early<=3);                  // a welcome and a keep-alive, not one every 0.1 s
    // The same JOIN again does not keep the slot: five seconds after the first, it is free.
    for(double end=now+3;now<end;now+=1.0/60){m0.poll(now);}
    join(77);
    for(double end=now+3;now<end;now+=1.0/60){m0.poll(now);}
    CHECK(peers()==1u);
    CHECK(drain()<=10);
    // A REFUSE from a stranger does not change what a joining guest was told.
    {const WireAddress at_somewhere=net::address(3,4000);
     CHECK(m1.join("a.mod",&at_somewhere,nullptr,now)==TSUCHI_OK);
     uint8_t d[17]{};const uint32_t magic=SESSION_MAGIC;const uint16_t protocol=SESSION_PROTOCOL;
     std::memcpy(d,&magic,4);std::memcpy(d+4,&protocol,2);d[6]=3;d[7]=255;d[16]=WIRE_SESSION_REFUSED_FULL;
     m1.poll(now);
     WireInfo joining{};joining.size=sizeof joining;CHECK(m1.info(&joining,now)==TSUCHI_OK&&joining.port!=0);
     const WireAddress at_guest=net::address(1,joining.port);
     net::send(raw,at_guest.ip,at_guest.port,d,17);
     m1.poll(now);
     WireInfo i{};i.size=sizeof i;CHECK(m1.info(&i,now)==TSUCHI_OK&&i.refused==0);
     CHECK(m1.leave("a.mod",now)==TSUCHI_OK);}
    net::close(raw);
    CHECK(m0.leave("a.mod",now)==TSUCHI_OK);
    return 0;
}

// ---- secured sessions -----------------------------------------------------------------------------
// A sealed datagram longer than any this protocol sends, from someone without the secret, is dropped before it is
// opened: a JOIN to the host, a WELCOME to a joining guest, and a link datagram from the host's own address.
// Machine 0 hosts with a secret and machine 1 has joined it.
int oversized(const WireAddress& at_host){
    auto fd_of=[](int machine,uint16_t port){for(int32_t fd=0;fd<32;++fd)if(net::sockets[fd].open&&net::sockets[fd].machine==machine&&net::sockets[fd].port==port)return fd;return -1;};
    auto big=[](uint8_t* d,uint8_t type,const uint8_t* header){
        std::memset(d,0xA5,1500);
        if(header)std::memcpy(d,header,SESSION_HEADER_BYTES);
        else{const uint32_t magic=SESSION_MAGIC;const uint16_t protocol=SESSION_PROTOCOL;std::memcpy(d,&magic,4);std::memcpy(d+4,&protocol,2);d[7]=255;}
        d[6]=uint8_t(type|SESSION_SEALED);
    };
    uint8_t d[1500];
    // To the host.
    {const int32_t raw=net::open(2,0);
     big(d,1,nullptr);net::send(raw,at_host.ip,at_host.port,d,sizeof d);
     m0.poll(now);
     WireInfo i{};i.size=sizeof i;CHECK(m0.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_HOSTING&&i.peers==3u);
     uint8_t ip[4];uint16_t port;uint8_t reply[1500];CHECK(net::receive(raw,ip,&port,reply,sizeof reply)<0);
     net::close(raw);}
    // To a guest on its way in.
    {const WireAddress at_somewhere=net::address(3,4000);
     uint8_t secret[SESSION_SECRET_BYTES];for(uint32_t k=0;k<SESSION_SECRET_BYTES;++k)secret[k]=uint8_t(k*13+1);
     CHECK(m2.set_secret("a.mod",secret,SESSION_SECRET_BYTES)==TSUCHI_OK);
     const WireOptions o=options("Moss");
     CHECK(m2.join("a.mod",&at_somewhere,&o,now)==TSUCHI_OK);
     m2.poll(now);
     WireInfo joining{};joining.size=sizeof joining;CHECK(m2.info(&joining,now)==TSUCHI_OK&&joining.port!=0);
     const int32_t raw=net::open(3,4000);
     const WireAddress at_guest=net::address(2,joining.port);
     big(d,2,nullptr);net::send(raw,at_guest.ip,at_guest.port,d,sizeof d);
     m2.poll(now);
     WireInfo i{};i.size=sizeof i;CHECK(m2.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINING&&i.self==WIRE_PEER_NONE&&i.refused==0);
     net::close(raw);
     CHECK(m2.leave("a.mod",now)==TSUCHI_OK);}
    // On the link, with the header of one of the host's own datagrams to the guest.
    {WireInfo g{};g.size=sizeof g;CHECK(m1.info(&g,now)==TSUCHI_OK&&g.state==WIRE_SESSION_JOINED);
     const int32_t host_fd=fd_of(0,at_host.port),guest_fd=fd_of(1,g.port);
     CHECK(host_fd>=0&&guest_fd>=0);
     uint8_t header[SESSION_HEADER_BYTES]{};bool found=false;
     for(double end=now+1;now<end&&!found;now+=1.0/60){
         m0.poll(now);
         for(uint32_t k=0;k<net::queued&&!found;++k)
             if(net::queue[k].to==guest_fd&&net::queue[k].size>=SESSION_HEADER_BYTES){std::memcpy(header,net::queue[k].data,SESSION_HEADER_BYTES);found=true;}
     }
     CHECK(found);
     const uint32_t messages=seen[1].messages,unreliable=seen[1].unreliable,left=seen[1].left;
     big(d,4,header);net::deliver(host_fd,guest_fd,d,sizeof d);
     big(d,5,header);net::deliver(host_fd,guest_fd,d,sizeof d);
     run(0.5,2);
     WireInfo i{};i.size=sizeof i;
     CHECK(m1.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINED&&i.self==1&&i.id==g.id);
     CHECK(seen[1].messages==messages&&seen[1].unreliable==unreliable&&seen[1].left==left);}
    return 0;
}

int secured(){
    uint8_t secret[SESSION_SECRET_BYTES],other[SESSION_SECRET_BYTES];
    for(uint32_t i=0;i<SESSION_SECRET_BYTES;++i){secret[i]=uint8_t(i*7+3);other[i]=uint8_t(i*11+5);}
    CHECK(m0.set_secret("a.mod",secret,31)==TSUCHI_INVALID);
    CHECK(m0.set_secret("a.mod",secret,SESSION_SECRET_BYTES)==TSUCHI_OK);
    CHECK(m1.set_secret("a.mod",secret,SESSION_SECRET_BYTES)==TSUCHI_OK);
    CHECK(m2.set_secret("a.mod",other,SESSION_SECRET_BYTES)==TSUCHI_OK);
    const WireOptions host_options=options("Olimar"),guest_options=options("Oatchi");
    const WireAddress at_host=net::address(0,WIRE_SESSION_DEFAULT_PORT);
    CHECK(m0.host("a.mod",&host_options,now)==TSUCHI_OK&&m0.secured());
    // A guest with another secret, and one with none, get nothing back: not a WELCOME, not a REFUSE.
    CHECK(m2.join("a.mod",&at_host,&guest_options,now)==TSUCHI_OK);
    CHECK(m3.join("a.mod",&at_host,&guest_options,now)==TSUCHI_OK&&!m3.secured());
    for(double end=now+2;now<end;now+=1.0/60){m0.poll(now);m2.poll(now);m3.poll(now);}
    {WireInfo i{};i.size=sizeof i;CHECK(m2.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINING&&i.refused==0);}
    {WireInfo i{};i.size=sizeof i;CHECK(m3.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINING&&i.refused==0);}
    {WireInfo i{};i.size=sizeof i;CHECK(m0.info(&i,now)==TSUCHI_OK&&i.peers==1u);}
    CHECK(m2.leave("a.mod",now)==TSUCHI_OK&&m3.leave("a.mod",now)==TSUCHI_OK);
    // The same secret joins. Its JOIN and WELCOME carry the players' names, sealed.
    net::watch="Oatchi";net::watched=0;
    CHECK(m1.join("a.mod",&at_host,&guest_options,now)==TSUCHI_OK&&m1.secured());
    run(1,2);
    CHECK(net::watched==0);
    net::watch=nullptr;
    {WireInfo i{};i.size=sizeof i;CHECK(m1.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINED&&i.self==1);}
    {uint32_t v=0;CHECK(m0.peer_channel(1,"a.mod",&v)==TSUCHI_OK&&v==3);}
    if(oversized(at_host))return 1;
    // Reliable messages arrive once and in order through loss, damage and duplication; damaged and repeated
    // datagrams are dropped, never delivered.
    net::loss_every=9;net::damage_every=7;net::duplicate_every=5;
    const uint32_t first=seen[0].expected,messages=seen[0].messages,disorder=seen[0].out_of_order;
    for(uint32_t k=0;k<200;++k){
        uint8_t m[4];const uint32_t n=first+k;std::memcpy(m,&n,4);
        CHECK(m1.send("a.mod",0,m,4,WIRE_SESSION_SEND_RELIABLE,now)==TSUCHI_OK);
        if(k%20==0)run(0.05,2);
    }
    run(6,2);
    CHECK(seen[0].messages==messages+200&&seen[0].out_of_order==disorder);
    CHECK(net::damaged>0&&net::duplicated>0);
    // Unreliable messages that arrive twice are delivered once.
    net::loss_every=0;net::damage_every=0;
    const uint32_t unreliable=seen[1].unreliable;
    for(uint32_t k=0;k<20;++k){const char m[]="tick";CHECK(m0.send("a.mod",1,m,4,0,now)==TSUCHI_OK);run(0.05,2);}
    run(0.5,2);
    CHECK(seen[1].unreliable==unreliable+20);
    net::duplicate_every=0;
    CHECK(m1.leave("a.mod",now)==TSUCHI_OK&&m0.leave("a.mod",now)==TSUCHI_OK);
    // The secret is the starter's own: another mod's session starts open, and none set gives an open session.
    CHECK(m0.set_secret("a.mod",nullptr,0)==TSUCHI_OK);
    CHECK(m0.host("a.mod",&host_options,now)==TSUCHI_OK&&!m0.secured());
    // The tap does see an open session's names, so it would have seen a sealed one's.
    CHECK(m1.set_secret("a.mod",nullptr,0)==TSUCHI_OK);
    net::watch="Oatchi";net::watched=0;
    CHECK(m1.join("a.mod",&at_host,&guest_options,now)==TSUCHI_OK);
    run(1,2);
    CHECK(net::watched>0);
    net::watch=nullptr;
    CHECK(m1.leave("a.mod",now)==TSUCHI_OK&&m0.leave("a.mod",now)==TSUCHI_OK);
    CHECK(m2.set_secret("a.mod",nullptr,0)==TSUCHI_OK);
    return 0;
}

int main(){
    // Subscriptions: one channel per mod.
    uint32_t subs[4][3]{};
    for(int m=0;m<4;++m)CHECK(machines[m]->subscribe("a.mod",3,on_event,&seen[m],&subs[m][0])==TSUCHI_OK);
    {uint32_t x=9;CHECK(m0.subscribe("a.mod",3,on_event,&seen[0],&x)==TSUCHI_BUSY&&x==0);}
    CHECK(m0.subscribe("Bad",1,on_event,nullptr,&subs[0][1])==TSUCHI_INVALID);
    CHECK(m0.subscribe(inactive,1,on_event,nullptr,&subs[0][1])==TSUCHI_NOT_READY);

    // Arguments.
    {WireOptions bad=options("x");bad.size=1;CHECK(m0.host("a.mod",&bad,now)==TSUCHI_INVALID);}
    {WireOptions bad=options("x",0,1);CHECK(m0.host("a.mod",&bad,now)==TSUCHI_INVALID);}
    {WireAddress a=net::address(0,0);CHECK(m1.join("a.mod",&a,nullptr,now)==TSUCHI_INVALID);}
    {const char data[]="x";CHECK(m0.send("a.mod",1,data,1,WIRE_SESSION_SEND_RELIABLE,now)==TSUCHI_NOT_READY);}
    CHECK(m0.leave("a.mod",now)==TSUCHI_NOT_FOUND);

    // Host, and a guest joining by address.
    const WireOptions host_options=options("Olimar"),guest_options=options("Oatchi");
    CHECK(m0.host("a.mod",&host_options,now)==TSUCHI_OK&&m0.host("a.mod",&host_options,now)==TSUCHI_BUSY);
    {Session other{net::Machine<0>::get};CHECK(other.host("a.mod",nullptr,now)==TSUCHI_BUSY);}   // the port is taken
    const WireAddress at_host=net::address(0,WIRE_SESSION_DEFAULT_PORT);
    CHECK(m1.join("a.mod",&at_host,&guest_options,now)==TSUCHI_OK);
    {WireInfo i{};i.size=sizeof i;CHECK(m1.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINING&&i.self==WIRE_PEER_NONE);}
    run(0.5,2);
    CHECK(seen[0].started==1&&seen[0].self==0&&seen[0].joined==1);
    CHECK(seen[1].started==1&&seen[1].self==1&&seen[1].joined==1);
    {WireInfo i{};i.size=sizeof i;CHECK(m1.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINED&&i.self==1&&i.peers==3u&&i.id&&std::strcmp(i.owner,"a.mod")==0);}
    {WirePeer p{};p.size=sizeof p;CHECK(m1.peer(0,&p,now)==TSUCHI_OK&&std::strcmp(p.name,"Olimar")==0&&p.address.ip[3]==1&&p.round_trip>=0);}
    {WirePeer p{};p.size=sizeof p;CHECK(m0.peer(1,&p,now)==TSUCHI_OK&&std::strcmp(p.name,"Oatchi")==0&&p.heard<0.1f);}
    {uint32_t v=0;CHECK(m0.peer_channel(1,"a.mod",&v)==TSUCHI_OK&&v==3&&m0.peer_channel(1,"b.mod",&v)==TSUCHI_NOT_FOUND);}
    {uint32_t v=0;CHECK(m1.peer_channel(0,"a.mod",&v)==TSUCHI_OK&&v==3&&m1.peer_channel(2,"a.mod",&v)==TSUCHI_NOT_FOUND);}
    // The shared clock: the guest started later but reads the host's time.
    {WireInfo h{},g{};h.size=g.size=sizeof h;m0.info(&h,now);m1.info(&g,now);CHECK(g.time>h.time-0.05f&&g.time<h.time+0.05f);}

    // Reliable messages both ways, through loss and reordering: each once, in order.
    net::loss_every=5;net::reorder=true;
    for(uint32_t n=0;n<600;++n){
        uint8_t m[40]{};std::memcpy(m,&n,4);
        while(m0.send("a.mod",1,m,sizeof m,WIRE_SESSION_SEND_RELIABLE,now)==TSUCHI_CAPACITY)run(0.05,2);
        while(m1.send("a.mod",0,m,sizeof m,WIRE_SESSION_SEND_RELIABLE,now)==TSUCHI_CAPACITY)run(0.05,2);
        if(n%50==0)run(1.0/60,2);
    }
    run(3,2);
    CHECK(seen[1].messages==600&&seen[1].out_of_order==0&&seen[0].messages==600&&seen[0].out_of_order==0);
    net::loss_every=0;net::reorder=false;
    // Unreliable, and the largest message.
    {uint8_t big[WIRE_SESSION_MAX_MESSAGE]{};CHECK(m0.send("a.mod",1,big,sizeof big,0,now)==TSUCHI_OK);
     CHECK(m0.send("a.mod",1,big,sizeof big+1,0,now)==TSUCHI_INVALID);run(0.1,2);CHECK(seen[1].unreliable==1);}
    CHECK(m0.send("a.mod",0,"x",1,0,now)==TSUCHI_INVALID&&m0.send("a.mod",3,"x",1,0,now)==TSUCHI_NOT_FOUND);
    CHECK(m0.send("b.mod",1,"x",1,0,now)==TSUCHI_BUSY);   // no channel

    // Two more guests, one finding the host by broadcast; guests reach each other through it.
    const WireOptions third=options("Moss"),fourth=options("Louie");
    CHECK(m2.join("a.mod",nullptr,&third,now)==TSUCHI_OK);
    CHECK(m3.join("a.mod",&at_host,&fourth,now)==TSUCHI_OK);
    run(1);
    CHECK(seen[2].self==2&&seen[3].self==3&&seen[1].joined==3&&seen[2].joined==3&&seen[3].joined==3&&seen[0].joined==3);
    {WirePeer p{};p.size=sizeof p;CHECK(m2.peer(3,&p,now)==TSUCHI_OK&&std::strcmp(p.name,"Louie")==0&&p.address.port==0);}
    {uint32_t v=0;CHECK(m2.peer_channel(1,"a.mod",&v)==TSUCHI_OK&&v==3);}
    {char m[16]="0000text-2to3";CHECK(m2.send("a.mod",3,m,14,WIRE_SESSION_SEND_RELIABLE,now)==TSUCHI_OK);run(0.2);
     CHECK(std::strcmp(seen[3].last,"text-2to3")==0&&seen[3].from_mask==(1u<<2));}
    {char m[16]="0000text-all";seen[0].last[0]=seen[1].last[0]=seen[3].last[0]=0;
     CHECK(m2.send("a.mod",WIRE_PEER_ALL,m,13,WIRE_SESSION_SEND_RELIABLE,now)==TSUCHI_OK);run(0.2);
     CHECK(std::strcmp(seen[0].last,"text-all")==0&&std::strcmp(seen[1].last,"text-all")==0&&std::strcmp(seen[3].last,"text-all")==0);}
    // The host is full: a fifth machine is refused.
    {Session fifth{net::Machine<3>::get};uint32_t s=0;fifth.subscribe("a.mod",3,on_event,&seen[3],&s);
     const WireOptions o=options("Extra",0,0);
     // (machine 3 already has a socket; the fifth gets its own port)
     CHECK(fifth.join("a.mod",&at_host,&o,now)==TSUCHI_OK);
     for(int i=0;i<30;++i){fifth.poll(now);run(1.0/60);}
     WireInfo f{};f.size=sizeof f;CHECK(fifth.info(&f,now)==TSUCHI_OK&&f.state==WIRE_SESSION_JOINING&&f.refused==WIRE_SESSION_REFUSED_FULL);
     CHECK(fifth.leave("a.mod",now)==TSUCHI_OK);}

    // Channels: a mod starting on one guest is seen by all.
    const uint32_t channels_before=seen[0].channels;
    CHECK(m1.subscribe("b.mod",7,on_event,&seen[1],&subs[1][1])==TSUCHI_OK);
    run(0.3);
    {uint32_t v=0;CHECK(m0.peer_channel(1,"b.mod",&v)==TSUCHI_OK&&v==7&&m3.peer_channel(1,"b.mod",&v)==TSUCHI_OK&&v==7);}
    CHECK(seen[0].channels>channels_before);
    CHECK(m1.unsubscribe(subs[1][1])==TSUCHI_OK&&m1.unsubscribe(subs[1][1])==TSUCHI_NOT_FOUND);
    run(0.3);
    {uint32_t v=0;CHECK(m0.peer_channel(1,"b.mod",&v)==TSUCHI_NOT_FOUND);}

    // A mod starting on the host later: guests see its channel and get its messages.
    {uint32_t late=0;CHECK(m0.subscribe("late.mod",4,on_event,&seen[0],&late)==TSUCHI_OK);
     uint32_t g=0;CHECK(m2.subscribe("late.mod",4,on_event,&seen[2],&g)==TSUCHI_OK);
     run(0.3);
     uint32_t v=0;CHECK(m1.peer_channel(0,"late.mod",&v)==TSUCHI_OK&&v==4);
     CHECK(m2.peer_channel(0,"late.mod",&v)==TSUCHI_OK&&v==4);
     char m[16]="0000text-late";seen[2].last[0]=0;
     CHECK(m0.send("late.mod",2,m,14,WIRE_SESSION_SEND_RELIABLE,now)==TSUCHI_OK);run(0.2);
     CHECK(std::strcmp(seen[2].last,"text-late")==0);
     seen[0].last[0]=0;CHECK(m2.send("late.mod",0,m,14,WIRE_SESSION_SEND_RELIABLE,now)==TSUCHI_OK);run(0.2);
     CHECK(std::strcmp(seen[0].last,"text-late")==0);
     CHECK(m0.unsubscribe(late)==TSUCHI_OK&&m2.unsubscribe(g)==TSUCHI_OK);run(0.2);}
    // A guest leaves; the others hear it.
    CHECK(m3.leave("a.mod",now)==TSUCHI_OK);
    run(0.3);
    CHECK(seen[3].ended==1&&seen[0].last_left==3&&seen[0].last_reason==WIRE_SESSION_LEFT_BYE&&seen[2].last_left==3);
    // A guest about to load says so on its keep-alives; the host waits past the timeout while it holds its thread.
    {const uint32_t left0=seen[0].left,left1=seen[1].left,left2=seen[2].left;
     for(int k=0;k<20;++k){now+=1.0/60;m0.poll(now);m1.poll(now,true);m2.poll(now);}
     run(WIRE_SESSION_TIMEOUT+10,3,1);
     CHECK(seen[0].left==left0&&seen[2].left==left2);
     run(1,3);
     WireInfo i{};i.size=sizeof i;
     CHECK(m1.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINED&&i.self==1&&seen[0].left==left0&&seen[1].left==left1);}
    // As in the game: the scene change is flagged for a moment, ordinary frames follow, and only then the thread
    // is held. The guest keeps saying it is busy after the flag clears, so the host still waits.
    {const uint32_t left0=seen[0].left,left2=seen[2].left;
     for(int k=0;k<24;++k){now+=1.0/60;m0.poll(now);m1.poll(now,true);m2.poll(now);}
     for(int k=0;k<60;++k){now+=1.0/60;m0.poll(now);m1.poll(now);m2.poll(now);}
     run(WIRE_SESSION_TIMEOUT+10,3,1);
     CHECK(seen[0].left==left0&&seen[2].left==left2);
     run(1,3);
     WireInfo i{};i.size=sizeof i;
     CHECK(m1.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINED&&i.self==1&&seen[0].left==left0);}
    // Long after the flag, a guest that stops answering is not waited for.
    run(31,3);
    // Everyone holds its thread at once (one shared load). Back, nobody counts its own silence against the others.
    {const uint32_t left0=seen[0].left,left1=seen[1].left,left2=seen[2].left;
     now+=WIRE_SESSION_TIMEOUT+10;
     run(1,3);
     CHECK(seen[0].left==left0&&seen[1].left==left1&&seen[2].left==left2);}
    // A guest that stops answering times out at the host, and joins again when it comes back.
    run(WIRE_SESSION_TIMEOUT+1,3,1);
    CHECK(seen[0].last_left==1&&seen[0].last_reason==WIRE_SESSION_LEFT_TIMEOUT&&seen[2].last_left==1);
    const uint32_t started_before=seen[1].started;
    run(1,3);
    CHECK(seen[1].started==started_before+1&&seen[1].self==1);   // told at once (BYE), joined again
    // The host leaves: guests go back to looking for it.
    CHECK(m0.leave("a.mod",now)==TSUCHI_OK);
    run(0.3,3);
    {WireInfo i{};i.size=sizeof i;CHECK(m1.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINING&&seen[1].last_reason==WIRE_SESSION_LEFT_BYE);}
    CHECK(m0.host("a.mod",&host_options,now)==TSUCHI_OK);
    run(1,3);
    {WireInfo i{};i.size=sizeof i;CHECK(m2.info(&i,now)==TSUCHI_OK&&i.state==WIRE_SESSION_JOINED);}   // found it again by broadcast

    // Its starter stopping ends the session.
    CHECK(m1.leave("a.mod",now)==TSUCHI_OK&&m2.leave("a.mod",now)==TSUCHI_OK&&m0.leave("a.mod",now)==TSUCHI_OK);
    {Session s{net::Machine<2>::get};uint32_t x=0;
     CHECK(s.host(inactive,nullptr,now)==TSUCHI_NOT_READY);
     CHECK(s.subscribe("c.mod",1,on_event,&seen[0],&x)==TSUCHI_OK&&s.subscribe("d.mod",1,on_event,&seen[2],&x)==TSUCHI_OK);
     const uint32_t ended=seen[2].ended;
     CHECK(s.host("c.mod",nullptr,now)==TSUCHI_OK);s.poll(now);
     WireInfo i{};i.size=sizeof i;s.info(&i,now);CHECK(i.state==WIRE_SESSION_HOSTING);
     stopping="c.mod";s.poll(now);CHECK(seen[2].ended==ended+1);   // (its own callback does not run once stopped)
     s.info(&i,now);CHECK(i.state==WIRE_SESSION_NONE);stopping="";}

    // Another protocol is refused.
    {CHECK(m0.host("a.mod",&host_options,now)==TSUCHI_OK);
     const int32_t raw=net::open(2,0);uint8_t d[64]{};const uint32_t magic=SESSION_MAGIC;const uint16_t protocol=SESSION_PROTOCOL+1;
     std::memcpy(d,&magic,4);std::memcpy(d+4,&protocol,2);d[6]=1;d[7]=255;
     const WireAddress a=net::address(0,WIRE_SESSION_DEFAULT_PORT);net::send(raw,a.ip,a.port,d,18);
     m0.poll(now);uint8_t ip[4];uint16_t port;uint8_t reply[64];
     const int32_t got=net::receive(raw,ip,&port,reply,sizeof reply);
     CHECK(got==17&&reply[6]==3&&reply[16]==WIRE_SESSION_REFUSED_VERSION);
     net::close(raw);CHECK(m0.leave("a.mod",now)==TSUCHI_OK);}

    if(traversal())return 1;
    if(admission())return 1;
    if(secured())return 1;
    std::printf("PASS session wire contracts (%u messages each way through loss and reordering)\n",seen[1].messages);
    return 0;
}
