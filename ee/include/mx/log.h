#ifndef MX_LOG_H
#define MX_LOG_H

/* The kernel's log (kernel version 16; docs/kernel.md, docs/mx-os-apps-plan.md
   step 2e-3): what every process writes with mx_printf (its tty) and the
   kernel's own output, each piece with its process (0: the kernel), in a
   ring of 8 KiB. Any process reads it, each from its own position. libmxk
   only: libmx's call returns -1. */

#include <mx/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The next piece after *pos (start with 0: the oldest kept), at most max
   bytes of it into buf, its process in *proc (0: not wanted): its length
   (cut to max), *pos moved past it. 0: nothing new (with wait, the thread
   waits for it instead). A reader that fell behind by more than the ring
   goes on from the oldest piece kept. Pieces are at most 64 bytes: a line
   may come in several, and lines of processes may be mixed. */
int mx_log_read(u32 *pos, void *buf, u32 max, int *proc, int wait);

#ifdef __cplusplus
}
#endif

#endif
