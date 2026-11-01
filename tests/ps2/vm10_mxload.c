/* VM step 10: started by mxload (loader/mxload.c), a physical program, to
   see that the loader puts it in place and hands over the machine. Run as
     run.sh mxload.elf 30 host:vm10_mxload.elf a bc
   (PCSX2: host:mxload.txt holding "host:vm10_mxload.elf a bc"). A few
   seconds; at the end the screen is bright green with 900D0000 in slot 0,
   or bright red with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: argc, slot 2: a value checked.
   Heartbeat in slots 30 and 31.
     1 started by mxload, argv[0] this ELF's path, then "a" and "bc"
     2 .data, .rodata and .bss as in the file (a 256 KiB .bss all zero)
     3 interrupts: the vblank-end interrupt counts
     4 host: I/O: the first bytes of argv[0], the ELF, read back */

#include "t.h"
#include <mx/hw/formats.h>
#include <mx/irq.h>
#include <mx/loader.h>
#include <mx/sif.h>
#include <mx/string.h>

static u32 data_word = 0x12345678u;
static const u32 table[64] = {
    1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987, 1597,
};
static u8 big[256 * 1024];

static int ends_with(const char *s, const char *tail) {
  size_t n = strlen(s), m = strlen(tail);
  return n >= m && strcmp(s + n - m, tail) == 0;
}

int main(int argc, char **argv) {
  mx_dbg_screen();
  mx_dbg_heartbeat(1);

  mx_dbg_step(1);
  mx_dbg_word(1, (u32)argc);
  mx_printf("vm10: 1 argc %d, argv[0] %s\n", argc, argc > 0 ? argv[0] : "-");
  CHECK_EQ(mx_loader_mxload(), 1);
  CHECK_EQ(argc, 3);
  if (argc == 3) {
    CHECK(ends_with(argv[0], "vm10_mxload.elf"));
    CHECK(strcmp(argv[1], "a") == 0);
    CHECK(strcmp(argv[2], "bc") == 0);
  }

  mx_dbg_step(2);
  mx_dbg_word(2, data_word);
  CHECK_EQ(data_word, 0x12345678u);
  u32 sum = 0;
  for (u32 i = 0; i < 64; i++)
    sum += table[i];
  CHECK_EQ(sum, 4179u);
  u32 nonzero = 0;
  for (u32 i = 0; i < sizeof big; i++)
    nonzero += big[i] != 0;
  CHECK_EQ(nonzero, 0);

  mx_dbg_step(3);
  u32 v0 = mx_irq_intc_count(MX_INTC_VBOF), t0 = mx_count();
  while (mx_irq_intc_count(MX_INTC_VBOF) - v0 < 10 && mx_count() - t0 < 300000000u)
    ;
  mx_dbg_word(2, mx_irq_intc_count(MX_INTC_VBOF) - v0);
  CHECK(mx_irq_intc_count(MX_INTC_VBOF) - v0 >= 10);

  mx_dbg_step(4);
  if (argc >= 1) {
    static u8 head[16] MX_ALIGNED(64);
    int fd = mx_fio_open(argv[0], MX_FIO_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
      CHECK_EQ(mx_fio_read(fd, head, sizeof head), sizeof head);
      mx_dbg_word(2, *(u32 *)head);
      CHECK_EQ(*(u32 *)head, 0x464c457fu);
      mx_fio_close(fd);
    }
  }

  mx_dbg_heartbeat(0);
  return t_done("vm10");
}
