#include "https_platform.h"
#include "https_memory.h"
#include "https_entropy_pool.h"
#include "https_work_signal.h"
#include "game.h"
#include "modules.h"
namespace tsuchi::https_platform {
namespace {
namespace t=game::table;
using CoreMask=uint64_t(*)();CoreMask available_cores=nullptr;
constexpr int kWorkerPriorityBelowGame=1;
https::HttpsAtomic<uint32_t> rng_state{},entropy_report{},entropy_detail[8]{};
https_memory::Memory memory;
void* heap=nullptr;
using HeapAllocate=void*(*)(void*,uint64_t,uint32_t);
using HeapFree=void(*)(void*,void*);
HeapAllocate heap_allocate=nullptr;HeapFree heap_free=nullptr;
void* allocate_heap(uint64_t n){return heap_allocate(heap,n,16);}
void free_heap(void* p){heap_free(heap,p);}
https::HttpsAtomic<uint64_t> init_diagnostic[7]{};
constexpr int AF_INET=2,SOCK_STREAM=1,IPPROTO_TCP=6,F_GETFL=3,F_SETFL=4,O_NONBLOCK=0x800,MSG_DONTWAIT=0x80;
constexpr int SOL_SOCKET=0xffff,SO_ERROR=0x1007;
struct Address {uint8_t len,family;uint16_t port;uint8_t ip[4],zero[8];};
struct AddrInfo {int32_t flags,family,socktype,protocol;uint32_t addrlen,pad;char* canonical;Address* address;AddrInfo* next;};
struct PollFd {int32_t fd;int16_t events,revents;};
static_assert(sizeof(Address)==16&&sizeof(AddrInfo)==48&&sizeof(PollFd)==8);
struct Dns {https::HttpsAtomic<uint32_t> state{};char name[256]{};uint8_t ip[4][4]{};uint32_t count=0;int result=-1;};
// idle0,reserved1,queued2,running3,ready4,abandoned5. Never a caller pointer.
Dns dns[4];uint32_t resolver_workers=0;
https::WorkSignal dns_work;
int error(){const int value=t::fn::socket_errno();init_diagnostic[6].store(uint32_t(value));return value;}
bool again(int e){return e==11||e==35||e==4;}
int stop(const https::HttpsJob& j,uint64_t deadline){return https::https_stopped(j)?-4:milliseconds()>=deadline?-3:0;}
void resolver(void*){
 for(;;){
  const auto observed=dns_work.sample();
  bool found=false;
  for(auto& slot:dns){
   if(!slot.state.exchange(2,3))continue;found=true;
   AddrInfo hints{};hints.family=AF_INET;hints.socktype=SOCK_STREAM;hints.protocol=IPPROTO_TCP;AddrInfo* results=nullptr;
   const int r=t::fn::socket_get_addr_info(slot.name,nullptr,&hints,&results);
   slot.count=0;slot.result=-1;
   unsigned examined=0;
   if(!r)for(const auto* row=results;row&&slot.count<4&&examined++<32;row=row->next){
    if(row->family!=AF_INET||!row->address||row->addrlen<sizeof(Address))continue;
    for(unsigned k=0;k<4;++k)slot.ip[slot.count][k]=row->address->ip[k];++slot.count;
   }
   if(results)t::fn::socket_free_addr_info(results);
   if(slot.count)slot.result=0;
   init_diagnostic[5].store((uint64_t(slot.count)<<32)|uint32_t(r));
   if(!slot.state.exchange(3,4))slot.state.store(0); // an abandoned caller cannot observe writes
  }
  if(!found)dns_work.wait(observed,wait_for_work);
 }
}
void abandon(Dns& slot){for(;;){const auto state=slot.state.load();if(state==2||state==4){if(slot.state.exchange(state,0))return;}else if(state==3){if(slot.state.exchange(3,5))return;}else return;}}
}
bool initialize(){
 static bool ready=false;
 if(ready)return true;
 // Capture the allocator on the game thread. Workers must not use the
 // reflection helpers or their game-thread memory-map cache.
 if(!heap){
  const uint64_t allocator=game::global<uint64_t>(t::data::allocator);
  const auto take=allocator?game::method(allocator,t::allocator_vtable::malloc):nullptr;
  const auto release=allocator?game::method(allocator,t::allocator_vtable::free):nullptr;
  if(!take||!release)return false;
  heap=reinterpret_cast<void*>(allocator);heap_allocate=take;heap_free=release;
  if(!memory.configure({allocate_heap,free_heap}))return false;
 }
 const uint64_t sdk=game::module_containing(game::global<uint64_t>(t::data::system_library_anchor));
 available_cores=reinterpret_cast<CoreMask>(game::module_symbol(sdk,"_ZN2nn2os26GetThreadAvailableCoreMaskEv"));
 // Without another available core, independent TLS would stall gameplay.
 if(!available_cores)return false;
 while(resolver_workers<2){if(!start_worker(resolver,nullptr))return false;++resolver_workers;}
 ready=true;
 return true;
}
void* allocate(uint64_t n){return memory.allocate(n);}
void free(void* p){memory.free(p);}
void* response_allocate(uint64_t n){return memory.allocate(n,true);}
void response_free(void* p){memory.free(p);}
https_memory::Snapshot memory_snapshot(){return memory.snapshot();}
bool start_worker(void(*entry)(void*),void* context){
 if(!entry)return false;
 constexpr uint64_t stack_bytes=256u<<10;
 // The original SDK constructor stores user fields beginning at0x1c0;
 // its ThreadType prefix and page-aligned stack ABI are kept exactly.
 struct Thread {alignas(16) uint8_t object[0x1c0];void* raw_stack;void* raw_object;};
 void* raw=allocate(sizeof(Thread)+15);if(!raw)return false;
 auto* thread=reinterpret_cast<Thread*>((reinterpret_cast<uint64_t>(raw)+15)&~uint64_t(15));*thread={};thread->raw_object=raw;
 thread->raw_stack=allocate(stack_bytes+4095);if(!thread->raw_stack){free(thread->raw_object);return false;}
 auto* stack=reinterpret_cast<void*>((reinterpret_cast<uint64_t>(thread->raw_stack)+4095)&~uint64_t(4095));
 void* current=t::fn::os_current_thread();
 // Workers are created from the game thread. Inheriting its core and
 // priority meant the scheduler ran a worker until it blocked, so a TLS
 // handshake (CPU-bound for a long time) stalled the game for its whole
 // length. Lowering the priority on that core is no answer: measured live,
 // the workers then got so little time that requests timed out. So they run
 // on the process's other cores, below every game thread there, and are
 // preempted the moment one of those is ready. Without another core (or the
 // SDK query for them) starting workers is refused.
 int priority=t::fn::os_thread_priority(current);
 int ideal=-1;uint64_t mask=0;
 t::fn::os_thread_core_mask(&ideal,&mask,current);
 const uint64_t others=available_cores?available_cores()&~mask&0xf:0;
 if(!others){free(thread->raw_stack);free(thread->raw_object);return false;}
 if(others){
  priority+=kWorkerPriorityBelowGame;if(priority>31)priority=31; // nn::os::LowestThreadPriority
  mask=others;ideal=3;while(!((others>>ideal)&1))--ideal;
 }
 const auto result=t::fn::os_create_thread(thread,reinterpret_cast<void*>(entry),context,stack,stack_bytes,priority);
 if(result){free(thread->raw_stack);free(thread->raw_object);return false;}
 t::fn::os_set_thread_core_mask(thread,ideal,mask);
 t::fn::os_set_thread_name(thread,"Tsuchi HTTPS");
 t::fn::os_start_thread(thread);
 // Persistent workers own the object and stack until process teardown. They
 // never join the game's curl GC and cannot be destroyed by its cleanup.
 init_diagnostic[2].store(init_diagnostic[2].load()|4);return true;
}
uint64_t system_tick(){
 register uint64_t ticks asm("x0");
 asm volatile("svc #0x1e":"=r"(ticks)::"x1","x2","x3","x4","x5","x6","x7","memory");
 return ticks;
}
constexpr uint32_t kEntropyRounds=8,kEntropySamples=4096;
struct EntropyPool {uint64_t words[4],seconds,tick;uint16_t samples[kEntropyRounds*kEntropySamples];};
EntropyPool entropy_pool{};uint8_t entropy_work[8192]{};uint16_t entropy_counts[https::https_entropy::buckets]{};
void sample_timing(uint16_t* out,uint32_t count){
 uint64_t previous=system_tick();
 for(uint32_t i=0;i<count;++i){
  volatile uint32_t sum=0;
  for(uint32_t k=0;k<64;++k){entropy_work[(k*127u)&8191u]^=uint8_t(sum+k);sum=sum+entropy_work[(k*61u)&8191u];}
  const uint64_t now=system_tick(),delta=now-previous;previous=now;
  out[i]=delta>65535?65535:uint16_t(delta);
 }
}

bool entropy_seed(uint8_t out[32]){
 auto& pool=entropy_pool;pool.seconds=uint64_t(unix_seconds());pool.tick=system_tick();
 // Kernel values mix into the seed, but emulator output earns no entropy credit.
 for(uint64_t i=0;i<4;++i){register uint64_t result asm("x0")=0;register uint64_t word asm("x1")=11;register uint64_t handle asm("x2")=0;register uint64_t subid asm("x3")=i;asm volatile("svc #0x29":"+r"(result),"+r"(word):"r"(handle),"r"(subid):"x4","x5","x6","x7","memory");pool.words[i]=uint32_t(result)?0:word;}
 uint32_t credit=0,rounds=0;
 while(rounds<kEntropyRounds&&(rounds<2||credit<https::https_entropy::required_bits)){
  uint16_t* samples=pool.samples+rounds*kEntropySamples;sample_timing(samples,kEntropySamples);
  credit+=https::https_entropy::estimate(samples,kEntropySamples,entropy_counts).credit_bits;++rounds;
 }
 const bool ok=credit>=https::https_entropy::required_bits&&tsuchi_https_sha256(&pool,sizeof(pool),out);
 volatile uint8_t* wipe=reinterpret_cast<uint8_t*>(&pool);for(uint32_t i=0;i<sizeof(pool);++i)wipe[i]=0;
 entropy_report.store((credit<<4)|(ok?8u:0u));return ok;
}
bool random(void* out,uint64_t count){
 if((!out&&count)||count>65536)return false;
 if(rng_state.exchange(0,1)){uint8_t seed[32]{};bool ok=entropy_seed(seed);if(ok)ok=tsuchi_https_random_seed(seed,sizeof(seed));volatile uint8_t* wipe=seed;for(unsigned i=0;i<sizeof(seed);++i)wipe[i]=0;rng_state.store(ok?2:3);}
 while(rng_state.load()==1)sleep_ms(1);
 return rng_state.load()==2&&tsuchi_https_random_fill(out,count);
}
int64_t unix_seconds(){int64_t seconds=-1;uint32_t r=t::fn::network_utc(&seconds);if(r)r=t::fn::user_utc(&seconds);return r||seconds<0?-1:seconds;}
uint64_t milliseconds(){return system_tick()/19200;}
uint32_t entropy_diagnostic(){return entropy_report.load();}
uint32_t entropy_detail_diagnostic(uint32_t i){return i<8?entropy_detail[i].load():0;}
void sleep_ms(uint32_t n){t::fn::os_sleep_thread(static_cast<long long>(n)*1000000);}
// Horizon address arbitration (4.0.0+), as specified by libnx svc.h/svc.s:
// WaitIfEqual=2, Signal=0, count=-1 broadcasts, timeout=-1 waits indefinitely.
// https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h
void wait_for_work(const uint32_t* word,uint32_t expected){
 register uint64_t address asm("x0")=reinterpret_cast<uint64_t>(word);
 register uint64_t type asm("x1")=2;
 register uint64_t value asm("x2")=int64_t(int32_t(expected));
 register int64_t timeout asm("x3")=-1;
 asm volatile("svc #0x34":"+r"(address):"r"(type),"r"(value),"r"(timeout):"x4","x5","x6","x7","memory");
}
void wake_for_work(const uint32_t* word){
 register uint64_t address asm("x0")=reinterpret_cast<uint64_t>(word);
 register uint64_t type asm("x1")=0;
 register uint64_t value asm("x2")=0;
 register int64_t count asm("x3")=-1;
 asm volatile("svc #0x35":"+r"(address):"r"(type),"r"(value),"r"(count):"x4","x5","x6","x7","memory");
}
int tcp_open(const https::HttpsJob& job,const char* host,uint16_t port,uint64_t deadline,int* out){
 init_diagnostic[4].store(1);init_diagnostic[5].store(0);init_diagnostic[6].store(0);
 if(out)*out=-1;if(!out||!host||!port||!resolver_workers)return -1;int stopped=stop(job,deadline);if(stopped)return stopped;
 unsigned length=0;while(host[length]&&length<256)++length;if(!length||length>=256)return -1;
 Dns* slot=nullptr;for(auto& row:dns)if(row.state.exchange(0,1)){slot=&row;break;}if(!slot)return -2;
 for(unsigned i=0;i<=length;++i)slot->name[i]=host[i];slot->state.store(2);
 dns_work.notify(wake_for_work);
 init_diagnostic[4].store(2);
 while(slot->state.load()!=4){stopped=stop(job,deadline);if(stopped){abandon(*slot);return stopped;}sleep_ms(5);}
 uint8_t ips[4][4];const unsigned count=slot->count;const int resolved=slot->result;
 init_diagnostic[4].store(3);
 for(unsigned i=0;i<count;++i)for(unsigned k=0;k<4;++k)ips[i][k]=slot->ip[i][k];slot->state.store(0);if(resolved)return -1;
 for(unsigned i=0;i<count;++i){
  stopped=stop(job,deadline);if(stopped)return stopped;
  init_diagnostic[4].store(4);
  const int fd=t::fn::socket_socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(fd<0){error();continue;}
  init_diagnostic[4].store(5);const int flags=t::fn::socket_fcntl(fd,F_GETFL,0);
  if(flags<0){error();tcp_close(fd);continue;}
  init_diagnostic[4].store(6);
  if(t::fn::socket_fcntl(fd,F_SETFL,flags|O_NONBLOCK)){error();tcp_close(fd);continue;}
  Address address{};address.len=sizeof(address);address.family=AF_INET;address.port=uint16_t((port>>8)|(port<<8));for(unsigned k=0;k<4;++k)address.ip[k]=ips[i][k];
  init_diagnostic[4].store(7);int r=t::fn::socket_connect(fd,&address,sizeof(address));
  // A nonblocking Windows-hosted SDK connect can report EAGAIN rather than
  // EINPROGRESS. Poll plus checked SO_ERROR must still establish completion.
  if(!r){init_diagnostic[4].store(10);*out=fd;return 0;}const int e=error();if(!again(e)&&e!=36&&e!=115&&e!=37&&e!=114){tcp_close(fd);continue;}
  for(;;){
   stopped=stop(job,deadline);if(stopped){tcp_close(fd);return stopped;}
   init_diagnostic[4].store(8);PollFd poll{fd,4,0};r=t::fn::socket_poll(&poll,1,0);
   if(r<0){if(error()==4)continue;break;}
   if(r&&poll.revents){init_diagnostic[4].store(9);int pending=-1;uint32_t size=sizeof(pending);
    if(t::fn::socket_get_sock_opt(fd,SOL_SOCKET,SO_ERROR,&pending,&size)||size!=sizeof(pending)||pending)break;
    if(poll.revents&4){init_diagnostic[4].store(10);*out=fd;return 0;}break;
   }sleep_ms(5);
  }tcp_close(fd);
 }return -1;
}
int32_t tcp_read(int fd,void* out,uint32_t count){if(fd<0||(!out&&count))return -1;if(count>65536)count=65536;const auto n=t::fn::socket_recv(fd,out,count,MSG_DONTWAIT);return n<0?(again(error())?-2:-1):n>count?-1:int32_t(n);}
// tcp_open already enabled O_NONBLOCK. Eden forwards send flags directly to a
// backend requiring zero; no per-call nonblocking flag is needed here.
int32_t tcp_write(int fd,const void* in,uint32_t count){if(fd<0||(!in&&count))return -1;if(count>65536)count=65536;const auto n=t::fn::socket_send(fd,in,count,0);return n<0?(again(error())?-2:-1):n>count?-1:int32_t(n);}
void tcp_close(int fd){if(fd>=0)t::fn::socket_close(fd);}

}
