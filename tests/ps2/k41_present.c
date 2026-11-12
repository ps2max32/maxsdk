/* fpresent.c (mx_frame_present with vsync) on libmxk without VM, under the
   kernel. Started by mxlink:  k41_present.elf     (status 0 expected) */
#include <mx/loader.h>
/* a core run of its own, as the shell's (not all memory): it starts beside a
   service with VM too (mx OS plan 3a) */
MX_CORE_HEAP(2 << 20);
#include "fpresent.c"
