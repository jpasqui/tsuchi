#pragma once
#include "https_jobs.h"
namespace tsuchi::https {
struct HttpsPlatform {
    void* (*allocate)(uint64_t);
    void (*free)(void*);
    void* (*response_allocate)(uint64_t);
    void (*response_free)(void*);
    bool (*random)(void*,uint64_t);
    int64_t (*unix_seconds)();
    uint64_t (*milliseconds)();
    void (*sleep_ms)(uint32_t);
    // tcp_open observes job cancellation and deadline, including DNS/connect.
    // Return0 on success,-3 timeout,-4 cancelled,-1 other failure.
    int (*tcp_open)(const HttpsJob&,const char*,uint16_t,uint64_t,int*);
    int32_t (*tcp_read)(int,void*,uint32_t); // -2 would-block,0 EOF,-1 failure
    int32_t (*tcp_write)(int,const void*,uint32_t);
    void (*tcp_close)(int);
    bool (*file_open)(const HttpsJob&,void**);
    bool (*file_write)(void*,uint64_t,const void*,uint64_t);
    bool (*file_commit)(void*); // consumes only on success
    void (*file_abort)(void*); // consumes on error/cancellation
    const unsigned char* roots; // PEM, includes trailing NUL
    uint32_t roots_bytes;
};
// Attach once before persistent workers start; never change it during a transfer.
bool set_https_tls_platform(const HttpsPlatform*);
void https_tls_run(HttpsJob&);
uint64_t https_tls_diagnostic(uint32_t);
}
