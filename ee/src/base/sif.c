/* Minimal SIF command and RPC client, enough to write to the IOP file
   server's tty. The packet layouts and command IDs are the IOP SIF protocol
   (SIF CMD/RPC, as implemented by the IOP ROM modules sifcmd/sifman). */

#include <mx/bios.h>
#include <mx/bits.h>
#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/hw/formats.h>
#include <mx/hw/regs.h>
#include <mx/iop.h>
#include <mx/sif.h>
#include <mx/timer.h>
#include <mx/string.h>
#include <mx/vm.h>
#include "../../../common/include/mxio.h"

/* System command IDs */
/* libmxk (MX_KERNEL) under mx-kernel: the kernel has the one client of the
   IOP's SIF, and the functions it offers go there (docs/kernel.md). */
#ifdef MX_KERNEL
#include <mx/kernel.h>
#define KERNEL(call) \
  if (mx__kernel)    \
  return mx__kernel->call
/* The kernel knows core addresses only: VM by its frame, written back here. */
static const void *core_of(const void *p, u32 n) {
  if (!mx_is_vm(p))
    return p;
  mx_dcache_writeback(p, n);
  return (const void *)(uintptr_t)mx_paddr_range(p, n).v;
}
#else
#define KERNEL(call)
#endif

#define CMD_CHANGE_SADDR 0x80000000u
#define CMD_SET_SREG 0x80000001u
#define CMD_INIT_CMD 0x80000002u
#define CMD_RESET 0x80000003u
#define CMD_RPC_END 0x80000008u
#define CMD_RPC_BIND 0x80000009u
#define CMD_RPC_CALL 0x8000000Au
#define CMD_RPC_RDATA 0x8000000Cu

/* SIF registers (sceSifGetReg/SetReg) */
#define REG_SUBADDR 2
#define REG_SMFLAG 4
#define SYSREG_SUBADDR 0x80000000u
#define SYSREG_RPCINIT 0x80000002u
#define SMFLAG_SIFINIT 0x10000
#define SMFLAG_CMDINIT 0x20000
#define SMFLAG_BOOTEND 0x40000

/* sceSifSetDma attributes */
#define DMA_INT_O 0x4
#define DMA_ERT 0x40

#define MXIO_READ_MAX (256u * 1024u)
#define FILEIO_SID 0x80000001u
#define FILEIO_OPEN 0
#define FILEIO_CLOSE 1
#define FILEIO_READ 2
#define FILEIO_WRITE 3
#define FILEIO_LSEEK 4
#define FILEIO_REMOVE 6
#define FILEIO_MKDIR 7
#define FILEIO_RMDIR 8
#define FILEIO_DOPEN 9
#define FILEIO_DCLOSE 10
#define FILEIO_DREAD 11

#define RPC_PKT_SIZE 64
#define NPKT 16

struct cmd_hdr {
  u32 size; /* psize:8, dsize:24 */
  u32 dest;
  u32 cid;
  u32 opt;
};

struct sif_dma {
  u32 src, dest;
  s32 size, attr;
};

/* Received packets land here by DMA from the IOP. Only touched through the
   uncached alias. */
static u8 rx_buf[128] MX_DMA_LINES;
/* Outgoing RPC packets, and replies to IOP requests (a small ring). */
static u8 pkt_pool[NPKT][RPC_PKT_SIZE] MX_DMA_LINES;
static u8 reply_ring[NPKT][RPC_PKT_SIZE] MX_DMA_LINES;
static volatile u32 pkt_busy;
static u32 reply_idx;
static u32 rpc_pid = 1;

static volatile u32 iop_buf;
static volatile s32 sregs[32];
static int sif_state; /* 0 = not tried, 1 = ready, -1 = no IOP */
static int sif0_handler_id;
/* the IOP's boots by mx_iop_reset: bindings of an earlier one are gone */
static u32 sif_epoch;

static volatile u32 *uw(void *p) { return (volatile u32 *)MX_UNCACHED(p); }

static u32 send_cmd(int intr, u32 cid, void *pkt, u32 psize, const void *src, u32 dest,
                    u32 dsize) {
  volatile u32 *h = uw(pkt);
  struct sif_dma t[2];
  int n = 0;
  h[0] = (psize & 0xFF) | (dsize << 8);
  h[1] = dsize ? dest : 0;
  h[2] = cid;
  if (dsize) {
    t[n].src = mx_paddr_range(src, dsize).v;
    t[n].dest = dest;
    t[n].size = (s32)dsize;
    t[n].attr = 0;
    n++;
  }
  t[n].src = (u32)(uintptr_t)MX_UNCACHED(pkt);
  t[n].dest = iop_buf;
  t[n].size = (s32)psize;
  t[n].attr = DMA_ERT | DMA_INT_O;
  n++;
  mx_sync();
  /* 0: the kernel's SIF DMA queue is full. A thread waits for room; an
     interrupt handler tries a while and then gives up (returns 0): waiting
     there with interrupts off hung the PS2, and the caller keeps the
     command for later (an RDATA answer, defer_rdata). */
  u32 id;
  if (intr) {
    for (int i = 0; i < 2000; i++)
      if ((id = mx_bios_isif_set_dma(t, n)))
        return id;
    return 0;
  }
  while (!(id = mx_bios_sif_set_dma(t, n)))
    ;
  return id;
}

/* RDATA requests (the IOP takes EE memory: the data of a write) are
   answered by the thread that waits for the call (wait_answer), in order,
   not in the interrupt handler: the kernel's SIF DMA queue is used from
   threads only (answers sent from the handler jammed the queue on the PS2:
   docs/vm.md). Dropping one leaves the IOP waiting for good. */
static struct {
  u32 pkt_addr, client, src, dest, size;
} deferred[NPKT];
static volatile u32 ndeferred;

static void send_rdata(int intr, const u32 *w) {
  u8 *r = reply_ring[reply_idx++ % NPKT];
  volatile u32 *rp = uw(r);
  rp[5] = w[0];
  rp[7] = w[1];
  rp[8] = CMD_RPC_RDATA;
  if (!send_cmd(intr, CMD_RPC_END, r, RPC_PKT_SIZE, (const void *)(uintptr_t)w[2], w[3], w[4]) &&
      ndeferred < NPKT) {
    for (int i = 0; i < 5; i++)
      (&deferred[ndeferred].pkt_addr)[i] = w[i];
    ndeferred++;
  }
}

/* Threads sleeping in wait_answer, woken by their answer (RPC_END), by a
   deferred RDATA (any of them sends it), and at each vblank start (to see
   whether they give up). */
#define NSLEEP 8
static struct mx_sif_client *volatile sleepers[NSLEEP];

static void wake_sleepers(void) {
  for (int i = 0; i < NSLEEP; i++) {
    struct mx_sif_client *c = sleepers[i];
    if (c && c->sleeping)
      mx_bios_isignal_sema(c->sema - 1);
  }
}

static void send_deferred(void) {
  while (ndeferred) {
    u32 w[5];
    int was = mx_di();
    for (int i = 0; i < 5; i++)
      w[i] = (&deferred[0].pkt_addr)[i];
    for (u32 k = 1; k < ndeferred; k++)
      deferred[k - 1] = deferred[k];
    ndeferred--;
    mx_irq_restore(was);
    send_rdata(0, w);
  }
}

static void *rpc_alloc(void) {
  for (;;) {
    int was = mx_di();
    for (int i = 0; i < NPKT; i++) {
      if (!(pkt_busy & (1u << i))) {
        pkt_busy |= 1u << i;
        mx_irq_restore(was);
        volatile u32 *p = uw(pkt_pool[i]);
        for (int k = 0; k < RPC_PKT_SIZE / 4; k++)
          p[k] = 0;
        u32 pid = rpc_pid++;
        p[4] = ((u32)i << 16) | 0x05; /* rec_id: index, flags */
        p[5] = (u32)(uintptr_t)MX_UNCACHED(pkt_pool[i]);
        p[6] = pid;
        return pkt_pool[i];
      }
    }
    mx_irq_restore(was);
  }
}

