#include "mod_catalog.h"
#include "json.h"
#include <string.h>
namespace tsuchi::catalog {
namespace {
struct Writer {
    char* out; size_t room, used=0; bool ok=true;
    void add(const char* s) { while(*s) { if(used+1>=room) { ok=false; return; } out[used++]=*s++; } out[used]=0; }
    void number(uint64_t n) { char digits[24]; char* p=digits+sizeof digits; *--p=0; do {*--p=char('0'+n%10);n/=10;}while(n);add(p); }
    void encode(const char* s) { constexpr char hex[]="0123456789ABCDEF"; for(;*s;++s) { const auto c=uint8_t(*s); char b[4]={char(c),0,0,0}; if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')) {b[0]='%';b[1]=hex[c>>4];b[2]=hex[c&15];} add(b); } }
};
const Json* field(const Json* j,const char* key) { return j&&j->kind==Json::Kind::object?j->find(key):nullptr; }
const char* text(const Json* j) { return j&&j->kind==Json::Kind::text&&strlen(j->text)==j->bytes?j->text:""; }
uint64_t number(const Json* j) { return j&&j->kind==Json::Kind::number&&j->whole&&j->integer>0?uint64_t(j->integer):0; }
bool flag(const Json* j) { return j&&j->kind==Json::Kind::boolean&&j->boolean; }
bool copy(char* out,size_t size,const char* value) { if(strlen(value)>=size)return false;memcpy(out,value,strlen(value)+1);return true; }
bool key(const char* s,bool repo) {
    if(!*s||strlen(s)>127)return false;
    uint32_t slash=0;const char* part=s;
    for(const char* p=s;;++p) {
        if(!*p||*p=='/') {
            const auto n=size_t(p-part);
            if(!n||(n==1&&part[0]=='.')||(n==2&&part[0]=='.'&&part[1]=='.'))return false;
            if(!*p)break;if(!repo||++slash>1)return false;part=p+1;
        }else if(!((*p>='0'&&*p<='9')||(repo&&((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||*p=='_'||*p=='-'||*p=='.'))))return false;
    }
    return slash==(repo?1u:0u);
}
bool prefix(const char* s,const char* p) { return strlen(s)>=strlen(p)&&!memcmp(s,p,strlen(p)); }
bool zip(const char* s) { size_t n=strlen(s);if(n<5)return false;s+=n-4;return s[0]=='.'&&(s[1]=='z'||s[1]=='Z')&&(s[2]=='i'||s[2]=='I')&&(s[3]=='p'||s[3]=='P'); }
bool url(const char* s) {if(!prefix(s,"https://")||strlen(s)>=1024)return false;for(;*s;++s)if(uint8_t(*s)<=32||uint8_t(*s)>=127||*s=='\\'||*s=='@'||*s=='#')return false;return true;}
}
uint64_t iso_time(const char* s){
 if(!s||strlen(s)!=20||s[4]!='-'||s[7]!='-'||s[10]!='T'||s[13]!=':'||s[16]!=':'||s[19]!='Z')return 0;
 auto digits=[&](uint32_t at,uint32_t count){int n=0;while(count--){if(s[at]<'0'||s[at]>'9')return -1;n=n*10+s[at++]-'0';}return n;};
 int y=digits(0,4),m=digits(5,2),d=digits(8,2),h=digits(11,2),min=digits(14,2),sec=digits(17,2);
 if(y<1970||m<1||m>12||d<1||d>31||h<0||h>23||min<0||min>59||sec<0||sec>60)return 0;
 y-=m<=2;const int era=y/400,yoe=y-era*400,doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
 const int days=era*146097+yoe*365+yoe/4-yoe/100+doy-719468;
 return days<0?0:uint64_t(days)*86400+uint64_t(h*3600+min*60+sec);
}
void plain(const char* s,char* out,size_t room) {
    size_t n=0; if(!room)return;
    while(*s&&n+1<room) {uint8_t c=uint8_t(*s);size_t bytes=c<128?1:c<224?2:c<240?3:4;if(n+bytes>=room)break;
        bool complete=true;for(size_t i=1;i<bytes;++i)if(!s[i]||(uint8_t(s[i])&192)!=128){complete=false;break;}if(!complete)break;
        if(c<32||c==127||c=='<'||c=='>'||c=='['||c==']')out[n++]=' ';else {memcpy(out+n,s,bytes);n+=bytes;}s+=bytes;
    }out[n]=0;
}
bool digest_valid(const char* s) {if(strlen(s)!=64)return false;for(;*s;++s)if(!((*s>='0'&&*s<='9')||(*s>='a'&&*s<='f')))return false;return true;}
bool search_url(Source source,const char* query,uint32_t page,char (&out)[1024]) {
    if(!page||page>166||strlen(query)>100)return false;Writer w{out,sizeof out};
    if(source==Source::github) {w.add("https://api.github.com/search/repositories?q=");
        // Treat the keyboard input as words: it cannot inject an OR or topic qualifier.
        char words[101]{};size_t n=0;for(const char* p=query;*p&&n<100;++p) {const uint8_t c=uint8_t(*p);words[n++]=c==':'||c=='"'||c=='\\'?' ':*p;}
        w.add("topic%3Atsuchi-mod%20is%3Apublic%20fork%3Afalse");if(*words){w.add("%20%22");w.encode(words);w.add("%22");}w.add("&sort=updated&per_page=6&page=");
    }else if(*query){w.add("https://gamebanana.com/apiv11/Util/Search/Results?_sSearchString=");w.encode(query);w.add("&_sModelName=Mod&_idGameRow=18712&_nPerpage=6&_nPage=");}
    else w.add("https://gamebanana.com/apiv11/Mod/Index?_nPerpage=6&_aFilters%5BGeneric_Game%5D=18712&_nPage=");
    w.number(page);return w.ok;
}
bool files_url(Source source,const Item& item,bool preview,uint32_t page,char (&out)[1024]) {
    (void)preview;if(!key(item.key,source==Source::github)||!page||page>100)return false;Writer w{out,sizeof out};
    if(source==Source::github){w.add("https://api.github.com/repos/");w.add(item.key);w.add("/releases?per_page=6&page=");w.number(page);}
    else {w.add("https://gamebanana.com/apiv11/Mod/");w.add(item.key);w.add("/ProfilePage");}return w.ok;
}
bool parse_page(Source source,const char* bytes,size_t size,uint32_t page,Page& out) {
    const Json* root=read_json(bytes,size);if(!root)return false;Page made{};bool ok=true;
    const Json* items=field(root,source==Source::github?"items":"_aRecords");
    if(!items||items->kind!=Json::Kind::list)ok=false;
    for(const Json* j=items?items->first:nullptr;ok&&j;j=j->next) {
        if(made.count==kPageSize){made.more=true;break;}Item item{};
        if(source==Source::github){
            if(flag(field(j,"private"))||flag(field(j,"fork")))continue;
            if(!copy(item.key,sizeof item.key,text(field(j,"full_name")))||!key(item.key,true)){ok=false;break;}
            plain(text(field(j,"name")),item.name,sizeof item.name);plain(text(field(j,"description")),item.description,sizeof item.description);
            plain(text(field(field(j,"owner"),"login")),item.author,sizeof item.author);
        }else {
            if(flag(field(j,"_bIsPrivate"))||flag(field(j,"_bIsWithheld"))||flag(field(j,"_bIsTrashed")))continue;
            if(number(field(field(j,"_aGame"),"_idRow"))!=18712)continue;
            uint64_t id=number(field(j,"_idRow"));if(!id){ok=false;break;}Writer w{item.key,sizeof item.key};w.number(id);
            plain(text(field(j,"_sName")),item.name,sizeof item.name);plain(text(field(j,"_sDescription")),item.description,sizeof item.description);
            plain(text(field(field(j,"_aSubmitter"),"_sName")),item.author,sizeof item.author);
        }
        if(!*item.name){ok=false;break;}made.items[made.count++]=item;
    }
    if(source==Source::github){made.more=uint64_t(page)*kPageSize<number(field(root,"total_count"))&&page<166;made.incomplete=flag(field(root,"incomplete_results"));}
    else made.more=items&&items->count>=kPageSize;
    release_json(root);if(ok)out=made;return ok;
}
bool parse_files(Source source,const Item& item,const char* bytes,size_t size,bool preview,Files& out,uint32_t skip) {
    const Json* root=read_json(bytes,size);if(!root)return false;Files made{};bool ok=true;
    const auto add=[&](const Json* j,const char* release) {
        const char* name=text(field(j,source==Source::github?"name":"_sFile"));if(!zip(name))return;
        File f{};uint64_t id=number(field(j,source==Source::github?"id":"_idRow"));
        const char* link=text(field(j,source==Source::github?"browser_download_url":"_sDownloadUrl"));
        if(!id||!url(link)||!copy(f.url,sizeof f.url,link))return;
        char expected[256]{};Writer p{expected,sizeof expected};
        if(source==Source::github){p.add("https://github.com/");p.add(item.key);p.add("/releases/download/");}
        else {p.add("https://gamebanana.com/dl/");p.number(id);}
        if(!p.ok||!(source==Source::github?prefix(link,expected):!strcmp(link,expected)))return;
        f.bytes=number(field(j,source==Source::github?"size":"_nFilesize"));if(!f.bytes||f.bytes>(512ull<<20))return;
        f.updated=source==Source::github?iso_time(text(field(j,"updated_at"))):number(field(j,"_tsDateAdded"));
        plain(name,f.name,sizeof f.name);plain(release,f.release,sizeof f.release);Writer w{f.identity,sizeof f.identity};w.number(id);
        const char* digest=text(field(j,"digest"));if(*digest){if(!prefix(digest,"sha256:")||!digest_valid(digest+7))return;copy(f.digest,sizeof f.digest,digest+7);}
        if(skip){--skip;return;}if(made.count==kFiles){made.more_files=true;return;}
        made.items[made.count++]=f;
    };
    if(source==Source::github){
        if(root->kind!=Json::Kind::list)ok=false;
        made.more=root->count>=6;
        for(const Json* r=ok?root->first:nullptr;r;r=r->next){if(flag(field(r,"draft"))||(!preview&&flag(field(r,"prerelease"))))continue;
            const Json* assets=field(r,"assets");if(!assets||assets->kind!=Json::Kind::list){ok=false;break;}
            for(const Json* a=assets->first;a;a=a->next)if(!strcmp(text(field(a,"state")),"uploaded"))add(a,text(field(r,"tag_name")));
        }
    }else {
        char id[32]{};Writer w{id,sizeof id};w.number(number(field(root,"_idRow")));
        const Json* files=field(root,"_aFiles");ok=!strcmp(id,item.key)&&number(field(field(root,"_aGame"),"_idRow"))==18712&&!flag(field(root,"_bIsPrivate"))&&!flag(field(root,"_bIsWithheld"))&&!flag(field(root,"_bIsTrashed"))&&files&&(files->kind==Json::Kind::list||files->kind==Json::Kind::object);
        for(const Json* a=ok?files->first:nullptr;a;a=a->next)add(a,"GameBanana");
    }
    release_json(root);if(ok)out=made;return ok;
}
}
