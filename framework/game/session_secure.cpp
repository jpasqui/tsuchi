#include "session_wire.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
// A secured session (DESIGN.md, "Secured sessions"): every JOIN, WELCOME, LINK, DATA and BYE is sealed with
// AES-256-GCM under keys made with HKDF-SHA256 from the session's 32-byte secret. Freestanding.
//
//   sealed JOIN     header (type | SEALED), guest nonce 16, counter 8, ciphertext, tag 16
//                   key: HKDF(secret, salt = guest nonce, "tsuchi join"); the counter grows with each JOIN sent
//   sealed WELCOME  header (type | SEALED), host nonce 16, link number 4, ciphertext, tag 16
//                   keys: HKDF(secret, salt = guest nonce | host nonce | link number, "tsuchi link"): 64 bytes,
//                   the guest's sending key and then the host's. WELCOME is the host's counter 0.
//   sealed others   header (type | SEALED), counter 8, ciphertext, tag 16, under the sender's link key
//
// The header (and for a JOIN its nonce) is authenticated with the body. The GCM nonce is four zero bytes and
// the counter; each key seals each counter once. The link number counts the host's links, so two links never
// share keys even if two nonces repeat.
namespace tsuchi::session_wire {
namespace {

constexpr uint32_t TAG = 16;
constexpr uint32_t COUNTER = 8;
constexpr uint32_t WINDOW = 64;   // counters behind the newest that may still arrive

void put64(uint8_t* at, uint64_t v) {
    for (int i = 0; i < 8; ++i) at[i] = uint8_t(v >> (56 - 8 * i));
}

uint64_t get64(const uint8_t* at) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | at[i];
    return v;
}

void put32(uint8_t* at, uint32_t v) {
    for (int i = 0; i < 4; ++i) at[i] = uint8_t(v >> (24 - 8 * i));
}

uint32_t get32(const uint8_t* at) {
    return (uint32_t(at[0]) << 24) | (uint32_t(at[1]) << 16) | (uint32_t(at[2]) << 8) | at[3];
}

// HKDF-SHA256 (RFC 5869), up to 64 bytes of output.
bool hkdf(const uint8_t secret[SESSION_SECRET_BYTES], const uint8_t* salt, uint32_t salt_n, const char* info,
          uint8_t* out, uint32_t n) {
    const mbedtls_md_info_t* sha = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!sha || n > 64) return false;
    uint8_t prk[32];
    if (mbedtls_md_hmac(sha, salt, salt_n, secret, SESSION_SECRET_BYTES, prk) != 0) return false;
    uint32_t info_n = 0;
    while (info[info_n]) ++info_n;
    uint8_t block[32 + 32 + 1];
    uint8_t previous[32];
    uint32_t previous_n = 0;
    for (uint8_t i = 1, done = 0; done < n; ++i) {
        uint32_t k = 0;
        for (uint32_t j = 0; j < previous_n; ++j) block[k++] = previous[j];
        for (uint32_t j = 0; j < info_n && k < sizeof block - 1; ++j) block[k++] = uint8_t(info[j]);
        block[k++] = i;
        if (mbedtls_md_hmac(sha, prk, sizeof prk, block, k, previous) != 0) return false;
        previous_n = 32;
        for (uint32_t j = 0; j < 32 && done < n; ++j) out[done++] = previous[j];
    }
    return true;
}

// AES-256-GCM with the counter as the nonce. Sealing writes the ciphertext and then the tag.
bool seal_gcm(const uint8_t key[32], uint64_t counter, const uint8_t* aad, uint32_t aad_n, const uint8_t* in, uint32_t n,
              uint8_t* out) {
    uint8_t nonce[12] = {};
    put64(nonce + 4, counter);
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int result = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (result == 0)
        result = mbedtls_gcm_crypt_and_tag(&context, MBEDTLS_GCM_ENCRYPT, n, nonce, sizeof nonce, aad, aad_n, in, out, TAG, out + n);
    mbedtls_gcm_free(&context);
    return result == 0;
}

