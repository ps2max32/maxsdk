/* VM step 16: VM locked for DMA (mx_vm_lock, <mx/vm.h>): a static buffer
   of a program linked into VM, and a range of a mapped file, sent to VU1
   data memory by DMA. Run as
     run.sh mxload.elf 60 host:vm16_lock.elf
   (PCSX2, with the EE interpreter: host:mxload.txt holding
   "host:vm16_lock.elf"). 16 frames and a swap file. Under a minute; at the
   end of steps 1-3 the screen shows the step; the result is in the output
   (and green or red if the frames of step 4 left the debug screen up).

   Slot 0 and the colour: the step. Slot 2: a value checked. Slots 4-7:
   pinned pages, drops, pages read from files, swap outs. Heartbeat in
   slots 30 and 31.
     1 a static buffer (VM) locked: mx_paddr_of gives its frame, a REF
       sends it
     2 a mapped file, 3 pages of it locked: sent from 3 places, then 40
       heap pages written (more than the frames): the locked pages stay at
       their frames, the data right
     3 unlocked, the heap written again: the pages dropped; locked again,
       read from the file, the data right
     4 the frame pipeline: a frame's stream REFs the locked range;
       mx_frame_unlock keeps the lock until the frame is done, then the
       range is unlocked */

#include "t.h"
#include <mx/dma.h>
#include <mx/frame.h>
#include <mx/gs.h>
#include <mx/loader.h>
#include <mx/sif.h>
#include <mx/vif.h>
#include <mx/vm.h>
#include <mx/vu.h>

#define PAGE MX_VM_PAGE
#define FILE_BYTES (4 * PAGE)
#define HEAP_PAGES 40

const struct mx_vm_config mx_vm_config = {.swap = "host:vm16swap.bin", .frames = 16 * PAGE};

static u32 sbuf[64] MX_ALIGNED(64); /* VM: the program's own bss */
static u64 tags[4][2] MX_CORE_DATA MX_ALIGNED(64);
static u8 slots[64 * 1024] MX_CORE_DATA MX_ALIGNED(64);

static u32 E(u32 i) { return i * 2654435761u ^ 0x1E1E1E1Eu; }

static void stats(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_dbg_word(4, s.pinned);
  mx_dbg_word(5, s.drops);
  mx_dbg_word(6, s.file_ins);
  mx_dbg_word(7, s.swap_outs);
}

static void step(u32 n, const char *what) {
  stats();
  mx_dbg_step(n);
  mx_printf("vm16: %u %s\n", n, what);
}

static const volatile u32 *vu1 = (const volatile u32 *)MX_VU1_DATA;

/* 16 quadwords from p into VU1 data memory at 0, through one REF */
static void send(const void *p) {
  u64 *c = &tags[0][0];
  c = mx_e_ref(c, p, 16, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, 0, 16, 0));
  c = mx_e_end(c, 0, 0, 0);
  mx_dcache_writeback(tags, sizeof tags);
  mx_dma_chain(MX_CH_VIF1, tags, 1);
  mx_dma_wait(MX_CH_VIF1);
}

static u32 vu1_wrong(const u32 *want) {
  u32 w = 0;
  for (u32 i = 0; i < 64; i++)
    w += vu1[i] != want[i];
  return w;
}

/* the file's words from byte offset o */
static u32 vu1_wrong_file(u32 o) {
  u32 w = 0;
  for (u32 i = 0; i < 64; i++)
    w += vu1[i] != E(o / 4 + i);
  return w;
}

static void thrash(u8 *heap, u32 round) {
  for (u32 k = 0; k < HEAP_PAGES; k++)
    ((volatile u32 *)(heap + k * PAGE))[round] = round + k;
  stats();
}

