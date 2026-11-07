#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <thread>
#include <ctime>
#include "https_tls.h"
#include "https_memory.h"
using namespace tsuchi::https;
namespace {
std::atomic<int> active_sockets{0};
tsuchi::https_memory::Memory memory;
void* heap_allocate(uint64_t bytes){return std::malloc(size_t(bytes));}
void heap_free(void* bytes){std::free(bytes);}
uint64_t milliseconds(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
void pause(uint32_t ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
void* allocate(uint64_t bytes){return memory.allocate(bytes);}
void release(void* bytes){memory.free(bytes);}
void* response_allocate(uint64_t bytes){return memory.allocate(bytes,true);}
bool random(void* out,uint64_t bytes){return bytes<=65536&&BCryptGenRandom(nullptr,static_cast<PUCHAR>(out),ULONG(bytes),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0;}
int64_t utc(){return int64_t(std::time(nullptr));}
int open(const HttpsJob& job,const char* host,uint16_t port,uint64_t deadline,int* out){
 addrinfo hints{},*results=nullptr;hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;char service[8]{};std::snprintf(service,sizeof(service),"%u",unsigned(port));
 if(getaddrinfo(host,service,&hints,&results))return -1;
 SOCKET socket_fd=INVALID_SOCKET;
 for(auto* row=results;row;row=row->ai_next){socket_fd=socket(row->ai_family,row->ai_socktype,row->ai_protocol);if(socket_fd==INVALID_SOCKET)continue;u_long nonblock=1;ioctlsocket(socket_fd,FIONBIO,&nonblock);const int r=connect(socket_fd,row->ai_addr,int(row->ai_addrlen));if(!r)break;if(WSAGetLastError()!=WSAEWOULDBLOCK){closesocket(socket_fd);socket_fd=INVALID_SOCKET;continue;}
  while(milliseconds()<deadline&&!https_stopped(job)){WSAPOLLFD poll{socket_fd,POLLWRNORM,0};if(WSAPoll(&poll,1,5)>0){int error=0,length=sizeof(error);if(getsockopt(socket_fd,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length)==0&&!error&&(poll.revents&POLLWRNORM))break;closesocket(socket_fd);socket_fd=INVALID_SOCKET;break;}}
  if(milliseconds()>=deadline||https_stopped(job)){if(socket_fd!=INVALID_SOCKET)closesocket(socket_fd);socket_fd=INVALID_SOCKET;}if(socket_fd!=INVALID_SOCKET)break;
 }
 freeaddrinfo(results);if(socket_fd==INVALID_SOCKET)return -1;*out=int(socket_fd);++active_sockets;return 0;
}
int32_t read(int fd,void* data,uint32_t bytes){const int r=recv(SOCKET(fd),static_cast<char*>(data),int(bytes),0);return r==SOCKET_ERROR?(WSAGetLastError()==WSAEWOULDBLOCK?-2:-1):r;}
int32_t write(int fd,const void* data,uint32_t bytes){const int r=send(SOCKET(fd),static_cast<const char*>(data),int(bytes),0);return r==SOCKET_ERROR?(WSAGetLastError()==WSAEWOULDBLOCK?-2:-1):r;}
void close(int fd){closesocket(SOCKET(fd));--active_sockets;}
}
void prepare(HttpsJob& job,const char* base,const char* path,uint64_t max=HTTPS_BODY_LIMIT){
 job.kind=HTTPS_MEMORY;job.max_bytes=max;job.connect_timeout_ms=10000;job.timeout_ms=20000;
 std::snprintf(job.url,sizeof(job.url),"%s%s",base,path);job.state.store(HTTPS_RUNNING);
}
void dispose(HttpsJob& job){https_body_free(job.body,release);}
void empty(){if(memory.snapshot().total){std::printf("FAIL retained allocation: %llu\n",(unsigned long long)memory.snapshot().total);std::exit(1);}}
bool successful(const HttpsJob& job){return job.state.load()==HTTPS_SUCCEEDED;}
int main(int count,char** args){
 if(count!=6)return 2;WSADATA startup{};if(WSAStartup(MAKEWORD(2,2),&startup))return 2;
 FILE* file=std::fopen(args[1],"rb");if(!file)return 2;std::fseek(file,0,SEEK_END);const long size=std::ftell(file);std::rewind(file);auto* roots=static_cast<unsigned char*>(std::malloc(size_t(size)+1));if(std::fread(roots,1,size_t(size),file)!=size_t(size))return 2;std::fclose(file);roots[size]=0;
 if(!memory.configure({heap_allocate,heap_free})||memory.snapshot().total)return 2;
 const HttpsPlatform platform{allocate,release,response_allocate,release,random,utc,milliseconds,pause,open,read,write,close,nullptr,nullptr,nullptr,nullptr,roots,uint32_t(size+1)};
 if(!set_https_tls_platform(&platform))return 2;
 for(int i=0;i<4;++i){
  HttpsJob job{};prepare(job,args[i+2],"/small");std::thread worker([&]{https_tls_run(job);});worker.join();
  if(active_sockets.load()!=0||successful(job)!=(i==0)){std::printf("FAIL certificate %d state %u error %u transport %u\n",i,job.state.load(),job.error.load(),job.transport_error.load());return 1;}
  char body[8]{};if(i==0&&(job.received.load()!=7||job.body.allocated!=7||!https_body_read(job.body,7,0,body,7)||std::memcmp(body,"captain",7)))return 1;
  if(i!=0&&job.body.allocated)return 1;dispose(job);empty();
 }
 HttpsJob limited{};prepare(limited,args[2],"/small",2);https_tls_run(limited);
 if(limited.state.load()!=HTTPS_FAILED||limited.error.load()!=HTTPS_ERROR_LIMIT||limited.body.allocated)return 1;empty();
 HttpsJob cancelled{};prepare(cancelled,args[2],"/slow-body");
 std::thread cancel_worker([&]{https_tls_run(cancelled);});const auto cancel_deadline=milliseconds()+15000;
 while(!cancelled.received.load()&&cancelled.state.load()==HTTPS_RUNNING&&milliseconds()<cancel_deadline)pause(2);
 if(!cancelled.received.load()){cancelled.cancelled.store(1);cancel_worker.join();puts("FAIL no partial body to cancel");return 1;}
 cancelled.cancelled.store(1);cancel_worker.join();
 if(cancelled.state.load()!=HTTPS_CANCELLED||cancelled.body.allocated||active_sockets.load()!=0)return 1;empty();
 HttpsJob truncated{};prepare(truncated,args[2],"/truncated");https_tls_run(truncated);
 if(truncated.state.load()!=HTTPS_FAILED||!truncated.received.load()||truncated.body.allocated)return 1;empty();
 HttpsJob chunked{};prepare(chunked,args[2],"/chunked");https_tls_run(chunked);
 unsigned char boundary[40]{};if(!successful(chunked)||chunked.received.load()!=70000||chunked.body.allocated!=2*HTTPS_BODY_CHUNK||!https_body_read(chunked.body,70000,65520,boundary,sizeof boundary))return 1;
 for(unsigned i=0;i<sizeof boundary;++i)if(boundary[i]!=uint8_t(65520+i))return 1;dispose(chunked);empty();
 HttpsJob chunk_limit{};prepare(chunk_limit,args[2],"/chunked",66000);https_tls_run(chunk_limit);
 if(chunk_limit.state.load()!=HTTPS_FAILED||chunk_limit.error.load()!=HTTPS_ERROR_LIMIT||!chunk_limit.received.load()||chunk_limit.body.allocated)return 1;empty();

 // Maximum request limits do not reserve maximum-sized response buffers.
 HttpsJob tiny_jobs[8]{};
 for(auto& job:tiny_jobs){prepare(job,args[2],"/small");https_tls_run(job);if(!successful(job)||job.body.allocated!=7)return 1;}
 if(memory.snapshot().responses!=56)return 1;
 const auto small_memory=memory.snapshot().total;
 for(auto& job:tiny_jobs)dispose(job);empty();memory.reset_peaks();
 // Two TLS workers are active, with eight completed large responses retained.
 HttpsJob large[8]{};for(auto& job:large)prepare(job,args[2],"/large");
 for(unsigned i=0;i<8;i+=2){std::thread a([&]{https_tls_run(large[i]);});std::thread b([&]{https_tls_run(large[i+1]);});a.join();b.join();}
 for(auto& job:large)if(!successful(job)||job.received.load()!=HTTPS_BODY_LIMIT||job.body.allocated!=HTTPS_BODY_LIMIT)return 1;
 if(memory.snapshot().responses!=tsuchi::https_memory::kResponseLimit)return 1;
 const auto large_peak=memory.snapshot().peak_total;
 HttpsJob exhausted{};prepare(exhausted,args[2],"/small");https_tls_run(exhausted);
 if(exhausted.state.load()!=HTTPS_FAILED||exhausted.error.load()!=HTTPS_ERROR_STORAGE||exhausted.body.allocated)return 1;
 dispose(large[0]);HttpsJob retry{};prepare(retry,args[2],"/large");https_tls_run(retry);if(!successful(retry))return 1;
 for(auto& job:large)dispose(job);dispose(retry);empty();
 for(unsigned i=0;i<12;++i){HttpsJob job{};prepare(job,args[2],i&1?"/small":"/truncated");https_tls_run(job);if(successful(job)!=(bool(i&1)))return 1;dispose(job);empty();}
 if(active_sockets.load())return 1;
 std::printf("MEMORY eight_small_retained=%llu concurrent_large_peak=%llu final_total=%llu\n",(unsigned long long)small_memory,(unsigned long long)large_peak,(unsigned long long)memory.snapshot().total);
 std::free(roots);
 puts("PASS independent TLS trust, hostname, expiry, untrusted rejection, limit, and cancellation");return 0;
}
