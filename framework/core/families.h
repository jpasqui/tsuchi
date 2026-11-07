// Which API families the running build of the game cannot serve. game/families.yaml
// lists them, the generator works out which of them each build loses, and the build
// selected at start-up sets this once. Each generated call of a family that is gone
// returns TSUCHI_UNSUPPORTED before it reaches the family's backend, and its start-up
// does not run, so it installs no hook.
#pragma once
#include <stdint.h>

namespace tsuchi {

inline uint64_t families_absent = 0;
inline bool family_absent(unsigned family) { return (families_absent >> family) & 1; }

} // namespace tsuchi
