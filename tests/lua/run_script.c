/* Runs one Lua script and prints what it returns, or its error. Built twice by the checks: with
 * Lua on this machine's C library, and with Lua on the framework's own, which this file then
 * gives its memory and its clock. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

void tsuchi_lua_write(const char *text, unsigned long length) { (void)text, (void)length; }
void tsuchi_lua_write_error(const char *format, const char *argument) { (void)format, (void)argument; }

/* What the framework's C library asks of whatever it is built into. */
void *tsuchi_libc_allocate(size_t bytes) { return malloc(bytes); }
void *tsuchi_libc_reallocate(void *block, size_t bytes) { return realloc(block, bytes); }
void tsuchi_libc_release(void *block) { free(block); }
void tsuchi_libc_write(const char *text, size_t length) { (void)text, (void)length; }
unsigned long long tsuchi_libc_ticks(void) {
    static unsigned long long ticks;
    return ++ticks;
}

static void *allocate(void *user, void *block, size_t old_size, size_t new_size) {
    (void)user, (void)old_size;
    if (new_size == 0) {
        free(block);
        return 0;
    }
    return realloc(block, new_size);
}

int main(int argc, char **argv) {
    static const luaL_Reg libraries[] = {{LUA_GNAME, luaopen_base},     {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
                                         {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},    {LUA_UTF8LIBNAME, luaopen_utf8},
                                         {0, 0}};
    static char script[1 << 16];
    FILE *file = argc == 2 ? fopen(argv[1], "rb") : 0;
    if (!file) return 3;
    const size_t length = fread(script, 1, sizeof script, file);
    fclose(file);
    lua_State *L = lua_newstate(allocate, 0);
    if (!L) return 1;
    for (const luaL_Reg *library = libraries; library->func; ++library) {
        luaL_requiref(L, library->name, library->func, 1);
        lua_pop(L, 1);
    }
    int status = luaL_loadbuffer(L, script, length, "=script");
    if (status == LUA_OK) status = lua_pcall(L, 0, 1, 0);
    size_t bytes = 0;
    const char *text = lua_tolstring(L, -1, &bytes);
    if (text) fwrite(text, 1, bytes, stdout);
    lua_close(L);
    return status == LUA_OK ? 0 : 2;
}
