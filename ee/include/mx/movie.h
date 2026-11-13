#ifndef MX_MOVIE_H
#define MX_MOVIE_H

/* A movie from a file: MPEG-2 or MPEG-1 video decoded on the IPU
   (<mx/mpeg.h>), alone (.m2v) or in a program stream with sound
   (<mx/mpegps.h>: LPCM, 48 kHz, 16 bits, stereo), which plays through
   <mx/snd.h>'s stream in step with the pictures. docs/video-plan.md.

   Make one with ffmpeg:
     -c:v mpeg2video -g 1 -bf 0 -q:v 5 -c:a pcm_s16be -ar 48000 -ac 2 -f vob -muxrate 40000000
   That is a stream of I pictures only, which the IPU decodes alone: the
   fast kind, for anything large (ipu.md: 1920 x 1080 in 24 ms). A stream
   with P and B pictures (leave -g 1 -bf 0 out; a third of the size or less)
   is found out when the movie is opened and decoded with the EE's help, at
   several times the cost (ipu.md: 640 x 384 in 25 ms, which holds 24 a
   second; 720 x 576 does not yet). It takes three more pictures of pinned
   memory and the scratchpad's first 768 bytes while a picture is decoded.

   The movie decodes into a buffer of its own; the program puts the picture
   on the screen: mx_movie_chain builds the DMA chain that sends it into a
   texture or a frame buffer, once, and the program sends that chain after
   every new picture.

   In a frame loop (a movie as a texture, a background, a cut scene):

     struct mx_movie *mv = mx_movie_open("host:intro.mpg", MX_MOVIE_RGB16);
     mx_movie_chain(mv, tags, tbp, tbw, MX_GS_PSM_CT16, 0, 0, MX_TAG_RET);
     each frame:
       if (mx_movie_update(mv) > 0)   -- a new picture: call the chain in this frame
       ...
     mx_movie_close(mv);

   A player that times the pictures itself (to the vblank, as
   examples/moviehd) uses mx_movie_decode, mx_movie_shown and mx_movie_poll.

   The file is read by a thread of the movie's own. Memory: the stream's
   buffer (1.5 MiB) and the picture (512 or 1024 bytes a macroblock) are
   pinned (<mx/mem.h>), 0.9 MiB more is pageable.
   One movie decodes at a time (the IPU is one); the calls are for one
   thread, the one that uses <mx/snd.h> if the movie has sound. */

#include <mx/mpeg.h>
#include <mx/types.h>

#define MX_MOVIE_RGB16 1u    /* pictures as RGB16 with dither (CT16); else RGB32 (CT32) */
#define MX_MOVIE_NO_SOUND 2u /* leave the sound of the file */

struct mx_movie;

struct mx_movie_info {
  u16 width, height;      /* pixels */
  u16 mbw, mbh;           /* the decoded picture in macroblocks (16 x 16 pixels each) */
  u32 rate_num, rate_den; /* pictures a second */
  u8 sound;               /* it plays sound */
  u8 rgb16;
  u8 predicted;           /* the stream has P or B pictures */
  /* so far */
  u32 pictures;           /* given out */
  u32 decode_us;          /* what decoding one took, the mean */
  u32 errors;             /* slices lost to damaged data */
  u32 sound_dry;          /* times the sound's stream ran empty */
  u32 sound_jumps;        /* times the sound was set right by a jump */
  s32 sound_behind_ms;    /* the sound behind the picture at the last look (ahead: negative) */
};

/* Open the file, read its start and the first picture's headers. Returns 0
   if it cannot be opened, holds no picture, or there is no memory. */
struct mx_movie *mx_movie_open(const char *path, u32 flags);
void mx_movie_close(struct mx_movie *m);
const struct mx_movie_info *mx_movie_info(const struct mx_movie *m);

/* The decoded picture: mbw * mbh macroblocks (<mx/mpeg.h>), pinned, written
   by the DMAC (mx_dcache_invalidate before reading it on the EE). */
void *mx_movie_picture(const struct mx_movie *m);
/* mx_mpeg_chain for that buffer: MX_MPEG_CHAIN_QW(mbw * mbh) quadwords at
   tags. */
u32 mx_movie_chain(const struct mx_movie *m, u64 *tags, u32 dbp, u32 dbw, u32 psm, u32 x, u32 y,
                   u32 end_id);

/* Once a frame: decodes the next picture when its time has come (by the
   EE's clock from the first call) and keeps the sound going. Returns 1: a
   new picture is in the buffer; 0: not yet; -1: the movie is over. A
   program that falls behind gets one picture a call until it is level. */
int mx_movie_update(struct mx_movie *m);

/* Decode the next picture now (sleeps for the file if it must). Returns 1,
   or 0 at the end. */
int mx_movie_decode(struct mx_movie *m);
/* The picture decoded last is on the screen from now: the sound follows
   these calls (it starts with the first). */
void mx_movie_shown(struct mx_movie *m);
/* Keep the sound and the reading going while waiting: every 20 ms or so. */
void mx_movie_poll(struct mx_movie *m);

#endif
