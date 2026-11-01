/* libc/malloc.c on the host: random malloc, free, realloc, memalign and
   calloc over a fake sbrk, the heap checked after each, the blocks' bytes
   kept and compared. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *mxc_malloc(size_t n);
void mxc_free(void *p);
void *mxc_realloc(void *p, size_t n);
void *mxc_calloc(size_t n, size_t m);
void *mxc_memalign(size_t a, size_t n);
size_t mxc_malloc_usable_size(void *p);
int mxc_malloc_check(void);

static char *arena, *brk_at;
#define ARENA (64u << 20)

void *mxc_morecore(intptr_t incr) {
  if (!arena)
    arena = brk_at = aligned_alloc(4096, ARENA);
  if (brk_at + incr > arena + ARENA)
    return (void *)-1;
  char *r = brk_at;
  brk_at += incr;
  return r;
}

#define N 2000
static struct {
  unsigned char *p;
  size_t n;
  unsigned char seed;
} live[N];

static int bytes_ok(int i) {
  for (size_t k = 0; k < live[i].n; k++)
    if (live[i].p[k] != (unsigned char)(live[i].seed + k))
      return 0;
  return 1;
}

static void fill(int i) {
  for (size_t k = 0; k < live[i].n; k++)
    live[i].p[k] = (unsigned char)(live[i].seed + k);
}

int test_malloc(void) {
  unsigned x = 12345;
  int fails = 0;
  for (int step = 0; step < 200000 && !fails; step++) {
    x = x * 1103515245u + 12345u;
    int i = (int)((x >> 8) % N);
    unsigned op = x >> 28;
    size_t n = (x >> 4) & 0xFF;
    if (op == 0)
      n = (x >> 4) & 0xFFFFF; /* large, up to 1 MiB */
    if (live[i].p && !bytes_ok(i)) {
      printf("malloc: block %d lost its bytes at step %d\n", i, step);
      fails++;
      break;
    }
    if (live[i].p && op < 6) {
      mxc_free(live[i].p);
      live[i].p = NULL;
    } else if (live[i].p && op < 10) {
      unsigned char *q = mxc_realloc(live[i].p, n + 1);
      if (!q) {
        printf("malloc: realloc failed\n");
        fails++;
        break;
      }
      size_t keep = live[i].n < n + 1 ? live[i].n : n + 1;
      for (size_t k = 0; k < keep; k++)
        if (q[k] != (unsigned char)(live[i].seed + k)) {
          printf("malloc: realloc lost bytes at step %d\n", step);
          fails++;
          break;
        }
      live[i].p = q;
      live[i].n = n + 1;
      fill(i);
    } else if (!live[i].p) {
      size_t align = op >= 14 ? (size_t)64 << (x & 7) : 0;
      live[i].p = align ? mxc_memalign(align, n) : op == 13 ? mxc_calloc(n, 1) : mxc_malloc(n);
      if (!live[i].p || (uintptr_t)live[i].p & 15 || (align && (uintptr_t)live[i].p & (align - 1)) ||
          mxc_malloc_usable_size(live[i].p) < n) {
        printf("malloc: bad block at step %d (align %zu)\n", step, align);
        fails++;
        break;
      }
      if (op == 13)
        for (size_t k = 0; k < n; k++)
          if (live[i].p[k]) {
            printf("malloc: calloc not zero\n");
            fails++;
            break;
          }
      live[i].n = n;
      live[i].seed = (unsigned char)x;
      fill(i);
    }
    int c = mxc_malloc_check();
    if (c) {
      printf("malloc: heap check %d at step %d\n", c, step);
      fails++;
    }
  }
  for (int i = 0; i < N; i++)
    if (live[i].p) {
      mxc_free(live[i].p);
      live[i].p = NULL;
    }
  if (!fails && mxc_malloc_check()) {
    printf("malloc: heap check after freeing all\n");
    fails++;
  }
  printf("malloc: %s (heap %zu KiB)\n", fails ? "FAIL" : "ok", (size_t)(brk_at - arena) >> 10);
  return fails;
}
