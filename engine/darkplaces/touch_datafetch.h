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

/// True while a join waits for its map: the join did not fail (touch_joinfail.c).
qbool TouchData_JoinPending(void);

/// For map lists (touch_uib_src.c, the native front ends): 0 the map can be
/// played, 1 its pack is still downloading (`progress` 0..1 when it is the
/// wanted map, else -1), 2 it is not here and nothing is fetching it.
int TouchData_MapState(const char *map, float *progress);

/// Changes whenever a map list built from TouchData_MapState is out of date
/// (a pack was mounted, the downloader started or stopped, the wanted map
/// moved on by a quarter MiB).
unsigned TouchData_Generation(void);

#endif
