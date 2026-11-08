# IOP drivers: DEV9, Ethernet, console, HDD

The design of maxsdk's own IOP drivers for the expansion bay (DEV9). They replace ps2sdk's
`ps2dev9`, `smap`, `netman` and `udptty`, and its ATA driver. Status per step is
at the end.

## Why new drivers

ps2sdk's network path works, but costs the IOP more than it needs to (measured with
`probe/p26_rawnet.c`, `probe/iop/rawnet.c`):

- **DMA by busy-wait, with nothing beside it.** `SpdDmaTransfer` (ps2dev9) starts DEV9 DMA (IOP
  channel 8) and spins on `CHCR.TR` for the whole transfer, once per frame, then smap's post-DMA
  callback spins on the FIFO's `DMAEN`. A semaphore guards the channel for ATA and SMAP, so an ATA
  transfer of milliseconds would spin as long.
- **Sending waits for an empty FIFO.** smap refills its 4 KiB TX FIFO (two full frames) only when
  both are out (TXDNV): 7.9 MB/s out against 11.8 in.
- **Three modules and one stack.** netman only passes frames between smap and one protocol stack.
  mxfast (the fast path for file data) had to sit inside lwIP's receive function, and the console
  (udptty) goes through lwIP's sockets.
- **Slow link-up after every IOP reset.** smap resets the PHY at each start and then waits at least
  3 s for auto-negotiation, although the PHY keeps its link when only the IOP is reset (mxlink does
  that after every program that loaded modules).

## The modules

| Module | Replaces | Does |
|---|---|---|
| `mxdev9.irx` | ps2dev9 | DEV9 power, SPEED interrupts, channel-8 DMA, MAC from the EEPROM, LED |
| `mxeth.irx` | smap + netman | SMAP and PHY, RX buffers, frame sorting to its clients, TX queue |
| `mxnet.irx` | ps2ip-nm | lwIP (as now), as a client of mxeth |
| `mxfast.irx` | the fast path in ps2ip-nm, udptty | `host:` files past lwIP, and the console, as a client of mxeth |
| `mxata.irx` | atad, ata_bd | ATA on mxdev9's DMA queue, as a block device for bdm |

The interfaces are maxsdk's own (`iop/dev9/*/include`); no ps2sdk program needs them. mxlink has
no other network: ps2sdk's modules are not on the card and not loaded (step 4).

## mxdev9

- **Hardware.** The CXD9611 (expansion bay: SCPH-3xxxx to 5xxxx with the network adapter, every
  slim model). The CXD9566 (PCMCIA, SCPH-1xxxx) is not supported: on it mxdev9 does not load, and
  mxlink has no network.
- **Start.** Powers DEV9 on if it is off (two 0.5 s sleeps, at power-on only), reads the MAC and
  its checksum from the EEPROM, turns all SPEED interrupts off. A later load (after an IOP reset)
  finds DEV9 on and takes a few milliseconds.
- **Interrupts.** One handler for `IOP_IRQ_DEV9`. Each client registers a handler for its SPEED
  interrupt bits (`mxdev9_intr`): ATA 0-1, SMAP 2-6. Handlers run in interrupt context and clear
  their own sources.
