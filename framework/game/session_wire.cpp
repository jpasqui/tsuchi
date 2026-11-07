#include "session_wire.h"
// session core (session_core.h has the wire format). Freestanding.
namespace tsuchi::session_wire {
namespace {
enum : uint8_t {T_JOIN=1,T_WELCOME=2,T_REFUSE=3,T_LINK=4,T_DATA=5,T_BYE=6};
enum : uint8_t {C_PEER=1,C_CHANNELS=2};
constexpr uint32_t HEADER=16,MESSAGE_HEADER=6,ENTRY_HEADER=6;
constexpr double KEEP_EVERY=0.1,JOIN_EVERY=0.5;
// A peer that says it is busy loading is waited for this long. A machine that did not poll for longer than
// STALL_AFTER was stopped itself (a load holds the game's thread), so it does not count that time against others.
// The game says it changes scenes for a moment, then holds its thread for the load after it (seen in Eden: the
// flag for 0.4 s, then stalls of 7 to 10 s with it clear). A machine keeps saying it is busy for BUSY_HOLD after
// the game last said so, and a peer that heard it waits BUSY_GRACE past its last busy keep-alive.
constexpr double BUSY_GRACE=90,BUSY_HOLD=30,STALL_AFTER=2;
constexpr uint8_t LINK_BUSY=1;
constexpr uint32_t RECEIVE_BUDGET=512,SEND_BUDGET=64;
constexpr uint32_t OP_SEND=1u,OP_QUERY=2u;
constexpr uint8_t BROADCAST[4]{255,255,255,255};

bool newer(uint32_t a,uint32_t b){return int32_t(a-b)>0;}
void bytes_copy(uint8_t* to,const uint8_t* from,uint32_t n){for(uint32_t i=0;i<n;++i)to[i]=from[i];}
bool same_ip(const uint8_t a[4],const uint8_t b[4]){return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2]&&a[3]==b[3];}

struct W {
    uint8_t* d;uint32_t cap,n=0;bool ok=true;
    W(uint8_t* data,uint32_t capacity,uint32_t at=0):d(data),cap(capacity),n(at){}
    void byte(uint8_t v){if(n+1>cap){ok=false;return;}d[n++]=v;}
    void u16(uint16_t v){byte(uint8_t(v));byte(uint8_t(v>>8));}
    void u32(uint32_t v){for(int i=0;i<4;++i)byte(uint8_t(v>>(8*i)));}
    void f32(float v){uint32_t b;__builtin_memcpy(&b,&v,4);u32(b);}
    void raw(const uint8_t* s,uint32_t k){if(n+k>cap){ok=false;return;}bytes_copy(d+n,s,k);n+=k;}
    void str(const char* s){uint32_t k=0;while(s[k]&&k<WIRE_SESSION_NAME_BYTES-1)++k;byte(uint8_t(k));raw(reinterpret_cast<const uint8_t*>(s),k);}
};
struct R {
    const uint8_t* d;uint32_t n,at=0;bool ok=true;
    R(const uint8_t* data,uint32_t size):d(data),n(size){}
    uint8_t byte(){if(at+1>n){ok=false;return 0;}return d[at++];}
    uint16_t u16(){uint16_t v=byte();v|=uint16_t(byte())<<8;return v;}
    uint32_t u32(){uint32_t v=0;for(int i=0;i<4;++i)v|=uint32_t(byte())<<(8*i);return v;}
    float f32(){const uint32_t b=u32();float v;__builtin_memcpy(&v,&b,4);return v;}
    // A name into out (terminated, cut to fit).
    void str(char* out){
        const uint8_t k=byte();uint32_t w=0;
        for(uint8_t i=0;i<k;++i){const uint8_t c=byte();if(w+1<WIRE_SESSION_NAME_BYTES)out[w++]=char(c);}
        out[w]=0;
    }
    const uint8_t* take(uint32_t k){if(at+k>n){ok=false;return nullptr;}const uint8_t* p=d+at;at+=k;return p;}
};
bool finite(float v){return v==v&&v-v==0.0f;}
}

uint32_t session_channel(const char* id){
    uint32_t h=2166136261u;
    for(;id&&*id;++id)h=(h^uint8_t(*id))*16777619u;
    return h?h:1u;
}

