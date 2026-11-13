// Touch: zstd pk3 entries (zip method 93), included by fs.c after its pack
// and qfile structures so the two hooks there stay one line each:
//
//   PK3_BuildFileList:  flags = Touch_PK3_EntryFlags(method, flags);
//   FS_OpenPackedFile:  if (pfile->flags & PACKFILE_FLAG_ZSTD) return Touch_FS_OpenZstd(file, pfile);
//   FS_Read:            Touch_FS_ZstdEnsure(file);
//   FS_Close:           Touch_FS_ZstdClose(file);
//
// A zstd entry is decoded whole and handed back as a data file: with
// fs_zstd_lazy (below) at its first read, else when it is opened. The
// engine loads nearly every packed file whole anyway (FS_LoadFile), and what
// streams (music, which the repack stores uncompressed) never takes this
// path. FS_Seek and FS_FileSize need only the size, known from the header.

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

// Godnotic: a zstd entry decodes on its first read, not at open.
// QuakeC's fexists() is fopen + fclose: the server and CSQC ask it of ~900
// models and sounds at every map load, and each open decoded the whole
// file (15 ms for the spider on a desktop; 0.48 s of a stormkeep load).
// fs_zstd_lazy 1: the open keeps the pack handle and the entry's place, the
// first FS_Read decodes (Touch_FS_ZstdEnsure), FS_Close frees what there is.
#define QFILE_FLAG_TOUCH_ZLAZY (1 << 12)  ///< zstd entry not decoded yet: handle, offset, packsize (buff_len) kept
#define QFILE_FLAG_TOUCH_ZOWNED (1 << 13) ///< data allocated apart from the qfile

static cvar_t fs_zstd_lazy = {CF_CLIENT | CF_SERVER, "fs_zstd_lazy", "1", "decode a zstd-compressed pack entry when it is first read rather than when it is opened (Godnotic)"};

/// reads the entry through file's handle (then closes it) and decodes it
/// into dst (real_length bytes)
static qbool Touch_FS_ZstdRead(qfile_t *file, fs_offset_t packsize, unsigned char *dst, const char *name)
{
	unsigned char *comp;
	const char *err;
	fs_offset_t got;

	comp = (unsigned char *)Mem_Alloc(tempmempool, packsize > 0 ? packsize : 1);
	if (FILEDESC_SEEK(file->handle, file->offset, SEEK_SET) == -1)
		got = -1;
	else
		got = FILEDESC_READ(file->handle, comp, (size_t)packsize);
	FILEDESC_CLOSE(file->handle);
	if (got != packsize)
	{
		Con_Printf("FS_OpenPackedFile: can't read zstd entry %s\n", name);
		Mem_Free(comp);
		return false;
	}
	err = Touch_Zstd_Decompress(dst, (size_t)file->real_length, comp, (size_t)packsize);
	Mem_Free(comp);
	if (err)
	{
		Con_Printf("FS_OpenPackedFile: can't decode zstd entry %s: %s\n", name, err);
		return false;
	}
	return true;
}

/// "file" is the packed file FS_OpenPackedFile just set up (its own handle,
/// offset and length). Replaces it with a data file holding the decoded
/// entry (or one that decodes on first read), or returns NULL.
static qfile_t *Touch_FS_OpenZstd(qfile_t *file, const packfile_t *pfile)
{
	qfile_t *out;
	if (fs_zstd_lazy.integer)
	{
		file->flags = QFILE_FLAG_DATA | QFILE_FLAG_TOUCH_ZLAZY;
		file->buff_len = pfile->packsize;
		file->data = NULL;
		return file;
	}
	// one block: the data follows the qfile, so FS_Close frees both
	out = (qfile_t *)Mem_Alloc(fs_mempool, sizeof(*out) + (size_t)pfile->realsize + 1);
	memset(out, 0, sizeof(*out));
	out->real_length = pfile->realsize;
	out->handle = file->handle;
	out->offset = file->offset;
	Mem_Free(file);
	if (!Touch_FS_ZstdRead(out, pfile->packsize, (unsigned char *)(out + 1), pfile->name))
	{
		Mem_Free(out);
		return NULL;
	}
	out->flags = QFILE_FLAG_DATA;
	out->ungetc = EOF;
	out->data = (const unsigned char *)(out + 1);
	return out;
}

/// FS_Read: decode a lazily opened zstd entry
static void Touch_FS_ZstdEnsure(qfile_t *file)
{
	unsigned char *data;
	if (!(file->flags & QFILE_FLAG_TOUCH_ZLAZY))
		return;
	file->flags &= ~QFILE_FLAG_TOUCH_ZLAZY;
	data = (unsigned char *)Mem_Alloc(fs_mempool, (size_t)file->real_length + 1);
	if (!Touch_FS_ZstdRead(file, file->buff_len, data, "(opened lazily)"))
	{
		// reads as empty (a failed decode at open would have failed the open)
		Mem_Free(data);
		file->real_length = 0;
		file->position = 0;
		file->buff_len = 0;
		return;
	}
	file->buff_len = 0;
	file->data = data;
	file->flags |= QFILE_FLAG_TOUCH_ZOWNED;
}

/// FS_Close of a data file: what a lazy zstd entry still holds
static void Touch_FS_ZstdClose(qfile_t *file)
{
	if (file->flags & QFILE_FLAG_TOUCH_ZLAZY)
		FILEDESC_CLOSE(file->handle);
	if ((file->flags & QFILE_FLAG_TOUCH_ZOWNED) && file->data)
		Mem_Free((void *)file->data);
	file->flags &= ~(QFILE_FLAG_TOUCH_ZLAZY | QFILE_FLAG_TOUCH_ZOWNED);
}

void Touch_FS_Init(void); // touch_warm.c registers it
void Touch_FS_Init(void)
{
	Cvar_RegisterVariable(&fs_zstd_lazy);
}

#endif
