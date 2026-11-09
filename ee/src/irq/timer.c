/* Timers (<mx/timer.h>): software timers on one EE timer, and the other
   EE timers lent out whole (mx_timer_claim).

   A program of its own (libmx) has them here, on timer 0; the kernel
   (apps/mxos: its maxsdk built with MX_SOFT_TIMER=1, timer 1) has them for
   all processes, and libmxk's calls go to it (kernel version 19). Timers 2
   and 3 are the BIOS's page, read-only to programs.

   The software timers are a table of slots, each due at a COP0 Count: a
   callback (once or every period) or a sleeping thread (mx_sleep_us). The
   EE timer is set, one-shot, to the earliest; its interrupt runs what is
   due and sets it again. Under the kernel a callback runs in its process's
   address space (mx__timer_enter, kproc.c), and a process's end takes its
   slots and its claims (mx__timer_reap). */

#include <mx/bios.h>
#include <mx/bits.h>
#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/hw/formats.h>
#include <mx/hw/regs.h>
#include <mx/irq.h>
#include <mx/kernel.h>
#include <mx/thread.h>
#include <mx/timer.h>

void mx_delay_cycles(u32 cycles) {
  u32 t0 = mx_count();
  while (mx_count() - t0 < cycles)
    ;
}

static const u32 tbase[2] = {MX_TIMER_T0, MX_TIMER_T1};

u32 mx_timer_base(int t) { return t >= 0 && t < 2 ? tbase[t] : 0; }


#ifdef MX_KERNEL
/* ---- libmxk: the kernel's (version 19) ------------------------------------ */

static int has19(void) {
  return mx__kernel && mx__kernel->version >= 19 &&
         mx__kernel->size >= __builtin_offsetof(struct mxk_services, timer_epc) + sizeof(void *);
}

int mx_timer_add(u32 us, u32 period_us, mx_irq_fn fn, void *arg) {
  if (mx__need_core((const void *)fn, "mx_timer_add: the handler") ||
      mx__need_core(arg, "mx_timer_add: the handler's argument") || !fn)
    return -1;
  return has19() ? mx__kernel->timer_add(us, period_us, fn, arg) : -1;
}
void mx_timer_remove(int id) {
  if (has19())
    mx__kernel->timer_remove(id);
}
/* before version 19 timer 0 was the program's */
int mx_timer_claim(int clock) { return has19() ? mx__kernel->timer_claim(clock) : 0; }
void mx_timer_release(int t) {
  if (has19())
    mx__kernel->timer_release(t);
}
u32 mx_timer_epc(void) {
  extern volatile u32 mx__irq_epc;
  return has19() ? mx__kernel->timer_epc() : mx__irq_epc;
}
void mx__timer_stop(void) {}

#else
/* ---- the timers themselves: a program of its own, or the kernel ---------- */

#ifndef MX_SOFT_TIMER
#define MX_SOFT_TIMER 0
#endif
#define SOFT_BASE (MX_SOFT_TIMER ? MX_TIMER_T1 : MX_TIMER_T0)
#define SOFT_CAUSE (MX_SOFT_TIMER ? MX_INTC_TIM1 : MX_INTC_TIM0)

static u32 us_cycles(u32 us) { return (u32)((u64)us * MX_EE_HZ / 1000000u); }

#define SLOTS 24
/* the timer counts the bus clock / 256: one count is 512 EE cycles (1.74 us) */
#define CYCLES_PER_COUNT (MX_EE_HZ / (MX_BUS_HZ / 256u))
/* one wait at most this long (Count wraps in 14.5 s) */
#define MAX_US 4000000u

enum { FREE, ALARM, SLEEP };

static struct {
  u8 kind, gen, proc, armed;
  u32 due;    /* COP0 Count */
  u32 period; /* cycles, 0: once */
  mx_irq_fn fn;
  void *arg;
  int sema; /* a sleeper's + 1, 0: not made yet */
} slot[SLOTS];
static int handler_on;
static struct {
  u8 used, proc;
} claim[2];

/* the kernel's (kproc.c): the caller's process, a handler of another
   process's run in its address space */
int mx__timer_proc(void) __attribute__((weak));
int mx__timer_enter(int proc) __attribute__((weak));
void mx__timer_leave(void) __attribute__((weak));
static int proc_now(void) { return mx__timer_proc ? mx__timer_proc() : 0; }

