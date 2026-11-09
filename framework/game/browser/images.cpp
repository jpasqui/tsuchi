#include "images.h"
namespace tsuchi { namespace browser {
namespace {
uint32_t be32(const uint8_t* p) {return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
uint32_t be16(const uint8_t* p) {return uint32_t(p[0])<<8|p[1];}
// Previews are decoded whole before they are reduced, so the bound is the
// decoder's transient buffer: 4 MP is 16 MiB of RGBA and covers 2560x1440.
bool dimensions(uint32_t w,uint32_t h,ImageKind kind) {
    if(!w||!h)return false;
    if(kind==ImageKind::Thumbnail)return w<=512&&h<=512;
    if(kind==ImageKind::Card)return w<=1280&&h<=1024;
    return w<=2560&&h<=2560&&w*h<=(4u<<20);
}
bool same(const char* a,const char* b) {while(*a&&*a==*b){++a;++b;}return *a==*b;}
bool copy(char* out,uint32_t cap,const char* in) {
    if(!in)return false;
    uint32_t n=0;for(;n+1<cap&&in[n];++n)out[n]=in[n];
    out[n]=0;return n&&in[n]==0;
}
bool https_url(const char* s) {
    if(!s)return false;
    const char* p="https://";for(uint32_t i=0;i<8;++i)if(s[i]!=p[i])return false;
    uint32_t i=8;bool host=false;
    for(;i<P4M_HTTPS_URL_BYTES&&s[i]&&s[i]!='/'&&s[i]!='?';++i){
        const uint8_t c=uint8_t(s[i]);if(c<=32||c>=127||c=='@'||c=='#'||c=='\\')return false;host=true;
    }
    if(!host)return false;
    for(;i<P4M_HTTPS_URL_BYTES&&s[i];++i){const uint8_t c=uint8_t(s[i]);if(c<=32||c>=127||c=='#'||c=='\\')return false;}
    return i<P4M_HTTPS_URL_BYTES;
}
// Table-driven: a screenshot is megabytes and this runs on the game thread.
uint32_t crc(const uint8_t* p,uint32_t n) {
    static uint32_t table[256];static bool built=false;
    if(!built){
        for(uint32_t i=0;i<256;++i){uint32_t v=i;for(uint32_t j=0;j<8;++j)v=(v>>1)^(0xedb88320u&uint32_t(-int32_t(v&1)));table[i]=v;}
        built=true;
    }
    uint32_t v=~0u;
    for(uint32_t i=0;i<n;++i)v=table[(v^p[i])&255]^(v>>8);
    return ~v;
}
}
namespace {
struct GifReader {
    const uint8_t* p;uint32_t n,at;
    bool byte(uint8_t& out){if(at>=n)return false;out=p[at++];return true;}
    bool word(uint32_t& out){if(n-at<2)return false;out=uint32_t(p[at])|uint32_t(p[at+1])<<8;at+=2;return true;}
    bool skip(uint32_t count){if(n-at<count)return false;at+=count;return true;}
    // The rest of a run of data sub-blocks, through its zero-length terminator.
    bool blocks(){uint8_t size;for(;;){if(!byte(size))return false;if(!size)return true;if(!skip(size))return false;}}
};
bool gif_header(const uint8_t* p,uint32_t n,ImageSize* out){
    if(n<13||p[0]!='G'||p[1]!='I'||p[2]!='F'||p[3]!='8'||(p[4]!='7'&&p[4]!='9')||p[5]!='a')return false;
    *out={uint32_t(p[6])|uint32_t(p[7])<<8,uint32_t(p[8])|uint32_t(p[9])<<8};
    return out->width&&out->height&&out->width<=512&&out->height<=512;
}
}
bool gif_first_frame(const uint8_t* p,uint32_t n,uint8_t* bgra,uint32_t capacity,ImageSize* out) {
    ImageSize size{};if(out)*out={};
    if(!p||!bgra||!out||!gif_header(p,n,&size)||uint64_t(size.width)*size.height*4>capacity)return false;
    const uint32_t width=size.width,height=size.height;
    GifReader r{p,n,10};uint8_t packed=0,unused=0;
    if(!r.byte(packed)||!r.byte(unused)||!r.byte(unused))return false;
    const uint8_t* global=nullptr;uint32_t global_colors=0;
    if(packed&0x80){global_colors=2u<<(packed&7);if(n-r.at<global_colors*3)return false;global=p+r.at;r.at+=global_colors*3;}
    int transparent=-1;
    for(;;){
        uint8_t kind=0;if(!r.byte(kind))return false;
        if(kind==0x21){
            uint8_t label=0,block=0;if(!r.byte(label))return false;
            if(label==0xf9){
                // Graphic control: which palette index is transparent, if any.
                if(!r.byte(block))return false;
                if(block==4){uint8_t flags=0,index=0;if(!r.byte(flags)||!r.skip(2)||!r.byte(index))return false;transparent=flags&1?int(index):-1;}
                else if(!r.skip(block))return false;
                if(block&&!r.blocks())return false;
            }else if(!r.blocks())return false;
            continue;
        }
        if(kind!=0x2c)return false; // the trailer, or anything else, before a picture
        uint32_t left=0,top=0,frame_width=0,frame_height=0;uint8_t flags=0,code_size=0;
        if(!r.word(left)||!r.word(top)||!r.word(frame_width)||!r.word(frame_height)||!r.byte(flags))return false;
        if(!frame_width||!frame_height||left+frame_width>width||top+frame_height>height)return false;
        const uint8_t* palette=global;uint32_t colors=global_colors;
        if(flags&0x80){colors=2u<<(flags&7);if(n-r.at<colors*3)return false;palette=p+r.at;r.at+=colors*3;}
        if(!palette||!r.byte(code_size)||code_size<2||code_size>8)return false;
        for(uint32_t i=0;i<width*height*4;++i)bgra[i]=0;
        // Interlaced rows arrive in four passes: every 8th from 0, every 8th
        // from 4, every 4th from 2, every 2nd from 1.
        const bool interlaced=(flags&0x40)!=0;
        const uint32_t pass1=(frame_height+7)/8,pass2=(frame_height+3)/8,pass3=(frame_height+1)/4;
        const uint32_t total=frame_width*frame_height;uint32_t written=0;
        auto put=[&](uint8_t index){
            if(written>=total)return;
            uint32_t row=written/frame_width;const uint32_t x=written%frame_width;++written;
            if(interlaced){
                if(row<pass1)row*=8;
                else if((row-=pass1)<pass2)row=4+row*8;
                else if((row-=pass2)<pass3)row=2+row*4;
                else row=1+(row-pass3)*2;
                if(row>=frame_height)return;
            }
            if(int(index)==transparent||index>=colors)return;
            uint8_t* pixel=bgra+(uint64_t(top+row)*width+left+x)*4;const uint8_t* color=palette+uint32_t(index)*3;
            pixel[0]=color[2];pixel[1]=color[1];pixel[2]=color[0];pixel[3]=255;
        };
        // LZW with codes of up to 12 bits. A table entry is a prefix code and
        // one more byte; entries are only ever built from codes already seen,
        // so a chain cannot be longer than the table.
        static uint16_t prefix[4096];static uint8_t suffix[4096],stack[4098];
        const uint32_t clear=1u<<code_size,stop=clear+1;
        uint32_t next=clear+2,bits=code_size+1u,previous=UINT32_MAX,buffer=0,held=0,block_left=0;uint8_t first=0;
        for(;;){
            while(held<bits){
                if(!block_left){uint8_t block=0;if(!r.byte(block))return false;if(!block)return written==total;block_left=block;}
                uint8_t value=0;if(!r.byte(value))return false;--block_left;
                buffer|=uint32_t(value)<<held;held+=8;
            }
            const uint32_t code=buffer&((1u<<bits)-1);buffer>>=bits;held-=bits;
            if(code==clear){next=clear+2;bits=code_size+1u;previous=UINT32_MAX;continue;}
            if(code==stop)return written==total;
            if(previous==UINT32_MAX){if(code>=clear)return false;first=uint8_t(code);put(first);previous=code;continue;}
            if(code>next||(code==next&&next>=4096))return false;
            uint32_t depth=0,current=code;
            if(code==next){stack[depth++]=first;current=previous;}
            while(current>=clear){
                if(current<clear+2||current>=next||depth>=4096)return false;
                stack[depth++]=suffix[current];current=prefix[current];
            }
            first=uint8_t(current);stack[depth++]=first;
            while(depth)put(stack[--depth]);
            if(next<4096){prefix[next]=uint16_t(previous);suffix[next]=first;++next;if(next==(1u<<bits)&&bits<12)++bits;}
            previous=code;
            if(written==total){*out=size;return true;}
        }
    }
}
bool image_envelope(const uint8_t* p,uint32_t n,ImageKind kind,ImageSize* out) {
    if(out)*out={};
    if(!p||!out||!n||n>(kind==ImageKind::Preview?Images::MAX_ENCODED:Images::MAX_THUMBNAIL_ENCODED))return false;
    // A GIF is only taken as a small image (an avatar); gif_first_frame is its decoder.
    if(n>=6&&p[0]=='G'&&p[1]=='I'&&p[2]=='F')return kind==ImageKind::Thumbnail&&gif_header(p,n,out);
    static const uint8_t sig[8]{137,80,78,71,13,10,26,10};
    bool png=n>=8;for(uint32_t i=0;i<8&&png;++i)png=p[i]==sig[i];
    if(png){
        uint32_t at=8,w=0,h=0;bool first=true,data=false,ended=false;
        while(at<n){
            if(n-at<12)return false;
            const uint32_t len=be32(p+at);if(len>n-at-12)return false;
            const uint8_t* t=p+at+4;const uint32_t tag=be32(t);
            if(crc(t,len+4)!=be32(t+len+4))return false;
            if(first){
                if(tag!=0x49484452||len!=13)return false;
                w=be32(t+4);h=be32(t+8);
                if(!dimensions(w,h,kind)||t[12]!=8||
                   !(t[13]==0||t[13]==2||t[13]==3||t[13]==4||t[13]==6)||t[14]||t[15]||t[16])return false;
                first=false;
            }else if(tag==0x49484452)return false;
            if(tag==0x49444154)data=true;
            // Reject APNG and unknown critical chunks before the game decoder.
            if(tag==0x6163544c||tag==0x6663544c||tag==0x66644154)return false;
            if(!(t[0]&32)&&tag!=0x49484452&&tag!=0x504c5445&&tag!=0x49444154&&tag!=0x49454e44)return false;
            at+=len+12;
            if(tag==0x49454e44){if(len||!data||at!=n)return false;ended=true;break;}
        }
        if(!ended)return false;
        *out={w,h};return true;
    }
    // Only baseline/progressive 8-bit grayscale/RGB JPEG. Walk bounded marker
    // lengths and entropy escapes; all scans must terminate in the final EOI.
    if(n<4||p[0]!=255||p[1]!=216)return false;
    uint32_t at=2,w=0,h=0;bool frame=false,scan=false,in_scan=false;
    while(at<n){
        if(in_scan){
            if(p[at++]!=255)continue;
            while(at<n&&p[at]==255)++at;
            if(at==n)return false;
            if(p[at]==0||(p[at]>=0xd0&&p[at]<=0xd7)){++at;continue;}
            --at;in_scan=false;
        }
        if(at>=n||p[at++]!=255)return false;
        while(at<n&&p[at]==255)++at;
        if(at==n)return false;
        const uint32_t marker=p[at++];
        if(marker==0xd9){if(!frame||!scan||at!=n)return false;*out={w,h};return true;}
        if(marker==0||marker==0xd8||marker==1||(marker>=0xd0&&marker<=0xd7))return false;
        if(n-at<2)return false;
        const uint32_t len=be16(p+at);if(len<2||len>n-at)return false;
        if(marker>=0xc0&&marker<=0xcf&&marker!=0xc4&&marker!=0xc8&&marker!=0xcc){
            if(frame||(marker!=0xc0&&marker!=0xc2)||len<8||p[at+2]!=8)return false;
            h=be16(p+at+3);w=be16(p+at+5);const uint32_t channels=p[at+7];
            if((channels!=1&&channels!=3)||len!=8+3*channels||!dimensions(w,h,kind))return false;
            frame=true;
        }
        if(marker==0xda){
            if(!frame||len<6)return false;
            const uint32_t channels=p[at+2];if(!channels||channels>3||len!=6+2*channels)return false;
            scan=true;in_scan=true;
        }
        at+=len;
    }
    return false;
}
uint32_t image_reduction(const ImageSize& decoded,ImageKind kind) {
    if(kind!=ImageKind::Preview||!decoded.width||!decoded.height)return 1;
    uint32_t factor=(decoded.width+1279)/1280;
    const uint32_t tall=(decoded.height+719)/720;if(tall>factor)factor=tall;
    return factor;
}
ImageSize image_stored_size(const ImageSize& decoded,ImageKind kind) {
    if(kind==ImageKind::Card){
        if(decoded.width<=Images::CARD_WIDTH)return decoded;
        const uint32_t height=uint32_t(uint64_t(decoded.height)*Images::CARD_WIDTH/decoded.width);
        return {Images::CARD_WIDTH,height?height:1};
    }
    const uint32_t factor=image_reduction(decoded,kind);
    return {decoded.width/factor,decoded.height/factor};
}
void Images::drop(Slot& s) {
    if(s.transfer&&api_)api_->release(s.transfer);
    if(s.texture.object&&backend_.release)backend_.release(backend_.context,s.texture);
    resident_bytes_-=s.bytes;if(s.kind!=ImageKind::Preview)thumbnail_bytes_-=s.bytes;s={};
}
void Images::attach(const P4M_HttpsApi* api,const char* owner,const ImageBackend& backend) {
    reset(0);api_=nullptr;owner_[0]=0;backend_=backend;
    if(api&&api->size>=sizeof(*api)&&api->abi==P4M_HTTPS_ABI&&api->get&&api->read&&api->status&&api->release&&
       backend.upload&&backend.release&&copy(owner_,sizeof owner_,owner))api_=api;
}
void Images::reset(uint32_t gen) {for(auto& s:slots_)drop(s);generation_=gen;clock_=0;sequence_=0;}
void Images::begin_visible() {++clock_;sequence_=0;for(auto& s:slots_)s.wanted=s.prefetch=false;}
Images::Slot* Images::acquire(uint64_t id,const char* url,ImageKind kind) {
    if(!api_||!generation_||!id||!https_url(url))return nullptr;
    const uint32_t first=kind==ImageKind::Preview?THUMBNAILS:0;
    const uint32_t end=kind==ImageKind::Preview?SLOTS:THUMBNAILS;
    // Every card asks every frame, so the lookup compares a hash before text.
    uint32_t hash=2166136261u;for(const char* p=url;*p;++p)hash=(hash^uint8_t(*p))*16777619u;
    Slot* found=nullptr;Slot* empty=nullptr;Slot* oldest=nullptr;
    for(uint32_t i=first;i<end;++i){auto& s=slots_[i];
        if(!s.url[0]){if(!empty)empty=&s;continue;}
        if(s.hash==hash&&s.kind==kind&&same(s.url,url)){found=&s;break;}
        if(!s.wanted&&!s.prefetch&&(!oldest||s.touched<oldest->touched))oldest=&s;
    }
    if(!found){
        found=empty?empty:oldest;if(!found)return nullptr;
        drop(*found);
        if(!copy(found->url,sizeof found->url,url)){found->url[0]=0;return nullptr;}
        found->kind=kind;found->state=ImageState::Waiting;found->hash=hash;
    }
    found->submission=id;found->touched=clock_;
    return found;
}
ImageView Images::want(uint64_t id,const char* url,ImageKind kind) {
    Slot* s=acquire(id,url,kind);if(!s)return {};
    if(!s->wanted){s->wanted=true;s->order=++sequence_;}
    return {s->state,s->texture,generation_,s->size};
}
ImageView Images::prefetch(uint64_t id,const char* url,ImageKind kind) {
    Slot* s=acquire(id,url,kind);if(!s)return {};
    if(!s->wanted&&!s->prefetch){s->prefetch=true;s->order=++sequence_;}
    return {s->state,s->texture,generation_,s->size};
}
ImageView Images::cached(const char* url,ImageKind kind) const {
    if(!url||!url[0])return {};
    uint32_t hash=2166136261u;for(const char* p=url;*p;++p)hash=(hash^uint8_t(*p))*16777619u;
    const uint32_t first=kind==ImageKind::Preview?THUMBNAILS:0,end=kind==ImageKind::Preview?SLOTS:THUMBNAILS;
    for(uint32_t i=first;i<end;++i){const auto& s=slots_[i];
        if(s.url[0]&&s.hash==hash&&s.kind==kind&&same(s.url,url))return {s.state,s.texture,generation_,s.size};}
    return {};
}
void Images::retry(uint64_t id,ImageKind kind) {
    for(auto& s:slots_)if(s.submission==id&&s.kind==kind&&s.state==ImageState::Failed){s.state=ImageState::Waiting;s.attempts=0;s.retry_at=0;}
}
uint32_t Images::pending() const {uint32_t n=0;for(const auto& s:slots_)if(s.transfer)++n;return n;}
uint32_t Images::unsettled() const {uint32_t n=0;for(const auto& s:slots_)if(s.wanted&&(s.state==ImageState::Waiting||s.state==ImageState::Loading))++n;return n;}
uint32_t Images::failed() const {uint32_t n=0;for(const auto& s:slots_)if(s.wanted&&s.state==ImageState::Failed)++n;return n;}
void Images::transfer_failed(Slot& s,bool permanent) {
    if(s.transfer){api_->release(s.transfer);s.transfer=0;}
    if(permanent)s.attempts=ATTEMPTS;
    if(s.attempts<ATTEMPTS&&++s.attempts<ATTEMPTS){s.state=ImageState::Waiting;s.retry_at=clock_+uint64_t(RETRY_FRAMES)*s.attempts;}
    else s.state=ImageState::Failed;
}
void Images::trim_thumbnails() {
    while(thumbnail_bytes_>THUMBNAIL_BYTES){
        Slot* oldest=nullptr;
        for(uint32_t i=0;i<THUMBNAILS;++i){auto& s=slots_[i];
            if(s.bytes&&!s.wanted&&!s.prefetch&&(!oldest||s.touched<oldest->touched))oldest=&s;}
        if(!oldest)return;
        drop(*oldest);
    }
}
void Images::decoded(Slot& s,bool ok,TextureToken token,const ImageSize& size) {
    if(ok&&token.object&&token.serial){
        s.texture=token;s.size=image_stored_size(size,s.kind);s.bytes=s.size.width*s.size.height*4;s.state=ImageState::Ready;
        resident_bytes_+=s.bytes;if(s.kind!=ImageKind::Preview){thumbnail_bytes_+=s.bytes;trim_thumbnails();}
    }else {
        // A file the validator or decoder rejects is not fetched again.
        if(token.object)backend_.release(backend_.context,token);
        s.state=ImageState::Failed;s.attempts=ATTEMPTS;
    }
}
void Images::pump() {
    if(!api_)return;
    // One decode per frame: of the finished transfers, the one asked for first.
    Slot* ready=nullptr;
    for(auto& s:slots_){
        if(!s.transfer)continue;
        P4M_HttpsStatus status{};status.size=sizeof status;
        if(api_->status(s.transfer,&status)!=P4M_OK){transfer_failed(s,false);continue;}
        if(status.state<P4M_HTTPS_SUCCEEDED)continue;
        if(status.state!=P4M_HTTPS_SUCCEEDED){
            // The server's answer will not change; a dropped connection might.
            transfer_failed(s,status.error==P4M_HTTPS_ERROR_HTTP||status.error==P4M_HTTPS_ERROR_LIMIT||status.error==P4M_HTTPS_ERROR_PROTOCOL);
            continue;
        }
        if(!ready||before(s,*ready))ready=&s;
    }
    if(ready){
        auto& s=*ready;P4M_HttpsStatus status{};status.size=sizeof status;uint32_t read=0;ImageSize size{};TextureToken token{};
        const uint32_t limit=s.kind==ImageKind::Preview?MAX_ENCODED:MAX_THUMBNAIL_ENCODED;
        const bool valid=api_->status(s.transfer,&status)==P4M_OK&&status.received&&status.received<=limit&&
            api_->read(s.transfer,0,encoded_,uint32_t(status.received),&read)==P4M_OK&&read==status.received&&
            image_envelope(encoded_,read,s.kind,&size);
        api_->release(s.transfer);s.transfer=0;
        // Decode first: the token is an argument of decoded() too.
        const bool ok=valid&&backend_.upload(backend_.context,encoded_,read,s.kind,size,&token);
        decoded(s,ok,token,size);
    }
    // Start what is on screen in the order it was asked for, then prefetches.
    for(uint32_t active=pending();active<MAX_INFLIGHT;++active){
        Slot* next=nullptr;
        for(auto& s:slots_)if((s.wanted||s.prefetch)&&s.state==ImageState::Waiting&&!s.transfer&&clock_>=s.retry_at&&(!next||before(s,*next)))next=&s;
        if(!next)break;
        P4M_HttpsRequest r{sizeof r,P4M_HTTPS_MEMORY,next->url,nullptr,next->kind==ImageKind::Preview?MAX_ENCODED:MAX_THUMBNAIL_ENCODED,10000,45000};
        const auto result=api_->get(owner_,&r,&next->transfer);
        if(result==P4M_OK)next->state=ImageState::Loading;
        else {
            next->transfer=0;
            if(result==P4M_CAPACITY||result==P4M_BUSY)break;
            next->state=ImageState::Failed;next->attempts=ATTEMPTS;
        }
    }
}
}}
