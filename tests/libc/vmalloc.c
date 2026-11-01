/* malloc in virtual memory: the program defines mx_vm_config, so VM is on
   before the C library starts and malloc (sbrk) hands out VM, here far
   more than the PS2's 32 MiB, with the frames kept small so most of it is
   in the swap file. On PCSX2 run it with PCSX2_EE=interp. */

#include <mx/mx.h>
#include <mx/vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const struct mx_vm_config mx_vm_config = {.swap = "host:vmalloc.swap", .frames = 4u << 20};

#define BLOCK (8u << 20)
#define NBLOCK 6 /* 48 MiB */

static unsigned val(unsigned b, unsigned i) { return (b * 0x9e3779b9u) ^ (i * 2654435761u); }

int main(void) {
  int fail = 0;
  if (!mx_vm_active()) {
    printf("vmalloc: VM is off\n");
    MX_TEST_END(1);
    return 1;
  }
  unsigned *blk[NBLOCK];
  for (unsigned b = 0; b < NBLOCK; b++) {
    blk[b] = malloc(BLOCK);
    if (!blk[b] || !mx_is_vm(blk[b])) {
      printf("vmalloc: block %u at %p\n", b, (void *)blk[b]);
      fail = 1;
      break;
    }
    for (unsigned i = 0; i < BLOCK / 4; i += 16)
      blk[b][i] = val(b, i);
  }
  for (unsigned b = 0; b < NBLOCK && !fail; b++) {
    unsigned bad = 0;
    for (unsigned i = 0; i < BLOCK / 4; i += 16)
      bad += blk[b][i] != val(b, i);
    if (bad) {
      printf("vmalloc: block %u: %u words wrong\n", b, bad);
      fail = 1;
    }
  }
  /* a string in VM through the C library */
  char *s = malloc(64);
  snprintf(s, 64, "%s %d", "in VM", 42);
  fail |= strcmp(s, "in VM 42") != 0;
  free(s);
  for (unsigned b = 0; b < NBLOCK; b++)
    free(blk[b]);

  struct mx_vm_stats st;
  mx_vm_stats(&st);
  printf("vmalloc: %u MiB in %u frames; swap in %u, out %u, drops %u\n", NBLOCK * BLOCK >> 20,
         st.frames, st.swap_ins, st.swap_outs, st.drops);
  printf("vmalloc: %s\n", fail ? "FAIL" : "ok");
  MX_TEST_END(fail);
  return fail;
}
