#ifndef MX_TIMER_H
#define MX_TIMER_H

/* Time: COP0 Count for short intervals (EE clock), and the EE timers for
   periodic interrupts. Use 0 and 1: the BIOS uses timer 3 for alarms, and
   maps the page of timers 2 and 3 (0x10001000) read-only, so a user-mode
   write to them raises a TLB modified exception (docs/vm.md). */

#include <mx/irq.h>
#include <mx/types.h>

#define MX_EE_HZ 294912000u
#define MX_BUS_HZ 147456000u

/* Busy-wait. */
void mx_delay_cycles(u32 cycles);
MX_INLINE void mx_delay_us(u32 us) { mx_delay_cycles(us * (MX_EE_HZ / 1000000u)); }

/* Call fn from the interrupt of timer t (0..1) every period bus clocks
   divided by 256 (period < 65536, i.e. up to ~113 ms). fn = 0 stops it. */
void mx_timer_periodic(int t, u32 period, mx_irq_fn fn, void *arg);

#endif
