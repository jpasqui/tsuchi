#include <assert.h>
#include <stdio.h>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <thread>
#include "https_work_signal.h"
using tsuchi::https::WorkSignal;
namespace {
std::mutex mutex;
std::condition_variable changed;
std::atomic<unsigned> parked{},calls{},done{};
bool force_gap=false;
WorkSignal work;
void wake(const uint32_t*){std::lock_guard<std::mutex> lock(mutex);changed.notify_all();}
void park(const uint32_t* word,uint32_t observed){
 ++calls;
 // Force publication after WorkSignal's precheck but before the OS comparison.
 if(force_gap){force_gap=false;work.notify(wake);}
 std::unique_lock<std::mutex> lock(mutex);++parked;
 assert(changed.wait_for(lock,std::chrono::seconds(5),[&]{return __atomic_load_n(word,__ATOMIC_ACQUIRE)!=observed;}));
 --parked;
}
void await(unsigned count){for(unsigned i=0;i<5000&&parked.load()!=count;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(parked==count);}
}
int main(){
 auto observed=work.sample();work.notify(wake);work.wait(observed,park);assert(calls==0);
 observed=work.sample();force_gap=true;work.wait(observed,park);assert(calls==1&&parked==0);
 std::atomic<unsigned> queued{},processed{};
 auto worker=[&]{while(!done){const auto generation=work.sample();unsigned n=queued.load();while(n&&!queued.compare_exchange_weak(n,n-1)){}if(n)++processed;else if(!done)work.wait(generation,park);}};
 std::thread a(worker),b(worker);await(2);
 const auto idle=calls.load();std::this_thread::sleep_for(std::chrono::milliseconds(30));assert(calls==idle);
 // Both sleepers must wake; one notification may publish a batch.
 queued=2;work.notify(wake);
 for(unsigned i=0;i<5000&&processed!=2;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(processed==2);await(2);
 // Exercise publication while workers take jobs and return to waiting.
 for(unsigned i=0;i<2000;++i){++queued;work.notify(wake);}
 for(unsigned i=0;i<5000&&processed!=2002;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(processed==2002);await(2);
 done=1;work.notify(wake);a.join();b.join();assert(!queued&&!parked);
 puts("PASS idle blocking, publication before/between waits, broadcast, and 2002 jobs");
}
