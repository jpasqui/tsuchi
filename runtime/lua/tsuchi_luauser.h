/* Lua 5.4 configuration for the framework (LUA_USER_H, included at the end of
 * luaconf.h). Used on every target so scripts behave the same everywhere. */
#ifndef TSUCHI_LUAUSER_H
#define TSUCHI_LUAUSER_H

/* Numbers print with a '.' whatever the locale. */
#undef lua_getlocaledecpoint
#define lua_getlocaledecpoint() '.'

/* Hex float literals use Lua's own parser (not strtod). */
#undef lua_strx2number

/* print() and warnings go to the framework log. */
#ifdef __cplusplus
extern "C" {
#endif
void tsuchi_lua_write(const char *text, unsigned long length);
void tsuchi_lua_write_error(const char *format, const char *argument);
#ifdef __cplusplus
}
#endif
#define lua_writestring(s, l) tsuchi_lua_write((s), (unsigned long)(l))
#define lua_writeline() tsuchi_lua_write("\n", 1)
#define lua_writestringerror(s, p) tsuchi_lua_write_error((s), (p))

/* The random seed must not depend on time() being meaningful. */
#define luai_makeseed(L) ((unsigned int)(unsigned long)(L))

#endif
