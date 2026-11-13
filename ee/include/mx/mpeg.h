#ifndef MX_MPEG_H
#define MX_MPEG_H

/* MPEG-2 and MPEG-1 video (ISO 13818-2, 11172-2) on the IPU (<mx/ipu.h>):
   an elementary stream's headers are read on the EE, its pictures decoded
   by the IPU. docs/video-plan.md says what is there and what is to come.

   Now: I pictures (frame pictures, 4:2:0), a slice an IDEC command. Other
   pictures are skipped (MX_MPEG_SKIPPED), so a stream of I pictures only
   plays in full (ffmpeg -c:v mpeg2video -g 1) and any other shows its I
   pictures.

   The stream is in memory, in a buffer the DMAC reads too: 16-byte
   aligned, in one physical piece (core memory, or VM pinned with
   MX_VM_PIN_DMA), written back (mx_dcache_writeback) if the EE wrote it.
   A position is a byte offset into it. A picture is decoded once all of it
   and the start code after it are in the buffer, plus MX_MPEG_AHEAD bytes;
   put a sequence_end_code (00 00 01 B7) and MX_MPEG_AHEAD zero bytes after
   the last.

   The decoded picture is mbw x mbh macroblocks of 16 x 16 pixels, each on
   its own (the IPU's order: macroblock after macroblock, rows of
   macroblocks from the top), as RGB32 (1024 bytes each, the GS's CT32) or
   RGB16 (512 bytes, CT16). mx_mpeg_chain builds the DMA chain that sends
   them to their places in GS memory.

   Uses the IPU (mx_ipu_init first) and its two DMA channels; one decoder at
   a time. */

#include <mx/ipu.h>
#include <mx/types.h>

enum {
  MX_MPEG_MORE = 0,    /* the buffer ends before the headers or the picture do */
  MX_MPEG_PICTURE = 1, /* a picture's headers were read / the picture is decoded */
  MX_MPEG_END = 2,     /* sequence_end_code */
  MX_MPEG_SKIPPED = 3, /* a picture this decoder does not decode: passed over */
  MX_MPEG_BAD = -1,    /* no sequence header before the picture, or a size of 0 */
};

#define MX_MPEG_AHEAD 32

enum { MX_MPEG_I = 1, MX_MPEG_P = 2, MX_MPEG_B = 3 };

struct mx_mpeg {
  /* the sequence (valid once a picture was found) */
  u16 width, height;      /* pixels */
  u16 mbw, mbh;           /* macroblocks: the decoded picture is 16 times these */
  u32 rate_num, rate_den; /* frames a second, e.g. 30000 / 1001 */
  u8 mpeg2;               /* 0: MPEG-1 */
  u8 progressive;         /* progressive_sequence */
  u8 aspect;              /* aspect_ratio_information (MPEG-2: 1 square pixels, 2 4:3, 3 16:9) */
  u8 chroma;              /* chroma_format: 1 is 4:2:0 */
  /* the picture (mx_mpeg_headers) */
  u8 type;       /* MX_MPEG_I/P/B */
  u8 structure;  /* 3: a frame; 1, 2: a top, bottom field */
  u8 top_first, repeat_first, progressive_frame;
  u16 temporal;  /* temporal_reference: the order of display within the group */
  /* the picture decoded (mx_mpeg_picture) */
  u16 decoded; /* macroblocks written */
  u16 errors;  /* slices that ended in an error (their rest keeps what out held) */
  /* private */
  u8 have_seq, dc_prec, frame_dct, conceal, q_type, vlc_fmt, alt_scan;
  u8 iq_load[2];   /* the IPU has not got iq[n] yet */
  u8 iq[2][64];    /* intra, non-intra quantizer matrix (zigzag order) */
  u32 flags;
};

/* flags: MX_IPU_RGB16 (else RGB32), with MX_IPU_DITHER if wanted. */
void mx_mpeg_init(struct mx_mpeg *m, u32 flags);

/* Read headers from *pos up to a picture's first slice. MX_MPEG_PICTURE:
   the fields above describe it and *pos is at its slices. MX_MPEG_MORE:
   *pos is at the header that is not all there (keep the buffer from it).
   MX_MPEG_END: *pos is after the code. */
int mx_mpeg_headers(struct mx_mpeg *m, const u8 *buf, u32 size, u32 *pos);

/* Decode the picture whose slices are at *pos (after mx_mpeg_headers) to
   out: mbw * mbh macroblocks. MX_MPEG_PICTURE: done, *pos after it.
   MX_MPEG_SKIPPED: not an I frame picture; *pos after it, out untouched.
   MX_MPEG_MORE: *pos unchanged (out may be written in part). out is
   written by the DMAC: mx_dcache_invalidate it before reading it. */
int mx_mpeg_picture(struct mx_mpeg *m, const u8 *buf, u32 size, u32 *pos, void *out);

/* The source chain (for the GIF channel, PATH3: mx_dma_chain(MX_CH_GIF, tags, 0),
   or called from a chain of a frame) that writes the decoded picture at mbs
   to GS memory: buffer dbp (in 64-word blocks) of width dbw (in 64 pixels)
   and format psm (CT32 or CT24 for RGB32; CT16 or CT16S for RGB16), its
   top left at x, y. One IMAGE transfer a macroblock, the pixels by
   reference: mbs must stay where it is and is not copied, so the chain is
   built once and sent after every picture decoded to mbs. tags:
   MX_MPEG_CHAIN_QW(mbw * mbh) quadwords of core memory; end_id MX_TAG_END, or
   MX_TAG_RET for a chain that is called. Returns the quadwords used. */
#define MX_MPEG_CHAIN_QW(mbs) (6 * (mbs) + 5)
u32 mx_mpeg_chain(u64 *tags, const struct mx_mpeg *m, const void *mbs, u32 dbp, u32 dbw, u32 psm,
                  u32 x, u32 y, u32 end_id);

#endif
