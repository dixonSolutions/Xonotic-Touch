#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef int qbool;
enum { GAME_XONOTIC = 1, GAME_OTHER = 2 };
static int gamemode = GAME_XONOTIC;
static int key_consoleactive;
#define KEY_CONSOLEACTIVE_USER 1
static jmp_buf aborted;
static char error[1024];
static void Host_Error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(error, sizeof(error), format, args);
    va_end(args);
    longjmp(aborted, 1);
}
#include "touch_join.h"
int main(void)
{
    TouchJoin_RequireWorld(1, "maps/present.bsp");
    if (!setjmp(aborted)) {
        TouchJoin_RequireWorld(0, "maps/missing.bsp");
        assert(!"unrenderable map must abort signon");
    }
    assert(strstr(error, "maps/missing.bsp"));
    assert(strstr(error, "reconnect"));
    assert(key_consoleactive & KEY_CONSOLEACTIVE_USER);
    if (!setjmp(aborted)) {
        TouchJoin_RequireWorld(0, NULL);
        assert(!"missing map name must still abort signon");
    }
    assert(strstr(error, "(unknown)"));
    gamemode = GAME_OTHER;
    TouchJoin_RequireWorld(0, "maps/other.bsp");
    puts("ok: valid worlds pass; missing worlds abort before prespawn with a recovery message");
}
