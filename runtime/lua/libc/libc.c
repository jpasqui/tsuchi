/* Freestanding C library subset for Lua 5.4 in the framework module.
 * See include/tsuchi_libc.h. Formatting is exact (big-integer digit generation);
 * strtod rounds every decimal number correctly, to the nearest double and ties to
 * even; math functions follow fdlibm's reductions and
 * polynomials. The embedding module supplies the tsuchi_libc_* hooks. */
#include "include/tsuchi_libc.h"

int errno;
static struct lconv tsuchi_lconv = {"."};
struct lconv *localeconv(void) { return &tsuchi_lconv; }
char *getenv(const char *name) { (void)name; return 0; }
char *strerror(int e) { (void)e; return (char *)"error"; }
void abort(void) { __builtin_trap(); }
int abs(int v) { return v < 0 ? -v : v; }
time_t time(time_t *t) { time_t v = (time_t)(tsuchi_libc_ticks() / 1000000ull); if (t) *t = v; return v; }
clock_t clock(void) { return (clock_t)tsuchi_libc_ticks(); }

/* ---- memory and strings ---- */
__attribute__((weak)) void *memset(void *d, int v, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)v; return d; }
__attribute__((weak)) void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
__attribute__((weak)) void *memmove(void *d, const void *s, size_t n) {
    unsigned char *p = d; const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++; else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
__attribute__((weak)) int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    for (; n; --n, ++p, ++q) if (*p != *q) return *p < *q ? -1 : 1;
    return 0;
}
void *memchr(const void *s, int c, size_t n) { const unsigned char *p = s; for (; n; --n, ++p) if (*p == (unsigned char)c) return (void *)p; return 0; }
__attribute__((weak)) size_t strlen(const char *s) { const char *p = s; while (*p) ++p; return (size_t)(p - s); }
__attribute__((weak)) int strcmp(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) { for (; n; --n, ++a, ++b) { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; if (!*a) return 0; } return 0; }
int strcoll(const char *a, const char *b) { return strcmp(a, b); }
char *strchr(const char *s, int c) { for (;; ++s) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
char *strpbrk(const char *s, const char *set) { for (; *s; ++s) if (strchr(set, *s)) return (char *)s; return 0; }
size_t strspn(const char *s, const char *set) { size_t n = 0; while (s[n] && strchr(set, s[n])) ++n; return n; }
char *strstr(const char *h, const char *n) {
    size_t k = strlen(n); if (!k) return (char *)h;
    for (; *h; ++h) if (*h == *n && !strncmp(h, n, k)) return (char *)h;
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }

/* ---- ctype (ASCII) ---- */
int isdigit(int c) { return c >= '0' && c <= '9'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isalpha(int c) { return islower(c) || isupper(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }

/* ---- heap ---- */
void *malloc(size_t n) { return tsuchi_libc_allocate(n ? n : 1); }
void *realloc(void *p, size_t n) { return tsuchi_libc_reallocate(p, n); }
void free(void *p) { if (p) tsuchi_libc_release(p); }

/* ---- files: none. Output to stdout/stderr goes to the host's log. ---- */
struct tsuchi_FILE { int id; };
static FILE tsuchi_streams[3] = {{0}, {1}, {2}};
FILE *stdin = &tsuchi_streams[0], *stdout = &tsuchi_streams[1], *stderr = &tsuchi_streams[2];
FILE *fopen(const char *p, const char *m) { (void)p; (void)m; errno = EINVAL; return 0; }
FILE *freopen(const char *p, const char *m, FILE *f) { (void)p; (void)m; (void)f; return 0; }
int fclose(FILE *f) { (void)f; return 0; }
int ferror(FILE *f) { (void)f; return 0; }
int feof(FILE *f) { (void)f; return 1; }
int getc(FILE *f) { (void)f; return EOF; }
int ungetc(int c, FILE *f) { (void)c; (void)f; return EOF; }
size_t fread(void *b, size_t s, size_t n, FILE *f) { (void)b; (void)s; (void)n; (void)f; return 0; }
int fflush(FILE *f) { (void)f; return 0; }
size_t fwrite(const void *b, size_t s, size_t n, FILE *f) {
    if (f == stdout || f == stderr) tsuchi_libc_write((const char *)b, s * n);
    return n;
}

/* ---- exact decimal digits of a double ---- */
typedef struct { uint32_t limb[130]; int n; } big; /* base 1e9, little-endian */
static void big_mul_small(big *b, uint32_t m) {
    uint64_t carry = 0;
    for (int i = 0; i < b->n; ++i) { uint64_t v = (uint64_t)b->limb[i] * m + carry; b->limb[i] = (uint32_t)(v % 1000000000u); carry = v / 1000000000u; }
    while (carry) { b->limb[b->n++] = (uint32_t)(carry % 1000000000u); carry /= 1000000000u; }
}
/* digits: significant decimal digits of |v| (no leading zeros); returns count;
 * *point: the value is 0.<digits> * 10^point. v must be finite and nonzero. */
static int exact_digits(double v, char *digits, int *point) {
    union { double d; uint64_t u; } x = {v};
    int e = (int)((x.u >> 52) & 0x7FF); uint64_t m = x.u & 0xFFFFFFFFFFFFFull;
    if (e) m |= 1ull << 52; else e = 1;
    e -= 1075; /* v = m * 2^e */
    while (!(m & 1)) { m >>= 1; ++e; }
    big b; b.n = 0;
    b.limb[b.n++] = (uint32_t)(m % 1000000000u);
    if (m / 1000000000u) b.limb[b.n++] = (uint32_t)((m / 1000000000u) % 1000000000u);
    if (m / 1000000000000000000ull) b.limb[b.n++] = (uint32_t)(m / 1000000000000000000ull);
    int dexp = 0;
    if (e > 0) { while (e >= 29) { big_mul_small(&b, 1u << 29); e -= 29; } if (e) big_mul_small(&b, 1u << e); }
    else if (e < 0) { int k = -e; dexp = e; while (k >= 13) { big_mul_small(&b, 1220703125u); k -= 13; } while (k--) big_mul_small(&b, 5u); }
    int n = 0; char tmp[10];
    for (int i = b.n - 1; i >= 0; --i) {
        uint32_t limb = b.limb[i]; int k = 0;
        for (int j = 0; j < 9; ++j) { tmp[j] = (char)('0' + limb % 10); limb /= 10; }
        k = 9; if (i == b.n - 1) { while (k > 1 && tmp[k - 1] == '0') --k; }
        while (k) digits[n++] = tmp[--k];
    }
    int start = 0; while (start < n - 1 && digits[start] == '0') ++start;
    if (start) { for (int i = start; i < n; ++i) digits[i - start] = digits[i]; n -= start; }
    *point = n + dexp;
    while (n > 1 && digits[n - 1] == '0') --n;
    return n;
}
/* Rounds digits to keep `keep` digits (round half to even on the exact value). */
static int round_digits(char *d, int n, int keep, int *point) {
    if (keep < 0) { d[0] = '0'; return 0; }
    if (keep >= n) return n;
    int up = 0;
    if (d[keep] > '5') up = 1;
    else if (d[keep] == '5') {
        int rest = 0; for (int i = keep + 1; i < n; ++i) if (d[i] != '0') { rest = 1; break; }
        up = rest || (keep > 0 && ((d[keep - 1] - '0') & 1));
    }
    n = keep;
    if (up) {
        int i = keep - 1;
        while (i >= 0 && d[i] == '9') d[i--] = '0';
        if (i >= 0) d[i]++;
        else { for (int j = n; j > 0; --j) d[j] = d[j - 1]; d[0] = '1'; ++*point; ++n; if (!keep) n = 1; }
    }
    return n;
}

typedef struct { char *out; size_t cap, len; } sink;
static void put(sink *s, char c) { if (s->len + 1 < s->cap) s->out[s->len] = c; s->len++; }
static void puts_n(sink *s, const char *t, size_t n) { while (n--) put(s, *t++); }
static void pad(sink *s, char c, int n) { while (n-- > 0) put(s, c); }

enum { F_MINUS = 1, F_PLUS = 2, F_SPACE = 4, F_HASH = 8, F_ZERO = 16 };
static void emit(sink *s, int flags, int width, const char *prefix, const char *body, size_t n) {
    size_t p = strlen(prefix); int fill = width - (int)(p + n);
    if (!(flags & F_MINUS) && !(flags & F_ZERO)) pad(s, ' ', fill);
    puts_n(s, prefix, p);
    if (!(flags & F_MINUS) && (flags & F_ZERO)) pad(s, '0', fill);
    puts_n(s, body, n);
    if (flags & F_MINUS) pad(s, ' ', fill);
}

static void fmt_float(sink *s, double v, int flags, int width, int prec, char conv) {
    char prefix[4] = {0}; int pl = 0;
    union { double d; uint64_t u; } x = {v};
    if (x.u >> 63) { prefix[pl++] = '-'; v = -v; } else if (flags & F_PLUS) prefix[pl++] = '+'; else if (flags & F_SPACE) prefix[pl++] = ' ';
    const int upper = conv == 'E' || conv == 'G' || conv == 'F' || conv == 'A';
    char body[1200]; int bn = 0;
    if (v != v || v > DBL_MAX) {
        const char *t = v != v ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        emit(s, flags & ~F_ZERO, width, prefix, t, 3); return;
    }
    if (conv == 'a' || conv == 'A') {
        prefix[pl++] = '0'; prefix[pl++] = upper ? 'X' : 'x';
        x.d = v; int e = (int)((x.u >> 52) & 0x7FF); uint64_t m = x.u & 0xFFFFFFFFFFFFFull; int lead = 1;
        if (!e) { lead = m ? 0 : 0; e = m ? -1022 : 0; } else e -= 1023;
        const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        int nd = 13; if (prec < 0) { while (nd && !((m >> (4 * (13 - nd))) & 0xF)) --nd; }
        else if (prec < 13) { int drop = 4 * (13 - prec); uint64_t half = 1ull << (drop - 1), r = m & ((1ull << drop) - 1); m >>= drop; if (r > half || (r == half && (m & 1))) { m++; if (m >> (4 * prec)) { m = 0; lead++; } } m <<= drop; nd = prec; }
        body[bn++] = (char)('0' + lead);
        if (nd || (flags & F_HASH)) body[bn++] = '.';
        for (int i = 0; i < nd; ++i) body[bn++] = hex[(m >> (4 * (12 - i))) & 0xF];
        for (int i = nd; i < prec; ++i) body[bn++] = '0';
        body[bn++] = upper ? 'P' : 'p'; body[bn++] = e < 0 ? '-' : '+'; if (e < 0) e = -e;
        char t[8]; int tn = 0; do { t[tn++] = (char)('0' + e % 10); e /= 10; } while (e);
        while (tn) body[bn++] = t[--tn];
        emit(s, flags, width, prefix, body, (size_t)bn); return;
    }
    if (prec < 0) prec = 6;
    char d[800]; int point = 1, n = 0;
    if (v == 0) { d[0] = '0'; n = 1; point = 1; } else n = exact_digits(v, d, &point);
    char kind = conv | 32; /* e, f or g */
    int strip = 0;
    if (kind == 'g') {
        int P = prec ? prec : 1;
        char t[800]; for (int i = 0; i < n; ++i) t[i] = d[i];
        int tp = point, tn = v == 0 ? 1 : round_digits(t, n, P, &tp);
        (void)tn;
        int X = v == 0 ? 0 : tp - 1;
        if (P > X && X >= -4) { kind = 'f'; prec = P - 1 - X; } else { kind = 'e'; prec = P - 1; }
        strip = !(flags & F_HASH);
    }
    if (kind == 'e') {
        int exp10 = v == 0 ? 0 : point - 1;
        if (v != 0) { int p2 = point; n = round_digits(d, n, prec + 1, &p2); exp10 = p2 - 1; }
        body[bn++] = d[0];
        int frac = prec; char fr[800]; int fn = 0;
        for (int i = 1; i <= frac; ++i) fr[fn++] = i < n ? d[i] : '0';
        if (strip) while (fn && fr[fn - 1] == '0') --fn;
        if (fn || (flags & F_HASH)) body[bn++] = '.';
        for (int i = 0; i < fn; ++i) body[bn++] = fr[i];
        body[bn++] = upper ? 'E' : 'e'; body[bn++] = exp10 < 0 ? '-' : '+'; if (exp10 < 0) exp10 = -exp10;
        if (exp10 < 10) body[bn++] = '0';
        char t[8]; int tn = 0; do { t[tn++] = (char)('0' + exp10 % 10); exp10 /= 10; } while (exp10);
        while (tn) body[bn++] = t[--tn];
    } else {
        /* digits up to 10^-prec: keep point + prec digits */
        if (v != 0) n = round_digits(d, n, point + prec, &point);
        if (v != 0 && n == 0) { n = 0; }
        if (point <= 0) body[bn++] = '0';
        else for (int i = 0; i < point; ++i) body[bn++] = i < n ? d[i] : '0';
        char fr[800]; int fn = 0;
        for (int i = 0; i < prec; ++i) { int idx = point + i; fr[fn++] = (idx >= 0 && idx < n) ? d[idx] : '0'; }
        if (strip) while (fn && fr[fn - 1] == '0') --fn;
        if (fn || (flags & F_HASH)) body[bn++] = '.';
        for (int i = 0; i < fn && bn < (int)sizeof body; ++i) body[bn++] = fr[i];
    }
    emit(s, flags, width, prefix, body, (size_t)bn);
}

static void fmt_int(sink *s, uint64_t u, int negative, int flags, int width, int prec, int base, int upper) {
    char prefix[4] = {0}; int pl = 0;
    if (negative) prefix[pl++] = '-'; else if (flags & F_PLUS) prefix[pl++] = '+'; else if (flags & F_SPACE) prefix[pl++] = ' ';
    char t[70]; int n = 0; const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (u) { t[n++] = digs[u % (unsigned)base]; u /= (unsigned)base; }
    if (prec < 0) prec = 1; else flags &= ~F_ZERO;
    char body[140]; int bn = 0;
    if ((flags & F_HASH) && base == 8 && prec <= n) prec = n + 1;
    if ((flags & F_HASH) && base == 16 && n) { prefix[pl++] = '0'; prefix[pl++] = upper ? 'X' : 'x'; }
    for (int i = n; i < prec; ++i) body[bn++] = '0';
    while (n) body[bn++] = t[--n];
    emit(s, flags, width, prefix, body, (size_t)bn);
}

int vsnprintf(char *out, size_t cap, const char *f, va_list ap) {
    sink s = {out, cap, 0};
    for (; *f; ++f) {
        if (*f != '%') { put(&s, *f); continue; }
        ++f; int flags = 0;
        for (;; ++f) {
            if (*f == '-') flags |= F_MINUS; else if (*f == '+') flags |= F_PLUS; else if (*f == ' ') flags |= F_SPACE;
            else if (*f == '#') flags |= F_HASH; else if (*f == '0') flags |= F_ZERO; else break;
        }
        int width = 0, prec = -1;
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { flags |= F_MINUS; width = -width; } ++f; }
        else while (isdigit(*f)) width = width * 10 + (*f++ - '0');
        if (*f == '.') { ++f; prec = 0; if (*f == '*') { prec = va_arg(ap, int); ++f; } else while (isdigit(*f)) prec = prec * 10 + (*f++ - '0'); }
        int length = 0; /* 0 int, 1 long, 2 long long, 3 size_t, -1 short */
        for (;; ++f) {
            if (*f == 'l') length = length ? 2 : 1; else if (*f == 'z' || *f == 'j' || *f == 't') length = 3;
            else if (*f == 'h') length = -1; else if (*f == 'L') length = 4; else break;
        }
        const char c = *f; if (!c) break;
        switch (c) {
        case 'd': case 'i': {
            long long v = length == 2 || length == 1 || length == 3 ? va_arg(ap, long long) : va_arg(ap, int);
            fmt_int(&s, v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, v < 0, flags, width, prec, 10, 0); break; }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = length == 2 || length == 1 || length == 3 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            fmt_int(&s, v, 0, flags & ~(F_PLUS | F_SPACE), width, prec, c == 'u' ? 10 : c == 'o' ? 8 : 16, c == 'X'); break; }
        case 'p': { uintptr_t v = (uintptr_t)va_arg(ap, void *); fmt_int(&s, v, 0, flags | F_HASH, width, prec, 16, 0); break; }
        case 'c': { char ch = (char)va_arg(ap, int); emit(&s, flags & ~F_ZERO, width, "", &ch, 1); break; }
        case 's': { const char *t = va_arg(ap, const char *); if (!t) t = "(null)"; size_t n = 0; while (t[n] && (prec < 0 || n < (size_t)prec)) ++n; emit(&s, flags & ~F_ZERO, width, "", t, n); break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            fmt_float(&s, va_arg(ap, double), flags, width, prec, c); break;
        case '%': put(&s, '%'); break;
        default: put(&s, '%'); put(&s, c); break;
        }
    }
    if (cap) out[s.len < cap ? s.len : cap - 1] = 0;
    return (int)s.len;
}
int snprintf(char *o, size_t n, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(o, n, f, ap); va_end(ap); return r; }
int sprintf(char *o, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(o, (size_t)-1 >> 1, f, ap); va_end(ap); return r; }
static int vwrite(const char *f, va_list ap) { char b[512]; int r = vsnprintf(b, sizeof b, f, ap); tsuchi_libc_write(b, r < (int)sizeof b ? (size_t)r : sizeof b - 1); return r; }
int printf(const char *f, ...) { va_list ap; va_start(ap, f); int r = vwrite(f, ap); va_end(ap); return r; }
int fprintf(FILE *o, const char *f, ...) { va_list ap; va_start(ap, f); int r = o == stdout || o == stderr ? vwrite(f, ap) : 0; va_end(ap); return r; }

/* ---- strtod ---- */
static const double tsuchi_pow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                   1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
/* Unsigned integers of up to 320 32-bit words, for comparing a decimal number with a double exactly. */
typedef struct { uint32_t w[320]; int n; } wide_t;
static void wide_mul_small(wide_t *b, uint32_t m, uint32_t add) {
    uint64_t carry = add;
    for (int i = 0; i < b->n; ++i) { uint64_t v = (uint64_t)b->w[i] * m + carry; b->w[i] = (uint32_t)v; carry = v >> 32; }
    if (carry) b->w[b->n++] = (uint32_t)carry;
}
static void wide_mul_pow5(wide_t *b, int k) {
    for (; k >= 13; k -= 13) wide_mul_small(b, 1220703125u, 0); /* 5^13 */
    uint32_t rest = 1; while (k--) rest *= 5;
    wide_mul_small(b, rest, 0);
}
static void wide_shl(wide_t *b, int bits) {
    const int words = bits / 32, shift = bits % 32;
    if (!b->n) return;
    if (shift) {
        uint32_t carry = 0;
        for (int i = 0; i < b->n; ++i) { const uint32_t v = b->w[i]; b->w[i] = v << shift | carry; carry = v >> (32 - shift); }
        if (carry) b->w[b->n++] = carry;
    }
    if (words) { for (int i = b->n - 1; i >= 0; --i) b->w[i + words] = b->w[i]; for (int i = 0; i < words; ++i) b->w[i] = 0; b->n += words; }
}
static int wide_cmp(const wide_t *a, const wide_t *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; --i) if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}
static uint64_t dbits(double d) { union { double d; uint64_t u; } x = {d}; return x.u; }
static double dfrom(uint64_t u) { union { uint64_t u; double d; } x = {u}; return x.d; }
/* A positive double, or infinity as 2^1024, as m * 2^e. */
static void dparts(double x, uint64_t *m, int *e) {
    const uint64_t u = dbits(x); const int be = (int)(u >> 52); const uint64_t f = u & ((1ull << 52) - 1);
    if (be == 0) { *m = f; *e = -1074; } else { *m = f | (1ull << 52); *e = be - 1075; }
}
/* Compares digits * 10^exp10 with the point halfway between the adjacent doubles a < b. */
static int cmp_half(const char *digits, int count, int exp10, double a, double b) {
    uint64_t ma, mb; int ea, eb; dparts(a, &ma, &ea); dparts(b, &mb, &eb);
    const int low = ea < eb ? ea : eb;
    const uint64_t sum = (ma << (ea - low)) + (mb << (eb - low)); /* the midpoint is sum * 2^(low - 1) */
    wide_t l, r; /* about 1.3 KB each */
    l.n = 0; for (int i = 0; i < count; ++i) { if (!l.n) { if (digits[i] != '0') { l.w[0] = (uint32_t)(digits[i] - '0'); l.n = 1; } } else wide_mul_small(&l, 10, (uint32_t)(digits[i] - '0')); }
    r.n = 0; if (sum) { r.w[0] = (uint32_t)sum; r.w[1] = (uint32_t)(sum >> 32); r.n = r.w[1] ? 2 : 1; }
    if (exp10 >= 0) wide_mul_pow5(&l, exp10); else wide_mul_pow5(&r, -exp10);
    const int shift = exp10 - (low - 1);
    if (shift >= 0) wide_shl(&l, shift); else wide_shl(&r, -shift);
    return wide_cmp(&l, &r);
}
double strtod(const char *s, char **end) {
    const char *p = s; while (isspace(*p)) ++p;
    int neg = 0; if (*p == '+' || *p == '-') neg = *p++ == '-';
    /* The significant digits, as many as decide the rounding of any double. A digit that is not zero past them
       is kept as a 1, which places the number strictly between its neighbours as the dropped digits do. */
    enum { kMostDigits = 800 };
    char digits[kMostDigits + 1]; int count = 0, exp10 = 0, any = 0, dropped = 0;
    for (; isdigit(*p); ++p) {
        any = 1;
        if (count < kMostDigits) { if (count || *p != '0') digits[count++] = *p; }
        else { ++exp10; dropped = dropped || *p != '0'; }
    }
    if (*p == '.') {
        ++p;
        for (; isdigit(*p); ++p) {
            any = 1;
            if (count < kMostDigits) { if (count || *p != '0') digits[count++] = *p; --exp10; }
            else dropped = dropped || *p != '0';
        }
    }
    if (!any) { if (end) *end = (char *)s; return 0; }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1; int en = 0, ev = 0;
        if (*q == '+' || *q == '-') en = *q++ == '-';
        if (isdigit(*q)) { while (isdigit(*q)) { if (ev < 100000) ev = ev * 10 + (*q - '0'); ++q; } exp10 += en ? -ev : ev; p = q; }
    }
    if (end) *end = (char *)p;
    if (dropped) { digits[count++] = '1'; --exp10; }
    else while (count && digits[count - 1] == '0') { --count; ++exp10; }
    if (!count) return neg ? -0.0 : 0.0;
    double v;
    if (count + exp10 > 310) v = HUGE_VAL;     /* at least 10^310 */
    else if (count + exp10 < -324) v = 0;      /* below 10^-324, under half the least double */
    else {
        /* A first guess from the leading 19 digits: exact for a short number of moderate size, otherwise a
           few units in the last place away, which the comparisons below put right. */
        uint64_t mant = 0; const int lead = count < 19 ? count : 19;
        for (int i = 0; i < lead; ++i) mant = mant * 10 + (uint64_t)(digits[i] - '0');
        int e = exp10 + (count - lead);
        v = (double)mant;
        const int exact = count <= 19 && mant < (1ull << 53) && e >= -22 && e <= 22;
        if (exact) v = e >= 0 ? v * tsuchi_pow10[e] : v / tsuchi_pow10[-e];
        else {
            while (e > 22 && v <= DBL_MAX) { v *= 1e22; e -= 22; }
            while (e < -22 && v != 0) { v /= 1e22; e += 22; }
            if (e > 22 || v > DBL_MAX) v = DBL_MAX;
            else if (e < -22) v = 0;
            else v = e >= 0 ? v * tsuchi_pow10[e] : v / tsuchi_pow10[-e];
            if (v > DBL_MAX) v = DBL_MAX;
            /* Each step moves to the neighbour that is nearer, until the number lies within half a unit of
               v, and a number exactly halfway goes to the even one. */
            for (int steps = 0; steps < 4096; ++steps) {
                if (v > DBL_MAX) break;
                const double up = dfrom(dbits(v) + 1);
                const int above = cmp_half(digits, count, exp10, v, up);
                if (above > 0 || (above == 0 && (dbits(v) & 1))) { v = up; continue; }
                if (v == 0) break;
                const double down = dfrom(dbits(v) - 1);
                const int below = cmp_half(digits, count, exp10, down, v);
                if (below < 0 || (below == 0 && (dbits(v) & 1))) { v = down; continue; }
                break;
            }
        }
    }
    if (v > DBL_MAX || v == 0) errno = ERANGE;
    return neg ? -v : v;
}
float strtof(const char *s, char **e) { return (float)strtod(s, e); }

