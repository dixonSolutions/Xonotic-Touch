/*
Godnotic: a syntax check for Lua mod sources with the game's own Lua 5.4
(engine/darkplaces/touch_lua54, built the way the engine builds it), for
CI and scripts/test-mods.sh: no game data and no engine needed.

  cc -O1 -o build/lua-syntax-check scripts/lua-syntax-check.c -Iengine/darkplaces -lm
  build/lua-syntax-check file.lua...        each file compiles, or the error
  build/lua-syntax-check -                  Lua text on stdin, chunks split
                                            by lines "--@@ <name>" (doc blocks)
Exit status 1 when any chunk has a syntax error.
*/
#include "touch_lua_vm.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int TouchLua_WorkStep(lua_State *L) { (void)L; return 0; }
int TouchLua_PatternAbort(void) { return 0; }

static int chk(lua_State *L, const char *name, const char *src, size_t len)
{
	char chunk[512];
	snprintf(chunk, sizeof(chunk), "@%s", name);
	if (luaL_loadbufferx(L, src, len, chunk, "t") != LUA_OK)
	{
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		lua_pop(L, 1);
		return 1;
	}
	lua_pop(L, 1);
	return 0;
}

static char *slurp(FILE *f, size_t *len)
{
	size_t cap = 65536, n = 0, r;
	char *buf = malloc(cap);
	while ((r = fread(buf + n, 1, cap - n, f)) > 0)
	{
		n += r;
		if (n == cap)
			buf = realloc(buf, cap *= 2);
	}
	*len = n;
	return buf;
}

int main(int argc, char **argv)
{
	lua_State *L = luaL_newstate();
	int i, bad = 0, n = 0;
	for (i = 1; i < argc; i++)
	{
		size_t len;
		char *src;
		if (!strcmp(argv[i], "-"))
		{
			char *p, *start, name[256] = "stdin";
			src = slurp(stdin, &len);
			src = realloc(src, len + 1);
			src[len] = 0;
			for (start = p = src; ; )
			{
				char *next = strstr(p, "--@@ ");
				if (next != p && (!next || next[-1] == '\n'))
				{
					if (next && next != start)
					{
						bad |= chk(L, name, start, (size_t)(next - start));
						n++;
					}
					else if (!next)
					{
						bad |= chk(L, name, start, strlen(start));
						n++;
						break;
					}
				}
				if (!next)
					break;
				if (next == src || next[-1] == '\n')
				{
					char *e = strchr(next, '\n');
					size_t k = e ? (size_t)(e - next - 5) : strlen(next + 5);
					if (k >= sizeof(name))
						k = sizeof(name) - 1;
					memcpy(name, next + 5, k);
					name[k] = 0;
					start = e ? e + 1 : next + strlen(next);
					p = start;
				}
				else
					p = next + 5;
			}
			free(src);
			continue;
		}
		{
			FILE *f = fopen(argv[i], "rb");
			if (!f)
			{
				fprintf(stderr, "%s: cannot open\n", argv[i]);
				bad = 1;
				continue;
			}
			src = slurp(f, &len);
			fclose(f);
			bad |= chk(L, argv[i], src, len);
			n++;
			free(src);
		}
	}
	lua_close(L);
	printf("%d Lua chunk(s) checked, %s\n", n, bad ? "SYNTAX ERRORS" : "all compile");
	return bad;
}
