/* MPEG video on the IPU (<mx/mpeg.h>): the headers on the EE, I pictures a
   slice an IDEC command. */

#include <mx/cpu.h>
#include <mx/dma.h>
#include <mx/gif.h>
#include <mx/mpeg.h>
#include <mx/string.h>

/* ISO 13818-2, 6.3.11: the default intra matrix, in the order it would be
   sent (zigzag); the default non-intra one is 16 throughout. */
static const u8 default_intra[64] = {
    8,  16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26,
    27, 27, 27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38,
    37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83};

static const u16 rates[9][2] = {{0, 1},        {24000, 1001}, {24, 1}, {25, 1},      {30000, 1001},
                                {30, 1},       {50, 1},       {60000, 1001}, {60, 1}};

void mx_mpeg_init(struct mx_mpeg *m, u32 flags) {
  memset(m, 0, sizeof *m);
  m->flags = flags & (MX_IPU_RGB16 | MX_IPU_DITHER);
}

/* ---- the headers, on the EE ------------------------------------------------ */

/* The next 00 00 01 at or after from; size if there is none whose code
   byte is in the buffer too. */
static u32 next_code(const u8 *b, u32 from, u32 size) {
  for (u32 i = from; i + 3 < size; i++) {
    if (b[i + 2] > 1) {
      i += 2;
      continue;
    }
    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
      return i;
  }
  return size;
}

struct bits {
  const u8 *p;
  u32 pos; /* in bits */
};
static u32 get(struct bits *b, u32 n) {
  u32 v = 0;
  for (; n; n--, b->pos++)
    v = v << 1 | (b->p[b->pos >> 3] >> (7 - (b->pos & 7)) & 1);
  return v;
}

static void set_matrix(struct mx_mpeg *m, int n, const u8 *from) {
  if (!m->iq_load[n] && !memcmp(m->iq[n], from, 64))
    return;
  memcpy(m->iq[n], from, 64);
  m->iq_load[n] = 1;
}
static void default_matrix(struct mx_mpeg *m, int n) {
  u8 d[64];
  if (n)
    memset(d, 16, 64);
  else
    memcpy(d, default_intra, 64);
  set_matrix(m, n, d);
}
/* load_intra_quantiser_matrix, load_non_intra_quantiser_matrix and what
   they bring (not byte aligned) */
static void get_matrices(struct mx_mpeg *m, struct bits *b, int defaults) {
  for (int n = 0; n < 2; n++) {
    if (get(b, 1)) {
      u8 q[64];
      for (int i = 0; i < 64; i++)
        q[i] = (u8)get(b, 8);
      set_matrix(m, n, q);
    } else if (defaults) {
      default_matrix(m, n);
    }
  }
}