/* ---- math ---- */
static inline uint64_t bits(double d) { union { double d; uint64_t u; } x = {d}; return x.u; }
static inline double from_bits(uint64_t u) { union { uint64_t u; double d; } x = {u}; return x.d; }
#if defined(__aarch64__)
double floor(double x) { double r; __asm__("frintm %d0, %d1" : "=w"(r) : "w"(x)); return r; }
double ceil(double x) { double r; __asm__("frintp %d0, %d1" : "=w"(r) : "w"(x)); return r; }
double fabs(double x) { double r; __asm__("fabs %d0, %d1" : "=w"(r) : "w"(x)); return r; }
double sqrt(double x) { double r; __asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x)); return r; }
#else
double fabs(double x) { return from_bits(bits(x) & ~(1ull << 63)); }
static double trunc_(double x) {
    uint64_t u = bits(x); int e = (int)((u >> 52) & 0x7FF) - 1023;
    if (e >= 52) return x; if (e < 0) return from_bits(u & (1ull << 63));
    return from_bits(u & ~((1ull << (52 - e)) - 1));
}
double floor(double x) { double t = trunc_(x); return t > x ? t - 1 : t; }
double ceil(double x) { double t = trunc_(x); return t < x ? t + 1 : t; }
double sqrt(double x) {
    if (x <= 0 || x != x || x > DBL_MAX) return x == 0 ? x : x > 0 ? x : (x - x) / (x - x);
    double r = x > 1 ? x : 1; for (int i = 0; i < 64; ++i) { double n = 0.5 * (r + x / r); if (n == r) break; r = n; }
    return r;
}
#endif
float floorf(float x) { return (float)floor(x); }