// ---- small helpers ------------------------------------------------------------------------
uint32_t Session::own_channels(Channel* out) const {
    uint32_t n=0;
    for(const auto& s:subs_)if(s.live&&owner_active(s.owner))out[n++]=Channel{s.channel,s.version};
    return n;
}
Session::Link* Session::link_to(uint32_t peer){
    if(peer>=WIRE_SESSION_MAX_PEERS||!peers_[peer].present)return nullptr;
    const int32_t i=state_==WIRE_SESSION_JOINED?0:peers_[peer].link;   // a guest reaches everyone through its host
    return i>=0&&links_[i].used?&links_[i]:nullptr;
}
Session::Link* Session::link_from(const uint8_t ip[4],uint16_t port){
    for(auto& l:links_)if(l.used&&l.port==port&&same_ip(l.ip,ip))return &l;
    return nullptr;
}
int32_t Session::free_link(){
    for(uint32_t i=0;i<WIRE_SESSION_MAX_PEERS-1;++i)if(!links_[i].used)return int32_t(i);
    return -1;
}
bool Session::store(Link& l){
    if(!l.store)l.store=arena_zeroed<LinkStore>();
    return l.store!=nullptr;
}
void Session::reset_link(Link& l){
    LinkStore* keep=l.store;
    l=Link{};l.store=keep;
    if(keep)for(auto& e:keep->early)e.sequence=0;
}
uint32_t Session::header(uint8_t* out,uint8_t type) const {
    W w(out,HEADER);
    w.u32(SESSION_MAGIC);w.u16(SESSION_PROTOCOL);w.byte(type);w.byte(uint8_t(self_));w.u32(session_id_);w.u32(instance_);
    return w.n;
}
void Session::send_raw(const uint8_t ip[4],uint16_t port,const uint8_t* data,uint32_t size){
    if(const NetBackend* b=backend();b&&socket_>=0)b->send_to(socket_,ip,port,data,size);
}
void Session::emit(uint32_t type,uint32_t peer,uint32_t reason,uint32_t channel,const void* data,uint32_t size){
    if(!polling_){                                         // from a call: delivered on the next poll
        if(type!=WIRE_SESSION_EVENT_MESSAGE&&queued_count_<kQueued)queued_[queued_count_++]=Queued{type,peer,reason};
        return;
    }
    WireEvent e{};e.size=sizeof e;e.type=type;e.peer=peer;e.reason=reason;e.data=data;e.data_size=size;
    DispatchGuard::Scope scope(guard_,OP_SEND|OP_QUERY);
    for(auto& s:subs_){
        if(!s.live||!s.callback||!owner_active(s.owner))continue;
        if(type==WIRE_SESSION_EVENT_MESSAGE&&s.channel!=channel)continue;
        s.callback(s.user,&e);
        if(type==WIRE_SESSION_EVENT_MESSAGE)return;
    }
    if(type==WIRE_SESSION_EVENT_MESSAGE)++dropped_;          // nobody here runs that mod
}

// ---- reliable queue -----------------------------------------------------------------------
bool Session::queue(Link& l,uint32_t channel,uint32_t from,uint32_t to,const uint8_t* payload,uint32_t size){
    if(!l.store)return false;
    const uint32_t length=MESSAGE_HEADER+size;
    if(l.next-1-l.acked>=SESSION_OUT_SLOTS||l.byte_used+length>SESSION_OUT_BYTES)return false;
    uint8_t head[MESSAGE_HEADER];W w(head,MESSAGE_HEADER);w.u32(channel);w.byte(uint8_t(from));w.byte(uint8_t(to));
    const uint32_t offset=(l.byte_head+l.byte_used)%SESSION_OUT_BYTES;
    for(uint32_t i=0;i<length;++i)
        l.store->bytes[(offset+i)%SESSION_OUT_BYTES]=i<MESSAGE_HEADER?head[i]:payload[i-MESSAGE_HEADER];
    l.store->out[l.next%SESSION_OUT_SLOTS]=OutEntry{l.next,offset,uint16_t(length),-1.0f};
    ++l.next;l.byte_used+=length;
    return true;
}
bool Session::room(const Link& l,uint32_t size) const {
    return l.store&&l.next-1-l.acked<SESSION_OUT_SLOTS&&l.byte_used+MESSAGE_HEADER+size<=SESSION_OUT_BYTES;
}
void Session::acknowledge(Link& l,uint32_t ack){
    if(!l.store||!newer(ack,l.acked)||newer(ack,l.next-1))return;
    while(l.acked!=ack){
        const OutEntry& e=l.store->out[(l.acked+1)%SESSION_OUT_SLOTS];
        l.byte_head=(l.byte_head+e.length)%SESSION_OUT_BYTES;l.byte_used-=e.length;++l.acked;
    }
}
// LINK datagrams to one peer: what is due, else (force) one to acknowledge and keep in touch.
void Session::send_link(Link& l,double now,bool force){
    if(!l.store)return;
    const float t=float(clock(now));
    const float resend=l.round_trip>0?(l.round_trip*1.5f+0.02f<0.05f?0.05f:l.round_trip*1.5f+0.02f):0.25f;
    uint32_t cursor=l.acked+1;
    for(uint32_t sent=0;sent<SEND_BUDGET;++sent){
        uint8_t d[SESSION_DATAGRAM];
        W w(d,SESSION_DATAGRAM,header(d,T_LINK));
        w.f32(t);w.f32(l.their_clock);w.f32(l.their_clock<0?0.0f:float(now-l.their_clock_at));w.u32(l.delivered);
        w.byte(busy_?LINK_BUSY:0);
        const uint32_t count_at=w.n;w.byte(0);
        uint32_t count=0;
        for(;cursor!=l.next&&count<255;++cursor){
            OutEntry& e=l.store->out[cursor%SESSION_OUT_SLOTS];
            if(e.sent>=0&&t-e.sent<resend)continue;
            if(w.n+ENTRY_HEADER+e.length>SESSION_DATAGRAM)break;
            w.u32(e.sequence);w.u16(e.length);
            for(uint32_t i=0;i<e.length;++i)w.byte(l.store->bytes[(e.offset+i)%SESSION_OUT_BYTES]);
            e.sent=t;++count;
        }
        if(!count&&!force)break;
        d[count_at]=uint8_t(count);
        send_raw(l.ip,l.port,d,w.n);
        l.sent_at=now;l.ack_due=false;force=false;
        if(cursor==l.next)break;
    }
}
void Session::send_data(Link& l,uint32_t channel,uint32_t from,uint32_t to,const uint8_t* payload,uint32_t size){
    uint8_t d[SESSION_DATAGRAM];
    W w(d,SESSION_DATAGRAM,header(d,T_DATA));
    w.u32(channel);w.byte(uint8_t(from));w.byte(uint8_t(to));w.raw(payload,size);
    if(w.ok)send_raw(l.ip,l.port,d,w.n);
}
void Session::send_join(double now){
    uint8_t d[SESSION_DATAGRAM];
    W w(d,SESSION_DATAGRAM,header(d,T_JOIN));
    w.str(name_);
    Channel c[WIRE_SESSION_MAX_SUBSCRIPTIONS];const uint32_t n=own_channels(c);
    w.byte(uint8_t(n));for(uint32_t i=0;i<n;++i){w.u32(c[i].id);w.u32(c[i].version);}
    send_raw(searching_?BROADCAST:target_ip_,target_port_,d,w.n);
    join_sent_=now;
}
void Session::send_bye(Link& l){
    uint8_t d[HEADER];const uint32_t n=header(d,T_BYE);
    for(int i=0;i<2;++i)send_raw(l.ip,l.port,d,n);
}

