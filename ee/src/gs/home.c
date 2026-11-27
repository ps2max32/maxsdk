/* The kernel's home menu (mx-kernel 7, docs/mx-os-plan.md step 1): when the
   kernel asks, the program stops at its next frame boundary
   (mx_frame_begin_n) with the GS idle, lends the kernel GS memory and waits
   until the menu is closed. The memory: free pages, else the texture
   cache's (dropped: the GS reads none of it between frames, and the cache
   fills again). Not in hires mode (mx_frame_sync refuses): the kernel then
   lets the pad's combination end the program. */

#include <mx/frame.h>
#include <mx/gs.h>
#include <mx/kernel.h>
#include <mx/mem.h>
#include <mx/mx.h>
#include <mx/screen.h>
#include <mx/vcache.h>
#include <mx/video.h>

/* the cache and the sound only if the program has them */
extern void mx_vcache_reset(void) __attribute__((weak));
extern void mx__snd_quiet(void) __attribute__((weak));

static u32 lend(u32 pages) {
  u32 p = mx_vram_find(pages);
  if (p == ~0u && mx_vcache_reset) {
    mx_vcache_reset();
    p = mx_vram_find(pages);
  }
  if (p == ~0u || mx_vram_take(p, pages, MX_VRAM_KERNEL) != 0)
    return ~0u;
  return p;
}

/* Kernel version 32: the picture on the screen for the shell (<mx/screen.h>'s
   mx_home_picture), read back from GS memory into pinned memory of this
   program's and named to the kernel, until this program goes on. The buffer
   mx_gs's display shows (DISPFB2's page); none in another set-up. The GS is
   idle (mx_frame_sync). The buffer (mx_dma_free), or 0. */
void *mx__home_capture(struct mx_picture *out) {
  const struct mx_rt *t = 0;
  for (int i = 0; i < 2; i++)
    if (mx_gs.rt[i].w && mx_gs.rt[i].fbp == mx_gs.shown_fbp)
      t = &mx_gs.rt[i];
  if (!t)
    return 0;
  u32 bpp = t->psm == MX_GS_PSM_CT32 ? 4 : t->psm == MX_GS_PSM_CT24 ? 3 :
            t->psm == MX_GS_PSM_CT16 || t->psm == MX_GS_PSM_CT16S ? 2 : 0;
  struct mx_picture p = {t->w, t->h, t->psm, 0, mx_video_line_lines(&mx_video), 0};
  if (!bpp || !p.w)
    return 0;
  /* a transfer is whole 128 bytes and at most 65535 quadwords (the DMA's
     count): in strips of whole lines, of about 256 KiB; a few lines less if
     the height is not whole strips' steps */
  u32 line = p.w * bpp, step = 1;
  while ((line * step) % 128)
    step++;
  p.h -= p.h % step;
  u32 rows = (256u << 10) / line / step * step;
  if (!rows)
    rows = step;
  p.bytes = line * p.h;
  if (!p.bytes || line * rows > 0xFFFFu * 16)
    return 0;
  u8 *buf = mx_dma_alloc(p.bytes);
  if (!buf)
    return 0;
  for (u32 y = 0; y < p.h; y += rows) {
    u32 n = p.h - y < rows ? p.h - y : rows;
    if (mx_gs_download(buf + y * line, MX_GS_PAGE(t->fbp), t->fbw, t->psm, 0, y, p.w, n) < 0) {
      mx_dma_free(buf);
      return 0;
    }
  }
  *out = p;
  return buf;
}

/* Kernel version 34: another process had the graphics since this one's last
   frame, or they were reset (the one in front ended): taken up again */
void mx__frame_lost(void);
static u32 gen_seen;
static int gen_known;
static void lost_check(void) {
  if (mx__kernel->version < 34 ||
      mx__kernel->size < __builtin_offsetof(struct mxk_services, gfx_gen) + sizeof(void *))
    return;
  u32 g = mx__kernel->gfx_gen();
  if (gen_known && g != gen_seen)
    mx__frame_lost();
  gen_seen = g, gen_known = 1;
}

void mx__home_poll(void) {
  if (!mx__kernel || mx__kernel->version < 7)
    return;
  /* kernel version 33: the GS is the process in front's; another's frames
     wait here until it is in front */
  if (mx__kernel->version >= 33 &&
      mx__kernel->size >= __builtin_offsetof(struct mxk_services, front_wait) + sizeof(void *))
    mx__kernel->front_wait();
  lost_check();
  u32 pages = mx__kernel->home_wanted();
  if (!pages)
    return;
  /* what was sent drawn, in hires mode the passes' pipeline paused (it
     starts again at the next frame), the sound silenced: another process
     may have the graphics and the sound now, and this one's interrupts of
     them stop meanwhile */
  int mx__frame_yield(void);
  int dropped = mx__frame_yield();
  if (mx__snd_quiet)
    mx__snd_quiet();
  int synced = mx_frame_sync() >= 0;
  struct mx_picture p;
  void *pic = 0;
  if (synced && mx__kernel->version >= 32 &&
      mx__kernel->size >= __builtin_offsetof(struct mxk_services, home_picture_read) + sizeof(void *) &&
      (pic = mx__home_capture(&p)) != 0)
    mx__kernel->home_picture_set(&p, mx_paddr_of(pic).v);
  u32 page = synced ? lend(pages) : ~0u;
  int end = mx__kernel->home_run(page, pages);
  if (pic) {
    mx__kernel->home_picture_set(0, 0);
    mx_dma_free(pic);
  }
  /* the shell was in front meanwhile; or, in hires mode, this program's
     records were dropped by the pause (with the textures they were to
     upload: the cache must not count them as there), also when nothing
     else had the graphics (no menu: a host's program without the shell) */
  if (!end && dropped) {
    mx__frame_lost();
    gen_known = 0;
    lost_check();
  } else if (!end)
    lost_check();
  if (page != ~0u)
    mx_vram_give(page, pages);
  if (end)
    mx_exit(0);
}