double ldexp(double x, int e) {
    if (x == 0 || x != x || x > DBL_MAX || x < -DBL_MAX) return x;
    while (e > 1000) { x *= 0x1p1000; e -= 1000; if (x > DBL_MAX || x < -DBL_MAX) return x; }
    while (e < -1000) { x *= 0x1p-1000; e += 1000; if (x == 0) return x; }
    if (e > 0) { while (e > 60) { x *= 0x1p60; e -= 60; } return x * (double)(1ull << e); }
    while (e < -60) { x *= 0x1p-60; e += 60; }
    return e ? x / (double)(1ull << -e) : x;
}
double frexp(double x, int *e) {
    uint64_t u = bits(x); int ee = (int)((u >> 52) & 0x7FF);
    if (!ee) { if (x == 0) { *e = 0; return x; } x *= 0x1p64; u = bits(x); ee = (int)((u >> 52) & 0x7FF) - 64; }
    else if (ee == 0x7FF) { *e = 0; return x; }
    *e = ee - 1022;
    return from_bits((u & ~(0x7FFull << 52)) | (1022ull << 52));
}
double fmod(double x, double y) {
    if (y == 0 || x != x || y != y || x > DBL_MAX || x < -DBL_MAX) return (x * y) / (x * y);
    if (y > DBL_MAX || y < -DBL_MAX) return x;
    double ax = fabs(x), ay = fabs(y);
    if (ax < ay) return x;
    /* Exact long division on the binary expansion. */
    int ex, ey; double mx = frexp(ax, &ex), my = frexp(ay, &ey);
    uint64_t ix = (uint64_t)ldexp(mx, 53), iy = (uint64_t)ldexp(my, 53); int n = ex - ey;
    while (n-- > 0) { if (ix >= iy) ix -= iy; ix <<= 1; }
    if (ix >= iy) ix -= iy;
    double r = ldexp((double)ix, ey - 53);
    return x < 0 ? -r : r;
}
float fmodf(float x, float y) { return (float)fmod(x, y); }

