/* Pages read without the VM lock (kernel version 44): while a thread waits
   for a page from a file, another thread's fault that needs no read goes
   on. Two reader threads (above the main thread) read the same 32 pages
   of a mapped file (host:k83.bin, 1 MiB), so they also meet on a page the
   other one is reading; the main thread meanwhile touches new pages of a
   paged heap (zero fills) and times each touch. Checks:
   - both readers read every page right;
   - the heap's pages hold what was written;
   - on the PS2 (dirty tracking: real timings) the longest touch is shorter
     than the readers' average read: it did not wait for a whole read, as
     it did under the lock (before version 44).
     k83_ioread.elf     (status 0 expected) */
#include "t.h"
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/mem.h>
#include <mx/thread.h>
#include <mx/vm.h>

MX_CORE_HEAP(256 << 10);

#define PAGE MX_VM_PAGE
#define NPG 32
#define HEAP_PG 64

static u32 E(u32 i) { return i * 2654435761u ^ 0x1E1E1E1Eu; }
static u32 buf[PAGE / 4] MX_DMA_LINES;

static u8 stack1[8192] MX_IRQ_DATA MX_ALIGNED(16);
static u8 stack2[8192] MX_IRQ_DATA MX_ALIGNED(16);
static const u32 *file;
static volatile u32 bad[2], cycles[2], done[2];

static void reader(void *arg) {
  u32 k = (u32)(uintptr_t)arg, t0 = mx_count();
  for (u32 pg = 0; pg < NPG; pg++) {
    u32 i = pg * (PAGE / 4) + k * 7;
    bad[k] += file[i] != E(i);
  }
  cycles[k] = mx_count() - t0;
  done[k] = 1;
}

int main(void) {
  CHECK(mx__kernel && mx__kernel->version >= 44);
  int fd = mx_fio_open("host:k83.bin", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  for (u32 pg = 0; pg < NPG; pg++) {
    for (u32 i = 0; i < PAGE / 4; i++)
      buf[i] = E(pg * (PAGE / 4) + i);
    CHECK_EQ(mx_fio_write(fd, buf, PAGE), 0); /* 0: all written */
  }
  mx_fio_close(fd);
  file = mx_vm_map_file("host:k83.bin", 0);
  CHECK(file != 0);
  u32 *heap = mx_heap_alloc(mx_heap_paged, HEAP_PG * PAGE, 64);
  CHECK(heap != 0);
  if (!file || !heap)
    return t_done("k83");

  struct mx_vm_stats s0, s1;
  mx_vm_stats(&s0);
  int prio = mx_thread_prio(mx_thread_self());
  int t1 = mx_thread_start(reader, (void *)0, stack1, sizeof stack1, prio - 1);
  int t2 = mx_thread_start(reader, (void *)1, stack2, sizeof stack2, prio - 1);
  CHECK(t1 >= 0 && t2 >= 0);
  /* the readers wait for their pages: the main thread runs then */
  u32 longest = 0, touched = 0;
  while (touched < HEAP_PG && !(done[0] && done[1])) {
    u32 t0 = mx_count();
    heap[touched * (PAGE / 4)] = E(touched) ^ 3;
    u32 c = mx_count() - t0;
    if (c > longest)
      longest = c;
    touched++;
  }
  mx_thread_join(t1);
  mx_thread_join(t2);
  mx_vm_stats(&s1);
  u32 reads = s1.file_ins - s0.file_ins;
  u32 per_read = reads ? (cycles[0] > cycles[1] ? cycles[0] : cycles[1]) / NPG : 0;
  mx_printf("k83: %u file pages read (%u ms), %u heap pages touched meanwhile, the longest "
            "touch %u us; a reader %u us a page; lock waits %u ms\n",
            reads, (s1.us_file_in - s0.us_file_in) / 1000, touched,
            longest / (MX_EE_HZ / 1000000u), per_read / (MX_EE_HZ / 1000000u),
            (s1.us_lock - s0.us_lock) / 1000);
  CHECK_EQ(bad[0], 0);
  CHECK_EQ(bad[1], 0);
  CHECK(reads >= NPG && reads <= NPG + 2); /* each page once, though both read it */
  u32 hbad = 0;
  for (u32 i = 0; i < touched; i++)
    hbad += heap[i * (PAGE / 4)] != (E(i) ^ 3);
  CHECK_EQ(hbad, 0);
  if (s1.dirty_tracking) {
    CHECK(touched > 0);
    CHECK(longest < per_read / 2);
  }
  mx_vm_unmap_file(file);
  mx_heap_free(mx_heap_paged, heap);
  return t_done("k83");
}