- **DMA**, two ways:
  - `mxdev9_dma_begin` / `mxdev9_dma_end`, for frames: a thread starts the transfer (if the
    channel is free), works on meanwhile, and `end` waits for what is left of it (the
    channel's interrupt, if it is on for queued requests, is ignored then). DICR2 is written only
    by intrman: mxdev9 turning channel 8's interrupt off and on in it for each frame spoilt other
    channels' transfers (the last 4 bytes of a memory card write, which come over SIF). A frame's DMA takes about 40 us; on the IOP an interrupt and a thread
    switch cost more than that (measured below), so waiting for the rest is the cheaper wait here.
  - `mxdev9_dma` queues a request (`struct mxdev9_dma`: device, buffer, blocks, direction,
    `start` and `done`), from a thread or an interrupt handler. It starts when the channel is free
    and its end is the channel's interrupt (`IOP_IRQ_DMA_DEV9`): mxdev9 calls `done`, which may
    queue the next one. For long transfers (ATA), where sleeping pays.
  - `start` (both ways) runs with interrupts off right before the transfer: it sets the device's
    FIFO size and DMA enable.
- **Power off.** Clients register a function for power-off (HDD: flush and spin down);
  `mxdev9_off` calls them and turns DEV9 off.

## mxeth

### Receive

- mxeth owns the receive buffers: `rx=<n>` frames of 1520 bytes (default 48, 72 KiB), each with
  a 32-byte header for the client that takes it.
- One driver thread does all the work. The SMAP's interrupt (RXEND) masks itself and wakes it.
  The thread takes every frame the SMAP holds: it starts a frame's DMA from the RX FIFO, gives the
  frame before it to the clients while the DMAC works, waits for the rest of the DMA, reads the last
  bytes (less than 64) by PIO and frees the descriptor.
- While frames keep coming it does not unmask the interrupt but looks again after `poll=<us>`
  (default 1000, an alarm: a sleep), as ps2sdk's smap does: at 12 MB/s that is one wake-up for
  about 8 frames. A look that finds nothing unmasks the interrupt, so the first frame after a quiet
  time is taken at once. `poll=0`: an interrupt for each batch.
- With no buffer free the frames stay in the SMAP (its RX FIFO holds 16 KiB); `mxeth_free` wakes
  the thread again.
- Each frame goes to the clients in order, `rx(frame)`: a client returns 1 if it takes the frame
  (it calls `mxeth_free` later, from any context), 0 to pass it on. A frame nobody takes is freed.
- Clients, in order: fast path (`mxfast.irx`, every frame to its UDP port: the data goes by SIF
  DMA straight from the receive buffer to the EE), console, lwIP. lwIP gets the buffer itself (a `pbuf_custom` whose free
  returns it to mxeth); when fewer than 8 buffers are free it copies into its own pbuf instead, so a
  socket nobody reads cannot starve the driver.

### Send

- `mxeth_send(struct mxeth_tx *)` queues a frame (data 4-byte aligned, else it goes by PIO;
  length; `done` callback), from any context. The driver thread moves queued frames into the TX
  FIFO while it has room (4 KiB: two full frames), each by DMA, with the `done` of the frames before
  it running during the DMA, and starts the MAC once for all of them (an EMAC3 access is slow).
  TXEND (a frame is out) wakes it for the next.
- `done` (the caller may reuse its data) runs in the driver thread. A send from the driver thread
  itself (from a client's `rx` or a `done`) does not wake it: it sends before it waits again.

### Link

- At start: if the PHY already has a link from auto-negotiation (after an IOP reset), mxeth takes
  the negotiated mode from it and is up at once. Otherwise it resets the PHY and polls auto-
  negotiation every 100 ms (at most 5 s, three tries), then tries 100 and 10 Mbit half duplex as smap
  does.
- Afterwards a link thread checks the PHY once a second (a sleep), and reports link up and down
  to the clients.
- PHY register access (MDIO) waits for the EMAC3's done bit with short sleeps (`DelayThread`): an
  access takes about 30 us, and happens at start and once a second.

## mxfast

The fast path (`common/include/mxfast.h`) without lwIP: a client of mxeth at priority 10, before
lwIP's 30. It takes every frame to UDP port 18197 (lwIP has no socket there):

- DATA goes by SIF DMA straight from the receive buffer; the buffer goes back to mxeth when its
  DMA is over (seen at the next frame, or at the read's end).
- Answers and HELLO give it the host's MAC and IP, so it builds its requests (OPEN, READ, SIZE,
  CLOSE, COMMIT, LOG) and the frames of a write as whole frames itself, for `mxeth_send`. The host
  must know the PS2's MAC: lwIP answers its ARP.
- Its caller (mxio's thread) sleeps on one semaphore, which `rx` gives at an answer or when a read
  is complete, a sent frame's `done`, or an alarm: a read without progress for 30 ms asks for what
  is missing, a request without answer for 200 ms is sent again.

## Console

udptty broadcast each `printf` as a UDP packet through lwIP: to every machine on the LAN, and lost
when nobody listened yet or a packet went missing (the test end marker too). The console is now
part of `mxfast.irx` (`src/console.c`): the IOP's `tty` device (stdin, stdout, stderr, so the EE's
tty output too) and Kprintf write into a ring of 16 KiB, and a thread sends it on:

- **To the host of the fast path.** ps2client's HELLO names its session; the console then goes
  to that host alone as TTY frames (`MXFAST_TTY`), built by mxfast, without lwIP. Each frame
  names its offset in the stream and the stream (new at each start of the module, so after an
  IOP reset); the host prints each byte once and in order and answers how far it has the
  stream. Without an answer for 100 ms the console sends again from there. While the host is
  there a writer waits for room in the ring instead of losing output (not one at the driver's
  priority or above, whose `rx` brings the answers; nor Kprintf in an interrupt: there the
  oldest bytes give way).
- **A new session** (the next ps2client) starts at what is written next: nothing the last one
  did not take, a test end marker for instance, reaches it. A host silent for 3 s is gone, and
  ps2client says goodbye when it stops (a HELLO without session).
- **Else by broadcast**, as udptty did (UDP to port 18194 through lwIP's socket, looked up when
  needed), for any client. At the start of the module the console waits up to 2 s for a HELLO
  first, so that the IOP's start-up messages reach the host that is there.

mxlink loads mxfast right after mxnet; udptty is not used. Input from the host (stdin) can come the
same way later.

## HDD (step 5)

`mxata.irx` (`iop/dev9/mxata`) drives the SPEED's ATA interface on mxdev9 (interrupt bits 0-1
and DMA device 0) and offers the disk to bdm as a block device (`ata`), as ps2sdk's `ata_bd` does:
bdm finds its MBR or GPT partitions and `bdmfs_fatfs` mounts FAT32 or exFAT as `ata0:`, the way it
mounts `usb0:`. A disk formatted on a PC works; Sony's APA/PFS (the HDD browser's format) does not.

