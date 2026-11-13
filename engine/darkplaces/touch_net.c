/*
Godnotic: faster joins over real networks. docs/NETWORK.md has the design,
the wire format and the measurements.

1. Windowed reliable channel.
   DarkPlaces sends reliable data (everything of the signon: the serverinfo
   and precache lists, baselines, stufftexts, the client's stringcmds) as
   fragments of at most 1 KB, one at a time: the next fragment, and the next
   message, waits for the acknowledgement of the last one, and a lost one is
   sent again after a fixed second. A join is about 60 KB of that, so at
   100 ms round trip it spends seconds just waiting.

   Between Godnotic peers up to net_reliable_window fragments are in flight
   (they may span messages), acknowledgements are cumulative, and a lost
   fragment is sent again after a timeout taken from the measured round trip
   (RFC 6298 smoothing) or after three duplicate acknowledgements. The
   receiver keeps fragments that arrive ahead of a gap (up to the window) and
   hands messages to the game in order and exactly once.

   Negotiation: the client adds "\rwin\N" to its connect string (N = the
   window it can take); a server that knows it answers "accept\rwin\M"
   (M = min(N, its own net_reliable_window)) and both then send with window M.
   A stock server ignores the unknown connect key and answers "accept"; a stock
   client never offers it and gets the stock "accept". The key and its
   meaning are those of Insanotic, so the two forks interoperate.

2. Signon without waiting for frames.
   While a client is in its signon the server wakes up for its packets
   (select on the sockets instead of a blind sleep until the next tick) and
   sends the reply the moment the request is parsed instead of at the end of
   the next tick; the client runs without its frame limiter, sleeps in select
   too, and sends its reliable messages at once instead of at cl_netfps. All
   of this is timing only: the bytes on the wire are what a stock peer sends.

3. touch_net_trace 1 prints every packet of a join with a timestamp, and the
   client's frame work time, for scripts/net-trace-report.py.
*/

#include "quakedef.h"
#include "lhnet.h"
#include "netconn.h"
#include "crypto.h"
#include "cl_parse.h"
#include "libcurl.h"
#include "sv_demo.h"
#include "touch_net.h"

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/select.h>
#define TOUCHNET_SELECT 1
#endif

static cvar_t net_reliable_window = {CF_CLIENT | CF_SERVER, "net_reliable_window", "32", "reliable 1 KB fragments in flight to a peer that supports it (Godnotic, Insanotic); 0 or 1 = one per round trip, as stock DarkPlaces"};
static cvar_t net_reliable_rto_min = {CF_CLIENT | CF_SERVER, "net_reliable_rto_min", "0.05", "shortest wait in seconds before a windowed reliable fragment is sent again"};
static cvar_t net_reliable_rto_max = {CF_CLIENT | CF_SERVER, "net_reliable_rto_max", "1", "longest wait in seconds before a windowed reliable fragment is sent again (stock DarkPlaces always waits 1)"};
static cvar_t net_signon_fast = {CF_CLIENT | CF_SERVER, "net_signon_fast", "1", "during a client's signon, wake up for its packets and answer at once instead of at the next frame or server tick (Godnotic); 0 = stock timing"};
static cvar_t touch_net_trace = {CF_CLIENT | CF_SERVER, "touch_net_trace", "0", "print every packet of a join with a timestamp (Godnotic, for scripts/net-trace-report.py)"};

#define TN_MAXWINDOW 64     // fragments in flight, and the receiver's reorder slots
#define TN_RING 160         // fragments queued to send (sent or not): 160 KB
#define TN_READYMAX (NET_MAXMESSAGE + (TN_MAXWINDOW + 2) * MAX_PACKETFRAGMENT)

typedef struct tn_frag_s
{
	unsigned short len;
	unsigned char eom;
	unsigned char resent;   // sent more than once: no round-trip sample (Karn)
	double sent;            // Sys_DirtyTime of the last send
	unsigned char data[MAX_PACKETFRAGMENT];
}
tn_frag_t;

typedef struct tn_conn_s
{
	struct tn_conn_s *next;
	netconn_t *conn;
	int window;             // negotiated fragments in flight

	// sending
	tn_frag_t *ring;        // TN_RING, allocated with the state
	unsigned int base;      // oldest unacknowledged fragment
	unsigned int sendnext;  // next fragment never sent
	unsigned int tail;      // one past the last queued fragment
	int dupacks;
	qbool recovery;
	unsigned int recover;   // recovery ends when this one is acknowledged
	double srtt, rttvar;    // seconds; srtt 0 = no sample yet
	int backoff;
	int rate, burstsize;

	// receiving
	unsigned char rxdata[TN_MAXWINDOW][MAX_PACKETFRAGMENT];
	unsigned short rxlen[TN_MAXWINDOW];
	unsigned char rxflags[TN_MAXWINDOW]; // 1 = held, 2 = end of message
	unsigned char *ready;   // complete messages not yet handed over: [len 4][bytes]...
	int readylen, readypos;

	// statistics (net_status, the trace)
	int fastretransmits, timeouts, heldahead, dupdata;
}
tn_conn_t;