int mx_mpeg_headers(struct mx_mpeg *m, const u8 *buf, u32 size, u32 *pos) {
  u32 p = *pos;
  for (;;) {
    p = next_code(buf, p, size);
    *pos = p;
    /* the longest header: a sequence header with both matrices, 140 bytes */
    if (p + 4 > size)
      return MX_MPEG_MORE;
    u32 code = buf[p + 3];
    if (code >= 0x01 && code <= 0xAF) {
      if (!m->have_seq || !m->type)
        return MX_MPEG_BAD;
      return MX_MPEG_PICTURE;
    }
    if (code == 0xB7) {
      *pos = p + 4;
      return MX_MPEG_END;
    }
    u32 next = next_code(buf, p + 4, size);
    if (next == size) /* the header may go on past the buffer */
      return MX_MPEG_MORE;
    struct bits b = {buf + p + 4, 0};
    if (code == 0xB3) { /* sequence_header */
      m->width = (u16)get(&b, 12);
      m->height = (u16)get(&b, 12);
      m->aspect = (u8)get(&b, 4);
      u32 r = get(&b, 4);
      m->rate_num = rates[r > 8 ? 0 : r][0];
      m->rate_den = rates[r > 8 ? 0 : r][1];
      get(&b, 18 + 1 + 10 + 1);
      get_matrices(m, &b, 1);
      m->mbw = (u16)((m->width + 15) >> 4);
      m->mbh = (u16)((m->height + 15) >> 4);
      m->mpeg2 = 0; /* until its extension follows */
      m->progressive = 1;
      m->chroma = 1;
      m->have_seq = m->width && m->height;
    } else if (code == 0x00) { /* picture_header */
      m->temporal = (u16)get(&b, 10);
      m->type = (u8)get(&b, 3);
      /* MPEG-1 has no more; MPEG-2's picture_coding_extension follows */
      m->structure = 3;
      m->dc_prec = 0;
      m->frame_dct = 1;
      m->conceal = m->q_type = m->vlc_fmt = m->alt_scan = 0;
      m->top_first = m->repeat_first = 0;
      m->progressive_frame = 1;
    } else if (code == 0xB5) { /* an extension */
      u32 id = get(&b, 4);
      if (id == 1) { /* sequence_extension */
        get(&b, 8);
        m->progressive = (u8)get(&b, 1);
        m->chroma = (u8)get(&b, 2);
        m->width |= (u16)(get(&b, 2) << 12);
        m->height |= (u16)(get(&b, 2) << 12);
        get(&b, 12 + 1 + 8 + 1);
        u32 n = get(&b, 2), d = get(&b, 5);
        m->rate_num *= n + 1;
        m->rate_den *= d + 1;
        m->mbw = (u16)((m->width + 15) >> 4);
        /* an interlaced sequence has whole macroblocks in each field */
        m->mbh = (u16)(m->progressive ? (m->height + 15) >> 4 : (m->height + 31) >> 5 << 1);
        m->mpeg2 = 1;
      } else if (id == 3) { /* quant_matrix_extension */
        get_matrices(m, &b, 0);
      } else if (id == 8) { /* picture_coding_extension */
        get(&b, 16);
        m->dc_prec = (u8)get(&b, 2);
        m->structure = (u8)get(&b, 2);
        m->top_first = (u8)get(&b, 1);
        m->frame_dct = (u8)get(&b, 1);
        m->conceal = (u8)get(&b, 1);
        m->q_type = (u8)get(&b, 1);
        m->vlc_fmt = (u8)get(&b, 1);
        m->alt_scan = (u8)get(&b, 1);
        m->repeat_first = (u8)get(&b, 1);
        get(&b, 1);
        m->progressive_frame = (u8)get(&b, 1);
      }
    }
    /* group_of_pictures_header, user_data and the rest: nothing wanted */
    p = next;
  }
}

/* ---- a picture, on the IPU -------------------------------------------------- */

struct dec {
  const u8 *buf;
  u32 q0;        /* the first quadword given to the IPU */
  u32 fedq, endq; /* quadwords given so far (from the buffer's start), and there are */
};

#define FEED_MAX 0xFFFFu

static int more(struct dec *d) {
  if (d->fedq >= d->endq)
    return 0;
  u32 n = d->endq - d->fedq;
  if (n > FEED_MAX)
    n = FEED_MAX;
  mx_ipu_more(d->buf + d->fedq * 16, n);
  d->fedq += n;
  return 1;
}

static int busy(void) { return (mx_ipu_status() & (u32)MX_BIT(IPU_CTRL_BUSY)) != 0; }

/* FDEC; *starved when the buffer ends before its 32 bits */
static u32 fdec(struct dec *d, u32 fb, int *starved) {
  u32 v = mx_ipu_fdec(fb);
  if (!busy())
    return v;
  do {
    if (!more(d)) {
      *starved = 1;
      return 0;
    }
    mx_ipu_wait();
  } while (busy());
  return mx_ipu_top();
}