- **One disk**, device 0, UDMA (the highest mode up to 4 that it offers), 48-bit LBA when it has it
  and needs it (28-bit commands otherwise). PIO only for IDENTIFY at the start.
- **DMA in the buffer's pieces.** The SPEED's buffer says how many sectors it holds (reading) or
  takes (writing): at most 31. Each piece is one request in mxdev9's queue, so frames get the
  channel in between and a request never waits for the disk. The next piece is queued from the
  interrupts: the DMA's end, or the buffer's ready interrupt (bit 1) when it was empty. The thread
  sleeps until the command's end (bit 0): nothing spins. A command is aborted after 30 s.
- **Start** in a thread of its own, so loading the module does not wait for the disk: reset,
  IDENTIFY, UDMA mode, a standby timer of 21 minutes, then `bdm_connect_bd`. Without the HDD
  connector (a slim PS2) or on a PSX (DVR) it does not stay loaded.
- **Buffers** must be 4-byte aligned for DMA; others go through a 4 KiB copy.
- **Flush and stop**: bdm's flush is FLUSH CACHE, its stop also STANDBY IMMEDIATE. The same runs
  as mxdev9's power-off function, but nothing calls `mxdev9_off` yet.
- Compatible adaptors (not Sony's, `SPEED 0x20` not 1) get ps2sdk's workaround: the transfer
  direction set before the command, DMA enabled with it.

Tested on PCSX2 (`probe/p34_ata.c`, the image given by the PCSX2 runner's `PCSX2_HDD`): FAT32 and
exFAT, a 4 MiB file read in 16, 32 and 64 KiB right, written and read back. Not yet on a PS2 with
a disk. Found on the way: `bdmfs_fatfs` (ps2sdk's) fails writes into a FAT32 file that `O_TRUNC`
emptied, after about 0.5 MB, with ps2sdk's `ata_bd` as well; exFAT does not.

## Waits

| Where | How it waits |
|---|---|
| A frame's DMA (about 40 us) | the rest of it, after the work that overlaps it: about 1 us a frame at 12 MB/s |
| SMAP FIFO DMAEN after a DMA | checked once; it is clear by then (counted if not) |
| RX | the interrupt after a quiet time, else an alarm each `poll` us |
| TX | TXEND; the thread waits on its event flag |
| ATA DMA | channel-8 interrupt per piece; the buffer's interrupt when it is empty; the command's end |
| PHY access (MDIO) | `DelayThread` polls, about 30 us, at start and once a second |
| Link check | a thread sleeping 1 s (100 ms while there is no link) |
| Power-on, FIFO and MAC resets | `DelayThread` polls, at start only |

## Measurements

`probe/p26_rawnet.c` with `probe/iop/rawnet.c` (the stack only counts frames, or sends each to
the EE by SIF DMA) and `rawnet_host.py`, SCPH-70004, 2026-11-07. "Free" is what a thread at the
lowest priority gets of the IOP.

| | ps2sdk: ps2dev9, netman, smap | mxdev9, mxeth |
|---|---|---|
| first answer to the host after the IOP reset | 1.0 s after mxeth's | at once (the PHY's link is taken as it is) |
| receive 12 MB/s, frames counted | 11.77 MB/s, 54% free | 11.75 MB/s, 61% free |
| receive to the EE, 2 / 6 / 10 MB/s | 73 / 52 / 30% free | 75 / 56 / 36% free |
| receive to the EE, 12 MB/s | 11.77 MB/s, 19% free | 11.75 MB/s, 26% free |
| send 4000 frames of 1514 bytes | 7.92 MB/s, 39% free | 10.91 MB/s, 0% free |

TCP through lwIP (`probe/iop/net.c`, `net_host.py`: 8 MiB each way), where lwIP is the limit as
before: to the PS2 3.12 s with ps2sdk's modules and `ps2ip-nm`, 3.27 s with mxdev9, mxeth and
`mxnet`; back 4.14 s and 3.73 s.

Through mxlink (mxdev9, mxeth and mxnet from the card), 2026-11-07: p29 (UDP echo 1132 us,
2 MiB TCP out and back in 2380 ms), the BSD sockets test, `vm22_net`, `fio_big` and `fio_stress`
pass; `p21_iolat` on `host:` (the fast path, still inside mxnet): a 32 KiB READ 4.1 ms, a
32 KiB WRITE 7.5 ms, as with ps2sdk's drivers. With `mxfast.irx`: READ 4.0 ms, WRITE 6.4 ms
(the read is bound by the wire and the host: 32 KiB take 2.8 ms at 100 Mbit/s).

No frame lost in any run. (Numbers since the DICR2 fix; before it, writing DICR2 each frame,
receiving to the EE left 33% at 12 MB/s.) Two designs measured first, both left 4% free at 12 MB/s:

- **Everything in interrupt context**, chained by the DMA queue (each frame: the SMAP's
  interrupt starts its DMA, the DMA's interrupt finishes it, the thread wakes for the clients):
  22 us in the SMAP's handler, 18 in the DMA's, 34 in the thread a frame, and the rest of about
  120 us in two interrupts and a thread switch. Sending reached 10.93 MB/s.
- **The thread, an interrupt per frame**, DMA overlapped: still about 120 us a frame.

What made the difference is fewer wake-ups (one for 8 frames, ps2sdk's polling), not where the DMA
is waited for. An IOP interrupt with its wake-up costs more than a frame's DMA.

Sending is bound by the IOP: one TXEND and one wake-up a frame (the FIFO holds two), about
139 us of IOP a frame against 117 for ps2sdk, which refills only when both frames are out.

## Steps

1. mxdev9 and mxeth, measured with `p26_rawnet` (`rawnet` built for mxeth: `rawnet_mx.irx`)
   against ps2sdk's modules. Done: "Measurements".
2. mxnet on mxeth (lwIP's netif on `mxeth_send` and an mxeth client: `mxnet.irx`, built beside
   `ps2ip-nm.irx` from `iop/tcpip/mxnet`; done, measured above); mxlink loads mxdev9, mxeth,
   mxnet when they are on the card, with the ps2sdk set as fallback (done); the fast path as an
   mxeth client, `mxfast.irx` (done).
3. Console over the fast-path protocol, in `mxfast.irx`; udptty not loaded with it (done).
4. ps2sdk's network off the card and out of mxlink: ps2dev9, netman, smap, ps2ip-nm (mxnet's netman
   build, with its own copy of the fast path) and udptty; no fallback, so no network on a PCMCIA
   PS2 or when one of maxsdk's modules does not load (done).
5. mxata, the HDD as a bdm block device (FAT32, exFAT) as `ata0:`; the shell's third device
   (done on PCSX2; on a PS2 with a disk still to do).