static tn_conn_t *tn_list;
static mempool_t *tn_mempool;

// client handshake state
static int tn_offered;          // window offered in the last connect
static double tn_offer_time;    // when that connect went out
static int tn_accept_window;    // window of the last accept (0 = stock)
static double tn_accept_rtt;

// trace
static double tn_frame_work;    // client frame work since the last trace line
static int tn_last_signon = -1;

static void TN_TraceSignon(void);

static tn_conn_t *TN_Find(netconn_t *conn)
{
	tn_conn_t *t;
	for (t = tn_list; t; t = t->next)
		if (t->conn == conn)
			return t;
	return NULL;
}

static tn_conn_t *TN_Create(netconn_t *conn, int window)
{
	tn_conn_t *t = TN_Find(conn);
	if (t)
		return t;
	if (!tn_mempool)
		tn_mempool = Mem_AllocPool("touch_net", 0, NULL);
	t = (tn_conn_t *)Mem_Alloc(tn_mempool, sizeof(*t));
	t->ring = (tn_frag_t *)Mem_Alloc(tn_mempool, TN_RING * sizeof(*t->ring));
	t->conn = conn;
	t->window = bound(2, window, TN_MAXWINDOW);
	// sequences carry on from where the connection is (0 on a new one)
	t->base = t->sendnext = t->tail = conn->nq.sendSequence;
	t->next = tn_list;
	tn_list = t;
	return t;
}

void TouchNet_Close(netconn_t *conn)
{
	tn_conn_t **p, *t;
	for (p = &tn_list; (t = *p); p = &t->next)
	{
		if (t->conn == conn)
		{
			*p = t->next;
			if (t->ready)
				Mem_Free(t->ready);
			Mem_Free(t->ring);
			Mem_Free(t);
			return;
		}
	}
}

static int TN_Clamp(const char *s)
{
	// a peer's number: anything odd means no window
	long v;
	char *end;
	if (!s || !*s)
		return 0;
	v = strtol(s, &end, 10);
	if (*end || v < 2)
		return 0;
	return (int)min(v, TN_MAXWINDOW);
}

static int TN_LocalWindow(void)
{
	return net_reliable_window.integer >= 2 ? min(net_reliable_window.integer, TN_MAXWINDOW) : 0;
}

/*
==============
Handshake
==============
*/

void TouchNet_AppendConnect(char *connect, size_t size)
{
	size_t len = strlen(connect);
	tn_offered = TN_LocalWindow();
	tn_offer_time = Sys_DirtyTime();
	tn_accept_window = 0;
	if (!tn_offered || len + 16 >= size)
	{
		tn_offered = 0;
		return;
	}
	dpsnprintf(connect + len, size - len, "\\rwin\\%d", tn_offered);
}

int TouchNet_ClientConnectionless(char *string, int length)
{
	char value[16];
	if (length < 6 || memcmp(string, "accept", 6) || !cls.connect_trying)
		return length;
	tn_accept_window = 0;
	if (length > 6 && string[6] == '\\')
	{
		// only a window we offered; never more than we offered
		if (tn_offered && InfoString_GetValue(string + 6, "rwin", value, sizeof(value)))
			tn_accept_window = min(TN_Clamp(value), tn_offered);
		tn_accept_rtt = Sys_DirtyTime() - tn_offer_time;
		// the rest of the stack sees the stock "accept"
		string[6] = 0;
		return 6;
	}
	return length;
}

void TouchNet_Established(netconn_t *conn)
{
	tn_conn_t *t;
	if (!conn)
		return;
	if (tn_accept_window >= 2)
	{
		t = TN_Create(conn, tn_accept_window);
		// the connect/accept exchange is a first round-trip sample
		if (tn_accept_rtt > 0 && tn_accept_rtt < 3)
		{
			t->srtt = tn_accept_rtt;
			t->rttvar = tn_accept_rtt / 2;
		}
	}
	if (touch_net_trace.integer)
		Con_Printf("NETTRACE cl %.3f est window=%d rtt=%.1f\n", Sys_DirtyTime() * 1000.0, tn_accept_window, tn_accept_rtt * 1000.0);
	tn_accept_window = 0;
}

