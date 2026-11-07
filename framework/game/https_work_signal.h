#pragma once
#include "https_jobs.h"
namespace tsuchi::https {
// Snapshot before scanning a queue; publish work before notify. The platform
// wait compares the same word atomically with parking, closing the lost-wake gap.
struct WorkSignal {
 HttpsAtomic<uint32_t> generation{};
 uint32_t sample() const{return generation.load();}
 void notify(void(*wake)(const uint32_t*)){
  __atomic_fetch_add(&generation.value,1u,__ATOMIC_RELEASE);wake(&generation.value);
 }
 void wait(uint32_t observed,void(*park)(const uint32_t*,uint32_t)) const{
  if(sample()==observed)park(&generation.value,observed);
 }
};
}
