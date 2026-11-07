# mxfast

The fast path for `host:` files (`common/include/mxfast.h`) on maxsdk's network drivers: a client of
mxeth (`docs/iop-drivers.md`) before lwIP. It takes every frame to UDP port 18197, sends DATA by SIF
DMA straight from mxeth's receive buffer to the EE, and builds its requests (and the frames of a
write) itself, to the host that said HELLO: lwIP is not used. Its caller (mxio) sleeps on a
semaphore that the answers, a whole read, sent frames and an alarm (timeouts and retries) give.

Library "mxfast", the same as `ps2ip-nm.irx`'s (`iop/tcpip/mxnet`), which has it in it for ps2sdk's
drivers. mxlink loads it after the network when maxsdk's drivers are used.
