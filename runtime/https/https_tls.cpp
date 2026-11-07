#include "https_tls.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/hmac_drbg.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/platform_util.h"
#include <string.h>
#include <time.h>
inline void* operator new(size_t,void* p) noexcept {return p;}
namespace {
const char* tls_find(const char* text,char c){do{if(*text==c)return text;}while(*text++);return nullptr;}
char* tls_find(char* text,char c){return const_cast<char*>(tls_find(static_cast<const char*>(text),c));}
bool prefix(const char* text,const char* value){while(*value){if(*text++!=*value++)return false;}return true;}
using namespace tsuchi::https;
const HttpsPlatform* platform=nullptr;
HttpsAtomic<uint64_t> diagnostics[4]{};
mbedtls_hmac_drbg_context process_rng{};
HttpsAtomic<uint32_t> rng_ready{},rng_lock{};
uint64_t rng_bytes=0;
void count(unsigned i){uint64_t n=diagnostics[i].load();while(!diagnostics[i].exchange(n,n+1))n=diagnostics[i].load();}
uint64_t length(const char* s){uint64_t n=0;while(s[n])++n;return n;}
bool copy(char* out,uint32_t cap,const char* in){uint32_t n=0;while(in[n]){if(n+1>=cap)return false;out[n]=in[n];++n;}out[n]=0;return true;}
bool same(const char* a,const char* b){return strcmp(a,b)==0;}
char lower(char c){return c>='A'&&c<='Z'?char(c+32):c;}
bool equals(const char* a,const char* b){while(*a&&lower(*a)==lower(*b)){++a;++b;}return !*a&&!*b;}
bool number(const char* s,uint64_t* out){if(!*s)return false;uint64_t n=0;for(;*s;++s){if(*s<'0'||*s>'9'||n>(UINT64_MAX-uint32_t(*s-'0'))/10)return false;n=n*10+uint32_t(*s-'0');}*out=n;return true;}
struct Url {char host[256]{},authority[272]{},path[HTTPS_URL_BYTES]{};uint16_t port=443;};
bool parse_url(const char* s,Url& u){
    if(!prefix(s,"https://"))return false;const char* a=s+8;uint32_t n=0;
    while(a[n]&&a[n]!='/'&&a[n]!='?'){const unsigned char c=a[n];if(n+1>=sizeof u.authority||c<=32||c>=127||c=='@'||c=='#'||c=='\\')return false;u.authority[n]=a[n];++n;}
    u.authority[n]=0;if(!n)return false;const char* colon=nullptr;
    // Bracketed IPv6 authorities are preserved for Host/SNI; the platform may
    // reject unsupported address families, but never change the identity.
    uint32_t h=0;
    if(a[0]=='['){uint32_t i=1;for(;i<n&&a[i]!=']';++i){if(h+1>=sizeof u.host)return false;u.host[h++]=a[i];}if(i==n||!h)return false;if(i+1<n){if(a[i+1]!=':')return false;colon=a+i+2;}}
    else {for(uint32_t i=0;i<n;++i){if(a[i]==':'){colon=a+i+1;break;}const char c=a[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='.')||h+1>=sizeof u.host)return false;u.host[h++]=c;}}
    if(!h)return false;u.host[h]=0;u.port=443;
    if(colon){uint64_t p=0;char port[6]{};const uint32_t bytes=uint32_t(a+n-colon);if(!bytes||bytes>=sizeof port)return false;memcpy(port,colon,bytes);if(!number(port,&p)||!p||p>65535)return false;u.port=uint16_t(p);}
    const char* rest=a+n;uint32_t p=0;if(*rest!='/')u.path[p++]='/';
    while(*rest){const unsigned char c=*rest;if(c<=32||c>=127||c=='#'||c=='\\'||p+1>=sizeof u.path)return false;u.path[p++]=*rest++;}u.path[p]=0;return true;
}
bool redirect_url(const Url& base,const char* location,char* out){
    char clean[HTTPS_URL_BYTES]{};uint32_t n=0;
    for(;location[n]&&location[n]!='#';++n){const unsigned char c=location[n];if(c<=32||c>=127||c=='\\'||n+1>=sizeof clean)return false;clean[n]=location[n];}clean[n]=0;if(!n)return false;
    uint32_t p=0;auto append=[&](const char* value){while(*value){if(p+1>=HTTPS_URL_BYTES)return false;out[p++]=*value++;}out[p]=0;return true;};
    if(prefix(clean,"https://")){if(!append(clean))return false;}
    else {
        // Only a colon before a path/query separator denotes a URI scheme.
        for(uint32_t i=0;clean[i]&&clean[i]!='/'&&clean[i]!='?';++i)if(clean[i]==':')return false;
        if(clean[0]=='/'&&clean[1]=='/'){if(!append("https:")||!append(clean))return false;}
        else {
            if(!append("https://")||!append(base.authority))return false;
            if(clean[0]=='/'){if(!append(clean))return false;}
            else {
                char prefix[HTTPS_URL_BYTES]{};uint32_t end=0;while(base.path[end]&&base.path[end]!='?')++end;
                if(clean[0]!='?')while(end&&base.path[end-1]!='/')--end;
                if(end>=sizeof prefix)return false;memcpy(prefix,base.path,end);prefix[end]=0;
                if(!append(prefix)||!append(clean))return false;
            }
        }
    }
    Url target;if(!parse_url(out,target))return false;
    const uint32_t offset=uint32_t(8+length(target.authority));char normalized[HTTPS_URL_BYTES]{};uint32_t used=0,pos=offset;
    // Remove literal dot segments for every redirect reference. Escaped bytes
    // stay untouched, and terminal dots retain the resulting directory slash.
    while(out[pos]&&out[pos]!='?'){
        if(out[pos]!='/')return false;++pos;const uint32_t begin=pos;while(out[pos]&&out[pos]!='/'&&out[pos]!='?')++pos;const uint32_t size=pos-begin;
        const bool dot=size==1&&out[begin]=='.',parent=size==2&&out[begin]=='.'&&out[begin+1]=='.';
        if(dot||parent){
            if(parent){while(used&&normalized[used-1]!='/')--used;if(used)--used;}
            if(!out[pos]||out[pos]=='?'){if(used+1>=sizeof normalized)return false;normalized[used++]='/';}
            continue;
        }
        if(used+size+1>=sizeof normalized)return false;normalized[used++]='/';memcpy(normalized+used,out+begin,size);used+=size;
    }
    if(!used)normalized[used++]='/';while(out[pos]){if(used+1>=sizeof normalized)return false;normalized[used++]=out[pos++];}normalized[used]=0;
    if(offset+used>=HTTPS_URL_BYTES)return false;memcpy(out+offset,normalized,used+1);return true;
}
// One request's TLS connection. No session or connection is resumed.
struct Link {
    mbedtls_ssl_context ssl{};mbedtls_ssl_config config{};mbedtls_x509_crt roots{};mbedtls_ctr_drbg_context rng{};
    int fd=-1;char host[256]{};uint16_t port=0;
    Link(){mbedtls_ssl_init(&ssl);mbedtls_ssl_config_init(&config);mbedtls_x509_crt_init(&roots);mbedtls_ctr_drbg_init(&rng);}
    ~Link(){if(fd>=0)platform->tcp_close(fd);mbedtls_ssl_free(&ssl);mbedtls_ssl_config_free(&config);mbedtls_x509_crt_free(&roots);mbedtls_ctr_drbg_free(&rng);}
};
// Opening a connection is the processor-heavy part of a request: parsing the
// root certificates and the handshake. Two workers doing it at the same moment
// take both of the cores they share with the game's other threads, and frames
// stall; one at a time leaves a core free. A worker that has waited two
// seconds for its turn goes ahead anyway, so a peer that stalls mid-handshake
// cannot hold up every other connection.
HttpsAtomic<uint32_t> opening_lock{};
struct Opening {
    bool held=false;
    template<class Active> explicit Opening(Active active){
        const uint64_t patience=platform->milliseconds()+2000;
        while(active()&&platform->milliseconds()<patience){if(opening_lock.exchange(0,1)){held=true;return;}platform->sleep_ms(2);}
    }
    ~Opening(){if(held)opening_lock.store(0);}
};
void destroy(Link* link){if(!link)return;link->~Link();platform->free(link);}
// One request's use of a connection: its deadline, cancellation and read buffer.
struct Session {
    HttpsJob& job;Link* link=nullptr;bool responded=false;
    int transport=0;uint32_t error=0;uint64_t deadline=0;unsigned char buffer[4096]{};uint32_t used=0,pos=0;
    Session(HttpsJob& j,uint64_t end):job(j),deadline(end){}
    bool active(){if(https_stopped(job))return false;if(platform->milliseconds()>=deadline){error=HTTPS_ERROR_TIMEOUT;return false;}return true;}
    bool fail(int r,uint32_t type=HTTPS_ERROR_TLS){transport=r;error=type;diagnostics[2].store(uint64_t(uint32_t(r)));return false;}
    static int entropy(void*,unsigned char* bytes,size_t n){return platform->random(bytes,n)?0:MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;}
    static int send(void* context,const unsigned char* bytes,size_t n){auto& s=*static_cast<Session*>(context);if(!s.active())return MBEDTLS_ERR_SSL_INTERNAL_ERROR;const auto r=platform->tcp_write(s.link->fd,bytes,uint32_t(n>65536?65536:n));return r==-2?MBEDTLS_ERR_SSL_WANT_WRITE:r<0?MBEDTLS_ERR_SSL_INTERNAL_ERROR:r;}
    static int receive(void* context,unsigned char* bytes,size_t n){auto& s=*static_cast<Session*>(context);if(!s.active())return MBEDTLS_ERR_SSL_INTERNAL_ERROR;const auto r=platform->tcp_read(s.link->fd,bytes,uint32_t(n>65536?65536:n));return r==-2?MBEDTLS_ERR_SSL_WANT_READ:r<0?MBEDTLS_ERR_SSL_INTERNAL_ERROR:r;}
    bool connect(const Url& u){
        count(0);if(platform->unix_seconds()<1577836800)return fail(-1);
        void* storage=platform->allocate(sizeof(Link));if(!storage){error=HTTPS_ERROR_STORAGE;return false;}
        link=::new(storage) Link();if(!copy(link->host,sizeof link->host,u.host))return fail(-1,HTTPS_ERROR_PROTOCOL);link->port=u.port;
        auto& ssl=link->ssl;auto& config=link->config;auto& roots=link->roots;auto& rng=link->rng;
        // The socket first: waiting for the network needs no turn.
        uint64_t end=platform->milliseconds()+job.connect_timeout_ms;if(end>deadline)end=deadline;
        int r=platform->tcp_open(job,u.host,u.port,end,&link->fd);if(r){if(r==-3)error=HTTPS_ERROR_TIMEOUT;else if(r!=-4)error=HTTPS_ERROR_NETWORK;transport=r;return false;}
        const Opening turn([this]{return active();});if(error||https_stopped(job))return false;
        r=mbedtls_x509_crt_parse(&roots,platform->roots,platform->roots_bytes);if(r)return fail(r);
        r=mbedtls_ctr_drbg_seed(&rng,entropy,nullptr,reinterpret_cast<const unsigned char*>("P4Modding HTTPS"),14);if(r)return fail(r);
        r=mbedtls_ssl_config_defaults(&config,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);if(r)return fail(r);
        mbedtls_ssl_conf_authmode(&config,MBEDTLS_SSL_VERIFY_REQUIRED);mbedtls_ssl_conf_ca_chain(&config,&roots,nullptr);mbedtls_ssl_conf_rng(&config,mbedtls_ctr_drbg_random,&rng);
        mbedtls_ssl_conf_min_tls_version(&config,MBEDTLS_SSL_VERSION_TLS1_2);mbedtls_ssl_conf_max_tls_version(&config,MBEDTLS_SSL_VERSION_TLS1_2);
        static const char* alpn[]={"http/1.1",nullptr};r=mbedtls_ssl_conf_alpn_protocols(&config,alpn);if(r)return fail(r);
        r=mbedtls_ssl_setup(&ssl,&config);if(r)return fail(r);r=mbedtls_ssl_set_hostname(&ssl,u.host);if(r)return fail(r);
        mbedtls_ssl_set_bio(&ssl,this,send,receive,nullptr);const uint64_t overall=deadline;deadline=end;
        while(active()){
            r=mbedtls_ssl_handshake(&ssl);if(!r)break;
            if(r!=MBEDTLS_ERR_SSL_WANT_READ&&r!=MBEDTLS_ERR_SSL_WANT_WRITE){const uint32_t flags=mbedtls_ssl_get_verify_result(&ssl);diagnostics[3].store(flags);if(!error)fail(r);deadline=overall;return false;}platform->sleep_ms(2);
        }
        deadline=overall;if(error||https_stopped(job))return false;
        const uint32_t flags=mbedtls_ssl_get_verify_result(&ssl);diagnostics[3].store(flags);if(flags)return fail(MBEDTLS_ERR_X509_CERT_VERIFY_FAILED);
        const char* protocol=mbedtls_ssl_get_alpn_protocol(&ssl);if(protocol&&!same(protocol,"http/1.1"))return fail(-1,HTTPS_ERROR_PROTOCOL);
        count(1);return true;
    }
    bool write_all(const char* bytes,uint32_t n){uint32_t p=0;while(p<n&&active()){const int r=mbedtls_ssl_write(&link->ssl,reinterpret_cast<const unsigned char*>(bytes+p),n-p);if(r>0)p+=uint32_t(r);else if(r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE)platform->sleep_ms(2);else return fail(r,HTTPS_ERROR_NETWORK);}return p==n;}
    int read_some(){while(active()){const int r=mbedtls_ssl_read(&link->ssl,buffer,sizeof buffer);if(r>0){pos=0;used=uint32_t(r);responded=true;return r;}if(r==0||r==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)return 0;if(r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE)platform->sleep_ms(2);else {fail(r,HTTPS_ERROR_NETWORK);return -1;}}return -1;}
    int byte(){if(pos==used&&read_some()<=0)return -1;return buffer[pos++];}
    bool line(char* out,uint32_t cap,uint32_t& budget){uint32_t n=0;for(;;){const int c=byte();if(c<0){if(!error)error=HTTPS_ERROR_PROTOCOL;return false;}if(!budget--||n+1>=cap){error=HTTPS_ERROR_PROTOCOL;return false;}if(c=='\r'){if(!budget--||byte()!='\n'){error=HTTPS_ERROR_PROTOCOL;return false;}out[n]=0;return true;}if(c=='\n'||!c||(c<32&&c!='\t')||c==127){error=HTTPS_ERROR_PROTOCOL;return false;}out[n++]=char(c);}}
    // Nothing of this response is left unread, in our buffer or in the TLS layer.
    bool drained(){return pos==used&&!mbedtls_ssl_get_bytes_avail(&link->ssl);}
};
// close: the server will not serve another request on this connection.
struct Headers {uint32_t status=0;bool has_size=false,chunked=false,close=false;uint64_t size=0;char location[HTTPS_URL_BYTES]{};};
bool headers(Session& s,Headers& h){
    char line[8192];uint32_t budget=32768;
    for(unsigned responses=0;responses<5;++responses){
        h={};if(!s.line(line,sizeof line,budget))return false;
        if(!prefix(line,"HTTP/1.1 ")&&!prefix(line,"HTTP/1.0 ")){s.error=HTTPS_ERROR_PROTOCOL;return false;}
        if(length(line)<12||line[9]<'1'||line[9]>'5'||line[10]<'0'||line[10]>'9'||line[11]<'0'||line[11]>'9'||(line[12]&&line[12]!=' ')){s.error=HTTPS_ERROR_PROTOCOL;return false;}
        h.status=uint32_t(line[9]-'0')*100+uint32_t(line[10]-'0')*10+uint32_t(line[11]-'0');s.job.http_status.store(h.status);
        h.close=line[7]=='0'; // HTTP/1.0 closes unless told otherwise; it is simply not reused
        while(s.line(line,sizeof line,budget)){
            if(!line[0])break;char* colon=tls_find(line,':');if(!colon||colon==line||line[0]==' '||line[0]=='\t'){s.error=HTTPS_ERROR_PROTOCOL;return false;}*colon=0;
            for(const char* p=line;*p;++p)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||tls_find("!#$%&'*+-.^_`|~",*p))){s.error=HTTPS_ERROR_PROTOCOL;return false;}
            char* value=colon+1;while(*value==' '||*value=='\t')++value;uint32_t n=uint32_t(length(value));while(n&&(value[n-1]==' '||value[n-1]=='\t'))value[--n]=0;
            if(equals(line,"Content-Length")){uint64_t size=0;if(!number(value,&size)||(h.has_size&&h.size!=size)){s.error=HTTPS_ERROR_PROTOCOL;return false;}h.has_size=true;h.size=size;}
            else if(equals(line,"Transfer-Encoding")){if(h.chunked||!equals(value,"chunked")){s.error=HTTPS_ERROR_PROTOCOL;return false;}h.chunked=true;}
            else if(equals(line,"Content-Encoding")){if(!equals(value,"identity")){s.error=HTTPS_ERROR_PROTOCOL;return false;}}
            else if(equals(line,"Connection")){if(!equals(value,"keep-alive"))h.close=true;}
            else if(equals(line,"Location")){if(h.location[0]||!copy(h.location,sizeof h.location,value)){s.error=HTTPS_ERROR_PROTOCOL;return false;}}
        }
        if(s.error||https_stopped(s.job))return false;if(h.chunked&&h.has_size){s.error=HTTPS_ERROR_PROTOCOL;return false;}
        if(h.status>=200)return true;if(h.status==101){s.error=HTTPS_ERROR_PROTOCOL;return false;}
    }s.error=HTTPS_ERROR_PROTOCOL;return false;
}
bool store_body(Session& s,void* file,const void* bytes,uint64_t n){
    auto& j=s.job;const auto received=j.received.load();if(n>j.max_bytes-received){s.error=HTTPS_ERROR_LIMIT;return false;}
    if(j.kind==HTTPS_MEMORY){
        const uint64_t total=j.total.load(),limit=total?total:j.max_bytes;
        if(!https_body_append(j.body,received,bytes,n,limit,platform->response_allocate)){s.error=HTTPS_ERROR_STORAGE;return false;}
    }
    else if(!platform->file_write(file,received,bytes,n)){s.error=HTTPS_ERROR_STORAGE;return false;}
    j.received.store(received+n);return true;
}
bool fixed_body(Session& s,void* file,uint64_t n){
    while(n&&s.active()){if(s.pos==s.used&&s.read_some()<=0){if(!s.error)s.error=HTTPS_ERROR_PROTOCOL;return false;}uint32_t bytes=s.used-s.pos;if(bytes>n)bytes=uint32_t(n);if(!store_body(s,file,s.buffer+s.pos,bytes))return false;s.pos+=bytes;n-=bytes;}return !n;
}
bool body(Session& s,const Headers& h,void* file){
    if(h.status==204||h.status==205){if(h.chunked||(h.has_size&&h.size)){s.error=HTTPS_ERROR_PROTOCOL;return false;}return true;}
    if(h.chunked){char line[1024];uint32_t budget=8192;for(;;){
        budget=8192;if(!s.line(line,sizeof line,budget))return false;uint64_t size=0;uint32_t i=0;
        while(line[i]&&line[i]!=';'){const char c=lower(line[i]);const unsigned digit=c>='0'&&c<='9'?unsigned(c-'0'):c>='a'&&c<='f'?unsigned(c-'a'+10):16;if(digit>=16||i>=16||size>(UINT64_MAX-digit)/16){s.error=HTTPS_ERROR_PROTOCOL;return false;}size=size*16+digit;++i;}
        if(!i){s.error=HTTPS_ERROR_PROTOCOL;return false;}if(!size){budget=8192;while(s.line(line,sizeof line,budget)){if(!line[0])return true;if(!tls_find(line,':')||line[0]==' '||line[0]=='\t'){s.error=HTTPS_ERROR_PROTOCOL;return false;}}return false;}
        if(size>s.job.max_bytes-s.job.received.load()){s.error=HTTPS_ERROR_LIMIT;return false;}
        if(!fixed_body(s,file,size))return false;if(s.byte()!='\r'||s.byte()!='\n'){s.error=HTTPS_ERROR_PROTOCOL;return false;}
    }}
    if(h.has_size){s.job.total.store(h.size);if(h.size>s.job.max_bytes){s.error=HTTPS_ERROR_LIMIT;return false;}return fixed_body(s,file,h.size);}
    for(;;){if(!s.active())return false;if(s.pos<s.used){if(!store_body(s,file,s.buffer+s.pos,s.used-s.pos))return false;s.pos=s.used;}const int r=s.read_some();if(!r)return true;if(r<0)return false;}
}
}
extern "C" void* tsuchi_tls_calloc(size_t n,size_t size){if(!platform||!size||n>SIZE_MAX/size||n*size>(8u<<20))return nullptr;void* p=platform->allocate(n*size);if(p)memset(p,0,n*size);return p;}
extern "C" void tsuchi_tls_free(void* p){if(platform&&p)platform->free(p);}
extern "C" int64_t tsuchi_tls_time(int64_t* out){const int64_t t=platform?platform->unix_seconds():-1;if(out)*out=t;return t;}
// Instantiate once from fresh trusted entropy supplied by the platform.
// Emulator kernel/CSRNG output is not an acceptable fallback.
// HMAC_DRBG's seed-buffer API has no automatic entropy-source reseeding.
extern "C" bool tsuchi_https_random_seed(const void* seed,uint32_t bytes){
    if(!platform||!seed||bytes!=32||rng_ready.load())return false;
    mbedtls_hmac_drbg_init(&process_rng);const auto* md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if(!md||mbedtls_hmac_drbg_seed_buf(&process_rng,md,static_cast<const unsigned char*>(seed),bytes)){mbedtls_hmac_drbg_free(&process_rng);return false;}
    rng_ready.store(1);return true;
}
// SHA-256 of a buffer, for the platform's entropy pool.
extern "C" bool tsuchi_https_sha256(const void* data,uint32_t bytes,uint8_t* out){
    return data&&out&&mbedtls_sha256(static_cast<const unsigned char*>(data),bytes,out,0)==0;
}
extern "C" bool tsuchi_https_random_fill(void* out,uint64_t bytes){
    if(!out||!rng_ready.load()||bytes>(1u<<20))return false;while(!rng_lock.exchange(0,1))platform->sleep_ms(1);
    bool ok=bytes<=(1ull<<30)-rng_bytes;uint64_t offset=0;
    while(ok&&offset<bytes){const auto n=size_t(bytes-offset>1024?1024:bytes-offset);ok=mbedtls_hmac_drbg_random(&process_rng,static_cast<unsigned char*>(out)+offset,n)==0;offset+=n;}
    if(ok)rng_bytes+=bytes;rng_lock.store(0);return ok;
}
extern "C" mbedtls_ms_time_t mbedtls_ms_time(void){return platform?int64_t(platform->milliseconds()):0;}
extern "C" struct tm* mbedtls_platform_gmtime_r(const mbedtls_time_t* value,struct tm* out){
    if(!value||!out||*value<0||*value>253402300799LL)return nullptr;*out={};int64_t days=*value/86400;const int64_t seconds=*value%86400;
    out->tm_hour=int(seconds/3600);out->tm_min=int(seconds/60%60);out->tm_sec=int(seconds%60);out->tm_wday=int((days+4)%7);
    int year=1970;auto leap=[](int y){return y%4==0&&(y%100!=0||y%400==0);};while(days>=(leap(year)?366:365)){days-=leap(year)?366:365;++year;}
    out->tm_year=year-1900;out->tm_yday=int(days);static const int months[]={31,28,31,30,31,30,31,31,30,31,30,31};int month=0;
    while(days>=months[month]+(month==1&&leap(year)?1:0)){days-=months[month]+(month==1&&leap(year)?1:0);++month;}
    out->tm_mon=month;out->tm_mday=int(days+1);return out;
}
namespace tsuchi::https {
bool set_https_tls_platform(const HttpsPlatform* p){
    if(!p||!p->allocate||!p->free||!p->response_allocate||!p->response_free||!p->random||!p->unix_seconds||!p->milliseconds||!p->sleep_ms||!p->tcp_open||!p->tcp_read||!p->tcp_write||!p->tcp_close||!p->roots||!p->roots_bytes||p->roots_bytes>(512u<<10)||p->roots[p->roots_bytes-1])return false;
    if(platform&&platform!=p)return false;platform=p;return true;
}
uint64_t https_tls_diagnostic(uint32_t i){return i<4?diagnostics[i].load():0;}
void https_tls_run(HttpsJob& j){
    if(!platform){https_finish(j,HTTPS_ERROR_TLS);return;}
    if(j.kind==HTTPS_FILE&&(!platform->file_open||!platform->file_write||!platform->file_commit||!platform->file_abort)){https_finish(j,HTTPS_ERROR_STORAGE);return;}
    const uint64_t deadline=platform->milliseconds()+j.timeout_ms;char url[HTTPS_URL_BYTES]{};copy(url,sizeof url,j.url);void* file=nullptr;uint32_t error=0;int transport=0;bool complete=false;
    for(unsigned hop=0;hop<=5&&!https_stopped(j);++hop){
        Url target;if(!parse_url(url,target)){error=HTTPS_ERROR_PROTOCOL;break;}
        char request[HTTPS_URL_BYTES+512]{};uint32_t n=0;auto append=[&](const char* text){while(*text&&n+1<sizeof request)request[n++]=*text++;request[n]=0;return !*text;};
        const bool ok=append("GET ")&&append(target.path)&&append(" HTTP/1.1\r\nHost: ")&&append(target.authority)&&append("\r\nUser-Agent: Tsuchi/net\r\nAccept: */*\r\nAccept-Encoding: identity\r\n\r\n");
        char next[HTTPS_URL_BYTES]{};bool redirect=false;
        {
            void* storage=platform->allocate(sizeof(Session));if(!storage){error=HTTPS_ERROR_STORAGE;break;}
            // Placement construction without depending on the hosted <new> header.
            auto* s=::new(storage) Session(j,deadline);

            Headers h;redirect=false;
            if(!ok)s->error=HTTPS_ERROR_PROTOCOL;
            else if(s->connect(target)&&s->write_all(request,n)&&headers(*s,h)){
                redirect=h.status==301||h.status==302||h.status==303||h.status==307||h.status==308;
                if(redirect){if(hop==5||!redirect_url(target,h.location,next))s->error=HTTPS_ERROR_PROTOCOL;}
                else if(h.status<200||h.status>=300)s->error=HTTPS_ERROR_HTTP;
                else if(h.status==206)s->error=HTTPS_ERROR_PROTOCOL;
                else {
                    if(j.kind==HTTPS_FILE&&!platform->file_open(j,&file))s->error=HTTPS_ERROR_STORAGE;
                    else complete=body(*s,h,file);
                }
            }
            error=s->error;transport=s->transport;
            // A request owns its TCP connection. Completion closes it, so a
            // stopped mod never leaves a parked connection behind.
            Link* link=s->link;s->~Session();platform->free(storage);destroy(link);
        }
        if(error||https_stopped(j)||complete)break;
        if(!redirect){error=HTTPS_ERROR_NETWORK;break;}copy(url,sizeof url,next);
    }
    if(file){
        if(complete&&!error&&!https_stopped(j)){if(platform->file_commit(file)){j.error.store(0);j.transport_error.store(0);j.state.store(HTTPS_SUCCEEDED);return;}error=HTTPS_ERROR_STORAGE;}
        platform->file_abort(file);
    }
    if(!complete&&!error&&!https_stopped(j))error=HTTPS_ERROR_NETWORK;
    // Cancellation is sampled once by finish. Body cleanup precedes its
    // terminal-state publication, so the game thread cannot race a second free.
    https_finish(j,error,transport,platform->response_free);
}
}
