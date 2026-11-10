/* Directories over the host link in a program linked into VM: dopen, dread
   and dclose of host:'s directory, many times, each name read into a fresh
   heap block with guards around it, and the heap checked after (a game's
   readdir that corrupted its heap). Started by the kernel:
     vm36_dir.elf     (status 0 expected) */
#include "t.h"
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/mem.h>

#define ROOM 256
#define GUARD 0x5A
#define ROUNDS 4

struct ent {
  struct ent *next;
  u8 before[16];
  char name[ROOM];
  u8 after[16];
};

static int guards_ok(const struct ent *e) {
  for (int i = 0; i < 16; i++)
    if (e->before[i] != GUARD || e->after[i] != GUARD)
      return 0;
  return 1;
}

int main(void) {
  u32 names = 0, first = 0;
  mx_printf("vm36: start\n");
  for (int round = 0; round < ROUNDS; round++) {
    int d = mx_fio_dopen("host:");
    CHECK(d >= 0);
    if (round < 2)
      mx_printf("vm36: round %d: dopen %d\n", round, d);
    struct ent *list = 0;
    u32 n = 0, size;
    for (;;) {
      struct ent *e = mx_heap_alloc(mx_heap_paged, sizeof *e, 16);
      CHECK(e != 0);
      memset(e, 0, sizeof *e);
      memset(e->before, GUARD, 16);
      memset(e->after, GUARD, 16);
      if (mx_fio_dread(d, e->name, ROOM, &size) <= 0) {
        CHECK(guards_ok(e));
        mx_heap_free(mx_heap_paged, e);
        break;
      }
      if (round < 1)
        mx_printf("vm36:   %s\n", e->name);
      CHECK(guards_ok(e));
      CHECK(strlen(e->name) < ROOM);
      e->next = list;
      list = e;
      n++;
    }
    CHECK(mx_fio_dclose(d) >= 0); /* the ROM's FILEIO answers with the descriptor */
    if (!round)
      first = n;
    CHECK_EQ(n, first);
    while (list) {
      struct ent *e = list;
      list = e->next;
      CHECK(guards_ok(e));
      mx_heap_free(mx_heap_paged, e);
    }
    /* the heap after it: blocks of several sizes */
    void *p[32];
    for (int i = 0; i < 32; i++) {
      p[i] = mx_heap_alloc(mx_heap_paged, 16 + i * 97, 16);
      CHECK(p[i] != 0);
      memset(p[i], i, 16 + i * 97);
    }
    for (int i = 0; i < 32; i++)
      mx_heap_free(mx_heap_paged, p[i]);
    names += n;
  }
  mx_printf("vm36: %u names in %d rounds (%u a round)\n", names, ROUNDS, first);
  CHECK(first > 2);
  return t_done("vm36");
}
