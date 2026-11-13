#ifndef MX_MP2_H
#define MX_MP2_H

/* MPEG audio layer II (ISO 11172-3: "MP2", the sound of most MPEG program
   streams) decoded on the EE in integers: frames of 1152 samples at 32, 44.1
   or 48 kHz, mono or stereo (joint stereo too). Not layer I or III, not the
   half sample rates of MPEG-2.

   <mx/movie.h> plays it from a program stream. A frame of 48 kHz stereo
   takes 3.4 ms: a seventh of the EE (docs/video-plan.md). */

#include <mx/types.h>

#define MX_MP2_SAMPLES 1152 /* a frame, each channel */

struct mx_mp2 {
  s32 v[2][1024]; /* the synthesis filter's memory, a ring */
  u32 pos;
};

void mx_mp2_init(struct mx_mp2 *d);

/* Decode the frame at the start of data. Returns the bytes it took, with
   *frames 1152 and out filled: 1152 pairs of left and right (a mono frame
   in both), *rate its samples a second. Bytes that are no frame are passed
   over up to the next one: the bytes taken with *frames 0. Returns 0 when
   the frame is not all in data yet. */
u32 mx_mp2_decode(struct mx_mp2 *d, const u8 *data, u32 bytes, s16 *out, u32 *frames, u32 *rate);

#endif
