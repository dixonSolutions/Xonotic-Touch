#ifndef TOUCH_CSPROGS_H
#define TOUCH_CSPROGS_H

// Godnotic: a stock server's CSQC from our own HTTP release instead of the
// server's UDP download (touch_csprogs.c, docs/JOIN_SPEED.md).
//
// A stock Xonotic server sends its csprogs.dat (4 MB, 0.8 MB deflated) over
// DarkPlaces' download protocol: one datagram per server tick, 30 a second
// on a stock 0.8.6 server, so about 40 KB/s whatever the link: 20 s on a
// perfect network, 45-60 s on a real one. When the server's CSQC (its size
// and CRC) is a build we know, the client fetches the same bytes over HTTP
// instead, checks them against the SHA-256 it was built with and puts them
// where the server's download would have gone (dlcache/csprogs.dat.SIZE.CRC);
// the stock code then finds it and asks the server for nothing. Anything
// else (unknown build, no network, a wrong file, too slow) falls back to the
// server's own download.

struct netconn_s;

void TouchCSProgs_Init(void);
// netconn.c, the stock reliable channel: a fragment of the server's first
// message has arrived; its CSQC stufftexts come in the first one, about 20
// round trips before the whole message is in on a stock server, so the
// fetch starts then
void TouchCSProgs_Peek(struct netconn_s *conn, const unsigned char *data, int length);
// cl_parse.c CL_BeginDownloads, before it asks the server for the CSQC:
// true = a fetch of this build runs (started now if need be), the load
// carries on when it ends; false = ask the server
qbool TouchCSProgs_Wait(const char *progname, int size, int crc);
// touch_loadtime.c: once per drawn frame
void TouchCSProgs_Frame(void);
// true while a fetch runs (for the join warm-up and the loading screen)
qbool TouchCSProgs_Running(void);
// the same peek: "maps/NAME.bsp", the world model of the server's first
// message, while that message is still arriving (NULL otherwise)
const char *TouchCSProgs_PeekMap(void);

#endif
