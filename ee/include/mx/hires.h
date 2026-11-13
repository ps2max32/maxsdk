#ifndef MX_HIRES_H
#define MX_HIRES_H

/* High resolution by passes (gsKit's hires, ee/gs/src/gsHires.c): a
   picture larger than GS memory holds twice (1920 x 540 at 16 bits is
   2 MiB), drawn into one framebuffer a strip at a time, just ahead of the
   beam, every field.

   The framebuffer is three or more strips (mx_hires_passes()) of the
   picture's width, each a whole number of pages high: a third of the
   picture (rounded up to whole pages), or less when that leaves fewer than
   three strips the beam reaches (640 x 224 at 16 bits: four 64-line strips). The frame stream
   (<mx/frame.h>) is not sent to VIF1: each frame's stream is recorded into
   memory (a chain ending in RET). When the beam starts showing strip k - 1,
   EE timer 0 counting H-blanks kicks pass k: strip k's drawing registers,
   a clear of it, and a CALL of the record; the frame's Z buffer is strip
   k + 1, which the beam shows after strip k, drawn over in its own pass
   before that. Every field draws all three strips from the newest record
   that is complete, so the picture is whole however slowly the frames
   come; a new record is taken at pass 0, the first pass of a field.

   In interlaced FRAME mode (1080i: each field shows all 540 lines) the
   lower field's passes draw half a line lower (XYOFFSET.Y + 8), which gives
   the two fields of a 1080-line picture (examples/field).

   What the frame stream may write: the GS registers of both contexts but
   ZBUF and SCISSOR, which belong to the passes (SCISSOR chooses the strip);
   XYOFFSET is the passes' too (the lower field's half line). FRAME is the
   same in every pass (mx_hires_frame): the stream may write it, for FBMSK or
   another format of the same buffer. Context 1
   (PRIM.CTXT 0) writes Z, context 2 (CTXT 1) has ZMSK set: draw with
   context 2 for no Z writes, and set the other registers of the context a
   primitive uses. Window coordinates are those of a w x h target
   (mx_gs_px, mx_gs_py: mx_gs.w, mx_gs.h are the picture's).

   Data the stream references (vertex arrays, textures) is read every pass
   until a later frame has replaced the record: until mx_frame_done()
   passes the frame. A frame's IMAGE transfers are done in every pass of
   it, in the order recorded, so a frame's own texture uploads are always
   right; textures left in GS memory by an earlier frame are right as long
   as no later upload overwrote them. Uploads that need not be repeated go
   into the once record (mx_hires_once).

   Not with the PATH3 stream (mx_frame3_*), mx_frame_present or
   mx_frame_vu1_compute. The GS must draw a whole frame once per strip, every field:
   a pass whose turn comes while the one before is still being sent is left
   out (mx_hires_stats.late), and the beam shows that strip as it is (half
   drawn, or the Z of the pass before). */

#include <mx/rt.h>
#include <mx/types.h>
#include <mx/video.h>

#define MX_HIRES_PASSES 8 /* strips at most */
#define MX_HIRES_SLOTS 4 /* records: one played, three waiting or being built (a fast frame is kept for when a slow one comes) */

struct mx_hires_stats {
  u32 fields;   /* fields drawn (pass 0 kicked) */
  u32 passes;   /* passes kicked */
  u32 late;     /* passes left out: the one before was still being sent */
  u32 shown;    /* records that became the one played */
  u32 rec_max;  /* the largest record, bytes */
  u32 rec_full; /* frames that did not fit their record (halves left out) */
  u32 held;     /* fields that kept the record before, as the next one's PATH3 uploads were not done */
  u32 pass_us[MX_HIRES_PASSES]; /* the longest pass of each strip, us (kick to end of DMA) */
};
extern struct mx_hires_stats mx_hires_stats;

/* DMAC, the GS and mode m (interlaced: FRAME mode), the framebuffer of
   w x h pixels of psm (CT32 with Z32; CT24 with Z24, which leaves the top
   byte of every word of the strips as it is: room for T8H/T4HL/T4HH
   textures, mx_hires_hbyte; CT16, CT16S with Z16, Z16S),
   shown on read circuit 2, and the frame pipeline in record mode: mem
   (64-byte aligned, uncached or not written by the EE) holds the
   MX_HIRES_SLOTS records; spr, spr_bytes as mx_frame_init. Load VU1
   microcode before (or in the stream). Returns 0, or -1 if the mode does
   not run or the framebuffer does not fit. mx_gs.tbp is the first block
   after the framebuffer. mx_frame_stop() ends the passes (the pass being
   sent and a once record over PATH3 finish): the GS can then be set up in
   another mode, with another pipeline (a mode change). */
int mx_hires_init(const struct mx_video_mode *m, u32 w, u32 h, u32 psm, void *mem, u32 bytes,
                  void *spr, u32 spr_bytes);
/* The number of strips (passes a field) of the framebuffer, after
   mx_hires_init. */
u32 mx_hires_passes(void);
/* FRAME of the framebuffer with mask, for the stream (it must be set back). */
u64 mx_hires_frame(u32 mask);
/* The pages whose top byte no pass writes (CT24 strips: all of them), the
   first in *page; 0 for another format. */
u32 mx_hires_hbyte(u32 *page);
/* The record of the frame being built (0 .. MX_HIRES_SLOTS - 1), after
   mx_frame_begin_n: data per frame can be kept per record. */
u32 mx_hires_slot(void);
/* on: the frame stream after this goes into the frame's once record, sent
   over PATH3 (GIF channel) as soon as the frame is complete, while the
   frames before are still played, not in every pass: texture uploads to GS
   memory that no frame being shown reads (GIF packets only, no VIF codes
   are sent). The frame is shown once they are done. A small once record
   (up to 16 KiB) is sent at the start of the frame's first pass instead. off: back into the main
   record. A once record that is full takes no more: the stream goes into
   the main one. */
void mx_hires_once(int on);
/* Dithering into 16 bits that changes with the field: n (up to
   MX_HIRES_DITHER) DIMX values, field f's passes drawn with dimx[f % n];
   n 0: the frame stream's own DIMX. Every field is drawn anew, so two
   matrices that complement each other give twice the shades on an
   interlaced screen, and a few in turn make the pattern look like grain. */
#define MX_HIRES_DITHER 8
void mx_hires_dither(const u64 *dimx, u32 n);
/* The colour each pass clears its strip to, for the frame being built. */
void mx_hires_clear(u32 r, u32 g, u32 b);
/* The field (CSR.FIELD) whose passes are drawn half a line lower in
   interlaced FRAME mode: 0 (default, as gsKit) or 1; -1: no shift. */
void mx_hires_shift_field(int f);
/* The framebuffer: the whole picture, w x h (the strips one below the
   other), for screenshots. */
const struct mx_rt *mx_hires_rt(void);
/* The picture of the record played last as a 24-bit TGA file ("host:..."):
   each strip drawn again and read back; in interlaced FRAME mode both
   fields (2 h lines). Stops the passes and the pipeline (for tests: the
   program ends after it). Returns 0, or -1. */
int mx_hires_save_tga(const char *path);

#endif
