#include "https_platform.h"
#include "https_tls.h"
#include "https_roots.h"
#include "https_work_signal.h"
namespace tsuchi::https {
namespace {
namespace platform=https_platform;
const HttpsPlatform transport{platform::allocate,platform::free,platform::response_allocate,platform::response_free,platform::random,platform::unix_seconds,platform::milliseconds,platform::sleep_ms,platform::tcp_open,platform::tcp_read,platform::tcp_write,platform::tcp_close,nullptr,nullptr,nullptr,nullptr,tsuchi_https_roots,sizeof(tsuchi_https_roots)};
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
