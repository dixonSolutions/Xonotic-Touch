// The engine-free parts of touch_csprogs.c (docs/JOIN_SPEED.md): reading a
// stock server's first message as its fragments arrive, and the SHA-256 a
// fetched file must have. Built and run by scripts/test-joinfast.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "touch_csprogs_logic.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
	char hex[65], map[64];
	// the head of a stock 0.8.6 server's first reliable message, as
	// SV_SendServerinfo writes it: svc_print, three svc_stufftext (9), then
	// svc_serverinfo (11) with its fields and the model list
	unsigned char msg[512];
	int n = 0, full;
	const char *parts[] = {"\x08\nServer: Xonotic Linux 11:11:11 Nov 11 2011 - release (progs 12345 crc)\n",
		"\x09" "csqc_progname csprogs.dat\n", "\x09" "csqc_progsize 4021257\n", "\x09" "csqc_progcrc 18349\n"};
	size_t i;
	for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
	{
		memcpy(msg + n, parts[i], strlen(parts[i]) + 1);
		n += (int)strlen(parts[i]) + 1;
	}
	msg[n++] = 11;
	memcpy(msg + n, "\x0b\x0d\x00\x00\x10\x01" "Stormkeep\0maps/stormkeep.bsp\0*1\0models/player/erebus.iqm\0", 64);
	n += 64;
	full = n;

	CHECK(TCPL_FindInt(msg, full, "csqc_progsize ") == 4021257, "size");
	CHECK(TCPL_FindInt(msg, full, "csqc_progcrc ") == 18349, "crc");
	// a fragment boundary inside the number: not yet
	{
		// cut the message inside "18349", as a fragment boundary would
		int at;
		for (at = 0; at + 5 <= full && memcmp(msg + at, "18349", 5); at++)
			;
		CHECK(at + 5 <= full, "test message has no CRC");
		CHECK(TCPL_FindInt(msg, at + 3, "csqc_progcrc ") == -1, "partial number taken");
	}
	CHECK(TCPL_FindInt(msg, 40, "csqc_progcrc ") == -1, "found before it arrived");
	CHECK(TCPL_FindInt((const unsigned char *)"csqc_progcrc x\n", 15, "csqc_progcrc ") == -1, "not a number");
	CHECK(TCPL_FindInt((const unsigned char *)"csqc_progcrc \n", 14, "csqc_progcrc ") == -1, "empty number");

	CHECK(TCPL_FindMap(msg, full, map, sizeof(map)) && !strcmp(map, "maps/stormkeep.bsp"), "map: %s", map);
	CHECK(!TCPL_FindMap(msg, full - 50, map, sizeof(map)), "map before its end arrived");
	CHECK(!TCPL_FindMap((const unsigned char *)"maps/../../x.bsp", 17, map, sizeof(map)), "a path out of maps/");
	CHECK(!TCPL_FindMap((const unsigned char *)"maps/a b.bsp", 13, map, sizeof(map)), "space in name");
	CHECK(!TCPL_FindMap((const unsigned char *)"maps/x.bsp\0", 11, map, 8), "name longer than the buffer");

	// FIPS 180-2 vectors
	TCPL_Sha256Hex((const unsigned char *)"abc", 3, hex);
	CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc) %s", hex);
	TCPL_Sha256Hex((const unsigned char *)"", 0, hex);
	CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256() %s", hex);
	TCPL_Sha256Hex((const unsigned char *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
	CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256(56 bytes) %s", hex);
	{
		unsigned char *a = malloc(1000000);
		memset(a, 'a', 1000000);
		TCPL_Sha256Hex(a, 1000000, hex);
		CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256(1M a) %s", hex);
		free(a);
	}
	printf("%s\n", fails ? "csprogs logic: FAILED" : "csprogs logic: all passed");
	return fails ? 1 : 0;
}
