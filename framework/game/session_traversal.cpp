#include "session_wire.h"
// Getting through routers (DESIGN.md, "Getting through routers"): the session's own STUN question, which
// tells it the address the internet sees for its socket, and the punches that open the host's router to a
// guest. Freestanding.
namespace tsuchi::session_wire {
namespace {

constexpr uint32_t STUN_COOKIE = 0x2112A442u;
constexpr uint16_t STUN_BINDING = 0x0001, STUN_ANSWER = 0x0101;
constexpr uint16_t STUN_MAPPED = 0x0001, STUN_XOR_MAPPED = 0x0020;
constexpr uint32_t STUN_HEADER = 20;
constexpr double STUN_EVERY = 0.5;       // seconds between questions until one is answered
constexpr uint32_t STUN_TRIES = 10;
constexpr double PUNCH_FOR = 5;          // seconds a host punches toward a guest
constexpr double PUNCH_EVERY = 0.2;

uint16_t get16(const uint8_t* at) {
    return uint16_t((at[0] << 8) | at[1]);
}

uint32_t get32(const uint8_t* at) {
    return (uint32_t(at[0]) << 24) | (uint32_t(at[1]) << 16) | (uint32_t(at[2]) << 8) | at[3];
}

void put16(uint8_t* at, uint16_t v) {
    at[0] = uint8_t(v >> 8);
    at[1] = uint8_t(v);
}

void put32(uint8_t* at, uint32_t v) {
    for (int i = 0; i < 4; ++i) at[i] = uint8_t(v >> (24 - 8 * i));
}

bool same_address(const uint8_t a[4], uint16_t a_port, const uint8_t b[4], uint16_t b_port) {
    return a_port == b_port && a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

bool valid_address(const WireAddress* at) {
    return at && at->size == sizeof(WireAddress) && !at->reserved && at->port;
}

} // namespace

bool Session::from_stun(const uint8_t ip[4], uint16_t port) const {
    return stun_port_ && same_address(ip, port, stun_ip_, stun_port_);
}

void Session::reset_traversal() {
    stun_port_ = 0;
    stun_sent_ = -1;
    stun_tries_ = 0;
    have_public_ = false;
    stun_failed_ = false;
    for (auto& p : punches_) p = Punch{};
}

void Session::send_stun() {
    uint8_t d[STUN_HEADER];
    put16(d, STUN_BINDING);
    put16(d + 2, 0);
    put32(d + 4, STUN_COOKIE);
    for (int i = 0; i < 12; ++i) d[8 + i] = stun_id_[i];
    send_raw(stun_ip_, stun_port_, d, STUN_HEADER);
}

// A datagram from the STUN server: the answer to this session's question, or nothing.
void Session::on_stun(const uint8_t* data, uint32_t size) {
    if (have_public_ || size < STUN_HEADER || get16(data) != STUN_ANSWER || get32(data + 4) != STUN_COOKIE) return;
    for (int i = 0; i < 12; ++i)
        if (data[8 + i] != stun_id_[i]) return;
    const uint32_t length = get16(data + 2);
    if (STUN_HEADER + length > size) return;
    bool found = false;
    uint8_t ip[4]{};
    uint16_t port = 0;
    for (uint32_t at = STUN_HEADER; at + 4 <= STUN_HEADER + length;) {
        const uint16_t type = get16(data + at);
        const uint16_t n = get16(data + at + 2);
        const uint8_t* value = data + at + 4;
        if (at + 4 + n > STUN_HEADER + length) return;
        // An IPv4 address: a reserved byte, the family 1, the port, the address.
        if ((type == STUN_XOR_MAPPED || (type == STUN_MAPPED && !found)) && n == 8 && value[1] == 1) {
            const bool xored = type == STUN_XOR_MAPPED;
            port = uint16_t(get16(value + 2) ^ (xored ? uint16_t(STUN_COOKIE >> 16) : 0));
            const uint32_t address = get32(value + 4) ^ (xored ? STUN_COOKIE : 0);
            put32(ip, address);
            found = true;
            if (xored) break;
        }
        at += 4 + ((n + 3u) & ~3u);
    }
    if (!found || !port) return;
    for (int i = 0; i < 4; ++i) public_ip_[i] = ip[i];
    public_port_ = port;
    have_public_ = true;
}

// Once per poll: the STUN question until it is answered, and the punches that are due.
void Session::poll_traversal(double now) {
    if (stun_port_ && !have_public_ && !stun_failed_ && (stun_sent_ < 0 || now - stun_sent_ >= STUN_EVERY)) {
        if (stun_tries_ == STUN_TRIES) {
            stun_failed_ = true;
        } else {
            send_stun();
            stun_sent_ = now;
            ++stun_tries_;
        }
    }
    if (state_ != WIRE_SESSION_HOSTING) return;
    uint8_t d[SESSION_HEADER_BYTES];
    const uint32_t n = header(d, SESSION_PUNCH);
    for (auto& p : punches_) {
        if (now >= p.until || (p.sent >= 0 && now - p.sent < PUNCH_EVERY)) continue;
        send_raw(p.ip, p.port, d, n);
        p.sent = now;
    }
}

TSUCHI_Result Session::discover(const char* owner, const WireAddress* server, double now) {
    if (!valid_id(owner) || !valid_address(server)) return TSUCHI_INVALID;
    if (guard_.active()) return TSUCHI_BUSY;
    if (state_ == WIRE_SESSION_NONE || !same(owner_, owner)) return TSUCHI_NOT_FOUND;
    for (int i = 0; i < 4; ++i) stun_ip_[i] = server->ip[i];
    stun_port_ = server->port;
    // A new transaction number, so a late answer to an earlier question is not taken.
    for (int i = 0; i < 12; i += 4) {
        seed_ ^= seed_ << 13;
        seed_ ^= seed_ >> 17;
        seed_ ^= seed_ << 5;
        put32(stun_id_ + i, seed_ ^ instance_);
    }
    stun_sent_ = -1;
    stun_tries_ = 0;
    have_public_ = false;
    stun_failed_ = false;
    poll_traversal(now);
    return TSUCHI_OK;
}

TSUCHI_Result Session::public_address(WireAddress* out) const {
    if (!out || out->size != sizeof(WireAddress)) return TSUCHI_INVALID;
    if (!guard_.permits(SESSION_OP_QUERY)) return TSUCHI_BUSY;
    if (state_ == WIRE_SESSION_NONE || !stun_port_) return TSUCHI_NOT_FOUND;
    if (stun_failed_) return TSUCHI_IO;
    if (!have_public_) return TSUCHI_NOT_READY;
    for (int i = 0; i < 4; ++i) out->ip[i] = public_ip_[i];
    out->port = public_port_;
    return TSUCHI_OK;
}

TSUCHI_Result Session::punch(const char* owner, const WireAddress* to, double now) {
    if (!valid_id(owner) || !valid_address(to)) return TSUCHI_INVALID;
    if (guard_.active()) return TSUCHI_BUSY;
    if (state_ != WIRE_SESSION_HOSTING || !same(owner_, owner)) return TSUCHI_NOT_FOUND;
    // Again toward the same guest starts its five seconds over; a new guest takes a free or the oldest slot.
    Punch* slot = nullptr;
    for (auto& p : punches_)
        if (same_address(p.ip, p.port, to->ip, to->port)) slot = &p;
    for (auto& p : punches_)
        if (!slot && now >= p.until) slot = &p;
    if (!slot) {
        slot = &punches_[0];
        for (auto& p : punches_)
            if (p.until < slot->until) slot = &p;
    }
    for (int i = 0; i < 4; ++i) slot->ip[i] = to->ip[i];
    slot->port = to->port;
    slot->until = now + PUNCH_FOR;
    slot->sent = -1;
    poll_traversal(now);
    return TSUCHI_OK;
}

} // namespace tsuchi::session_wire