static void rpc_free(void *p) {
  u32 i = (u32)((u8 *)p - pkt_pool[0]) / RPC_PKT_SIZE;
  if (i < NPKT)
    pkt_busy &= ~(1u << i);
}

/* EE servers for the IOP's binds and calls (mx_sif_serve). A bind packet:
   pkt_addr (word 5), client (7), sid (8); a call: pkt_addr, client, rpc
   number (8), its data was sent to our buffer. The answer is an RPC_END with
   the packet address, the client and the command (8), for a bind also the
   server, its buffer and cbuffer (9-11): the IOP's sifrpc takes them into the
   client and frees its packet. */
#define NSERVER 4

static struct server {
  u32 sid;
  mx_sif_server_fn fn;
  void *arg;
} servers[NSERVER];
static u8 server_buf[NSERVER][MX_SIF_SERVER_BUF] MX_DMA_LINES;

static void serve(const u32 *pkt) {
  u8 *r = reply_ring[reply_idx++ % NPKT];
  volatile u32 *rp = uw(r);
  for (int i = 4; i < 12; i++)
    rp[i] = 0;
  rp[5] = pkt[5];
  rp[7] = pkt[7];
  rp[8] = pkt[2];
  if (pkt[2] == CMD_RPC_BIND) {
    for (int i = 0; i < NSERVER; i++) {
      if (servers[i].fn && servers[i].sid == pkt[8]) {
        rp[9] = (u32)(uintptr_t)&servers[i];
        rp[10] = (u32)(uintptr_t)server_buf[i];
      }
    }
  } else {
    struct server *s = (struct server *)(uintptr_t)pkt[13];
    if (s >= servers && s < servers + NSERVER && s->fn)
      s->fn((int)pkt[8], s->arg);
  }
  send_cmd(1, CMD_RPC_END, r, RPC_PKT_SIZE, 0, 0, 0);
}

/* SIF0 (IOP -> EE) DMA end: one command packet is in rx_buf. */
static int sif0_handler(int ch, void *arg, void *addr) {
  (void)ch, (void)arg, (void)addr;
  volatile u32 *rx = uw(rx_buf);
  u32 psize = rx[0] & 0xFF;
  if (!psize)
    return 0;
  u32 pkt[28];
  u32 words = (psize + 3) / 4;
  if (words > 28)
    words = 28;
  for (u32 i = 0; i < words; i++)
    pkt[i] = rx[i];
  rx[0] = 0;
  /* the clear (an uncached store, maybe still in the write buffer) must be
     done before SIF0 takes the next packet: landing after it, it would
     wipe that packet's size, and the packet (say a READ's answer) is lost */
  mx_sync();
  mx_bios_isif_set_dchain();

  switch (pkt[2]) {
  case CMD_CHANGE_SADDR:
    iop_buf = pkt[4];
    break;
  case CMD_SET_SREG:
    if (pkt[4] < 32)
      sregs[pkt[4]] = (s32)pkt[5];
    break;
  case CMD_RPC_END: {
    struct mx_sif_client *c = (struct mx_sif_client *)(uintptr_t)pkt[7];
    if (pkt[8] == CMD_RPC_BIND) {
      c->server = pkt[9];
      c->buf = pkt[10];
      c->cbuf = pkt[11];
    }
    /* the reply's packet address is the IOP's (a call's answer does not
       name ours): free ours from the client. A call never gives up waiting
       (wait_answer), so this is the answer to the client's current one */
    rpc_free(c->pkt);
    c->done = 1;
    if (c->sleeping)
      mx_bios_isignal_sema(c->sema - 1);
    break;
  }
  case CMD_RPC_RDATA: {
    /* The IOP wants EE memory (e.g. the data of a write): send it back with
       an RPC_END (pkt_addr, client, source, destination, size). */
    u32 w[5] = {pkt[5], pkt[7], pkt[8], pkt[9], pkt[10]};
    /* answered by the thread waiting for the call that the IOP takes the
       data for (wait_answer): the kernel's SIF DMA queue only from threads */
    if (ndeferred < NPKT) {
      for (int i = 0; i < 5; i++)
        (&deferred[ndeferred].pkt_addr)[i] = w[i];
      ndeferred++;
      wake_sleepers();
    } else
      send_rdata(1, w);
    break;
  }
  case CMD_RPC_BIND:
  case CMD_RPC_CALL:
    serve(pkt);
    break;
  default:
    break;
  }
  return 0;
}

int mx_sif_serve(u32 sid, mx_sif_server_fn fn, void *arg) {
  if (mx__need_core((const void *)fn, "mx_sif_serve: the server") ||
      mx__need_core(arg, "mx_sif_serve: the server's argument"))
    return -1;
  KERNEL(sif_serve(sid, fn, arg));
  for (int i = 0; i < NSERVER; i++) {
    if (!servers[i].fn || servers[i].sid == sid) {
      servers[i].arg = arg;
      servers[i].sid = sid;
      servers[i].fn = fn;
      return 0;
    }
  }
  return -1;
}

int mx_sif_ready(void) {
#ifdef MX_KERNEL
  if (mx__kernel)
    return 1;
#endif
  return sif_state == 1;
}


void mx_sif_exit(void) {
#ifdef MX_KERNEL
  if (mx__kernel)
    return;
#endif
  if (sif0_handler_id > 0)
    mx_bios_remove_dmac_handler(MX_DMAC_SIF0, sif0_handler_id);
  sif0_handler_id = 0;
  sif_state = 0;
}

int mx_sif_init(void) {
#ifdef MX_KERNEL
  if (mx__kernel)
    return 0;
#endif
  if (sif_state)
    return sif_state > 0 ? 0 : -1;
  sif_state = -1;

  /* Clear a pending SIF0 end, and start receiving if nobody did. */
  if (MX_R32(MX_D_STAT) & (1u << MX_DMAC_SIF0))
    MX_R32(MX_D_STAT) = 1u << MX_DMAC_SIF0;
  if (!(MX_R32(MX_DMA_SIF0_CHCR) & MX_BIT(DMA_CHCR_STR)))
    mx_bios_sif_set_dchain();

  sif0_handler_id = mx_bios_add_dmac_handler(MX_DMAC_SIF0, sif0_handler, 0, 0);
  mx_bios_enable_dmac(MX_DMAC_SIF0);

  static u8 init_pkt[32] MX_DMA_LINES;
  iop_buf = (u32)mx_bios_sif_get_reg(SYSREG_SUBADDR);
  if (iop_buf) {
    /* The IOP side is already set up (by ps2link): tell it our receive
       buffer. */
    uw(init_pkt)[4] = (u32)(uintptr_t)MX_UNCACHED(rx_buf);
    send_cmd(0, CMD_CHANGE_SADDR, init_pkt, 32, 0, 0, 0);
  } else {
    u32 t0 = mx_count();
    while (!(mx_bios_sif_get_reg(REG_SMFLAG) & SMFLAG_CMDINIT))
      if (mx_count() - t0 > 300000000u)
        return -1;
    iop_buf = (u32)mx_bios_sif_get_reg(REG_SUBADDR);
    mx_bios_sif_set_reg(SYSREG_SUBADDR, (s32)iop_buf);
    uw(init_pkt)[3] = 0;
    uw(init_pkt)[4] = (u32)(uintptr_t)MX_UNCACHED(rx_buf);
    send_cmd(0, CMD_INIT_CMD, init_pkt, 32, 0, 0, 0);
  }

  if (!mx_bios_sif_get_reg(SYSREG_RPCINIT)) {
    static u8 rpc_init_pkt[16] MX_DMA_LINES;
    uw(rpc_init_pkt)[3] = 1;
    send_cmd(0, CMD_INIT_CMD, rpc_init_pkt, 16, 0, 0, 0);
    u32 t0 = mx_count();
    while (!sregs[0])
      if (mx_count() - t0 > 300000000u)
        return -1;
    mx_bios_sif_set_reg(SYSREG_RPCINIT, 1);
  }
  sif_state = 1;
  return 0;
}