qbool TouchNet_ServerAccept(netconn_t *conn, lhnetsocket_t *mysocket, lhnetaddress_t *peeraddress, const char *connectstring)
{
	char value[16], reply[64];
	tn_conn_t *t;
	int offer, window;
	if (!conn)
		return false;
	offer = InfoString_GetValue(connectstring, "rwin", value, sizeof(value)) ? TN_Clamp(value) : 0;
	t = TN_Find(conn);
	if (!t)
	{
		window = min(offer, TN_LocalWindow());
		if (window < 2)
			return false;
		t = TN_Create(conn, window);
	}
	else if (offer < 2)
	{
		// the same address again without a window (a stock client now):
		// the stock "accept" and the stock channel
		TouchNet_Close(conn);
		return false;
	}
	// a repeated connect gets the same answer
	dpsnprintf(reply, sizeof(reply), "\377\377\377\377accept\\rwin\\%d", t->window);
	NetConn_WriteString(mysocket, reply, peeraddress);
	if (touch_net_trace.integer)
		Con_Printf("NETTRACE sv %.3f accept window=%d\n", Sys_DirtyTime() * 1000.0, t->window);
	return true;
}

/*
==============
Sending
==============
*/

static int TN_CryptoFlag(netconn_t *conn)
{
	// as NetConn_AddCryptoFlag: resends differ on an encrypted connection
	int flag = 0, r;
	if (conn->crypto.authenticated)
	{
		r = rand() % 7 + 1;
		if (r & 1)
			flag |= NETFLAG_CRYPTO0;
		if (r & 2)
			flag |= NETFLAG_CRYPTO1;
		if (r & 4)
			flag |= NETFLAG_CRYPTO2;
	}
	return flag;
}

static double TN_RTO(tn_conn_t *t)
{
	double rto;
	if (t->srtt <= 0)
		rto = 0.5;
	else
	{
		// twice the round trip, or the RFC 6298 bound when it varies more;
		// plus a server tick for the frame the peer answers in
		rto = max(2 * t->srtt, t->srtt + 4 * t->rttvar) + 0.016;
	}
	rto *= (double)(1 << t->backoff);
	return bound(net_reliable_rto_min.value, rto, max(net_reliable_rto_min.value, net_reliable_rto_max.value));
}

// fragments allowed in flight: the window, unless the rate the peer asked
// for (bytes per second) over a round trip is less, but never under 4
static int TN_EffectiveWindow(tn_conn_t *t)
{
	int w = t->window;
	if (t->rate > 0 && t->srtt > 0)
	{
		int bdp = (int)(t->rate * t->srtt * 2 / MAX_PACKETFRAGMENT) + 2;
		w = min(w, max(4, bdp));
	}
	return w;
}

static void TN_Cleartime(netconn_t *conn, int rate, int burstsize, int len)
{
	// as NetConn_UpdateCleartime, for fragments sent outside
	// NetConn_SendUnreliableMessage (on an acknowledgement)
	double bursttime;
	if (rate <= 0)
		return;
	bursttime = burstsize / (double)rate;
	if (conn->cleartime < host.realtime - bursttime)
		conn->cleartime = host.realtime - bursttime;
	conn->cleartime += len / (double)rate;
}

static int TN_SendFragment(tn_conn_t *t, unsigned int seq)
{
	netconn_t *conn = t->conn;
	tn_frag_t *f = &t->ring[seq % TN_RING];
	unsigned char sendbuffer[NET_HEADERSIZE + MAX_PACKETFRAGMENT];
	unsigned char cryptosendbuffer[NET_HEADERSIZE + MAX_PACKETFRAGMENT + CRYPTO_HEADERSIZE];
	unsigned int packetLen = NET_HEADERSIZE + f->len;
	const void *sendme;
	size_t sendmelen = 0;
	qbool resend = (int)(seq - t->sendnext) < 0;

	StoreBigLong(sendbuffer, packetLen | NETFLAG_DATA | (f->eom ? NETFLAG_EOM : 0) | TN_CryptoFlag(conn));
	StoreBigLong(sendbuffer + 4, seq);
	memcpy(sendbuffer + NET_HEADERSIZE, f->data, f->len);
	conn->outgoing_netgraph[conn->outgoing_packetcounter].reliablebytes += packetLen + 28;
	sendme = Crypto_EncryptPacket(&conn->crypto, sendbuffer, packetLen, cryptosendbuffer, &sendmelen, sizeof(cryptosendbuffer));
	if (sendme)
		NetConn_Write(conn->mysocket, sendme, (int)sendmelen, &conn->peeraddress);
	if (resend)
	{
		f->resent = 1;
		conn->packetsReSent++;
	}
	else
		conn->packetsSent++;
	f->sent = Sys_DirtyTime();
	conn->lastSendTime = host.realtime;
	return (int)sendmelen + 28;
}

