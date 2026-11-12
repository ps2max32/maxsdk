/* frame.c (the render pipeline) on libmxk without VM, under the kernel: the
   frame pipeline as a program of the kernel's has it (the shell, games).
   Started by mxlink:  k40_frame.elf     (status 0 expected) */
#include <mx/loader.h>
/* a core run of its own, as the shell's (not all memory): it starts beside a
   service with VM too (mx OS plan 3a) */
MX_CORE_HEAP(2 << 20);
#include "frame.c"
