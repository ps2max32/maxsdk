/* High resolution by passes (<mx/hires.h>). */

#include <mx/dma.h>
#include <mx/frame.h>
#include <mx/hires.h>
#include <mx/timer.h>
#include <mx/vcache.h>
#include <mx/vram.h>
#include <mx/vu.h>
#include <mx/irq.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include "frame_int.h"

struct mx_hires_stats mx_hires_stats;

enum { FREE, BUILDING, WAITING, PLAYED, RETIRING };

/* the pass chains, written by the timer interrupt through the uncached
   segment: CALL with the pass's registers, the record, END */
#define PASS_QW 24 /* a chain is 20 quadwords, 22 with the FINISH */
/* a once record up to this size (quadwords) is sent in the first pass of
   its record, not over PATH3: PATH3 only gets the GS between passes, so a
   record could wait fields for it; larger ones (a level's textures) would
   make that pass late */
#define ONCE_INLINE_QW 1024
#define NPASS_CHAINS 32

static struct {
  struct mx_dmaq q;
  struct mx_rt rt;
  struct mx_display disp[2];
  u32 strip_h, strip_pages, page_h;
  u32 pages; /* the framebuffer's */
  u32 npass; /* strips */
  u32 zpsm;
  u32 line[MX_HIRES_PASSES]; /* the H-blank count at which the beam starts strip j */
  struct {
    u8 *mem;
    u8 *once;          /* the once record: sent over PATH3 when the record is complete */
    u8 *strip[MX_HIRES_PASSES]; /* the strips' own records (mx_hires_strip_records) */
    volatile int once_pending; /* its PATH3 DMA may still run */
    int once_inline;   /* to be CALLed by the record's first pass */
    volatile int state;
    u32 frame;
    u32 rgb;
    mx_event last; /* the DMA of its last pass */
    int kicked;
    int layout;    /* L_*: where its strips lie (mx_hires_widths) */
  } slot[MX_HIRES_SLOTS];
  u32 qw_per_slot, once_qw; /* main and once records' quadwords */
  u32 strip_qw;             /* a strip's own record's, 0: none */
  /* the passes that may run and are not sent yet, in the order they came */
  volatile u8 queue[MX_HIRES_PASSES];
  volatile u32 nqueue;
  volatile int busy;        /* a pass is being sent */
  volatile int played;   /* the slot played, -1: none yet */
  /* the display is off from mx_hires_init until a record was played a whole
     field (the framebuffer holds what was there before: another program's
     picture, or data read as pixels); the circuits' on flags meanwhile */
  volatile u32 first_field; /* the field the first record was played from, 0: none yet */
  int hidden;
  u8 disp_on[2][2];
  volatile u32 wait[MX_HIRES_SLOTS]; /* the waiting slots, oldest first */
  volatile u32 nwait;
  int building;          /* the slot of the frame being built, -1: none */
  u32 rgb;               /* clear colour of the frame being built */
  volatile u32 next_line; /* index into line[] of the next timer compare */
  volatile u32 parity;    /* the field the passes of this field are drawn for */
  int shift_field;
  u32 prev_at, prev_k;
  u32 may_at[MX_HIRES_PASSES]; /* when each strip's pass might run, this field */
  struct mx_hires_pass *log, *log_cur; /* mx_hires_log; the entry of the pass being sent */
  u32 log_n;
  u64 *chains;
  u32 nchain;
} h;

static u64 pass_mem[NPASS_CHAINS][PASS_QW * 2] MX_ALIGNED(64);

/* A picture drawn at its whole width or at half of it, from record to
   record (mx_hires_widths, mx_hires_width): its strips lie in one of three
   layouts. L_FULL the buffer as mx_hires_init made it; at half the width
   the strips take half the pages, at the buffer's start (L_FRONT) or at
   its end (L_END). A record's passes draw while the beam still shows the
   record before (the strips it has not reached yet), so a record may only
   follow one whose strips its own do not overwrite before the beam has
   shown them: L_FRONT follows anything; L_FULL follows L_FULL or L_END
   (whose strips lie behind its own); so from L_FRONT back to the whole
   width one record of L_END first. While the records are L_FRONT, the rest
   of the buffer (but its last row of pages, mx_hires_spare's) and the rest
   of the Z strip are free: given to GS memory (the cache takes them), and
   taken back before a record of L_END (mx_vcache_release). The display
   changes at the vblank after a record of another layout began to be
   played: its strips were all drawn in that field. */