/* Wait for the answer to c's command; after about 2 s give up (-1) if
   may_give_up (a bind: no server there), otherwise keep waiting: the IOP may
   still read the call's packet and write its answer, so neither may be
   reused before it answers (slow devices: a host: file over the network,
   USB). */
volatile u32 mx__sif_irq_lost;

int mx__irq_vbon_hook(void (*fn)(void));

/* Wait for c's answer asleep (other threads run meanwhile), in thread code
   with interrupts on (the BIOS's semaphores; IE, EIE, not EXL). Returns 1
   with *r the result (0, or -1: gave up after limit cycles from t0, unless
   limit is 0), or 0 if it cannot sleep here (the caller spins). */
int mx__sif_spin; /* probes: wait spinning, as before sleeping waits */
static int sleep_wait(struct mx_sif_client *c, u32 limit, u32 t0, int *r) {
  u32 st = mx_status();
  if ((st & 0x10003u) != 0x10001u || mx__sif_spin)
    return 0;
  if (!c->sema) {
    struct {
      s32 count, max_count, init_count, wait_threads;
      u32 attr, option;
    } p = {0, 1, 0, 0, 0, 0};
    int id = mx_bios_create_sema(&p);
    if (id < 0)
      return 0;
    c->sema = id + 1;
  }
  static int hooked;
  if (!hooked) {
    hooked = 1;
    mx__irq_vbon_hook(wake_sleepers);
  }
  int slot = -1, was = mx_di();
  for (int i = 0; i < NSLEEP && slot < 0; i++)
    if (!sleepers[i])
      sleepers[i] = c, slot = i;
  if (slot >= 0)
    c->sleeping = 1;
  mx_irq_restore(was);
  if (slot < 0)
    return 0;
  *r = 0;
  for (;;) {
    if (ndeferred)
      send_deferred();
    if (c->done)
      break;
    if (limit && mx_count() - t0 > limit) {
      *r = -1;
      break;
    }
    mx_bios_wait_sema(c->sema - 1);
  }
  was = mx_di();
  c->sleeping = 0;
  sleepers[slot] = 0;
  mx_irq_restore(was);
  mx_bios_poll_sema(c->sema - 1); /* a wake that came after the answer */
  return 1;
}

/* time (µs) and number of waits for the IOP, all, and of them for the file
   server (mx__sif_report) */
static u32 wait_us, waits, fio_us, fio_waits;
/* the counts so far: waits, their µs, the file server's waits, their µs */
void mx__sif_waits(u32 out[4]) {
#ifdef MX_KERNEL
  if (mx__kernel) {
    mx__kernel->sif_waits(out);
    return;
  }
#endif
  out[0] = waits, out[1] = wait_us, out[2] = fio_waits, out[3] = fio_us;
}

void mx__sif_report(void) {
  u32 w[4];
  mx__sif_waits(w);
  mx_printf("maxsdk: IOP waits %u in %u ms, of them the file server's %u in %u ms\n", w[0],
            w[1] / 1000, w[2], w[3] / 1000);
}

static int wait_answer1(struct mx_sif_client *c, int may_give_up);
static int wait_answer(struct mx_sif_client *c, int may_give_up) {
  u32 t0 = mx_count();
  int r = wait_answer1(c, may_give_up);
  wait_us += (mx_count() - t0) / (MX_EE_HZ / 1000000u);
  waits++;
  return r;
}

static int wait_answer1(struct mx_sif_client *c, int may_give_up) {
  u32 limit = may_give_up > 1 ? (u32)may_give_up : 600000000u;
  u32 t0 = mx_count();
  /* the answer comes by interrupt: waiting with them off hangs (it once
     did, in the VM's gather on the PS2). Slot 29 of the debug screen
     (<mx/debug.h>) shows the first time: 0x01000000 | Status. */
  u32 st = mx_status();
  if ((!(st & 1) || !(st >> 16 & 1)) && !mx__sif_irq_lost) {
    mx__sif_irq_lost = 1u << 24 | (st & 0xFFFFFFu);
    mx_dbg_word(29, mx__sif_irq_lost);
  }
  int r;
  if (sleep_wait(c, may_give_up ? limit : 0, t0, &r))
    return r;
  while (!c->done) {
    if (ndeferred)
      send_deferred();
    if (may_give_up && mx_count() - t0 > limit)
      return -1;
  }
  return 0;
}

static int wait_done(struct mx_sif_client *c) { return wait_answer(c, 0); }

int mx_sif_bind(struct mx_sif_client *c, u32 sid) { return mx_sif_bind_tries(c, sid, 1000); }

int mx_sif_bind_tries(struct mx_sif_client *c, u32 sid, int n) {
  KERNEL(sif_bind_tries(c, sid, n));
  for (int tries = 0; tries < n; tries++) {
    u8 *p = rpc_alloc();
    volatile u32 *w = uw(p);
    c->pkt = p;
    c->done = 0;
    c->server = 0;
    w[7] = (u32)(uintptr_t)c;
    w[8] = sid;
    send_cmd(0, CMD_RPC_BIND, p, RPC_PKT_SIZE, 0, 0, 0);
    if (wait_answer(c, 1) < 0)
      return -1;
    if (c->server) {
      c->epoch = sif_epoch;
      return 0;
    }
  }
  return -1;
}

int mx_sif_bound(const struct mx_sif_client *c) {
  KERNEL(sif_bound(c));
  return c->server && c->epoch == sif_epoch;
}

/* An RPC call; wait (cycles): 0 forever, otherwise give up after about
   that long and return -2. A call given up leaves its client to its late
   answer, if one comes (it frees the call's packet then): the caller must
   use another client struct for its next call (fio_call). */
int mx__sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                 u32 rsize, u32 wait);

static int sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                    u32 rsize, u32 wait) {
#ifdef MX_KERNEL
  if (mx__kernel)
    return mx__kernel->sif_call(c, fno, core_of(send, ssize), ssize,
                                rsize ? (void *)core_of(recv, rsize) : recv, rsize, wait);
#endif
  return mx__sif_call(c, fno, send, ssize, recv, rsize, wait);
}

int mx_sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                u32 rsize) {
  return sif_call(c, fno, send, ssize, recv, rsize, 0);
}

static void sif_send_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize,
                          void *recv, u32 rsize);

void mx_sif_call_async(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                       u32 rsize) {
#ifdef MX_KERNEL
  if (mx__kernel) {
    mx__kernel->sif_call_async(c, fno, core_of(send, ssize), ssize,
                               rsize ? (void *)core_of(recv, rsize) : recv, rsize);
    return;
  }
#endif
  sif_send_call(c, fno, send, ssize, recv, rsize);
}

int mx_sif_wait(struct mx_sif_client *c) {
  KERNEL(sif_wait(c));
  return wait_done(c);
}

/* The kernel's (mxlink's) limit for calls that would wait for ever, while it
   cleans up after a program: a server that does not answer then means an IOP
   to reset. Cycles, 0: none. Each call that gave up counts. */
u32 mx__sif_wait_limit, mx__sif_timeouts;

int mx__sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                 u32 rsize, u32 wait) {
  sif_send_call(c, fno, send, ssize, recv, rsize);
  if (!wait)
    wait = mx__sif_wait_limit;
  if (!wait)
    return wait_done(c);
  if (wait_answer(c, (int)wait) < 0) {
    mx__sif_timeouts++;
    return -2;
  }
  return 0;
}

