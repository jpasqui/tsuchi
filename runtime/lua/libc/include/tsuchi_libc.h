/* The C library subset Lua 5.4 needs, for the freestanding framework module
 * (libc.c). The standard header names in this directory all
 * include this file. Not a general C library: only what Lua's core, auxlib and
 * the base, coroutine, table, string, math and utf8 libraries call. */
#ifndef TSUCHI_LIBC_H
#define TSUCHI_LIBC_H
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <float.h>
#ifdef __cplusplus
extern "C" {
#endif

/* string.h */
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
int strcoll(const char *, const char *);
char *strchr(const char *, int);
char *strpbrk(const char *, const char *);
size_t strspn(const char *, const char *);
char *strstr(const char *, const char *);
char *strcpy(char *, const char *);
char *strerror(int);
char *strrchr(const char *, int);


/* stdlib.h */
void *malloc(size_t);
#define alloca __builtin_alloca
size_t malloc_usable_size(void *);
void *realloc(void *, size_t);
void free(void *);
void abort(void) __attribute__((noreturn));
int abs(int);
double strtod(const char *, char **);
float strtof(const char *, char **);
char *getenv(const char *);
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* stdio.h: formatting is real; files are absent (fopen always fails). */
typedef struct tsuchi_FILE FILE;
extern FILE *stdin, *stdout, *stderr;
#define EOF (-1)
#define BUFSIZ 512
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int sprintf(char *, const char *, ...);
int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
size_t fwrite(const void *, size_t, size_t, FILE *);
size_t fread(void *, size_t, size_t, FILE *);
int fflush(FILE *);
int fputc(int, FILE *);
int putchar(int);
FILE *fopen(const char *, const char *);
FILE *freopen(const char *, const char *, FILE *);
int fclose(FILE *);
int ferror(FILE *);
int feof(FILE *);
int getc(FILE *);
int ungetc(int, FILE *);

/* ctype.h (ASCII) */
int isalnum(int); int isalpha(int); int iscntrl(int); int isdigit(int); int isgraph(int);
int islower(int); int isprint(int); int ispunct(int); int isspace(int); int isupper(int);
int isxdigit(int); int tolower(int); int toupper(int);

/* math.h */
#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
double floor(double); double ceil(double); double fabs(double); double sqrt(double);
double fmod(double, double); double pow(double, double); double exp(double);
double log(double); double log2(double); double log10(double);
double sin(double); double cos(double); double tan(double);
double asin(double); double acos(double); double atan(double); double atan2(double, double);
double ldexp(double, int); double frexp(double, int *);
#define isnan(x) __builtin_isnan(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#define isinf(x) __builtin_isinf(x)
long lrint(double);
double fmin(double, double); double fmax(double, double);
double trunc(double); double round(double); double copysign(double, double);
double hypot(double, double); double cbrt(double);
double sinh(double); double cosh(double); double tanh(double);
double asinh(double); double acosh(double); double atanh(double);
double expm1(double); double log1p(double);
float floorf(float); float fmodf(float, float); float powf(float, float);

/* setjmp.h */
#ifndef TSUCHI_LIBC_HOST
/* callee-saved registers x19..x30, sp, d8..d15 */
typedef unsigned long long jmp_buf[22];
int setjmp(jmp_buf) __attribute__((returns_twice));
void longjmp(jmp_buf, int) __attribute__((noreturn));
#define _setjmp setjmp
#define _longjmp longjmp
#else
/* On another machine, for the checks: that machine's own, with room for whatever it keeps. */
typedef __attribute__((aligned(16))) unsigned long long jmp_buf[64];
void longjmp(jmp_buf, int) __attribute__((noreturn));
#ifdef _WIN64
int _setjmp(jmp_buf, void *) __attribute__((returns_twice));
#define setjmp(env) _setjmp((env), 0)
#else
int _setjmp(jmp_buf) __attribute__((returns_twice));
#define setjmp(env) _setjmp(env)
#endif
#endif

/* locale.h */
struct lconv { char *decimal_point; };
struct lconv *localeconv(void);

/* errno.h */
extern int errno;
#define ERANGE 34
#define EINVAL 22

/* signal.h */
typedef int sig_atomic_t;

/* time.h */
typedef long long time_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 1000000L
time_t time(time_t *);
clock_t clock(void);
struct timeval { long long tv_sec; long tv_usec; };
int gettimeofday(struct timeval *, void *);
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; long tm_gmtoff; };
struct tm *localtime_r(const time_t *, struct tm *);

/* Host hooks the embedding module provides. */
void *tsuchi_libc_allocate(size_t bytes);
void *tsuchi_libc_reallocate(void *pointer, size_t bytes);
void tsuchi_libc_release(void *pointer);
void tsuchi_libc_write(const char *text, size_t length); /* print output */
unsigned long long tsuchi_libc_ticks(void);              /* any increasing counter */

#ifdef __cplusplus
}
#endif
#endif
