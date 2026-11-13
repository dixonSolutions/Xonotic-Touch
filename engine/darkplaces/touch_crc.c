/*
Godnotic: CRC_Block eight bytes at a time.

A local game computes the 16-bit CCITT CRC of the server progs (7 MB) and
three times of csprogs.dat (4.3 MB: the server for its clients, the client to
check it, PRVM_Prog_Load for the VM), and of menu.dat (2.5 MB) at start:
about 20 MB per map load with com_crc16.c's byte-at-a-time table walk, whose
every step waits on the previous one's table load.

touch_crc_fast 1 (the default; the cvar is the A/B switch and costs nothing
to read) computes the same CRC with the slicing-by-8 method: eight tables,
T[k][b] = the CRC of byte b followed by k zero bytes, so eight bytes cost
eight independent loads and one dependency step. The result is identical bit
for bit (the touch_crc_selftest command checks it against the byte walk).
*/

#include "quakedef.h"
#include "touch_crc.h"

static cvar_t touch_crc_fast = {CF_CLIENT | CF_SERVER, "touch_crc_fast", "1", "compute progs and download CRCs eight bytes at a time (Godnotic); 0 = one byte at a time"};

static unsigned short tcrc_table[8][256];
static qbool tcrc_ready;
static int tcrc_force = -1; // touch_crc_selftest: 0 or 1 overrides the cvar

// CRC_Block runs before the cvar is registered (menu.dat): the default
// string says on until then
static qbool TouchCRC_Enabled(void)
{
	if (tcrc_force >= 0)
		return tcrc_force != 0;
	return touch_crc_fast.string && touch_crc_fast.string[0] && atof(touch_crc_fast.string) != 0;
}

static void TouchCRC_Tables(void)
{
	int b, k, bit;
	for (b = 0; b < 256; b++)
	{
		unsigned short c = (unsigned short)(b << 8);
		for (bit = 0; bit < 8; bit++)
			c = (unsigned short)((c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1));
		tcrc_table[0][b] = c;
	}
	for (k = 1; k < 8; k++)
		for (b = 0; b < 256; b++)
		{
			unsigned short p = tcrc_table[k - 1][b];
			tcrc_table[k][b] = (unsigned short)((p << 8) ^ tcrc_table[0][p >> 8]);
		}
	tcrc_ready = true;
}

qbool TouchCRC_Block(const unsigned char *data, size_t size, unsigned short *crcout)
{
	unsigned short crc = 0xffff;
	if (size < 64 || !TouchCRC_Enabled())
		return false;
	if (!tcrc_ready)
		TouchCRC_Tables();
	while (size >= 8)
	{
		crc = (unsigned short)(
			tcrc_table[7][(crc >> 8) ^ data[0]] ^
			tcrc_table[6][(crc & 0xff) ^ data[1]] ^
			tcrc_table[5][data[2]] ^
			tcrc_table[4][data[3]] ^
			tcrc_table[3][data[4]] ^
			tcrc_table[2][data[5]] ^
			tcrc_table[1][data[6]] ^
			tcrc_table[0][data[7]]);
		data += 8;
		size -= 8;
	}
	while (size--)
		crc = (unsigned short)((crc << 8) ^ tcrc_table[0][(crc >> 8) ^ *data++]);
	*crcout = crc; // CRC_XOR_VALUE is 0
	return true;
}

// compares both methods on random buffers of every length up to 300 and on
// a 4 MB one, and times them
static void TouchCRC_SelfTest_f(cmd_state_t *cmd)
{
	const size_t bufsize = 4 << 20;
	unsigned char *buf = (unsigned char *)Mem_Alloc(tempmempool, bufsize);
	size_t len, i;
	int fails = 0;
	unsigned short fast;
	double t0, t1, t2;
	for (i = 0; i < bufsize; i++)
		buf[i] = (unsigned char)(rand() >> 4);
	for (len = 64; len <= 300; len++)
	{
		tcrc_force = 0;
		{
			unsigned short slow = CRC_Block(buf + (len & 7), len);
			tcrc_force = 1;
			if (!TouchCRC_Block(buf + (len & 7), len, &fast) || fast != slow)
				fails++;
		}
	}
	tcrc_force = 0;
	t0 = Sys_DirtyTime();
	{
		unsigned short slow = CRC_Block(buf, bufsize);
		t1 = Sys_DirtyTime();
		tcrc_force = 1;
		TouchCRC_Block(buf, bufsize, &fast);
		t2 = Sys_DirtyTime();
		if (fast != slow)
			fails++;
	}
	tcrc_force = -1;
	Mem_Free(buf);
	Con_Printf("touch_crc_selftest: %s, 4 MB: bytewise %.2f ms, slicing-by-8 %.2f ms\n", fails ? "FAILED" : "ok", (t1 - t0) * 1000.0, (t2 - t1) * 1000.0);
}

void TouchCRC_Init(void)
{
	Cvar_RegisterVariable(&touch_crc_fast);
	Cmd_AddCommand(CF_SHARED, "touch_crc_selftest", TouchCRC_SelfTest_f, "check the fast CRC against the bytewise one and time both (Godnotic)");
}