// Opens n bytes of ciphertext followed by their tag. False, and nothing to use in out, when the tag is wrong.
bool open_gcm(const uint8_t key[32], uint64_t counter, const uint8_t* aad, uint32_t aad_n, const uint8_t* in, uint32_t n,
              uint8_t* out) {
    uint8_t nonce[12] = {};
    put64(nonce + 4, counter);
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int result = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (result == 0) result = mbedtls_gcm_auth_decrypt(&context, n, nonce, sizeof nonce, aad, aad_n, in + n, TAG, in, out);
    mbedtls_gcm_free(&context);
    return result == 0;
}

// A replay window: true, and the counter marked, for a counter not seen before and not too old.
bool fresh(uint64_t& top, uint64_t& window, bool& any, uint64_t counter) {
    if (!any || counter > top) {
        const uint64_t shift = any ? counter - top : WINDOW;
        window = shift >= WINDOW ? 0 : window << shift;
        window |= 1;
        top = counter;
        any = true;
        return true;
    }
    const uint64_t behind = top - counter;
    if (behind >= WINDOW || (window >> behind) & 1) return false;
    window |= uint64_t(1) << behind;
    return true;
}

} // namespace

TSUCHI_Result Session::set_secret(const char* owner, const uint8_t* secret, uint32_t size) {
    if (!valid_id(owner) || (size != 0 && size != SESSION_SECRET_BYTES) || (size && !secret)) return TSUCHI_INVALID;
    if (guard_.active()) return TSUCHI_BUSY;
    copy(next_secret_owner_, owner);
    next_secret_set_ = size != 0;
    for (uint32_t i = 0; i < SESSION_SECRET_BYTES; ++i) next_secret_[i] = size ? secret[i] : 0;
    return TSUCHI_OK;
}

// At start: the secret the starting mod set, if it set one.
void Session::take_secret(const char* owner) {
    secret_set_ = next_secret_set_ && same(next_secret_owner_, owner);
    for (uint32_t i = 0; i < SESSION_SECRET_BYTES; ++i) secret_[i] = secret_set_ ? next_secret_[i] : 0;
    links_made_ = 0;
}

void Session::new_nonce(uint8_t out[SESSION_NONCE_BYTES]) {
    for (uint32_t i = 0; i < SESSION_NONCE_BYTES; i += 4) {
        seed_ ^= seed_ << 13;
        seed_ ^= seed_ >> 17;
        seed_ ^= seed_ << 5;
        put32(out + i, seed_ ^ instance_ ^ (i * 0x9E3779B9u));
    }
}

// A datagram for a link, sealed when the session is: header, then body. False when it could not be sealed.
uint32_t Session::seal_for(Link& l, const uint8_t* d, uint32_t n, uint8_t* out) {
    if (!secret_set_) {
        for (uint32_t i = 0; i < n; ++i) out[i] = d[i];
        return n;
    }
    if (n < SESSION_HEADER_BYTES || n + SESSION_SEAL > SESSION_DATAGRAM) return 0;
    for (uint32_t i = 0; i < SESSION_HEADER_BYTES; ++i) out[i] = d[i];
    out[6] |= SESSION_SEALED;
    const uint64_t counter = l.send_counter++;
    put64(out + SESSION_HEADER_BYTES, counter);
    const uint32_t body = n - SESSION_HEADER_BYTES;
    uint8_t* sealed = out + SESSION_HEADER_BYTES + COUNTER;
    if (!seal_gcm(l.send_key, counter, out, SESSION_HEADER_BYTES, d + SESSION_HEADER_BYTES, body, sealed)) return 0;
    return SESSION_HEADER_BYTES + COUNTER + body + TAG;
}