static const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10, inv_ln2 = 1.44269504088896338700e+00;
double exp(double x) {
    if (x != x) return x;
    if (x > 709.782712893383973096) return HUGE_VAL;
    if (x < -745.13321910194110842) return 0;
    double k = floor(x * inv_ln2 + 0.5), hi = x - k * ln2_hi, lo = k * ln2_lo, r = hi - lo, t = r * r;
    double c = r - t * (1.66666666666666019037e-01 + t * (-2.77777777770155933842e-03 + t * (6.61375632143793436117e-05 +
               t * (-1.65339022054652515390e-06 + t * 4.13813679705723846039e-08))));
    double y = 1 - ((lo - (r * c) / (2 - c)) - hi);
    return ldexp(y, (int)k);
}
double log(double x) {
    if (x != x || x < 0) return (x - x) / (x - x) + (x != x ? x : 0);
    if (x == 0) return -HUGE_VAL;
    if (x > DBL_MAX) return x;
    int k; double m = frexp(x, &k); /* x = m*2^k, 0.5 <= m < 1 */
    if (m < 0.70710678118654752440) { m *= 2; --k; }
    double f = m - 1, s = f / (2 + f), z = s * s, w = z * z;
    double t1 = w * (3.999999999940941908e-01 + w * (2.222219843214978396e-01 + w * 1.531383769920937332e-01));
    double t2 = z * (6.666666666666735130e-01 + w * (2.857142874366239149e-01 + w * (1.818357216161805012e-01 + w * 1.479819860511658591e-01)));
    double R = t2 + t1, hfsq = 0.5 * f * f;
    return k * ln2_hi - ((hfsq - (s * (hfsq + R) + k * ln2_lo)) - f);
}
double log2(double x) { return log(x) * inv_ln2; }
double log10(double x) { return log(x) * 4.34294481903251827651e-01; }
static int is_integer(double y) { return y == floor(y) && fabs(y) < 0x1p63; }
double pow(double x, double y) {
    if (y == 0) return 1;
    if (x != x || y != y) return x + y;
    if (is_integer(y) && fabs(y) <= 0x1p31) {
        long long n = (long long)y; int neg = n < 0; if (neg) n = -n;
        double r = 1, b = x; while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        return neg ? 1 / r : r;
    }
    if (x == 0) return y < 0 ? HUGE_VAL : 0;
    if (x < 0) { if (!is_integer(y)) return (x - x) / (x - x); double r = exp(y * log(-x)); return fmod(y, 2) != 0 ? -r : r; }
    return exp(y * log(x));
}
float powf(float x, float y) { return (float)pow(x, y); }

