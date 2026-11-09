#include "json.h"
namespace tsuchi { namespace browser { namespace json {
static bool ws(char c) { return c==' '||c=='\r'||c=='\n'||c=='\t'; }
static void space(const char*& p,const char* e) { while(p<e&&ws(*p))++p; }
static int hex(char c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; }
static bool code(const char*& p,const char* e,uint32_t& cp) {
    if(p>=e)return false;
    uint8_t c=(uint8_t)*p++; cp=c;
    if(c=='\\') {
        if(p>=e)return false; c=(uint8_t)*p++;
        if(c=='u') {
            cp=0; for(int i=0;i<4;++i) { if(p>=e||hex(*p)<0)return false;cp=cp*16+(uint32_t)hex(*p++); }
            if(cp>=0xd800&&cp<=0xdbff) {
                if(e-p<6||*p++!='\\'||*p++!='u')return false;
                uint32_t lo=0;for(int i=0;i<4;++i) { if(hex(*p)<0)return false;lo=lo*16+(uint32_t)hex(*p++); }
                if(lo<0xdc00||lo>0xdfff)return false;cp=0x10000+((cp-0xd800)<<10)+lo-0xdc00;
            } else if(cp>=0xdc00&&cp<=0xdfff)return false;
            return cp!=0;
        }
        if(c=='"'||c=='\\'||c=='/')cp=c;
        else if(c=='b')cp=8; else if(c=='f')cp=12; else if(c=='n')cp=10; else if(c=='r')cp=13; else if(c=='t')cp=9;else return false;
        return true;
    }
    if(c<32)return false;
    if(c<128)return true;
    int n;uint32_t min;
    if(c>=0xc2&&c<=0xdf){n=1;cp=c&31;min=128;}
    else if(c>=0xe0&&c<=0xef){n=2;cp=c&15;min=2048;}
    else if(c>=0xf0&&c<=0xf4){n=3;cp=c&7;min=65536;}
    else return false;
    while(n--) { if(p>=e||((uint8_t)*p&0xc0)!=0x80)return false;cp=(cp<<6)|((uint8_t)*p++&63); }
    return cp>=min&&cp<=0x10ffff&&!(cp>=0xd800&&cp<=0xdfff);
}
static bool string(const char*& p,const char* e) {
    if(p>=e||*p++!='"')return false;
    while(p<e&&*p!='"') {uint32_t cp;if(!code(p,e,cp))return false;}
    if(p>=e)return false;++p;return true;
}
static bool literal(const char*&p,const char*e,const char*s) {while(*s) {if(p>=e||*p++!=*s++)return false;}return true;}
static bool value(const char*&p,const char*e,uint32_t depth) {
    space(p,e);if(p>=e||depth>24)return false;
    if(*p=='"')return string(p,e);
    if(*p=='{'||*p=='[') {
        char close=*p++=='{'?'}':']';space(p,e);if(p<e&&*p==close){++p;return true;}
        for(;;) {
            if(close=='}') {if(!string(p,e))return false;space(p,e);if(p>=e||*p++!=':')return false;}
            if(!value(p,e,depth+1))return false;space(p,e);if(p>=e)return false;
            if(*p==close){++p;return true;}if(*p++!=',')return false;space(p,e);
        }
    }
    if(*p=='t')return literal(p,e,"true");if(*p=='f')return literal(p,e,"false");if(*p=='n')return literal(p,e,"null");
    if(*p=='-')++p;if(p>=e)return false;
    if(*p=='0')++p;else {if(*p<'1'||*p>'9')return false;while(p<e&&*p>='0'&&*p<='9')++p;}
    if(p<e&&*p=='.'){++p;if(p>=e||*p<'0'||*p>'9')return false;while(p<e&&*p>='0'&&*p<='9')++p;}
    if(p<e&&(*p=='e'||*p=='E')){++p;if(p<e&&(*p=='+'||*p=='-'))++p;if(p>=e||*p<'0'||*p>'9')return false;while(p<e&&*p>='0'&&*p<='9')++p;}
    return true;
}
bool document(const char*b,uint32_t n,Value&o) {o={};if(!b||!n||n>(512u<<10))return false;const char*p=b,*e=b+n;space(p,e);const char*s=p;if(!value(p,e,0))return false;const char*end=p;space(p,e);if(p!=e)return false;o={s,end};return true;}
bool text(Value v,char*out,uint32_t cap,bool*truncated) {
    if(truncated)*truncated=false;if(!out||!cap)return false;out[0]=0;if(!v||*v.begin!='"')return false;
    const char*p=v.begin+1,*e=v.end-1;uint32_t used=0;bool cut=false;
    while(p<e) {
        uint32_t cp;if(!code(p,e,cp))return false;char b[4];uint32_t n;
        if(cp<128){b[0]=(char)cp;n=1;}else if(cp<2048){b[0]=(char)(0xc0|(cp>>6));b[1]=(char)(0x80|(cp&63));n=2;}
        else if(cp<65536){b[0]=(char)(0xe0|(cp>>12));b[1]=(char)(0x80|((cp>>6)&63));b[2]=(char)(0x80|(cp&63));n=3;}
        else {b[0]=(char)(0xf0|(cp>>18));b[1]=(char)(0x80|((cp>>12)&63));b[2]=(char)(0x80|((cp>>6)&63));b[3]=(char)(0x80|(cp&63));n=4;}
        if(!cut&&used+n<cap)for(uint32_t i=0;i<n;++i)out[used++]=b[i];else cut=true;
    }
    out[used]=0;if(truncated)*truncated=cut;return true;
}
static bool key(Value v,const char*s) {const char*p=v.begin+1,*e=v.end-1;while(p<e){uint32_t cp;if(!code(p,e,cp)||!*s||cp!=(uint8_t)*s++)return false;}return !*s;}
Value member(Value v,const char*s) {
    if(!v||*v.begin!='{')return {};const char*p=v.begin+1;Value found={};space(p,v.end);
    while(p<v.end&&*p!='}') {
        const char*k=p;if(!string(p,v.end))return {};Value name={k,p};space(p,v.end);if(p>=v.end||*p++!=':')return {};space(p,v.end);
        const char*b=p;if(!value(p,v.end,0))return {};if(key(name,s)){if(found)return {};found={b,p};}
        space(p,v.end);if(p<v.end&&*p==','){++p;space(p,v.end);}else break;
    }return found;
}
bool next(Value a,uint32_t& cursor,Value&o) {
    o={};if(!a||*a.begin!='[')return false;const char*p=a.begin+(cursor?cursor:1);space(p,a.end);if(p>=a.end||*p==']')return false;
    const char*b=p;if(!value(p,a.end,0))return false;o={b,p};space(p,a.end);if(p<a.end&&*p==',')++p;cursor=(uint32_t)(p-a.begin);return true;
}
bool number(Value v,uint64_t&o) {o=0;if(!v||v.begin==v.end)return false;for(const char*p=v.begin;p<v.end;++p){if(*p<'0'||*p>'9')return false;uint32_t d=*p-'0';if(o>(UINT64_MAX-d)/10)return false;o=o*10+d;}return true;}
bool entry(Value object,uint32_t& cursor,Value& name,Value& out) {
    name={};out={};if(!object||*object.begin!='{')return false;
    const char* p=object.begin+(cursor?cursor:1);space(p,object.end);
    if(p>=object.end||*p=='}')return false;
    const char* begin=p;if(!string(p,object.end))return false;name={begin,p};
    space(p,object.end);if(p>=object.end||*p++!=':')return false;space(p,object.end);
    begin=p;if(!value(p,object.end,0))return false;out={begin,p};
    space(p,object.end);if(p<object.end&&*p==',')++p;
    cursor=(uint32_t)(p-object.begin);return true;
}
bool real(Value v,double& out) {
    out=0;if(!v||v.begin==v.end)return false;
    const char* p=v.begin;bool negative=false;if(*p=='-'){negative=true;++p;}
    if(p==v.end||*p<'0'||*p>'9')return false;
    // JSON syntax is checked by document(); retain at most 18 significant
    // decimal digits and adjust the exponent for discarded integer digits.
    double significand=0;int exponent=0,digits=0;
    while(p<v.end&&*p>='0'&&*p<='9'){
        if(digits<18){significand=significand*10+(*p-'0');if(significand!=0)++digits;}
        else ++exponent;++p;
    }
    if(p<v.end&&*p=='.'){
        ++p;while(p<v.end&&*p>='0'&&*p<='9'){
            if(digits<18){significand=significand*10+(*p-'0');--exponent;if(significand!=0)++digits;}
            ++p;
        }
    }
    if(p<v.end&&(*p=='e'||*p=='E')){
        ++p;bool minus=false;if(p<v.end&&(*p=='-'||*p=='+'))minus=*p++=='-';
        int exp=0;while(p<v.end&&*p>='0'&&*p<='9'){if(exp<1000000)exp=exp*10+(*p-'0');++p;}
        exponent+=minus?-exp:exp;
    }
    if(p!=v.end)return false;if(!significand){out=0;return true;}
    if(exponent>308)return false;if(exponent<-400){out=0;return true;}
    while(exponent>0){significand*=10;--exponent;}
    while(exponent<0){significand/=10;++exponent;}
    if(!(significand<=__DBL_MAX__))return false;
    out=negative?-significand:significand;return true;
}
bool boolean(Value v,bool&o) {if(!v)return false;if(v.end-v.begin==4&&v.begin[0]=='t'){o=true;return true;}if(v.end-v.begin==5&&v.begin[0]=='f'){o=false;return true;}return false;}
} } }
