/* mx_sleep_us: a thread sleeps on a semaphore, and EE timer 0 wakes it.

   The timer runs one-shot, set to the earliest of the sleepers' ends; its
   interrupt signals each sleeper whose end has passed (COP0 Count says when),
   then sets the timer to the next. Without sleepers it is off. Not the BIOS's
   alarms (SetAlarm): those fire late or wrap when interrupts stay off for
   more than a line, which happens here (EE Kernel Reference, SetAlarm). */
#include <mx/bits.h>
#include <mx/cpu.h>
#include <mx/hw/formats.h>
#include <mx/hw/regs.h>
#include <mx/irq.h>
#include <mx/thread.h>
#include <mx/timer.h>

#define SLOTS 16
/* timer 0 counts the bus clock / 256: one count is 512 EE cycles (1.74 us) */
#define CYCLES_PER_COUNT (MX_EE_HZ / (MX_BUS_HZ / 256u))
/* one wait at most this long; longer sleeps take several (Count wraps in 14 s) */
#define CHUNK_US 1000000u

static struct {
  u32 due;  /* COP0 Count at the end */
  int sema; /* + 1, 0: not made yet */
  u8 used, waiting;
} slot[SLOTS];
static int handler_on;

static void timer_stop(void) { MX_R32(MX_TIMER_T0 + MX_TIMER_MODE_OFFSET) = 0; }

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
  u32 b = MX_TIMER_T0;
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
  if (!handler_on) {
    mx_irq_intc(MX_INTC_TIM0, tick, 0);
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

/* mx_exit: no interrupt of ours once the program has gone */
void mx__sleep_stop(void) {
  timer_stop();
  if (handler_on)
    mx_irq_intc(MX_INTC_TIM0, 0, 0);
  handler_on = 0;
}