// ---- control messages -----------------------------------------------------------------------
void Session::announce_peer(uint32_t about,bool present,uint32_t reason,int32_t except_link){
    uint8_t p[4+WIRE_SESSION_NAME_BYTES];W w(p,sizeof p);
    w.byte(C_PEER);w.byte(uint8_t(about));w.byte(present?1:0);w.byte(uint8_t(reason));w.str(peers_[about].name);
    for(uint32_t i=0;i<WIRE_SESSION_MAX_PEERS-1;++i)
        if(links_[i].used&&int32_t(i)!=except_link&&links_[i].peer!=about)queue(links_[i],0,self_,links_[i].peer,p,w.n);
}
void Session::announce_channels(uint32_t about,int32_t only_link,int32_t except_link){
    const PeerRecord& r=peers_[about];
    uint8_t p[3+8*WIRE_SESSION_MAX_SUBSCRIPTIONS];W w(p,sizeof p);
    w.byte(C_CHANNELS);w.byte(uint8_t(about));w.byte(uint8_t(r.channel_count));
    for(uint32_t i=0;i<r.channel_count;++i){w.u32(r.channels[i].id);w.u32(r.channels[i].version);}
    for(uint32_t i=0;i<WIRE_SESSION_MAX_PEERS-1;++i){
        if(!links_[i].used||links_[i].peer==about||int32_t(i)==except_link)continue;
        if(only_link>=0&&int32_t(i)!=only_link)continue;
        queue(links_[i],0,self_,links_[i].peer,p,w.n);
    }
}
void Session::on_control(uint32_t from,const uint8_t* payload,uint32_t size,int32_t via){
    R r(payload,size);
    const uint8_t kind=r.byte();
    if(kind==C_PEER&&state_==WIRE_SESSION_JOINED&&from==WIRE_PEER_HOST){
        const uint8_t peer=r.byte(),present=r.byte(),reason=r.byte();char name[WIRE_SESSION_NAME_BYTES];r.str(name);
        if(!r.ok||peer==WIRE_PEER_HOST||peer>=WIRE_SESSION_MAX_PEERS||peer==self_)return;
        PeerRecord& p=peers_[peer];
        if(present&&!p.present){p=PeerRecord{};p.present=true;copy(p.name,name);emit(WIRE_SESSION_EVENT_PEER_JOINED,peer,0);}
        else if(!present&&p.present){p=PeerRecord{};emit(WIRE_SESSION_EVENT_PEER_LEFT,peer,reason?reason:WIRE_SESSION_LEFT_BYE);}
    }else if(kind==C_CHANNELS){
        uint8_t peer=r.byte();const uint8_t count=r.byte();
        if(state_==WIRE_SESSION_HOSTING)peer=uint8_t(from);                  // a guest speaks only for itself
        else if(from!=WIRE_PEER_HOST)return;
        if(peer>=WIRE_SESSION_MAX_PEERS||peer==self_||!peers_[peer].present||count>WIRE_SESSION_MAX_SUBSCRIPTIONS)return;
        Channel c[WIRE_SESSION_MAX_SUBSCRIPTIONS];
        for(uint8_t i=0;i<count;++i){c[i].id=r.u32();c[i].version=r.u32();}
        if(!r.ok)return;
        PeerRecord& p=peers_[peer];p.channel_count=count;for(uint8_t i=0;i<count;++i)p.channels[i]=c[i];
        if(state_==WIRE_SESSION_HOSTING)announce_channels(peer,-1,via);       // the other guests learn it too
        emit(WIRE_SESSION_EVENT_CHANNELS,peer,0);
    }
}
// This machine's channels changed (a mod subscribed, stopped or started): tell the others.
void Session::check_channels(){
    Channel c[WIRE_SESSION_MAX_SUBSCRIPTIONS];const uint32_t n=own_channels(c);
    bool same_set=n==announced_count_;
    for(uint32_t i=0;same_set&&i<n;++i)same_set=c[i].id==announced_[i].id&&c[i].version==announced_[i].version;
    if(same_set)return;
    announced_count_=n;for(uint32_t i=0;i<n;++i)announced_[i]=c[i];
    if(self_>=WIRE_SESSION_MAX_PEERS)return;
    PeerRecord& me=peers_[self_];me.channel_count=n;for(uint32_t i=0;i<n;++i)me.channels[i]=c[i];
    if(state_==WIRE_SESSION_HOSTING||state_==WIRE_SESSION_JOINED)announce_channels(self_,-1,-1);
}

