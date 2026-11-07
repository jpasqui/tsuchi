/* Checks a compiled mod makes in the game, written to its log.
 *
 * A check is one thing a mod finds out about the running game and reports as passed or failed.
 * It is lines in the mod's log, one for each thing that happens to it:
 *
 *     check <name> planned
 *     check <name> waiting <for what>
 *     check <name> pass [<note>]
 *     check <name> fail <why>
 *
 * A mod plans its checks as it starts. Then it begins one, states what it expects, and ends it:
 *
 *     tsuchi_check_start(host, mod);
 *     tsuchi_check_plan("refused");
 *     tsuchi_check_begin("refused");
 *     tsuchi_expect_result(host->storage->write(mod, "../escape", "x", 1), TSUCHI_INVALID, "a name that leads out");
 *     tsuchi_check_end();
 *
 * That writes `check refused pass`, or `check refused fail a name that leads out: got ok, wanted
 * invalid`. The first expectation that fails is the one told. A check that cannot run yet says
 * what it waits for with tsuchi_check_waiting and is tried again later, on a step.
 *
 * Include this in one source file of the mod. It works in C and in C++, and needs nothing but
 * the framework's headers and the SDK's <string.h>. */
#ifndef TSUCHI_CHECK_H
#define TSUCHI_CHECK_H
#include <stddef.h>
#include <string.h>

#include "tsuchi/tsuchi.h"

#define TSUCHI_CHECK_LINE 480 /* the longest line written, with its terminator */
#define TSUCHI_CHECK_WAITS 16 /* checks whose last reason for waiting is remembered */

static struct {
    const TSUCHI_Host *host;
    TSUCHI_Mod *mod;
    const char *name;             /* the check that is begun, or null */
    int failed;                   /* whether an expectation of it has failed */
    char why[TSUCHI_CHECK_LINE];  /* the first that did */
    const char *waits[TSUCHI_CHECK_WAITS]; /* the checks that have waited */
    char reasons[TSUCHI_CHECK_WAITS][96];  /* and what each last said it waits for */
} tsuchi_check_state;

/* ---- Text, built without a C library ---------------------------------------------------- */

/* Adds text to a line of TSUCHI_CHECK_LINE bytes, as much of it as fits. */
static inline void tsuchi_check_add(char *line, const char *text) {
    size_t used = strlen(line);
    for (; *text && used + 1 < TSUCHI_CHECK_LINE; ++text) line[used++] = *text;
    line[used] = 0;
}

/* Adds a whole number. */
static inline void tsuchi_check_add_number(char *line, int64_t number) {
    char digits[24];
    int at = (int)sizeof digits;
    uint64_t left = number < 0 ? 0 - (uint64_t)number : (uint64_t)number;
    digits[--at] = 0;
    do digits[--at] = (char)('0' + left % 10);
    while (left /= 10);
    if (number < 0) digits[--at] = '-';
    tsuchi_check_add(line, digits + at);
}

/* Adds a number with three places after the point. One too large to write that way is written as `large`. */
static inline void tsuchi_check_add_real(char *line, double number) {
    const double size = number < 0 ? -number : number;
    if (!(size < 9.0e15)) {
        tsuchi_check_add(line, number != number ? "nan" : number < 0 ? "-large" : "large");
        return;
    }
    const uint64_t thousandths = (uint64_t)(size * 1000.0 + 0.5);
    char places[5] = {'.', (char)('0' + thousandths / 100 % 10), (char)('0' + thousandths / 10 % 10), (char)('0' + thousandths % 10), 0};
    if (number < 0 && thousandths) tsuchi_check_add(line, "-");
    tsuchi_check_add_number(line, (int64_t)(thousandths / 1000));
    tsuchi_check_add(line, places);
}

/* The name of a result, as the reference pages write it. */
static inline const char *tsuchi_check_result_name(TSUCHI_Result result) {
    switch (result) {
    case TSUCHI_OK: return "ok";
    case TSUCHI_NOT_FOUND: return "not_found";
    case TSUCHI_INVALID: return "invalid";
    case TSUCHI_BUSY: return "busy";
    case TSUCHI_NOT_READY: return "not_ready";
    case TSUCHI_CAPACITY: return "capacity";
    case TSUCHI_UNSUPPORTED: return "unsupported";
    case TSUCHI_IO: return "io";
    case TSUCHI_DENIED: return "denied";
    case TSUCHI_NO_MEMORY: return "no_memory";
    }
    return "unknown";
}