// The body of a sealed datagram from a link, opened into out. False for anything forged, damaged or replayed.
bool Session::open_from(Link& l, const uint8_t* data, uint32_t size, uint8_t* out, uint32_t& n) {
    if (size < SESSION_HEADER_BYTES + COUNTER + TAG) return false;
    const uint64_t counter = get64(data + SESSION_HEADER_BYTES);
    const uint32_t body = size - SESSION_HEADER_BYTES - COUNTER - TAG;
    const uint8_t* sealed = data + SESSION_HEADER_BYTES + COUNTER;
    uint64_t top = l.receive_top, window = l.receive_window;
    bool any = l.received_any;
    if (!fresh(top, window, any, counter)) return false;   // checked before opening, marked only after
    if (!open_gcm(l.receive_key, counter, data, SESSION_HEADER_BYTES, sealed, body, out)) return false;
    l.receive_top = top, l.receive_window = window, l.received_any = any;
    n = body;
    return true;
}

// A guest's JOIN, sealed with the join key of its nonce.
uint32_t Session::seal_join(const uint8_t* d, uint32_t n, uint8_t* out) {
    if (n < SESSION_HEADER_BYTES || n + SESSION_NONCE_BYTES + COUNTER + TAG > SESSION_DATAGRAM) return 0;
    uint8_t key[32];
    if (!hkdf(secret_, join_nonce_, SESSION_NONCE_BYTES, "tsuchi join", key, sizeof key)) return 0;
    for (uint32_t i = 0; i < SESSION_HEADER_BYTES; ++i) out[i] = d[i];
    out[6] |= SESSION_SEALED;
    for (uint32_t i = 0; i < SESSION_NONCE_BYTES; ++i) out[SESSION_HEADER_BYTES + i] = join_nonce_[i];
    const uint64_t counter = join_counter_++;
    put64(out + SESSION_HEADER_BYTES + SESSION_NONCE_BYTES, counter);
    const uint32_t aad = SESSION_HEADER_BYTES + SESSION_NONCE_BYTES;
    const uint32_t body = n - SESSION_HEADER_BYTES;
    uint8_t* sealed = out + aad + COUNTER;
    if (!seal_gcm(key, counter, out, aad, d + SESSION_HEADER_BYTES, body, sealed)) return 0;
    return aad + COUNTER + body + TAG;
}

// The host: a sealed JOIN's body, and its nonce. False for one not sealed with this session's secret.
bool Session::open_join(const uint8_t* data, uint32_t size, uint8_t* out, uint32_t& n, uint8_t nonce[SESSION_NONCE_BYTES]) {
    const uint32_t aad = SESSION_HEADER_BYTES + SESSION_NONCE_BYTES;
    if (size < aad + COUNTER + TAG) return false;
    for (uint32_t i = 0; i < SESSION_NONCE_BYTES; ++i) nonce[i] = data[SESSION_HEADER_BYTES + i];
    uint8_t key[32];
    if (!hkdf(secret_, nonce, SESSION_NONCE_BYTES, "tsuchi join", key, sizeof key)) return false;
    const uint64_t counter = get64(data + aad);
    const uint32_t body = size - aad - COUNTER - TAG;
    const uint8_t* sealed = data + aad + COUNTER;
    if (!open_gcm(key, counter, data, aad, sealed, body, out)) return false;
    n = body;
    return true;
}

// The keys of one link from both nonces and the host's link number. The guest sends with the first half.
bool Session::link_keys(Link& l, const uint8_t guest_nonce[SESSION_NONCE_BYTES], const uint8_t host_nonce[SESSION_NONCE_BYTES],
                        uint32_t link_number, bool host) {
    uint8_t salt[SESSION_NONCE_BYTES * 2 + 4];
    for (uint32_t i = 0; i < SESSION_NONCE_BYTES; ++i) {
        salt[i] = guest_nonce[i];
        salt[SESSION_NONCE_BYTES + i] = host_nonce[i];
    }
    put32(salt + SESSION_NONCE_BYTES * 2, link_number);
    uint8_t keys[64];
    if (!hkdf(secret_, salt, sizeof salt, "tsuchi link", keys, sizeof keys)) return false;
    for (uint32_t i = 0; i < 32; ++i) {
        l.send_key[i] = host ? keys[32 + i] : keys[i];
        l.receive_key[i] = host ? keys[i] : keys[32 + i];
    }
    l.send_counter = 0;
    l.receive_top = 0, l.receive_window = 0, l.received_any = false;
    return true;
}

