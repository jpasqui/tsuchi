// The in-game check of compiled mods runs this and reads what it writes to the log, one thing to a line.
#include <string.h>

#include "tsuchi/tsuchi.h"

namespace {

const TSUCHI_Host* host;
TSUCHI_Mod* self;

// A global that code has to set up, and memory that starts as zeros: both have to be right once the mod is loaded.
__attribute__((noinline)) int made_at_start() {
    volatile int value = 41;
    return value + 1;
}
int setup = made_at_start();
char zeros[6000];

int frames;
bool told_actors;

void add(char* line, const char* text) { memcpy(line + strlen(line), text, strlen(text) + 1); }
void add(char* line, uint64_t number) {
    char digits[24];
    int at = sizeof digits;
    digits[--at] = 0;
    do digits[--at] = char('0' + number % 10);
    while (number /= 10);
    add(line, digits + at);
}

void on_step(void*, const TSUCHI_TimeStep*) {
    ++frames;
    if (frames == 30) host->mod->log(self, "compiled frames 30");
    if (told_actors || frames % 30) return;
    uint64_t count = 0; // asked for with no room: only how many there are
    host->entity->list(self, TSUCHI_Object{}, nullptr, 0, &count);
    if (count < 100) return;
    told_actors = true;
    char line[64] = "compiled actors ";
    add(line, count);
    host->mod->log(self, line);
}

} // namespace

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host* given, TSUCHI_Mod* mod) {
    host = given, self = mod;
    char text[64];
    uint64_t size = 0;
    char line[200] = "compiled started ";
    if (host->mod->id(mod, text, sizeof text, &size) != TSUCHI_OK) return TSUCHI_INVALID;
    add(line, text);
    if (host->mod->version(mod, text, sizeof text, &size) != TSUCHI_OK) return TSUCHI_INVALID;
    add(line, " "), add(line, text);
    if (host->mod->version_of(mod, "check.library", text, sizeof text, &size) != TSUCHI_OK) return TSUCHI_INVALID;
    add(line, " with "), add(line, text);
    host->mod->log(mod, line);

    bool clean = true;
    for (char byte : zeros) clean = clean && !byte;
    zeros[5999] = 1;
    line[0] = 0;
    add(line, "compiled memory "), add(line, uint64_t(setup)), add(line, clean ? " zeros" : " dirty");
    host->mod->log(mod, line);

    TSUCHI_Subscription subscription;
    return host->time->on_step(mod, on_step, nullptr, &subscription);
}