// ---- peers coming and going ------------------------------------------------------------------
void Session::drop_link(Link& l,uint32_t reason){
    const uint32_t peer=l.peer;const int32_t index=int32_t(&l-links_);
    reset_link(l);
    if(peer<WIRE_SESSION_MAX_PEERS&&peers_[peer].present){
        announce_peer(peer,false,reason,index);
        peers_[peer]=PeerRecord{};
        emit(WIRE_SESSION_EVENT_PEER_LEFT,peer,reason);
    }
}
void Session::lose_host(uint32_t reason,double now){
    for(uint32_t p=0;p<WIRE_SESSION_MAX_PEERS;++p)
        if(p!=self_&&peers_[p].present){peers_[p]=PeerRecord{};emit(WIRE_SESSION_EVENT_PEER_LEFT,p,reason);}
    if(self_<WIRE_SESSION_MAX_PEERS)peers_[self_]=PeerRecord{};
    reset_link(links_[0]);
    state_=WIRE_SESSION_JOINING;self_=WIRE_PEER_NONE;session_id_=0;have_offset_=false;
    searching_=search_origin_;
    if(searching_)target_port_=search_port_;
    join_sent_=-1;announced_count_=0;(void)now;
}
void Session::end(uint32_t reason){
    for(uint32_t p=0;p<WIRE_SESSION_MAX_PEERS;++p)
        if(p!=self_&&peers_[p].present)emit(WIRE_SESSION_EVENT_PEER_LEFT,p,WIRE_SESSION_LEFT_ENDED);
    emit(WIRE_SESSION_EVENT_ENDED,self_,reason);
    if(const NetBackend* b=backend();b&&socket_>=0)b->close(socket_);
    socket_=-1;
    for(auto& l:links_){tsuchi::release(l.store);l.store=nullptr;reset_link(l);}
    for(auto& p:peers_)p=PeerRecord{};
    state_=WIRE_SESSION_NONE;self_=WIRE_PEER_NONE;session_id_=0;refused_=0;owner_[0]=0;have_offset_=false;announced_count_=0;
}

