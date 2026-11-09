#ifndef P4M_DOWNLOADER_INSTALL_H
#define P4M_DOWNLOADER_INSTALL_H
#include "transport.h"
namespace tsuchi { namespace browser { namespace install {
constexpr uint32_t max_entries=128, max_name=512;
constexpr uint64_t max_archive=512ull<<20, max_expanded=1ull<<30;
enum class State:uint32_t { Idle,Queued,Validating,Extracting,Committing,Installed,Failed,Cancelled };
enum class Error:uint32_t { None,Unavailable,Storage,Zip,Unsupported,Path,Limit,Crc,Exists,Cancelled,Conflict };
constexpr uint32_t conflict_result=0xfffffffe, limit_result=0xfffffffd;
constexpr uint32_t max_installed=128;
// Optional fixed-size local metadata; no pointers, markup or URLs from workers
// reach a native widget. Legacy installs without metadata keep numeric identity.
struct Record {uint32_t abi,submission_id,file_id,asset_count;uint64_t expanded_bytes;char title[160],filename[256],thumbnail[512];char mod_id[48]{},source_key[128]{};uint32_t source=1;bool enabled=true,pending=false,legacy_assets=false,legacy_cheats=false,live_assets=false;uint64_t updated=0;char digest[65]{},archive_name[256]{};};

enum class LibraryState:uint32_t { Idle,Queued,Loading,Ready,Failed };
struct Library {LibraryState state;uint32_t revision,result,count,removed_submission,removed_file;Record records[max_installed];};
struct Request {uint32_t submission_id,file_id;char cache_key[P4M_HTTPS_PATH_BYTES];char title[160]{},filename[256]{},thumbnail[512]{};};
struct Status {State state;Error error;uint32_t sdk_result,submission_id,file_id,entry_count,completed_files;uint64_t compressed_done,compressed_total,expanded_done,expanded_total;bool cancel_requested;char group[96],current_file[256],message[321];};
class Installer {
public:
 bool available() const {return true;}
 bool library_available() const {return true;}
 P4M_Result begin(const Request&) {return P4M_UNSUPPORTED;}
 bool status(Status&) const;
 void cancel();
 P4M_Result refresh_library();
 P4M_Result uninstall(uint32_t submission,uint32_t file);
 bool library(Library&) const;
};
Installer& installer();
} } }
#endif
