#ifndef TOUCH_JOINTIME_H
#define TOUCH_JOINTIME_H

// Godnotic: the timeline of one join or local start, phase by phase
// (docs/JOIN_SPEED.md).
//
// A join starts at a tap (the front end's play or connect), else at the
// connect command, else at a local server's spawn. From there each event
// below is stamped the first time it happens, in milliseconds. At the first
// game frame, and again once the game is playable (the first frame after
// which ten frames in a row took under 50 ms), touch_jointime 1 prints
//   JOINTIME <event> <ms>
// lines and a summary, and _touch_jointime holds "event=ms ..." for scripts
// (_touch_jointime_serial counts finished joins). Costs one clock read per
// event; nothing is printed unless asked.

typedef enum touch_jt_event_e
{
	TJT_TAP,           // the player's tap (fe play / fe connect)
	TJT_SPAWN_BEGIN,   // a local server starts its map (SV_SpawnServer)
	TJT_SPAWN_END,     // ... and is up
	TJT_CHALLENGE_TX,  // first getchallenge sent
	TJT_CHALLENGE_RX,  // challenge received
	TJT_CONNECT_TX,    // connect sent
	TJT_ACCEPT,        // accept received: the client is connected
	TJT_SIGNON1,       // serverinfo parsed, precache lists known
	TJT_CURL_BEGIN,    // an HTTP pack download started (server's curl lines)
	TJT_CURL_END,      // ... every one finished
	TJT_CSPROGS_BEGIN, // the server's CSQC is being fetched (UDP or HTTP)
	TJT_CSPROGS_END,   // ... and is here
	TJT_PRECACHE_END,  // models and sounds loaded, prespawn sent
	TJT_SIGNON2,
	TJT_SIGNON3,
	TJT_CSQC_BEGIN,    // CL_VM_Init (client progs and CSQC_Init)
	TJT_CSQC_END,
	TJT_SIGNON4,       // begin: the server sends the first entity frame
	TJT_FIRST_FRAME,   // the first game frame is drawn
	TJT_PLAYABLE,      // frames are smooth from here on
	TJT_COUNT
}
touch_jt_event_t;

void TouchJoinTime_Init(void);
// the front end's play / connect: a join starts now
void TouchJoinTime_Tap(void);
// connect command (touch_net.c TouchNet_ConnectAgain is called from it)
void TouchJoinTime_Connect(void);
void TouchJoinTime_Mark(int event);
// cl_parse.c: the client's signon stage changed
void TouchJoinTime_Signon(int stage);
// where the CSQC came from: "local" (the bundled one), "cache" (dlcache),
// "http" (touch_csprogs.c), "udp" (the server's own download)
void TouchJoinTime_CSProgsSource(const char *source, long long bytes);
// touch_net.c TouchNet_TracePacket: every packet, before its trace
void TouchJoinTime_Packet(const void *data, int length, int sent);
// touch_loadtime.c: phases begun and ended (server spawn, CSQC init)
void TouchJoinTime_Phase(int tlt_phase, qbool begin);
// touch_loadtime.c TouchLoadTime_Frame: after every drawn frame
void TouchJoinTime_Frame(double now);
// milliseconds since the running join started, -1 when none runs
double TouchJoinTime_Since(void);
// the running join's round trip as its challenge measured it, seconds; -1
// when not known
double TouchJoinTime_RTT(void);

#endif