static const double pio2_1 = 1.57079632673412561417e+00, pio2_1t = 6.07710050650619224932e-11,
                    pio2_2 = 6.07710050630396597660e-11, pio2_2t = 2.02226624879595063154e-21,
                    pio2_3 = 2.02226624871116645580e-21, pio2_3t = 8.47842766036889956997e-32, inv_pio2 = 6.36619772367581382433e-01;
static double k_sin(double x, double y) {
    double z = x * x, v = z * x;
    double r = 8.33333333332248946124e-03 + z * (-1.98412698298579493134e-04 + z * (2.75573137070700676789e-06 + z * (-2.50507602534068634195e-08 + z * 1.58969099521155010221e-10)));
    return x - ((z * (0.5 * y - v * r) - y) - v * -1.66666666666666324348e-01);
}
static double k_cos(double x, double y) {
    double z = x * x, w = z * z;
    double r = z * (4.16666666666666019037e-02 + z * (-1.38888888888741095749e-03 + z * 2.48015872894767294178e-05)) +
               w * w * (-2.75573143513906633035e-07 + z * (2.08757232129817482790e-09 + z * -1.13596475577881948265e-11));
    double hz = 0.5 * z, one_hz = 1 - hz;
    return one_hz + (((1 - one_hz) - hz) + (z * r - x * y));
}
/* x = n*pi/2 + (y0 + y1); accurate for |x| up to about 2^20 * pi/2. */
static int rem_pio2(double x, double *y0, double *y1) {
    double n = floor(x * inv_pio2 + 0.5);
    double r = x - n * pio2_1, w = n * pio2_1t, y = r - w;
    double t = r; w = n * pio2_2; r = t - w; w = n * pio2_2t - ((t - r) - w); y = r - w;
    t = r; w = n * pio2_3; r = t - w; w = n * pio2_3t - ((t - r) - w); y = r - w;
    *y0 = y; *y1 = (r - y) - w;
    return (int)((long long)n & 3);
}
/* Where rem_pio2 stops being accurate. */
static const double large_trig = 0x1p20 * 1.57079632679489661923;
/* 4/pi = sum of four_over_pi[i] * 2^(-64 * i). */
static const uint64_t four_over_pi[20] = {
    0x0000000000000001, 0x45f306dc9c882a53, 0xf84eafa3ea69bb81, 0xb6c52b3278872083, 0xfca2c757bd778ac3, 0x6e48dc74849ba5c0, 0x0c925dd413a32439,
    0xfc3bd63962534e7d, 0xd1046bea5d768909, 0xd338e04d68befc82, 0x7323ac7306a673e9, 0x3908bf177bf25076, 0x3ff12fffbc0b301f, 0xde5e2316b414da3e,
    0xda6cfd9e4f96136e, 0x9e8c7ecd3cbfd45a, 0xea4f758fd7cbe2f6, 0x7a0e73ef14a525d4, 0xd7f6bf623f1aba10, 0xac06608df8f6d757};