static int TN_FillWindow(tn_conn_t *t)
{
	int total = 0, w = TN_EffectiveWindow(t);
	while (t->sendnext != t->tail && (int)(t->sendnext - t->base) < w)
	{
		total += TN_SendFragment(t, t->sendnext);
		t->sendnext++;
	}
	t->conn->nq.sendSequence = t->sendnext;
	t->conn->nq.ackSequence = t->base;
	return total;
}

// go back to the oldest unacknowledged fragment and send what was in flight
static int TN_ResendWindow(tn_conn_t *t)
{
	unsigned int seq;
	int total = 0;
	for (seq = t->base; seq != t->sendnext; seq++)
		total += TN_SendFragment(t, seq);
	return total;
}

static qbool TN_Queue(tn_conn_t *t, const unsigned char *data, int length)
{
	int nfrags = (length + MAX_PACKETFRAGMENT - 1) / MAX_PACKETFRAGMENT, i, len;
	tn_frag_t *f;
	if ((int)(t->tail - t->base) + nfrags > TN_RING)
		return false;
	for (i = 0; i < nfrags; i++)
	{
		len = min(length - i * MAX_PACKETFRAGMENT, MAX_PACKETFRAGMENT);
		f = &t->ring[t->tail % TN_RING];
		f->len = len;
		f->eom = i == nfrags - 1;
		f->resent = 0;
		f->sent = 0;
		memcpy(f->data, data + i * MAX_PACKETFRAGMENT, len);
		t->tail++;
	}
	return true;
}

int TouchNet_SendReliable(netconn_t *conn, qbool suppress, int rate, int burstsize, int *totallen)
{
	tn_conn_t *t = TN_Find(conn);
	if (!t)
		return 0;
	t->rate = rate;
	t->burstsize = burstsize;
	// the stock sender below never sends: its buffer stays empty, which also
	// tells sv_send.c a new reliable message may go into the demo now
	conn->sendMessageLength = 0;
	if (!suppress && conn->message.cursize)
	{
		if (developer_networking.integer && conn == cls.netcon)
		{
			Con_Print("client sending reliable message to server:\n");
			SZ_HexDumpToConsole(&conn->message);
		}
		if (!TN_Queue(t, conn->message.data, conn->message.cursize))
		{
			// as when the stock buffer overflows: the server drops the client
			Con_Printf("NetConn_SendUnreliableMessage: reliable queue full (%d fragments pending)\n", (int)(t->tail - t->base));
			conn->message.overflowed = true;
			return -1;
		}
		SZ_Clear(&conn->message);
		conn->reliableMessagesSent++;
	}
	if (t->base != t->sendnext && Sys_DirtyTime() - t->ring[t->base % TN_RING].sent > TN_RTO(t))
	{
		*totallen += TN_ResendWindow(t);
		t->timeouts++;
		t->backoff = min(t->backoff + 1, 4);
		t->recovery = true;
		t->recover = t->sendnext - 1;
		t->dupacks = 0;
	}
	*totallen += TN_FillWindow(t);
	return 0;
}

/*
==============
Receiving
==============
*/

qbool TouchNet_Handles(netconn_t *conn, unsigned int flags)
{
	return (flags & (NETFLAG_ACK | NETFLAG_DATA)) && TN_Find(conn);
}

static void TN_SendAck(netconn_t *conn, unsigned int sequence)
{
	unsigned char temppacket[8];
	unsigned char cryptosendbuffer[8 + CRYPTO_HEADERSIZE];
	const void *sendme;
	size_t sendmelen;
	conn->outgoing_netgraph[conn->outgoing_packetcounter].ackbytes += 8 + 28;
	StoreBigLong(temppacket, 8 | NETFLAG_ACK | TN_CryptoFlag(conn));
	StoreBigLong(temppacket + 4, sequence);
	sendme = Crypto_EncryptPacket(&conn->crypto, temppacket, 8, cryptosendbuffer, &sendmelen, sizeof(cryptosendbuffer));
	if (sendme)
		NetConn_Write(conn->mysocket, sendme, (int)sendmelen, &conn->peeraddress);
}

static void TN_Heard(netconn_t *conn, double newtimeout)
{
	conn->lastMessageTime = host.realtime;
	conn->lastMessageDirtyTime = Sys_DirtyTime();
	conn->timeout = host.realtime + newtimeout;
}

