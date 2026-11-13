// Touch: zstd pk3 entries (zip method 93), included by fs.c after its pack
// and qfile structures so the two hooks there stay one line each:
//
//   PK3_BuildFileList:  flags = Touch_PK3_EntryFlags(method, flags);
//   FS_OpenPackedFile:  if (pfile->flags & PACKFILE_FLAG_ZSTD) return Touch_FS_OpenZstd(file, pfile);
//
// A zstd entry is decoded whole when it is opened and handed back as a data
// file. The engine loads nearly every packed file whole anyway (FS_LoadFile),
// and what streams (music, which the repack stores uncompressed) never takes
// this path. That keeps FS_Read, FS_Seek and FS_Close untouched.

#ifndef TOUCH_ASSETS_FS_H
#define TOUCH_ASSETS_FS_H

#include "touch_assets.h"

extern mempool_t *fs_mempool; // defined further down in fs.c

/// file compressed using zstd (zip method 93)
#define PACKFILE_FLAG_ZSTD (1 << 3)

static int Touch_PK3_EntryFlags(unsigned short method, int flags)
{
	if (method == TOUCH_ZIP_METHOD_ZSTD)
		return (flags & ~PACKFILE_FLAG_DEFLATED) | PACKFILE_FLAG_ZSTD;
	return flags;
}

/// "file" is the packed file FS_OpenPackedFile just set up (its own handle,
/// offset and length). Replaces it with a data file holding the decoded
/// entry, or returns NULL.
static qfile_t *Touch_FS_OpenZstd(qfile_t *file, const packfile_t *pfile)
{
	unsigned char *comp;
	qfile_t *out;
	const char *err;
	fs_offset_t got;

	comp = (unsigned char *)Mem_Alloc(tempmempool, pfile->packsize > 0 ? pfile->packsize : 1);
	if (FILEDESC_SEEK(file->handle, file->offset, SEEK_SET) == -1)
		got = -1;
	else
		got = FILEDESC_READ(file->handle, comp, (size_t)pfile->packsize);
	FILEDESC_CLOSE(file->handle);
	Mem_Free(file);
	if (got != pfile->packsize)
	{
		Con_Printf("FS_OpenPackedFile: can't read zstd entry %s\n", pfile->name);
		Mem_Free(comp);
		return NULL;
	}

	// one block: the data follows the qfile, so FS_Close frees both
	out = (qfile_t *)Mem_Alloc(fs_mempool, sizeof(*out) + (size_t)pfile->realsize + 1);
	memset(out, 0, sizeof(*out));
	err = Touch_Zstd_Decompress(out + 1, (size_t)pfile->realsize, comp, (size_t)pfile->packsize);
	Mem_Free(comp);
	if (err)
	{
		Con_Printf("FS_OpenPackedFile: can't decode zstd entry %s: %s\n", pfile->name, err);
		Mem_Free(out);
		return NULL;
	}
	out->flags = QFILE_FLAG_DATA;
	out->ungetc = EOF;
	out->real_length = pfile->realsize;
	out->data = (const unsigned char *)(out + 1);
	return out;
}

#endif
