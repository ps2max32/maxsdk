/* VM step 14: files mapped read-only into the file window (mx_vm_map_file,
   0x4000_0000), paged from the file, sharing 8 frames (256 KiB) with VM,
   no swap file. Under a minute; at the end the screen is bright green with
   900D0000 in slot 0, or bright red with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: the word or round, slot 2: a
   value checked. Slots 4-7: pages read from files, drops, swap outs,
   faults. Heartbeat in slots 30 and 31.
     1 host:vm14a.bin written (300 KiB: 9 pages and a part), VM on
     2 mapped: in the file window; every word right, twice (more than the
       frames: read from the file again, dropped, never written out)
     3 200 random reads
     4 a pin for DMA (MX_VM_PIN_DMA) and a VIF1 transfer of mapped data; a
       pin for writing refused
     5 mx_fio_write from the mapping to host:vm14b.bin, read back
     6 a second file mapped after the first; the second unmapped and mapped
       again at the same place; the first unmapped, then both gone

   vm14_mapfile.elf store: a store into the mapping, which ends the program
   with a report (status 255). */

#include "t.h"
#include <mx/dma.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/vif.h>
#include <mx/vm.h>
#include <mx/vu.h>

#define PAGE MX_VM_PAGE
#define BYTES (300u * 1024)
#define NW (BYTES / 4)

static const struct mx_vm_config cfg = {.frames = 8 * PAGE};

static u32 E(u32 i) { return i * 2654435761u ^ 0xA5A5A5A5u; }

static u32 buf[PAGE / 4] MX_ALIGNED(64);
static u64 tags[4][2] MX_ALIGNED(64);

static void stats(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_dbg_word(4, s.file_ins);
  mx_dbg_word(5, s.drops);
  mx_dbg_word(6, s.swap_outs);
  mx_dbg_word(7, s.faults);
}

static void step(u32 n, const char *what) {
  stats();
  mx_dbg_step(n);
  mx_printf("vm14: %u %s\n", n, what);
}

static int write_file(const char *path) {
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0)
    return -1;
  for (u32 at = 0; at < BYTES; at += PAGE) {
    u32 n = BYTES - at < PAGE ? BYTES - at : PAGE;
    for (u32 i = 0; i < n / 4; i++)
      buf[i] = E(at / 4 + i);
    if (mx_fio_write(fd, buf, n) != 0)
      return -1;
  }
  /* CLOSE answers with the descriptor under ps2link, 0 on PCSX2 (docs/bios.md) */
  return mx_fio_close(fd) < 0 ? -1 : 0;
}

static u32 wrong_all(const u32 *m) {
  u32 w = 0;
  for (u32 i = 0; i < NW; i++) {
    if (!(i & 4095)) {
      mx_dbg_word(1, i);
      stats();
    }
    w += m[i] != E(i);
  }
  return w;
}

