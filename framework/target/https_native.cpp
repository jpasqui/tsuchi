#include "https_platform.h"
#include "https_tls.h"
#include "https_roots.h"
#include "https_work_signal.h"
namespace tsuchi::https {
namespace {
namespace platform=https_platform;
struct File {const FileSink* sink;void* value;};
bool file_open(const HttpsJob& job,void** out){
    if(!job.sink)return false;
    auto* file=static_cast<File*>(platform::allocate(sizeof(File)));if(!file)return false;
    file->sink=job.sink;file->value=nullptr;
    if(!file->sink->open(job,&file->value)){platform::free(file);return false;}*out=file;return true;
}
bool file_write(void* p,uint64_t offset,const void* data,uint64_t size){auto* f=static_cast<File*>(p);return f->sink->write(f->value,offset,data,size);}
bool file_commit(void* p){auto* f=static_cast<File*>(p);if(!f->sink->commit(f->value))return false;platform::free(f);return true;}
void file_abort(void* p){auto* f=static_cast<File*>(p);f->sink->abort(f->value);platform::free(f);}
const HttpsPlatform transport{platform::allocate,platform::free,platform::response_allocate,platform::response_free,platform::random,platform::unix_seconds,platform::milliseconds,platform::sleep_ms,platform::tcp_open,platform::tcp_read,platform::tcp_write,platform::tcp_close,file_open,file_write,file_commit,file_abort,tsuchi_https_roots,sizeof(tsuchi_https_roots)};
WorkSignal work;
void worker(void*){for(;;){const auto observed=work.sample();if(auto* job=https_take_job())https_tls_run(*job);else work.wait(observed,platform::wait_for_work);}}
bool start(){
    static uint32_t workers=0;
    if(!platform::initialize())return false;
    while(workers<2){if(!platform::start_worker(worker,nullptr))return workers!=0;++workers;}
    return true;
}
void dispose(HttpsJob* job){https_body_free(job->body,platform::response_free);}
void wake(){work.notify(platform::wake_for_work);}
const WorkerBackend service{start,dispose,wake};
}
void https_native_start(){if(set_https_tls_platform(&transport))set_worker_backend(&service);}
}
