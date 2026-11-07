# mxnet

The TCP/IP stack on the IOP: lwIP from `libs/lwip`, as `mxnet.irx` on maxsdk's mxeth
(`docs/iop-drivers.md`), whose receive buffers it takes as pbufs without a copy. Its exports are
ps2sdk's `ps2ip` library. The fast path that takes file data past lwIP is a module of its own,
`iop/tcpip/mxfast`.

`src/ps2ip.c`, `src/sys_arch.c`, `src/exports.tab`, `src/imports.lst`, `src/irx_imports.h` and
`include/` came from ps2sdk's `iop/tcpip/tcpip-netman` and `tcpip-base` (its netman side is gone). Licence: `LICENSE`
(Academic Free License 2.0). maxsdk owns these copies and changes them.