enum { L_FULL, L_FRONT, L_END, L_N };
static struct {
  u32 fbp, fbw, w, strip_pages, ox; /* ox: XYOFFSET's x, the picture of that width centred as h.rt's */
} lay[L_N];
static int cfg_widths, widths; /* mx_hires_widths: asked for; this picture's */
static u32 want_w;             /* mx_hires_width */
static int cur_l;              /* the layout of the last record begun */
static int shown_l;            /* the display's */
static volatile int disp_next; /* the layout for the display at the next vblank, -1: none */
static int lent;               /* the spare room of the half layouts is GS memory's */
static int front_new;          /* the record being built is the first L_FRONT after another */
static u32 front_first;        /* the frame of the first L_FRONT record since another, ~0u: not built yet */
static u32 spare_fb, spare_fb_n, spare_z, spare_z_n; /* that room */
static u32 cfg_strip_lines; /* mx_hires_strip_lines */
static int cfg_own_z;       /* mx_hires_own_z */
static u32 cfg_ahead = MX_HIRES_SLOTS; /* mx_hires_ahead: the most records waiting to be played */
static int own_z;           /* this picture's: the passes' Z is a strip of its own */
static u32 own_zbp;         /* its first page */
/* mx_hires_save_tga's line */
static u32 shot_line[2048] MX_ALIGNED(64);
static u8 shot_out[2048 * 3];

/* an EE timer of its own (mx_timer_claim): it counts H-blanks */
static int tmr = -1;
static u32 tmr_base;
#define TMR_MODE MX_R32(tmr_base + MX_TIMER_MODE_OFFSET)
#define TMR_COUNT MX_R32(tmr_base + MX_TIMER_COUNT_OFFSET)
#define TMR_COMP MX_R32(tmr_base + MX_TIMER_COMP_OFFSET)
static void tmr_off(void) {
  if (tmr < 0)
    return;
  TMR_MODE = 0;
  mx_irq_intc(MX_INTC_TIM0 + tmr, 0, 0);
  mx_timer_release(tmr);
  tmr = -1;
}

/* A slot retired by the pass 0 that replaced it is free once the DMA of
   its last pass is done (interrupts off or in a handler). The frames are
   done up to the oldest one still played or retiring: a record none of whose
   passes was sent (all left out) is free at once, while an older one's last
   pass may still be read (its memory must not be taken for a new frame). */
static u32 freed_to; /* one past the newest frame freed */
static void retire(void) {
  for (int i = 0; i < MX_HIRES_SLOTS; i++)
    if (h.slot[i].state == RETIRING && (!h.slot[i].kicked || mx_event_done(h.slot[i].last))) {
      h.slot[i].state = FREE;
      if (freed_to < h.slot[i].frame + 1)
        freed_to = h.slot[i].frame + 1;
    }
  u32 to = freed_to;
  for (int i = 0; i < MX_HIRES_SLOTS; i++)
    if ((h.slot[i].state == RETIRING || h.slot[i].state == PLAYED) && h.slot[i].frame < to)
      to = h.slot[i].frame;
  if (mx_gs.frames_done < to)
    mx_gs.frames_done = to;
}

/* The chain of pass k of slot s (shifted: half a line lower; finish: a
   FINISH after it), in the next of the pass chains. */
/* the dither matrices, one a field in turn (mx_hires_dither); 0: the
   stream's own DIMX */
#define GIF_AD_NOP 0x7F /* SCE_GS_NOP (GIF Starting Guide): an A+D write to no register */
static u64 dither_m[MX_HIRES_DITHER];
static u32 dither_n;

void mx_hires_dither(const u64 *dimx, u32 n) {
  n = n > MX_HIRES_DITHER ? MX_HIRES_DITHER : n;
  for (u32 i = 0; i < n; i++)
    dither_m[i] = dimx[i];
  dither_n = n;
}

