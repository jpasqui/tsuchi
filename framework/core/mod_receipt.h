#pragma once
#include "mod_catalog.h"
#include "sha256.h"
#include <string.h>
#include <stddef.h>
namespace tsuchi::catalog {
constexpr uint32_t kPackageMods=16;
struct ReceiptV1 {
    char magic[8]; uint32_t source, count;
    char folder[33], key[128], title[161], asset[32], release[128], digest[65];
    char mods[kPackageMods][48];
    uint8_t checksum[32];
};
struct Receipt {
    char magic[8]; uint32_t source, count;
    char folder[33], key[128], title[161], asset[32], release[128], digest[65];
    char mods[kPackageMods][48];
    char filename[256]{};
    uint64_t updated=0;
    uint32_t legacy_assets=0; // payload kind: 0 native, 1 cooked assets, 2 cheats (V2-compatible)
    char previous[33]{};
    uint64_t revision=0;
    uint8_t checksum[32];
};
inline bool folder_valid(const char* s) { if(strlen(s)!=32)return false;for(;*s;++s)if(!((*s>='0'&&*s<='9')||(*s>='a'&&*s<='f')))return false;return true; }
inline void seal(Receipt& r) { memcpy(r.magic,"TSMOD02",8);sha256(&r,offsetof(Receipt,checksum),r.checksum); }
inline bool terminated(const char* s,size_t size) {for(size_t i=0;i<size;++i)if(!s[i])return true;return false;}
inline bool receipt_valid(const Receipt& r) {
    // Bounded terminators before using any persisted string.
    if(memcmp(r.magic,"TSMOD02",8)||r.source>1||!r.count||r.count>kPackageMods||r.folder[32]||r.digest[64]||!terminated(r.key,sizeof r.key)||!terminated(r.title,sizeof r.title)||!terminated(r.asset,sizeof r.asset)||!terminated(r.release,sizeof r.release))return false;
    if(r.legacy_assets>2)return false;
    if(!terminated(r.filename,sizeof r.filename))return false;
    if(!terminated(r.previous,sizeof r.previous)||(r.previous[0]&&!folder_valid(r.previous)))return false;
    if(!folder_valid(r.folder)||!digest_valid(r.digest))return false;
    for(uint32_t i=0;i<r.count;++i){const auto* id=r.mods[i];if(!terminated(id,48)||!*id)return false;for(const char* c=id;*c;++c)if(!((*c>='a'&&*c<='z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='_'||*c=='-'))return false;if(id[0]=='.'||!strcmp(id,"tsuchi")||(strlen(id)>=7&&!memcmp(id,"tsuchi.",7)))return false;for(uint32_t j=0;j<i;++j)if(!strcmp(id,r.mods[j]))return false;}
    uint8_t sum[32];sha256(&r,offsetof(Receipt,checksum),sum);return !memcmp(sum,r.checksum,32);
}
inline bool read_receipt(const void* bytes,size_t size,Receipt& r){
    if(size==sizeof(Receipt)){memcpy(&r,bytes,size);return receipt_valid(r);}
    if(size!=sizeof(ReceiptV1))return false;ReceiptV1 old{};memcpy(&old,bytes,size);
    uint8_t hash[32];sha256(&old,offsetof(ReceiptV1,checksum),hash);
    if(memcmp(old.magic,"TSMOD01",8)||memcmp(hash,old.checksum,32))return false;
    r={};memcpy(&r,&old,offsetof(ReceiptV1,checksum));seal(r);return receipt_valid(r);
}
inline bool same_mods(const Receipt& a,const Receipt& b){
    if(a.source!=b.source||strcmp(a.key,b.key)||a.count!=b.count)return false;
    for(uint32_t i=0;i<a.count;++i){bool found=false;for(uint32_t j=0;j<b.count;++j)if(!strcmp(a.mods[i],b.mods[j]))found=true;if(!found)return false;}return true;
}
}