static uint64_t shl64(uint64_t a, unsigned n) { return n >= 64 ? 0 : a << n; }
static uint64_t shr64(uint64_t a, unsigned n) { return n >= 64 ? 0 : a >> n; }
/* x = q*pi/2 + z, |z| <= pi/4, for a finite x of at least large_trig (Payne and Hanek's reduction, with as many
   bits of 4/pi as the largest double needs). Returns q & 3. */
static int rem_pio2_large(double x, double *z) {
    union { double d; uint64_t u; } v = {x};
    const int exp = (int)((v.u >> 52) & 0x7FF) - 1023 - 52;
    const uint64_t ix = (v.u & 0xFFFFFFFFFFFFFull) | (1ull << 52);
    /* The three words of 4/pi whose product with x has its leading digit at 2^-61. */
    const unsigned digit = (unsigned)(exp + 61) / 64, shift = (unsigned)(exp + 61) % 64;
    const uint64_t z0 = shl64(four_over_pi[digit], shift) | shr64(four_over_pi[digit + 1], 64 - shift);
    const uint64_t z1 = shl64(four_over_pi[digit + 1], shift) | shr64(four_over_pi[digit + 2], 64 - shift);
    const uint64_t z2 = shl64(four_over_pi[digit + 2], shift) | shr64(four_over_pi[digit + 3], 64 - shift);
    const unsigned __int128 p1 = (unsigned __int128)z1 * ix, p2 = (unsigned __int128)z2 * ix;
    const uint64_t z1lo = (uint64_t)p1, lo = z1lo + (uint64_t)(p2 >> 64);
    uint64_t hi = z0 * ix + (uint64_t)(p1 >> 64) + (lo < z1lo);
    /* The top three bits are the octant; the rest is the fraction of an eighth of a turn. */
    uint64_t j = hi >> 61;
    hi = hi << 3 | lo >> 61;
    double f = 0;
    if (hi) {
        const unsigned lz = (unsigned)__builtin_clzll(hi);
        union { uint64_t u; double d; } fraction;
        fraction.u = ((shl64(hi, lz + 1) | shr64(lo, 64 - (lz + 1))) >> 12) | ((uint64_t)(1023 - (lz + 1)) << 52);
        f = fraction.d;
    }
    if (j & 1) { j = (j + 1) & 7; f -= 1; }
    *z = f * 7.85398163397448278999e-01;
    return (int)(j >> 1);
}
double sin(double x) {
    if (x != x || x > DBL_MAX || x < -DBL_MAX) return x - x;
    if (fabs(x) < 7.85398163397448278999e-01) return k_sin(x, 0);
    if (fabs(x) >= large_trig) {
        double z; const int q = rem_pio2_large(fabs(x), &z);
        const double s = q == 0 ? k_sin(z, 0) : q == 1 ? k_cos(z, 0) : q == 2 ? -k_sin(z, 0) : -k_cos(z, 0);
        return x < 0 ? -s : s;
    }
    double a, b; switch (rem_pio2(x, &a, &b)) { case 0: return k_sin(a, b); case 1: return k_cos(a, b); case 2: return -k_sin(a, b); default: return -k_cos(a, b); }
}
double cos(double x) {
    if (x != x || x > DBL_MAX || x < -DBL_MAX) return x - x;
    if (fabs(x) < 7.85398163397448278999e-01) return k_cos(x, 0);
    if (fabs(x) >= large_trig) {
        double z; const int q = rem_pio2_large(fabs(x), &z);
        return q == 0 ? k_cos(z, 0) : q == 1 ? -k_sin(z, 0) : q == 2 ? -k_cos(z, 0) : k_sin(z, 0);
    }
    double a, b; switch (rem_pio2(x, &a, &b)) { case 0: return k_cos(a, b); case 1: return -k_sin(a, b); case 2: return -k_cos(a, b); default: return k_sin(a, b); }
}
double tan(double x) { return sin(x) / cos(x); }

