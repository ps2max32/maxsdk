/* High resolution by passes (<mx/hires.h>). */

#include <mx/dma.h>
#include <mx/frame.h>
#include <mx/hires.h>
#include <mx/irq.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include "frame_int.h"

struct mx_hires_stats mx_hires_stats;

enum { FREE, BUILDING, WAITING, PLAYED, RETIRING };

/* the pass chains, written by the timer interrupt through the uncached
   segment: CALL with the pass's registers, the record, END */
#define PASS_QW 24 /* a chain is 19 quadwords, 21 with the FINISH */
/* a once record up to this size (quadwords) is sent in the first pass of
   its record, not over PATH3: PATH3 only gets the GS between passes, so a
   record could wait fields for it; larger ones (a level's textures) would
   make that pass late */
#define ONCE_INLINE_QW 1024
#define NPASS_CHAINS 16

static struct {
  struct mx_dmaq q;
  struct mx_rt rt;
  struct mx_display disp[2];
  u32 strip_h, strip_pages, page_h;
  u32 npass; /* strips */
  u32 zpsm;
  u32 line[MX_HIRES_PASSES]; /* the H-blank count at which the beam starts strip j */
  struct {
    u8 *mem;
    u8 *once;          /* the once record: sent over PATH3 when the record is complete */
    volatile int once_pending; /* its PATH3 DMA may still run */
    int once_inline;   /* to be CALLed by the record's first pass */
    volatile int state;
    u32 frame;
    u32 rgb;
    mx_event last; /* the DMA of its last pass */
    int kicked;
  } slot[MX_HIRES_SLOTS];
  u32 qw_per_slot, once_qw; /* main and once records' quadwords */
  volatile int played;   /* the slot played, -1: none yet */
  volatile u32 wait[MX_HIRES_SLOTS]; /* the waiting slots, oldest first */
  volatile u32 nwait;
  int building;          /* the slot of the frame being built, -1: none */
  u32 rgb;               /* clear colour of the frame being built */
  volatile u32 next_line; /* index into line[] of the next timer compare */
  volatile u32 parity;    /* the field the passes of this field are drawn for */
  int shift_field;
  mx_event prev;
  int prev_on;
  u32 prev_at, prev_k;
  u64 *chains;
  u32 nchain;
} h;

static u64 pass_mem[NPASS_CHAINS][PASS_QW * 2] MX_ALIGNED(64);
/* mx_hires_save_tga's line */
static u32 shot_line[2048] MX_ALIGNED(64);
static u8 shot_out[2048 * 3];

/* timer 0: the page of timers 2 and 3 is mapped read-only on the PS2 (the
   kernel's; a write is a TLB modified exception, PS2 2026-10-31) */
#define TMR_MODE MX_R32(MX_TIMER_T0_MODE)
#define TMR_COUNT MX_R32(MX_TIMER_T0_COUNT)
#define TMR_COMP MX_R32(MX_TIMER_T0_COMP)

/* A slot retired by the pass 0 that replaced it is free once the DMA of
   its last pass is done (interrupts off or in a handler). */
static void retire(void) {
  for (int i = 0; i < MX_HIRES_SLOTS; i++)
    if (h.slot[i].state == RETIRING && (!h.slot[i].kicked || mx_event_done(h.slot[i].last))) {
      h.slot[i].state = FREE;
      if (mx_gs.frames_done < h.slot[i].frame + 1)
        mx_gs.frames_done = h.slot[i].frame + 1;
    }
}

/* The chain of pass k of slot s (shifted: half a line lower; finish: a
   FINISH after it), in the next of the pass chains. */