static u64 *pass_chain(int s, u32 k, int shifted, int finish, int once) {
  u32 y0 = k * h.strip_h;
  /* the lines of the strip that are shown (the last one is shorter) */
  u32 rows = h.rt.h - y0 < h.strip_h ? h.rt.h - y0 : h.strip_h;
  u32 oy = h.rt.oy + (shifted ? 8 : 0);
  /* the slot's layout (mx_hires_widths) */
  u32 fbp = h.rt.fbp, fbw = h.rt.fbw, w = h.rt.w, sp = h.strip_pages, ox = h.rt.ox;
  if (widths) {
    int l = h.slot[s].layout;
    fbp = lay[l].fbp, fbw = lay[l].fbw, w = lay[l].w, sp = lay[l].strip_pages, ox = lay[l].ox;
  }
  /* one FRAME for all strips: the stream may write it. Line y is line y of the buffer,
     SCISSOR chooses the strip. ZBUF is the pass's: at line y of strip k, Z is in strip
     k + 1, so ZBP is a strip after FRAME's base; the last strip's Z is strip 0, n - 1
     strips before it (mx_hires_init leaves room for that base, not memory it uses) */
  u32 zbp = fbp + ((k + 1) % h.npass) * sp - k * sp;
  /* a strip of its own for the Z (mx_hires_own_z): behind the buffer, so that the base
     of strip k's lines is not below page 0 */
  if (own_z)
    zbp = own_zbp - k * sp;
  u32 rgb = h.slot[s].rgb;
  u64 *c0 = MX_UNCACHED(pass_mem[h.nchain]);
  h.nchain = (h.nchain + 1) % NPASS_CHAINS;
  u64 *c = c0;
  /* the record's texture uploads, before anything of it is drawn */
  if (once)
    c = mx_e_call(c, h.slot[s].once, 0, 0, 0);
  /* the batches of the pass before are drawn before the registers change */
  c = mx_e_call(c, h.slot[s].mem, 17, mx_vif_flush(), mx_vif_direct(17));
  c = mx_e_ad_tag(c, 16, 1);
  /* this field's dither matrix (the stream writes none then), else the
     one in effect again (a NOP write) */
  if (dither_n)
    c = mx_e_ad(c, MX_GS_REG_DIMX, dither_m[mx_hires_stats.fields % dither_n]);
  else
    c = mx_e_ad(c, GIF_AD_NOP, 0);
  /* PRIM's attributes are PRIM's (not PRMODE's), colours clamped */
  c = mx_e_ad(c, MX_GS_REG_PRMODECONT, 1);
  c = mx_e_ad(c, MX_GS_REG_COLCLAMP, 1);
  for (int ctx = 0; ctx < 2; ctx++) {
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_FRAME_1, ctx), mx_gs_frame(fbp, fbw, h.rt.psm, 0));
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_ZBUF_1, ctx), mx_gs_zbuf(zbp, h.zpsm, (u32)ctx));
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_XYOFFSET_1, ctx), mx_gs_xyoffset(ox, oy));
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_SCISSOR_1, ctx), mx_gs_scissor(0, w - 1, y0, y0 + rows - 1));
  }
  /* the strip and its Z cleared */
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16 & 0xFF, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(ox, oy + (y0 << 4), 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(ox + (w << 4), oy + ((y0 + h.strip_h) << 4), 0));
  /* the strip's own record of the frame, after the main one */
  if (h.strip_qw)
    c = mx_e_call(c, h.slot[s].strip[k], 0, 0, 0);
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

void mx_hires_log(struct mx_hires_pass *log, u32 n) {
  int was = mx_di();
  h.log = n ? log : 0, h.log_n = n, h.log_cur = 0;
  mx_hires_stats.logged = 0;
  mx_irq_restore(was);
}

/* The log's next entry for a pass of strip k (0: no log). */
static struct mx_hires_pass *log_pass(u32 k) {
  if (!h.log)
    return 0;
  struct mx_hires_pass *e = &h.log[mx_hires_stats.logged++ % h.log_n];
  e->frame = h.played >= 0 ? h.slot[h.played].frame : 0;
  e->wait_us = (mx_count() - h.may_at[k]) / (MX_EE_HZ / 1000000);
  e->run_us = 0;
  e->strip = (u8)k, e->over = 0, e->out = 0, e->pad = 0;
  return e;
}

/* Pass k of the slot played into the VIF1 queue (in an interrupt handler,
   or with interrupts off). Returns 0, or -1: not sent. */
static int send(u32 k) {
  int shifted = mx_video.interlace && mx_video.ffmd && h.shift_field == (int)h.parity;
  int s = h.played;
  int once = h.slot[s].once_inline;
  u64 *c0 = pass_chain(s, k, shifted, 0, once);
  mx_event e;
  if (mx_dmaq_try_chain(&h.q, c0, 1, &e) < 0)
    return -1;
  h.log_cur = log_pass(k);
  h.prev_at = mx_count(), h.prev_k = k;
  h.busy = 1;
  h.slot[s].last = e, h.slot[s].kicked = 1;
  h.slot[s].once_inline = 0;
  mx_hires_stats.passes++;
  return 0;
}

/* The next pass that may run, if none is being sent. */
static void start_next(void) {
  while (!h.busy && h.nqueue) {
    u32 k = h.queue[0];
    for (u32 i = 1; i < h.nqueue; i++)
      h.queue[i - 1] = h.queue[i];
    h.nqueue--;
    if (send(k) < 0) {
      struct mx_hires_pass *e = log_pass(k);
      if (e)
        e->out = 1;
      mx_hires_stats.late++;
    }
  }
}

void mx__gs_event(void); /* gs.c: a thread that waits for a record looks again */
/* A pass's DMA has ended (the queue's end interrupt): its time, from its
   start; then the next one. */
