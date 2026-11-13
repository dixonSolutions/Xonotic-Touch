// Touch: zstd decoder for pk3 entries stored with zip method 93.
//
// touch_vendor/zstd/zstddeclib.c is the upstream single-file decompressor of
// zstd 1.5.7 (BSD, see touch_vendor/zstd/LICENSE), generated unmodified with
// build/single_file_libs/combine.py. Its symbols are kept hidden so a
// system libzstd that libcurl pulls in never binds to this copy or the
// other way round.

#if defined(__GNUC__) || defined(__clang__)
# define ZSTDLIB_VISIBLE __attribute__((visibility("hidden")))
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wpedantic"
# pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
# pragma GCC diagnostic ignored "-Wmissing-prototypes"
# pragma GCC diagnostic ignored "-Wshadow"
# pragma GCC diagnostic ignored "-Wsign-compare"
# pragma GCC diagnostic ignored "-Wstrict-prototypes"
# pragma GCC diagnostic ignored "-Wunused-function"
# if !defined(__clang__)
#  pragma GCC diagnostic ignored "-Wc++-compat"
# endif
#endif
#include "touch_vendor/zstd/zstddeclib.c"
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#endif

#include "quakedef.h"
#include "touch_assets.h"

const char *Touch_Zstd_Decompress(void *dst, size_t dstsize, const void *src, size_t srcsize)
{
	size_t r = ZSTD_decompress(dst, dstsize, src, srcsize);
	if (ZSTD_isError(r))
		return ZSTD_getErrorName(r);
	if (r != dstsize)
		return "size differs from the zip header";
	return NULL;
}