static int TN_Ack(tn_conn_t *t, unsigned int a, double newtimeout)
{
	netconn_t *conn = t->conn;
	tn_frag_t *f;
	double r, now;
	int total = 0;
	if ((int)(a - t->base) >= 0 && (int)(a - t->sendnext) < 0)
	{
		// new data acknowledged, up to and including a
		f = &t->ring[a % TN_RING];
		now = Sys_DirtyTime();
		if (!f->resent && f->sent > 0)
		{
			r = now - f->sent;
			if (t->srtt <= 0)
			{
				t->srtt = r;
				t->rttvar = r / 2;
			}
			else
			{
				t->rttvar = 0.75 * t->rttvar + 0.25 * fabs(t->srtt - r);
				t->srtt = 0.875 * t->srtt + 0.125 * r;
			}
		}
		t->base = a + 1;
		t->dupacks = 0;
		t->backoff = 0;
		TN_Heard(conn, newtimeout);
		if (t->recovery)
		{
			if ((int)(a - t->recover) >= 0)
				t->recovery = false;
			else if (t->base != t->sendnext)
				total += TN_SendFragment(t, t->base); // a partial ack: the next hole
		}
		total += TN_FillWindow(t);
	}
	else if (a == t->base - 1 && t->base != t->sendnext)
	{
		// the peer has a gap at base
		TN_Heard(conn, newtimeout);
		if (++t->dupacks == 3 && !t->recovery)
		{
			total += TN_SendFragment(t, t->base);
			t->fastretransmits++;
			t->recovery = true;
			t->recover = t->sendnext - 1;
		}
	}
	// anything else is stale or bogus: ignored
	conn->nq.ackSequence = t->base;
	TN_Cleartime(conn, t->rate, t->burstsize, total);
	return 1;
}

static qbool TN_Ready(tn_conn_t *t, const unsigned char *data, int length)
{
	if (!t->ready)
		t->ready = (unsigned char *)Mem_Alloc(tn_mempool, TN_READYMAX);
	if (t->readypos == t->readylen)
		t->readypos = t->readylen = 0;
	if (t->readylen + 4 + length > TN_READYMAX && t->readypos)
	{
		memmove(t->ready, t->ready + t->readypos, t->readylen - t->readypos);
		t->readylen -= t->readypos;
		t->readypos = 0;
	}
	if (t->readylen + 4 + length > TN_READYMAX)
		return false;
	StoreLittleLong(t->ready + t->readylen, length);
	memcpy(t->ready + t->readylen + 4, data, length);
	t->readylen += 4 + length;
	return true;
}

// the oldest complete message into cl_message / sv_message
static qbool TN_Deliver(tn_conn_t *t)
{
	int length;
	sizebuf_t *msg;
	if (!t->ready || t->readypos >= t->readylen)
		return false;
	length = BuffLittleLong(t->ready + t->readypos);
	msg = t->conn == cls.netcon ? &cl_message : &sv_message;
	SZ_Clear(msg);
	SZ_Write(msg, t->ready + t->readypos + 4, length);
	MSG_BeginReading(msg);
	t->readypos += 4 + length;
	return true;
}

static void TN_Append(tn_conn_t *t, const unsigned char *data, int length, qbool eom)
{
	netconn_t *conn = t->conn;
	if (conn->receiveMessageLength + length <= (int)sizeof(conn->receiveMessage))
	{
		memcpy(conn->receiveMessage + conn->receiveMessageLength, data, length);
		conn->receiveMessageLength += length;
	}
	else
	{
		Con_Printf("Reliable message (seq: %u) too big for message buffer!\nDropping the message!\n", conn->nq.receiveSequence);
		conn->receiveMessageLength = 0;
		return;
	}
	if (eom)
	{
		conn->reliableMessagesReceived++;
		if (conn->receiveMessageLength > 0 && !TN_Ready(t, conn->receiveMessage, conn->receiveMessageLength))
			Con_Printf("Reliable messages arrived faster than they were read: one dropped\n");
		conn->receiveMessageLength = 0;
	}
}

