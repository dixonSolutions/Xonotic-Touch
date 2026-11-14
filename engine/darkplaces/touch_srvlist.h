#ifndef TOUCH_SRVLIST_H
#define TOUCH_SRVLIST_H

// Godnotic: the fast public server list (docs/NETWORK.md, "The server list").
//
// The stock list waits for the masters (up to net_slist_timeout, 4 s, when
// one is silent), then asks 128 servers a second, every server three times,
// and shows nothing until answers come. This one shows the last known list in
// the first frame, asks each server the moment a master names it, at a rate
// tied to the clock and not to the frame rate, asks the silent ones again
// instead of everyone, and measures itself (net_slist_stats).
//
// Hooks in netconn.c (each one line, marked // Touch:):
//   ServerList_QueryList        TouchSrvList_Refresh(resetcache)
//   NetConn_QueryQueueFrame     if (TouchSrvList_QueryFrame()) return;
//   ..._ServerList_ProcessReply TouchSrvList_Replied(n)
//   and three accessors to its static helpers, defined there:

struct serverlist_entry_s;
void TouchSrvList_ViewInsert(struct serverlist_entry_s *entry);
void TouchSrvList_ViewRemove(struct serverlist_entry_s *entry);
// adds an address to the list unless it is there; false when the list is full
qbool TouchSrvList_AddServer(const char *address, qbool isfavorite);

void TouchSrvList_Init(void);
// touch_lan.c: once per client frame; once when the file system is up
void TouchSrvList_Frame(void);
void TouchSrvList_FirstFrame(void);
// a refresh was started (masters asked, LAN broadcast sent)
void TouchSrvList_Refresh(qbool resetcache);
// true: the fast list sent this frame's queries, the stock scheduler stays out
qbool TouchSrvList_QueryFrame(void);
// server n answered and its ping is in
void TouchSrvList_Replied(unsigned n);
// net_slist_fast is on (touch_lan.c: its own address cache stands down)
qbool TouchSrvList_Fast(void);
// row n is shown from the saved list and has not answered yet this refresh
qbool TouchSrvList_IsStale(unsigned n);
// a finger is on the menu: lists keep their rows where they are
qbool TouchSrvList_Hold(void);
// changes whenever a row was added, removed, answered or went stale
unsigned TouchSrvList_Serial(void);

#endif
