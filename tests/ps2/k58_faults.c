/* The fault log under the kernel (<mx/vm.h>: mx_vm_trace_restart without a
   trace): 64 pages new to the program, written, then read; a mark between;
   the log saved to host:k58.flog, read back and checked: the head
   (version 2), the zero fills of the 64 pages as stores, with an EPC and
   the time going up, the mark.
     k58_faults.elf      (status 0) */
#include <mx/sif.h>
#include <mx/vm.h>
#include "t.h"

#define NP 64

int main(void) {
  u8 *m = mx_sbrk(NP * MX_VM_PAGE + MX_VM_PAGE);
  if (m == (u8 *)-1)
    m = 0;
  CHECK(m != 0);
  if (!m)
    return t_done("k58");
  u8 *p = (u8 *)(((uintptr_t)m + MX_VM_PAGE - 1) & ~(uintptr_t)(MX_VM_PAGE - 1));
  mx_vm_trace_restart();
  for (u32 i = 0; i < NP; i++)
    p[i * MX_VM_PAGE + 100] = (u8)i; /* a new page each: a zero fill, written */
  mx_vm_trace_mark(0x1234);
  u32 sum = 0;
  for (u32 i = 0; i < NP; i++)
    sum += p[i * MX_VM_PAGE + 100]; /* resident: no fault */
  CHECK_EQ(sum, NP * (NP - 1) / 2);
  int n = mx_vm_trace_save("host:k58.flog");
  mx_printf("k58: %d events saved\n", n);
  CHECK(n >= NP + 1);

  static struct mx_vm_trace_head h;
  static struct mx_vm_fault e[512];
  int fd = mx_fio_open("host:k58.flog", MX_FIO_RDONLY);
  CHECK(fd >= 0);
  if (fd < 0)
    return t_done("k58");
  CHECK_EQ(mx_fio_read(fd, &h, sizeof h), (int)sizeof h);
  CHECK(h.magic == MX_VM_TRACE_MAGIC && h.version == 2 && h.page == MX_VM_PAGE && h.block == 0);
  CHECK_EQ(h.events, (u32)n);
  u32 k = h.events < 512 ? h.events : 512;
  CHECK_EQ(mx_fio_read(fd, e, k * sizeof *e), (int)(k * sizeof *e));
  mx_fio_close(fd);

  u32 ours = 0, mark = ~0u, last_t = 0, slow = 0;
  for (u32 i = 0; i < k; i++) {
    u32 va = e[i].va_flags & ~(MX_VM_PAGE - 1), kind = e[i].va_flags & MX_VM_FAULT_KIND;
    CHECK(e[i].t_us >= last_t);
    last_t = e[i].t_us;
    if (kind == MX_VM_FAULT_MARK) {
      CHECK_EQ(e[i].epc, 0x1234u);
      mark = i;
      continue;
    }
    if (e[i].us > slow)
      slow = e[i].us;
    if (va >= (uintptr_t)p && va < (uintptr_t)p + NP * MX_VM_PAGE) {
      CHECK(kind == MX_VM_FAULT_ZERO || kind == MX_VM_FAULT_SWAP);
      CHECK(e[i].va_flags & MX_VM_FAULT_WRITE);
      CHECK(e[i].epc != 0);
      CHECK(mark == ~0u); /* all before the mark */
      ours++;
    }
  }
  mx_printf("k58: %u faults on the pages, the mark at %u, the slowest %u us, %u us in all\n", ours,
            mark, slow, last_t);
  CHECK_EQ(ours, NP);
  CHECK(mark != ~0u);
  return t_done("k58");
}
