// Godnotic: the game's half of the progressive data download
// (docs/DATA_FETCH.md, "Talking to the running game").
#ifndef TOUCH_DATAFETCH_H
#define TOUCH_DATAFETCH_H

void TouchData_Init(void);  // Host_Init
void TouchData_Frame(void); // Host_Frame, after Cbuf_Frame

/// cl_parse.c through touch_join.h: the server's world did not load. True when
/// it is a map of the data set that is still to come; the map is asked for and
/// the join carries on by itself when it is here. `text` gets what to tell
/// the player.
qbool TouchData_JoinNeedsMap(const char *worldmodel, char *text, size_t textsize);

#endif
