#pragma once
#include "provider.h"
namespace tsuchi { namespace browser { namespace card_presentation {
// Design-canvas pixels: pause at each end; move only while focused/hovered.
inline float title_offset(float elapsed,float overflow,bool active){
    if(!active||!(overflow>0)||!(elapsed>0)||elapsed>1.e12f||overflow>10000)return 0;
    const float travel=overflow/30.f,period=2.f*(1.f+travel);
    if(elapsed>=period)elapsed-=float(uint64_t(elapsed/period))*period;
    if(elapsed<=1.f)return 0;elapsed-=1.f;
    if(elapsed<travel)return elapsed*30.f;elapsed-=travel;
    if(elapsed<=1.f)return overflow;elapsed-=1.f;
    const float out=overflow-elapsed*30.f;return out>0?out:0;
}
struct Text {
    char data[128]{};uint32_t used=0;
    void add(const char* s){while(*s&&used+1<sizeof data)data[used++]=*s++;data[used]=0;}
    void number(uint64_t value){char digits[21];uint32_t n=0;do{digits[n++]=char('0'+value%10);value/=10;}while(value);while(n){char c[2]{digits[--n],0};add(c);}}
    void count(uint64_t value){const uint64_t units[]{1,1000,1000000,1000000000,1000000000000ull,1000000000000000ull,1000000000000000000ull};
        uint32_t index=0;while(index<6&&value>=units[index+1])++index;const uint64_t unit=units[index];
        // One decimal only below ten of the unit: 1.2k, then 12k, 123k.
        number(value/unit);if(index){const uint64_t digit=(value%unit)/(unit/10);if(digit&&value/unit<10){add(".");number(digit);}const char suffix[2]{" kmbtpe"[index],0};add(suffix);}}
};
inline void date(Text& text,uint64_t timestamp){
    if(timestamp<946684800||timestamp>253402300799ull){text.add("--");return;}
    uint64_t days=timestamp/86400;uint32_t year=1970;
    for(;;){const bool leap=year%4==0&&(year%100!=0||year%400==0);uint32_t length=leap?366:365;if(days<length)break;days-=length;++year;}
    const uint32_t lengths[]{31,28,31,30,31,30,31,31,30,31,30,31};uint32_t month=0;
    for(;month<11;++month){uint32_t length=lengths[month]+(month==1&&year%4==0&&(year%100!=0||year%400==0));if(days<length)break;days-=length;}
    if(month+1<10)text.add("0");text.number(month+1);text.add("/");if(days+1<10)text.add("0");text.number(days+1);
    text.add("/");if(year%100<10)text.add("0");text.number(year%100);
}
// GameBanana leaves a count out of a listing when it is zero, so an absent
// count is shown as 0.
inline Text metadata(const Listing& item){Text text;text.add("Likes ");text.count(item.likes);
    text.add("   Views ");text.count(item.views);text.add("   Upd ");date(text,item.updated);return text;}
}}}
