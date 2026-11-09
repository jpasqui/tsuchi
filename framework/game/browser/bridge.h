#pragma once
#include "provider.h"
#include "install.h"
namespace tsuchi::browser {
void bridge_start(TSUCHI_Mod*);
const P4M_HttpsApi* transport();
P4M_Result begin_install(const Listing&, const File&,const char* replacing=nullptr);
void install_download_status(Download&);
void cancel_install();
uint32_t source_id(uint32_t source,const char* key);
const char* source_key(uint32_t id);
uint32_t file_id(uint32_t source,const char* key);
}
