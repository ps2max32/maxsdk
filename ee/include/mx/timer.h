#ifndef MX_TIMER_H
#define MX_TIMER_H

/* Time: COP0 Count for short intervals (EE clock), and timers.

   Timers are a service (src/irq/timer.c): software timers, any number up to
   a table of slots, on one EE timer that the program never sees; and the
   other EE timers lent out whole for what needs a clock of its own (mx_hires
   counts lines). Under mx-kernel (libmxk, kernel version 19) the kernel's,
   for all processes; in a program of its own its own. Nobody picks a timer
   by its number. The BIOS uses timer 3 for alarms, and maps the page of
   timers 2 and 3 (0x10001000) read-only, so a user-mode write to them
   raises a TLB modified exception (docs/vm.md). */

#include <mx/irq.h>
#include <mx/types.h>

#define MX_EE_HZ 294912000u
#define MX_BUS_HZ 147456000u

/* Busy-wait: the CPU stays in it. To wait without, mx_sleep_us. */
void mx_delay_cycles(u32 cycles);
MX_INLINE void mx_delay_us(u32 us) { mx_delay_cycles(us * (MX_EE_HZ / 1000000u)); }

/* A software timer: fn(arg) from an interrupt after us microseconds, and
   then every period_us (0: once), each interval at most 4 s; fn and arg in
   core (MX_CORE_FN, MX_CORE_DATA), fn short (interrupts are off). Its id
   (> 0), -1 when the table is full. A timer once is gone after its call;
   mx_timer_remove stops one. A process's end takes its timers. Accuracy:
   about 2 us, later while interrupts are off. */
int mx_timer_add(u32 us, u32 period_us, mx_irq_fn fn, void *arg);
void mx_timer_remove(int id);
/* In a timer's fn: where the interrupted code was (the profiler). */
u32 mx_timer_epc(void);

/* An EE timer of its own, for a clock the software timers do not have:
   its number (0 or 1: registers at mx_timer_base(t), interrupt cause
   MX_INTC_TIM0 + t), -1 when none is free. The caller sets its clock
   (clock: what it is for, MX_TIMER_HBLANK or MX_TIMER_BUS) and its
   interrupt (mx_irq_intc); mx_timer_release gives it back, and so does
   the process's end. */
enum { MX_TIMER_BUS, MX_TIMER_HBLANK };
int mx_timer_claim(int clock);
void mx_timer_release(int t);
u32 mx_timer_base(int t);

/* The old call: fn every period bus clocks / 256 (up to ~113 ms), now a
   software timer (t only tells two of them apart). fn = 0 stops it. */
void mx_timer_periodic(int t, u32 period, mx_irq_fn fn, void *arg);

#endif