static void soft_stop(void) { MX_R32(SOFT_BASE + MX_TIMER_MODE_OFFSET) = 0; }

/* interrupts off: the timer to the earliest end, or off */
static void arm(void) {
  u32 now = mx_count(), wait = 0;
  int any = 0;
  for (int i = 0; i < SLOTS; i++) {
    if (!slot[i].armed)
      continue;
    s32 left = (s32)(slot[i].due - now);
    u32 w = left > 0 ? (u32)left : 0;
    if (!any || w < wait)
      wait = w, any = 1;
  }
  soft_stop();
  if (!any)
    return;
  u32 counts = wait / CYCLES_PER_COUNT + 1;
  if (counts > 0xFFFF)
    counts = 0xFFFF; /* about 113 ms: the interrupt then sets it again */
  MX_R32(SOFT_BASE + MX_TIMER_COUNT_OFFSET) = 0;
  MX_R32(SOFT_BASE + MX_TIMER_COMP_OFFSET) = counts;
  MX_R32(SOFT_BASE + MX_TIMER_MODE_OFFSET) =
      (u32)(MX_SET(TIMER_MODE_CLKS, 2) | MX_BIT(TIMER_MODE_CUE) | MX_BIT(TIMER_MODE_CMPE) |
            MX_BIT(TIMER_MODE_EQUF) | MX_BIT(TIMER_MODE_OVFF));
}

static void tick(void *arg) {
  (void)arg;
  u32 now = mx_count();
  for (int i = 0; i < SLOTS; i++) {
    if (!slot[i].armed || (s32)(now - slot[i].due) < 0)
      continue;
    if (slot[i].kind == SLEEP) {
      slot[i].armed = 0;
      mx_isema_signal(slot[i].sema - 1);
      continue;
    }
    mx_irq_fn fn = slot[i].fn;
    void *a = slot[i].arg;
    if (slot[i].period) {
      slot[i].due += slot[i].period;
      if ((s32)(now - slot[i].due) >= 0) /* behind: from now on */
        slot[i].due = now + slot[i].period;
    } else {
      slot[i].armed = 0, slot[i].kind = FREE, slot[i].gen++;
    }
    int sw = mx__timer_enter ? mx__timer_enter(slot[i].proc) : 0;
    fn(a);
    if (sw)
      mx__timer_leave();
  }
  arm(); /* also acknowledges: the mode is written with EQUF set */
}

static void handler(void) {
  if (!handler_on) {
    handler_on = 1;
    soft_stop();
    mx_irq_intc(SOFT_CAUSE, tick, 0);
  }
}

/* interrupts off: a free slot, or -1 */
static int take(int kind) {
  for (int i = 0; i < SLOTS; i++)
    if (slot[i].kind == FREE) {
      slot[i].kind = (u8)kind, slot[i].armed = 0, slot[i].proc = (u8)proc_now();
      return i;
    }
  return -1;
}

int mx_timer_add(u32 us, u32 period_us, mx_irq_fn fn, void *arg) {
  if (!fn || mx__need_core((const void *)fn, "mx_timer_add: the handler") ||
      mx__need_core(arg, "mx_timer_add: the handler's argument"))
    return -1;
  if (us > MAX_US)
    us = MAX_US;
  if (period_us > MAX_US)
    period_us = MAX_US;
  handler();
  int was = mx_di();
  int i = take(ALARM);
  if (i >= 0) {
    slot[i].fn = fn, slot[i].arg = arg;
    slot[i].period = period_us ? us_cycles(period_us) : 0;
    slot[i].due = mx_count() + us_cycles(us ? us : 1);
    slot[i].armed = 1;
    arm();
  }
  mx_irq_restore(was);
  return i < 0 ? -1 : i + 1 + (slot[i].gen << 8);
}

void mx_timer_remove(int id) {
  int i = (id & 0xFF) - 1;
  if (id <= 0 || i >= SLOTS)
    return;
  int was = mx_di();
  if (slot[i].kind == ALARM && slot[i].gen == (u8)(id >> 8) &&
      (!mx__timer_proc || slot[i].proc == proc_now())) {
    slot[i].kind = FREE, slot[i].armed = 0, slot[i].gen++;
    arm();
  }
  mx_irq_restore(was);
}

