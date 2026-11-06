#ifndef MX_PROF_H
#define MX_PROF_H

/* A PC-sampling profiler: timer 1's interrupt takes the interrupted PC
   (the BIOS handler's address argument) hz times a second and counts it in 16-byte buckets. The
   report prints the buckets with the most samples ("prof: <pc> <n>"), for
   tools/prof.py to turn into functions with the ELF. Timer 1, as
   mx_trace_sample: not both. */

#include <mx/types.h>

void mx_prof_start(u32 hz);
void mx_prof_stop(void);
/* the top buckets (at most top) and the total, to the debug output */
void mx_prof_report(u32 top);

#endif