static void sif_send_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize,
                          void *recv, u32 rsize) {
  u8 *p = rpc_alloc();
  volatile u32 *w = uw(p);
  c->pkt = p;
  c->done = 0;
  w[7] = (u32)(uintptr_t)c;
  w[8] = (u32)fno;
  w[9] = ssize;
  w[10] = rsize ? mx_paddr_range(recv, rsize).v : 0;
  w[11] = rsize;
  w[12] = 1; /* rmode: send RPC_END when done */
  w[13] = c->server;
  if (!mx_is_spr(send) && ((uintptr_t)send & 0xF0000000u) != 0x20000000u)
    mx_dcache_writeback(send, ssize);
  send_cmd(0, CMD_RPC_CALL, p, RPC_PKT_SIZE, send, c->buf, ssize);
}

/* ---- IOP memory ----------------------------------------------------------- */

/* RDATA from the EE (sceSifGetOtherData): the IOP sends size bytes from src
   by SIF DMA to dest and answers with an RPC_END (the client in word 7). The
   reads are of 16-byte aligned blocks into rd_buf. */
static struct mx_sif_client rdc;
static u8 rd_buf[512] MX_DMA_LINES;

int mx_iop_read(u32 iop, void *dst, u32 size) {
  KERNEL(iop_read(iop, dst, size));
  if (!mx_sif_ready() && mx_sif_init() < 0)
    return -1;
  u8 *d = dst;
  while (size) {
    u32 base = iop & ~15u, off = iop - base;
    u32 n = size < sizeof rd_buf - off ? size : sizeof rd_buf - off;
    u32 len = (off + n + 15) & ~15u;
    mx_dcache_writeback(rd_buf, sizeof rd_buf); /* no dirty lines for the DMA to meet */
    mx_dcache_invalidate(rd_buf, sizeof rd_buf);
    u8 *p = rpc_alloc();
    volatile u32 *w = uw(p);
    rdc.pkt = p;
    rdc.done = 0;
    w[7] = (u32)(uintptr_t)&rdc;
    w[8] = base;
    w[9] = (u32)(uintptr_t)rd_buf;
    w[10] = len;
    send_cmd(0, CMD_RPC_RDATA, p, RPC_PKT_SIZE, 0, 0, 0);
    if (wait_done(&rdc) < 0)
      return -1;
    const volatile u8 *b = MX_UNCACHED(rd_buf);
    for (u32 i = 0; i < n; i++)
      d[i] = b[off + i];
    d += n;
    iop += n;
    size -= n;
  }
  return 0;
}

int mx_iop_write(u32 iop, const void *src, u32 size) {
  if ((iop | (u32)(uintptr_t)src | size) & 15)
    return -1;
  if (!mx_is_spr(src) && ((uintptr_t)src & 0xF0000000u) != 0x20000000u)
    mx_dcache_writeback(src, size);
  struct sif_dma t = {mx_paddr_range(src, size).v, iop, (s32)size, 0};
  mx_sync();
  u32 id;
  while (!(id = mx_bios_sif_set_dma(&t, 1)))
    ;
  while (mx_bios_sif_dma_stat(id) >= 0)
    ;
  return 0;
}

int mx_iop_write_word(u32 iop, u32 value) {
  static u32 blk[4] MX_DMA_LINES;
  if (iop & 3)
    return -1;
  u32 base = iop & ~15u;
  volatile u32 *b = MX_UNCACHED(blk);
  u32 tmp[4];
  if (mx_iop_read(base, tmp, 16) < 0)
    return -1;
  for (int i = 0; i < 4; i++)
    b[i] = tmp[i];
  b[(iop - base) / 4] = value;
  return mx_iop_write(base, (const void *)b, 16);
}

/* ---- IOP modules ---------------------------------------------------------- */

#define LOADFILE_SID 0x80000006

/* The argument of LOADFILE's function 0 (load a module): the result comes
   back in its first two words. */
struct lf_load_arg {
  s32 result; /* in: the length of args */
  s32 modres;
  char path[252];
  char args[252];
};

static struct mx_sif_client lf;
static struct lf_load_arg lf_arg MX_DMA_LINES;
static s32 lf_result[4] MX_DMA_LINES;

int mx_iop_load_module(const char *path) {
  if ((!mx_sif_ready() && mx_sif_init() < 0) || (!mx_sif_bound(&lf) && mx_sif_bind(&lf, LOADFILE_SID) < 0))
    return -1;
  u32 i = 0;
  for (; path[i] && i < sizeof lf_arg.path - 1; i++)
    lf_arg.path[i] = path[i];
  lf_arg.path[i] = 0;
  lf_arg.result = 0;
  lf_arg.modres = 0;
  volatile s32 *r = MX_UNCACHED(lf_result);
  if (mx_sif_call(&lf, 0, &lf_arg, sizeof lf_arg, (void *)r, 8) < 0)
    return -1;
  return r[0];
}

/* ---- tty ---------------------------------------------------------------- */

#ifndef MX_KERNEL /* the kernel has the file server's client: ee/src/base/kstubs.c */
struct fio_write_arg {
  s32 fd;
  u32 ptr;
  u32 size;
  u32 mis;
  u8 aligned[16];
};

/* The file server's clients: one at a time, the next one when a call gets
   no answer (fio_call). */
static struct mx_sif_client fio_pool[4];
static struct mx_sif_client *fio_c = &fio_pool[0];
#define fio (*fio_c)

/* A call to the file server that gives up after 3 s and returns -2. The
   IOP's file server drops what it sends when its SIF DMA queue is full (a
   READ's data, or its answer, under load: the PS2 under ps2link): the call
   then never ends. Its client is left to a late answer and the next call
   uses another one, bound to the same server. Callers that can repeat the
   call (the VM's swap file: a seek and a transfer) do. (Seen before the
   answers' buffers got cache lines of their own, MX_DMA_LINES, docs/vm.md;
   not seen since: kept as a fallback.) */
/* file server calls so far (the VM's statistics count its own) */
volatile u32 mx__fio_calls;

static int fio_call(int fno, const void *send, u32 ssize, void *recv, u32 rsize) {
  mx__fio_calls++;
  u32 w0[4], w1[4];
  mx__sif_waits(w0);
  /* 12 s: the cycle counter wraps at 14.5 */
  int r = sif_call(fio_c, fno, send, ssize, recv, rsize, 12u * 294912000u);
  mx__sif_waits(w1);
  fio_us += w1[1] - w0[1];
  fio_waits++;
  if (r == -2) {
    /* the debug screen (<mx/debug.h>), slot 27: timeouts so far << 24 |
       the function << 16 | RDATA answers waiting << 8 | packets in use */
    static u32 timeouts;
    mx_dbg_word(27, ++timeouts << 24 | (u32)(fno & 0xFF) << 16 | (ndeferred & 0xFF) << 8 |
                        (u32)__builtin_popcount(pkt_busy));
    struct mx_sif_client *n = &fio_pool[(u32)(fio_c - fio_pool + 1) % 4];
    n->server = fio_c->server, n->buf = fio_c->buf, n->cbuf = fio_c->cbuf;
    n->epoch = fio_c->epoch;
    n->done = 0, n->pkt = 0;
    fio_c = n;
  }
  return r;
}
static struct fio_write_arg wr_arg MX_DMA_LINES;
static u32 wr_result[4] MX_DMA_LINES;
static char wr_data[1024] MX_DMA_LINES;

static int fio_bind(void) {
  if (!mx_sif_bound(&fio) && (mx_sif_init() < 0 || mx_sif_bind(&fio, FILEIO_SID) < 0))
    return -1;
  return 0;
}

