#ifndef MX_MPEGPS_H
#define MX_MPEGPS_H

/* The MPEG program stream (ISO 13818-1, and 11172-1's): a file's video and
   sound in turns, packet after packet. This takes the packets apart; the
   video's bytes go to <mx/mpeg.h>, the sound's to <mx/snd.h>.

   Sound: LPCM as on a DVD (private stream 1, substreams 0xA0 .. 0xA7):
   16-bit samples, the high byte first, channels interleaved. MPEG audio
   packets are named (MX_PS_MPA) and not decoded here.

   Make one with ffmpeg: -c:v mpeg2video -g 1 -bf 0 -c:a pcm_s16be -ar 48000 -ac 2 -f vob
   (with -muxrate above the video's rate, e.g. 40000000). */

#include <mx/types.h>

enum {
  MX_PS_MORE = 0,  /* the buffer ends before the packet does */
  MX_PS_VIDEO = 1, /* a video stream's bytes (stream 0xE0 .. 0xEF) */
  MX_PS_LPCM = 2,  /* samples */
  MX_PS_MPA = 3,   /* MPEG audio (stream 0xC0 .. 0xDF) */
  MX_PS_OTHER = 4, /* anything else with a payload (AC-3, subpictures, private stream 2) */
  MX_PS_END = 5,   /* the program end code */
};

struct mx_ps_packet {
  const u8 *data; /* the payload, in the buffer */
  u32 bytes;
  u8 stream;  /* the stream's number: the id's low bits, the substream's for private stream 1 */
  u8 has_pts;
  u32 pts;    /* when the first unit that starts in the packet is shown or heard: 1/90000 s
                 (the low 32 bits: 13 hours) */
  /* MX_PS_LPCM */
  u32 rate;   /* samples a second: 48000 or 96000 */
  u8 channels, bits;
};

/* The next packet with a payload at or after *pos (pack and system headers
   and padding are passed over; bytes that are no packet too, up to the next
   start code). Returns its kind with *p filled and *pos after it;
   MX_PS_MORE with *pos at what is not all there (keep the buffer from it);
   MX_PS_END with *pos after the code. */
int mx_ps_next(const u8 *buf, u32 size, u32 *pos, struct mx_ps_packet *p);

#endif
