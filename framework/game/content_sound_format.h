#pragma once
#include <stdint.h>
// Bounded v135 embedded PCM one-shot subset produced by tools/creatures/wwise_bank.py.
// Header/object validation precedes the native bank parser. No stock nodes copied.
namespace tsuchi::sound_bank_format {
constexpr uint32_t MAX_BYTES=16u*1024u*1024u,MAX_SAMPLES=64,MAX_IDS=1+4*MAX_SAMPLES;
inline uint32_t u32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline uint16_t u16(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
inline bool tag(const uint8_t* p,const char* s){return p[0]==s[0]&&p[1]==s[1]&&p[2]==s[2]&&p[3]==s[3];}
struct Bank {
    uint32_t bank=0,project=0,count=0,id_count=0;
    uint32_t ids[MAX_IDS]{},events[MAX_SAMPLES]{},sounds[MAX_SAMPLES]{},actions[MAX_SAMPLES]{},media[MAX_SAMPLES]{},media_bytes[MAX_SAMPLES]{};
};
inline void assign(Bank& out,const Bank& in){
    // Freestanding cores also run in test modules without a C runtime.
    auto* d=reinterpret_cast<volatile uint8_t*>(&out);const auto* s=reinterpret_cast<const uint8_t*>(&in);
    for(uint32_t i=0;i<sizeof(Bank);++i)d[i]=s[i];
}
inline bool add(Bank& b,uint32_t id){
    if(!id||b.id_count==MAX_IDS)return false;
    for(uint32_t i=0;i<b.id_count;++i)if(b.ids[i]==id)return false;
    b.ids[b.id_count++]=id;return true;
}
inline bool zero(const uint8_t* p,uint32_t n){for(uint32_t i=0;i<n;++i)if(p[i])return false;return true;}
inline bool pcm(const uint8_t* p,uint32_t n){
    if(n<64||!tag(p,"RIFF")||u32(p+4)!=n-8||!tag(p+8,"WAVE"))return false;
    uint32_t at=12,channels=0,rate=0,align=0,bytes=0;bool format=false,data=false;
    while(at<n){
        if(n-at<8)return false;const uint8_t* q=p+at;uint32_t size=u32(q+4);at+=8;
        if(size>n-at)return false;
        if(tag(q,"fmt ")){
            if(format||size!=24||u16(p+at)!=0xFFFE)return false;
            channels=u16(p+at+2);rate=u32(p+at+4);align=u16(p+at+12);
            if((channels!=1&&channels!=2)||rate<8000||rate>192000||align!=channels*2||u16(p+at+14)!=16||u16(p+at+16)!=6||u16(p+at+18)!=16||u32(p+at+8)!=rate*align)return false;
            const uint32_t config=channels|0x100u|((channels==1?4u:3u)<<12);
            if(u32(p+at+20)!=config)return false;format=true;
        }else if(tag(q,"data")){if(data||!size)return false;data=true;bytes=size;}
        else if(!tag(q,"JUNK")||!zero(p+at,size))return false;
        at+=size;if(size&1){if(at==n)return false;++at;}
    }
    return at==n&&format&&data&&bytes%align==0&&bytes/align<=rate*30u;
}
inline bool parse(const void* bytes,uint32_t size,Bank& b){
    b=Bank{};if(!bytes||size<64||size>MAX_BYTES)return false;
    const auto* p=static_cast<const uint8_t*>(bytes);const uint8_t* parts[4]{};uint32_t lengths[4]{};uint32_t at=0;
    const char* tags[4]{"BKHD","DIDX","DATA","HIRC"};
    for(uint32_t i=0;i<4;++i){
        if(size-at<8||!tag(p+at,tags[i]))return false;const uint32_t n=u32(p+at+4);at+=8;
        if(n>size-at)return false;parts[i]=p+at;lengths[i]=n;if(i==2&&at%16)return false;at+=n;
    }
    if(at!=size||lengths[0]<20||u32(parts[0])!=135||u32(parts[0]+8)!=0||u32(parts[0]+12)!=16||!zero(parts[0]+20,lengths[0]-20))return false;
    b.bank=u32(parts[0]+4);b.project=u32(parts[0]+16);if(!add(b,b.bank))return false;
    if(!lengths[1]||lengths[1]%12)return false;b.count=lengths[1]/12;if(b.count>MAX_SAMPLES)return false;
    uint32_t end=0;
    for(uint32_t i=0;i<b.count;++i){
        const auto* q=parts[1]+i*12;const uint32_t id=u32(q),start=u32(q+4),n=u32(q+8);
        if(!add(b,id)||start%16||start<end||start>lengths[2]||n>lengths[2]-start||!zero(parts[2]+end,start-end)||!pcm(parts[2]+start,n))return false;
        b.media[i]=id;b.media_bytes[i]=n;end=start+n;
    }
    if(end!=lengths[2]||lengths[3]<4||u32(parts[3])!=b.count*3)return false;at=4;
    for(uint32_t i=0;i<b.count;++i){
        for(uint32_t kind=2;kind<=4;++kind){
            if(lengths[3]-at<9)return false;const auto* q=parts[3]+at;const uint32_t n=u32(q+1),id=u32(q+5);at+=9;
            if(q[0]!=kind||n<4||n-4>lengths[3]-at||!add(b,id))return false;const auto* body=parts[3]+at;const uint32_t len=n-4;
            if(kind==2){
                if((len!=44&&len!=45)||u32(body)!=0x10001||body[4]!=0||u32(body+5)!=b.media[i]||u32(body+9)!=b.media_bytes[i]||body[13]!=0x80)return false;
                if(body[14]!=1||body[15]!=0||body[16]!=0||!u32(body+17)||u32(body+21)!=0||!zero(body+25,3))return false;
                uint32_t pos=28;if(len==45){if(body[pos]!=3||body[pos+1]!=1)return false;pos+=2;}else{if(body[pos]!=1)return false;++pos;}
                if(!zero(body+pos,5)||body[pos+5]!=0x10||!zero(body+pos+6,9))return false;
                b.sounds[i]=id;
            }else if(kind==3){
                if(len!=14||u16(body)!=0x403||u32(body+2)!=b.sounds[i]||!zero(body+6,3)||body[9]!=4||u32(body+10)!=b.bank)return false;b.actions[i]=id;
            }else{
                if(len!=5||body[0]!=1||u32(body+1)!=b.actions[i])return false;b.events[i]=id;
            }
            at+=len;
        }
    }
    return at==lengths[3];
}
inline bool collides(const Bank& a,const Bank& b){for(uint32_t i=0;i<a.id_count;++i)for(uint32_t j=0;j<b.id_count;++j)if(a.ids[i]==b.ids[j])return true;return false;}
inline bool contains(const Bank& b,uint32_t event){for(uint32_t i=0;i<b.count;++i)if(b.events[i]==event)return true;return false;}
}
