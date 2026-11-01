/* VM step 4: from an exception to thread code and back, as a page fault
   that needs the swap file goes, but without the TLB: a break instruction
   enters the VM's exception vector (src/vm/tlb.S, at EXL = 1), which only
   saves the registers and sends the thread on to the slow path in thread
   code (mx__vm_slow) on its own stack; that calls on_break() here, which
   may do host: I/O, then resumes after the break by the VM's syscall.
   About 20 s; at the end the screen is bright green with 900D0000 in slot
   0, or bright red with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: breaks handled, slot 2: the
   break code last seen, slot 3: Status (low 20 bits) after the last
   resume, slot 4: file I/O rounds done in on_break. Heartbeat in slots 30
   and 31.
     1 one break, nothing done in on_break: back after it, Status as before
     2 every general register, an upper half and HI/LO kept across a break
     3 200 breaks, each writing and reading back 32 KiB of host: file in
       on_break
     4 the same from a second thread (100), while the main thread waits
     5 50 breaks while a timer interrupt runs every 100 ms */

#include "t.h"
#include <mx/sif.h>

void mx__vm_break_init(void (*fn)(u32 code));

#define SLOT 32768u

static u8 buf[SLOT] MX_ALIGNED(64), back[SLOT] MX_ALIGNED(64);
static int fd = -1;
static volatile u32 nbreak, last_code, rounds, bad, do_io;

static u8 byte(u32 seed, u32 i) { return (u8)((i * 2654435761u ^ seed * 40503u) >> 13); }

static void io_round(void) {
  u32 k = rounds, at = (k & 7) * SLOT;
  for (u32 i = 0; i < SLOT; i++)
    buf[i] = byte(k, i);
  int ok = mx_fio_lseek(fd, (s32)at, MX_FIO_SEEK_SET) == (int)at &&
           mx_fio_write(fd, buf, SLOT) == 0 &&
           mx_fio_lseek(fd, (s32)at, MX_FIO_SEEK_SET) == (int)at &&
           mx_fio_read(fd, back, SLOT) == (int)SLOT;
  for (u32 i = 0; ok && i < SLOT; i++)
    ok = back[i] == byte(k, i);
  if (!ok) {
    bad++;
    mx_printf("vm4: I/O round %u failed\n", k);
  }
  rounds = k + 1;
  mx_dbg_word(4, rounds);
}

/* thread code, on the thread that broke */
static void on_break(u32 code) {
  nbreak++;
  last_code = code;
  mx_dbg_word(1, nbreak);
  mx_dbg_word(2, code);
  if (do_io)
    io_round();
}

static void brk(void) {
  __asm__ volatile("break 0x55" ::: "memory");
  mx_dbg_word(3, mx_status() & 0xFFFFF);
}

/* step 2: registers loaded with known values, a break, then stored */
static u32 out[32];
static void regs(void) {
  register u32 *base __asm__("$23") = out;
  __asm__ volatile(".set noat\n\t"
                   "li $1, 0x1001\n\tli $2, 0x2002\n\tli $3, 0x3003\n\tli $4, 0x4004\n\t"
                   "li $5, 0x5005\n\tli $6, 0x6006\n\tli $7, 0x7007\n\tli $8, 0x8008\n\t"
                   "li $9, 0x9009\n\tli $10, 0x0a0a\n\tli $11, 0x0b0b\n\tli $12, 0x0c0c\n\t"
                   "li $13, 0x0d0d\n\tli $14, 0x0e0e\n\tli $15, 0x0f0f\n\tli $16, 0x1010\n\t"
                   "li $17, 0x1111\n\tli $18, 0x1212\n\tli $19, 0x1313\n\tli $20, 0x1414\n\t"
                   "li $21, 0x1515\n\tli $22, 0x1616\n\tli $24, 0x1818\n\tli $25, 0x1919\n\t"
                   "mthi $20\n\tmtlo $21\n\t"
                   "pcpyld $8, $9, $8\n\t" /* $8 upper = 0x9009 */
                   "break 0x66\n\t"
                   "sw $1, 4($23)\n\tsw $2, 8($23)\n\tsw $3, 12($23)\n\tsw $4, 16($23)\n\t"
                   "sw $5, 20($23)\n\tsw $6, 24($23)\n\tsw $7, 28($23)\n\tsw $8, 32($23)\n\t"
                   "sw $9, 36($23)\n\tsw $10, 40($23)\n\tsw $11, 44($23)\n\tsw $12, 48($23)\n\t"
                   "sw $13, 52($23)\n\tsw $14, 56($23)\n\tsw $15, 60($23)\n\tsw $16, 64($23)\n\t"
                   "sw $17, 68($23)\n\tsw $18, 72($23)\n\tsw $19, 76($23)\n\tsw $20, 80($23)\n\t"
                   "sw $21, 84($23)\n\tsw $22, 88($23)\n\tsw $24, 96($23)\n\tsw $25, 100($23)\n\t"
                   "mfhi $1\n\tsw $1, 0($23)\n\tmflo $1\n\tsw $1, 92($23)\n\t"
                   "pcpyud $1, $8, $0\n\tsw $1, 108($23)\n\t"
                   ".set at"
                   :
                   : "r"(base)
                   : "$1", "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9", "$10", "$11", "$12",
                     "$13", "$14", "$15", "$16", "$17", "$18", "$19", "$20", "$21", "$22", "$24",
                     "$25", "hi", "lo", "memory");
}