static int TN_Data(tn_conn_t *t, unsigned int flags, unsigned int sequence, const unsigned char *data, size_t length, double newtimeout)
{
	netconn_t *conn = t->conn;
	int d = (int)(sequence - conn->nq.receiveSequence), slot;
	if (length > MAX_PACKETFRAGMENT)
	{
		// bigger than any fragment a sender makes: not ours to take
		TN_SendAck(conn, conn->nq.receiveSequence - 1);
		return 1;
	}
	if (d < 0)
	{
		conn->receivedDuplicateCount++;
		t->dupdata++;
	}
	else if (d >= t->window)
	{
		// beyond what the window allows: dropped, the sender sends it again
	}
	else if (d > 0)
	{
		slot = sequence % TN_MAXWINDOW;
		if (!(t->rxflags[slot] & 1))
		{
			memcpy(t->rxdata[slot], data, length);
			t->rxlen[slot] = (unsigned short)length;
			t->rxflags[slot] = 1 | ((flags & NETFLAG_EOM) ? 2 : 0);
			t->heldahead++;
		}
	}
	else
	{
		TN_Heard(conn, newtimeout);
		TN_Append(t, data, (int)length, (flags & NETFLAG_EOM) != 0);
		conn->nq.receiveSequence++;
		// fragments that waited behind this one
		for (;;)
		{
			slot = conn->nq.receiveSequence % TN_MAXWINDOW;
			if (!(t->rxflags[slot] & 1))
				break;
			TN_Append(t, t->rxdata[slot], t->rxlen[slot], (t->rxflags[slot] & 2) != 0);
			t->rxflags[slot] = 0;
			conn->nq.receiveSequence++;
		}
	}
	// cumulative: everything before receiveSequence is here
	TN_SendAck(conn, conn->nq.receiveSequence - 1);
	return TN_Deliver(t) ? 2 : 1;
}

int TouchNet_Received(netconn_t *conn, unsigned int flags, unsigned int sequence, const unsigned char *data, size_t length, int originallength, double newtimeout)
{
	tn_conn_t *t = TN_Find(conn);
	if (!t)
		return 0;
	if (flags & NETFLAG_ACK)
	{
		conn->incoming_netgraph[conn->incoming_packetcounter].ackbytes += originallength + 28;
		return TN_Ack(t, sequence, newtimeout);
	}
	conn->incoming_netgraph[conn->incoming_packetcounter].reliablebytes += originallength + 28;
	return TN_Data(t, flags, sequence, data, length, newtimeout);
}

void TouchNet_ClientAfterParse(int ret)
{
	tn_conn_t *t;
	TN_TraceSignon();
	// CL_ParseServerMessage may disconnect: look the connection up each time
	while (ret == 2 && cls.netcon && (t = TN_Find(cls.netcon)) && TN_Deliver(t))
	{
		CL_ParseServerMessage();
		TN_TraceSignon();
	}
	// during the signon the answer (spawn, begin, a sentcvar) goes out now
	if (ret == 2 && net_signon_fast.integer && cls.netcon && cls.state == ca_connected && cls.signon < SIGNONS
	 && cls.netcon->message.cursize && !cls.netcon->sendMessageLength && cls.protocol != PROTOCOL_QUAKEWORLD)
	{
		sizebuf_t empty;
		unsigned char buf[4];
		memset(&empty, 0, sizeof(empty));
		empty.data = buf;
		empty.maxsize = sizeof(buf);
		NetConn_SendUnreliableMessage(cls.netcon, &empty, cls.protocol, cl_rate.integer, cl_rate_burstsize.integer, false);
	}
}

void TouchNet_ServerAfterRead(void)
{
	client_t *client = host_client;
	netconn_t *conn;
	tn_conn_t *t;
	sizebuf_t empty;
	unsigned char buf[4];
	int maxrate, clientrate;

	while (client && client->active && (conn = client->netconnection) && (t = TN_Find(conn)) && TN_Deliver(t))
	{
		SV_ReadClientMessage();
		host_client = client;
	}
	// a client in its signon gets the answer to its request (the signon data
	// after prespawn, the client data after spawn) now, not at the end of
	// the next server tick; what sv_send.c does for the reliable part of
	// SV_SendClientDatagram
	if (!net_signon_fast.integer || !client || !client->active || client->begun || !(conn = client->netconnection))
		return;
	if (!conn->message.cursize || client->sendsignon == 2 || conn->sendMessageLength || sv.protocol == PROTOCOL_QUAKEWORLD)
		return;
	maxrate = max(NET_MINRATE, sv_maxrate.integer);
	clientrate = bound(NET_MINRATE, client->rate, maxrate);
	SV_WriteDemoMessage(client, &conn->message, false);
	memset(&empty, 0, sizeof(empty));
	empty.data = buf;
	empty.maxsize = sizeof(buf);
	NetConn_SendUnreliableMessage(conn, &empty, sv.protocol, clientrate, client->rate_burstsize, false);
	if (client->sendsignon == 1 && !conn->message.cursize)
		client->sendsignon = 2;
}

/*
==============
Signon speed
==============
*/

static qbool TN_ClientInSignon(void)
{
	return cls.state == ca_connected && cls.netcon && cls.signon < SIGNONS && !cls.demoplayback;
}

static qbool TN_ServerHasSignon(void)
{
	int i;
	if (!sv.active)
		return false;
	for (i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].active && svs.clients[i].netconnection && !svs.clients[i].begun)
			return true;
	return false;
}

