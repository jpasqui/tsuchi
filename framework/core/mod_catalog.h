// Public-source metadata. Parsing never changes the caller's last good page on failure.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace tsuchi::catalog {
enum class Source : uint32_t { github, gamebanana };
constexpr uint32_t kPageSize = 6, kFiles = 12;
struct Item { char key[128], name[161], description[321], author[128]; char replacing[48]{}; };
struct Page { Item items[kPageSize]; uint32_t count; bool more, incomplete; };
struct File { char name[161], url[1024], release[128], identity[32], digest[65]; uint64_t bytes; uint64_t updated=0; };
struct Files { File items[kFiles]; uint32_t count; bool more, more_files; };
bool search_url(Source, const char* query, uint32_t page, char (&out)[1024]);
bool files_url(Source, const Item&, bool preview, uint32_t page, char (&out)[1024]);
bool parse_page(Source, const char*, size_t, uint32_t page, Page&);
bool parse_files(Source, const Item&, const char*, size_t, bool preview, Files&, uint32_t skip=0);
void plain(const char*, char*, size_t); // bounded UTF-8 without rich-text/control characters
bool digest_valid(const char*);
uint64_t iso_time(const char*);
}
