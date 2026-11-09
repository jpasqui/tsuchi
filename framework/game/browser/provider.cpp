#include "provider.h"
#include "bridge.h"
#include "json.h"
namespace tsuchi { namespace browser {
namespace {
using json::Value;
void clear(void*p,uint32_t n){auto*b=(uint8_t*)p;while(n--)*b++=0;}
uint32_t length(const char*s,uint32_t max){if(!s)return max;uint32_t n=0;while(n<max&&s[n])++n;return n;}
bool copy(char*d,uint32_t cap,const char*s){uint32_t n=length(s,cap);if(n>=cap)return false;for(uint32_t i=0;i<=n;++i)d[i]=s[i];return true;}
bool equal(const char*a,const char*b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
bool prefix(const char*a,const char*b){while(*b)if(*a++!=*b++)return false;return true;}
struct Writer {char*p;uint32_t cap,n;bool ok;Writer(char*b,uint32_t c):p(b),cap(c),n(0),ok(c>0){if(c)p[0]=0;}
    void add(const char*s){while(*s){if(n+1>=cap){ok=false;return;}p[n++]=*s++;p[n]=0;}}
    void num(uint64_t v){char b[21];uint32_t i=0;do{b[i++]=(char)('0'+v%10);v/=10;}while(v);while(i){char s[2]={b[--i],0};add(s);}}
};
bool search_text(const char*s,uint32_t n){
    bool visible=false;for(uint32_t i=0;i<n;){uint8_t c=(uint8_t)s[i++];uint32_t cp=c,extra=0,min=0;
        if(c>=0xc2&&c<=0xdf){cp=c&31;extra=1;min=0x80;}else if(c>=0xe0&&c<=0xef){cp=c&15;extra=2;min=0x800;}else if(c>=0xf0&&c<=0xf4){cp=c&7;extra=3;min=0x10000;}else if(c>=0x80)return false;
        if(extra>n-i)return false;for(uint32_t j=0;j<extra;++j){uint8_t b=(uint8_t)s[i++];if((b&0xc0)!=0x80)return false;cp=(cp<<6)|(b&63);}
        if((extra&&cp<min)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)||cp<32||(cp>=127&&cp<=159))return false;
        if(cp!=32)visible=true;
    }return visible;
}
void percent(Writer&w,const char*s){const char*hex="0123456789ABCDEF";while(*s){uint8_t c=(uint8_t)*s++;char b[4]={0,0,0,0};
    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')b[0]=(char)c;
    else{b[0]='%';b[1]=hex[c>>4];b[2]=hex[c&15];}w.add(b);
}}
bool unique(Value object,const char*const*keys,uint32_t count){
    if(!object||*object.begin!='{'||count>32)return false;uint32_t pos=0,seen=0;Value key,value;
    while(json::entry(object,pos,key,value)){char name[64];bool cut=false;if(!json::text(key,name,sizeof(name),&cut)||cut)continue;
        for(uint32_t i=0;i<count;++i)if(equal(name,keys[i])){uint32_t bit=1u<<i;if(seen&bit)return false;seen|=bit;}
    }return true;
}
bool uint32(Value v,uint32_t& out){uint64_t n;if(!json::number(v,n)||n>UINT32_MAX)return false;out=(uint32_t)n;return true;}
uint64_t num(Value v,const char*k){uint64_t n=0;json::number(json::member(v,k),n);return n;}
bool flag(Value v,const char*k){bool b=false;json::boolean(json::member(v,k),b);return b;}
void str(Value v,const char*k,char*out,uint32_t cap){json::text(json::member(v,k),out,cap);}
bool ascii_url(const char*s){uint32_t n=length(s,url_bytes);if(n<9||n>=url_bytes||!prefix(s,"https://"))return false;bool host=false;for(uint32_t i=8;i<n;++i){uint8_t c=(uint8_t)s[i];if(c<=32||c>=127||c=='#'||c=='@'||c=='\\')return false;if(i==8&&c=='/')return false;if(c=='/')host=true;}return host;}
bool image_url(const char*s){return ascii_url(s)&&prefix(s,"https://images.gamebanana.com/");}
// A URL that does not fit its field is dropped, never requested truncated.
void url_text(Value v,const char*k,char*out,uint32_t cap){bool cut=false;if(!json::text(json::member(v,k),out,cap,&cut)||cut)out[0]=0;}
bool image_file(Value v,const char*base,const char*key,char*out){
    char file[256];url_text(v,key,file,sizeof(file));out[0]=0;if(!base[0]||!file[0])return false;
    Writer w(out,url_bytes);w.add(base);w.add("/");w.add(file);if(!w.ok||!image_url(out))out[0]=0;return out[0]!=0;
}
// GameBanana lists 220 and 530 px copies for a mod's first screenshot only.
// Every other screenshot has just the original and a 100 px copy, so the
// screenshot itself is always the original; the image cache reduces it.
void image(Value v,uint32_t id,Preview&out){
    clear(&out,sizeof(out));out.id=id;char base[url_bytes];url_text(v,"_sBaseUrl",base,sizeof(base));
    if(!image_file(v,base,"_sFile",out.url)){url_text(v,"_sUrl",out.url,sizeof(out.url));if(!image_url(out.url))out.url[0]=0;}
    if(!image_file(v,base,"_sFile100",out.tile))copy(out.tile,sizeof(out.tile),out.url);
    if(!image_file(v,base,"_sFile220",out.thumbnail))copy(out.thumbnail,sizeof(out.thumbnail),out.tile);
    if(!image_file(v,base,"_sFile530",out.card))copy(out.card,sizeof(out.card),out.thumbnail);
    str(v,"_sCaption",out.caption,sizeof(out.caption));
}
// An avatar is a PNG, a JPEG or a GIF (shown as its first frame). Of the two
// listed, the larger one is taken when it is one of those. A member who has
// not set one is given GameBanana's placeholder (static/img/defaults/...),
// which counts as no picture: the page then shows the stock profile glyph.
bool decodable(const char*s){uint32_t n=length(s,url_bytes);if(n<5)return false;const char*e=s+n;
    auto ends=[&](const char*x){uint32_t k=length(x,8);if(k>n)return false;for(uint32_t i=0;i<k;++i){char c=e[-(int)k+(int)i];if(c>='A'&&c<='Z')c=(char)(c-'A'+'a');if(c!=x[i])return false;}return true;};
    for(uint32_t i=0;i+10<=n;++i){const char*x="/defaults/";uint32_t k=0;while(k<10&&s[i+k]==x[k])++k;if(k==10)return false;}
    return ends(".png")||ends(".jpg")||ends(".jpeg")||ends(".gif");}
void avatar(Value v,char*out){
    const char*keys[]={"_sHdAvatarUrl","_sAvatarUrl"};out[0]=0;
    for(const char*key:keys){char candidate[url_bytes];url_text(v,key,candidate,sizeof(candidate));if(image_url(candidate)&&decodable(candidate)){copy(out,url_bytes,candidate);return;}}
}
bool unavailable(Value v){return flag(v,"_bIsPrivate")||flag(v,"_bIsWithheld")||flag(v,"_bIsTrashed");}
bool listing(Value v,Listing&o,bool model){
    const char*submitter_key[]={"_aSubmitter"};if(!unique(v,submitter_key,1))return false;
    clear(&o,sizeof(o));o.source=1;if(unavailable(v)||!uint32(json::member(v,"_idRow"),o.id)||!o.id)return false;
    char m[16];if(model){str(v,"_sModelName",m,sizeof(m));if(!equal(m,"Mod"))return false;}
    uint32_t game=0;if(!uint32(json::member(json::member(v,"_aGame"),"_idRow"),game)||game!=game_id)return false;
    if(!json::text(json::member(v,"_sName"),o.title,sizeof(o.title))||!o.title[0])return false;
    Value author=json::member(v,"_aSubmitter");const char*identity_key[]={"_idRow"};if(author&&*author.begin=='{'&&!unique(author,identity_key,1))return false;
    str(author,"_sName",o.author,sizeof(o.author));Value author_id=json::member(author,"_idRow");if(author_id&&(!uint32(author_id,o.author_id)||!o.author_id))return false;
    avatar(author,o.author_avatar_url);str(v,"_sVersion",o.version,sizeof(o.version));
    Value c=json::member(v,"_aCategory");if(!c)c=json::member(v,"_aRootCategory");str(c,"_sName",o.category,sizeof(o.category));uint32(json::member(c,"_idRow"),o.category_id);
    if(!o.category_id){char url[url_bytes];str(c,"_sProfileUrl",url,sizeof(url));const char*p=url;while(*p)++p;const char*e=p;while(p>url&&p[-1]>='0'&&p[-1]<='9')--p;uint64_t n;if(p<e&&json::number({p,e},n)&&n<=UINT32_MAX)o.category_id=(uint32_t)n;}
    o.created=num(v,"_tsDateAdded");o.updated=num(v,"_tsDateModified");if(!o.updated)o.updated=o.created;o.has_files=flag(v,"_bHasFiles");
    struct Count {const char* key;uint64_t* value;uint32_t bit;};
    const Count counts[]={{"_nLikeCount",&o.likes,Likes},{"_nViewCount",&o.views,Views},{"_nDownloadCount",&o.downloads,Downloads},{"_nPostCount",&o.comments,Comments}};
    for(const auto& count:counts){Value field=json::member(v,count.key);if(field){if(!json::number(field,*count.value))return false;o.statistics|=count.bit;}}
    Writer w(o.profile_url,sizeof(o.profile_url));w.add("https://gamebanana.com/mods/");w.num(o.id);
    Value a=json::member(json::member(v,"_aPreviewMedia"),"_aImages"),im;uint32_t pos=0;if(json::next(a,pos,im))image(im,1,o.image);
    return true;
}
// Descriptions are HTML and the page shows text. Block elements become line
// breaks, list items start with a dash, inline elements and scripts are
// dropped, entities are decoded, and whitespace is collapsed as a browser would.
// Works in place: nothing written is ever longer than what it replaces.
bool tag_named(const char*tag,const char*name){
    uint32_t i=0;for(;name[i];++i){char c=tag[i];if(c>='A'&&c<='Z')c=(char)(c-'A'+'a');if(c!=name[i])return false;}
    const char e=tag[i];return e=='>'||e=='/'||e==' '||e=='\t'||e=='\n'||e=='\r'||!e;
}
void plain(char*s){
    uint32_t r=0,w=0;bool space=false;
    auto breaks=[&](uint32_t count){
        // At least `count` line breaks before what follows, never more than two, none at the start.
        while(w&&s[w-1]==' ')--w;space=false;if(!w)return;
        uint32_t have=0;while(have<w&&s[w-1-have]=='\n')++have;
        for(;have<count;++have)s[w++]='\n';
    };
    auto put=[&](char c){if(space&&w&&s[w-1]!='\n')s[w++]=' ';space=false;s[w++]=c;};
    while(s[r]){
        const char c=s[r];
        if(c=='<'){
            const char*tag=s+r+1;const bool closing=*tag=='/';if(closing)++tag;
            // The content of these is not text for the reader.
            const char*hidden=tag_named(tag,"script")?"</script":tag_named(tag,"style")?"</style":nullptr;
            uint32_t end=r+1;char quote=0;
            while(s[end]){const char t=s[end++];if(quote){if(t==quote)quote=0;}else if(t=='"'||t=='\'')quote=t;else if(t=='>')break;}
            if(hidden&&!closing){
                while(s[end]){if(s[end]=='<'){uint32_t k=0;while(hidden[k]){char t=s[end+k];if(t>='A'&&t<='Z')t=(char)(t-'A'+'a');if(t!=hidden[k])break;++k;}if(!hidden[k])break;}++end;}
            }else {
                static const char*const paragraph[]={"p","h1","h2","h3","h4","h5","h6","blockquote","pre","table","hr"};
                static const char*const line[]={"br","div","ul","ol","tr","li"};
                uint32_t count=0;for(const char*name:paragraph)if(tag_named(tag,name))count=2;
                for(const char*name:line)if(tag_named(tag,name))count=1;
                if(count)breaks(count);
                if(!closing&&tag_named(tag,"li")){s[w++]='-';s[w++]=' ';}
            }
            r=end;continue;
        }
        if(c=='&'){
            struct E{const char*k;const char*v;};
            static const E entities[]={{"amp;","&"},{"lt;","<"},{"gt;",">"},{"quot;","\""},{"apos;","'"},{"nbsp;"," "},{"mdash;","-"},{"ndash;","-"},
                {"hellip;","..."},{"rsquo;","'"},{"lsquo;","'"},{"rdquo;","\""},{"ldquo;","\""},{"bull;","-"},{"copy;","(c)"},{"reg;","(R)"},{"trade;","(TM)"}};
            bool done=false;
            for(const auto&e:entities)if(prefix(s+r+1,e.k)){
                if(e.v[0]==' '&&!e.v[1])space=true;else for(const char*v=e.v;*v;++v)put(*v);
                r+=1+length(e.k,8);done=true;break;
            }
            if(!done&&s[r+1]=='#'){
                uint32_t i=r+2,cp=0,digits=0;const bool hex=s[i]=='x'||s[i]=='X';if(hex)++i;
                for(;digits<7;++i,++digits){const char d=s[i];uint32_t v;if(d>='0'&&d<='9')v=(uint32_t)(d-'0');else if(hex&&d>='a'&&d<='f')v=(uint32_t)(d-'a'+10);else if(hex&&d>='A'&&d<='F')v=(uint32_t)(d-'A'+10);else break;cp=cp*(hex?16:10)+v;}
                if(digits&&s[i]==';'&&cp<=0x10ffff&&!(cp>=0xd800&&cp<=0xdfff)){
                    // A numeric entity is at least four characters; its UTF-8 form is at most four bytes.
                    if(cp==160||cp<=32)space=true;
                    else if(cp<128){if(cp!=127)put((char)cp);}
                    else if(cp<0x800){put((char)(0xc0|(cp>>6)));s[w++]=(char)(0x80|(cp&63));}
                    else if(cp<0x10000){put((char)(0xe0|(cp>>12)));s[w++]=(char)(0x80|((cp>>6)&63));s[w++]=(char)(0x80|(cp&63));}
                    else {put((char)(0xf0|(cp>>18)));s[w++]=(char)(0x80|((cp>>12)&63));s[w++]=(char)(0x80|((cp>>6)&63));s[w++]=(char)(0x80|(cp&63));}
                    r=i+1;done=true;
                }
            }
            if(done)continue;
        }
        ++r;
        if((uint8_t)c<=32)space=true;else put(c);
    }
    while(w&&(s[w-1]=='\n'||s[w-1]==' '))--w;
    s[w]=0;
}
}
bool batch_url(const Query&q,uint32_t page,char*out,uint32_t cap){
    uint32_t search=length(q.search,sizeof(q.search));if(!page||page>1000000||(uint32_t)q.sort>(uint32_t)Sort::Downloads||search>=sizeof(q.search))return false;
    if(search){if(q.category_id||q.author_id||q.sort!=Sort::Latest||!search_text(q.search,search))return false;
        Writer w(out,cap);w.add("https://gamebanana.com/apiv11/Util/Search/Results?_sSearchString=");percent(w,q.search);w.add("&_sModelName=Mod&_idGameRow=18712&_nPerpage=15&_nPage=");w.num(page);return w.ok;
    }
    Writer w(out,cap);w.add("https://gamebanana.com/apiv11/Mod/Index?_nPerpage=24&_aFilters%5BGeneric_Game%5D=18712&_nPage=");w.num(page);
    if(q.category_id){w.add("&_aFilters%5BGeneric_Category%5D=");w.num(q.category_id);}
    if(q.author_id){w.add("&_aFilters%5BGeneric_Submitter%5D=");w.num(q.author_id);}
    if(q.sort==Sort::Popular)w.add("&_sSort=Generic_MostLiked");
    if(q.sort==Sort::Downloads)w.add("&_sSort=Generic_MostDownloaded");return w.ok;
}
// A listing page is parsed in three parts so the caller can spread the records
// over several calls: the document and its metadata, then records, then totals.
bool batch_begin(const char*b,uint32_t n,Batch&o,uint32_t expected_perpage,Value&records){
    if(expected_perpage!=batch_size&&expected_perpage!=search_batch_size)return false;
    Value root;if(!json::document(b,n,root))return false;Value a=json::member(root,"_aRecords"),meta=json::member(root,"_aMetadata");
    uint32_t perpage;if(!a||*a.begin!='['||!uint32(json::member(meta,"_nRecordCount"),o.total)||!uint32(json::member(meta,"_nPerpage"),perpage)||perpage!=expected_perpage)return false;
    o.perpage=perpage;o.count=0;records=a;return true;
}
// 1: every record is parsed. 0: `budget` records were parsed and more remain. -1: invalid.
int batch_records(Value records,uint32_t&pos,Batch&o,uint32_t expected_author,uint32_t budget){
    Value v;
    while(budget--){
        if(!json::next(records,pos,v))return 1;
        if(o.count>=o.perpage)return -1;
        Listing&l=o.listings[o.count];if(!listing(v,l,true)||(expected_author&&l.author_id!=expected_author))return -1;
        for(uint32_t i=0;i<o.count;++i)if(o.listings[i].id==l.id)return -1;
        ++o.count;
    }
    return 0;
}
bool batch_end(Batch&o){if(o.count>o.total)return false;o.has_more=o.count>0&&(uint64_t)o.page*o.perpage<o.total;return true;}
bool parse_batch(const char*b,uint32_t n,Batch&o,uint32_t expected_perpage,uint32_t expected_author){
    Value records;uint32_t pos=0;
    return batch_begin(b,n,o,expected_perpage,records)&&batch_records(records,pos,o,expected_author,UINT32_MAX)==1&&batch_end(o);
}
bool counts_url(const uint32_t*ids,uint32_t count,char*out,uint32_t cap){
    if(!ids||!count||count>batch_size)return false;
    Writer w(out,cap);w.add("https://gamebanana.com/apiv11/Mod/Multi?_csvRowIds=");
    for(uint32_t i=0;i<count;++i){if(!ids[i])return false;if(i)w.add(",");w.num(ids[i]);}
    w.add("&_csvProperties=_idRow,_nDownloadCount");return w.ok;
}
bool parse_counts(const char*b,uint32_t n,Counts&o){
    Value root;if(!json::document(b,n,root)||*root.begin!='[')return false;
    o.count=0;uint32_t pos=0;Value v;
    while(json::next(root,pos,v)){
        if(o.count>=batch_size)return false;
        auto&record=o.records[o.count];if(!uint32(json::member(v,"_idRow"),record.id)||!record.id)return false;
        for(uint32_t i=0;i<o.count;++i)if(o.records[i].id==record.id)return false;
        // A count the provider leaves out is zero, as in a listing.
        record.downloads=0;Value field=json::member(v,"_nDownloadCount");if(field&&!json::number(field,record.downloads))return false;
        ++o.count;
    }
    return true;
}
bool parse_author(const char*b,uint32_t n,AuthorProfile&o){
    Value root;if(!json::document(b,n,root))return false;uint32_t id=0;bool hidden=true;
    const char*critical[]={"_idRow","_bIsPrivate","_bIsWithheld","_bIsTrashed"};if(!unique(root,critical,4))return false;
    if(!o.id||!uint32(json::member(root,"_idRow"),id)||id!=o.id||!json::boolean(json::member(root,"_bIsPrivate"),hidden)||hidden)return false;
    const char* visibility[]={"_bIsWithheld","_bIsTrashed"};for(const char* key:visibility){Value v=json::member(root,key);if(v&&(!json::boolean(v,hidden)||hidden))return false;}
    uint32_t generation=o.generation;clear(&o,sizeof(o));o.generation=generation;o.id=id;
    if(!json::text(json::member(root,"_sName"),o.name,sizeof(o.name))||!o.name[0])return false;plain(o.name);if(!o.name[0])return false;
    str(root,"_sUserTitle",o.title,sizeof(o.title));plain(o.title);avatar(root,o.avatar_url);
    Value joined=json::member(root,"_tsJoinDate"),subscribers=json::member(root,"_nSubscriberCount");if(joined){if(!json::number(joined,o.joined))return false;o.has_joined=true;}if(subscribers){if(!json::number(subscribers,o.subscribers))return false;o.has_subscribers=true;}
    Writer w(o.profile_url,sizeof(o.profile_url));w.add("https://gamebanana.com/members/");w.num(id);return w.ok;
}
bool parse_detail(const char*b,uint32_t n,Detail&o){
    Value root;if(!json::document(b,n,root))return false;uint32_t requested=o.listing.id;if(!listing(root,o.listing,false)||o.listing.id!=requested)return false;
    json::text(json::member(root,"_sText"),o.description,sizeof(o.description),&o.description_truncated);
    // The one-line summary stands in for a missing description; otherwise it is the tagline.
    o.tagline[0]=0;str(root,"_sDescription",o.tagline,sizeof(o.tagline));plain(o.tagline);
    if(!o.description[0]){json::text(json::member(root,"_sDescription"),o.description,sizeof(o.description),&o.description_truncated);o.tagline[0]=0;}
    plain(o.description);
    o.file_count=0;o.preview_count=0;o.files_truncated=false;o.previews_truncated=false;uint32_t pos=0;Value v,a=json::member(root,"_aFiles");
    while(json::next(a,pos,v)){
        if(o.file_count>=max_files){o.files_truncated=true;continue;}
        File&f=o.files[o.file_count];clear(&f,sizeof(f));f.submission_id=o.listing.id;if(!uint32(json::member(v,"_idRow"),f.id)||!f.id)return false;
        for(uint32_t i=0;i<o.file_count;++i)if(o.files[i].id==f.id)return false;
        if(!json::number(json::member(v,"_nFilesize"),f.size)||!json::text(json::member(v,"_sFile"),f.name,sizeof(f.name))||!f.name[0])return false;
        bool cut=false;if(!json::text(json::member(v,"_sDownloadUrl"),f.url,sizeof(f.url),&cut)||cut)return false;
        char expected[64];Writer w(expected,sizeof(expected));w.add("https://gamebanana.com/dl/");w.num(f.id);if(!equal(expected,f.url))return false;
        f.updated=num(v,"_tsDateAdded");str(v,"_sVersion",f.version,sizeof(f.version));str(v,"_sDescription",f.description,sizeof(f.description));plain(f.description);++o.file_count;
    }
    pos=0;a=json::member(json::member(root,"_aPreviewMedia"),"_aImages");uint32_t id=0;
    while(json::next(a,pos,v)){++id;if(o.preview_count>=max_previews){o.previews_truncated=true;continue;}Preview&im=o.previews[o.preview_count];image(v,id,im);if(im.url[0])++o.preview_count;}
    o.listing.has_files=o.file_count!=0;return true;
}
bool parse_categories(const char*b,uint32_t n,Categories&o){
    Value root;if(!json::document(b,n,root))return false;uint32_t game;if(!uint32(json::member(root,"_idRow"),game)||game!=game_id)return false;Value a=json::member(root,"_aModRootCategories"),v;if(!a||*a.begin!='[')return false;
    o.count=0;o.truncated=false;uint32_t pos=0;while(json::next(a,pos,v)){if(o.count==max_categories){o.truncated=true;continue;}Category&c=o.records[o.count];clear(&c,sizeof(c));if(!uint32(json::member(v,"_idRow"),c.id)||!c.id||!json::text(json::member(v,"_sName"),c.name,sizeof(c.name))||!c.name[0])return false;for(uint32_t i=0;i<o.count;++i)if(o.records[i].id==c.id)return false;++o.count;}return true;
}
Provider::Provider(){clear(this,sizeof(*this));}
P4M_Result Provider::initialize(const P4M_HttpsApi*a,const char*owner,void*scratch,uint32_t cap){
    if(api_)return P4M_BUSY;if(!a||a->abi!=P4M_HTTPS_ABI||a->size<sizeof(*a)||!a->get||!a->status||!a->read||!a->cancel||!a->release||!a->capabilities||!scratch||cap<json_bytes||!owner||!owner[0]||length(owner,P4M_ID_BYTES)>=P4M_ID_BYTES)return P4M_INVALID;
    uint32_t flags=0;P4M_Result r=a->capabilities(&flags);if(r!=P4M_OK)return r;const uint32_t required=P4M_HTTPS_ASYNC|P4M_HTTPS_VERIFIED_TLS|P4M_HTTPS_MEMORY_RESULTS;if((flags&required)!=required)return P4M_UNSUPPORTED;
    api_=a;github_.initialize(a,owner_);copy(owner_,sizeof(owner_),owner);scratch_=(char*)scratch;scratch_capacity_=json_bytes;return P4M_OK;
}
void Provider::cancel(P4M_Transfer&h,Operation&o){if(h&&api_){api_->cancel(h);api_->release(h);h=0;}if(o.state==State::Loading){o.state=State::Cancelled;o.error=Error::None;}}
void Provider::cancel_batch(){cancel(batch_handle_,batch_op_);github_.cancel_search();merged_op_.state=State::Cancelled;parsing_=false;}void Provider::cancel_counts(){cancel(counts_handle_,counts_op_);}void Provider::cancel_detail(){cancel(detail_handle_,detail_op_);github_.cancel_detail();}void Provider::cancel_categories(){cancel(categories_handle_,categories_op_);}
void Provider::cancel_author(){cancel(author_handle_,author_op_);github_.cancel_author();}
void Provider::cancel_download(){if(download_.operation.state==State::Loading)cancel_install();}
void Provider::shutdown(){cancel_batch();cancel_detail();cancel_author();cancel_categories();cancel_counts();cancel_download();cancel(download_handle_,download_.operation);api_=nullptr;scratch_=nullptr;scratch_capacity_=0;}
P4M_Result Provider::start(P4M_Transfer&h,Operation&o,const char*url,const char*path,uint64_t max,uint32_t generation){
    cancel(h,o);clear(&o,sizeof(o));o.generation=generation;if(!api_){o.state=State::Failed;o.error=Error::Api;o.api_result=P4M_NOT_READY;return o.api_result;}
    P4M_HttpsRequest req={sizeof(req),(uint32_t)(path?P4M_HTTPS_FILE:P4M_HTTPS_MEMORY),url,path,max,15000,path?3600000u:60000u};o.api_result=api_->get(owner_,&req,&h);
    if(o.api_result!=P4M_OK){h=0;o.state=State::Failed;o.error=Error::Api;}else o.state=State::Loading;return o.api_result;
}
P4M_Result Provider::begin_batch(const Query&q,uint32_t page,uint32_t generation){
 char url[2048]{};if(q.author_id<0x80000000&&!batch_url(q,page,url,sizeof url))return TSUCHI_INVALID;
 cancel_batch();batch_={};batch_.page=page;batch_.generation=generation;merged_op_={};merged_op_.generation=generation;merged_op_.state=State::Loading;merged_sort_=q.sort;
 batch_perpage_=q.search[0]?search_batch_size:batch_size;batch_author_=q.author_id;
 gamebanana_batch_=q.author_id<0x80000000;
 github_batch_=!q.category_id&&(!q.author_id||q.author_id>=0x80000000);
 TSUCHI_Result gh=github_batch_?github_.search(q,page,generation):TSUCHI_UNSUPPORTED,gb=TSUCHI_UNSUPPORTED;
 if(q.author_id>=0x80000000){batch_op_={};batch_op_.generation=generation;batch_op_.state=State::Ready;}
 else{gb=start(batch_handle_,batch_op_,url,nullptr,json_bytes,generation);}
 if(gb!=TSUCHI_OK&&gh!=TSUCHI_OK){merged_op_.state=State::Failed;merged_op_.error=Error::Api;return gb;}
 return TSUCHI_OK;
}
P4M_Result Provider::begin_detail(uint32_t id,uint32_t generation){cancel_detail();github_detail_=id>=0x80000000;if(github_detail_)return github_.detail(id,generation);if(!id)return P4M_INVALID;char url[96];Writer w(url,sizeof(url));w.add("https://gamebanana.com/apiv11/Mod/");w.num(id);w.add("/ProfilePage");cancel_detail();clear(&detail_,sizeof(detail_));detail_.generation=generation;detail_.listing.id=id;return start(detail_handle_,detail_op_,url,nullptr,json_bytes,generation);}
P4M_Result Provider::begin_author(uint32_t id,uint32_t generation){cancel_author();github_author_=id>=0x80000000;if(github_author_)return github_.author(id,generation);if(!id)return P4M_INVALID;char url[96];Writer w(url,sizeof(url));w.add("https://gamebanana.com/apiv11/Member/");w.num(id);w.add("/ProfilePage");cancel_author();clear(&author_,sizeof(author_));author_.generation=generation;author_.id=id;return start(author_handle_,author_op_,url,nullptr,json_bytes,generation);}
P4M_Result Provider::begin_categories(){cancel_categories();clear(&categories_,sizeof(categories_));return start(categories_handle_,categories_op_,"https://gamebanana.com/apiv11/Game/18712/ProfilePage",nullptr,json_bytes,0);}
P4M_Result Provider::begin_counts(const uint32_t*ids,uint32_t count,uint32_t generation){
    char url[512];if(!counts_url(ids,count,url,sizeof(url)))return P4M_INVALID;
    cancel_counts();clear(&counts_,sizeof(counts_));counts_.generation=generation;
    return start(counts_handle_,counts_op_,url,nullptr,json_bytes,generation);
}
P4M_Result Provider::begin_download(const File& f,const char*,uint64_t,const char* replacing){
 if(download_.operation.state==State::Loading)return P4M_BUSY;
 const auto result=begin_install(detail_.listing,f,replacing);if(result!=P4M_OK)return result;
 download_={};download_.submission_id=f.submission_id;download_.file_id=f.id;download_.operation.state=State::Loading;return result;
}
void Provider::poll(P4M_Transfer&h,Operation&o,uint32_t kind){
    if(!h||o.state!=State::Loading||!api_)return;P4M_HttpsStatus st={};st.size=sizeof(st);P4M_Result r=api_->status(h,&st);o.api_result=r;
    if(r!=P4M_OK){o.state=State::Failed;o.error=Error::Api;api_->release(h);h=0;return;}o.transfer=st;
    if(st.state==P4M_HTTPS_QUEUED||st.state==P4M_HTTPS_RUNNING)return;
    if(st.state==P4M_HTTPS_CANCELLED){o.state=State::Cancelled;}else if(st.state!=P4M_HTTPS_SUCCEEDED){o.state=State::Failed;o.error=Error::Transport;}
    else if(kind==3){o.state=State::Ready;}
    else if(!st.received||st.received>scratch_capacity_){o.state=State::Failed;o.error=Error::Limit;}
    else {
        uint32_t got=0;r=api_->read(h,0,scratch_,(uint32_t)st.received,&got);o.api_result=r;
        if(r!=P4M_OK||got!=st.received){o.state=State::Failed;o.error=Error::Api;}
        else if(kind==0){
            // Stays Loading: pump() parses the records a few per call.
            Value records;
            if(batch_begin(scratch_,got,batch_,batch_perpage_,records)){parse_begin_=records.begin;parse_end_=records.end;parse_pos_=0;parsing_=true;}
            else {o.state=State::Failed;o.error=Error::Schema;}
        }
        else {bool ok=kind==1?parse_detail(scratch_,got,detail_):kind==4?parse_author(scratch_,got,author_):kind==5?parse_counts(scratch_,got,counts_):parse_categories(scratch_,got,categories_);if(kind==4)clear(scratch_,got);o.state=ok?State::Ready:State::Failed;o.error=ok?Error::None:Error::Schema;}
    }
    api_->release(h);h=0;
}
void Provider::pump(){
    github_.pump();
    if(github_detail_&&github_.detail_operation().state==State::Ready)detail_=github_.mod();
    if(github_author_&&github_.author_operation().state==State::Ready)author_=github_.profile();
    if(!parsing_)poll(batch_handle_,batch_op_,0);
    if(parsing_){
        const int done=batch_records({parse_begin_,parse_end_},parse_pos_,batch_,batch_author_,parse_records_per_pump);
        if(done){
            parsing_=false;const bool ok=done>0&&batch_end(batch_);
            if(batch_op_.state==State::Loading){batch_op_.state=ok?State::Ready:State::Failed;batch_op_.error=ok?Error::None:Error::Schema;}
        }
    }else {
        // These read their answer into the buffer a listing is being parsed from.
        poll(detail_handle_,detail_op_,1);poll(author_handle_,author_op_,4);poll(categories_handle_,categories_op_,2);poll(counts_handle_,counts_op_,5);
    }
    if(download_.file_id)install_download_status(download_);
    merge_sources();
}
void Provider::merge_sources(){
 if(merged_op_.state!=State::Loading||batch_op_.state==State::Loading)return;
 const auto& gh=github_.search_operation();if(github_batch_&&gh.state==State::Loading)return;
 const bool gb_ok=gamebanana_batch_&&batch_op_.state==State::Ready,gh_ok=github_batch_&&gh.state==State::Ready;
 if(!gb_ok&&!gh_ok){merged_op_.state=State::Failed;merged_op_.error=Error::Transport;return;}
 if(!gb_ok){batch_.count=0;batch_.total=0;batch_.has_more=false;if(gamebanana_batch_)batch_.failed_sources|=2;}
 if(github_batch_&&!gh_ok)batch_.failed_sources|=1;
 if(gh_ok){const auto& incoming=github_.batch();for(uint32_t i=0;i<incoming.count&&batch_.count<batch_size+6;++i)batch_.listings[batch_.count++]=incoming.listings[i];
  batch_.has_more=batch_.has_more||incoming.has_more;batch_.total+=incoming.total;}
 // Each fetch retains a bounded mixed batch and stable source-qualified IDs.
 // The existing continuous list owns focus, duplicate suppression and paging.
 for(uint32_t i=1;i<batch_.count;++i){auto item=batch_.listings[i];uint32_t at=i;
  auto score=[&](const Listing& l){return merged_sort_==Sort::Latest?l.updated:merged_sort_==Sort::Downloads?l.downloads:l.likes;};
  while(at&&score(item)>score(batch_.listings[at-1])){batch_.listings[at]=batch_.listings[at-1];--at;}batch_.listings[at]=item;}
 merged_op_.state=State::Ready;merged_op_.error=Error::None;
}
} }