qbool TouchNet_ClientUrgent(void)
{
	return net_signon_fast.integer && TN_ClientInSignon() && cls.netcon->message.cursize && !cls.netcon->sendMessageLength;
}

qbool TouchNet_FastSignon(void)
{
	// an HTTP download is paced by curl's own select (Sys_Sleep)
	return net_signon_fast.integer && cls.state != ca_dedicated && TN_ClientInSignon() && !cls.timedemo && !Curl_Running();
}

double TouchNet_Sleep(double sleeptime)
{
#ifdef TOUCHNET_SELECT
	fd_set fds;
	struct timeval tv;
	lhnetsocket_t *s;
	int maxfd = -1;
	double wait;

	if (!net_signon_fast.integer)
		return sleeptime;
	if (TouchNet_FastSignon())
	{
		// commands a packet queued run next frame, our own server answers
		// within the frame: no wait for those
		if (!List_Is_Empty(&cmd_local->cbuf->start) || cls.netcon->message.cursize || sv.active)
			return 0;
		// a remote server: until its next packet, at most 5 ms (the loading
		// screen and the console keep going)
		wait = 0.005;
	}
	else if (TN_ServerHasSignon() && sleeptime > 0)
		wait = min(sleeptime, 0.1); // until the next tick, or a client's packet
	else
		return sleeptime;
	FD_ZERO(&fds);
	List_For_Each_Entry(s, &lhnet_socketlist.list, lhnetsocket_t, list)
	{
		if ((s->address.addresstype == LHNETADDRESSTYPE_INET4 || s->address.addresstype == LHNETADDRESSTYPE_INET6) && s->inetsocket >= 0 && s->inetsocket < FD_SETSIZE)
		{
			FD_SET(s->inetsocket, &fds);
			if (s->inetsocket > maxfd)
				maxfd = s->inetsocket;
		}
	}
	if (maxfd < 0)
		return min(sleeptime, wait);
	tv.tv_sec = 0;
	tv.tv_usec = (long)(wait * 1000000.0);
	select(maxfd + 1, &fds, NULL, NULL, &tv);
	return 0;
#else
	return sleeptime;
#endif
}

static qbool TN_Counting(void)
{
	return cls.connect_trying || TN_ClientInSignon() || (cls.state == ca_connected && tn_last_signon >= 0 && tn_last_signon < SIGNONS);
}

// client frame work of this join so far, the running frame included
static double TN_WorkMs(void)
{
	return (tn_frame_work + (TN_Counting() ? Sys_DirtyTime() - host.dirtytime : 0)) * 1000.0;
}

static void TN_TraceSignon(void)
{
	if (touch_net_trace.integer && cls.state == ca_connected && cls.signon != tn_last_signon)
	{
		Con_Printf("NETTRACE cl %.3f signon %d work=%.1f\n", Sys_DirtyTime() * 1000.0, cls.signon, TN_WorkMs());
		tn_last_signon = cls.signon;
	}
}

void TouchNet_Frame(double worktime)
{
	if (!touch_net_trace.integer || cls.state == ca_dedicated)
		return;
	TN_TraceSignon();
	if (TN_Counting())
		tn_frame_work += worktime;
	if (cls.state != ca_connected)
		tn_last_signon = -1;
	// a new join counts from zero
	if (cls.state != ca_connected && !cls.connect_trying)
		tn_frame_work = 0;
}

/*
==============
Trace
==============
*/

