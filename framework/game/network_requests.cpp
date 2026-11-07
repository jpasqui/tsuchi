#include <string.h>
#include "frame.h"
#include "holds.h"
#include "host.h"
#include "https_jobs.h"
#include "network_requests.h"
namespace tsuchi {
namespace {
constexpr uint32_t kJobs=64;
struct JobSlot {https::HttpsJob job;uint64_t id;bool abandoned;};
JobSlot jobs[kJobs]{};
const https::WorkerBackend* backend=nullptr;
uint64_t next_request=1;
struct RequestKept {JobSlot* slot;bool notified;};
struct RequestWanted {const char* url;uint64_t max_bytes;uint32_t timeout_ms;};
TSUCHI_Result request_apply(const Hold& hold,RequestKept& kept,const RequestWanted& wanted,bool fresh){
    if(!fresh)return TSUCHI_BUSY;
    if(!backend)return TSUCHI_UNSUPPORTED;
    JobSlot* chosen=nullptr;for(auto& slot:jobs)if(slot.job.state.exchange(0,6)){chosen=&slot;break;}
    if(!chosen)return TSUCHI_CAPACITY;
    if(!backend->start()){chosen->job.state.store(0);return TSUCHI_UNSUPPORTED;}
    auto& job=chosen->job;
    chosen->id=hold.what;chosen->abandoned=false;kept.slot=chosen;
    memcpy(job.url,wanted.url,strlen(wanted.url)+1);job.kind=https::HTTPS_MEMORY;
    job.max_bytes=wanted.max_bytes;job.timeout_ms=wanted.timeout_ms;job.connect_timeout_ms=wanted.timeout_ms<60000?wanted.timeout_ms:60000;
    job.cancelled.store(0);job.error.store(0);job.http_status.store(0);job.transport_error.store(0);job.received.store(0);job.total.store(0);job.body={};
    job.state.store(https::HTTPS_QUEUED);
    if(backend->wake)backend->wake();
    return TSUCHI_OK;
}
TSUCHI_Result failure(const https::HttpsJob& job){if(!job.error.load())return TSUCHI_OK;return job.error.load()==https::HTTPS_ERROR_STORAGE?TSUCHI_NO_MEMORY:job.error.load()==https::HTTPS_ERROR_LIMIT?TSUCHI_CAPACITY:TSUCHI_IO;}
bool terminal(uint32_t state){return state>=https::HTTPS_SUCCEEDED&&state<=https::HTTPS_CANCELLED;}
bool request_keep(const Hold& hold,RequestKept& kept){
    const uint32_t state=kept.slot->job.state.load();
    if(terminal(state)&&!kept.notified){kept.notified=true;const uint32_t http=kept.slot->job.http_status.load();net::emit_completed_to(hold.mod,{{hold.what},state-1,http});}
    return true;
}
void dispose(JobSlot& slot){if(backend&&backend->dispose)backend->dispose(&slot.job);slot.job.body={};slot.abandoned=false;slot.id=0;slot.job.state.store(0);}
void request_restore(const Hold&,RequestKept& kept,HoldEnd){
    auto& slot=*kept.slot;slot.abandoned=true;slot.job.cancelled.store(1);
    // A queued worker and the game race only through state. A running worker
    // retains its stable job until its terminal release publication.
    if(slot.job.state.exchange(https::HTTPS_QUEUED,https::HTTPS_CANCELLED)||terminal(slot.job.state.load()))dispose(slot);
}
Holds<RequestKept,RequestWanted,request_apply,request_keep,request_restore> requests{"net.request",HoldOn::nothing,false};
TSUCHI_Result request_of(TSUCHI_Mod* mod,TSUCHI_Request request,RequestKept*& out){out=requests.find(mod,0,request.id);return out?TSUCHI_OK:requests.find(0,request.id)?TSUCHI_DENIED:TSUCHI_NOT_FOUND;}
void clean(){for(auto& slot:jobs)if(slot.abandoned&&terminal(slot.job.state.load()))dispose(slot);}
bool valid_url(const char* url){
    const char* prefix="https://";const char* p=url;
    while(*prefix)if(*p++!=*prefix++)return false;
    if(!*p)return false;uint32_t size=8;
    for(;*p;++p){const uint8_t c=uint8_t(*p);if(++size>1023||c<=32||c>=127||c=='@'||c=='#'||c=='\\')return false;}
    return true;
}
}
namespace https {
void set_worker_backend(const WorkerBackend* next){backend=next;}
HttpsJob* https_take_job(){for(auto& slot:jobs)if(slot.job.state.exchange(HTTPS_QUEUED,HTTPS_RUNNING))return &slot.job;return nullptr;}
}
TSUCHI_Result network_request_get(TSUCHI_Mod* mod,const char* url,uint64_t max_bytes,uint32_t timeout_ms,TSUCHI_Request* out){
    if(!valid_url(url)||!max_bytes||max_bytes>4194304||!timeout_ms||timeout_ms>300000)return TSUCHI_INVALID;
    uint32_t count=0;for(uint32_t i=0;i<requests.count();++i)count+=requests.hold_at(i).mod==mod;
    if(count>=8||!next_request)return TSUCHI_CAPACITY;
    const uint64_t id=next_request++;
    const TSUCHI_Result result=requests.take(mod,0,id,{url,max_bytes,timeout_ms});if(result==TSUCHI_OK)out->id=id;return result;
}
TSUCHI_Result network_request_info(TSUCHI_Mod* mod,TSUCHI_Request request,TSUCHI_RequestInfo* out){RequestKept* held=nullptr;const TSUCHI_Result result=request_of(mod,request,held);if(result!=TSUCHI_OK)return result;auto& job=held->slot->job;const uint32_t state=job.state.load();*out={state-1,job.http_status.load(),job.received.load(),job.total.load(),terminal(state)&&failure(job)!=TSUCHI_OK?result_name(failure(job)):""};return TSUCHI_OK;}
TSUCHI_Result network_request_read(TSUCHI_Mod* mod,TSUCHI_Request request,uint64_t offset,void* buffer,uint64_t capacity,uint64_t* out){RequestKept* held=nullptr;const TSUCHI_Result result=request_of(mod,request,held);if(result!=TSUCHI_OK)return result;auto& job=held->slot->job;if(job.state.load()!=https::HTTPS_SUCCEEDED)return TSUCHI_NOT_READY;const uint64_t size=job.received.load();*out=offset>=size?0:capacity<size-offset?capacity:size-offset;if(*out&&!https::https_body_read(job.body,size,offset,buffer,*out))return TSUCHI_IO;return TSUCHI_OK;}
TSUCHI_Result network_request_cancel(TSUCHI_Mod* mod,TSUCHI_Request request){RequestKept* held=nullptr;const TSUCHI_Result result=request_of(mod,request,held);if(result!=TSUCHI_OK)return result;auto& job=held->slot->job;if(!terminal(job.state.load())){job.cancelled.store(1);job.state.exchange(https::HTTPS_QUEUED,https::HTTPS_CANCELLED);}return TSUCHI_OK;}
TSUCHI_Result network_request_release(TSUCHI_Mod* mod,TSUCHI_Request request){RequestKept* held=nullptr;const TSUCHI_Result result=request_of(mod,request,held);return result==TSUCHI_OK?requests.release(mod,0,request.id):result;}
void network_requests_start(){requests.start();each_frame(clean);}
}