static MX_IRQ_FN void pass_done(void *arg) {
  (void)arg;
  if (!h.busy)
    return;
  u32 t = (h.q.t_last_end - h.prev_at) / (MX_EE_HZ / 1000000);
  if (t > mx_hires_stats.pass_us[h.prev_k])
    mx_hires_stats.pass_us[h.prev_k] = t;
  mx_hires_stats.pass_n[h.prev_k]++;
  mx_hires_stats.pass_sum_us[h.prev_k] += t;
  if (h.log_cur)
    h.log_cur->run_us = t ? t : 1, h.log_cur = 0;
  h.busy = 0;
  start_next();
  mx__gs_event();
}

/* the displays to layout l (with interrupts off) */
static void disp_layout(int l) {
  for (int i = 0; i < 2; i++) {
    struct mx_circuit *c = &h.disp[i].c[1];
    c->fbp = lay[l].fbp, c->fbw = lay[l].fbw, c->w = lay[l].w;
  }
  shown_l = l;
  if (!h.hidden)
    mx_display_apply(&h.disp[0]);
}

/* Pass k may run from now: the beam has left strip k + 1, which the pass
   takes for its Z (in an interrupt handler). */
static void may_run(u32 k) {
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
      if (!h.first_field)
        h.first_field = mx_hires_stats.fields;
      for (u32 i = 1; i < h.nwait; i++)
        h.wait[i - 1] = h.wait[i];
      h.nwait--;
      h.slot[h.played].state = PLAYED;
      h.slot[h.played].kicked = 0;
      /* another layout: the display follows at the vblank, when this field's
         passes have drawn all of its strips */
      if (widths && h.slot[h.played].layout != shown_l)
        disp_next = h.slot[h.played].layout;
      mx_hires_stats.shown++;
      if (old >= 0)
        h.slot[old].state = RETIRING;
      retire();
      mx__gs_event();
    }
    /* drawn during this field for the next */
    h.parity = (u32)MX_GET(GS_CSR_FIELD, MX_R64(MX_GS_CSR)) ^ 1;
  }
  if (h.played < 0)
    return;
  h.may_at[k] = mx_count();
  if (h.nqueue < MX_HIRES_PASSES)
    h.queue[h.nqueue++] = (u8)k;
  start_next();
}

/* The beam comes to strip k: a pass for it that has not started is left
   out (its strip shows what was there; the passes after it keep their
   time). */
static void too_late(u32 k) {
  if (h.busy && !mx_hires_stats.stuck[15] && mx_count() - h.prev_at > MX_EE_HZ / 33) {
    u32 *st = mx_hires_stats.stuck;
    st[0] = MX_R32(MX_VIF_VIF1_STAT), st[1] = MX_R32(MX_VIF_VIF1_CODE);
    st[2] = MX_R32(MX_DMA_VIF1_CHCR), st[3] = MX_R32(MX_DMA_VIF1_MADR), st[4] = MX_R32(MX_DMA_VIF1_QWC);
    st[5] = MX_R32(MX_DMA_VIF1_TADR), st[6] = MX_R32(MX_DMA_VIF1_ASR0), st[7] = MX_R32(MX_DMA_VIF1_ASR1);
    st[8] = MX_R32(MX_GIF_STAT);
    st[9] = MX_R32(MX_DMA_GIF_CHCR), st[10] = MX_R32(MX_DMA_GIF_MADR), st[11] = MX_R32(MX_DMA_GIF_QWC);
    st[12] = MX_R32(MX_DMA_GIF_TADR), st[13] = h.prev_k, st[14] = mx_vpu_stat(), st[15] = 1;
    if (st[14] & MX_VPU_VBS1)
      mx_vu1_stop_dump(mx_hires_stats.stuck_vu1);
    u32 ta = st[5] & 0x01FFFFF0u;
    if (ta >= 16 * 16 && ta + 4 * 16 <= 0x02000000u)
      for (u32 i = 0; i < 20; i++)
        for (u32 j = 0; j < 4; j++)
          mx_hires_stats.stuck_mem[i][j] = *(volatile u32 *)(uintptr_t)(0xA0000000u + ta - 16 * 16 + i * 16 + j * 4);
  }
  if (h.busy && h.prev_k == k) {
    mx_hires_stats.over++;
    if (h.log_cur)
      h.log_cur->over = 1;
  }
  for (u32 i = 0; i < h.nqueue; i++)
    if (h.queue[i] == k) {
      for (u32 j = i + 1; j < h.nqueue; j++)
        h.queue[j - 1] = h.queue[j];
      h.nqueue--;
      struct mx_hires_pass *e = log_pass(k);
      if (e)
        e->out = 1;
      mx_hires_stats.late++;
      return;
    }
}