void TouchNet_TracePacket(lhnetsocket_t *mysocket, const void *data, int length, const lhnetaddress_t *peeraddress, int sent)
{
	const unsigned char *d = (const unsigned char *)data;
	unsigned int head, seq;
	const char *side, *kind;
	char text[96];
	int i, n, o;
	if (!touch_net_trace.integer || length < 4)
		return;
	if (cls.state != ca_dedicated
	 && ((TN_ClientInSignon() && mysocket == cls.netcon->mysocket && !LHNETADDRESS_Compare(peeraddress, &cls.netcon->peeraddress))
	  || (cls.connect_trying && !LHNETADDRESS_Compare(peeraddress, &cls.connect_address))))
		side = "cl";
	else if (sv.active && TN_ServerHasSignon())
		side = "sv";
	else
		return;
	if (d[0] == 255 && d[1] == 255 && d[2] == 255 && d[3] == 255)
	{
		n = min(length - 4, (int)sizeof(text) - 1);
		for (i = 0; i < n && d[4 + i] >= 32 && d[4 + i] < 127 && d[4 + i] != '\\'; i++)
			text[i] = d[4 + i];
		text[i] = 0;
		Con_Printf("NETTRACE %s %.3f %s oob %d %s w=%.1f\n", side, Sys_DirtyTime() * 1000.0, sent ? "tx" : "rx", length, text[0] ? text : "-", TN_WorkMs());
		return;
	}
	if (length < 8)
		return;
	head = BuffBigLong(d);
	seq = BuffBigLong(d + 4);
	if (head & NETFLAG_ACK)
		kind = "ack";
	else if (head & NETFLAG_DATA)
		kind = (head & NETFLAG_EOM) ? "data_eom" : "data";
	else if (head & NETFLAG_UNRELIABLE)
		kind = "unrel";
	else
		kind = "other";
	text[0] = 0;
	// the client's string commands (prespawn, spawn, begin, sentcvar ...):
	// the first word of each, comma separated
	if (sent && !strcmp(side, "cl") && (head & NETFLAG_DATA))
	{
		for (i = 8, n = 0; i < length && d[i] == clc_stringcmd; )
		{
			i++;
			if (n && n < (int)sizeof(text) - 1)
				text[n++] = ',';
			// the command's first word; its arguments are left out
			for (o = 1; i < length && d[i]; i++)
			{
				if (d[i] <= 32)
					o = 0;
				else if (o && d[i] < 127 && n < (int)sizeof(text) - 1)
					text[n++] = d[i];
			}
			i++;
		}
		text[n] = 0;
	}
	Con_Printf("NETTRACE %s %.3f %s %s %d %u %s w=%.1f\n", side, Sys_DirtyTime() * 1000.0, sent ? "tx" : "rx", kind, length, seq, text[0] ? text : "-", TN_WorkMs());
}

/*
==============
HTTP downloads
==============
*/

#define TN_CURLSLOTS 16
static double tn_curl_bytes[TN_CURLSLOTS];

void TouchNet_CurlProgress(int index, double bytes)
{
	if (index >= 0 && index < TN_CURLSLOTS)
		tn_curl_bytes[index] = bytes;
}

static void TN_Size(char *out, size_t size, double bytes)
{
	if (bytes >= 1048576.0)
		dpsnprintf(out, size, "%.1f MB", bytes / 1048576.0);
	else
		dpsnprintf(out, size, "%.0f KB", bytes / 1024.0);
}

void TouchNet_CurlText(char *text, size_t size, int index, const char *filename, qbool queued, double progress, double speed)
{
	char have[32], total[32], left[32];
	double bytes, all, secs;
	if (queued || index < 0 || index >= TN_CURLSLOTS || progress <= 0 || progress > 1)
		return;
	// Curl_GetDownloadAmount: (startpos + received) / (startpos + length)
	bytes = tn_curl_bytes[index];
	if (bytes <= 0)
		return;
	all = bytes / progress;
	TN_Size(have, sizeof(have), bytes);
	TN_Size(total, sizeof(total), all);
	if (speed > 1)
	{
		secs = (all - bytes) / speed;
		if (secs >= 3600)
			dpsnprintf(left, sizeof(left), ", %d h %02d min left", (int)(secs / 3600), (int)(secs / 60) % 60);
		else
			dpsnprintf(left, sizeof(left), ", %d:%02d left", (int)(secs / 60), (int)secs % 60);
	}
	else
		left[0] = 0;
	dpsnprintf(text, size, "Downloading %s ...  %s of %s (%.0f%%) @ %.1f KiB/s%s", filename, have, total, 100.0 * progress, speed / 1024.0, left);
}

static void TouchNet_Status_f(cmd_state_t *cmd)
{
	tn_conn_t *t;
	for (t = tn_list; t; t = t->next)
		Con_Printf("%-21s window=%d inflight=%u queued=%u srtt=%.1fms rto=%.0fms fastretx=%d timeouts=%d held=%d dup=%d\n",
			t->conn->address, t->window, t->sendnext - t->base, t->tail - t->sendnext, t->srtt * 1000.0, TN_RTO(t) * 1000.0,
			t->fastretransmits, t->timeouts, t->heldahead, t->dupdata);
}

void TouchNet_Init(void)
{
	Cvar_RegisterVariable(&net_reliable_window);
	Cvar_RegisterVariable(&net_reliable_rto_min);
	Cvar_RegisterVariable(&net_reliable_rto_max);
	Cvar_RegisterVariable(&net_signon_fast);
	Cvar_RegisterVariable(&touch_net_trace);
	TouchCurl_Init();
	Cmd_AddCommand(CF_SHARED, "net_reliable_status", TouchNet_Status_f, "windowed reliable connections: window, round trip, retransmits (Godnotic)");
}