static u64 *pass_chain(int s, u32 k, int shifted, int finish, int once) {
  u32 y0 = k * h.strip_h;
  /* the lines of the strip that are shown (the last one is shorter) */
  u32 rows = h.rt.h - y0 < h.strip_h ? h.rt.h - y0 : h.strip_h;
  u32 oy = h.rt.oy + (shifted ? 8 : 0);
  /* one FRAME for all strips: the stream may write it. Line y is line y of the buffer,
     SCISSOR chooses the strip. ZBUF is the pass's: at line y of strip k, Z is in strip
     k + 1, so ZBP is a strip after FRAME's base; the last strip's Z is strip 0, n - 1
     strips before it (mx_hires_init leaves room for that base, not memory it uses) */
  u32 fbp = h.rt.fbp;
  u32 zbp = fbp + ((k + 1) % h.npass) * h.strip_pages - k * h.strip_pages;
  u32 rgb = h.slot[s].rgb;
  u64 *c0 = MX_UNCACHED(pass_mem[h.nchain]);
  h.nchain = (h.nchain + 1) % NPASS_CHAINS;
  u64 *c = c0;
  /* the record's texture uploads, before anything of it is drawn */
  if (once)
    c = mx_e_call(c, h.slot[s].once, 0, 0, 0);
  /* the batches of the pass before are drawn before the registers change */
  c = mx_e_call(c, h.slot[s].mem, 16, mx_vif_flush(), mx_vif_direct(16));
  c = mx_e_ad_tag(c, 15, 1);
  /* PRIM's attributes are PRIM's (not PRMODE's), colours clamped */
  c = mx_e_ad(c, MX_GS_REG_PRMODECONT, 1);
  c = mx_e_ad(c, MX_GS_REG_COLCLAMP, 1);
  for (int ctx = 0; ctx < 2; ctx++) {
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_FRAME_1, ctx), mx_gs_frame(fbp, h.rt.fbw, h.rt.psm, 0));
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_ZBUF_1, ctx), mx_gs_zbuf(zbp, h.zpsm, (u32)ctx));
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_XYOFFSET_1, ctx), mx_gs_xyoffset(h.rt.ox, oy));
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_SCISSOR_1, ctx), mx_gs_scissor(0, h.rt.w - 1, y0, y0 + rows - 1));
  }
  /* the strip and its Z cleared */
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16 & 0xFF, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(h.rt.ox, oy + (y0 << 4), 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2,
              mx_gs_xyz(h.rt.ox + (h.rt.w << 4), oy + ((y0 + h.strip_h) << 4), 0));
  if (finish) {
    c = mx_e_end(c, 2, mx_vif_flush(), mx_vif_direct(2));
    c = mx_e_ad_tag(c, 1, 1);
    c = mx_e_ad(c, MX_GS_REG_FINISH, 1);
  } else {
    c = mx_e_end(c, 0, mx_vif_flush(), 0);
  }
  if (c - c0 > 2 * PASS_QW) {
    mx_printf("mx_hires: a pass chain of %d quadwords\n", (int)(c - c0) / 2);
    mx_exit(254);
  }
  /* the uncached writes are done before the DMA reads them */
  mx_sync();
  return c0;
}

