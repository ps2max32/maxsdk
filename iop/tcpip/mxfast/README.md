# mxfast

The fast path for `host:` files (`common/include/mxfast.h`) on maxsdk's network drivers: a client of
mxeth (`docs/iop-drivers.md`) before lwIP. It takes every frame to UDP port 18197, sends DATA by SIF
DMA straight from mxeth's receive buffer to the EE, and builds its requests (and the frames of a
write) itself, to the host that said HELLO: lwIP is not used. Its caller (mxio) sleeps on a
semaphore that the answers, a whole read, sent frames and an alarm (timeouts and retries) give.

It is the console too (`src/console.c`): the IOP's `tty` device and Kprintf, to the host of the
fast path as TTY frames that the host answers (nothing lost, nothing twice), or by broadcast to
port 18194 (as ps2sdk's udptty did, so any ps2client sees it) when no such host is there. Design: `docs/iop-drivers.md`, "Console".

Library "mxfast", which mxio looks for. mxlink loads it right after mxnet.