/* fio calls from more than one thread (the VM pager uses the file server
   from the faulting thread): one at a time. Not from interrupt handlers or
   with interrupts off (debug output there), which cannot wait. */
static int fio_sema = -1;

static int fio_nolock;

static void fio_lock(void) {
  if (fio_nolock || !mx_vm_active() || !(mx_status() & (1u << 16)))
    return;
  if (fio_sema < 0) {
    struct {
      s32 count, max_count, init_count, wait_threads;
      u32 attr, option;
    } p = {0, 1, 1, 0, 0, 0};
    fio_sema = mx_bios_create_sema(&p);
  }
  mx_bios_wait_sema(fio_sema);
}

static void fio_unlock(void) {
  if (!fio_nolock && fio_sema >= 0 && mx_vm_active() && (mx_status() & (1u << 16))) {
    mx_bios_signal_sema(fio_sema);
  }
}

/* A crash: the report goes through here, and the thread that crashed may hold
   the lock. Another one gets a moment to finish. */
void mx__fio_crash(void) {
  if (fio_sema >= 0)
    for (int i = 0; i < 100 && mx_bios_poll_sema(fio_sema) < 0; i++)
      mx_delay_us(1000);
  fio_nolock = 1;
}

/* mx_exit: stop ends the program's threads. Not one in a file call: it would
   leave the lock taken, and the end of the program writes to the tty. So the
   lock first, for a moment; after them nobody is left to share it with. */
void mx__fio_ending(void (*stop)(void)) {
  if (fio_sema >= 0 && !fio_nolock)
    for (int i = 0; i < 100 && mx_bios_poll_sema(fio_sema) < 0; i++)
      mx_delay_us(1000);
  stop();
  fio_nolock = 1;
}

/* The most one READ asks for: the file server's transfer buffer. */
#define FIO_READ_CHUNK 0x4000u
/* the same, changed by probes (probe/p21_iolat.c; 0: FIO_READ_CHUNK) */
u32 mx__fio_read_chunk = FIO_READ_CHUNK;

/* The most one WRITE sends from memory. */
#define FIO_CHUNK 0x10000u
/* the same, changed by probes (probe/p23_blocksize.c; 0: FIO_CHUNK) */
u32 mx__fio_write_chunk = FIO_CHUNK;

/* ---- mxio (common/include/mxio.h), the file service in place of FILEIO -------

   Files go through it when the IOP has it. Its descriptors start at MXIO_FD,
   so everything else sees file numbers as before; directories, remove and
   appending stay with FILEIO. It takes positions: they are kept here. */
#define MXIO_FD 64
static struct mx_sif_client mxio;
static struct {
  u32 pos, stride, piece;
} mxio_file[MXIO_FILES];
static u32 mxio_arg[80] MX_DMA_LINES;
static s32 mxio_out[MXIO_ANSWER] MX_DMA_LINES;
static u8 mxio_bounce[4 * 1024] MX_DMA_LINES;
static u8 mxio_frames[MXIO_BUFFER] MX_DMA_LINES;

static int is_mxio(int fd) { return fd >= MXIO_FD && fd < MXIO_FD + MXIO_FILES; }

static int mxio_up(void) {
  if (!mx_sif_bound(&mxio) && (mx_sif_init() < 0 || mx_sif_bind_tries(&mxio, MXIO_SID, 1) < 0))
    return 0;
  return mxio.server != 0;
}

/* One call: its first answer word, < 0 if the call itself failed */
static int mxio_call(int fno, const void *send, u32 ssize) {
  volatile s32 *r = MX_UNCACHED(mxio_out);
  r[0] = -1;
  if (sif_call(&mxio, fno, send, (ssize + 15) & ~15u, (void *)r, MXIO_ANSWER * 4,
               12u * 294912000u) < 0)
    return -1;
  return r[0];
}

static int mxio_open(const char *path, int flags) {
  volatile u32 *a = MX_UNCACHED(mxio_arg);
  volatile char *d = (volatile char *)(a + 1);
  u32 i = 0;
  a[0] = (u32)flags;
  for (; path[i] && i < 250; i++)
    d[i] = path[i];
  d[i] = 0;
  int fd = mxio_call(MXIO_OPEN, (void *)a, 4 + i + 1);
  if (fd < 0)
    return fd;
  const volatile s32 *r = MX_UNCACHED(mxio_out);
  mxio_file[fd].pos = 0;
  mxio_file[fd].stride = (u32)r[1];
  mxio_file[fd].piece = (u32)r[2];
  return fd + MXIO_FD;
}

static int mxio_read_at(int fd, u32 pos, u32 size, u32 ee) {
  volatile u32 *a = MX_UNCACHED(mxio_arg);
  a[0] = (u32)fd, a[1] = pos, a[2] = size, a[3] = ee;
  return mxio_call(MXIO_READ, (void *)a, 16);
}

/* As fio_read1: at most n bytes into pa, the CPU's view of them at d. Whole
   quadwords at an aligned address go there directly; DMA only writes those. */
static int mxio_read1(int fd, u8 *d, mx_paddr pa, u32 n) {
  int f = fd - MXIO_FD;
  u32 done = 0, direct = pa.v & 15 ? 0 : n & ~15u;
  if (direct) {
    mx_dcache_writeback(d, direct);
    mx_dcache_invalidate(d, direct);
    int got = mxio_read_at(f, mxio_file[f].pos, direct, pa.v);
    mx_dcache_invalidate(d, direct);
    if (got < 0)
      return got;
    mxio_file[f].pos += (u32)got;
    done = (u32)got;
    if (done < direct)
      return (int)done;
  }
  while (done < n) {
    u32 len = n - done > sizeof mxio_bounce ? sizeof mxio_bounce : n - done;
    mx_dcache_invalidate(mxio_bounce, sizeof mxio_bounce);
    int got = mxio_read_at(f, mxio_file[f].pos, len, mx_paddr_of(mxio_bounce).v);
    if (got < 0)
      return done ? (int)done : got;
    const volatile u8 *b = MX_UNCACHED(mxio_bounce);
    for (int i = 0; i < got; i++)
      d[done + (u32)i] = b[i];
    mxio_file[f].pos += (u32)got;
    done += (u32)got;
    if ((u32)got < len)
      break;
  }
  return (int)done;
}

/* n bytes from p, in the pieces OPEN asked for: returns 0, else -1 */
static int mxio_write(int fd, const u8 *p, u32 n) {
  int f = fd - MXIO_FD;
  u32 stride = mxio_file[f].stride, piece = mxio_file[f].piece;
  while (n) {
    u32 len = n > MXIO_MAX_WRITE ? MXIO_MAX_WRITE : n, last = 0, done = 0;
    for (u32 i = 0; done < len; i++) {
      u32 c = len - done > piece ? piece : len - done;
      memcpy(mxio_frames + MXIO_HEAD + i * stride, p + done, c);
      last = MXIO_HEAD + i * stride + c;
      done += c;
    }
    u32 *a = (u32 *)mxio_frames;
    a[0] = (u32)f, a[1] = mxio_file[f].pos, a[2] = len;
    mx_dcache_writeback(mxio_frames, last);
    if (mxio_call(MXIO_WRITE, mxio_frames, last) != (int)len)
      return -1;
    mxio_file[f].pos += len, p += len, n -= len;
  }
  return 0;
}

static int mxio_seek(int fd, s32 offset, int whence) {
  int f = fd - MXIO_FD;
  s32 base = 0;
  if (whence == MX_FIO_SEEK_CUR)
    base = (s32)mxio_file[f].pos;
  else if (whence == MX_FIO_SEEK_END) {
    volatile u32 *a = MX_UNCACHED(mxio_arg);
    a[0] = (u32)f;
    base = mxio_call(MXIO_SIZE, (void *)a, 16);
    if (base < 0)
      return base;
  }
  if (base + offset < 0)
    return -1;
  mxio_file[f].pos = (u32)(base + offset);
  return base + offset;
}

