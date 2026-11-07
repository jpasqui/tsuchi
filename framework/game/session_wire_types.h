#pragma once
#include <stdint.h>
#include <string.h>
#include "registry.h"
#include "network_transport.h"
namespace tsuchi::session_wire {
constexpr uint32_t WIRE_ID_BYTES=48;
struct WireAddress {uint32_t size; uint8_t ip[4]; uint16_t port,reserved;};
bool session_owner_active(const char* id);
inline bool owner_active(const char* id) {return session_owner_active(id);}
inline bool owner_available(const char* id) {return session_owner_active(id);}
inline bool valid_id(const char* id) {return valid_mod_id(id);}
template<class T> inline void copy(T* to,const T* from){do{*to++=*from;}while(*from++);}
template<class T> inline bool same(const T* a,const T* b){if(!a||!b)return false;while(*a&&*a==*b){++a;++b;}return *a==*b;}
template<class T> T* arena_zeroed(){auto* p=static_cast<T*>(allocate(sizeof(T)));if(p)memset(p,0,sizeof(T));return p;}
struct HandleSlot {uint32_t generation=1;bool retired=false;uint32_t handle(uint32_t slot)const{return(generation<<8)|(slot+1);}bool matches(uint32_t h)const{return !retired&&(h>>8)==generation;}void invalidate(){if(generation==0xffffff)retired=true;else ++generation;}};
inline uint32_t handle_slot(uint32_t h,uint32_t capacity){const uint32_t i=h&255;return i&&i<=capacity?i-1:UINT32_MAX;}
class DispatchGuard {uint32_t depth_=0,allowed_=0;public:bool active()const{return depth_!=0;}bool permits(uint32_t op)const{return !depth_||(allowed_&op)==op;}class Scope{DispatchGuard& g_;uint32_t saved_;public:Scope(DispatchGuard& g,uint32_t allowed):g_(g),saved_(g.allowed_){++g_.depth_;g_.allowed_=allowed;}~Scope(){--g_.depth_;g_.allowed_=saved_;}};};
constexpr uint32_t WIRE_SESSION_MAX_PEERS=4u; /* the host and three guests */
constexpr uint32_t WIRE_SESSION_MAX_MESSAGE=1024u; /* bytes in one message, reliable or not */
constexpr uint32_t WIRE_SESSION_MAX_SUBSCRIPTIONS=16u; /* one per mod */
constexpr uint32_t WIRE_SESSION_DEFAULT_PORT=47700u;
constexpr float WIRE_SESSION_TIMEOUT=20.0f;           /* seconds */
constexpr uint32_t WIRE_SESSION_NAME_BYTES=32u;
constexpr uint32_t WIRE_PEER_HOST=0u;
constexpr uint32_t WIRE_PEER_ALL=255u; /* send: every other peer */
constexpr uint32_t WIRE_PEER_NONE=255u; /* info: no peer number yet */

enum {
    WIRE_SESSION_NONE=0u,      /* not in a session */
    WIRE_SESSION_HOSTING=1u,   /* this machine hosts (with or without guests) */
    WIRE_SESSION_JOINING=2u,   /* looking for its host, or waiting for its answer */
    WIRE_SESSION_JOINED=3u     /* a guest in its host's session */
};
enum { WIRE_SESSION_SEND_RELIABLE=1u };   /* send flags: delivered once, in order */
enum {
    WIRE_SESSION_EVENT_STARTED=1u,     /* this machine is in: hosting began, or the host took it
                                         (again, after a lost host); peer is its own number */
    WIRE_SESSION_EVENT_PEER_JOINED=2u, /* another peer is in the session */
    WIRE_SESSION_EVENT_PEER_LEFT=3u,   /* reason: WIRE_SESSION_LEFT_* */
    WIRE_SESSION_EVENT_MESSAGE=4u,     /* a message on this mod's channel from peer */
    WIRE_SESSION_EVENT_CHANNELS=5u,    /* peer's channels changed (a mod started or stopped there) */
    WIRE_SESSION_EVENT_ENDED=6u        /* this machine left the session (leave, or its starter stopped) */
};
enum {
    WIRE_SESSION_LEFT_BYE=1u,       /* it left */
    WIRE_SESSION_LEFT_TIMEOUT=2u,   /* not heard for WIRE_SESSION_TIMEOUT seconds */
    WIRE_SESSION_LEFT_ENDED=3u      /* this machine left, so every peer is gone */
};
enum {
    WIRE_SESSION_REFUSED_FULL=1u,      /* the host has WIRE_SESSION_MAX_PEERS (or its own limit) */
    WIRE_SESSION_REFUSED_VERSION=2u    /* the other machine runs another session protocol */
};

typedef struct WireOptions {
    uint32_t size;                       /* sizeof(WireOptions) */
    uint16_t port;                       /* host: the port it listens on; guest: the host's port
                                            when looking for one. 0: WIRE_SESSION_DEFAULT_PORT */
    uint16_t max_peers;                  /* host: 2..WIRE_SESSION_MAX_PEERS; 0: the maximum */
    char name[WIRE_SESSION_NAME_BYTES];   /* this player's name for the others ("" for none) */
} WireOptions;

typedef struct WireInfo {
    uint32_t size;                       /* sizeof(WireInfo) */
    uint32_t state;                      /* WIRE_SESSION_NONE, _HOSTING, _JOINING or _JOINED */
    uint32_t self;                       /* this machine's peer number, or WIRE_PEER_NONE */
    uint32_t peers;                      /* bit n set: peer n is in the session (self included) */
    uint32_t id;                         /* the session's number, picked by the host (0 before) */
    uint32_t refused;                    /* JOINING: why the host last refused (WIRE_SESSION_REFUSED_*), or 0 */
    float time;                          /* the shared clock, seconds: the host's session time */
    uint16_t port;                       /* the local port */
    uint16_t reserved;
    char owner[WIRE_ID_BYTES];            /* the mod that started it ("" when none) */
} WireInfo;

typedef struct WirePeer {
    uint32_t size;                       /* sizeof(WirePeer) */
    WireAddress address;              /* guests see only the host's address; others' are zero */
    char name[WIRE_SESSION_NAME_BYTES];
    float round_trip;                    /* seconds, smoothed; -1 until measured (host and guests
                                            measure only the link between them) */
    float heard;                         /* seconds since its last datagram (0 for this machine) */
    uint32_t pending;                    /* reliable messages to it not yet acknowledged */
    uint32_t reserved;
} WirePeer;

typedef struct WireEvent {
    uint32_t size, type;                 /* sizeof(WireEvent); WIRE_SESSION_EVENT_* */
    uint32_t peer;                       /* who; STARTED: this machine's own number */
    uint32_t reason;                     /* PEER_LEFT: WIRE_SESSION_LEFT_*; MESSAGE: send flags;
                                            ENDED: WIRE_SESSION_LEFT_ENDED */
    const void *data;                    /* MESSAGE: the bytes, borrowed for the callback */
    uint32_t data_size;
    uint32_t reserved;
} WireEvent;
typedef void (*WireCallback)(void *user, const WireEvent *event);


}
