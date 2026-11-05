#ifndef IOP_IRX_IMPORTS_H
#define IOP_IRX_IMPORTS_H

#include "irx.h"
#include "intrman.h"
#include "loadcore.h"
#include "stdio.h"
#include "thbase.h"
#include "timrman.h"

/* clang puts file-scope asm before all data, so the head of an import table
   has to be file-scope asm too (see tools/mxlink/iop/irx_clang.h) */
#define IRX_CLANG_STR(val) STR(val)
#undef DECLARE_IMPORT_TABLE
#define DECLARE_IMPORT_TABLE(modname, major, minor)                                        \
  __asm__(".section\t.text\n\t.balign 4\n\t"                                               \
          ".word " IRX_CLANG_STR(IMPORT_MAGIC) ", 0\n\t"                                   \
          ".short " IRX_CLANG_STR(IRX_VER(major, minor)) ", 0\n\t"                         \
          "9:\n\t.ascii \"" #modname "\"\n\t.space 8 - (. - 9b)");

#endif
