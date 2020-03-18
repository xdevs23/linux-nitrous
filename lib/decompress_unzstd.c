// SPDX-License-Identifier: GPL-2.0-only
/*
 * Wrapper for decompressing ZSTD-compressed kernel, initramfs, and initrd
 * Based on decompress_unlz4.c
 *
 * Copyright (C) 2020, Petr Malat <oss@malat.biz>
 */

#ifdef STATIC
#define PREBOOT
#include "zstd/zstd_internal.h"
#include "zstd/huf_decompress.c"
#include "zstd/entropy_common.c"
#include "zstd/fse_decompress.c"
#include "zstd/zstd_common.c"
#include "zstd/decompress.c"
#include "xxhash.c"
#else
#include <linux/decompress/unzstd.h>
#include <linux/zstd.h>
#endif
#include <linux/types.h>
#include <linux/decompress/mm.h>
#include <linux/compiler.h>

STATIC inline int INIT unzstd(u8 *input, long in_len,
				long (*fill)(void *, unsigned long),
				long (*flush)(void *, unsigned long),
				u8 *output, long *posp,
				void (*error)(char *x))
{
	int ret = -1, ws = 1 << ZSTD_WINDOWLOG_MAX;
	u8 *inp, *outp;
	ZSTD_DStream *zstd;
	void *workspace;
	size_t workspace_size;
	ZSTD_outBuffer out;
	ZSTD_inBuffer in;
	unsigned long out_len;
	unsigned long pos;

	if (output) {
		out_len = ULONG_MAX; // Caller knows data will fit
		outp = output;
	} else if (!flush) {
		error("NULL output pointer and no flush function provided");
		goto exit_0;
	} else {
		out_len = ZSTD_DStreamOutSize();
		outp = large_malloc(out_len);
		if (!outp) {
			error("Could not allocate output buffer");
			goto exit_0;
		}
	}

	if (input && fill) {
		error("Both input pointer and fill function provided,");
		goto exit_1;
	} else if (input) {
		ZSTD_frameParams p;

		inp = input;
		if (!ZSTD_getFrameParams(&p, input, in_len))
			ws = p.windowSize;
	} else if (!fill) {
		error("NULL input pointer and missing fill function");
		goto exit_1;
	} else {
		in_len = ZSTD_DStreamInSize();
		inp = large_malloc(in_len);
		if (!inp) {
			error("Could not allocate input buffer");
			goto exit_1;
		}
	}

	workspace_size = ZSTD_DStreamWorkspaceBound(ws);
	workspace = large_malloc(workspace_size);
	if (!workspace) {
		error("Could not allocate workspace");
		goto exit_2;
	}

	zstd = ZSTD_initDStream(ws, workspace, workspace_size);
	if (!zstd) {
		error("Could not initialize ZSTD");
		goto exit_3;
	}

	in.src = inp;
	in.size = in_len;
	in.pos = 0;
	if (posp)
		*posp = 0;

	for (;;) {
		if (fill) {
			in.size = fill(inp, in_len);
			if (in.size == 0)
				break;
		} else if (in.size == in.pos) {
			break;
		}
init:		out.dst = outp;
		out.size = out_len;
		out.pos = 0;
		pos = in.pos;

		ret = ZSTD_decompressStream(zstd, &out, &in);
		if (posp)
			*posp += in.pos - pos;
		if (ZSTD_isError(ret)) {
			error("Decompression failed");
			ret = -EIO;
			goto exit_3;
		}

		if (flush && out.pos) {
			if (flush(out.dst, out.pos) != out.pos) {
				ret = -EIO;
				goto exit_3;
			}
			goto init;
		}

		if (ret == 0) {
			ret = ZSTD_resetDStream(zstd);
			if (ZSTD_isError(ret)) {
				ret = -EIO;
				goto exit_3;
			}
		}
		if (in.pos < in.size)
			goto init;
	}

	ret = 0;

exit_3:	large_free(workspace);
exit_2:	if (!input)
		large_free(inp);
exit_1:	if (!output)
		large_free(outp);
exit_0:	return ret;
}

#ifdef PREBOOT
STATIC int INIT __decompress(unsigned char *buf, long in_len,
			      long (*fill)(void*, unsigned long),
			      long (*flush)(void*, unsigned long),
			      unsigned char *output, long out_len,
			      long *posp,
			      void (*error)(char *x)
	)
{
	return unzstd(buf, in_len, fill, flush, output, posp, error);
}
#endif
