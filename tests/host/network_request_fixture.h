#pragma once
#include <cstring>
#include "net_service.h"
namespace network_request_fixture {
inline TSUCHI_Mod* owner=nullptr;
inline bool live=false,completed=false;
inline uint64_t next=1,current=0;
inline constexpr unsigned char payload[]={'{',0,0xc3,0xa9,'}'};
inline TSUCHI_Result request(TSUCHI_Mod* mod,uint32_t method,const char* url,const char* headers,const void* data,uint64_t size,uint64_t maximum,uint32_t timeout,TSUCHI_Request* out){
    if(method<TSUCHI_HTTP_METHOD_POST||method>TSUCHI_HTTP_METHOD_REMOVE||std::strcmp(url,"https://example.org/runs")||
       std::strcmp(headers,"Content-Type: application/json\r\nAuthorization: Bearer fixture\r\n")||size!=sizeof payload||
       std::memcmp(data,payload,sizeof payload)||maximum!=65536||timeout!=1000)return TSUCHI_INVALID;
    if(live)return TSUCHI_BUSY;
    owner=mod;live=true;completed=false;out->id=current=next++;return TSUCHI_OK;
}
inline TSUCHI_Result info(TSUCHI_Mod* mod,TSUCHI_Request id,TSUCHI_RequestInfo* out){
    if(!live||mod!=owner||id.id!=current)return TSUCHI_NOT_FOUND;
    *out={uint32_t(completed?TSUCHI_REQUEST_STATE_SUCCEEDED:TSUCHI_REQUEST_STATE_QUEUED),completed?200u:0u,completed?uint64_t(sizeof payload):0,0,""};return TSUCHI_OK;
}
inline TSUCHI_Result read(TSUCHI_Mod* mod,TSUCHI_Request id,uint64_t offset,void* buffer,uint64_t capacity,uint64_t* out){
    TSUCHI_RequestInfo status{};if(info(mod,id,&status)!=TSUCHI_OK)return TSUCHI_NOT_FOUND;
    if(!completed)return TSUCHI_NOT_READY;
    const auto left=offset<sizeof payload?sizeof payload-offset:0;*out=left<capacity?left:capacity;
    if(*out)std::memcpy(buffer,payload+offset,*out);return TSUCHI_OK;
}
inline TSUCHI_Result release(TSUCHI_Mod* mod,TSUCHI_Request id){
    TSUCHI_RequestInfo status{};if(info(mod,id,&status)!=TSUCHI_OK)return TSUCHI_NOT_FOUND;
    live=false;return TSUCHI_OK;
}
inline bool finish(TSUCHI_Mod* mod,uint64_t id){
    if(!live||owner!=mod||current!=id||completed)return false;
    completed=true;tsuchi::net::emit_completed_to(owner,{{id},TSUCHI_REQUEST_STATE_SUCCEEDED,200});return true;
}
inline void start(){
    static tsuchi::net::Backend backend{};backend.request=request;backend.request_info=info;backend.read=read;backend.release_request=release;
    tsuchi::net::set_backend(&backend);
}
}
