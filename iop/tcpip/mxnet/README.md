# mxnet

The TCP/IP stack on the IOP: lwIP from `libs/lwip`, plus the fast path that takes file data past
lwIP (`src/mxfast.c`, `common/include/mxfast.h`). Built twice: `ps2ip-nm.irx` on ps2sdk's smap and
netman, `mxnet.irx` (`MXNET_MXETH`) on maxsdk's mxeth (`docs/iop-drivers.md`), whose receive
buffers it takes as pbufs without a copy.

`src/ps2ip.c`, `src/sys_arch.c`, `src/exports.tab`, `src/imports.lst`, `src/irx_imports.h` and
`include/` came from ps2sdk's `iop/tcpip/tcpip-netman` and `tcpip-base`. Licence: `LICENSE`
(Academic Free License 2.0). maxsdk owns these copies and changes them.