static const double atanhi[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01, 1.57079632679489655800e+00};
static const double atanlo[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17, 6.12323399573676603587e-17};
static const double aT[] = {3.33333333333329318027e-01, -1.99999999998764832476e-01, 1.42857142725034663711e-01, -1.11111104054623557880e-01,
                            9.09088713343650656196e-02, -7.69187620504482999495e-02, 6.66107313738753120669e-02, -5.83357013379057348645e-02,
                            4.97687799461593236017e-02, -3.65315727442169155270e-02, 1.62858201153657823623e-02};
double atan(double x) {
    if (x != x) return x;
    int neg = x < 0; double ax = fabs(x); int id;
    if (ax > 0x1p66) return neg ? -atanhi[3] - atanlo[3] : atanhi[3] + atanlo[3];
    if (ax < 0.4375) { if (ax < 0x1p-27) return x; id = -1; }
    else if (ax < 1.1875) { if (ax < 0.6875) { id = 0; ax = (2 * ax - 1) / (2 + ax); } else { id = 1; ax = (ax - 1) / (ax + 1); } }
    else if (ax < 2.4375) { id = 2; ax = (ax - 1.5) / (1 + 1.5 * ax); }
    else { id = 3; ax = -1 / ax; }
    double z = ax * ax, w = z * z;
    double s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    double s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0) return x - x * (s1 + s2);
    double r = atanhi[id] - ((ax * (s1 + s2) - atanlo[id]) - ax);
    return neg ? -r : r;
}
static const double pi = 3.1415926535897931160e+00, pi_lo = 1.2246467991473531772e-16;
double atan2(double y, double x) {
    if (x != x || y != y) return x + y;
    if (x == 1.0) return atan(y);
    int ys = (bits(y) >> 63) != 0, xs = (bits(x) >> 63) != 0;
    if (y == 0) return xs ? (ys ? -pi : pi) : y;
    if (x == 0) return ys ? -pi / 2 : pi / 2;
    if (x > DBL_MAX || x < -DBL_MAX) {
        if (y > DBL_MAX || y < -DBL_MAX) return xs ? (ys ? -3 * pi / 4 : 3 * pi / 4) : (ys ? -pi / 4 : pi / 4);
        return xs ? (ys ? -pi : pi) : (ys ? -0.0 : 0.0);
    }
    if (y > DBL_MAX || y < -DBL_MAX) return ys ? -pi / 2 : pi / 2;
    double z = atan(fabs(y / x));
    if (!xs) return ys ? -z : z;
    z = pi - (z - pi_lo);
    return ys ? -z : z;
}
double asin(double x) { if (x > 1 || x < -1) return (x - x) / (x - x); return atan2(x, sqrt((1 - x) * (1 + x))); }
double acos(double x) { if (x > 1 || x < -1) return (x - x) / (x - x); return atan2(sqrt((1 - x) * (1 + x)), x); }

