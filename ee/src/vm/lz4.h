#ifndef MX_VM_LZ4_H
#define MX_VM_LZ4_H
#include <mx/types.h>
/* LZ4 block format (lz4.c), for one page of at most 64 KiB.
   compress: in[0..n) to out (room bytes): its length, 0 when it does not
   fit. decompress: in[0..n) to out (room bytes): the bytes made, 0 when the
   block is bad. Neither is thread-safe (one hash table). */
u32 mx__lz4_compress(const u8 *in, u32 n, u8 *out, u32 room);
u32 mx__lz4_decompress(const u8 *in, u32 n, u8 *out, u32 room);
#endif
