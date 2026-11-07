# mxfast

The fast path for `host:` files (`common/include/mxfast.h`) on maxsdk's network drivers: a client of
mxeth (`docs/iop-drivers.md`) before lwIP. It takes every frame to UDP port 18197, sends DATA by SIF
DMA straight from mxeth's receive buffer to the EE, and builds its requests (and the frames of a
write) itself, to the host that said HELLO: lwIP is not used. Its caller (mxio) sleeps on a
semaphore that the answers, a whole read, sent frames and an alarm (timeouts and retries) give.

It is the console too (`src/console.c`), in place of udptty: the IOP's `tty` device and Kprintf,
to the host of the fast path as TTY frames that the host answers (nothing lost, nothing twice), or
by broadcast as udptty when no such host is there. Design: `docs/iop-drivers.md`, "Console".

Library "mxfast", the same as `ps2ip-nm.irx`'s (`iop/tcpip/mxnet`), which has it in it for ps2sdk's
drivers. mxlink loads it right after mxnet when maxsdk's drivers are used, and udptty not then.
