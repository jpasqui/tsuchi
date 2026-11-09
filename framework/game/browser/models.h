#ifndef TSUCHI_BROWSER_MODELS_H
#define TSUCHI_BROWSER_MODELS_H
#include "transport.h"
namespace tsuchi { namespace browser {
constexpr uint32_t game_id=18712, batch_size=24, search_batch_size=15, max_files=32, max_previews=12, max_categories=16, json_bytes=512u<<10;
// Image URLs are about 70 characters. One that does not fit is treated as absent.
constexpr uint32_t url_bytes=512;
// Listing records parsed per pump() call.
constexpr uint32_t parse_records_per_pump=6;
enum class Sort : uint32_t { Latest=0, Popular=1, Downloads=2 };
struct Query { uint32_t category_id; Sort sort; char search[128]; uint32_t author_id; };
// url: the screenshot itself. card: about 530 px wide, for a listing card.
// thumbnail: about 220 px wide. tile: about 100 px wide, for the screenshot strip.
struct Preview { uint32_t id,width,height; char url[url_bytes],card[url_bytes],thumbnail[url_bytes],tile[url_bytes],caption[160]; };
enum Statistic : uint32_t { Likes=1,Views=2,Downloads=4,Comments=8 };
struct Listing { uint32_t source=1; char source_key[128]{};  uint32_t id,category_id; uint64_t updated,created,likes,views,downloads,comments; uint32_t statistics; bool has_files; char title[160],author[96],category[96],version[64],profile_url[64]; Preview image; uint32_t author_id; char author_avatar_url[url_bytes]; };
struct Batch { uint32_t generation,page,count,total; bool has_more; uint32_t failed_sources=0; Listing listings[batch_size+6]; uint32_t perpage; };
struct AuthorProfile { uint32_t generation,id; uint64_t joined,subscribers; bool has_joined,has_subscribers; char name[96],title[160],avatar_url[url_bytes],profile_url[128]; };
struct File { uint32_t id,submission_id; uint64_t size,updated; char name[256],url[1024],version[128],description[256]; char identity[32]{},digest[65]{}; };
struct Detail { uint32_t generation; Listing listing; char description[4096]; uint32_t file_count,preview_count; bool description_truncated,files_truncated,previews_truncated; File files[max_files]; Preview previews[max_previews];
    char tagline[200]; };   // the author's one-line summary (_sDescription), empty when there is none
// A listing does not carry its download count; this is the separate answer for
// a page of listings. found: the provider answered for that ID.
struct Counts { uint32_t generation,count; struct Record { uint32_t id; uint64_t downloads; } records[batch_size]; };
struct Category { uint32_t id; char name[96]; };
struct Categories { uint32_t count; bool truncated; Category records[max_categories]; };
enum class State : uint32_t { Idle,Loading,Ready,Failed,Cancelled };
enum class Error : uint32_t { None,Api,Transport,Json,Schema,Limit,Unavailable };
struct Operation { State state; Error error; P4M_Result api_result; uint32_t generation; P4M_HttpsStatus transfer; bool cancel_requested; };
struct Download { Operation operation; uint32_t submission_id,file_id; char cache_key[P4M_HTTPS_PATH_BYTES]; };
} }
#endif