/* Pass k of the slot played into the VIF1 queue (in the timer interrupt). */
static void kick(u32 k) {
  retire();
  if (k == 0) {
    mx_hires_stats.fields++;
    /* the oldest waiting record is played from this field on, once its
       texture uploads (PATH3) are done */
    int s0 = h.nwait ? (int)h.wait[0] : -1;
    if (s0 >= 0 && h.slot[s0].once_pending && !mx_dma_busy(MX_CH_GIF))
      h.slot[s0].once_pending = 0;
    if (s0 >= 0 && h.slot[s0].once_pending)
      mx_hires_stats.held++;
    if (h.nwait && !h.slot[s0].once_pending) {
      int old = h.played;
      h.played = (int)h.wait[0];
      for (u32 i = 1; i < h.nwait; i++)
        h.wait[i - 1] = h.wait[i];
      h.nwait--;
      h.slot[h.played].state = PLAYED;
      h.slot[h.played].kicked = 0;
      mx_hires_stats.shown++;
      if (old >= 0)
        h.slot[old].state = RETIRING;
      retire();
    }
    /* drawn during this field (the beam on its last strip) for the next */
    h.parity = (u32)MX_GET(GS_CSR_FIELD, MX_R64(MX_GS_CSR)) ^ 1;
  }
  if (h.played < 0)
    return;
  /* the pass before is still being sent: this one is left out (the queue
     stays one deep, and the pass chains it reads are not overwritten); its
     strip shows what was there */
  /* the pass before's time, from its kick to the end of its DMA */
  if (h.prev_on && mx_event_done(h.prev)) {
    u32 t = (h.q.t_last_end - h.prev_at) / (MX_EE_HZ / 1000000);
    if (t > mx_hires_stats.pass_us[h.prev_k])
      mx_hires_stats.pass_us[h.prev_k] = t;
    h.prev_on = 0;
  }
  if (h.prev_on && !mx_event_done(h.prev)) {
    mx_hires_stats.late++;
    return;
  }

  int shifted = mx_video.interlace && mx_video.ffmd && h.shift_field == (int)h.parity;
  int s = h.played;
  int once = h.slot[s].once_inline;
  u64 *c0 = pass_chain(s, k, shifted, 0, once);
  mx_event e;
  if (mx_dmaq_try_chain(&h.q, c0, 1, &e) < 0) {
    mx_hires_stats.late++;
    return;
  }
  h.prev_at = mx_count(), h.prev_k = k;
  h.prev = e, h.prev_on = 1;
  h.slot[s].last = e, h.slot[s].kicked = 1;
  h.slot[s].once_inline = 0;
  mx_hires_stats.passes++;
}

/* The beam starts strip j: pass j + 1 (j the last: pass 0, for the next
   field). */
static void timer_irq(void *arg) {
  (void)arg;
  TMR_MODE = TMR_MODE | (u32)MX_BIT(TIMER_MODE_EQUF);
  u32 j = h.next_line;
  if (j >= h.npass)
    return;
  h.next_line = j + 1;
  if (j + 1 < h.npass)
    TMR_COMP = h.line[j + 1];
  kick((j + 1) % h.npass);
}

/* The vblank starts: H-blanks counted from here. */
static void vsync_hook(void) {
  TMR_COUNT = 0;
  h.next_line = 0;
  TMR_COMP = h.line[0];
}

static u32 align_up(u32 v, u32 a) { return (v + a - 1) / a * a; }

u64 mx_hires_frame(u32 mask) { return mx_gs_frame(h.rt.fbp, h.rt.fbw, h.rt.psm, mask); }