// ---- receiving -----------------------------------------------------------------------------------
void Session::on_join(const uint8_t ip[4],uint16_t port,uint32_t instance,const uint8_t* body,uint32_t size,double now){
    R r(body,size);
    char name[WIRE_SESSION_NAME_BYTES];r.str(name);
    const uint8_t count=r.byte();
    if(count>WIRE_SESSION_MAX_SUBSCRIPTIONS)return;
    Channel c[WIRE_SESSION_MAX_SUBSCRIPTIONS];
    for(uint8_t i=0;i<count;++i){c[i].id=r.u32();c[i].version=r.u32();}
    if(!r.ok)return;
    auto welcome=[&](Link& l){
        uint8_t d[SESSION_DATAGRAM];W w(d,SESSION_DATAGRAM,header(d,T_WELCOME));
        w.byte(uint8_t(l.peer));w.byte(uint8_t(max_peers_));w.str(name_);
        send_raw(ip,port,d,w.n);
    };
    if(Link* l=link_from(ip,port)){
        if(l->instance==instance&&l->acked==0&&l->delivered==0){l->heard=now;welcome(*l);return;}   // our welcome was lost
        drop_link(*l,l->instance==instance?WIRE_SESSION_LEFT_TIMEOUT:WIRE_SESSION_LEFT_BYE);          // it started over
    }
    uint32_t guests=0;for(uint32_t p=1;p<WIRE_SESSION_MAX_PEERS;++p)guests+=peers_[p].present?1:0;
    uint32_t peer=1;while(peer<max_peers_&&peers_[peer].present)++peer;
    const int32_t index=free_link();
    if(guests+1>=max_peers_||peer>=max_peers_||index<0||!store(links_[index])){
        uint8_t d[HEADER+1];W w(d,sizeof d,header(d,T_REFUSE));w.byte(WIRE_SESSION_REFUSED_FULL);
        send_raw(ip,port,d,w.n);return;
    }
    Link& l=links_[index];reset_link(l);
    l.used=true;l.peer=peer;for(int i=0;i<4;++i)l.ip[i]=ip[i];l.port=port;l.instance=instance;l.heard=now;
    PeerRecord& p=peers_[peer];p=PeerRecord{};p.present=true;copy(p.name,name);p.link=index;
    p.channel_count=count;for(uint8_t i=0;i<count;++i)p.channels[i]=c[i];
    welcome(l);
    // The newcomer learns everyone else and their channels; everyone else learns it.
    announce_channels(WIRE_PEER_HOST,index,-1);
    for(uint32_t other=1;other<WIRE_SESSION_MAX_PEERS;++other){
        if(other==peer||!peers_[other].present)continue;
        uint8_t m[4+WIRE_SESSION_NAME_BYTES];W w(m,sizeof m);
        w.byte(C_PEER);w.byte(uint8_t(other));w.byte(1);w.byte(0);w.str(peers_[other].name);
        queue(l,0,self_,peer,m,w.n);
        announce_channels(other,index,-1);
    }
    announce_peer(peer,true,0,index);
    announce_channels(peer,-1,index);
    emit(WIRE_SESSION_EVENT_PEER_JOINED,peer,0);
}
void Session::on_welcome(const uint8_t ip[4],uint16_t port,uint32_t session,uint32_t instance,const uint8_t* body,uint32_t size,double now){
    R r(body,size);
    const uint8_t you=r.byte(),max=r.byte();char name[WIRE_SESSION_NAME_BYTES];r.str(name);
    if(!r.ok||!session||you==WIRE_PEER_HOST||you>=WIRE_SESSION_MAX_PEERS||max<2||max>WIRE_SESSION_MAX_PEERS)return;
    if(!searching_&&(port!=target_port_||!same_ip(ip,target_ip_)))return;
    Link& l=links_[0];
    if(!store(l))return;
    reset_link(l);
    l.used=true;l.peer=WIRE_PEER_HOST;for(int i=0;i<4;++i){l.ip[i]=ip[i];target_ip_[i]=ip[i];}l.port=port;target_port_=port;
    l.instance=instance;l.heard=now;
    searching_=false;
    state_=WIRE_SESSION_JOINED;self_=you;session_id_=session;max_peers_=max;refused_=0;
    PeerRecord& host=peers_[WIRE_PEER_HOST];host=PeerRecord{};host.present=true;copy(host.name,name);host.link=0;
    PeerRecord& me=peers_[self_];me=PeerRecord{};me.present=true;copy(me.name,name_);
    announced_count_=0;   // check_channels announces ours
    emit(WIRE_SESSION_EVENT_STARTED,self_,0);
    emit(WIRE_SESSION_EVENT_PEER_JOINED,WIRE_PEER_HOST,0);
    check_channels();
}
void Session::on_message(Link& l,const uint8_t* message,uint32_t size,bool reliable){
    R r(message,size);
    const uint32_t channel=r.u32();uint32_t from=r.byte();const uint32_t to=r.byte();
    if(!r.ok||size-MESSAGE_HEADER>WIRE_SESSION_MAX_MESSAGE)return;
    const uint8_t* payload=message+MESSAGE_HEADER;const uint32_t n=size-MESSAGE_HEADER;
    if(state_==WIRE_SESSION_HOSTING){
        from=l.peer;                                                             // a guest speaks only for itself
        if(to!=WIRE_PEER_HOST){                                                   // pass it on
            for(auto& other:links_){
                if(!other.used||&other==&l||(to!=WIRE_PEER_ALL&&other.peer!=to))continue;
                if(reliable){if(!queue(other,channel,from,to,payload,n))++dropped_;}
                else send_data(other,channel,from,to,payload,n);
            }
            if(to!=WIRE_PEER_ALL)return;
        }
    }else if(to!=self_&&to!=WIRE_PEER_ALL)return;
    if(from>=WIRE_SESSION_MAX_PEERS||!peers_[from].present)return;
    if(channel==0){if(reliable)on_control(from,payload,n,int32_t(&l-links_));return;}
    emit(WIRE_SESSION_EVENT_MESSAGE,from,reliable?WIRE_SESSION_SEND_RELIABLE:0u,channel,payload,n);
}
void Session::on_link(Link& l,const uint8_t* body,uint32_t size,double now){
    R r(body,size);
    const float sent=r.f32(),echo=r.f32(),held=r.f32();const uint32_t ack=r.u32();const uint8_t flags=r.byte(),count=r.byte();
    if(!r.ok||!finite(sent)||!finite(echo)||!finite(held))return;
    l.grace_until=flags&LINK_BUSY?now+BUSY_GRACE:0;
    l.their_clock=sent;l.their_clock_at=now;
    if(echo>=0){
        const float sample=float(clock(now))-echo-held;
        if(sample>=0&&sample<10){
            l.round_trip=l.round_trip<0?sample:l.round_trip*0.875f+sample*0.125f;
            if(state_==WIRE_SESSION_JOINED&&&l==&links_[0]){                    // the host's clock, shared
                const float offset=sent+sample*0.5f-float(clock(now));
                if(!have_offset_||sample<=offset_round_trip_*1.5f){
                    clock_offset_=have_offset_?clock_offset_+(offset-clock_offset_)*0.25f:offset;
                    have_offset_=true;offset_round_trip_=offset_round_trip_<0||sample<offset_round_trip_?sample:offset_round_trip_*0.99f+sample*0.01f;
                }
            }
        }
    }
    acknowledge(l,ack);
    for(uint8_t i=0;i<count;++i){
        const uint32_t sequence=r.u32();const uint16_t length=r.u16();
        const uint8_t* m=r.take(length);
        if(!r.ok||length<MESSAGE_HEADER||length>WIRE_SESSION_MAX_MESSAGE+MESSAGE_HEADER)return;
        l.ack_due=true;
        if(!newer(sequence,l.delivered)||newer(sequence,l.delivered+SESSION_EARLY))continue;   // old, or too far ahead
        if(sequence!=l.delivered+1){
            Early& e=l.store->early[sequence%SESSION_EARLY];
            e.sequence=sequence;e.length=length;bytes_copy(e.data,m,length);
            continue;
        }
        l.delivered=sequence;on_message(l,m,length,true);
        if(!l.used)return;                                                       // it left meanwhile
        for(;;){
            Early& e=l.store->early[(l.delivered+1)%SESSION_EARLY];
            if(e.sequence!=l.delivered+1)break;
            e.sequence=0;++l.delivered;on_message(l,e.data,e.length,true);
            if(!l.used)return;
        }
    }
}
void Session::receive(const uint8_t ip[4],uint16_t port,const uint8_t* data,uint32_t size,double now){
    R r(data,size);
    if(r.u32()!=SESSION_MAGIC)return;
    const uint16_t protocol=r.u16();const uint8_t type=r.byte();r.byte();
    const uint32_t session=r.u32(),instance=r.u32();
    if(!r.ok)return;
    const uint8_t* body=data+HEADER;const uint32_t n=size-HEADER;
    if(type==T_REFUSE){                                                          // any protocol: the reason is always here
        if(state_==WIRE_SESSION_JOINING&&n>=1)refused_=body[0];
        return;
    }
    if(protocol!=SESSION_PROTOCOL){
        if(state_==WIRE_SESSION_HOSTING&&type==T_JOIN){
            uint8_t d[HEADER+1];W w(d,sizeof d,header(d,T_REFUSE));w.byte(WIRE_SESSION_REFUSED_VERSION);send_raw(ip,port,d,w.n);
        }
        return;
    }
    if(type==T_JOIN){if(state_==WIRE_SESSION_HOSTING)on_join(ip,port,instance,body,n,now);return;}
    if(type==T_WELCOME){if(state_==WIRE_SESSION_JOINING)on_welcome(ip,port,session,instance,body,n,now);return;}
    Link* l=link_from(ip,port);
    if(!l||l->instance!=instance||session!=session_id_){
        // A guest this host dropped (it timed out here first) is told, so it joins again at once.
        if(state_==WIRE_SESSION_HOSTING&&session==session_id_&&type!=T_BYE){uint8_t d[HEADER];send_raw(ip,port,d,header(d,T_BYE));}
        return;
    }
    l->heard=now;
    if(type==T_LINK)on_link(*l,body,n,now);
    else if(type==T_DATA)on_message(*l,body,n,false);
    else if(type==T_BYE){
        if(state_==WIRE_SESSION_HOSTING)drop_link(*l,WIRE_SESSION_LEFT_BYE);
        else lose_host(WIRE_SESSION_LEFT_BYE,now);
    }
}

