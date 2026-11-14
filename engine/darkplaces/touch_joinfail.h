#ifndef TOUCH_JOINFAIL_H
#define TOUCH_JOINFAIL_H

// Godnotic: a join that fails says why, and can be retried (docs/NETWORK.md,
// "When a join fails"). Called from touch_lan.c: no hook of its own in an
// upstream file.

void TouchJoinFail_Init(void);
// once per client frame, before NetConn_ClientFrame sends its next request
void TouchJoinFail_Frame(void);
// a join is being attempted or is in its signon: the server list stays quiet
qbool TouchJoinFail_Joining(void);

#endif