// The host's WELCOME on a new link, sealed with the link's own keys. A fresh WELCOME makes the keys from a new
// host nonce and link number; a repeated one (the first was lost) is the same datagram again.
uint32_t Session::seal_welcome(Link& l, const uint8_t guest_nonce[SESSION_NONCE_BYTES], const uint8_t* d, uint32_t n,
                               uint8_t* out, bool fresh) {
    if (n < SESSION_HEADER_BYTES || n + SESSION_NONCE_BYTES + 4 + TAG > SESSION_DATAGRAM) return 0;
    if (fresh) {
        new_nonce(l.host_nonce);
        l.link_number = ++links_made_;
        if (!link_keys(l, guest_nonce, l.host_nonce, l.link_number, true)) return 0;
        l.send_counter = 1;   // the WELCOME is counter 0
    }
    for (uint32_t i = 0; i < SESSION_HEADER_BYTES; ++i) out[i] = d[i];
    out[6] |= SESSION_SEALED;
    for (uint32_t i = 0; i < SESSION_NONCE_BYTES; ++i) out[SESSION_HEADER_BYTES + i] = l.host_nonce[i];
    put32(out + SESSION_HEADER_BYTES + SESSION_NONCE_BYTES, l.link_number);
    const uint32_t aad = SESSION_HEADER_BYTES + SESSION_NONCE_BYTES + 4;
    const uint32_t body = n - SESSION_HEADER_BYTES;
    if (!seal_gcm(l.send_key, 0, out, aad, d + SESSION_HEADER_BYTES, body, out + aad)) return 0;
    return aad + body + TAG;
}

// Sends a datagram to a link, sealed when the session is secured.
void Session::send_on(Link& l, const uint8_t* d, uint32_t n) {
    uint8_t out[SESSION_DATAGRAM];
    const uint32_t m = seal_for(l, d, n, out);
    if (m) send_raw(l.ip, l.port, out, m);
}

// The guest: a sealed WELCOME's body, opened with the keys it makes for link l. False for a forged one.
bool Session::open_welcome(Link& l, const uint8_t* data, uint32_t size, uint8_t* out, uint32_t& n) {
    const uint32_t aad = SESSION_HEADER_BYTES + SESSION_NONCE_BYTES + 4;
    if (size < aad + TAG) return false;
    const uint8_t* host_nonce = data + SESSION_HEADER_BYTES;
    const uint32_t link_number = get32(data + SESSION_HEADER_BYTES + SESSION_NONCE_BYTES);
    Link trial = l;
    if (!link_keys(trial, join_nonce_, host_nonce, link_number, false)) return false;
    const uint32_t body = size - aad - TAG;
    const uint8_t* sealed = data + aad;
    if (!open_gcm(trial.receive_key, 0, data, aad, sealed, body, out)) return false;
    uint64_t top = 0, window = 0;
    bool any = false;
    fresh(top, window, any, 0);
    trial.receive_top = top, trial.receive_window = window, trial.received_any = any;
    for (uint32_t i = 0; i < 32; ++i) {
        l.send_key[i] = trial.send_key[i];
        l.receive_key[i] = trial.receive_key[i];
    }
    l.send_counter = trial.send_counter;
    l.receive_top = trial.receive_top, l.receive_window = trial.receive_window, l.received_any = trial.received_any;
    n = body;
    return true;
}

} // namespace tsuchi::session_wire
