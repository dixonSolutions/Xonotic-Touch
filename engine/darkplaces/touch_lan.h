#ifndef TOUCH_LAN_H
#define TOUCH_LAN_H

/*
Godnotic LAN games: hosting a listen server for the local network, finding
games there without a master server, and an optional join password.
See touch_lan.c and docs/LAN.md. Every function is called from one hook
line in netconn.c marked "// Touch:".
*/

struct lhnetsocket_s;
struct lhnetaddress_s;

// NetConn_Init: cvars and commands.
void TouchLAN_Init(void);
// NetConn_ClientFrame: repeated LAN queries, the host's status cvars.
void TouchLAN_Frame(void);

// ServerList_QueryList, once per client socket after the stock
// 255.255.255.255:26000 query: subnet-directed broadcasts on every interface,
// the other LAN ports, the cached servers, and a few repeats.
void TouchLAN_Broadcast(struct lhnetsocket_s *sock);
// ServerList_QueryList: true when the list wants LAN games only, so the
// master servers are not asked.
qbool TouchLAN_LanOnly(void);
// netconn.c's ServerList_BuildDPServerQuery, exported for the repeats.
void TouchLAN_BuildQuery(char *buffer, size_t buffersize, double querytime);

// Client, answering a server's challenge: add the password proof to the
// connect string when the player gave a password for this server.
void TouchLAN_AppendConnect(char *connect, size_t size, const char *challenge, const struct lhnetaddress_s *peer);
// Client: the server rejected the connection. A password problem opens the
// menu's password prompt.
void TouchLAN_Rejected(const char *reason, int length, const struct lhnetaddress_s *peer);

// Server, on a connect request: true when it was refused for want of the
// right password (the reject is sent here).
qbool TouchLAN_RefuseJoin(struct lhnetsocket_s *sock, struct lhnetaddress_s *peer, const char *connect);
// NetConn_OpenServerPorts: a new server starts (not a changelevel).
void TouchLAN_ServerPortsOpened(void);
// The running local game was started from the Host screen (a LAN game the
// player hosts on purpose), and how many humans other than the local player
// are in it: what keeps it running in the background on Android.
qbool TouchLAN_HostingOnPurpose(void);
int TouchLAN_OtherHumans(void);

// touch_net.c: a connection was accepted; a LAN or VPN game is remembered
// (touch_lan_hosts) and asked on every LAN search.
void TouchLAN_Joined(const struct lhnetaddress_s *peer);

// Server, building a getinfo/getstatus reply: mark a password-protected game.
void TouchLAN_StatusFlags(char *qcstatus, size_t size);

#endif
