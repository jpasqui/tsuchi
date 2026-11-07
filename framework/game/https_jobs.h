#pragma once
#include <stdint.h>
#include "https_body.h"
namespace tsuchi::https {
constexpr uint32_t HTTPS_ID_BYTES=48, HTTPS_URL_BYTES=2048;
constexpr uint32_t HTTPS_MEMORY=1,HTTPS_FILE=2;
constexpr uint32_t HTTPS_QUEUED=1,HTTPS_RUNNING=2,HTTPS_SUCCEEDED=3,HTTPS_FAILED=4,HTTPS_CANCELLED=5;
constexpr uint32_t HTTPS_ERROR_NONE=0,HTTPS_ERROR_NETWORK=1,HTTPS_ERROR_TLS=2,HTTPS_ERROR_HTTP=3,HTTPS_ERROR_TIMEOUT=4,HTTPS_ERROR_LIMIT=5,HTTPS_ERROR_STORAGE=6,HTTPS_ERROR_PROTOCOL=7;
template<class T> struct HttpsAtomic {
 T value{};T load() const{return __atomic_load_n(&value,__ATOMIC_ACQUIRE);}
 void store(T v){__atomic_store_n(&value,v,__ATOMIC_RELEASE);}
 bool exchange(T expected,T desired){return __atomic_compare_exchange_n(&value,&expected,desired,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
};
struct HttpsJob {
 char url[HTTPS_URL_BYTES]{};
 uint32_t kind=HTTPS_MEMORY,connect_timeout_ms=0,timeout_ms=0;
 uint64_t max_bytes=0;
 HttpsAtomic<uint32_t> state{},cancelled{},error{},http_status{},transport_error{};
 HttpsAtomic<uint64_t> received{},total{};
 HttpsBody body{};
};
inline bool https_stopped(const HttpsJob& j){return j.cancelled.load()!=0;}
inline void https_finish(HttpsJob& j,uint32_t error,int32_t transport=0,void(*release_response)(void*)=nullptr){const bool cancelled=https_stopped(j);if(release_response&&(cancelled||error))https_body_free(j.body,release_response);j.error.store(cancelled?0:error);j.transport_error.store(uint32_t(transport));j.state.store(cancelled?HTTPS_CANCELLED:error?HTTPS_FAILED:HTTPS_SUCCEEDED);}
HttpsJob* https_take_job();
struct WorkerBackend {bool(*start)();void(*dispose)(HttpsJob*);void(*wake)();};
void set_worker_backend(const WorkerBackend*);
void https_native_start();
}