int mx_hires_init(const struct mx_video_mode *m, u32 w, u32 hgt, u32 psm, void *mem, u32 bytes,
                  void *spr, u32 spr_bytes) {
  struct mx_video_mode mm = *m;
  if (mm.interlace)
    mm.ffmd = 1;
  if (mx_gs_init_video(&mm) < 0)
    return -1;
  int b16 = psm == MX_GS_PSM_CT16 || psm == MX_GS_PSM_CT16S;
  /* CT24 with Z24: no write reaches the strips' top bytes (textures there) */
  h.zpsm = psm == MX_GS_PSM_CT16S ? MX_GS_PSM_Z16S
           : b16                  ? MX_GS_PSM_Z16
           : psm == MX_GS_PSM_CT24 ? MX_GS_PSM_Z24
                                   : MX_GS_PSM_Z32;
  h.page_h = b16 ? 64 : 32;
  /* a third of the picture in whole pages; less when that leaves fewer
     than three strips (the beam would never reach the third's line) */
  h.strip_h = align_up((hgt + 2) / 3, h.page_h);
  while (h.strip_h > h.page_h && (hgt + h.strip_h - 1) / h.strip_h < 3)
    h.strip_h -= h.page_h;
  h.npass = (hgt + h.strip_h - 1) / h.strip_h;
  if (h.npass < 3 || h.npass > MX_HIRES_PASSES)
    return -1;
  u32 fbw = (w + 63) / 64;
  h.strip_pages = fbw * h.strip_h / h.page_h;
  /* the last strip's ZBP is n - 1 strips below the buffer (pass_chain): the buffer
     that high; the pages below it stay free for textures */
  u32 fbp = mx_vram_alloc_at((h.npass - 1) * h.strip_pages, h.npass * h.strip_pages);
  if (fbp == ~0u)
    return -1;
  mx_rt_init(&h.rt, fbp, w, hgt, psm, MX_RT_NOZ, MX_RT_NOZ);
  mx_gs.w = w, mx_gs.h = hgt, mx_gs.fbw = fbw;
  mx_gs.fbp[0] = mx_gs.fbp[1] = fbp;
  for (int i = 0; i < 2; i++)
    mx_display_init(&h.disp[i], &h.rt);
  mx_gs_displays(h.disp, 2);
  mx_display_apply(&h.disp[0]);

  /* the beam's line of each strip: DISPLAY2's DY (frame lines when
     interlaced) and MAGV; + 4, as gsKit_hires_flip found */
  u64 d = mx_gs.dregs[MX_DISP_DISPLAY2];
  u32 dy = (u32)(d >> 12) & 0x7FF, magv = ((u32)(d >> 27) & 3) + 1;
  u32 top = (dy + 4) >> (mm.interlace ? 1 : 0);
  u32 per = mm.interlace && !mm.ffmd ? 1 : 2; /* field lines per 2 buffer lines */
  for (u32 j = 0; j < h.npass; j++)
    h.line[j] = top + j * h.strip_h * magv * per / 2;
  /* line[npass - 1] kicks pass 0 of the next field, whose Z buffer is
     strip 1 (its clear writes all of it): with three strips the beam has
     only just left strip 1 there, so a few lines later (pass 0 still has
     the last strip and the vblank) */
  h.line[h.npass - 1] += 8;

  /* a slot: its main record, then its once record (a quarter) */
  u32 slot_qw = bytes / MX_HIRES_SLOTS / 16;
  h.once_qw = slot_qw / 4;
  h.qw_per_slot = slot_qw - h.once_qw;
  mx_dcache_invalidate(mem, slot_qw * 16 * MX_HIRES_SLOTS);
  /* the pass chains are written uncached: no dirty line of them (crt0's
     clearing of .bss) may be written back over one later */
  mx_dcache_invalidate(pass_mem, sizeof pass_mem);
  for (int i = 0; i < MX_HIRES_SLOTS; i++) {
    h.slot[i].mem = (u8 *)mem + i * slot_qw * 16;
    h.slot[i].once = h.slot[i].mem + h.qw_per_slot * 16;
    h.slot[i].once_pending = 0;
    h.slot[i].state = FREE;
    h.slot[i].rgb = 0;
    h.slot[i].kicked = 0;
  }
  h.played = -1;
  h.nwait = 0;
  h.building = -1;
  h.rgb = 0;
  h.shift_field = 0;
  h.prev_on = 0;
  h.nchain = 0;
  mx_hires_stats = (struct mx_hires_stats){0};

  mx_dmaq_init(&h.q, MX_CH_VIF1);
  mx__frame_init_record(spr, spr_bytes);

  /* timer 0 counts H-blanks; its compare is the next strip's line */
  TMR_MODE = 0;
  h.next_line = h.npass;
  TMR_COUNT = 0;
  TMR_COMP = 0xFFFF;
  mx_irq_intc(MX_INTC_TIM0, timer_irq, 0);
  TMR_MODE = (u32)(MX_SET(TIMER_MODE_CLKS, 3) | MX_BIT(TIMER_MODE_CUE) | MX_BIT(TIMER_MODE_CMPE) |
                  MX_BIT(TIMER_MODE_EQUF) | MX_BIT(TIMER_MODE_OVFF));
  int was = mx_di();
  mx__vsync_hook = vsync_hook;
  mx_irq_restore(was);
  return 0;
}

