#ifndef MX_FRAME_INT_H
#define MX_FRAME_INT_H

/* Between frame.c (the VIF1 stream) and frame3.c (the PATH3 stream). */

#include <mx/dma.h>

/* Everything built so far in the VIF1 stream goes to VIF1 now (without the
   idle work of mx_frame_idle: the PATH3 stream's caller may be using VU0). */
void mx__frame_kick(void);
/* The texture uploads' events of the GIF queue are void (it starts again). */
void mx__frame_tex_forget(void);
/* A wait of the streams has lasted a second: where they stand (once). */
void mx__frame_stalled(const char *what);
/* The VIF1 stream is in chain mode. */
int mx__frame_chain(void);

/* Set by mx_frame3_init while the PATH3 stream is on: */
/* at the end of a frame, before its FINISH (the join) */
extern void (*mx__frame3_end)(void);
/* in mx_frame_stop, after VIF1 has drained */
extern void (*mx__frame3_stop)(void);
/* the GIF queue (texture uploads go through it) */
extern struct mx_dmaq *mx__frame3_q;
/* its part of mx__frame_stalled's report */
extern void (*mx__frame3_report)(void);

/* Record mode, for hires.c (<mx/hires.h>): the stream's halves go into a
   record per frame instead of to VIF1. */
void mx__frame_init_record(void *spr, u32 spr_bytes);
/* hires.c: the memory of the next record and of its once record (waits
   until one is free; qw: their quadwords), the end of the records (at end,
   once_end; full: halves were left out), the frame stream's mx_frame_ready, and the end of the pipeline */
u8 *mx__hires_begin(u32 *qw, u8 **once, u32 *once_qw);
void mx__hires_end(u8 *end, u8 *once_end, int full);
int mx__hires_ready(void);
void mx__hires_stop(void);

/* gs.c: called by the vblank interrupt (vblank start), 0: none */
extern void (*mx__vsync_hook)(void);

#endif
