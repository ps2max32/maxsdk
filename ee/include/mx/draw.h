#ifndef MX_DRAW_H
#define MX_DRAW_H

/* The drawing hardware under mx-kernel (kernel version 39, mx OS plan step
   6, part 4): the GS, the GIF, VIF0/1 and the VUs, the IPU, the scratchpad
   and the timer beam racing claims are the process in front's. Only the
   process in front gets their interrupts (its mx_irq_intc and mx_irq_dmac
   handlers, which take only these: the GS, the vblank's start and end,
   VIF0/1, VU0/1, the IPU and timer 0; the channels of VIF0/1, the GIF, the
   IPU and the scratchpad). Another process's handlers are not called while
   it is not in front.

   A program hands the graphics over at a frame boundary (mx_proc_spawn,
   mx_proc_front, the home button): what it sent is drawn first, beam
   racing's pipeline paused. When it is in front again, maxsdk takes the
   graphics up at its next frame: the frame pipeline, the DMA queues, the
   texture cache, the VU programs, the HUD's font, beam racing's pipeline.
   What the program itself keeps in GS or VU memory or in the scratchpad
   over frames is gone then (the other process drew there): a session's
   lost function is called, in the program's thread, at that frame's
   start, for it to send or draw that again. A program that draws all
   again each frame needs no session: libgl (libs/libgl) needs none, it
   sends its GS state and CLUTs each frame and its textures go through the
   texture cache; a texture it drew into (glCopyTexSubImage2D) is gone, and
   the program draws it again as for its next frame. */

#include <mx/types.h>

struct mx_draw_ops {
  /* the graphics were another process's since this program's last frame:
     what it keeps in GS memory, VU memory or the scratchpad is gone */
  void (*lost)(void *arg);
  void *arg;
};

/* A session: ops are called as said above (ops must stay valid until
   mx_draw_close). Its id (>= 0), -1 when 4 are open. */
int mx_draw_open(const struct mx_draw_ops *ops);
void mx_draw_close(int id);

#endif
