# Lua on BuzzOS

Vendored **Lua 5.4.7** (https://www.lua.org/) for the user-space `/bin/lua`
interpreter.

## Upstream

- Version: 5.4.7
- License: MIT (see headers in each source file and `readme.html`)
- Sources: copied from the official `src/` tree (standalone `luac` omitted)

## Port notes

- Built freestanding against BuzzOS mini libc (`-ffreestanding`, x86_64).
- Platform overrides live in `src/user/ports/lua/buzzos_lua_port.h`
  (`LUA_USE_C89`, package path, no `os.execute` shell, fixed decimal point).
- Error handling uses user-space `setjmp`/`longjmp` (`src/user/libc/setjmp.asm`).
- Dynamic C modules (`package.loadlib`) and `io.popen` are unavailable.
- Scripts: `lua`, `lua -e 'print(1+1)'`, `lua /fs/hello.lua`.

## Rebuild

```text
make
```

The initrd embeds `/bin/lua` and seeds `/fs/hello.lua` when the image is built.
