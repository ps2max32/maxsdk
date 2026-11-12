/* Probe: dump EE kernel memory (the exception vectors and what they call),
   read in kernel mode by a syscall handler. Arguments: [hex address [hex
   words]]; the default is the vectors, 0x80000000 + 0x100 words. */

#include <mx/bios.h>
#include <mx/mx.h>

/* One word, read by a syscall handler (the kernel runs it in kernel mode):
   SetSyscall(0x5A), the number ps2sdk uses for its kernel copy. Writing
   Status.KSU from user mode to get there hung the PS2. */
static u32 k_read(u32 a) { return *(volatile u32 *)a; }

static u32 kread(u32 a) {
  register u32 a0 __asm__("$4") = a;
  register u32 v1 __asm__("$3") = 0x5A; /* the syscall number */
  register u32 v0 __asm__("$2");
  __asm__ volatile("syscall" : "=r"(v0), "+r"(v1) : "r"(a0) : "memory");
  return v0;
}

static u32 hex(const char *s) {
  u32 v = 0;
  for (; *s; s++)
    v = v << 4 | (u32)(*s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10);
  return v;
}

int main(int argc, char **argv) {
  u32 a = argc >= 2 ? hex(argv[1]) : 0x80000000u, n = argc >= 3 ? hex(argv[2]) : 0x100;
  mx_bios_set_syscall(0x5A, (void *)k_read);
  mx_bios_flush_cache(0);
  mx_printf("kdump %08x %x words, Status %08x\n", a, n, mx_status());
  for (u32 i = 0; i < n; i += 4) {
    u32 w[4];
    for (int k = 0; k < 4; k++)
      w[k] = kread(a + 4 * (i + (u32)k));
    mx_printf("kd %08x: %08x %08x %08x %08x\n", a + 4 * i, w[0], w[1], w[2], w[3]);
  }
  MX_TEST_END(0);
  return 0;
}