/* The beam starts strip j: it has left strip j - 1, the Z of pass j - 2,
   which may run now. The pass of the last strip but one waits for the end
   of the last strip, the vblank (vsync_hook). */
/* an interrupt handler: pinned (mx OS plan 4b) */
static MX_IRQ_FN void timer_irq(void *arg) {
  (void)arg;
  TMR_MODE = TMR_MODE | (u32)MX_BIT(TIMER_MODE_EQUF);
  u32 j = h.next_line;
  if (j >= h.npass)
    return;
  h.next_line = j + 1;
  if (j + 1 < h.npass)
    TMR_COMP = h.line[j + 1];
  too_late(j);
  /* with a Z strip of its own a pass waits for its own strip alone: the beam has
     left strip j - 1 */
  if (j)
    may_run(own_z ? j - 1 : (j + h.npass - 2) % h.npass);
}

/* The vblank starts: H-blanks counted from here. The beam has left the
   last strip: the pass of the one before it may run. */
static void vsync_hook(void) {
  TMR_COUNT = 0;
  h.next_line = 0;
  TMR_COMP = h.line[0];
  if (disp_next >= 0) {
    disp_layout(disp_next);
    disp_next = -1;
  }
  may_run(own_z ? h.npass - 1 : h.npass - 2);
}

void mx_hires_strip_lines(u32 lines) { cfg_strip_lines = lines; }
void mx_hires_own_z(int on) { cfg_own_z = on; }
void mx_hires_widths(int on) { cfg_widths = on; }
void mx_hires_width(u32 w) { want_w = w; }
void mx_hires_ahead(u32 n) { cfg_ahead = n ? n : 1; }
u32 mx_hires_strip_height(void) { return h.strip_h; }

static u32 align_up(u32 v, u32 a) { return (v + a - 1) / a * a; }

u64 mx_hires_frame(u32 mask) {
  if (widths && h.building >= 0) {
    int l = h.slot[h.building].layout;
    return mx_gs_frame(lay[l].fbp, lay[l].fbw, h.rt.psm, mask);
  }
  return mx_gs_frame(h.rt.fbp, h.rt.fbw, h.rt.psm, mask);
}