/* ---- The lines ---------------------------------------------------------------------------- */

/* Tells the helper whose checks these are. Call it first, in tsuchi_mod_start. */
static inline void tsuchi_check_start(const TSUCHI_Host *host, TSUCHI_Mod *mod) {
    memset(&tsuchi_check_state, 0, sizeof tsuchi_check_state);
    tsuchi_check_state.host = host;
    tsuchi_check_state.mod = mod;
}

/* Writes `check <name> <word>`, and what follows when there is any. */
static inline void tsuchi_check_say(const char *name, const char *word, const char *rest) {
    char line[TSUCHI_CHECK_LINE] = "check ";
    tsuchi_check_add(line, name), tsuchi_check_add(line, " "), tsuchi_check_add(line, word);
    if (rest && *rest) tsuchi_check_add(line, " "), tsuchi_check_add(line, rest);
    tsuchi_check_state.host->mod->log(tsuchi_check_state.mod, line);
}

/* Says that the mod has a check of this name. Call it for every check as the mod starts, so that whoever reads
 * the log knows of a check that never ran. */
static inline void tsuchi_check_plan(const char *name) { tsuchi_check_say(name, "planned", 0); }

/* Says that a check cannot run yet, or cannot finish, and what it waits for. The line is written when the reason
 * is another than the last one given for that check, so this can be called on every try. */
static inline void tsuchi_check_waiting(const char *name, const char *for_what) {
    int slot = 0;
    while (slot < TSUCHI_CHECK_WAITS && tsuchi_check_state.waits[slot] && strcmp(tsuchi_check_state.waits[slot], name)) ++slot;
    if (slot < TSUCHI_CHECK_WAITS) {
        char *said = tsuchi_check_state.reasons[slot];
        size_t length = strlen(for_what);
        if (length > sizeof tsuchi_check_state.reasons[slot] - 1) length = sizeof tsuchi_check_state.reasons[slot] - 1;
        if (tsuchi_check_state.waits[slot] && strlen(said) == length && !memcmp(said, for_what, length)) return;
        tsuchi_check_state.waits[slot] = name;
        memcpy(said, for_what, length);
        said[length] = 0;
    }
    tsuchi_check_say(name, "waiting", for_what);
}

/* Begins a check: what is expected from here to tsuchi_check_end is expected of it. */
static inline void tsuchi_check_begin(const char *name) {
    tsuchi_check_state.name = name;
    tsuchi_check_state.failed = 0;
    tsuchi_check_state.why[0] = 0;
}

/* True when no expectation of the check that is begun has failed. */
static inline bool tsuchi_check_holds(void) { return !tsuchi_check_state.failed; }

/* Where the first failed expectation's text goes, or null when one has failed already. */
static inline char *tsuchi_check_failing(const char *what) {
    if (tsuchi_check_state.failed) return 0;
    tsuchi_check_state.failed = 1;
    tsuchi_check_state.why[0] = 0;
    tsuchi_check_add(tsuchi_check_state.why, what);
    return tsuchi_check_state.why;
}

/* Expects something to be true. `what` says what, for the line a failure writes. Every expectation returns
 * whether it held, so that the steps that depend on it can be left out. */
static inline bool tsuchi_expect(bool held, const char *what) {
    if (!held) tsuchi_check_failing(what);
    return held;
}

/* Expects a call to have answered as it should. */
static inline bool tsuchi_expect_result(TSUCHI_Result got, TSUCHI_Result wanted, const char *what) {
    char *why = got == wanted ? 0 : tsuchi_check_failing(what);
    if (why) {
        tsuchi_check_add(why, ": got "), tsuchi_check_add(why, tsuchi_check_result_name(got));
        tsuchi_check_add(why, ", wanted "), tsuchi_check_add(why, tsuchi_check_result_name(wanted));
    }
    return got == wanted;
}

