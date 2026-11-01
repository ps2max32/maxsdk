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
#include <mx/string.h>
#include <mx/vm.h>

/* System command IDs */
#define CMD_CHANGE_SADDR 0x80000000u
#define CMD_SET_SREG 0x80000001u
#define CMD_INIT_CMD 0x80000002u
#define CMD_RPC_END 0x80000008u
#define CMD_RPC_BIND 0x80000009u
#define CMD_RPC_CALL 0x8000000Au
#define CMD_RPC_RDATA 0x8000000Cu

/* SIF registers (sceSifGetReg/SetReg) */
#define REG_SUBADDR 2
#define REG_SMFLAG 4
#define SYSREG_SUBADDR 0x80000000u
#define SYSREG_RPCINIT 0x80000002u
#define SMFLAG_CMDINIT 0x20000

/* sceSifSetDma attributes */
#define DMA_INT_O 0x4
#define DMA_ERT 0x40

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

int mx_sif_ready(void) { return sif_state == 1; }

void mx_sif_exit(void) {
  if (sif0_handler_id > 0)
    mx_bios_remove_dmac_handler(MX_DMAC_SIF0, sif0_handler_id);
  sif0_handler_id = 0;
  sif_state = 0;
}

int mx_sif_init(void) {
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

void mx__irq_vbon_hook(void (*fn)(void));

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

static int wait_answer(struct mx_sif_client *c, int may_give_up) {
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
    if (c->server)
      return 0;
  }
  return -1;
}

/* An RPC call; wait (cycles): 0 forever, otherwise give up after about
   that long and return -2. A call given up leaves its client to its late
   answer, if one comes (it frees the call's packet then): the caller must
   use another client struct for its next call (fio_call). */
static int sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                    u32 rsize, u32 wait);

int mx_sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                u32 rsize) {
  return sif_call(c, fno, send, ssize, recv, rsize, 0);
}

static void sif_send_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize,
                          void *recv, u32 rsize);

void mx_sif_call_async(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                       u32 rsize) {
  sif_send_call(c, fno, send, ssize, recv, rsize);
}

int mx_sif_wait(struct mx_sif_client *c) { return wait_done(c); }

static int sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                    u32 rsize, u32 wait) {
  sif_send_call(c, fno, send, ssize, recv, rsize);
  if (!wait)
    return wait_done(c);
  return wait_answer(c, (int)wait) < 0 ? -2 : 0;
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
  if ((!mx_sif_ready() && mx_sif_init() < 0) || (!lf.server && mx_sif_bind(&lf, LOADFILE_SID) < 0))
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
  /* 12 s: the cycle counter wraps at 14.5 */
  int r = sif_call(fio_c, fno, send, ssize, recv, rsize, 12u * 294912000u);
  if (r == -2) {
    /* the debug screen (<mx/debug.h>), slot 27: timeouts so far << 24 |
       the function << 16 | RDATA answers waiting << 8 | packets in use */
    static u32 timeouts;
    mx_dbg_word(27, ++timeouts << 24 | (u32)(fno & 0xFF) << 16 | (ndeferred & 0xFF) << 8 |
                        (u32)__builtin_popcount(pkt_busy));
    struct mx_sif_client *n = &fio_pool[(u32)(fio_c - fio_pool + 1) % 4];
    n->server = fio_c->server, n->buf = fio_c->buf, n->cbuf = fio_c->cbuf;
    n->done = 0, n->pkt = 0;
    fio_c = n;
  }
  return r;
}
static struct fio_write_arg wr_arg MX_DMA_LINES;
static u32 wr_result[4] MX_DMA_LINES;
static char wr_data[1024] MX_DMA_LINES;

static int fio_bind(void) {
  if (!fio.server && (mx_sif_init() < 0 || mx_sif_bind(&fio, FILEIO_SID) < 0))
    return -1;
  return 0;
}

/* fio calls from more than one thread (the VM pager uses the file server
   from the faulting thread): one at a time. Not from interrupt handlers or
   with interrupts off (debug output there), which cannot wait. */
static int fio_sema = -1;

static void fio_lock(void) {
  if (!mx_vm_active() || !(mx_status() & (1u << 16)))
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
  if (fio_sema >= 0 && mx_vm_active() && (mx_status() & (1u << 16))) {
    mx_bios_signal_sema(fio_sema);
  }
}

/* The most one READ asks for: the file server's transfer buffer. */
#define FIO_READ_CHUNK 0x4000u
/* the same, changed by probes (probe/p21_iolat.c) */
u32 mx__fio_read_chunk = FIO_READ_CHUNK;

/* The most one WRITE sends from memory. */
#define FIO_CHUNK 0x10000u

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
  if (n >= 256 && !mx_is_spr(s) && ((uintptr_t)s & 0xF0000000u) != 0x20000000u) {
    for (u32 done = 0; done < n && r == 0;) {
      struct mx_vm_piece pc = mx_vm_first_piece(s + done, n - done > FIO_CHUNK ? FIO_CHUNK : n - done);
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

static int fio_open_unlocked(const char *path, int flags) {
  if (fio_bind() < 0)
    return -1;
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
  while (total < n) {
    /* at most the file server's transfer buffer a READ (16 KiB): reads it
       had to send in two pieces stalled after the first under ps2link */
    u32 len = n - total > mx__fio_read_chunk ? mx__fio_read_chunk : n - total;
    struct mx_vm_piece pc = mx_vm_first_piece(d + total, len);
    len = pc.bytes;
    int got = fio_read1(fd, d + total, pc.addr, len);
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
  int r = fio_close_unlocked(fd);
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
  static int patched;
  if (!patched) {
    patched = 1;
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
  u32 i = 0;
  for (; room && i < room - 1 && i < sizeof de.name && e->name[i]; i++)
    name[i] = e->name[i];
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
  static int patched;
  if (!patched) {
    patched = 1;
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
