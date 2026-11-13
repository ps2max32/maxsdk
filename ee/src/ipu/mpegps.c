/* The MPEG program stream's packets (<mx/mpegps.h>). */

#include <mx/mpegps.h>

static u32 pts_of(const u8 *b) {
  return (u32)(b[0] >> 1 & 7) << 30 | (u32)b[1] << 22 | (u32)(b[2] >> 1) << 15 | (u32)b[3] << 7 |
         (u32)b[4] >> 1;
}

int mx_ps_next(const u8 *buf, u32 size, u32 *pos, struct mx_ps_packet *p) {
  u32 at = *pos;
  for (;;) {
    /* a start code and the two bytes after it */
    if (at + 6 > size) {
      *pos = at;
      return MX_PS_MORE;
    }
    if (buf[at] || buf[at + 1] || buf[at + 2] != 1) {
      at++;
      continue;
    }
    u32 id = buf[at + 3];
    if (id == 0xB9) {
      *pos = at + 4;
      return MX_PS_END;
    }
    if (id == 0xBA) { /* pack header: MPEG-2's 14 bytes and its stuffing, MPEG-1's 12 */
      u32 n = 12;
      if (buf[at + 4] >> 6 == 1) {
        if (at + 14 > size) {
          *pos = at;
          return MX_PS_MORE;
        }
        n = 14 + (buf[at + 13] & 7);
      }
      at += n;
      continue;
    }
    if (id < 0xBB) { /* a video stream's own start code: not at a packet */
      at += 3;
      continue;
    }
    u32 len = (u32)buf[at + 4] << 8 | buf[at + 5], end = at + 6 + len;
    if (end > size) {
      *pos = at;
      return MX_PS_MORE;
    }
    int with_header = id == 0xBD || (id >= 0xC0 && id <= 0xEF);
    if (!with_header) {
      /* system header, padding, private stream 2 and the like */
      at = end;
      if (id != 0xBF)
        continue;
      *p = (struct mx_ps_packet){.data = buf + at - len, .bytes = len, .stream = (u8)id};
      *pos = at;
      return MX_PS_OTHER;
    }
    *p = (struct mx_ps_packet){.stream = (u8)id};
    u32 d = at + 6; /* the payload's start, to be found */
    if (len >= 3 && buf[d] >> 6 == 2) {
      /* MPEG-2: flags, the header's length, then the PTS if flagged */
      if ((buf[d + 1] & 0x80) && buf[d + 2] >= 5 && len >= 8) {
        p->has_pts = 1;
        p->pts = pts_of(buf + d + 3);
      }
      d += 3 + buf[d + 2];
    } else {
      /* MPEG-1: stuffing, the STD buffer's size, the PTS (and DTS), or 0x0F */
      while (d < end && buf[d] == 0xFF)
        d++;
      if (d + 2 <= end && buf[d] >> 6 == 1)
        d += 2;
      if (d + 5 <= end && buf[d] >> 4 == 2) {
        p->has_pts = 1;
        p->pts = pts_of(buf + d);
        d += 5;
      } else if (d + 10 <= end && buf[d] >> 4 == 3) {
        p->has_pts = 1;
        p->pts = pts_of(buf + d);
        d += 10;
      } else {
        d++;
      }
    }
    if (d > end)
      d = end;
    int kind = MX_PS_OTHER;
    if (id >= 0xE0) {
      kind = MX_PS_VIDEO;
      p->stream = (u8)(id & 15);
    } else if (id >= 0xC0) {
      kind = MX_PS_MPA;
      p->stream = (u8)(id & 31);
    } else if (d < end) {
      /* private stream 1: the substream's number first */
      u32 sub = buf[d];
      p->stream = (u8)sub;
      if (sub >= 0xA0 && sub <= 0xA7 && d + 7 <= end) {
        /* LPCM: frames, the first one's place (2), flags, the format, the range */
        u32 f = buf[d + 5];
        static const u8 bits[4] = {16, 20, 24, 0};
        kind = MX_PS_LPCM;
        p->stream = (u8)(sub & 7);
        p->bits = bits[f >> 6];
        p->rate = f >> 4 & 1 ? 96000 : 48000;
        p->channels = (u8)((f & 7) + 1);
        d += 7;
      }
    }
    p->data = buf + d;
    p->bytes = end - d;
    *pos = end;
    return kind;
  }
}
