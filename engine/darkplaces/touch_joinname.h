#ifndef TOUCH_JOINNAME_H
#define TOUCH_JOINNAME_H

// Godnotic: the name of the server a join is for, so every screen of the join
// can say "Joining <name> (<address>)" (docs/UI_BRIDGE.md 4.13). The name
// comes from the server list when the server is in it, else from the server
// itself (a getinfo answered with its hostname), so a typed address gets its
// name a moment later. Colour codes are stripped and a long name is cut with
// an ellipsis. A local game has no name: it clears it.
//
// Published as _touch_join_address, _touch_join_name and _touch_join_label
// ("name (address)", or the address alone while no name is known), and in the
// native front end's connect snapshot (touch_frontend.c).

#include "lhnet.h"

void TouchJoinName_Init(void);
// once per client frame (from TouchJoinFail_Frame, touch_joinfail.c)
void TouchJoinName_Frame(void);
// the name of the server at addr ("" when unknown or another server)
const char *TouchJoinName_For(const char *addr);
// "name (addr)" when the name of addr is known, else addr
const char *TouchJoinName_Label(const char *addr, char *buf, size_t size);
// the answer to our own getinfo: true when the packet was ours (netconn.c drops it,
// so a typed server never lands in the server list)
qbool TouchJoinName_Connectionless(const char *string, int length, lhnetaddress_t *peeraddress);
// a name as the join screens show it: colour codes out, one line, at most
// TOUCHJOINNAME_MAXCHARS characters ending in an ellipsis
void TouchJoinName_Clean(const char *in, char *out, size_t outsize);

#define TOUCHJOINNAME_MAXCHARS 32

#endif
