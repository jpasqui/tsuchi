#include "mod_zip.h"
namespace tsuchi::mod_zip {
namespace {
uint16_t u16(const uint8_t*p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
uint32_t u32(const uint8_t*p){return uint32_t(u16(p))|(uint32_t(u16(p+2))<<16);}
void zero(void*p,uint32_t n){auto*b=(uint8_t*)p;while(n--)*b++=0;}
void copy(char*d,const char*s,uint32_t cap){uint32_t n=0;while(s[n]&&n+1<cap){d[n]=s[n];++n;}d[n]=0;}
bool equal(const char*a,const char*b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
char lower(char c){return c>='A'&&c<='Z'?char(c+32):c;}
bool alias(const char*a,const char*b){while(*a&&lower(*a)==lower(*b)){++a;++b;}return *a==*b;}
uint32_t len(const char*s){uint32_t n=0;while(s[n])++n;return n;}
bool reserved(const char*s,uint32_t n){uint32_t k=0;while(k<n&&s[k]!='.')++k;if(k==3){char a=lower(s[0]),b=lower(s[1]),c=lower(s[2]);if((a=='c'&&b=='o'&&c=='n')||(a=='p'&&b=='r'&&c=='n')||(a=='a'&&b=='u'&&c=='x')||(a=='n'&&b=='u'&&c=='l'))return true;}if(k==4&&s[3]>='1'&&s[3]<='9'){char a=lower(s[0]),b=lower(s[1]),c=lower(s[2]);if((a=='c'&&b=='o'&&c=='m')||(a=='l'&&b=='p'&&c=='t'))return true;}return false;}
bool begins(const char*a,const char*b){while(*b)if(*a++!=*b++)return false;return true;}
bool hex_id(const char* s,uint32_t n,uint64_t& value){
    if(n!=16)return false;value=0;
    for(uint32_t i=0;i<n;++i){char c=lower(s[i]);uint32_t d=c>='0'&&c<='9'?uint32_t(c-'0'):c>='a'&&c<='f'?uint32_t(c-'a'+10):16;if(d==16)return false;value=(value<<4)|d;}return true;
}
struct Entry {char name[max_name],base[max_name];uint64_t local,data,end;uint32_t compressed,expanded,crc;uint16_t method,flags;bool directory;};
struct Huff {uint16_t count[16],first[16],index[16],symbols[288];uint16_t total;};
struct Work {Entry entries[max_entries];uint8_t input[65536],window[32768],output[65536];uint32_t crc_table[256];Huff lit,dist,codes;uint8_t lengths[320];};
struct Context {
    const Platform&p;const Cancellation&cancel;Status&s;void(*progress)(void*);void*user;Work*w;const Request&request;void*input=nullptr,*stage=nullptr,*output=nullptr;uint64_t input_size=0;Error error=Error::None;uint32_t result=0;
    bool stop(){if(cancel.load()){error=Error::Cancelled;return true;}return false;}
    bool fail(Error e,uint32_t r=0){error=e;result=r;return false;}
    bool read(uint64_t off,void*out,uint32_t n){if(stop())return false;if(off>input_size||n>input_size-off)return fail(Error::Zip);uint32_t got=0;uint32_t r=p.input_read(p.user,input,off,out,n,&got);return !r&&got==n?true:fail(Error::Storage,r);}
    void update(){if(progress)progress(user);}
};
bool extras(Context&c,uint64_t off,uint32_t bytes){while(bytes){uint8_t h[4];if(bytes<4)return c.fail(Error::Zip);if(!c.read(off,h,4))return false;uint16_t id=u16(h),size=u16(h+2);if(size>bytes-4)return c.fail(Error::Zip);if(id==1)return c.fail(Error::Unsupported);off+=4+size;bytes-=4+size;}return true;}
bool path(Context&c,Entry&e,char*wrapper,bool&plain_seen){
    uint32_t n=len(e.name);if(!n||n>=max_name||e.name[0]=='/')return c.fail(Error::Path);
    uint32_t start=0;for(uint32_t i=0;i<=n;++i){char ch=e.name[i];if(ch=='/'||!ch){uint32_t size=i-start;if(!size){if(i!=n||!e.directory)return c.fail(Error::Path);}else if(e.name[start]=='.'||e.name[i-1]=='.'||e.name[i-1]==' '||reserved(e.name+start,size)||(size==5&&lower(e.name[start])=='e'&&lower(e.name[start+1])=='x'&&lower(e.name[start+2])=='e'&&lower(e.name[start+3])=='f'&&lower(e.name[start+4])=='s'))return c.fail(Error::Path);start=i+1;}else if((uint8_t)ch<32||(uint8_t)ch>=127||ch=='\\'||ch==':'||ch=='"'||ch=='<'||ch=='>'||ch=='|'||ch=='?'||ch=='*')return c.fail(Error::Path);}
    (void)wrapper; (void)plain_seen;
    const char* rel=e.name;
    constexpr const char* root="romfs/tsuchi/mods/";
    if(!begins(rel,"romfs/")) {
        const char* slash=rel;while(*slash&&*slash!='/')++slash;
        if(*slash)rel=slash+1;
    }
    if(e.directory) {
        if(!*rel)return true;
        const char* a=rel,*r=root;while(*a&&*a==*r){++a;++r;}
        if(!*a)return true;
    }
    if(!begins(rel,root)) {
        const char* base=e.name;for(const char* p=e.name;*p;++p)if(*p=='/')base=p+1;
        if(!e.directory&&(equal(base,"README.md")||equal(base,"README.txt")||equal(base,"LICENSE")||equal(base,"LICENSE.txt")))return true;
        if(!c.request.derived_id[0])return c.fail(Error::Path);
        if(e.directory)return true;
        uint64_t build=0;
        if(len(base)==20&&alias(base+16,".txt")&&hex_id(base,16,build)){
            // A title-qualified package must name this game. The release wrapper
            // and cheats folder may vary, but title IDs cannot silently be stripped.
            const char* part=e.name;
            for(const char* p=e.name;p<base;++p)if(*p=='/'){uint64_t title=0;if(hex_id(part,uint32_t(p-part),title)&&title!=c.request.program_id)return c.fail(Error::Unsupported);part=p+1;}
            if(!e.expanded||e.expanded>=64*1024)return c.fail(Error::Limit);
            uint32_t at=len(c.request.derived_id);if(!at||at>=48)return c.fail(Error::Path);
            copy(e.base,c.request.derived_id,sizeof e.base);copy(e.base+at,"/cheats/",sizeof(e.base)-at);at+=len("/cheats/");
            for(uint32_t i=0;i<16;++i){char ch=base[i];e.base[at+i]=ch>='a'&&ch<='f'?char(ch-32):ch;}
            copy(e.base+at+16,".txt",sizeof(e.base)-at-16);c.s.cheats=true;return true;
        }
        // Only complete, unambiguous cooked triples are admitted. Never discard
        // another payload (scripts, cheats, executable patches) from a mixed ZIP.
        const char* dot=nullptr;for(const char* p=base;*p;++p)if(*p=='.')dot=p;
        if(!dot||!(equal(dot,".pak")||equal(dot,".utoc")||equal(dot,".ucas")))return c.fail(Error::Unsupported);
        const uint32_t stem=uint32_t(dot-base);if(!stem||stem>160)return c.fail(Error::Path);
        for(uint32_t i=0;i<stem;++i){char ch=base[i];if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='.'||ch=='_'||ch=='-'))return c.fail(Error::Path);}
        // The mod's ID comes from the request alone, never from the container's file name: a renamed
        // container is the same mod, and its update replaces it.
        uint32_t at=len(c.request.derived_id);if(!at||at>=48)return c.fail(Error::Path);
        copy(e.base,c.request.derived_id,sizeof e.base);
        e.base[at++]='/';copy(e.base+at,base,sizeof(e.base)-at);c.s.assets=true;return true;
    }
    rel+=len(root);
    if(!*rel)return e.directory?true:c.fail(Error::Path);
    const char* slash=rel;while(*slash&&*slash!='/')++slash;
    const uint32_t id_size=uint32_t(slash-rel);
    if(!id_size||id_size>=48||!*slash)return c.fail(Error::Path);
    for(uint32_t i=0;i<id_size;++i)if(!((rel[i]>='a'&&rel[i]<='z')||(rel[i]>='0'&&rel[i]<='9')||rel[i]=='.'||rel[i]=='_'||rel[i]=='-'))return c.fail(Error::Path);
    if((id_size==6&&!__builtin_memcmp(rel,"tsuchi",6))||begins(rel,"tsuchi."))return c.fail(Error::Path);
    if(!e.directory&&!slash[1])return c.fail(Error::Path);
    copy(e.base,rel,sizeof(e.base));return true;
}
bool validate(Context&c,uint32_t&count){
    if(c.input_size<22||c.input_size>max_archive)return c.fail(Error::Limit);uint8_t tail[22];if(!c.read(c.input_size-22,tail,22))return false;
    if(u32(tail)!=0x06054b50||u16(tail+4)||u16(tail+6)||u16(tail+8)!=u16(tail+10)||u16(tail+20))return c.fail(Error::Unsupported);
    count=u16(tail+10);if(!count||count>max_entries||count==65535)return c.fail(Error::Limit);
    uint64_t central=u32(tail+16),size=u32(tail+12),end=c.input_size-22;if(central+size!=end)return c.fail(Error::Zip);
    char wrapper[256]={};bool plain_seen=false;uint64_t cursor=central;uint32_t files=0;c.s.expanded_total=0;
    for(uint32_t i=0;i<count;++i){
        if(c.stop())return false;Entry&e=c.w->entries[i];zero(&e,sizeof(e));uint8_t h[46];if(!c.read(cursor,h,sizeof(h)))return false;
        if(u32(h)!=0x02014b50)return c.fail(Error::Zip);uint16_t name=u16(h+28),extra=u16(h+30),comment=u16(h+32),version=u16(h+6);
        if(!name||name>=max_name||cursor+46+name+extra+comment>end||u16(h+34)||version>20)return c.fail(Error::Unsupported);
        e.flags=u16(h+8);e.method=u16(h+10);e.crc=u32(h+16);e.compressed=u32(h+20);e.expanded=u32(h+24);e.local=u32(h+42);
        if((e.flags&~uint16_t(0x080e))||(e.method!=0&&e.method!=8)||(e.method==0&&(e.flags&6)))return c.fail(Error::Unsupported);
        if(!c.read(cursor+46,e.name,name))return false;e.name[name]=0;for(uint32_t k=0;k<name;++k)if(!e.name[k])return c.fail(Error::Path);e.directory=e.name[name-1]=='/';
        uint32_t mode=u32(h+38)>>16,type=mode&0170000;if((type&&type!=(e.directory?0040000u:0100000u))||(!e.directory&&(u32(h+38)&0x10))||(u32(h+38)&0x400))return c.fail(Error::Path);
        if(!extras(c,cursor+46+name,extra)||!path(c,e,wrapper,plain_seen))return false;
        if(e.directory){if(e.compressed||e.expanded||e.crc)return c.fail(Error::Zip);}else{
            if(e.expanded>max_expanded||e.expanded>uint64_t(e.compressed)*200+1048576)return c.fail(Error::Limit);
            if(c.s.expanded_total>max_expanded-e.expanded)return c.fail(Error::Limit);c.s.expanded_total+=e.expanded;if(e.base[0])++files;
        }
        for(uint32_t j=0;j<i;++j){if(alias(e.name,c.w->entries[j].name)||(e.base[0]&&c.w->entries[j].base[0]&&alias(e.base,c.w->entries[j].base)))return c.fail(Error::Path);}
        cursor+=46+name+extra+comment;
    }
    if(cursor!=end||!files)return c.fail(Error::Zip);
    if(c.s.cheats){
        if(c.s.assets)return c.fail(Error::Unsupported);
        char prefix[64]{};copy(prefix,c.request.derived_id,sizeof prefix);copy(prefix+len(prefix),"/cheats/",sizeof(prefix)-len(prefix));
        for(uint32_t i=0;i<count;++i){const Entry& e=c.w->entries[i];if(!e.directory&&e.base[0]&&!begins(e.base,prefix))return c.fail(Error::Unsupported);}
    }
    if(c.s.assets){
        // One container: a mod mounts one, and the package's one mod ID cannot name two.
        uint32_t paks=0;
        for(uint32_t i=0;i<count;++i){const Entry& e=c.w->entries[i];const uint32_t n=len(e.base);if(!e.directory&&n>4&&equal(e.base+n-4,".pak")&&++paks>1)return c.fail(Error::Unsupported);}
        for(uint32_t i=0;i<count;++i){const Entry& e=c.w->entries[i];if(e.directory||!e.base[0])continue;
            if(!begins(e.base,c.request.derived_id)||e.base[len(c.request.derived_id)]!='/')return c.fail(Error::Unsupported);
            const char* dot=nullptr;for(const char* p=e.base;*p;++p)if(*p=='.')dot=p;
            if(!dot)return c.fail(Error::Unsupported);uint32_t stem=uint32_t(dot-e.base),mask=0;
            for(uint32_t j=0;j<count;++j){const Entry& other=c.w->entries[j];if(other.directory||len(other.base)<=stem||__builtin_memcmp(other.base,e.base,stem)||other.base[stem]!='.')continue;
                const char* ext=other.base+stem;mask|=equal(ext,".pak")?1:equal(ext,".utoc")?2:equal(ext,".ucas")?4:0;}
            if(mask!=7)return c.fail(Error::Unsupported);
        }
    }
    for(uint32_t i=0;i<count;++i){
        Entry&e=c.w->entries[i];uint8_t h[30];if(e.local>=central)return c.fail(Error::Zip);if(!c.read(e.local,h,30))return false;
        if(u32(h)!=0x04034b50||u16(h+4)>20||u16(h+6)!=e.flags||u16(h+8)!=e.method)return c.fail(Error::Zip);
        uint16_t name=u16(h+26),extra=u16(h+28);if(name!=len(e.name)||name>=max_name)return c.fail(Error::Zip);char local[max_name];if(!c.read(e.local+30,local,name))return false;for(uint32_t k=0;k<name;++k)if(local[k]!=e.name[k])return c.fail(Error::Zip);
        if(!extras(c,e.local+30+name,extra))return false;e.data=e.local+30+name+extra;e.end=e.data+e.compressed;if(e.end>central)return c.fail(Error::Zip);
        if(e.flags&8){uint32_t crc=u32(h+14),compressed=u32(h+18),expanded=u32(h+22);if((crc&&crc!=e.crc)||(compressed&&compressed!=e.compressed)||(expanded&&expanded!=e.expanded))return c.fail(Error::Zip);uint8_t d[16];if(e.end+12>central||!c.read(e.end,d,12))return false;uint32_t offset=0;if(u32(d)==0x08074b50){if(e.end+16>central||!c.read(e.end,d,16))return false;offset=4;}if(u32(d+offset)!=e.crc||u32(d+offset+4)!=e.compressed||u32(d+offset+8)!=e.expanded)return c.fail(Error::Zip);e.end+=12+offset;}
        else if(u32(h+14)!=e.crc||u32(h+18)!=e.compressed||u32(h+22)!=e.expanded)return c.fail(Error::Zip);
    }
    // Every local region is exactly covered once: no overlaps, prefixes, gaps or
    // hidden executable/trailing payload before the central directory.
    uint64_t covered=0;for(uint32_t i=0;i<count;++i){uint32_t next=count;for(uint32_t j=0;j<count;++j)if(c.w->entries[j].local==covered){if(next!=count)return c.fail(Error::Zip);next=j;}if(next==count)return c.fail(Error::Zip);covered=c.w->entries[next].end;}
    if(covered!=central)return c.fail(Error::Zip);c.s.entry_count=files;c.s.compressed_total=c.input_size;c.update();return true;
}
bool build(Huff&h,const uint8_t*l,uint32_t n,bool distance=false,bool code_tree=false){
    zero(&h,sizeof(h));for(uint32_t i=0;i<n;++i){if(l[i]>15)return false;if(l[i])++h.count[l[i]];}
    uint32_t left=1,code=0,index=0;for(uint32_t bits=1;bits<=15;++bits){left*=2;if(left<h.count[bits])return false;left-=h.count[bits];code=(code+h.count[bits-1])*2;h.first[bits]=(uint16_t)code;h.index[bits]=(uint16_t)index;index+=h.count[bits];}
    h.total=(uint16_t)index;if(!index)return distance&&!code_tree;if(left&&(code_tree||index!=1||h.count[1]!=1))return false;
    uint16_t offsets[16];for(uint32_t i=0;i<16;++i)offsets[i]=h.index[i];for(uint32_t i=0;i<n;++i)if(l[i])h.symbols[offsets[l[i]]++]=(uint16_t)i;return true;
}
struct Bits {
    Context&c;const Entry&e;uint32_t position=0,buffer_pos=0,buffer_count=0;uint64_t bits=0;uint32_t bit_count=0;
    bool byte(uint32_t&v){if(position>=e.compressed)return c.fail(Error::Zip);if(buffer_pos==buffer_count){buffer_count=e.compressed-position;if(buffer_count>65536)buffer_count=65536;if(!c.read(e.data+position,c.w->input,buffer_count))return false;buffer_pos=0;}v=c.w->input[buffer_pos++];++position;return true;}
    bool get(uint32_t n,uint32_t&v){while(bit_count<n){uint32_t b;if(!byte(b))return false;bits|=uint64_t(b)<<bit_count;bit_count+=8;}v=uint32_t(bits)&((1u<<n)-1);bits>>=n;bit_count-=n;return true;}
    void align(){bits=0;bit_count=0;}
    bool symbol(const Huff&h,uint32_t&out){uint32_t code=0;for(uint32_t n=1;n<=15;++n){uint32_t b;if(!get(1,b))return false;code=(code<<1)|b;if(code>=h.first[n]&&code-h.first[n]<h.count[n]){out=h.symbols[h.index[n]+code-h.first[n]];return true;}}return c.fail(Error::Zip);}
};
struct Sink {
    Context&c;const Entry&e;uint32_t count=0,buffered=0,crc=0xffffffff;
    bool flush(){if(c.stop())return false;if(buffered){uint32_t r=c.p.output_write(c.p.user,c.output,count-buffered,c.w->output,buffered);if(r)return c.fail(Error::Storage,r);c.s.expanded_done+=buffered;buffered=0;c.update();}return true;}
    bool put(uint8_t b){if(count>=e.expanded)return c.fail(Error::Zip);c.w->window[count&32767]=b;c.w->output[buffered++]=b;crc=c.w->crc_table[(crc^b)&255]^(crc>>8);++count;if(buffered==65536)return flush();return true;}
    bool match(uint32_t distance,uint32_t length){if(!distance||distance>32768||distance>count||length>e.expanded-count)return c.fail(Error::Zip);while(length--)if(!put(c.w->window[(count-distance)&32767]))return false;return true;}
};
bool dynamic(Bits&b){
    uint32_t x,hlit,hdist,hclen;if(!b.get(5,x))return false;hlit=x+257;if(hlit>286)return b.c.fail(Error::Zip);if(!b.get(5,x))return false;hdist=x+1;if(!b.get(4,x))return false;hclen=x+4;
    static const uint8_t order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint8_t lengths[19]={};for(uint32_t i=0;i<hclen;++i){if(!b.get(3,x))return false;lengths[order[i]]=(uint8_t)x;}
    if(!build(b.c.w->codes,lengths,19,false,true))return b.c.fail(Error::Zip);uint32_t n=0,total=hlit+hdist;auto*l=b.c.w->lengths;
    while(n<total){if(!b.symbol(b.c.w->codes,x))return false;if(x<=15)l[n++]=(uint8_t)x;else{uint32_t repeat,value=0;if(x==16){if(!n||!b.get(2,repeat))return b.c.fail(Error::Zip);repeat+=3;value=l[n-1];}else if(x==17){if(!b.get(3,repeat))return false;repeat+=3;}else if(x==18){if(!b.get(7,repeat))return false;repeat+=11;}else return b.c.fail(Error::Zip);if(repeat>total-n)return b.c.fail(Error::Zip);while(repeat--)l[n++]=(uint8_t)value;}}
    if(!l[256]||!build(b.c.w->lit,l,hlit)||!build(b.c.w->dist,l+hlit,hdist,true))return b.c.fail(Error::Zip);return true;
}
bool compressed(Bits&b,Sink&s){
    static const uint16_t lengths[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t lextra[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t distances[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t dextra[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for(;;){uint32_t sym,x;if(!b.symbol(b.c.w->lit,sym))return false;if(sym<256){if(!s.put((uint8_t)sym))return false;}else if(sym==256)return true;else{if(sym<257||sym>285)return b.c.fail(Error::Zip);uint32_t k=sym-257;if(!b.get(lextra[k],x))return false;uint32_t length=lengths[k]+x;if(!b.symbol(b.c.w->dist,sym)||sym>=30)return b.c.fail(Error::Zip);if(!b.get(dextra[sym],x))return false;if(!s.match(distances[sym]+x,length))return false;}}
}
bool extract(Context&c,const Entry&e){
    Sink sink{c,e};if(e.method==0){if(e.compressed!=e.expanded)return c.fail(Error::Zip);uint32_t pos=0;while(pos<e.compressed){uint32_t n=e.compressed-pos;if(n>65536)n=65536;if(!c.read(e.data+pos,c.w->input,n))return false;for(uint32_t i=0;i<n;++i)if(!sink.put(c.w->input[i]))return false;pos+=n;}}
    else {Bits b{c,e};bool done=false;while(!done){uint32_t last,type;if(!b.get(1,last)||!b.get(2,type))return false;done=last!=0;if(type==0){b.align();uint32_t lo,hi,nlo,nhi;if(!b.byte(lo)||!b.byte(hi)||!b.byte(nlo)||!b.byte(nhi))return false;uint32_t n=lo|(hi<<8);if((n^uint32_t(nlo|(nhi<<8)))!=65535)return c.fail(Error::Zip);while(n--){uint32_t v;if(!b.byte(v)||!sink.put((uint8_t)v))return false;}}
        else if(type==1){uint8_t l[288],d[32];for(uint32_t i=0;i<288;++i)l[i]=i<144?8:i<256?9:i<280?7:8;for(auto&v:d)v=5;if(!build(c.w->lit,l,288)||!build(c.w->dist,d,32,true)||!compressed(b,sink))return false;}
        else if(type==2){if(!dynamic(b)||!compressed(b,sink))return false;}else return c.fail(Error::Zip);
    }if(b.position!=e.compressed)return c.fail(Error::Zip);}
    if(sink.count!=e.expanded)return c.fail(Error::Zip);if((sink.crc^0xffffffff)!=e.crc)return c.fail(Error::Crc);if(!sink.flush())return false;return true;
}
}
uint64_t workspace_bytes(){return sizeof(Work);}
Outcome extract_zip(const Platform&p,const Request&r,const Cancellation&cancel,Status&s,void(*progress)(void*),void*user){
    if(cancel.load())return {Error::Cancelled,0,false};auto*w=(Work*)p.allocate(sizeof(Work));if(!w)return {Error::Limit,0,false};zero(w,sizeof(*w));Context c{p,cancel,s,progress,user,w,r};bool committed=false;
    uint32_t result=p.input_open(p.user,r.cache_key,&c.input,&c.input_size);if(result){c.fail(Error::Storage,result);}else{
        uint32_t count=0;if(validate(c,count)&&!c.stop()){
            result=p.stage_begin(p.user,s.group,&c.stage);if(result)c.fail((result&0x3fffff)==0x402?Error::Exists:Error::Storage,result);else{
                for(uint32_t i=0;i<256;++i){uint32_t v=i;for(uint32_t j=0;j<8;++j)v=(v>>1)^(0xedb88320u&uint32_t(-int32_t(v&1)));w->crc_table[i]=v;}
                s.state=State::Extracting;c.update();bool ok=true;uint64_t expanded=0;
                for(uint32_t i=0;i<count&&ok;++i){const Entry&e=w->entries[i];if(e.directory||!e.base[0])continue;if(c.stop()){ok=false;break;}copy(s.current_file,e.base,sizeof(s.current_file));c.update();result=p.output_open(p.user,c.stage,e.base,e.expanded,&c.output);if(result){ok=c.fail(Error::Storage,result);break;}
                    ok=extract(c,e);uint32_t finish=p.output_finish(p.user,c.output);c.output=nullptr;if(finish&&ok)ok=c.fail(Error::Storage,finish);if(ok){expanded+=e.expanded;s.expanded_done=expanded;s.compressed_done=e.end;++s.completed_files;c.update();}
                }
                if(ok&&!c.stop()){s.state=State::Committing;c.update();if(!c.stop()){result=p.commit(p.user,c.stage);if(result)c.fail(result==limit_result?Error::Limit:result==conflict_result?Error::Conflict:(result&0x3fffff)==0x402?Error::Exists:Error::Storage,result);else{committed=true;c.stage=nullptr;s.compressed_done=s.compressed_total;s.expanded_done=s.expanded_total;}}}
            }
        }
    }
    if(c.output)p.output_finish(p.user,c.output);
    if(c.stage){uint32_t cleanup=p.abort(p.user,c.stage);if(cleanup&&c.error==Error::None){c.error=Error::Storage;c.result=cleanup;}}
    if(c.input)p.input_close(p.user,c.input);
    if(committed&&p.input_remove)p.input_remove(p.user,r.cache_key);
    p.free(w);return {committed?Error::None:c.error==Error::None?Error::Zip:c.error,c.result,committed};
}
}
