/* A VM program that waits for and runs beside another VM program (mx OS
   plan, the address space plan's phase 3a): each has an address space of
   its own on the kernel's one VM (vm.c's spaces), at the same addresses.
   This one fills 2 MiB of its VM (.bss) with a pattern, starts itself with
   "child" (which fills the same addresses with another pattern and checks
   it), first waiting for it (mx_loader_launch), then beside it
   (mx_proc_spawn, mx_proc_wait); after each its own pattern must be
   intact, and its VM still work. The child ends with 7: a run in which it
   took this one's place (it did not wait) ends with 7 and fails. Started by mxlink:
     vm38_vmbeside.elf     (status 0 expected) */

#include "t.h"
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/vm.h>

#define WORDS (2u << 20) / 4
static u32 big[WORDS];

static u32 word(u32 who, u32 i) { return (who + 1) * 0x9E3779B1u ^ i * 2654435761u; }
static void fill(u32 who) {
  for (u32 i = 0; i < WORDS; i += 8)
    big[i] = word(who, i);
}
static u32 wrong(u32 who) {
  u32 w = 0;
  for (u32 i = 0; i < WORDS; i += 8)
    w += big[i] != word(who, i);
  return w;
}

static u32 words(char *to, const char *a, const char *b) {
  u32 n = (u32)strlen(a) + 1;
  memcpy(to, a, n);
  memcpy(to + n, b, strlen(b) + 1);
  return n + (u32)strlen(b) + 1;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child")) {
    /* the same addresses, its own pages */
    u32 z = 0;
    for (u32 i = 0; i < WORDS; i += 8)
      z += big[i] != 0;
    fill(7);
    u32 w = wrong(7);
    mx_printf("vm38: child, process %d: %u words not zero at its start, %u wrong\n",
              mx_proc_self(), z, w);
    /* 7, not 0: a run in which it took the caller's place fails */
    return z || w ? 1 : 7;
  }
  CHECK(mx_vm_active());
  fill(1);
  CHECK_EQ(wrong(1), 0);
  char args[96];
  u32 n = words(args, argv[0], "child");

  /* 1: it waits */
  int st = mx_loader_launch(2, args, n, 0);
  u32 w = wrong(1);
  mx_printf("vm38: 1 waited: the child's status %d; %u words wrong here\n", st, w);
  CHECK_EQ(st, 7);
  CHECK_EQ(w, 0);

  /* 2: beside it */
  int id = mx_proc_spawn(2, args, n, 0);
  CHECK(id > 0);
  int r = id > 0 ? mx_proc_wait(id, &st, 0) : -1;
  w = wrong(1);
  mx_printf("vm38: 2 beside: process %d, wait %d, status %d; %u words wrong here\n", id, r, st, w);
  CHECK_EQ(r, 1);
  CHECK_EQ(st, 7);
  CHECK_EQ(w, 0);

  /* its VM still works */
  fill(2);
  CHECK_EQ(wrong(2), 0);
  return t_done("vm38");
}