/* mx_sleep_us (src/thread/sleep.c), not under libmxk */
void mx__timer_sleep(u32 us) {
  handler();
  int was = mx_di();
  int i = take(SLEEP);
  mx_irq_restore(was);
  if (i >= 0 && !slot[i].sema) {
    int s = mx_sema_create(0, 1);
    if (s >= 0)
      slot[i].sema = s + 1;
  }
  if (i < 0 || !slot[i].sema) {
    /* more timers than slots, or no semaphore: wait without sleeping */
    if (i >= 0)
      slot[i].kind = FREE;
    mx_delay_us(us);
    return;
  }
  while (us) {
    u32 n = us > MAX_US ? MAX_US : us;
    was = mx_di();
    slot[i].due = mx_count() + us_cycles(n);
    slot[i].armed = 1;
    arm();
    mx_irq_restore(was);
    mx_sema_wait(slot[i].sema - 1);
    us -= n;
  }
  slot[i].kind = FREE;
}

int mx_timer_claim(int clock) {
  (void)clock; /* the caller sets the timer's clock */
  int was = mx_di(), t = -1;
  for (int k = 0; k < 2 && t < 0; k++)
    if (k != MX_SOFT_TIMER && !claim[k].used)
      claim[k].used = 1, claim[k].proc = (u8)proc_now(), t = k;
  mx_irq_restore(was);
  return t;
}

void mx_timer_release(int t) {
  if (t < 0 || t >= 2 || t == MX_SOFT_TIMER)
    return;
  int was = mx_di();
  if (claim[t].used && (!mx__timer_proc || claim[t].proc == proc_now())) {
    MX_R32(tbase[t] + MX_TIMER_MODE_OFFSET) = 0;
    claim[t].used = 0;
  }
  mx_irq_restore(was);
}

u32 mx_timer_epc(void) {
  extern volatile u32 mx__irq_epc;
  return mx__irq_epc;
}

/* mx_exit (a program of its own): no interrupt of ours once it has gone */
void mx__timer_stop(void) {
  int was = mx_di();
  for (int i = 0; i < SLOTS; i++)
    if (slot[i].kind == ALARM)
      slot[i].kind = FREE, slot[i].armed = 0, slot[i].gen++;
  soft_stop();
  mx_irq_restore(was);
  if (handler_on)
    mx_irq_intc(SOFT_CAUSE, 0, 0);
  handler_on = 0;
}

/* the kernel, at its start in a thread of its own: the sleepers'
   semaphores and the handler are its, not a program's */
void mx__timer_init(void) {
  for (int i = 0; i < SLOTS; i++) {
    slot[i].kind = FREE, slot[i].armed = 0;
    int s = mx_sema_create(0, 1);
    slot[i].sema = s >= 0 ? s + 1 : 0;
  }
  claim[0].used = claim[1].used = 0;
  handler_on = 0;
  handler();
}

/* the kernel, at a process's end, its threads gone: its timers, sleepers
   and claims free, the soft timer set again (the end stopped the timers) */
void mx__timer_reap(int proc) {
  int was = mx_di();
  for (int i = 0; proc > 0 && i < SLOTS; i++)
    if (slot[i].kind != FREE && slot[i].proc == proc) {
      if (slot[i].kind == SLEEP && slot[i].sema)
        mx_bios_ipoll_sema(slot[i].sema - 1); /* a wake it did not take */
      slot[i].kind = FREE, slot[i].armed = 0, slot[i].gen++;
    }
  for (int k = 0; proc > 0 && k < 2; k++)
    if (claim[k].used && claim[k].proc == proc)
      claim[k].used = 0;
  arm();
  mx_irq_restore(was);
}
#endif

/* ---- the old call: a timer by its number, period in bus clocks / 256 ------ */

static int compat_id[2] = {-1, -1};

void mx_timer_periodic(int t, u32 period, mx_irq_fn fn, void *arg) {
  if (t < 0 || t > 1)
    return;
  if (compat_id[t] >= 0)
    mx_timer_remove(compat_id[t]);
  compat_id[t] = -1;
  if (!fn)
    return;
  u32 us = period * 125u / 72u; /* 256 / 147.456 MHz = 1.736 us */
  compat_id[t] = mx_timer_add(us ? us : 1, us ? us : 1, fn, arg);
}
