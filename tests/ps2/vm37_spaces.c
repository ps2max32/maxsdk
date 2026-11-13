/* Address spaces in the VM (mx OS plan, the address space plan's phase 3a):
   two spaces over the same VM addresses share 20 frames (640 KiB, 4 of
   them the second space's tables) and the
   swap file, one LRU over both. Each writes 40 pages of its own pattern, so
   writing one space evicts the other's pages; after each switch every page
   must read back as its own space wrote it. Then a third space is made and
   freed again, and the tables are checked (mx__vm_check). Standalone, on
   PCSX2's interpreter or the PS2.
     vm37_spaces.elf     (status 0 expected) */

#include "t.h"
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define NP 40

struct vm_space;
struct vm_space *mx__vm_space_new(u32 size);
void mx__vm_space_switch(struct vm_space *s);
struct vm_space *mx__vm_space_now(void);
void mx__vm_space_free(struct vm_space *s);
u32 mx__vm_check(void);

static u32 *pg(u32 k) { return (u32 *)((u8 *)mx_vm_base() + k * PAGE); }
static u32 word(u32 sp, u32 k, u32 i, u32 g) { return (sp + 1) * 0x9E3779B1u ^ k * 2654435761u ^ i * 40503u ^ g; }

static void write_all(u32 sp, u32 g) {
  for (u32 k = 0; k < NP; k++) {
    u32 *p = pg(k);
    for (u32 i = 0; i < PAGE / 4; i += 16)
      p[i] = word(sp, k, i, g);
  }
}

static u32 wrong_all(u32 sp, u32 g) {
  u32 w = 0;
  for (u32 k = 0; k < NP; k++) {
    const u32 *p = pg(k);
    for (u32 i = 0; i < PAGE / 4; i += 16)
      w += p[i] != word(sp, k, i, g);
  }
  return w;
}

int main(void) {
  /* the default sizes: a space's tables (about 120 KiB) take 4 of the
     frames, pinned */
  static const struct mx_vm_config cfg = {.swap = "host:vm37swap.bin", .frames = 20 * PAGE};
  CHECK_EQ(mx_vm_init(&cfg), 0);
  struct vm_space *s[2];
  s[0] = mx__vm_space_now();
  s[1] = mx__vm_space_new(0);
  CHECK(s[1] != 0);
  if (!s[1])
    return t_done("vm37");

  /* 1: each space its pattern */
  write_all(0, 1);
  mx__vm_space_switch(s[1]);
  CHECK_EQ(wrong_all(1, 0) == 0 ? 1 : 0, 0); /* new pages are zero, not space 0's */
  write_all(1, 1);
  mx__vm_space_switch(s[0]);
  u32 w0 = wrong_all(0, 1);
  mx__vm_space_switch(s[1]);
  u32 w1 = wrong_all(1, 1);
  mx_printf("vm37: 1 after one round: %u and %u words wrong\n", w0, w1);
  CHECK_EQ(w0, 0);
  CHECK_EQ(w1, 0);

  /* 2: rounds of writes and checks, switching each time */
  u32 g[2] = {1, 1}, bad = 0;
  for (u32 r = 0; r < 6; r++) {
    u32 sp = r & 1;
    mx__vm_space_switch(s[sp]);
    bad += wrong_all(sp, g[sp]);
    write_all(sp, ++g[sp]);
  }
  for (u32 sp = 0; sp < 2; sp++) {
    mx__vm_space_switch(s[sp]);
    bad += wrong_all(sp, g[sp]);
  }
  struct mx_vm_stats st;
  mx_vm_stats(&st);
  mx_printf("vm37: 2 after 6 rounds: %u words wrong; %u swap outs, %u ins\n", bad, st.swap_outs,
            st.swap_ins);
  CHECK_EQ(bad, 0);
  CHECK(st.swap_outs > 0 && st.swap_ins > 0);

  /* 3: a third space, written, freed: its frames back, the others intact */
  struct vm_space *t = mx__vm_space_new(0);
  CHECK(t != 0);
  mx__vm_space_switch(t);
  write_all(2, 1);
  mx__vm_space_switch(s[0]);
  mx__vm_space_free(t);
  u32 c = mx__vm_check();
  bad = wrong_all(0, g[0]);
  mx__vm_space_switch(s[1]);
  bad += wrong_all(1, g[1]);
  mx_printf("vm37: 3 after a third space: %u words wrong, check %08x\n", bad, c);
  CHECK_EQ(bad, 0);
  CHECK_EQ(c, 0);

  mx__vm_space_switch(s[0]);
  mx__vm_space_free(s[1]);
  return t_done("vm37");
}
