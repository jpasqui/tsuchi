#pragma once
#include <stdint.h>

namespace tsuchi::https_memory {
constexpr uint64_t kTotalLimit = 48u * 1024u * 1024u;
constexpr uint64_t kResponseLimit = 32u * 1024u * 1024u;
struct Backend { void* (*allocate)(uint64_t); void (*free)(void*); };
struct Snapshot {
    uint64_t total, responses, peak_total, peak_responses, blocks, failures;
};

// The backend is captured before workers start. No game metadata is read here.
// Allocation headers and alignment count toward the total, and response payloads
// have their own limit. Reserving before a backend call bounds concurrent growth.
class Memory {
    struct alignas(16) Header { uint64_t magic, bytes, charged, response; };
    static constexpr uint64_t kMagic = 0x5453554348494e45ull;
    Backend backend_{};
    uint64_t total_limit_, response_limit_;
    uint32_t lock_ = 0;
    Snapshot state_{};
    void lock() { while (__atomic_exchange_n(&lock_, 1u, __ATOMIC_ACQUIRE)) {} }
    void unlock() { __atomic_store_n(&lock_, 0u, __ATOMIC_RELEASE); }
    void unreserve(uint64_t charged, uint64_t response) {
        state_.total -= charged; state_.responses -= response; --state_.blocks;
    }
public:
    explicit constexpr Memory(uint64_t total = kTotalLimit, uint64_t responses = kResponseLimit)
        : total_limit_(total), response_limit_(responses) {}
    bool configure(Backend backend) {
        if (!backend.allocate || !backend.free) return false;
        lock();
        const bool permitted = !backend_.allocate ||
            (backend_.allocate == backend.allocate && backend_.free == backend.free);
        if (permitted) backend_ = backend;
        unlock(); return permitted;
    }
    void* allocate(uint64_t bytes, bool response = false) {
        if (!bytes || bytes > UINT64_MAX - sizeof(Header) - 15) return nullptr;
        const uint64_t charged = ((bytes + 15) & ~uint64_t(15)) + sizeof(Header);
        lock();
        if (!backend_.allocate || charged > total_limit_ - state_.total ||
            (response && bytes > response_limit_ - state_.responses)) {
            ++state_.failures; unlock(); return nullptr;
        }
        state_.total += charged; state_.responses += response ? bytes : 0; ++state_.blocks;
        if (state_.total > state_.peak_total) state_.peak_total = state_.total;
        if (state_.responses > state_.peak_responses) state_.peak_responses = state_.responses;
        unlock();
        // The backend may block. Do not hold the accounting lock across it.
        auto* header = static_cast<Header*>(backend_.allocate(charged));
        if (!header) {
            lock(); unreserve(charged, response ? bytes : 0); ++state_.failures; unlock();
            return nullptr;
        }
        *header = {kMagic, bytes, charged, response ? bytes : 0};
        return header + 1;
    }
    void free(void* pointer) {
        if (!pointer) return;
        auto* header = static_cast<Header*>(pointer) - 1;
        if (header->magic != kMagic) return;
        const uint64_t charged = header->charged, response = header->response;
        header->magic = 0;
        backend_.free(header);
        lock(); unreserve(charged, response); unlock();
    }
    Snapshot snapshot() { lock(); const Snapshot result = state_; unlock(); return result; }
    void reset_peaks() { lock(); state_.peak_total = state_.total; state_.peak_responses = state_.responses; unlock(); }
};
}
