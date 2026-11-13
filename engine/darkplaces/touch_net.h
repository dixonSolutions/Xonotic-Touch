#ifndef TOUCH_NET_H
#define TOUCH_NET_H

// Godnotic: faster joins over real networks (docs/NETWORK.md).
//
// A windowed reliable channel between Godnotic peers (negotiated with
// "\rwin\N" in connect/accept, the convention Insanotic uses too), a signon
// that answers as soon as a packet arrives instead of at the next frame or
// server tick, and a packet trace of the join (touch_net_trace 1).
//
// Every function here is a no-op for a connection that did not negotiate the
// window, so a stock DarkPlaces or Xonotic peer sees exactly the old protocol.

struct netconn_s;
struct lhnetsocket_s;
struct lhnetaddress_s;

void TouchNet_Init(void);

// handshake (netconn.c)
// client: add "\rwin\N" to the connect string
void TouchNet_AppendConnect(char *connect, size_t size);
// client: an "accept\rwin\N" becomes a plain "accept" (what crypto and the
// stock parser check) and the window is kept for TouchNet_Established;
// returns the new length
int TouchNet_ClientConnectionless(char *string, int length);
// client: cls.netcon was just opened by an accept
void TouchNet_Established(struct netconn_s *conn);
// server: answer a connect that offered a window with "accept\rwin\N";
// false = send the stock "accept"
qbool TouchNet_ServerAccept(struct netconn_s *conn, struct lhnetsocket_s *mysocket, struct lhnetaddress_s *peeraddress, const char *connectstring);

// reliable channel (netconn.c)
// NetConn_SendUnreliableMessage: queue conn->message and keep the window
// full; -1 when the queue overflowed (conn->message.overflowed is set)
int TouchNet_SendReliable(struct netconn_s *conn, qbool suppress, int rate, int burstsize, int *totallen);
// NetConn_ReceivedMessage: does the windowed channel take this data/ack packet?
qbool TouchNet_Handles(struct netconn_s *conn, unsigned int flags);
int TouchNet_Received(struct netconn_s *conn, unsigned int flags, unsigned int sequence, const unsigned char *data, size_t length, int originallength, double newtimeout);
void TouchNet_Close(struct netconn_s *conn);
// after a message was parsed: the next one a filled gap completed, if any
void TouchNet_ClientAfterParse(int ret);
void TouchNet_ServerAfterRead(void);

// signon speed
// cl_input.c CL_SendMove: send a reliable message now during the signon
qbool TouchNet_ClientUrgent(void);
// touch_loadtime.c: the client has no frame limiter during the signon
qbool TouchNet_FastSignon(void);
// touch_loadtime.c (end of the host frame): wait for a packet instead of
// sleeping blind while a signon runs; returns what is left to sleep
double TouchNet_Sleep(double sleeptime);
// touch_loadtime.c: once per host frame, with the frame's work time
void TouchNet_Frame(double worktime);

// HTTP downloads on the loading screen (libcurl.c, cl_screen.c): the bytes
// a download has, and its line with the size and the time left
void TouchNet_CurlProgress(int index, double bytes);
// touch_curl.h (in libcurl.c): curl_segments and curl_segment_size
void TouchCurl_Init(void);
void TouchNet_CurlText(char *text, size_t size, int index, const char *filename, qbool queued, double progress, double speed);

// sv_send.c: keep a client's datagrams inside its path MTU (a VPN's 1280,
// say) so they are never IP fragments; net_mtu sets it by hand
void TouchNet_ClampDatagram(struct netconn_s *conn, int *maxsize, int *maxsize2);

// cl_main.c CL_DisconnectEx: a lost connection is joined again
void TouchNet_Disconnecting(qbool kicked, const char *reason);
// touch_android.c: the app went to the background / came back
void TouchNet_Away(qbool away);
// what the automatic reconnect is doing, or NULL when nothing
const char *TouchNet_ReconnectState(void);

// trace (NetConn_Read / NetConn_Write)
void TouchNet_TracePacket(struct lhnetsocket_s *mysocket, const void *data, int length, const struct lhnetaddress_s *peeraddress, int sent);

#endif