/* mx_hires_init's memory, for the pipeline's restart after a pause */
static void *cfg_mem, *cfg_spr;
static u32 cfg_bytes, cfg_spr_bytes;
static int paused; /* mx__hires_pause: the pipeline stopped until the next frame */
static int pipeline_start(void);

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
  h.strip_h = align_up(cfg_strip_lines ? cfg_strip_lines : (hgt + 2) / 3, h.page_h);
  while (h.strip_h > h.page_h && (hgt + h.strip_h - 1) / h.strip_h < 3)
    h.strip_h -= h.page_h;
  h.npass = (hgt + h.strip_h - 1) / h.strip_h;
  if (h.npass < 3 || h.npass > MX_HIRES_PASSES)
    return -1;
  u32 fbw = (w + 63) / 64;
  h.strip_pages = fbw * h.strip_h / h.page_h;
  /* the last strip's ZBP is n - 1 strips below the buffer (pass_chain): the buffer
     that high; the pages below it stay free for textures. The last strip has the
     pages of its lines only (540 lines in strips of 128: 28, a row of pages) */
  u32 last_rows = (hgt - (h.npass - 1) * h.strip_h + h.page_h - 1) / h.page_h;
  h.pages = (h.npass - 1) * h.strip_pages + last_rows * fbw;
  own_z = cfg_own_z;
  /* with a Z strip of its own the buffer starts GS memory and the strip follows it */
  u32 fbp = mx_vram_alloc_at(own_z ? 0 : (h.npass - 1) * h.strip_pages, h.pages);
  if (fbp == ~0u)
    return -1;
  if (own_z) {
    own_zbp = mx_vram_alloc_at(fbp + h.pages, h.strip_pages);
    if (own_zbp == ~0u) {
      mx_vram_free(fbp, h.pages);
      return -1;
    }
  }
  mx_rt_init(&h.rt, fbp, w, hgt, psm, MX_RT_NOZ, MX_RT_NOZ);
  mx_gs.w = w, mx_gs.h = hgt, mx_gs.fbw = fbw;
  mx_gs.fbp[0] = mx_gs.fbp[1] = fbp;
  /* the layouts at half the width (with a Z strip of its own: the Z is the
     same place in all of them) */
  widths = cfg_widths && own_z && (w / 2) % 64 == 0;
  cur_l = shown_l = L_FULL, disp_next = -1, lent = 0, front_new = 0, front_first = ~0u;
  if (widths) {
    u32 hfbw = w / 2 / 64, hsp = hfbw * h.strip_h / h.page_h;
    u32 hpages = (h.npass - 1) * hsp + last_rows * hfbw;
    lay[L_FULL].fbp = fbp, lay[L_FULL].fbw = fbw, lay[L_FULL].w = w, lay[L_FULL].strip_pages = h.strip_pages;
    lay[L_FRONT].fbp = fbp, lay[L_END].fbp = fbp + h.pages - hpages;
    lay[L_FULL].ox = h.rt.ox;
    for (int l = L_FRONT; l <= L_END; l++)
      lay[l].fbw = hfbw, lay[l].w = w / 2, lay[l].strip_pages = hsp, lay[l].ox = h.rt.ox + ((w - w / 2) / 2 << 4);
    /* the spare room: the buffer after L_FRONT's strips up to L_END's last
       row (its spare blocks are mx_hires_spare's in every layout), and the Z
       strip's second half */
    u32 row = hgt / h.page_h;
    spare_fb = fbp + hpages, spare_fb_n = lay[L_END].fbp + row * hfbw - spare_fb;
    spare_z = own_zbp + hsp, spare_z_n = h.strip_pages - hsp;
  }
  for (int i = 0; i < 2; i++)
    mx_display_init(&h.disp[i], &h.rt);
  for (int i = 0; i < 2; i++)
    for (int c = 0; c < 2; c++)
      h.disp_on[i][c] = h.disp[i].c[c].on, h.disp[i].c[c].on = 0;
  h.hidden = 1, h.first_field = 0;
  mx_gs_displays(h.disp, 2);
  mx_display_apply(&h.disp[0]); /* off: PMODE's circuits */

  /* the beam's line of each strip: DISPLAY2's DY (frame lines when
     interlaced) and MAGV; + 4, as gsKit_hires_flip found */
  u64 d = mx_gs.dregs[MX_DISP_DISPLAY2];
  u32 dy = (u32)(d >> 12) & 0x7FF, magv = ((u32)(d >> 27) & 3) + 1;
  u32 top = (dy + 4) >> (mm.interlace ? 1 : 0);
  u32 per = mm.interlace && !mm.ffmd ? 1 : 2; /* field lines per 2 buffer lines */
  for (u32 j = 0; j < h.npass; j++)
    h.line[j] = top + j * h.strip_h * magv * per / 2;
  /* the beam must have left a pass's Z strip (its clear writes all of it)
     when the pass may run: line j lets pass j - 2 run, a few lines into
     strip j */
  for (u32 j = 1; j < h.npass; j++)
    h.line[j] += 8;

  cfg_mem = mem, cfg_bytes = bytes, cfg_spr = spr, cfg_spr_bytes = spr_bytes;
  mx_hires_stats = (struct mx_hires_stats){0};
  /* VU1 and VIF1 from the start: a program before may have left VU1 running */
  if (mx_vpu_stat() & MX_VPU_VBS1)
    mx_vu_reset(2);
  paused = 0;
  return pipeline_start();
}

/* The passes' pipeline: the records, the queue on VIF1, the timer at the
   strips' lines, the vblank hook. Started by mx_hires_init, and again after
   a pause (another process had the graphics: mx__hires_pause). The video
   mode, the framebuffer and the display stay as they are. */
static int pipeline_start(void) {
  void *mem = cfg_mem, *spr = cfg_spr;
  u32 bytes = cfg_bytes, spr_bytes = cfg_spr_bytes;
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
  freed_to = mx_gs.frames_done;
  h.building = -1;
  h.rgb = 0;
  h.shift_field = 0;
  h.busy = 0;
  h.nqueue = 0;
  h.strip_qw = 0;
  h.nchain = 0;

  mx_dmaq_init(&h.q, MX_CH_VIF1);
  h.q.on_done = pass_done;
  h.q.on_done_arg = 0;
  mx__frame_init_record(spr, spr_bytes);

  /* a timer counts H-blanks; its compare is the next strip's line */
  if (tmr < 0)
    tmr = mx_timer_claim(MX_TIMER_HBLANK);
  if (tmr < 0) {
    mx_printf("mx_hires_init: no EE timer free\n");
    return -1;
  }
  tmr_base = mx_timer_base(tmr);
  TMR_MODE = 0;
  h.next_line = h.npass;
  TMR_COUNT = 0;
  TMR_COMP = 0xFFFF;
  mx_irq_intc(MX_INTC_TIM0 + tmr, timer_irq, 0);
  TMR_MODE = (u32)(MX_SET(TIMER_MODE_CLKS, 3) | MX_BIT(TIMER_MODE_CUE) | MX_BIT(TIMER_MODE_CMPE) |
                  MX_BIT(TIMER_MODE_EQUF) | MX_BIT(TIMER_MODE_OVFF));
  int was = mx_di();
  mx__vsync_hook = vsync_hook;
  mx_irq_restore(was);
  return 0;
}

