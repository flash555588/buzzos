/* BuzzOS platform overrides for Lua 5.4.
 * Included before other headers via -include when building Lua. */
#ifndef BUZZOS_LUA_PORT_H
#define BUZZOS_LUA_PORT_H

/* Prefer ISO C89 surface; no dlopen/popen/POSIX reentrancy helpers. */
#ifndef LUA_USE_C89
#define LUA_USE_C89
#endif

/* Module search path inside BuzzOS VFS / persistent /fs. */
#ifndef LUA_PATH_DEFAULT
#define LUA_PATH_DEFAULT \
    "./?.lua;./?/init.lua;/fs/?.lua;/fs/?/init.lua;/fs/lua/?.lua;/share/lua/?.lua"
#endif

#ifndef LUA_CPATH_DEFAULT
#define LUA_CPATH_DEFAULT ""
#endif

/* No shell: os.execute returns "no shell" / failure. */
#ifndef l_system
#define l_system(cmd) ((cmd) == NULL ? 0 : -1)
#endif

/* Decimal point without full locale machinery. */
#ifndef lua_getlocaledecpoint
#define lua_getlocaledecpoint() ('.')
#endif

#endif /* BUZZOS_LUA_PORT_H */