static int slot_free(void) {
  int was = mx_di();
  retire();
  int r = -1;
  for (int i = 0; i < MX_HIRES_SLOTS && r < 0; i++)
    if (h.slot[i].state == FREE)
      r = i;
  mx_irq_restore(was);
  return r;
}

int mx__hires_ready(void) { return h.building >= 0 || slot_free() >= 0; }

u8 *mx__hires_begin(u32 *qw, u8 **once, u32 *once_qw) {
  if (h.building < 0) {
    u32 t0 = mx_count();
    int s;
    int reported = 0;
    while ((s = slot_free()) < 0)
      if (!reported && mx_count() - t0 > MX_EE_HZ) {
        mx_printf("mx_hires: no free record for 1 s (played %d, waiting %u, fields %u)\n",
                  h.played, h.nwait, mx_hires_stats.fields);
        reported = 1;
      }
    h.slot[s].state = BUILDING;
    h.slot[s].rgb = h.rgb;
    h.building = s;
  }
  *qw = h.qw_per_slot;
  *once = h.slot[h.building].once;
  *once_qw = h.once_qw;
  return h.slot[h.building].mem;
}

void mx__hires_end(u8 *end, u8 *once_end, int full) {
  int s = h.building;
  /* the once record (texture uploads to places no frame shown uses) over
     PATH3 now, while the frames before are played: on the GIF channel its
     tags' VIF codes are not sent, and its data are GIF packets */
  h.slot[s].once_pending = 0;
  h.slot[s].once_inline = 0;
  u32 once_n = (u32)(once_end - h.slot[s].once) / 16;
  if (once_n > 1 && once_n <= ONCE_INLINE_QW) {
    h.slot[s].once_inline = 1;
  } else if (once_n > 1) {
    /* the once records before are sent: theirs are no longer pending (pass
       0 only sees whether the channel is busy, which is then this one) */
    mx_dma_wait(MX_CH_GIF);
    int was = mx_di();
    for (int i = 0; i < MX_HIRES_SLOTS; i++)
      h.slot[i].once_pending = 0;
    mx_irq_restore(was);
    mx_dma_chain(MX_CH_GIF, h.slot[s].once, 0);
    h.slot[s].once_pending = 1;
  }
  u32 used = (u32)(end - h.slot[s].mem);
  if (used > mx_hires_stats.rec_max)
    mx_hires_stats.rec_max = used;
  if (full && !mx_hires_stats.rec_full++)
    mx_printf("mx_hires: a frame does not fit its record (%u bytes)\n", h.qw_per_slot * 16);
  h.slot[s].rgb = h.rgb;
  h.slot[s].frame = mx_gs.frames;
  int was = mx_di();
  h.slot[s].state = WAITING;
  h.wait[h.nwait++] = (u32)s;
  mx_irq_restore(was);
  h.building = -1;
}

void mx__hires_stop(void) {
  int was = mx_di();
  mx__vsync_hook = 0;
  mx_irq_restore(was);
  TMR_MODE = 0;
  mx_irq_intc(MX_INTC_TIM0, 0, 0);
  /* the pass being sent and a once record over PATH3 finish: the pipeline
     can be started again (another mode) */
  mx_dmaq_release(&h.q);
  mx_dma_wait(MX_CH_GIF);
}

u32 mx_hires_slot(void) {
  if (h.building < 0) {
    u32 qw, oqw;
    u8 *once;
    mx__hires_begin(&qw, &once, &oqw);
  }
  return (u32)h.building;
}

u32 mx_hires_passes(void) { return h.npass; }
u32 mx_hires_hbyte(u32 *page) {
  *page = h.rt.fbp;
  return h.rt.psm == MX_GS_PSM_CT24 ? h.npass * h.strip_pages : 0;
}

