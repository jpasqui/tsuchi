#pragma once
#include "https_jobs.h"
#include "https_memory.h"
#include <stdint.h>
namespace tsuchi::https_platform {
// Worker platform for an independent TLS provider. Initialization is local and
// starts resolver helpers only on first HTTPS use; no DNS on game thread.
bool initialize();
bool start_worker(void(*)(void*),void*); // persistent owned SDK thread/stack
// Address arbitration waits indefinitely while the word matches; wake broadcasts.
void wait_for_work(const uint32_t*,uint32_t);
void wake_for_work(const uint32_t*);
void* allocate(uint64_t);
void free(void*);
void* response_allocate(uint64_t);
void response_free(void*);
https_memory::Snapshot memory_snapshot();
bool random(void*,uint64_t);
bool entropy_seed(uint8_t out[32]); // worker-only: the gathered seed (see https_platform.cpp)
uint32_t entropy_detail_diagnostic(uint32_t); // first round: most common, predicted, lowest, highest; 4: rounds
uint32_t entropy_diagnostic();      // bits 0-2 launcher/saved/timing, bit 3 seeded, above: timing credit in bits
int64_t unix_seconds(); // negative means unavailable; never fabricate UTC
uint64_t milliseconds();
void sleep_ms(uint32_t);
// connect: 0 success, -1 network, -2 capacity/retry, -3 deadline, -4 cancel.
// DNS state owns copied names/results and never retains a caller's job pointer.
int tcp_open(const https::HttpsJob&,const char*,uint16_t,uint64_t deadline,int*);
// One nonblocking call, at most64KiB. >=0 bytes (read0 EOF), -2 would-block,
// -1 fatal. Caller controls deadlines/cancellation between calls.
int32_t tcp_read(int,void*,uint32_t);
int32_t tcp_write(int,const void*,uint32_t);
void tcp_close(int);
bool file_open(const https::HttpsJob&,void**);
bool file_write(void*,uint64_t,const void*,uint64_t);
bool file_commit(void*);
void file_abort(void*);
}

extern "C" bool tsuchi_https_random_seed(const void*,uint32_t);
extern "C" bool tsuchi_https_random_fill(void*,uint64_t);
extern "C" bool tsuchi_https_sha256(const void*,uint32_t,uint8_t*);
