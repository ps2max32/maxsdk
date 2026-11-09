/* mx_sleep_us: a thread sleeps on a semaphore, and an EE timer wakes it.

   The timer runs one-shot, set to the earliest of the sleepers' ends; its
   interrupt signals each sleeper whose end has passed (COP0 Count says when),
   then sets the timer to the next. Without sleepers it is off. Not the BIOS's
   alarms (SetAlarm): those fire late or wrap when interrupts stay off for
   more than a line, which happens here (EE Kernel Reference, SetAlarm).

   Timer 0 in a program of its own (libmx). Under mx-kernel (libmxk, kernel
   version 14) the kernel's, on timer 1 (its maxsdk is built with
   MX_SLEEP_TIMER=1): programs side by side each set the timer and stopped
   it at their end (the other's sleepers were never woken), and timer 0 is
   the programs' (mx_hires' beam racing). The kernel makes the semaphores and
   the handler at its start (mx__sleep_init) and frees the slots of a
   program's threads at its end (mx__sleep_reap). */
#include <mx/bios.h>
#include <mx/bits.h>
#include <mx/cpu.h>
#include <mx/hw/formats.h>
#include <mx/hw/regs.h>
#include <mx/irq.h>
#include <mx/kernel.h>
#include <mx/thread.h>
#include <mx/timer.h>

#ifndef MX_SLEEP_TIMER
#define MX_SLEEP_TIMER 0
#endif
#if MX_SLEEP_TIMER == 1
#define TIMER_BASE MX_TIMER_T1
#define TIMER_CAUSE MX_INTC_TIM1
#else
#define TIMER_BASE MX_TIMER_T0
#define TIMER_CAUSE MX_INTC_TIM0
#endif

#define SLOTS 16
/* the timer counts the bus clock / 256: one count is 512 EE cycles (1.74 us) */
#define CYCLES_PER_COUNT (MX_EE_HZ / (MX_BUS_HZ / 256u))
/* one wait at most this long; longer sleeps take several (Count wraps in 14 s) */
#define CHUNK_US 1000000u

static struct {
  u32 due;  /* COP0 Count at the end */
  int sema; /* + 1, 0: not made yet */
  u8 used, waiting;
  s16 tid; /* the thread using it */
} slot[SLOTS];
static int handler_on;

static void timer_stop(void) { MX_R32(TIMER_BASE + MX_TIMER_MODE_OFFSET) = 0; }

/* With interrupts off: the timer to the earliest end, or off. */
static void arm(void) {
  u32 now = mx_count(), wait = 0;
  int any = 0;
  for (int i = 0; i < SLOTS; i++) {
    if (!slot[i].waiting)
      continue;
    s32 left = (s32)(slot[i].due - now);
    u32 w = left > 0 ? (u32)left : 0;
    if (!any || w < wait)
      wait = w, any = 1;
  }
  timer_stop();
  if (!any)
    return;
  u32 counts = wait / CYCLES_PER_COUNT + 1;
  if (counts > 0xFFFF)
    counts = 0xFFFF; /* about 113 ms: the interrupt then sets it again */
  u32 b = TIMER_BASE;
  MX_R32(b + MX_TIMER_COUNT_OFFSET) = 0;
  MX_R32(b + MX_TIMER_COMP_OFFSET) = counts;
  MX_R32(b + MX_TIMER_MODE_OFFSET) =
      (u32)(MX_SET(TIMER_MODE_CLKS, 2) | MX_BIT(TIMER_MODE_CUE) | MX_BIT(TIMER_MODE_CMPE) |
            MX_BIT(TIMER_MODE_EQUF) | MX_BIT(TIMER_MODE_OVFF));
}

static void tick(void *arg) {
  (void)arg;
  u32 now = mx_count();
  for (int i = 0; i < SLOTS; i++)
    if (slot[i].waiting && (s32)(now - slot[i].due) >= 0) {
      slot[i].waiting = 0;
      mx_isema_signal(slot[i].sema - 1);
    }
  arm(); /* also acknowledges: the mode is written with EQUF set */
}

static int take_slot(void) {
  int was = mx_di();
  for (int i = 0; i < SLOTS; i++)
    if (!slot[i].used) {
      slot[i].used = 1;
      slot[i].tid = (s16)mx_bios_get_thread_id();
      mx_irq_restore(was);
      return i;
    }
  mx_irq_restore(was);
  return -1;
}

static void sleep_once(int i, u32 us) {
  int was = mx_di();
  slot[i].due = mx_count() + us * (MX_EE_HZ / 1000000u);
  slot[i].waiting = 1;
  arm();
  mx_irq_restore(was);
  mx_sema_wait(slot[i].sema - 1);
}

void mx_sleep_us(u32 us) {
  if (!us) {
    mx_thread_yield();
    return;
  }
#ifdef MX_KERNEL
  if (mx__kernel && mx__kernel->version >= 14 &&
      mx__kernel->size >= __builtin_offsetof(struct mxk_services, sleep_us) + sizeof(void *) &&
      mx__kernel->sleep_us) {
    mx__kernel->sleep_us(us);
    return;
  }
#endif
  if (!handler_on) {
    mx_irq_intc(TIMER_CAUSE, tick, 0);
    handler_on = 1;
  }
  int i = take_slot();
  if (i >= 0 && !slot[i].sema) {
    int s = mx_sema_create(0, 1);
    if (s >= 0)
      slot[i].sema = s + 1;
  }
  if (i < 0 || !slot[i].sema) {
    /* more sleepers than slots, or no semaphore: wait without sleeping */
    if (i >= 0)
      slot[i].used = 0;
    mx_delay_us(us);
    return;
  }
  while (us) {
    u32 n = us > CHUNK_US ? CHUNK_US : us;
    sleep_once(i, n);
    us -= n;
  }
  slot[i].used = 0;
}

/* mx_exit: no interrupt of ours once the program has gone (not the
   kernel's timer: this program did not set it then) */
void mx__sleep_stop(void) {
  if (!handler_on)
    return;
  timer_stop();
  mx_irq_intc(TIMER_CAUSE, 0, 0);
  handler_on = 0;
}

#ifndef MX_KERNEL
/* the kernel (apps/mxos), at its start in a thread of its own: the
   semaphores and the handler are its, not a program's */
void mx__sleep_init(void) {
  for (int i = 0; i < SLOTS; i++) {
    slot[i].used = slot[i].waiting = 0;
    int s = mx_sema_create(0, 1);
    slot[i].sema = s >= 0 ? s + 1 : 0;
  }
  timer_stop();
  mx_irq_intc(TIMER_CAUSE, tick, 0);
  handler_on = 1;
}

/* the kernel, at a program's end, its threads gone: their slots free, and
   the timer set again (the end stopped it) for the other's sleepers */
void mx__sleep_reap(void) {
  int was = mx_di();
  for (int i = 0; i < SLOTS; i++)
    if (slot[i].used && mx_bios_refer_thread_status(slot[i].tid, 0) <= 0) { /* 0: none */
      slot[i].used = slot[i].waiting = 0;
      if (slot[i].sema)
        mx_bios_ipoll_sema(slot[i].sema - 1); /* a wake it did not take */
    }
  arm();
  mx_irq_restore(was);
}
#endif
