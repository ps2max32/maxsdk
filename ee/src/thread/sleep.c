/* mx_sleep_us: a thread sleeps on a semaphore, and a software timer wakes it
   (src/irq/timer.c). Under mx-kernel (libmxk, kernel version 14) the
   kernel's, one timer for all processes. */
#include <mx/kernel.h>
#include <mx/thread.h>
#include <mx/timer.h>

void mx__timer_sleep(u32 us);

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
  mx_delay_us(us); /* an older kernel's: its timer 0 is the program's */
#else
  mx__timer_sleep(us);
#endif
}