static int mxio_close(int fd) {
  volatile u32 *a = MX_UNCACHED(mxio_arg);
  a[0] = (u32)(fd - MXIO_FD);
  return mxio_call(MXIO_CLOSE, (void *)a, 16);
}

/* A program ended: what it left open */
void mx__mxio_cleanup(void) {
  if (mxio_up()) {
    volatile u32 *a = MX_UNCACHED(mxio_arg);
    a[0] = 0;
    mxio_call(MXIO_CLOSE_ALL, (void *)a, 16);
  }
}

/* One WRITE of n bytes that the IOP reads from pa (the CPU's view of them
   at p): the bytes before the first quadword boundary go in the call (mis,
   as ps2sdk's fioWrite), the rest the IOP takes from memory. Returns the
   bytes written. */
static int fio_write1(int fd, const u8 *p, mx_paddr pa, u32 n) {
  struct fio_write_arg *a = MX_UNCACHED(&wr_arg);
  u32 mis = (16 - (pa.v & 15)) & 15;
  if (mis > n)
    mis = n;
  for (u32 i = 0; i < mis; i++)
    a->aligned[i] = p[i];
  if (!mx_is_spr(p) && ((uintptr_t)p & 0xF0000000u) != 0x20000000u)
    mx_dcache_writeback(p, n);
  a->fd = fd;
  a->ptr = pa.v;
  a->size = n;
  a->mis = mis;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (fio_call(FILEIO_WRITE, a, sizeof *a, (void *)r, 4) < 0)
    return -1;
  return r[0];
}

/* 0 if a WRITE wrote all n bytes, else -1. The tty (fds 0-2 under ps2link)
   answers with other counts: only a failed call counts there. */
static int written(int fd, int got, u32 n) {
  if (got < 0)
    return -1;
  return fd <= 2 || got == (int)n ? 0 : -1;
}

/* Each descriptor's file position as far as known (-1: not known), so a
   seek to where the file already is costs no call: the VM pager's swap
   transfers to consecutive slots, a read after a read. Reads and writes
   advance it; an error forgets it. Under the fio lock. */
#define NPOS 32
static s32 fpos[NPOS] = {[0 ... NPOS - 1] = -1};
static void pos_set(int fd, s32 at) {
  if ((u32)fd < NPOS)
    fpos[fd] = at;
}
static void pos_add(int fd, int r, u32 n) {
  if ((u32)fd < NPOS)
    fpos[fd] = r < 0 || fpos[fd] < 0 ? -1 : fpos[fd] + (s32)n;
}

/* Small writes (the tty) are copied, so the data may be anywhere and change
   at once; large ones go from where they are (core memory or VM; the
   scratchpad and uncached data are copied). VM is pinned for the call
   first: a page fault under the lock would wait for the pager, which needs
   the file server too. */
int mx_fio_write(int fd, const void *data, u32 n) {
  if (fio_bind() < 0)
    return -1;
  const u8 *s = data;
  u32 n0 = n;
  int vm = mx_is_vm(s), r = 0;
  if (vm && mx_vm_pin(s, n, 0) < 0)
    return -1;
  fio_lock();
  if (is_mxio(fd)) {
    r = mxio_write(fd, s, n);
    fio_unlock();
    if (vm)
      mx_vm_unpin(s, n);
    return r;
  }
  if (n >= 256 && !mx_is_spr(s) && ((uintptr_t)s & 0xF0000000u) != 0x20000000u) {
    u32 chunk = mx__fio_write_chunk ? mx__fio_write_chunk : FIO_CHUNK;
    for (u32 done = 0; done < n && r == 0;) {
      struct mx_vm_piece pc = mx_vm_first_piece(s + done, n - done > chunk ? chunk : n - done);
      r = written(fd, fio_write1(fd, s + done, pc.addr, pc.bytes), pc.bytes);
      done += pc.bytes;
    }
  } else {
    while (n && r == 0) {
      u32 len = n > sizeof wr_data ? sizeof wr_data : n;
      char *d = MX_UNCACHED(wr_data);
      for (u32 i = 0; i < len; i++)
        d[i] = (char)s[i];
      r = written(fd, fio_write1(fd, (const u8 *)d, mx_paddr_of(wr_data), len), len);
      s += len;
      n -= len;
    }
  }
  pos_add(fd, r, n0);
  fio_unlock();
  if (vm)
    mx_vm_unpin(data, n0);
  return r;
}

int mx_tty_write(const char *s, u32 n) { return mx_fio_write(1, s, n); }

/* The open arguments of the file server: flags, then the path. */
static struct {
  s32 flags;
  char path[256];
} op_arg MX_DMA_LINES;

/* Where a path without a device is: the directory of the program's ELF, so
   one ELF finds its files on host: as on usb0:. Under the fio lock. */
static char fio_base[128];
void mx__fio_base(const char *argv0) {
  u32 end = 0, colon = 0;
  for (u32 i = 0; argv0[i] && i < sizeof fio_base - 1; i++) {
    if (argv0[i] == ':')
      colon = 1;
    if (argv0[i] == ':' || argv0[i] == '/')
      end = i + 1;
  }
  if (!colon)
    end = 0;
  for (u32 i = 0; i < end; i++)
    fio_base[i] = argv0[i];
  fio_base[end] = 0;
}
static const char *fio_resolve(const char *path) {
  static char full[256];
  if (!fio_base[0])
    return path;
  for (const char *p = path; *p; p++)
    if (*p == ':')
      return path;
  u32 n = 0;
  for (; fio_base[n]; n++)
    full[n] = fio_base[n];
  for (; *path && n < sizeof full - 1; path++)
    full[n++] = *path;
  full[n] = 0;
  return full;
}

static int fio_open_unlocked(const char *path, int flags) {
  if (fio_bind() < 0)
    return -1;
  path = fio_resolve(path);
  if (!(flags & 0x0100 /* APPEND */) && mxio_up()) {
    int fd = mxio_open(path, flags);
    /* a missing file is an answer too: FILEIO would say the same */
    return fd;
  }
  __typeof__(op_arg) *a = MX_UNCACHED(&op_arg);
  a->flags = flags;
  u32 i = 0;
  for (; path[i] && i < sizeof a->path - 1; i++)
    a->path[i] = path[i];
  a->path[i] = 0;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (fio_call(FILEIO_OPEN, a, sizeof *a, (void *)r, 4) < 0)
    return -1;
  return r[0];
}

int mx_fio_open(const char *path, int flags) {
  fio_lock();
  int r = fio_open_unlocked(path, flags);
  pos_set(r, flags & 0x0100 /* APPEND: writes go to the end */ ? -1 : 0);
  fio_unlock();
  return r;
}

/* READ: the IOP sends the 16-byte aligned middle of the data by SIF DMA to
   ptr, and the parts before and after it in the record at read_data (the EE
   copies them). The parts are those of the size asked for: after a short
   read (the end of the file) the DMA ends on the quadword after the last byte
   read, and the part after it is not data. */
struct fio_read_data {
  u32 size1, size2;
  u32 dest1, dest2;
  u8 buf1[16], buf2[16];
};
static struct {
  s32 fd;
  u32 ptr;
  s32 size;
  u32 read_data;
} rd_arg MX_DMA_LINES;
static struct fio_read_data rd_data MX_DMA_LINES;

/* One READ of at most n bytes into pa (the CPU's view of them at d);
   returns the bytes read. */
