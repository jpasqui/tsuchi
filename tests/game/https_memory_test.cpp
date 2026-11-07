#include <assert.h>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <thread>
#include "https_memory.h"
#include "https_jobs.h"
using namespace tsuchi;
namespace {
https_memory::Memory memory;
std::atomic<bool> fail{false};
void* take(uint64_t n) { return fail.load() ? nullptr : std::malloc(size_t(n)); }
void drop(void* p) { std::free(p); }
void* response(uint64_t n) { return memory.allocate(n,true); }
void release(void* p) { memory.free(p); }
}
int main() {
    static_assert(sizeof(https_memory::Memory)<256,"no static payload pool");
    assert(memory.snapshot().total==0 && !memory.allocate(10));
    assert(memory.configure({take,drop}));
    assert(!memory.configure({nullptr,drop}));
    fail=true; assert(!memory.allocate(1024));fail=false;
    assert(memory.snapshot().total==0 && memory.snapshot().blocks==0);
    assert(!memory.allocate(UINT64_MAX) && !memory.allocate(0));

    // A declared 4 MiB limit must not reserve 4 MiB for a seven-byte body.
    https::HttpsBody tiny{};assert(memory.snapshot().total==0);
    assert(https::https_body_append(tiny,0,"captain",7,7,response));
    assert(tiny.allocated==7 && memory.snapshot().responses==7);
    char small[8]{};assert(https::https_body_read(tiny,7,2,small,3)&&!memcmp(small,"pta",3));
    assert(!https::https_body_read(tiny,7,6,small,2));
    https::https_body_free(tiny,release);assert(memory.snapshot().total==0);

    // Reads cross chunk boundaries; failed/cancelled bodies disappear before
    // the terminal state is visible, and disposal can safely run again.
    static unsigned char data[70000];for(unsigned i=0;i<sizeof data;++i)data[i]=uint8_t(i);
    https::HttpsJob job{};
    assert(https::https_body_append(job.body,0,data,sizeof data,https::HTTPS_BODY_LIMIT,response));
    assert(job.body.allocated==2*https::HTTPS_BODY_CHUNK);
    unsigned char copied[40]{};
    assert(https::https_body_read(job.body,sizeof data,65520,copied,sizeof copied));
    assert(!memcmp(copied,data+65520,sizeof copied));
    job.state.store(https::HTTPS_RUNNING);job.cancelled.store(1);
    https::https_finish(job,0,0,release);
    assert(job.state.load()==https::HTTPS_CANCELLED && !job.body.allocated && !memory.snapshot().responses);
    https::https_body_free(job.body,release);assert(memory.snapshot().total==0);

    // Eight retained 4 MiB responses fit within the response budget. TLS and
    // stacks retain separate headroom under the combined ceiling.
    https::HttpsBody bodies[8]{};
    void* stacks[4]{};for(auto*& p:stacks){p=memory.allocate(256*1024+4095);assert(p);}
    for(auto& body:bodies) for(uint64_t at=0;at<https::HTTPS_BODY_LIMIT;at+=65536)
        assert(https::https_body_append(body,at,data,65536,https::HTTPS_BODY_LIMIT,response));
    assert(memory.snapshot().responses==https_memory::kResponseLimit);
    assert(!response(1));
    void* workspace=memory.allocate(2*1024*1024);assert(workspace);
    assert(memory.snapshot().total<=https_memory::kTotalLimit);
    https::https_body_free(bodies[3],release);
    assert(https::https_body_append(bodies[3],0,"x",1,1,response));
    for(auto& body:bodies)https::https_body_free(body,release);
    release(workspace);for(auto* p:stacks)release(p);
    assert(memory.snapshot().total==0 && memory.snapshot().blocks==0);

    // Varying allocations and out-of-order frees return memory to the backend
    // instead of leaving fragments in a permanent TLS arena.
    for(unsigned iteration=0;iteration<1000;++iteration){
        void* values[8]{};for(unsigned i=0;i<8;++i){values[i]=memory.allocate((iteration*37+i*117)%50000+1,i&1);assert(values[i]);}
        for(unsigned i=0;i<8;++i)release(values[(i*3)%8]);
    }
    std::thread workers[4];
    for(auto& thread:workers)thread=std::thread([]{
        for(unsigned i=0;i<2000;++i){void* p=memory.allocate((i*13)%65536+1,i&1);assert(p);release(p);}
    });
    for(auto& thread:workers)thread.join();
    assert(memory.snapshot().total==0 && memory.snapshot().responses==0 && memory.snapshot().blocks==0);
    https_memory::Memory limited(1024,512);assert(limited.configure({take,drop}));
    void* p=limited.allocate(512,true);assert(p&&!limited.allocate(1,true));
    assert(!limited.allocate(512));limited.free(p);assert(!limited.snapshot().total);
    const auto stats=memory.snapshot();
    std::printf("PASS HTTPS on-demand budgets, eight retained 4 MiB bodies, chunk reads, cancellation, exhaustion, rollback, fragmentation and concurrency; peak_total=%llu peak_responses=%llu final_total=%llu\n",(unsigned long long)stats.peak_total,(unsigned long long)stats.peak_responses,(unsigned long long)stats.total);
}