static void step(u32 n, const char *what) {
  mx_dbg_step(n);
  mx_printf("vm4: %u %s\n", n, what);
}

static u8 tstack[16384] MX_ALIGNED(16);
static void second(void *arg) {
  (void)arg;
  for (u32 i = 0; i < 100; i++)
    brk();
}

static volatile u32 ticks;
static void tick(void *arg) {
  (void)arg;
  ticks++;
}

int main(void) {
  mx_dbg_screen();
  mx_dbg_heartbeat(1);
  mx__vm_break_init(on_break);

  step(1, "one break");
  u32 st0 = mx_status();
  brk();
  CHECK_EQ(nbreak, 1);
  CHECK_EQ(last_code, 0x55u << 10);
  CHECK_EQ(mx_status(), st0);
  mx_printf("vm4: break code %x, Status %08x -> %08x\n", last_code, st0, mx_status());

  step(2, "registers");
  regs();
  static const u32 want[32] = {0x1414, 0x1001, 0x2002, 0x3003, 0x4004, 0x5005, 0x6006,
                               0x7007, 0x8008, 0x9009, 0x0a0a, 0x0b0b, 0x0c0c, 0x0d0d,
                               0x0e0e, 0x0f0f, 0x1010, 0x1111, 0x1212, 0x1313, 0x1414,
                               0x1515, 0x1616, 0x1515, 0x1818, 0x1919, 0,      0x9009};
  for (u32 i = 0; i < 28; i++)
    if (i != 26 && out[i] != want[i]) {
      mx_printf("FAIL vm4: register slot %u = %x, expected %x\n", i, out[i], want[i]);
      t_fail++;
    }
  CHECK_EQ(nbreak, 2);

  step(3, "200 breaks with I/O");
  fd = mx_fio_open("host:vm4.bin", MX_FIO_RDONLY | MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  if (fd < 0)
    return t_done("vm4");
  do_io = 1;
  for (u32 i = 0; i < 200; i++)
    brk();
  CHECK_EQ(rounds, 200);
  CHECK_EQ(bad, 0);
  CHECK_EQ(mx_status(), st0);

  step(4, "a second thread");
  mx_thread_set_prio(mx_thread_self(), 64);
  int t = mx_thread_start(second, 0, tstack, sizeof tstack, 64);
  CHECK(t >= 0);
  mx_thread_join(t);
  CHECK_EQ(rounds, 300);
  CHECK_EQ(bad, 0);

  step(5, "with a timer");
  mx_timer_periodic(1, 57600, tick, 0);
  for (u32 i = 0; i < 50; i++)
    brk();
  mx_timer_periodic(1, 0, 0, 0);
  CHECK_EQ(rounds, 350);
  CHECK_EQ(bad, 0);
  CHECK(ticks > 0);
  CHECK_EQ(mx_status(), st0);
  mx_printf("vm4: %u breaks, %u I/O rounds, %u bad, %u ticks\n", nbreak, rounds, bad, ticks);
  mx_fio_close(fd);
  mx_dbg_heartbeat(0);
  return t_done("vm4");
}
