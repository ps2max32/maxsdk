/* A line of 300 characters and more from a program whose core is at a run
   of its own: the kernel writes such a line to the tty from the program's
   buffer by DMA (mx_fio_write's long path), at its physical address.
   Linked into VM, started by mxlink. */
#include <mx/debug.h>

static char line[640];

int main(void) {
  for (int i = 0; i < 600; i++)
    line[i] = (char)('a' + i % 26);
  line[600] = 0;
  mx_printf("vm29: static %s\n", line);
  char local[640];
  for (int i = 0; i < 600; i++)
    local[i] = (char)('A' + i % 26);
  local[600] = 0;
  mx_printf("vm29: stack %s\n", local);
  mx_printf("vm29: %u %u %u %s\n", 1u, 2u, 3u, "done");
  mx_printf("@@PS2TEST_END@@ status=0\n");
  return 0;
}
