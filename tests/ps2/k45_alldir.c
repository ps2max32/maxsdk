/* A program that takes all memory: 16 MiB filled with a pattern, then the
   host link's file calls (open, dopen of a missing and of host:'s directory,
   dread to the end, dclose); no word of it may change. Started by the kernel:
     k45_alldir.elf   (status 0 expected) */
#include "t.h"
#include <mx/sif.h>

#define WORDS (16u << 18) /* 16 MiB */
static u32 big[WORDS] __attribute__((aligned(64)));
#define PAT 0xA5C3E1F0u

static void fill(void) {
  for (u32 i = 0; i < WORDS; i++)
    big[i] = PAT ^ i;
}
static u32 scan(const char *what) {
  u32 bad = 0;
  for (u32 i = 0; i < WORDS; i++)
    if (big[i] != (PAT ^ i)) {
      if (bad < 12)
        mx_printf("k45: %s: %08x = %08x\n", what, (u32)(uintptr_t)&big[i], big[i]);
      bad++;
    }
  mx_printf("k45: %s: %u words changed\n", what, bad);
  return bad;
}

int main(void) {
  mx_printf("k45: big %08x-%08x\n", (u32)(uintptr_t)big, (u32)(uintptr_t)(big + WORDS));
  fill();
  mx_sleep_us(1000000);
  CHECK(scan("idle") == 0);
  fill();
  int fd = mx_fio_open("host:k45_alldir.elf", 0);
  mx_printf("k45: open %d\n", fd);
  if (fd >= 0)
    mx_fio_close(fd);
  CHECK(scan("open") == 0);
  fill();
  int d = mx_fio_dopen("host:nothere");
  mx_printf("k45: dopen missing %d\n", d);
  CHECK(scan("dopen-missing") == 0);
  fill();
  char name[256];
  u32 size, n = 0;
  d = mx_fio_dopen("host:");
  mx_printf("k45: dopen %d\n", d);
  CHECK(scan("dopen") == 0);
  fill();
  if (d >= 0) {
    while (mx_fio_dread(d, name, sizeof name, &size) > 0)
      n++;
    mx_printf("k45: %u names\n", n);
    CHECK(scan("dread") == 0);
    fill();
    CHECK(mx_fio_dclose(d) >= 0);
    CHECK(scan("dclose") == 0);
  }
  return t_done("k45");
}
