#ifndef P4M_DOWNLOADER_JSON_H
#define P4M_DOWNLOADER_JSON_H
#include <stdint.h>
namespace tsuchi { namespace browser { namespace json {
struct Value { const char* begin; const char* end; explicit operator bool() const { return begin!=nullptr; } };
bool document(const char*,uint32_t,Value&);
Value member(Value,const char*);
bool next(Value,uint32_t&,Value&);
// Bounded object iteration; used by strict schema consumers to reject unknown
// and duplicate keys (member() alone intentionally cannot distinguish them).
bool entry(Value,uint32_t&,Value& key,Value& value);
bool real(Value,double&);
bool text(Value,char*,uint32_t,bool* truncated=nullptr);
bool number(Value,uint64_t&);
bool boolean(Value,bool&);
} } }
#endif