// ---- public calls ------------------------------------------------------------------------------
TSUCHI_Result Session::start(const char* owner,const WireOptions* options,double now){
    if(!valid_id(owner)||(options&&options->size!=sizeof(WireOptions)))return TSUCHI_INVALID;
    if(guard_.active())return TSUCHI_BUSY;
    if(!backend())return TSUCHI_UNSUPPORTED;
    if(!owner_available(owner))return TSUCHI_NOT_READY;
    if(state_!=WIRE_SESSION_NONE)return TSUCHI_BUSY;
    if(!backend()->available())return TSUCHI_IO;
    copy(owner_,owner);
    name_[0]=0;
    if(options){uint32_t i=0;for(;i+1<WIRE_SESSION_NAME_BYTES&&options->name[i];++i)name_[i]=options->name[i];name_[i]=0;}
    started_=now;have_offset_=false;offset_round_trip_=-1;refused_=0;announced_count_=0;queued_count_=0;
    seed_^=uint32_t(uint64_t(now*1000003.0))^0x5BD1E995u;
    auto next=[&](){seed_^=seed_<<13;seed_^=seed_>>17;seed_^=seed_<<5;return seed_?seed_:1u;};
    instance_=next();session_id_=0;
    for(auto& p:peers_)p=PeerRecord{};
    for(auto& l:links_)reset_link(l);
    return TSUCHI_OK;
}
TSUCHI_Result Session::host(const char* owner,const WireOptions* options,double now){
    const uint32_t wanted=options&&options->max_peers?options->max_peers:WIRE_SESSION_MAX_PEERS;
    if(options&&options->size==sizeof(WireOptions)&&(wanted<2||wanted>WIRE_SESSION_MAX_PEERS))return TSUCHI_INVALID;
    if(const TSUCHI_Result r=start(owner,options,now);r!=TSUCHI_OK)return r;
    const uint16_t port=options&&options->port?options->port:uint16_t(WIRE_SESSION_DEFAULT_PORT);
    const int32_t fd=backend()->udp_open(port,false);
    if(fd<0){owner_[0]=0;return fd==-3?TSUCHI_BUSY:TSUCHI_IO;}
    socket_=fd;port_=port;max_peers_=wanted;
    session_id_=instance_^0xA5A5A5A5u;if(!session_id_)session_id_=1;
    state_=WIRE_SESSION_HOSTING;self_=WIRE_PEER_HOST;
    PeerRecord& me=peers_[WIRE_PEER_HOST];me.present=true;copy(me.name,name_);
    emit(WIRE_SESSION_EVENT_STARTED,WIRE_PEER_HOST,0);
    return TSUCHI_OK;
}
TSUCHI_Result Session::join(const char* owner,const WireAddress* host,const WireOptions* options,double now){
    if(host&&(host->size!=sizeof(WireAddress)||host->reserved||!host->port))return TSUCHI_INVALID;
    if(const TSUCHI_Result r=start(owner,options,now);r!=TSUCHI_OK)return r;
    const int32_t fd=backend()->udp_open(0,host==nullptr);
    if(fd<0){owner_[0]=0;return TSUCHI_IO;}
    socket_=fd;
    uint8_t ip[4]{};uint16_t port=0;port_=backend()->local(fd,ip,&port)==0?port:0;
    search_origin_=searching_=host==nullptr;
    search_port_=options&&options->port?options->port:uint16_t(WIRE_SESSION_DEFAULT_PORT);
    if(host){for(int i=0;i<4;++i)target_ip_[i]=host->ip[i];target_port_=host->port;}
    else target_port_=search_port_;
    state_=WIRE_SESSION_JOINING;self_=WIRE_PEER_NONE;max_peers_=WIRE_SESSION_MAX_PEERS;join_sent_=-1;
    return TSUCHI_OK;
}
TSUCHI_Result Session::leave(const char* owner,double now){
    if(!valid_id(owner))return TSUCHI_INVALID;
    if(guard_.active())return TSUCHI_BUSY;
    if(state_==WIRE_SESSION_NONE)return TSUCHI_NOT_FOUND;
    for(auto& l:links_)if(l.used)send_bye(l);
    end(WIRE_SESSION_LEFT_ENDED);(void)now;
    return TSUCHI_OK;
}
TSUCHI_Result Session::info(WireInfo* out,double now) const {
    if(!out||out->size!=sizeof(WireInfo))return TSUCHI_INVALID;
    if(!guard_.permits(OP_QUERY))return TSUCHI_BUSY;
    *out=WireInfo{};out->size=sizeof(WireInfo);
    out->state=state_;out->self=self_;out->id=session_id_;out->refused=refused_;out->port=port_;
    for(uint32_t p=0;p<WIRE_SESSION_MAX_PEERS;++p)if(peers_[p].present)out->peers|=1u<<p;
    if(state_!=WIRE_SESSION_NONE)out->time=float(clock(now))+(state_==WIRE_SESSION_HOSTING?0.0f:clock_offset_);
    copy(out->owner,owner_);
    return TSUCHI_OK;
}
TSUCHI_Result Session::peer(uint32_t peer,WirePeer* out,double now) const {
    if(!out||out->size!=sizeof(WirePeer))return TSUCHI_INVALID;
    if(!guard_.permits(OP_QUERY))return TSUCHI_BUSY;
    if(peer>=WIRE_SESSION_MAX_PEERS||!peers_[peer].present)return TSUCHI_NOT_FOUND;
    *out=WirePeer{};out->size=sizeof(WirePeer);out->address.size=sizeof(WireAddress);
    copy(out->name,peers_[peer].name);out->round_trip=-1;
    if(peer==self_)return TSUCHI_OK;
    const int32_t index=state_==WIRE_SESSION_JOINED?0:peers_[peer].link;
    if(index<0||!links_[index].used)return TSUCHI_OK;
    const Link& l=links_[index];
    if(peers_[peer].link==index)for(int i=0;i<4;++i)out->address.ip[i]=l.ip[i];
    if(peers_[peer].link==index)out->address.port=l.port;
    out->round_trip=l.round_trip;out->heard=float(now-l.heard);out->pending=l.next-1-l.acked;
    return TSUCHI_OK;
}
TSUCHI_Result Session::peer_channel(uint32_t peer,const char* mod_id,uint32_t* version) const {
    if(version)*version=0;
    if(!valid_id(mod_id)||!version)return TSUCHI_INVALID;
    if(!guard_.permits(OP_QUERY))return TSUCHI_BUSY;
    if(peer>=WIRE_SESSION_MAX_PEERS||!peers_[peer].present)return TSUCHI_NOT_FOUND;
    const uint32_t id=session_channel(mod_id);
    if(peer==self_){
        for(const auto& s:subs_)if(s.live&&s.channel==id&&owner_active(s.owner)){*version=s.version;return TSUCHI_OK;}
        return TSUCHI_NOT_FOUND;
    }
    const PeerRecord& p=peers_[peer];
    for(uint32_t i=0;i<p.channel_count;++i)if(p.channels[i].id==id){*version=p.channels[i].version;return TSUCHI_OK;}
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result Session::send(const char* owner,uint32_t to,const void* data,uint32_t size,uint32_t flags,double now){
    if(!valid_id(owner)||!data||!size||size>WIRE_SESSION_MAX_MESSAGE||(flags&~uint32_t(WIRE_SESSION_SEND_RELIABLE)))return TSUCHI_INVALID;
    if(!guard_.permits(OP_SEND))return TSUCHI_BUSY;
    const uint32_t channel=session_channel(owner);
    bool open=false;for(const auto& s:subs_)open=open||(s.live&&s.channel==channel);
    if(!open)return TSUCHI_BUSY;
    if(!owner_active(owner))return TSUCHI_NOT_READY;
    if(state_!=WIRE_SESSION_HOSTING&&state_!=WIRE_SESSION_JOINED)return TSUCHI_NOT_READY;
    if(to==self_||(to!=WIRE_PEER_ALL&&to>=WIRE_SESSION_MAX_PEERS))return TSUCHI_INVALID;
    if(to!=WIRE_PEER_ALL&&!peers_[to].present)return TSUCHI_NOT_FOUND;
    Link* targets[WIRE_SESSION_MAX_PEERS-1];uint32_t count=0;
    if(state_==WIRE_SESSION_JOINED){if(links_[0].used)targets[count++]=&links_[0];}
    else if(to==WIRE_PEER_ALL){for(auto& l:links_)if(l.used)targets[count++]=&l;}
    else if(Link* l=link_to(to))targets[count++]=l;
    if(!count)return TSUCHI_NOT_READY;
    const auto* bytes=static_cast<const uint8_t*>(data);
    if(flags&WIRE_SESSION_SEND_RELIABLE){
        for(uint32_t i=0;i<count;++i)if(!room(*targets[i],size))return TSUCHI_CAPACITY;
        for(uint32_t i=0;i<count;++i)queue(*targets[i],channel,self_,to,bytes,size);
    }else for(uint32_t i=0;i<count;++i)send_data(*targets[i],channel,self_,to,bytes,size);
    (void)now;
    return TSUCHI_OK;
}
TSUCHI_Result Session::subscribe(const char* owner,uint32_t version,WireCallback callback,void* user,uint32_t* out){
    if(out)*out=0;
    if(!out||!callback||!valid_id(owner))return TSUCHI_INVALID;
    if(guard_.active())return TSUCHI_BUSY;
    if(!owner_available(owner))return TSUCHI_NOT_READY;
    const uint32_t channel=session_channel(owner);
    for(const auto& s:subs_)if(s.live&&s.channel==channel)return TSUCHI_BUSY;
    for(uint32_t i=0;i<WIRE_SESSION_MAX_SUBSCRIPTIONS;++i){
        Subscription& s=subs_[i];
        if(s.live||s.slot.retired)continue;
        copy(s.owner,owner);s.channel=channel;s.version=version;s.callback=callback;s.user=user;s.live=true;
        *out=s.slot.handle(i);return TSUCHI_OK;
    }
    return TSUCHI_CAPACITY;
}
TSUCHI_Result Session::unsubscribe(uint32_t handle){
    const uint32_t i=handle_slot(handle,WIRE_SESSION_MAX_SUBSCRIPTIONS);
    if(i==UINT32_MAX||!subs_[i].live||!subs_[i].slot.matches(handle))return TSUCHI_NOT_FOUND;
    if(guard_.active())return TSUCHI_BUSY;
    subs_[i].live=false;subs_[i].callback=nullptr;subs_[i].user=nullptr;subs_[i].slot.invalidate();
    return TSUCHI_OK;
}

// ---- the frame --------------------------------------------------------------------------------
void Session::poll(double now,bool busy){
    if(stop_pending_){stop_pending_=false;end(WIRE_SESSION_LEFT_ENDED);}
    // Back from a stall: this machine heard nothing because it did not listen, so the time is not theirs.
    if(last_poll_>=0&&now-last_poll_>STALL_AFTER)
        for(auto& l:links_)if(l.used){l.heard+=now-last_poll_;if(l.grace_until>0)l.grace_until+=now-last_poll_;}
    last_poll_=now;
    if(busy)busy_until_=now+BUSY_HOLD;
    busy_=busy||now<busy_until_;
    polling_=true;
    for(uint32_t i=0;i<queued_count_;++i)emit(queued_[i].type,queued_[i].peer,queued_[i].reason);
    queued_count_=0;
    const NetBackend* b=backend();
    if(state_==WIRE_SESSION_NONE||!b){polling_=false;return;}
    if(!owner_active(owner_)){                                                   // its starter stopped
        for(auto& l:links_)if(l.used)send_bye(l);
        end(WIRE_SESSION_LEFT_ENDED);polling_=false;return;
    }
    check_channels();
    for(uint32_t k=0;k<RECEIVE_BUDGET&&socket_>=0;++k){
        uint8_t data[1500];uint8_t ip[4]{};uint16_t port=0;
        const int32_t got=b->receive_from(socket_,ip,&port,data,sizeof data);
        if(got<0)break;
        if(uint32_t(got)>=HEADER)receive(ip,port,data,uint32_t(got),now);
        if(state_==WIRE_SESSION_NONE){polling_=false;return;}
    }
    if(state_==WIRE_SESSION_HOSTING){
        for(auto& l:links_)if(l.used&&now-l.heard>double(WIRE_SESSION_TIMEOUT)&&now>=l.grace_until)drop_link(l,WIRE_SESSION_LEFT_TIMEOUT);
    }else if(state_==WIRE_SESSION_JOINED){
        if(now-links_[0].heard>double(WIRE_SESSION_TIMEOUT)&&now>=links_[0].grace_until)lose_host(WIRE_SESSION_LEFT_TIMEOUT,now);
    }
    if(state_==WIRE_SESSION_JOINING&&(join_sent_<0||now-join_sent_>=JOIN_EVERY))send_join(now);
    for(auto& l:links_)if(l.used)send_link(l,now,l.ack_due||l.sent_at<0||now-l.sent_at>=KEEP_EVERY);
    polling_=false;
    if(stop_pending_){stop_pending_=false;end(WIRE_SESSION_LEFT_ENDED);}
}

void Session::stop(){
    if(state_==WIRE_SESSION_NONE)return;
    if(guard_.active()){
        stop_pending_=true;
        if(const NetBackend* b=backend();b&&socket_>=0)b->close(socket_);
        socket_=-1;
    }else{for(auto& l:links_)if(l.used)send_bye(l);end(WIRE_SESSION_LEFT_ENDED);}
}
void Session::stop_channel(uint32_t handle){
    const uint32_t i=handle_slot(handle,WIRE_SESSION_MAX_SUBSCRIPTIONS);
    if(i==UINT32_MAX||!subs_[i].live||!subs_[i].slot.matches(handle))return;
    subs_[i].live=false;subs_[i].callback=nullptr;subs_[i].user=nullptr;subs_[i].slot.invalidate();
}

}
