# IOP drivers: DEV9, Ethernet, console, HDD

The design of maxsdk's own IOP drivers for the expansion bay (DEV9). They replace ps2sdk's
`ps2dev9`, `smap`, `netman` and `udptty`, and later its ATA and HDD drivers. Status per step is
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
  mxfast (the fast path for file data) has to sit inside lwIP's receive function, and the console
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
| mxeth client | udptty | the console, on the fast path's protocol (step 3) |
| `mxata.irx` (later) | atad, ata_bd | ATA on mxdev9's DMA queue, as a block device for mxio |

The interfaces are maxsdk's own (`iop/dev9/*/include`); no ps2sdk program needs them. The ps2sdk
modules stay on the card, and mxlink loads them if `mxdev9` or `mxeth` is missing or does not load.

## mxdev9

- **Hardware.** The CXD9611 (expansion bay: SCPH-3xxxx to 5xxxx with the network adapter, every
  slim model). The CXD9566 (PCMCIA, SCPH-1xxxx) is not supported: on it mxdev9 does not load and the
  ps2sdk modules are used.
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
- Clients, in order: fast path (UDP to its port: the data goes by SIF DMA straight from the
  receive buffer to the EE), console, lwIP. lwIP gets the buffer itself (a `pbuf_custom` whose free
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

## Console (step 3)

udptty sends each `printf` as a UDP packet through lwIP. The new console is a client of mxeth:
lines go out as frames of the fast-path protocol (`common/include/mxfast.h`, a new type), from a
ring the `printf` hook fills, without lwIP and without a socket. ps2client reads them on the
fast-path port (on a branch, as all ps2client changes). Input from the host (stdin) can come the
same way.

## HDD (step 5)

`mxata.irx` drives the SPEED's ATA interface on mxdev9 (interrupt bits 0-1 and DMA device 0),
with UDMA, and offers the disk as a block device to mxio, with its own interface. Transfers are
split into requests of at most 64 KiB so that frames get the channel in between. Power-off flushes
and stops the disk (mxdev9's power-off functions).

## Waits

| Where | How it waits |
|---|---|
| A frame's DMA (about 40 us) | the rest of it, after the work that overlaps it: about 1 us a frame at 12 MB/s |
| SMAP FIFO DMAEN after a DMA | checked once; it is clear by then (counted if not) |
| RX | the interrupt after a quiet time, else an alarm each `poll` us |
| TX | TXEND; the thread waits on its event flag |
| ATA DMA (step 5) | channel-8 interrupt |
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
32 KiB WRITE 7.5 ms, as with ps2sdk's drivers.

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
   mxeth client (next).
3. Console over the fast-path protocol; udptty gone.
4. netman, smap and ps2dev9 off the card.
5. mxata.
