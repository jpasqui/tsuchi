#include <assert.h>
#include <stdio.h>
#include "registry.h"
using namespace tsuchi;
namespace {
Topic topic{"owner.events",[](void(*fn)(),void* context,const void* event){reinterpret_cast<void(*)(void*,const int*)>(fn)(context,static_cast<const int*>(event));}};
TSUCHI_Mod* a=nullptr;TSUCHI_Mod* b=nullptr;TSUCHI_Subscription subscription_a{},other{},later{};
int heard_a=0,heard_b=0,heard_later=0;uint64_t token=1;
uint64_t probe(){return token;}
void late(void*,const int*){++heard_later;}
void second(void*,const int*){++heard_b;}
void first(void*,const int* value){++heard_a;if(*value==1){assert(cancel(a,subscription_a)==TSUCHI_OK);assert(subscribe(topic,a,reinterpret_cast<void(*)()>(late),nullptr,&later)==TSUCHI_OK);int nested=2;emit_to(topic,b,&nested);}if(*value==3)mod_stop(a);}
}
int main(){a=mod_create("owner.alpha");b=mod_create("owner.beta");assert(subscribe(topic,a,reinterpret_cast<void(*)()>(first),nullptr,&subscription_a)==TSUCHI_OK);assert(subscribe(topic,b,reinterpret_cast<void(*)()>(second),nullptr,&other)==TSUCHI_OK);int value=1;emit_to(topic,a,&value);assert(heard_a==1&&heard_b==1&&heard_later==0);emit_to(topic,a,&value);assert(heard_later==1&&heard_b==1);emit_to(topic,nullptr,&value);assert(heard_later==1);emit(topic,&value);assert(heard_later==2&&heard_b==2);set_thread_probe(probe);note_game_thread();token=2;emit_to(topic,a,&value);assert(heard_later==2);token=1;mod_stop(a);emit_to(topic,a,&value);assert(heard_later==2);assert(subscriber_count(topic)==1);mod_stop(b);assert(subscriber_count(topic)==0);puts("PASS targeted event dispatch, nested events, stop, subscription changes, and thread refusal");}