int main(int argc, char **argv) {
  int store = argc >= 2 && argv[1][0] == 's';
  mx_dbg_screen();
  mx_dbg_heartbeat(1);

  step(1, "a file written, VM on");
  CHECK_EQ(write_file("host:vm14a.bin"), 0);
  CHECK_EQ(mx_vm_init(&cfg), 0);

  step(2, "mapped, read twice");
  u32 size = 0;
  const u32 *m = mx_vm_map_file("host:vm14a.bin", &size);
  CHECK(m != 0);
  if (!m)
    return t_done("vm14");
  mx_printf("vm14: mapped at %p, %u bytes\n", (const void *)m, size);
  CHECK_EQ((uintptr_t)m, MX_VM_MAP_BASE);
  CHECK_EQ(size, BYTES);
  if (store) {
    mx_printf("vm14: a store into the mapping (ends the program)\n");
    ((volatile u32 *)m)[3] = 0;
    mx_printf("vm14: the store went through\n");
    return t_done("vm14");
  }
  CHECK_EQ(wrong_all(m), 0);
  struct mx_vm_stats s1, s2;
  mx_vm_stats(&s1);
  CHECK_EQ(wrong_all(m), 0);
  mx_vm_stats(&s2);
  mx_printf("vm14: %u pages from the file, %u drops, %u swap outs\n", s2.file_ins, s2.drops,
            s2.swap_outs);
  CHECK(s1.file_ins >= 10);
  CHECK(s2.file_ins - s1.file_ins >= 2); /* 10 pages, 8 frames: some again */
  CHECK(s2.drops >= 2);
  CHECK_EQ(s2.swap_outs, 0);

  step(3, "200 random reads");
  u32 x = 5, bad = 0;
  for (u32 n = 0; n < 200; n++) {
    x = x * 1103515245u + 12345u;
    u32 i = (x >> 8) % NW;
    mx_dbg_word(1, n);
    bad += m[i] != E(i);
  }
  CHECK_EQ(bad, 0);

  step(4, "DMA from the mapping");
  const u8 *p = (const u8 *)m + 3 * PAGE - 128;
  CHECK_EQ(mx_vm_pin(p, 256, MX_VM_PIN_WRITE), -1);
  CHECK_EQ(mx_vm_pin(p, 256, MX_VM_PIN_DMA), 0);
  struct mx_vm_piece pc[2];
  CHECK_EQ(mx_vm_pieces(p, 256, pc, 2), 1);
  u64 *c = &tags[0][0];
  c = mx_e_ref(c, p, 16, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, 0, 16, 0));
  c = mx_e_end(c, 0, 0, 0);
  mx_dcache_writeback(tags, sizeof tags);
  mx_dma_chain(MX_CH_VIF1, tags, 1);
  CHECK_EQ(mx_dma_wait(MX_CH_VIF1), 0);
  const volatile u32 *vu = (const volatile u32 *)MX_VU1_DATA;
  u32 w0 = (3 * PAGE - 128) / 4;
  bad = 0;
  for (u32 i = 0; i < 64; i++)
    bad += vu[i] != E(w0 + i);
  mx_dbg_word(2, vu[0]);
  CHECK_EQ(bad, 0);
  mx_vm_unpin(p, 256);

  step(5, "written out from the mapping");
  int fd = mx_fio_open("host:vm14b.bin", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  CHECK_EQ(mx_fio_write(fd, (const u8 *)m + PAGE + 100, 50000), 0);
  mx_fio_close(fd);
  fd = mx_fio_open("host:vm14b.bin", MX_FIO_RDONLY);
  CHECK(fd >= 0);
  static u8 back[50000] MX_ALIGNED(64);
  CHECK_EQ(mx_fio_read(fd, back, sizeof back), (int)sizeof back);
  mx_fio_close(fd);
  bad = 0;
  for (u32 i = 0; i < sizeof back; i++)
    bad += back[i] != ((const u8 *)m)[PAGE + 100 + i];
  CHECK_EQ(bad, 0);

  step(6, "two files, unmapped and mapped again");
  u32 size2 = 0;
  const u8 *m2 = mx_vm_map_file("host:vm14b.bin", &size2);
  CHECK(m2 != 0);
  CHECK_EQ(size2, 50000);
  CHECK_EQ((uintptr_t)m2, MX_VM_MAP_BASE + 10 * PAGE);
  if (m2) {
    CHECK_EQ(memcmp(m2, back, sizeof back), 0);
    mx_vm_unmap_file(m2);
  }
  const u8 *m3 = mx_vm_map_file("host:vm14b.bin", 0);
  CHECK_EQ((uintptr_t)m3, (uintptr_t)m2);
  if (m3)
    CHECK_EQ(m3[49999], back[49999]);
  mx_vm_unmap_file(m);
  mx_vm_unmap_file(m3);
  const u32 *m4 = mx_vm_map_file("host:vm14a.bin", 0);
  CHECK_EQ((uintptr_t)m4, MX_VM_MAP_BASE);
  if (m4)
    CHECK_EQ(m4[NW - 1], E(NW - 1));
  mx_vm_unmap_file(m4);

  stats();
  mx_dbg_heartbeat(0);
  return t_done("vm14");
}
