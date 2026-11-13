# Lua 5.4.8 (vendored)

Unmodified source files from `lua-5.4.8.tar.gz` (<https://www.lua.org/ftp/>,
sha256 `4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae`).
MIT license, see `LICENSE`.

Only the core, the auxiliary library and the libraries the Godnotic
sandbox offers are carried: base, coroutine, table, string, math and utf8.
The io, os, debug and package libraries (`liolib.c`, `loslib.c`, `ldblib.c`,
`loadlib.c`, `linit.c`) and the stand-alone `lua.c` / `luac.c` are left out
on purpose, so no build can link them in by accident.

The files are compiled as a single translation unit by
`../touch_lua_vm.c`, which is also where the sandbox's pattern-matching time
check is attached to `lstrlib.c` (through the `l_unlikely` macro, without
editing the file). To update: replace the files with a newer 5.4.x release,
keep the same list, and run `scripts/test-lua-sandbox.sh`.