int main(int argc, char **argv) {
  (void)argc, (void)argv;
  mx_gs_init(640, 224);
  mx_dbg_screen();
  mx_dbg_heartbeat(1);
  CHECK_EQ(mx_loader_mxload(), 1);
  struct mx_vm_stats s0, s1;

  step(1, "a static VM buffer locked");
  CHECK(mx_is_vm(sbuf));
  for (u32 i = 0; i < 64; i++)
    sbuf[i] = E(i) ^ 0xFFu;
  CHECK_EQ(mx_vm_lock(sbuf, sizeof sbuf), 0);
  mx_dbg_word(2, mx_paddr_of(sbuf).v);
  CHECK_EQ(mx_paddr_of(sbuf).v, mx_vm_first_piece(sbuf, 4).addr.v);
  mx_dcache_writeback(sbuf, sizeof sbuf);
  send(sbuf);
  CHECK_EQ(vu1_wrong(sbuf), 0);
  mx_vm_unlock(sbuf, sizeof sbuf);

  step(2, "a mapped file, 3 pages locked");
  u8 *heap = mx_sbrk(HEAP_PAGES * PAGE);
  CHECK(heap != (void *)-1);
  if (heap == (void *)-1)
    return t_done("vm16");
  for (u32 i = 0; i < FILE_BYTES / 4; i++)
    ((u32 *)heap)[i] = E(i);
  int fd = mx_fio_open("host:vm16.bin", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  CHECK_EQ(mx_fio_write(fd, heap, FILE_BYTES), 0);
  mx_fio_close(fd);
  const u8 *m = mx_vm_map_file("host:vm16.bin", 0);
  CHECK(m != 0);
  if (!m)
    return t_done("vm16");
  const u8 *r = m + PAGE - 512; /* 3 pages: the end of 0, 1, the start of 2 */
  const u32 rbytes = PAGE + 1024;
  CHECK_EQ(mx_vm_lock(r, rbytes), 0);
  u32 pa = mx_paddr_of(r).v;
  mx_dbg_word(2, pa);
  u32 offs[3] = {0, PAGE / 2, rbytes - 256};
  for (u32 k = 0; k < 3; k++) {
    send(r + offs[k]);
    CHECK_EQ(vu1_wrong_file(PAGE - 512 + offs[k]), 0);
  }
  thrash(heap, 1);
  CHECK_EQ(mx_paddr_of(r).v, pa);
  send(r + PAGE / 2);
  CHECK_EQ(vu1_wrong_file(PAGE - 512 + PAGE / 2), 0);

  step(3, "unlocked, dropped, locked again");
  mx_vm_unlock(r, rbytes);
  mx_vm_stats(&s0);
  thrash(heap, 2);
  mx_vm_stats(&s1);
  mx_printf("vm16: %u drops, %u swap outs while unlocked\n", s1.drops - s0.drops,
            s1.swap_outs - s0.swap_outs);
  CHECK(s1.drops - s0.drops >= 3); /* the mapped pages: never written out */
  mx_vm_stats(&s0);
  CHECK_EQ(mx_vm_lock(r, rbytes), 0);
  mx_vm_stats(&s1);
  CHECK(s1.file_ins - s0.file_ins >= 3);
  send(r + offs[2]);
  CHECK_EQ(vu1_wrong_file(PAGE - 512 + offs[2]), 0);

  step(4, "mx_frame_unlock");
  mx_vu_reset(2);
  mx_vu_forget(1);
  CHECK_EQ(mx_frame_init_chain(slots, sizeof slots, (void *)0x70002000, 0x2000), 0);
  mx_frame_begin(0x10, 0x10, 0x10);
  u32 fr = mx_frame_number();
  u64 *c = mx_frame_reserve(1);
  c = mx_e_ref(c, r, 16, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, 0, 16, 0));
  mx_frame_commit(c);
  mx_frame_unlock(r, rbytes);
  mx_vm_stats(&s0);
  CHECK(s0.pinned >= 3); /* still locked: the frame reads it */
  mx_frame_end();
  u32 t0 = mx_count();
  while (mx_frame_done() <= fr && mx_count() - t0 < 300000000u)
    ;
  CHECK(mx_frame_done() > fr);
  CHECK_EQ(vu1_wrong_file(PAGE - 512), 0);
  mx_frame_begin(0x10, 0x10, 0x10); /* unlocks what the done frames read */
  mx_vm_stats(&s1);
  mx_printf("vm16: pinned %u while the frame was in flight, %u after it\n", s0.pinned,
            s1.pinned);
  CHECK_EQ(s1.pinned, 0);
  mx_frame_end();
  t0 = mx_count();
  while (mx_frame_done() <= fr + 1 && mx_count() - t0 < 300000000u)
    ;
  mx_frame_stop();
  mx_vm_unmap_file(m);

  stats();
  mx_dbg_heartbeat(0);
  return t_done("vm16");
}
