#include <stdint.h>
#if __SIZEOF_LONG__ == 8
#define PRId64 "ld"
#define PRIu64 "lu"
#define PRIx64 "lx"
#define PRIX64 "lX"
#else
#define PRId64 "lld"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIX64 "llX"
#endif
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
