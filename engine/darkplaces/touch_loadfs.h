#ifndef TOUCH_LOADFS_H
#define TOUCH_LOADFS_H

// Godnotic: cached directory listings for FS_FindFile's loose directories
// (touch_fs_dirindex, see touch_loadfs.c).

// False when name (relative, '/'-separated) cannot exist under dirpath (an
// absolute directory path ending in '/'); true when it may, and stat() decides.
qbool TouchLoadFS_MayHave(const char *dirpath, const char *name);
// The engine created, wrote or removed a file: listings are read again.
void TouchLoadFS_Changed(void);
void TouchLoadFS_Init(void);

#endif
