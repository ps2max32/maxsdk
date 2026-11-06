# mxnet

The TCP/IP stack on the IOP, `ps2ip-nm.irx`: lwIP from `libs/lwip`, on smap and netman, plus
the fast path that takes file data past lwIP (`src/mxfast.c`, `common/include/mxfast.h`).

`src/ps2ip.c`, `src/sys_arch.c`, `src/exports.tab`, `src/imports.lst`, `src/irx_imports.h` and
`include/` came from ps2sdk's `iop/tcpip/tcpip-netman` and `tcpip-base`. Licence: `LICENSE`
(Academic Free License 2.0). maxsdk owns these copies and changes them.
