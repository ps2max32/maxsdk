/* Probe: what ReferThreadStatus (its result:status word) and ReferSemaStatus return for ids that are
   there and ids that are not (mxlink's end of a program scans all ids:
   tools/mxlink/ee/end.c). Ids 0-24 and 250-256, before and after a thread
   and a semaphore are made and deleted. */
#include <mx/bios.h>
#include <mx/mx.h>
#include <mx/thread.h>

static void row(const char *what) {
  u32 st[16];
  mx_printf("p34_ids: %s, threads with a status (id:result:status):", what);
  for (int id = 0; id <= 256; id++) {
    for (int k = 0; k < 16; k++)
      st[k] = 0xEEEEEEEEu;
    int r = mx_bios_refer_thread_status(id, st);
    if (r != 0 || st[0] != 0)
      mx_printf(" %d:%d:%x", id, r, st[0]);
  }
  mx_printf("\np34_ids: %s, semas:", what);
  for (int id = 0; id < 256; id++)
    if (mx_bios_refer_sema_status(id, st) >= 0)
      mx_printf(" %d", id);
  mx_printf("\n");
}

static void idle(void *a) {
  (void)a;
  mx_bios_sleep_thread();
}
static u8 stack[4096] MX_ALIGNED(16);

int main(void) {
  mx_printf("p34_ids: this thread %d\n", mx_bios_get_thread_id());
  row("start");
  int t = mx_thread_start(idle, 0, stack, sizeof stack, 100);
  int s = mx_sema_create(0, 1);
  mx_printf("p34_ids: made thread %d, sema %d\n", t, s);
  row("made");
  mx_bios_terminate_thread(t);
  mx_bios_delete_thread(t);
  mx_sema_delete(s);
  row("deleted");
  mx_printf("@@PS2TEST_END@@ status=0\n");
  return 0;
}