void mx__gs_event_wait(void);
void mx__gs_event(void);

/* The layout of the record begun now: the width asked for (mx_hires_width),
   by way of L_END from L_FRONT; the spare room given to GS memory once a
   record of L_FRONT is done (no record of another layout is played or
   shown since), and taken back before L_END. */
static int choose_layout(void) {
  int half = want_w && want_w < h.rt.w;
  if (half) {
    if (cur_l == L_FRONT && !lent && front_first != ~0u && mx_gs.frames_done > front_first) {
      mx_vram_free(spare_fb, spare_fb_n);
      mx_vram_free(spare_z, spare_z_n);
      lent = 1;
    }
    return L_FRONT;
  }
  if (cur_l != L_FRONT)
    return L_FULL;
  if (lent) {
    /* both asked each time: each stops the cache placing more there */
    int a = mx_vcache_release(spare_fb, spare_fb_n, MX_VRAM_FIXED);
    int b = mx_vcache_release(spare_z, spare_z_n, MX_VRAM_FIXED);
    if (a < 0 || b < 0)
      return L_FRONT;
    lent = 0;
  }
  front_first = ~0u;
  return L_END;
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

/* paused: the restart at the next frame frees every record */
int mx__hires_ready(void) { return paused || h.building >= 0 || slot_free() >= 0; }

u8 *mx__hires_begin(u32 *qw, u8 **once, u32 *once_qw) {
  if (paused) {
    paused = 0;
    pipeline_start();
  }
  /* the display on once a record was played a whole field */
  if (h.hidden && h.first_field && mx_hires_stats.fields > h.first_field) {
    for (int i = 0; i < 2; i++)
      for (int c = 0; c < 2; c++)
        h.disp[i].c[c].on = h.disp_on[i][c];
    h.hidden = 0;
    mx_display_apply(&h.disp[0]);
  }
  if (h.building < 0) {
    u32 t0 = mx_count();
    int s;
    int reported = 0;
    /* No more records waiting than may (mx_hires_ahead): asleep a part of a
       millisecond at a time, and for a tenth of a second at most (a pipeline that
       plays nothing now, stopped or held, takes no record: the frame goes on) */
    while (h.nwait >= cfg_ahead && mx_count() - t0 < MX_EE_HZ / 10)
      mx_sleep_us(250);
    while ((s = slot_free()) < 0) {
      /* asleep until a pass ends or a field begins (a record is free once
         the pass 0 that replaced it is past and its last pass's DMA done) */
      mx__gs_event_wait();
      if (!reported && mx_count() - t0 > MX_EE_HZ) {
        mx_printf("mx_hires: no free record for 1 s (played %d, waiting %u, fields %u)\n",
                  h.played, h.nwait, mx_hires_stats.fields);
        reported = 1;
      }
    }
    h.slot[s].state = BUILDING;
    h.slot[s].rgb = h.rgb;
    h.slot[s].layout = L_FULL;
    h.building = s;
    if (widths) {
      int l = choose_layout();
      h.slot[s].layout = l;
      front_new = l == L_FRONT && cur_l != L_FRONT;
      cur_l = l;
      mx_gs.w = lay[l].w, mx_gs.fbw = lay[l].fbw;
      mx_gs.fbp[0] = mx_gs.fbp[1] = lay[l].fbp;
    }
  }
  *qw = h.qw_per_slot;
  *once = h.slot[h.building].once;
  *once_qw = h.once_qw;
  return h.slot[h.building].mem;
}

u32 mx_hires_strip_records(void *mem, u32 bytes) {
  u32 qw = bytes / (MX_HIRES_SLOTS * h.npass) / 16;
  if (!h.npass || qw < 8)
    return 0;
  mx_dcache_invalidate(mem, qw * 16 * MX_HIRES_SLOTS * h.npass);
  int was = mx_di();
  for (int i = 0; i < MX_HIRES_SLOTS; i++)
    for (u32 k = 0; k < h.npass; k++) {
      h.slot[i].strip[k] = (u8 *)mem + (i * h.npass + k) * qw * 16;
      /* empty until a frame writes it: a slot may be played before */
      u64 *t = (u64 *)MX_UNCACHED(h.slot[i].strip[k]);
      t[0] = mx_dmatag(MX_TAG_RET, 0, MX_PADDR_NONE, 0);
      t[1] = 0;
    }
  mx_sync();
  h.strip_qw = qw;
  mx_irq_restore(was);
  return qw * 16;
}

u32 mx_hires_strip_record_bytes(void) { return h.strip_qw * 16; }

u32 mx__hires_strips(u8 **at, u32 *qw) {
  if (!h.strip_qw || h.building < 0)
    return 0;
  for (u32 k = 0; k < h.npass; k++)
    at[k] = h.slot[h.building].strip[k];
  *qw = h.strip_qw;
  return h.npass;
}

void mx__hires_strips_end(u8 *const *end) {
  int s = h.building;
  for (u32 k = 0; k < h.npass; k++) {
    u32 used = (u32)(end[k] - h.slot[s].strip[k]) + 16;
    if (used > mx_hires_stats.strip_max)
      mx_hires_stats.strip_max = used;
  }
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
  if (widths && front_new)
    front_first = mx_gs.frames, front_new = 0;
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
  tmr_off();
  /* the pass being sent and a once record over PATH3 finish: the pipeline
     can be started again (another mode) */
  mx_dmaq_release(&h.q);
  mx_dma_wait(MX_CH_GIF);
}

/* Another process is to have the graphics (mx_proc_spawn, mx_proc_front,
   the home button), or had them (mx__frame_lost): the pipeline stopped, the
   passes queued sent first (mx__hires_stop), and started again at the next
   frame (mx OS plan step 6, part 4: the drawing hardware's interrupts are
   the process in front's only, so a pipeline left running beside would
   wait for a pass's end that never comes). Frames being built are dropped:
   the next frame draws all again. */
void mx__hires_pause(void) {
  if (paused || tmr < 0)
    return;
  mx__hires_stop();
  paused = 1;
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
  return h.rt.psm == MX_GS_PSM_CT24 ? h.pages : 0;
}

/* a page's blocks by their row and column (GS manual 8.3: block configuration in a page) */
static const u8 blocks_16s[8][4] = {{0, 2, 16, 18},  {1, 3, 17, 19},  {8, 10, 24, 26}, {9, 11, 25, 27},
                                    {4, 6, 20, 22},  {5, 7, 21, 23},  {12, 14, 28, 30}, {13, 15, 29, 31}};
static const u8 blocks_16[8][4] = {{0, 2, 8, 10},    {1, 3, 9, 11},   {4, 6, 12, 14},  {5, 7, 13, 15},
                                   {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const u8 blocks_32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21},
                                   {2, 3, 6, 7, 18, 19, 22, 23},
                                   {8, 9, 12, 13, 24, 25, 28, 29},
                                   {10, 11, 14, 15, 26, 27, 30, 31}};

u32 mx_hires_spare(u32 *at, u32 n) {
  if (!own_z || !h.npass)
    return 0;
  int b16 = h.rt.psm == MX_GS_PSM_CT16 || h.rt.psm == MX_GS_PSM_CT16S;
  u32 rows = b16 ? 8 : 4, cols = b16 ? 4 : 8; /* blocks of 8 lines */
  u32 used = h.rt.h % h.page_h; /* the last row's lines that are shown */
  if (!used)
    return 0;
  u32 first_free = (used + 7) / 8;
  /* the blocks below the picture */
  u32 free = 0;
  for (u32 r = first_free; r < rows; r++)
    for (u32 c = 0; c < cols; c++)
      free |= 1u << (h.rt.psm == MX_GS_PSM_CT16S ? blocks_16s[r][c] : b16 ? blocks_16[r][c] : blocks_32[r][c]);
  u32 row = h.rt.h / h.page_h, k = 0;
  /* at half the width too: the last row of L_END, the part of L_FULL's
     that is the same pages */
  u32 p0 = widths ? lay[L_END].fbp + row * lay[L_END].fbw : h.rt.fbp + row * h.rt.fbw;
  u32 np = widths ? lay[L_END].fbw : h.rt.fbw;
  for (u32 p = 0; p < np; p++)
    for (u32 g = 0; g < 32; g += 4)
      if ((free >> g & 15) == 15 && k < n)
        at[k++] = (p0 + p) * 32 + g;
  return k;
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
  tmr_off();
  mx_dmaq_drain(&h.q);
  int s = h.played;
  if (s < 0)
    return -1;
  int both = mx_video.interlace && mx_video.ffmd && h.shift_field >= 0;
  int twice = mx_video.interlace && mx_video.ffmd;
  u32 lfbp = h.rt.fbp, lfbw = h.rt.fbw, lw = h.rt.w, lsp = h.strip_pages;
  if (widths) {
    int l = h.slot[s].layout;
    lfbp = lay[l].fbp, lfbw = lay[l].fbw, lw = lay[l].w, lsp = lay[l].strip_pages;
  }
  u32 w = lw > 2048 ? 2048 : lw, hh = h.rt.h, rows = h.strip_h;
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
        if (mx_gs_download(shot_line, MX_GS_PAGE(lfbp + k * lsp), lfbw,
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
