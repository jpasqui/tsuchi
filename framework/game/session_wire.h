#pragma once
#include "session_wire_types.h"
// Framework-internal side of session. One Session is one machine's session; the
// public API drives a single instance, and the host tests run several side by side over
// a network double. Freestanding. Game-thread only.
//
// Wire format (little-endian; strings are a length byte and that many bytes):
//
//   header   u32 magic "P4MS", u16 protocol, u8 type, u8 from (peer, 255 unknown),
//            u32 session (the host's number; 0 while joining), u32 instance (the sender's
//            number for this session start, so a restarted machine is told apart)
//   JOIN     str name, u8 channel count, count x (u32 channel, u32 version)   guest -> host
//   WELCOME  u8 your peer, u8 max peers, str host name                        host -> guest
//   REFUSE   u8 reason (WIRE_SESSION_REFUSED_*)                                host -> guest
//   LINK     f32 sent (the sender's clock), f32 echo (the last clock it heard from us),
//            f32 held (seconds since it heard it), u32 ack, u8 count,
//            count x (u32 sequence, u16 length, message)                      both ways
//   DATA     message (unreliable)                                             both ways
//   BYE      (nothing)                                                        both ways
//   PUNCH    (nothing): opens the sender's router to the receiver, which ignores it (session_traversal.cpp)
//
//   message  u32 channel (FNV-1a of the mod ID; 0 is the framework's own), u8 from,
//            u8 to (a peer or WIRE_PEER_ALL), payload
//
// Channel 0 (reliable) carries: PEER u8 peer, u8 present, str name (the host tells guests
// about each other), and CHANNELS u8 peer, u8 count, count x (u32 channel, u32 version).
namespace tsuchi::session_wire {

constexpr uint32_t SESSION_MAGIC=0x534D3450u;   // "P4MS" read as bytes
constexpr uint16_t SESSION_PROTOCOL=3;   // 2: LINK datagrams carry a flags byte (busy loading); 3: sealed datagrams
constexpr uint32_t SESSION_DATAGRAM=1200;       // stays under common path MTUs
constexpr uint32_t SESSION_OUT_SLOTS=1024;      // reliable messages queued per link
constexpr uint32_t SESSION_OUT_BYTES=192u*1024u;
constexpr uint32_t SESSION_EARLY=64;            // reliable messages held ahead of a gap
constexpr uint8_t SESSION_PUNCH=7;              // the datagram type of a punch
constexpr uint8_t SESSION_SEALED=0x80;          // in the type byte: the rest is sealed (session_secure.cpp)
constexpr uint32_t SESSION_SECRET_BYTES=32;
constexpr uint32_t SESSION_NONCE_BYTES=16;
constexpr uint32_t SESSION_SEAL=24;             // a sealed link datagram's counter and tag
constexpr uint32_t SESSION_ROOM=SESSION_DATAGRAM-SESSION_SEAL;   // what a link datagram holds before sealing
constexpr uint32_t SESSION_HEADER_BYTES=16;     // a datagram's header
constexpr uint32_t SESSION_OP_QUERY=2u;         // calls that read, which an event callback may make

uint32_t session_channel(const char* mod_id);   // FNV-1a, never 0

class Session {
public:
    constexpr explicit Session(const NetBackend* (*net)()):net_(net){}
    // The public calls (see session_wire_types.h). `now` is this machine's clock in seconds.
    TSUCHI_Result host(const char* owner,const WireOptions* options,double now);
    TSUCHI_Result join(const char* owner,const WireAddress* host,const WireOptions* options,double now);
    TSUCHI_Result leave(const char* owner,double now);
    TSUCHI_Result info(WireInfo* out,double now) const;
    TSUCHI_Result peer(uint32_t peer,WirePeer* out,double now) const;
    TSUCHI_Result peer_channel(uint32_t peer,const char* mod_id,uint32_t* version) const;
    TSUCHI_Result send(const char* owner,uint32_t to,const void* data,uint32_t size,uint32_t flags,double now);
    // Getting through routers (DESIGN.md): ask a STUN server what the internet sees for this session's socket,
    // read the answer, and punch toward a guest from the hosting session.
    // A secured session (DESIGN.md, "Secured sessions"): the secret the next session this mod starts seals its
    // datagrams with. 32 bytes, or none (size 0) for an open session.
    TSUCHI_Result set_secret(const char* owner,const uint8_t* secret,uint32_t size);
    bool secured() const {return secret_set_;}
    TSUCHI_Result discover(const char* owner,const WireAddress* server,double now);
    TSUCHI_Result public_address(WireAddress* out) const;
    TSUCHI_Result punch(const char* owner,const WireAddress* to,double now);
    TSUCHI_Result subscribe(const char* owner,uint32_t version,WireCallback callback,void* user,uint32_t* out);
    TSUCHI_Result unsubscribe(uint32_t subscription);
    // Once per frame: receive, deliver events, resend, keep in touch, time peers out. `busy`: this machine is
    // about to stop answering for a while (the game is changing scenes or saving), so its peers wait longer.
    void poll(double now,bool busy=false);
    // Mod stop may occur inside an event. Socket closure is immediate.
    void stop();
    void stop_channel(uint32_t subscription);
    bool dispatching() const {return guard_.active();}
    // Tests.
    uint32_t dropped() const {return dropped_;}

private:
    struct Channel {uint32_t id=0,version=0;};
    struct PeerRecord {
        bool present=false;
        char name[WIRE_SESSION_NAME_BYTES]{};
        uint32_t channel_count=0;Channel channels[WIRE_SESSION_MAX_SUBSCRIPTIONS];
        int32_t link=-1;             // this machine's direct link to it, or -1 (a guest reaches others through the host)
    };
    struct OutEntry {uint32_t sequence,offset;uint16_t length;float sent;};
    struct Early {uint32_t sequence;uint16_t length;uint8_t data[WIRE_SESSION_MAX_MESSAGE+6];};
    struct Queued {uint32_t type=0,peer=0,reason=0;};   // events raised by a call, delivered on the next poll
    static constexpr uint32_t kQueued=32;
    struct LinkStore {OutEntry out[SESSION_OUT_SLOTS];uint8_t bytes[SESSION_OUT_BYTES];Early early[SESSION_EARLY];};
    struct Link {
        bool used=false;
        bool confirmed=false;        // the other end answered on this link (a LINK or DATA from it, or a WELCOME)
        // Sealing (secured sessions): the link's keys, its counters, the window of counters received, and on the
        // host the nonce and number its WELCOME was made with.
        uint8_t send_key[32]{},receive_key[32]{};
        uint64_t send_counter=0,receive_top=0,receive_window=0;bool received_any=false;
        uint8_t host_nonce[SESSION_NONCE_BYTES]{};uint32_t link_number=0;
        uint32_t peer=WIRE_PEER_NONE;
        uint8_t ip[4]{};uint16_t port=0;
        uint32_t instance=0;
        double heard=0,sent_at=-1;
        double grace_until=0;        // it said it was busy loading: not timed out before this
        float round_trip=-1;
        float their_clock=-1;double their_clock_at=0;   // for echoing
        // Reliable out: FIFO of messages in `bytes` (a ring), indexed by `out`.
        uint32_t next=1,acked=0,byte_head=0,byte_used=0;
        // Reliable in.
        uint32_t delivered=0;bool ack_due=false;
        LinkStore* store=nullptr;
    };
    struct Subscription {
        HandleSlot slot;bool live=false;
        char owner[WIRE_ID_BYTES]{};uint32_t channel=0,version=0;
        WireCallback callback=nullptr;void* user=nullptr;
    };