static int fio_read1(int fd, u8 *d, mx_paddr pa, u32 n) {
  /* the IOP's DMA must not meet dirty lines, nor the EE stale ones after it */
  mx_dcache_writeback(d, n);
  mx_dcache_invalidate(d, n);
  volatile struct fio_read_data *rd = MX_UNCACHED(&rd_data);
  rd->size1 = rd->size2 = 0;
  __typeof__(rd_arg) *a = MX_UNCACHED(&rd_arg);
  a->fd = fd;
  a->ptr = pa.v;
  a->size = (s32)n;
  a->read_data = mx_paddr_of(&rd_data).v;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (fio_call(FILEIO_READ, a, sizeof *a, (void *)r, 4) < 0)
    return -1;
  int got = r[0];
  mx_dcache_invalidate(d, n);
  if (got > 0) {
    /* the parts' addresses are physical, like ptr: as offsets into d */
    u32 o1 = rd->dest1 - pa.v, o2 = rd->dest2 - pa.v;
    for (u32 i = 0; i < rd->size1 && i < 16 && o1 + i < (u32)got; i++)
      d[o1 + i] = rd->buf1[i];
    if (o2 + rd->size2 <= (u32)got)
      for (u32 i = 0; i < rd->size2 && i < 16; i++)
        d[o2 + i] = rd->buf2[i];
  }
  return got;
}

int mx_fio_read(int fd, void *data, u32 n) {
  if (fio_bind() < 0)
    return -1;
  u8 *d = data;
  int vm = mx_is_vm(d);
  /* whole pages of the buffer not read in first: the read overwrites them */
  if (vm && mx_vm_pin(d, n, MX_VM_PIN_WRITE | MX_VM_PIN_OVERWRITE) < 0)
    return -1;
  fio_lock();
  u32 total = 0;
  int r = 0;
  u32 chunk = is_mxio(fd) ? MXIO_READ_MAX : mx__fio_read_chunk ? mx__fio_read_chunk : FIO_READ_CHUNK;
  while (total < n) {
    /* at most the file server's transfer buffer a READ (16 KiB): reads it
       had to send in two pieces stalled after the first under ps2link */
    u32 len = n - total > chunk ? chunk : n - total;
    struct mx_vm_piece pc = mx_vm_first_piece(d + total, len);
    len = pc.bytes;
    int got = is_mxio(fd) ? mxio_read1(fd, d + total, pc.addr, len)
                          : fio_read1(fd, d + total, pc.addr, len);
    if (got < 0) {
      r = total ? (int)total : got;
      pos_add(fd, -1, 0);
      goto out;
    }
    total += (u32)got;
    if ((u32)got < len)
      break; /* the end of the file */
  }
  r = (int)total;
  pos_add(fd, 0, total);
out:
  fio_unlock();
  if (vm) {
    if (total < n) /* short: what the pin did not read in back */
      mx_vm_overwrite_done(d, n, total);
    mx_vm_unpin(d, n);
  }
  return r;
}

static int fio_lseek1(int fd, s32 offset, int whence) {
  static s32 ls_arg[4] MX_DMA_LINES;
  volatile s32 *a = (volatile s32 *)MX_UNCACHED(ls_arg);
  a[0] = fd;
  a[1] = offset;
  a[2] = whence;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (fio_call(FILEIO_LSEEK, (void *)a, 16, (void *)r, 4) < 0)
    return -1;
  return r[0];
}

static int fio_lseek_unlocked(int fd, s32 offset, int whence) {
  if (fio_bind() < 0)
    return -1;
  /* ps2client (ps2link's host) takes a negative offset from the current
     position or the end as a huge positive one: it answers with the right
     position, but its file is then past the end (docs/bios.md). From the
     position itself instead. */
  if (offset < 0 && whence != MX_FIO_SEEK_SET) {
    int base = fio_lseek1(fd, 0, whence);
    if (base < 0)
      return base;
    if (base + offset < 0)
      return -1;
    return fio_lseek1(fd, base + offset, MX_FIO_SEEK_SET);
  }
  return fio_lseek1(fd, offset, whence);
}

int mx_fio_lseek(int fd, s32 offset, int whence) {
  fio_lock();
  int r;
  if (is_mxio(fd)) {
    r = mxio_seek(fd, offset, whence);
    fio_unlock();
    return r;
  }
  if (whence == MX_FIO_SEEK_SET && offset >= 0 && (u32)fd < NPOS && fpos[fd] == offset)
    r = offset; /* there already */
  else {
    r = fio_lseek_unlocked(fd, offset, whence);
    pos_set(fd, r < 0 ? -1 : r);
  }
  fio_unlock();
  return r;
}

static int fio_close_unlocked(int fd) {
  if (fio_bind() < 0)
    return -1;
  static s32 cl_arg[4] MX_DMA_LINES;
  volatile s32 *a = (volatile s32 *)MX_UNCACHED(cl_arg);
  a[0] = fd;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (fio_call(FILEIO_CLOSE, (void *)a, 16, (void *)r, 4) < 0)
    return -1;
  return r[0];
}

int mx_fio_close(int fd) {
  fio_lock();
  pos_set(fd, -1);
  int r = is_mxio(fd) ? mxio_close(fd) : fio_close_unlocked(fd);
  fio_unlock();
  return r;
}

/* DOPEN: the path (at a 16-byte boundary); DREAD: the IOP sends the entry
   (io_dirent_t: its stat, 40 bytes, then the name) by SIF DMA to buf. FILEIO
   1.1's DREAD starts that DMA with interrupts on: patched before the first
   DOPEN (mx_iop_patch_fileio). */
static int fio_dopen_unlocked(const char *path) {
  if (fio_bind() < 0)
    return -1;
  path = fio_resolve(path);
  static u32 patched; /* sif_epoch + 1: patched in this boot of the IOP */
  if (patched != sif_epoch + 1) {
    patched = sif_epoch + 1;
    mx_iop_patch_fileio();
  }
  static char dp_arg[256] MX_DMA_LINES;
  char *a = MX_UNCACHED(dp_arg);
  u32 i = 0;
  for (; path[i] && i < sizeof dp_arg - 1; i++)
    a[i] = path[i];
  a[i] = 0;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (fio_call(FILEIO_DOPEN, a, sizeof dp_arg, (void *)r, 4) < 0)
    return -1;
  return r[0];
}

int mx_fio_dopen(const char *path) {
  fio_lock();
  int r = fio_dopen_unlocked(path);
  fio_unlock();
  return r;
}

static int fio_dread_unlocked(int fd, char *name, u32 room, u32 *size) {
  if (fio_bind() < 0)
    return -1;
  static struct {
    u32 mode, attr, size;
    u8 time[3][8];
    u32 hisize;
    char name[256];
    u32 unknown;
  } de MX_DMA_LINES;
  static s32 dr_arg[4] MX_DMA_LINES;
  mx_dcache_writeback(&de, sizeof de); /* no dirty lines for the DMA to meet */
  mx_dcache_invalidate(&de, sizeof de);
  volatile s32 *a = (volatile s32 *)MX_UNCACHED(dr_arg);
  a[0] = fd;
  a[1] = (s32)(uintptr_t)&de;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (fio_call(FILEIO_DREAD, (void *)a, 16, (void *)r, 4) < 0)
    return -1;
  if (r[0] <= 0)
    return r[0];
  volatile __typeof__(de) *e = MX_UNCACHED(&de);
  /* drivers written for iomanX (bdmfs_fatfs: mass:) fill in its larger
     dirent: 64 bytes of stat, so the name comes 24 bytes later */
  volatile const char *src = e->name[0] ? e->name : e->name + 24;
  u32 max = e->name[0] ? sizeof de.name : sizeof de.name - 24;
  u32 i = 0;
  for (; room && i < room - 1 && i < max && src[i]; i++)
    name[i] = src[i];
  if (room)
    name[i] = 0;
  if (size)
    *size = e->size;
  return r[0];
}

int mx_fio_dread(int fd, char *name, u32 room, u32 *size) {
  fio_lock();
  int r = fio_dread_unlocked(fd, name, room, size);
  fio_unlock();
  return r;
}

