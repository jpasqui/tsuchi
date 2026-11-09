#pragma once
#include "mod_catalog.h"
#include "mod_receipt.h"
#include "text.h"
#include "tsuchi/core.h"
namespace tsuchi {
enum class DownloadPhase { idle, downloading, extracting, validating, installed, failed, cancelled };
struct DownloadStatus {DownloadPhase phase;uint64_t done,total;char message[321];};
void mod_download_start();
TSUCHI_Result mod_download_begin(catalog::Source,const catalog::Item&,const catalog::File&);
void mod_download_cancel();
DownloadStatus mod_download_status();
uint32_t downloaded_package_count();
const catalog::Receipt* downloaded_package(uint32_t);
TSUCHI_Result refresh_downloaded_packages();
TSUCHI_Result remove_downloaded_package(uint32_t);
TSUCHI_Result activate_downloaded_assets(const char* id,Text& problem);
bool downloaded_assets_active(const char* id);
void mod_browser_start(TSUCHI_Mod* framework);
}