    const NetBackend* backend() const {return net_?net_():nullptr;}
    double clock(double now) const {return now-started_;}
    TSUCHI_Result start(const char* owner,const WireOptions* options,double now);
    uint32_t own_channels(Channel* out) const;
    Link* link_to(uint32_t peer);
    Link* link_from(const uint8_t ip[4],uint16_t port);
    int32_t free_link();
    bool store(Link& l);
    void reset_link(Link& l);
    bool queue(Link& l,uint32_t channel,uint32_t from,uint32_t to,const uint8_t* payload,uint32_t size);
    bool room(const Link& l,uint32_t size) const;
    void acknowledge(Link& l,uint32_t ack);
    void send_raw(const uint8_t ip[4],uint16_t port,const uint8_t* data,uint32_t size);
    uint32_t header(uint8_t* out,uint8_t type) const;
    void send_link(Link& l,double now,bool force);
    void send_data(Link& l,uint32_t channel,uint32_t from,uint32_t to,const uint8_t* payload,uint32_t size);
    void send_join(double now);
    void send_bye(Link& l);
    void receive(const uint8_t ip[4],uint16_t port,const uint8_t* data,uint32_t size,double now);
    void on_join(const uint8_t ip[4],uint16_t port,uint32_t instance,const uint8_t* body,uint32_t size,double now);
    void on_welcome(const uint8_t ip[4],uint16_t port,uint32_t session,uint32_t instance,const uint8_t* body,uint32_t size,double now);
    void on_link(Link& l,const uint8_t* body,uint32_t size,double now);
    void on_message(Link& l,const uint8_t* message,uint32_t size,bool reliable);
    void on_control(uint32_t from,const uint8_t* payload,uint32_t size,int32_t via);
    void drop_link(Link& l,uint32_t reason);
    void announce_peer(uint32_t about,bool present,uint32_t reason,int32_t except_link);
    void announce_channels(uint32_t about,int32_t only_link,int32_t except_link);
    void check_channels();
    void lose_host(uint32_t reason,double now);
    void end(uint32_t reason);
    void emit(uint32_t type,uint32_t peer,uint32_t reason,uint32_t channel=0,const void* data=nullptr,uint32_t size=0);
    // session_secure.cpp
    void take_secret(const char* owner);
    void new_nonce(uint8_t out[SESSION_NONCE_BYTES]);
    uint32_t seal_for(Link& l,const uint8_t* d,uint32_t n,uint8_t* out);
    bool open_from(Link& l,const uint8_t* data,uint32_t size,uint8_t* out,uint32_t& n);
    uint32_t seal_join(const uint8_t* d,uint32_t n,uint8_t* out);
    bool open_join(const uint8_t* data,uint32_t size,uint8_t* out,uint32_t& n,uint8_t nonce[SESSION_NONCE_BYTES]);
    bool link_keys(Link& l,const uint8_t guest_nonce[SESSION_NONCE_BYTES],const uint8_t host_nonce[SESSION_NONCE_BYTES],uint32_t link_number,bool host);
    uint32_t seal_welcome(Link& l,const uint8_t guest_nonce[SESSION_NONCE_BYTES],const uint8_t* d,uint32_t n,uint8_t* out,bool fresh);
    bool open_welcome(Link& l,const uint8_t* data,uint32_t size,uint8_t* out,uint32_t& n);
    void send_on(Link& l,const uint8_t* d,uint32_t n);
    // session_traversal.cpp
    struct Punch {uint8_t ip[4];uint16_t port;double until,sent;};
    bool from_stun(const uint8_t ip[4],uint16_t port) const;
    void on_stun(const uint8_t* data,uint32_t size);
    void poll_traversal(double now);
    void send_stun();
    void reset_traversal();

