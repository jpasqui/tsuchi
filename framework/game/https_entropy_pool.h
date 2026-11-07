#pragma once
// How much randomness a run of timing measurements can be trusted for.
//
// The HTTPS seed is gathered inside the game: the time one fixed piece of work
// takes is measured thousands of times, and the small differences between
// those times (scheduling, caches, the host under an emulator) are the
// randomness. A clock that only counts emulated instructions gives the same
// time every round, or a repeating pattern; that must earn nothing. So a run
// is credited by how predictable it is, not by how many measurements it has:
//   - the share of the most common value, and
//   - how often a measurement repeats an earlier one (1 to 8 back) or
//     continues the last step,
// and the worse of the two decides. The credit is deliberately far below what
// either figure allows (at most half a bit per measurement).
#include <stdint.h>
namespace tsuchi::https::https_entropy {
struct Estimate {
    uint32_t samples=0;
    uint32_t most_common=0;   // measurements sharing the most common value
    uint32_t predicted=0;     // measurements the best simple predictor got right
    uint32_t credit_bits=0;
};
constexpr uint32_t buckets=1024,lags=8;
// counts: scratch for `buckets` values. A measurement is counted by its low
// bits (a round can take longer than `buckets` ticks under an emulator), which
// puts different values together and so can only make a run look more
// predictable; the repeat check compares whole values.
inline Estimate estimate(const uint16_t* deltas,uint32_t count,uint16_t* counts){
    Estimate out;out.samples=count;
    if(!deltas||!counts||count<64||count>65535)return out;
    for(uint32_t i=0;i<buckets;++i)counts[i]=0;
    for(uint32_t i=0;i<count;++i){const uint32_t v=deltas[i]&(buckets-1);if(++counts[v]>out.most_common)out.most_common=counts[v];}
    for(uint32_t lag=1;lag<=lags;++lag){uint32_t hits=0;for(uint32_t i=lag;i<count;++i)if(deltas[i]==deltas[i-lag])++hits;if(hits>out.predicted)out.predicted=hits;}
    {uint32_t hits=0;for(uint32_t i=2;i<count;++i)if(int32_t(deltas[i])-int32_t(deltas[i-1])==int32_t(deltas[i-1])-int32_t(deltas[i-2]))++hits;if(hits>out.predicted)out.predicted=hits;}
    const uint32_t worst=out.most_common>out.predicted?out.most_common:out.predicted;
    // Sixteenths of a bit per measurement.
    const uint32_t rate=worst*4<=count?8:worst*2<=count?4:worst*10<=count*9?1:0;
    out.credit_bits=count*rate/16;
    return out;
}
// A seed needs this much credit when timing is its only source.
constexpr uint32_t required_bits=256;
}