/* QuickJS's additional C-library calls. Time zones are UTC on the target. */
char *strrchr(const char *s, int c) {
    const char *last = 0;
    do { if (*s == (char)c) last = s; } while (*s++);
    return (char *)last;
}
int fputc(int c, FILE *stream) { unsigned char b = (unsigned char)c; return fwrite(&b, 1, 1, stream) == 1 ? b : EOF; }
int gettimeofday(struct timeval *v, void *zone) {
    (void)zone;
    unsigned long long ticks = tsuchi_libc_ticks();
    v->tv_sec = (long long)(ticks / 1000000); v->tv_usec = (long)(ticks % 1000000);
    return 0;
}
struct tm *localtime_r(const time_t *t, struct tm *out) {
    /* QuickJS only reads tm_gmtoff; its calendar arithmetic is internal. */
    (void)t; memset(out, 0, sizeof *out); return out;
}
double copysign(double x, double y) {
    union { double d; uint64_t u; } a = {x}, b = {y};
    a.u = (a.u & 0x7fffffffffffffffull) | (b.u & 0x8000000000000000ull); return a.d;
}
double trunc(double x) { return copysign(floor(fabs(x)), x); }
double round(double x) {
    double a = fabs(x), f = floor(a); return copysign(f + (a - f >= 0.5), x);
}
double hypot(double x, double y) {
    x = fabs(x); y = fabs(y);
    if (isinf(x) || isinf(y)) return INFINITY;
    if (isnan(x) || isnan(y)) return NAN;
    if (x < y) { double t = x; x = y; y = t; }
    if (!x) return 0; y /= x; return x * sqrt(1 + y * y);
}
double expm1(double x) {
    if (fabs(x) > 0.5) return exp(x) - 1;
    double sum = x, term = x;
    for (int n = 2; n < 32; ++n) { term *= x / n; sum += term; }
    return sum;
}
double log1p(double x) {
    if (fabs(x) > 0.5) return log(1 + x);
    double z = x / (2 + x), term = z, sum = z, zz = z * z;
    for (int n = 3; n < 65; n += 2) { term *= zz; sum += term / n; }
    return 2 * sum;
}
double sinh(double x) {
    double a = fabs(x), e = expm1(a);
    if (a > 700) return copysign(exp(a - 0.6931471805599453), x);
    return copysign(0.5 * (e + e / (e + 1)), x);
}
double cosh(double x) { double a = fabs(x); if (a > 700) return exp(a - 0.6931471805599453); double e = exp(a); return 0.5 * (e + 1 / e); }
double tanh(double x) { if (fabs(x) > 20) return copysign(1, x); double e = expm1(2 * fabs(x)); return copysign(e / (e + 2), x); }
double asinh(double x) { double a = fabs(x); return copysign(a > 1e150 ? log(a) + 0.6931471805599453 : log1p(a + a * a / (1 + sqrt(1 + a * a))), x); }
double acosh(double x) { if (x < 1) return NAN; return x > 1e150 ? log(x) + 0.6931471805599453 : log1p((x - 1) + sqrt((x - 1) * (x + 1))); }
double atanh(double x) { return copysign(0.5 * log1p(2 * fabs(x) / (1 - fabs(x))), x); }
double cbrt(double x) {
    if (!isfinite(x) || !x) return x;
    int e; double a = frexp(fabs(x), &e), r = exp(log(a) / 3);
    int rem = e % 3; e /= 3;
    r *= rem == 1 ? 1.2599210498948732 : rem == 2 ? 1.5874010519681994 : rem == -1 ? 0.7937005259840998 : rem == -2 ? 0.6299605249474366 : 1;
    r = ldexp(r, e);
    r = (2 * r + (fabs(x) / r) / r) / 3;
    return copysign(r, x);
}

/* The module's libc allocation hook stores the requested size 16 bytes before the block. */
size_t malloc_usable_size(void *p) { return p ? ((uint64_t *)p)[-2] : 0; }
int putchar(int c) { return fputc(c, stdout); }
long lrint(double x) {
    double f = floor(x), d = x - f;
    return (long)(f + (d > 0.5 || (d == 0.5 && fmod(f, 2) != 0)));
}
double fmin(double x, double y) { if (isnan(x)) return y; if (isnan(y)) return x; return x < y || (x == y && signbit(x)) ? x : y; }
double fmax(double x, double y) { if (isnan(x)) return y; if (isnan(y)) return x; return x > y || (x == y && !signbit(x)) ? x : y; }