void mx_hires_clear(u32 r, u32 g, u32 b) { h.rgb = (r & 0xFF) | (g & 0xFF) << 8 | (b & 0xFF) << 16; }

void mx_hires_shift_field(int f) { h.shift_field = f; }

const struct mx_rt *mx_hires_rt(void) { return &h.rt; }

/* Each strip drawn alone (its pass, then read back before the next pass
   uses it as Z), for both fields in interlaced FRAME mode: their lines one
   after the other, the field drawn higher first. */
int mx_hires_save_tga(const char *path) {
  /* the newest record played, then the passes stopped */
  u32 t0 = mx_count();
  while (h.nwait && mx_count() - t0 < MX_EE_HZ)
    ;
  int was = mx_di();
  mx__vsync_hook = 0;
  mx_irq_restore(was);
  TMR_MODE = 0;
  mx_irq_intc(MX_INTC_TIM0, 0, 0);
  mx_dmaq_drain(&h.q);
  int s = h.played;
  if (s < 0)
    return -1;
  int both = mx_video.interlace && mx_video.ffmd && h.shift_field >= 0;
  int twice = mx_video.interlace && mx_video.ffmd;
  u32 w = h.rt.w > 2048 ? 2048 : h.rt.w, hh = h.rt.h, rows = h.strip_h;
  int b16 = h.rt.psm == MX_GS_PSM_CT16 || h.rt.psm == MX_GS_PSM_CT16S;
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0)
    return -1;
  u32 oh = twice ? 2 * hh : hh;
  u8 hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (u8)w, (u8)(w >> 8), (u8)oh, (u8)(oh >> 8), 24, 0x20};
  mx_fio_write(fd, hdr, sizeof hdr);
  for (u32 k = 0; k < h.npass; k++) {
    u32 n = hh - k * rows < rows ? hh - k * rows : rows;
    for (int f = 0; f < 1 + both; f++) {
      u32 fin = mx_gs.finish;
      mx_dmaq_chain(&h.q, pass_chain(s, k, f, 1, h.slot[s].once_inline), 1);
      h.slot[s].once_inline = 0;
      mx_dmaq_drain(&h.q);
      mx_gs_wait_finish(fin, 1);
      for (u32 y = 0; y < n; y++) {
        if (mx_gs_download(shot_line, MX_GS_PAGE(h.rt.fbp + k * h.strip_pages), h.rt.fbw,
                           b16 ? h.rt.psm : MX_GS_PSM_CT32, 0, y, w, 1) < 0)
          break;
        for (u32 x = 0; x < w; x++) {
          u32 p = b16 ? ((const u16 *)shot_line)[x] : shot_line[x];
          u32 r = b16 ? (p & 31) << 3 : p & 0xFF, g = b16 ? (p >> 5 & 31) << 3 : p >> 8 & 0xFF,
              b = b16 ? (p >> 10 & 31) << 3 : p >> 16 & 0xFF;
          shot_out[3 * x] = (u8)b, shot_out[3 * x + 1] = (u8)g, shot_out[3 * x + 2] = (u8)r;
        }
        /* the line's place: in both fields' lines when twice */
        u32 line = k * rows + y;
        for (int c = 0; c < 1 + (twice && !both); c++) {
          u32 at = twice ? 2 * line + (u32)(both ? f : c) : line;
          mx_fio_lseek(fd, (int)(18 + at * w * 3), MX_FIO_SEEK_SET);
          mx_fio_write(fd, shot_out, w * 3);
        }
      }
    }
  }
  /* the header again (once it was lost on the PS2: zeros, the lines right) */
  mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
  mx_fio_write(fd, hdr, sizeof hdr);
  mx_fio_close(fd);
  mx_dmaq_release(&h.q);
  return 0;
}