static int is_slice(u32 code) { return code >> 8 == 1 && (code & 0xFF) >= 0x01 && (code & 0xFF) <= 0xAF; }

/* Past a picture's slices on the EE: the next start code that is no slice. */
static int skip_slices(const u8 *buf, u32 size, u32 *pos) {
  u32 p = *pos;
  for (;;) {
    p = next_code(buf, p, size);
    if (p + 4 + MX_MPEG_AHEAD > size)
      return MX_MPEG_MORE;
    if (buf[p + 3] < 0x01 || buf[p + 3] > 0xAF)
      break;
    p += 4;
  }
  *pos = p;
  return MX_MPEG_SKIPPED;
}

int mx_mpeg_picture(struct mx_mpeg *m, const u8 *buf, u32 size, u32 *pos, void *out) {
  if (!m->have_seq)
    return MX_MPEG_BAD;
  /* what the IPU alone decodes: an intra frame picture in 4:2:0 without
     concealment vectors, a row of macroblocks within a transfer */
  if (m->type != MX_MPEG_I || m->structure != 3 || m->chroma != 1 || m->conceal || m->mbw > 1023)
    return skip_slices(buf, size, pos);

  for (int n = 0; n < 2; n++)
    if (m->iq_load[n]) {
      mx_ipu_setiq_mem(n, m->iq[n]);
      m->iq_load[n] = 0;
    }
  mx_ipu_ctrl((u32)(MX_SET(IPU_CTRL_IDP, m->dc_prec) | MX_SET(IPU_CTRL_AS, m->alt_scan) |
                    MX_SET(IPU_CTRL_IVF, m->vlc_fmt) | MX_SET(IPU_CTRL_QST, m->q_type) |
                    MX_SET(IPU_CTRL_MP1, !m->mpeg2) | MX_SET(IPU_CTRL_PCT, MX_MPEG_I)));

  u32 mbq = m->flags & MX_IPU_RGB16 ? MX_IPU_MB_RGB16 : MX_IPU_MB_RGB32;
  u32 flags = m->flags | (m->mpeg2 && !m->frame_dct ? MX_IPU_IDEC_DTD : 0);
  struct dec d = {buf, *pos >> 4, 0, size >> 4};
  u32 n = d.endq - d.q0;
  if (n > FEED_MAX)
    n = FEED_MAX;
  d.fedq = d.q0 + n;
  mx_ipu_input(buf + d.q0 * 16, n, (*pos & 15) * 8);

  int starved = 0;
  m->decoded = m->errors = 0;
  u32 code = 0x100u | buf[*pos + 3]; /* mx_mpeg_headers saw it */
  while (is_slice(code)) {
    u32 row = (code & 0xFF) - 1, col = 0, fb = 0;
    /* quantiser_scale_code (5), extra_bit_slice (0), then the first
       macroblock's address increment: '1' for column 0 */
    u32 v = fdec(&d, 32, &starved);
    if (starved)
      break;
    int bad = (v >> 26 & 1) || row >= m->mbh;
    if (v >> 25 & 1) {
      fb = 7;
    } else if (!bad) {
      u32 inc = mx_ipu_vdec(MX_IPU_VLC_MBAI, 6);
      if (busy()) { /* the transfer's end inside the code */
        if (!more(&d)) {
          starved = 1;
          break;
        }
        mx_ipu_wait();
        inc = 0; /* the slice is given up */
      }
      col = (inc & 0xFFFF) - 1;
      bad = !inc || col >= m->mbw;
      fb = 0;
    }
    if (!bad) {
      int r = mx_ipu_idec(fb, v >> 27, flags, (u8 *)out + (row * m->mbw + col) * mbq * 16, m->mbw - col);
      while (r == MX_IPU_STARVED) {
        if (!more(&d)) {
          starved = 1;
          break;
        }
        r = mx_ipu_wait();
      }
      if (starved)
        break;
      if (r > 0)
        m->decoded += (u16)r;
      u32 st = mx_ipu_status();
      bad = r < 0 || MX_GET(IPU_CTRL_ECD, st) || !MX_GET(IPU_CTRL_SCD, st);
      if (r == MX_IPU_FULL) { /* the IPU was reset */
        m->iq_load[0] = m->iq_load[1] = 1;
        m->errors++;
        return skip_slices(buf, size, pos) == MX_MPEG_MORE ? MX_MPEG_MORE : MX_MPEG_PICTURE;
      }
    }
    if (bad) {
      /* on from the next start code, found on the EE */
      m->errors++;
      u32 p = next_code(buf, d.q0 * 16 + mx_ipu_pos() / 8 + 1, size);
      if (p + 4 + MX_MPEG_AHEAD > size) {
        starved = 1;
        break;
      }
      d.q0 = p >> 4;
      n = d.endq - d.q0;
      if (n > FEED_MAX)
        n = FEED_MAX;
      d.fedq = d.q0 + n;
      mx_ipu_input(buf + d.q0 * 16, n, (p & 15) * 8);
      code = 0x100u | buf[p + 3];
      continue;
    }
    code = mx_ipu_top();
  }
  if (starved) {
    mx_ipu_reset();
    m->iq_load[0] = m->iq_load[1] = 1;
    return MX_MPEG_MORE;
  }
  *pos = d.q0 * 16 + mx_ipu_pos() / 8;
  mx_ipu_input_end(); /* the channel would go on filling the FIFO with what follows */
  return MX_MPEG_PICTURE;
}

