/* The home button brings the shell in front (kernel version 25, mx OS plan
   step 4d). This program is the shell (started by the kernel as its shell)
   and, with the argument "game", the game: the shell starts itself as the
   game (mx_proc_spawn: it runs beside), the game draws frames and presses
   the home button twice (a test kernel's "@@PAD" line: the kernel holds the
   pad's buttons for that long). At the first press the shell is in front
   with an MX_EVENT_HOME for the game, which is paused, and brings it back
   (mx_proc_front: the shell paused until the next press); at the second it
   ends it (mx_proc_kill of a paused process), and gets its end (status 9)
   as an event and from mx_proc_wait.
     as the kernel's shell: MXSHELL.ELF [poll]     (status 0 expected; poll: PCSX2) */
#include <mx/frame.h>
#include <mx/gs.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/msg.h>
#include <mx/pad.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/video.h>
#include "t.h"

MX_CORE_HEAP(512 << 10);

/* the DMAC reads it: core memory */
static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024) MX_CORE_DATA;

static u64 *box(u64 *c, u32 x0, u32 y0, u32 x1, u32 y1, u32 rgb) {
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x0), mx_gs_py(y0), 0));
  return mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x1), mx_gs_py(y1), 0));
}

/* the game: frames until it is ended; the home button at frames 300 and 600 (poll: steps 60 and
   180). poll: no frames (PCSX2's frame pipeline stalls under the kernel), the kernel's home calls
   made as the frame pipeline makes them at a frame boundary (mx__home_poll), each 20 ms */
static int game(int poll) {
  int pal = mx_video_default() == &mx_video_pal;
  u32 h = pal ? 512 : 448;
  mx_gs_init_mode(640, h, pal ? MX_GS_PAL : 0);
  if (!poll)
    mx_frame_init(ring, sizeof ring, (void *)0x70000000, 0x2000);
  /* poll: the pad read through the kernel without mx_pad_init (its modules
     change PCSX2's IOP: the kernel there restarts at the end) */
  if (!poll && mx_pad_init() != 0)
    mx_printf("k50 game: no pad\n");
  if (poll)
    for (u32 n = 0;; n++) {
      if (n == 60 || n == 180)
        mx_printf("@@PAD %x 300\n", MX_PAD_HOME);
      if (n % 60 == 0)
        mx_printf("k50 game: step %u\n", n);
      struct mx_pad_state ps;
      (void)mx__kernel->pad_read(0, &ps, 0);
      u32 pages = mx__kernel->home_wanted();
      if (pages)
        mx__kernel->home_run(~0u, pages);
      mx_sleep_us(20000);
    }
  /* frames: about 5 s a step, to be seen on a TV */
  for (u32 n = 0;; n++) {
    if (n == 300 || n == 600)
      mx_printf("@@PAD %x 300\n", MX_PAD_HOME);
    if (n % 60 == 0)
      mx_printf("k50 game: frame %u\n", n);
    (void)mx_pad_buttons(0);
    mx_frame_begin(n & 0x7F, 0x20, 0x60);
    u32 x = 100 + n % 400;
    mx_frame_commit(box(mx_frame_direct(5), x, h / 2, x + 40, h / 2 + 40, 0xFFFFFF));
    mx_frame_present();
    mx_frame_end();
  }
}

static void event(int port, u32 type, int id, int status) {
  struct mx_event e;
  struct mx_msg m;
  memset(&e, 0, sizeof e);
  int r = mx_msg_receive(port, &e, sizeof e, &m);
  mx_printf("k50: event %d: type %u, process %d, status %d (from %d)\n", r, e.type, e.proc,
            e.status, m.from);
  CHECK(r > 0);
  CHECK_EQ(m.from, 0);
  CHECK_EQ(e.type, type);
  CHECK_EQ(e.proc, id);
  CHECK_EQ(e.status, status);
}

int main(int argc, char **argv) {
  if (argc > 1 && !strncmp(argv[1], "game", 4)) {
    if (mx__kernel->proc_shell() != 0)
      mx_printf("k50 game: FAILED: proc_shell says it is the shell\n");
    return game(!strcmp(argv[1], "gamepoll"));
  }
  CHECK(mx__kernel && mx__kernel->version >= 25);
  CHECK_EQ(mx__kernel->proc_shell(), 1); /* started as the kernel's shell */
  /* a mode of its own, not the game's (the default's): at each switch the
     kernel sets the mode of the one in front again (PCSX2's log shows it) */
  int pal = mx_video_default() == &mx_video_pal;
  mx_gs_init_mode(640, pal ? 448 : 512, pal ? 0 : MX_GS_PAL);
  mx_printf("k50: the shell's mode: %s\n", pal ? "NTSC" : "PAL");
  int port = mx_port_create("k50");
  CHECK(port > 0);
  CHECK_EQ(mx_proc_events(port), 0);
  static char args[256];
  u32 n = (u32)strlen(argv[0]) + 1;
  memcpy(args, argv[0], n);
  const char *role = argc > 1 && !strcmp(argv[1], "poll") ? "gamepoll" : "game";
  memcpy(args + n, role, strlen(role) + 1);
  int id = mx_proc_spawn(2, args, n + (u32)strlen(role) + 1, 0);
  mx_printf("k50: the game is process %d\n", id);
  CHECK(id > 0);
  /* kernel version 26: the roles in the list, the shell's only for it */
  if (mx__kernel->version >= 26) {
    struct mx_proc_info p[8];
    int n = mx_proc_list(p, 8), seen = 0;
    for (int i = 0; i < n && i < 8; i++) {
      if (p[i].id == mx_proc_self()) {
        CHECK_EQ(p[i].flags & MX_PROC_SHELL, MX_PROC_SHELL);
        seen++;
      }
      if (p[i].id == id) {
        CHECK_EQ(p[i].flags & MX_PROC_SHELL, 0);
        seen++;
      }
    }
    CHECK_EQ(seen, 2);
  }

  /* frames: 5 s in front at each press, to be seen on a TV */
  u32 stay = strcmp(role, "game") ? 0 : 5000000;
  event(port, MX_EVENT_HOME, id, 0); /* the first press: in front, the game paused */
  int st = -1;
  CHECK_EQ(mx_proc_wait(id, &st, 1), 0);
  mx_printf("k50: in front at the home button; the game back\n");
  if (stay)
    mx_sleep_us(stay);
  CHECK_EQ(mx_proc_front(id), 0); /* back here at the second press */
  event(port, MX_EVENT_HOME, id, 0);
  mx_printf("k50: in front again; the game ended\n");
  if (stay)
    mx_sleep_us(stay);
  CHECK_EQ(mx_proc_kill(id, 9), 0);
  event(port, MX_EVENT_END, id, 9);
  CHECK_EQ(mx_proc_wait(id, &st, 0), 1);
  CHECK_EQ(st, 9);
  return t_done("k50");
}
