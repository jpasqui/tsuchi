#pragma once
#include <stdint.h>
namespace tsuchi::https {
constexpr uint32_t HTTPS_GET=0,HTTPS_POST=1,HTTPS_PUT=2,HTTPS_PATCH=3,HTTPS_DELETE=4;
constexpr uint32_t HTTPS_HEADERS_LIMIT=8192,HTTPS_UPLOAD_LIMIT=65536,HTTPS_HEADER_COUNT=32;
inline char http_lower(char c){return c>='A'&&c<='Z'?char(c+32):c;}
inline bool http_name(const char* a,uint32_t bytes,const char* b){
    uint32_t n=0;while(b[n]){if(n==bytes||http_lower(a[n])!=http_lower(b[n]))return false;++n;}return n==bytes;
}
inline bool http_token(char c){
    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'))return true;
    const char* allowed="!#$%&'*+-.^_`|~";while(*allowed)if(c==*allowed++)return true;return false;
}
// The caller still owns the input. Validation completes before any worker starts.
inline bool https_request_valid(uint32_t method,const char* headers,const void* body,uint64_t size,uint32_t* header_bytes){
    if(method>HTTPS_DELETE||!headers||size>HTTPS_UPLOAD_LIMIT||(size&&!body)||(method==HTTPS_GET&&size))return false;
    uint32_t bytes=0;while(bytes<=HTTPS_HEADERS_LIMIT&&headers[bytes])++bytes;
    if(bytes>HTTPS_HEADERS_LIMIT)return false;
    uint32_t names[HTTPS_HEADER_COUNT]{},lengths[HTTPS_HEADER_COUNT]{},count=0,pos=0;
    while(pos<bytes){
        if(count==HTTPS_HEADER_COUNT)return false;
        const uint32_t begin=pos;while(pos<bytes&&http_token(headers[pos]))++pos;
        const uint32_t name_bytes=pos-begin;
        if(!name_bytes||pos==bytes||headers[pos++]!=':')return false;
        const char* reserved[]={"Host","Content-Length","Transfer-Encoding","Connection","Expect","Upgrade","Trailer","TE","Proxy-Authorization","Proxy-Connection","Accept-Encoding","User-Agent","Accept"};
        for(const char* name:reserved)if(http_name(headers+begin,name_bytes,name))return false;
        for(uint32_t i=0;i<count;++i){
            if(lengths[i]!=name_bytes)continue;bool equal=true;
            for(uint32_t k=0;k<name_bytes;++k)if(http_lower(headers[names[i]+k])!=http_lower(headers[begin+k])){equal=false;break;}
            if(equal)return false;
        }
        names[count]=begin;lengths[count++]=name_bytes;
        while(pos<bytes&&headers[pos]!='\r'){
            const auto c=uint8_t(headers[pos++]);if((c<32&&c!='\t')||c>=127)return false;
        }
        if(pos+1>=bytes||headers[pos]!='\r'||headers[pos+1]!='\n')return false;pos+=2;
    }
    if(header_bytes)*header_bytes=bytes;return true;
}
inline const char* https_method_name(uint32_t method){
    switch(method){case HTTPS_GET:return "GET";case HTTPS_POST:return "POST";case HTTPS_PUT:return "PUT";case HTTPS_PATCH:return "PATCH";case HTTPS_DELETE:return "DELETE";default:return nullptr;}
}
}