/* ---- to GS memory ------------------------------------------------------------ */

u32 mx_mpeg_chain(u64 *tags, const struct mx_mpeg *m, const void *mbs, u32 dbp, u32 dbw, u32 psm,
                  u32 x, u32 y, u32 end_id) {
  u32 mbq = m->flags & MX_IPU_RGB16 ? MX_IPU_MB_RGB16 : MX_IPU_MB_RGB32;
  u64 *c = tags;
  for (u32 i = 0; i < (u32)m->mbw * m->mbh; i++) {
    u32 regs = i ? 2 : 4;
    *c++ = mx_dmatag(MX_TAG_CNT, regs + 2, MX_PADDR_NONE, 0);
    *c++ = 0;
    *c++ = mx_giftag(regs, 0, -1, MX_GIF_FLG_PACKED, 1);
    *c++ = MX_GIF_REG_AD;
    if (!i) {
      *c++ = mx_gs_bitbltbuf(0, 0, 0, dbp, dbw, psm);
      *c++ = MX_GS_REG_BITBLTBUF;
      *c++ = mx_gs_trxreg(16, 16);
      *c++ = MX_GS_REG_TRXREG;
    }
    *c++ = mx_gs_trxpos(0, 0, x + (i % m->mbw) * 16, y + (i / m->mbw) * 16, 0);
    *c++ = MX_GS_REG_TRXPOS;
    *c++ = 0;
    *c++ = MX_GS_REG_TRXDIR;
    *c++ = mx_giftag(mbq, 1, -1, MX_GIF_FLG_IMAGE, 0);
    *c++ = 0;
    *c++ = mx_dmatag(MX_TAG_REF, mbq, mx_paddr_range((const u8 *)mbs + i * mbq * 16, mbq * 16), 0);
    *c++ = 0;
  }
  /* the texture cache must not keep the old picture */
  *c++ = mx_dmatag(end_id, 2, MX_PADDR_NONE, 0);
  *c++ = 0;
  *c++ = mx_giftag(1, 1, -1, MX_GIF_FLG_PACKED, 1);
  *c++ = MX_GIF_REG_AD;
  *c++ = 0;
  *c++ = MX_GS_REG_TEXFLUSH;
  mx_dcache_writeback(tags, (u32)((u8 *)c - (u8 *)tags));
  return (u32)(c - tags) / 2;
}