    const NetBackend* (*net_)();
    uint32_t state_=WIRE_SESSION_NONE;
    char owner_[WIRE_ID_BYTES]{};
    int32_t socket_=-1;
    uint16_t port_=0,target_port_=0;
    uint8_t target_ip_[4]{};bool searching_=false,search_origin_=false;uint16_t search_port_=0;
    uint32_t self_=WIRE_PEER_NONE,max_peers_=WIRE_SESSION_MAX_PEERS,session_id_=0,instance_=0,refused_=0;
    char name_[WIRE_SESSION_NAME_BYTES]{};
    double started_=0,join_sent_=-1,last_poll_=-1,busy_until_=-1;
    bool busy_=false;
    float clock_offset_=0;bool have_offset_=false;float offset_round_trip_=-1;
    PeerRecord peers_[WIRE_SESSION_MAX_PEERS];
    Link links_[WIRE_SESSION_MAX_PEERS-1];
    Subscription subs_[WIRE_SESSION_MAX_SUBSCRIPTIONS];
    uint32_t announced_count_=0;Channel announced_[WIRE_SESSION_MAX_SUBSCRIPTIONS];
    DispatchGuard guard_;
    bool polling_=false;
    bool stop_pending_=false;
    Queued queued_[kQueued]{};uint32_t queued_count_=0;
    uint32_t seed_=0x9E3779B9u,dropped_=0;
    // A secured session.
    uint8_t secret_[SESSION_SECRET_BYTES]{};bool secret_set_=false;
    uint8_t next_secret_[SESSION_SECRET_BYTES]{};bool next_secret_set_=false;char next_secret_owner_[WIRE_ID_BYTES]{};
    uint8_t join_nonce_[SESSION_NONCE_BYTES]{};uint64_t join_counter_=0;uint32_t links_made_=0;
    uint8_t joining_nonce_[SESSION_NONCE_BYTES]{};   // the host: the nonce of the JOIN being handled
    const Link* welcome_keys_=nullptr;               // the guest: the keys of the WELCOME being handled
    // Getting through routers.
    uint8_t stun_ip_[4]{};uint16_t stun_port_=0;
    uint8_t stun_id_[12]{};      // the question's transaction number
    double stun_sent_=-1;uint32_t stun_tries_=0;
    uint8_t public_ip_[4]{};uint16_t public_port_=0;
    bool have_public_=false,stun_failed_=false;
    Punch punches_[WIRE_SESSION_MAX_PEERS-1]{};
};

}
