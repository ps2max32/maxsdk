#ifndef MXSWAP_H
#define MXSWAP_H
/* mxswap.irx: the VM's swap file on the IOP (kernel version 49,
   iop/vm/mxswap, ee/src/vm/vm.c). The EE compresses a page (LZ4) and puts
   it here; the IOP keeps it in a queue in its memory and a thread of low
   priority writes it to the swap file at the page's slot (slot * 32 KiB,
   only its bytes), so the EE never waits for a write. A get comes from the
   queue when the page is still there, else from the file, read by the RPC
   thread at high priority: a read waits at most for the write under way.

   Calls (sceSifCallRpc, MXSWAP_SID), all words, 16-byte aligned:
     OPEN   in: [0] the file's size, [1..3] 0, then the path (0-terminated)
                                              out: [0] 0 or < 0
            (the file made that long at once: every write is inside it)
     PUT    in: [0] slot, [1] bytes, [2..3] 0, then the bytes
                                              out: [0] 0, < 0, or MXSWAP_FULL
                                                   (not taken; never waits)
     ROOM   in: [0] bytes                     out: [0] 1 (a PUT of that many fits now), 0
     GET    in: [0] slot, [1] bytes           out: [0] 0 or < 0, [1] from
                                                   the queue (1) or the file
                                                   (0), [2..3] 0, the bytes
     FLUSH  every queued page written         out: [0] 0 or < 0
     CLOSE  FLUSH, then the file closed       out: [0] 0
     STAT                                     out: struct mxswap_stat
     BLOCK  in: [0] the writer's block in bytes (a multiple of 512; 0: as
                it is)                        out: [0] the block now */
#define MXSWAP_SID 0x4D585650u /* "MXVP" (not "MXSW": mxsock's wait services, MXSOCK_WAIT_SID + 0..3) */
enum { MXSWAP_OPEN = 1, MXSWAP_PUT, MXSWAP_GET, MXSWAP_FLUSH, MXSWAP_CLOSE, MXSWAP_STAT, MXSWAP_BLOCK, MXSWAP_ROOM };
#define MXSWAP_FULL (-11) /* PUT's: no room in the queue now; a GET is served meanwhile */
#define MXSWAP_PAGE 32768u
#define MXSWAP_HEAD 16u /* PUT's and GET's words before the bytes */
#define MXSWAP_BUF (MXSWAP_HEAD + MXSWAP_PAGE)
struct mxswap_stat {
  unsigned int queued, queue_bytes, pool_bytes; /* now */
  unsigned int puts, gets, from_queue, writes, write_errors, full; /* full: PUTs not taken */
  /* the gets from the file, in microseconds in all: the wait for the
     file (a write under way), its lseek, its read */
  unsigned int get_wait_us, get_seek_us, get_read_us;
};
#endif