static int fio_dclose_unlocked(int fd) {
  if (fio_bind() < 0)
    return -1;
  static s32 dc_arg[4] MX_DMA_LINES;
  volatile s32 *a = (volatile s32 *)MX_UNCACHED(dc_arg);
  a[0] = fd;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (fio_call(FILEIO_DCLOSE, (void *)a, 16, (void *)r, 4) < 0)
    return -1;
  return r[0];
}

int mx_fio_dclose(int fd) {
  fio_lock();
  int r = fio_dclose_unlocked(fd);
  fio_unlock();
  return r;
}

/* REMOVE, MKDIR and RMDIR: the path (the ROM's MKDIR takes no mode). FILEIO
   1.1's REMOVE goes on into MKDIR: patched first (mx_iop_patch_fileio). */
static int fio_path_call_unlocked(int fn, const char *path) {
  if (fio_bind() < 0)
    return -1;
  path = fio_resolve(path);
  static u32 patched; /* sif_epoch + 1: patched in this boot of the IOP */
  if (patched != sif_epoch + 1) {
    patched = sif_epoch + 1;
    mx_iop_patch_fileio();
  }
  static char pa_arg[256] MX_DMA_LINES;
  char *a = MX_UNCACHED(pa_arg);
  u32 i = 0;
  for (; path[i] && i < sizeof pa_arg - 1; i++)
    a[i] = path[i];
  a[i] = 0;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (fio_call((u32)fn, a, sizeof pa_arg, (void *)r, 4) < 0)
    return -1;
  return r[0];
}

static int fio_path_call(int fn, const char *path) {
  fio_lock();
  int r = fio_path_call_unlocked(fn, path);
  fio_unlock();
  return r;
}

int mx_fio_remove(const char *path) { return fio_path_call(FILEIO_REMOVE, path); }
int mx_fio_mkdir(const char *path, int mode) {
  (void)mode;
  return fio_path_call(FILEIO_MKDIR, path);
}
int mx_fio_rmdir(const char *path) { return fio_path_call(FILEIO_RMDIR, path); }
#endif /* !MX_KERNEL */

/* ---- IOP reset ---------------------------------------------------------- */

static int reset_wait(void);

/* As ps2sdk's SifIopReset, SifIopSync and SifInitRpc: a RESET command to
   the IOP's SIF command handler, then the SIF registers as a new IOP boot
   expects them; the IOP sets BOOTEND in SMFLAG when it is up again. */
int mx_iop_reset(const char *arg) {
  static struct {
    u32 size, dest, cid, opt;
    s32 arglen, mode;
    char arg[80];
  } pkt MX_DMA_LINES;
#ifdef MX_KERNEL
  if (mx__kernel) {
    int r = mx__kernel->iop_reset(arg);
    sif_epoch++;
    return r;
  }
#endif
  if (!mx_sif_ready() && mx_sif_init() < 0)
    return -1;
  u32 n = 0;
  if (arg)
    for (; arg[n] && n < sizeof pkt.arg - 1; n++)
      pkt.arg[n] = arg[n];
  pkt.arg[n] = 0;
  pkt.size = sizeof pkt; /* psize; dsize 0 */
  pkt.dest = 0;
  pkt.cid = CMD_RESET;
  pkt.opt = 0;
  pkt.arglen = (s32)n;
  pkt.mode = 0;
  mx_dcache_writeback(&pkt, sizeof pkt);

  mx_bios_sif_stop_dma();
  struct sif_dma t = {(u32)(uintptr_t)&pkt, (u32)mx_bios_sif_get_reg(SYSREG_SUBADDR),
                      (s32)sizeof pkt, DMA_ERT | DMA_INT_O};
  mx_bios_sif_set_reg(REG_SMFLAG, SMFLAG_BOOTEND);
  if (!mx_bios_sif_set_dma(&t, 1))
    return -1;
  mx_bios_sif_set_reg(REG_SMFLAG, SMFLAG_SIFINIT);
  mx_bios_sif_set_reg(REG_SMFLAG, SMFLAG_CMDINIT);
  mx_bios_sif_set_reg(SYSREG_RPCINIT, 0);
  mx_bios_sif_set_reg(SYSREG_SUBADDR, 0);
  return reset_wait();
}

/* For mx_iop_reboot_image (iop.c): LOADFILE loads path (UDNL) with args
   (args_len bytes, strings one after the other), which reboots the IOP: no
   answer comes. Then the SIF registers and the wait as for mx_iop_reset. */
int mx__iop_reboot_udnl(const char *path, const char *args, u32 args_len) {
#ifdef MX_KERNEL
  if (mx__kernel)
    return -1; /* the kernel resets the IOP by a path (iop_reset) only */
#endif
  if ((!mx_sif_ready() && mx_sif_init() < 0) ||
      (!mx_sif_bound(&lf) && mx_sif_bind(&lf, LOADFILE_SID) < 0))
    return -1;
  u32 i = 0;
  for (; path[i] && i < sizeof lf_arg.path - 1; i++)
    lf_arg.path[i] = path[i];
  lf_arg.path[i] = 0;
  if (args_len > sizeof lf_arg.args)
    args_len = sizeof lf_arg.args;
  for (i = 0; i < args_len; i++)
    lf_arg.args[i] = args[i];
  lf_arg.result = (s32)args_len;
  lf_arg.modres = 0;
  mx_sif_call_async(&lf, 0, &lf_arg, sizeof lf_arg, (void *)MX_UNCACHED(lf_result), 8);
  /* as ps2sdk's SifIopRebootBuffer after its LoadModule without waiting */
  mx_bios_sif_set_reg(REG_SMFLAG, SMFLAG_SIFINIT);
  mx_bios_sif_set_reg(REG_SMFLAG, SMFLAG_CMDINIT);
  mx_bios_sif_set_reg(REG_SMFLAG, SMFLAG_BOOTEND);
  mx_bios_sif_set_reg(SYSREG_RPCINIT, 0);
  mx_bios_sif_set_reg(SYSREG_SUBADDR, 0);
  return reset_wait();
}

/* After the IOP got its reboot: wait for it to come back up. */
static int reset_wait(void) {
  /* everything of the IOP before is gone: our bindings, its packets, the
     file positions; mx_sif_init starts again once the IOP is up */
  mx_sif_exit();
  sif_epoch++;
  pkt_busy = 0;
  ndeferred = 0;
  for (int i = 0; i < 32; i++)
    sregs[i] = 0;
#ifndef MX_KERNEL
  for (int i = 0; i < NPOS; i++)
    fpos[i] = -1;
#endif
  u32 t0 = mx_count();
  while (!(mx_bios_sif_get_reg(REG_SMFLAG) & SMFLAG_BOOTEND))
    if (mx_count() - t0 > 10u * 294912000u) /* 10 s (the counter wraps at 14.5) */
      return -1;
  if (mx_sif_init() < 0)
    return -1;
  /* BOOTEND comes before the ROM's servers are up (LOADFILE, FILEIO and
     the heap start after it): wait until each answers a bind. Not bind
     after bind: answering them kept the IOP from starting the servers
     (PCSX2), so 10 ms between tries */
  static const u32 sids[3] = {LOADFILE_SID, FILEIO_SID, 0x80000003u /* the IOP heap */};
  static struct mx_sif_client c[3]; /* static: each keeps its semaphore */
  for (int i = 0; i < 3; i++) {
    t0 = mx_count();
    while (mx_sif_bind_tries(&c[i], sids[i], 1) < 0) {
      if (mx_count() - t0 > 10u * 294912000u)
        return -1;
      u32 t1 = mx_count();
      while (mx_count() - t1 < 2949120u) /* 10 ms */
        ;
    }
  }
  return 0;
}
