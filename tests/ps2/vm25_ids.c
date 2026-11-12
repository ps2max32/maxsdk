/* probe/p34_ids.c in VM, under mx-kernel: after programs that ended without
   the restart, the BIOS has no threads or semaphores of theirs left. */
/* maxsdk's own test: the kernel's thread and semaphore calls under their BIOS
   names (libmxk's src/base/ksys.c), no BIOS call (the link's check) */
#define MX_BIOS_CALLS 1
#include "../../probe/p34_ids.c"
