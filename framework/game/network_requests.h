#pragma once
#include "net_service.h"
namespace tsuchi {
TSUCHI_Result network_request_get(TSUCHI_Mod*, const char*, uint64_t, uint32_t, TSUCHI_Request*);
TSUCHI_Result network_request_info(TSUCHI_Mod*, TSUCHI_Request, TSUCHI_RequestInfo*);
TSUCHI_Result network_request_read(TSUCHI_Mod*, TSUCHI_Request, uint64_t, void*, uint64_t, uint64_t*);
TSUCHI_Result network_request_cancel(TSUCHI_Mod*, TSUCHI_Request);
TSUCHI_Result network_request_release(TSUCHI_Mod*, TSUCHI_Request);
void network_requests_start();
}