/* Expects a whole number. */
static inline bool tsuchi_expect_number(int64_t got, int64_t wanted, const char *what) {
    char *why = got == wanted ? 0 : tsuchi_check_failing(what);
    if (why) {
        tsuchi_check_add(why, ": got "), tsuchi_check_add_number(why, got);
        tsuchi_check_add(why, ", wanted "), tsuchi_check_add_number(why, wanted);
    }
    return got == wanted;
}

/* Expects a number that may have a part after the point, to within `slack` either way. */
static inline bool tsuchi_expect_near(double got, double wanted, double slack, const char *what) {
    const bool held = got >= wanted - slack && got <= wanted + slack;
    char *why = held ? 0 : tsuchi_check_failing(what);
    if (why) {
        tsuchi_check_add(why, ": got "), tsuchi_check_add_real(why, got);
        tsuchi_check_add(why, ", wanted "), tsuchi_check_add_real(why, wanted);
    }
    return held;
}

/* Expects a text. */
static inline bool tsuchi_expect_text(const char *got, const char *wanted, const char *what) {
    const bool held = !strcmp(got, wanted);
    char *why = held ? 0 : tsuchi_check_failing(what);
    if (why) {
        tsuchi_check_add(why, ": got \""), tsuchi_check_add(why, got);
        tsuchi_check_add(why, "\", wanted \""), tsuchi_check_add(why, wanted), tsuchi_check_add(why, "\"");
    }
    return held;
}

/* Expects `count` bytes to be the same as others. */
static inline bool tsuchi_expect_bytes(const void *got, const void *wanted, size_t count, const char *what) {
    const unsigned char *a = (const unsigned char *)got, *b = (const unsigned char *)wanted;
    size_t at = 0;
    while (at < count && a[at] == b[at]) ++at;
    if (at < count) {
        char *why = tsuchi_check_failing(what);
        if (why) {
            tsuchi_check_add(why, ": byte "), tsuchi_check_add_number(why, (int64_t)at);
            tsuchi_check_add(why, " is "), tsuchi_check_add_number(why, a[at]);
            tsuchi_check_add(why, ", wanted "), tsuchi_check_add_number(why, b[at]);
        }
    }
    return at == count;
}

/* Ends the check that is begun, and writes whether it passed: `pass` with the note when there is one, or `fail`
 * with the first expectation that did not hold. Returns whether it passed. */
static inline bool tsuchi_check_end_with(const char *note) {
    const bool passed = !tsuchi_check_state.failed;
    if (tsuchi_check_state.name) tsuchi_check_say(tsuchi_check_state.name, passed ? "pass" : "fail", passed ? note : tsuchi_check_state.why);
    tsuchi_check_state.name = 0;
    return passed;
}
static inline bool tsuchi_check_end(void) { return tsuchi_check_end_with(0); }

/* ---- What a check acts on ------------------------------------------------------------------- */

/* Reads one of the mod's files as lines: `targets.txt`, where a check keeps the names of the things of the game's
 * it acts on. The file is read into `buffer` and each line's start is put in `lines`; a line ends at its line
 * break, and an empty line or one that begins with `#` is passed over. Returns how many lines there are, or -1
 * when the file cannot be read, does not fit, or has more than `most` lines. */
static inline int tsuchi_check_lines(const char *path, char *buffer, size_t capacity, const char **lines, int most) {
    const TSUCHI_FileService *files = tsuchi_check_state.host->file;
    uint64_t size = 0, got = 0;
    int count = 0;
    if (files->size(tsuchi_check_state.mod, path, &size) != TSUCHI_OK || size + 1 > capacity) return -1;
    if (files->read(tsuchi_check_state.mod, path, 0, buffer, size, &got) != TSUCHI_OK || got != size) return -1;
    buffer[size] = 0;
    for (char *at = buffer; at < buffer + size;) {
        char *end = at;
        while (*end && *end != '\n' && *end != '\r') ++end;
        const bool more = end < buffer + size;
        *end = 0;
        if (*at && *at != '#') {
            if (count == most) return -1;
            lines[count++] = at;
        }
        at = more ? end + 1 : end;
    }
    return count;
}

#endif
