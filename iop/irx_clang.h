/*
 * irx_clang.h - ps2sdk's import and export tables for clang (Rules.mk,
 * IOP_CLANG=1; the import half is mxlink's iop/irx_clang.h).
 *
 * irx.h's DECLARE_IMPORT_TABLE and DECLARE_EXPORT_TABLE are a C variable
 * followed by file-scope asm for the entries, which needs gcc's
 * -fno-toplevel-reorder to stay in that order. clang has no such option and
 * puts file-scope asm first, so here each table's header is file-scope asm as
 * well. Force-included when compiling imports.lst and exports.tab.
 */

#ifndef IOP_IRX_CLANG_H
#define IOP_IRX_CLANG_H

#include "irx.h"

#define IRX_CLANG_STR(val) STR(val)

/* struct irx_import_table up to its stubs: magic, next, version, mode, name */
#undef DECLARE_IMPORT_TABLE
#define DECLARE_IMPORT_TABLE(modname, major, minor) \
	__asm__ (".section\t.text\n\t"		\
		".balign 4\n\t"			\
		".word " IRX_CLANG_STR(IMPORT_MAGIC) ", 0\n\t"	\
		".short " IRX_CLANG_STR(IRX_VER(major, minor)) ", 0\n\t"	\
		"9:\n\t.ascii \"" #modname "\"\n\t"	\
		".space 8 - (. - 9b)");

/* struct irx_export_table up to its function pointers, as _exp_<modname>
   (RegisterLibraryEntries(&_exp_<modname>)): magic, next, version, mode,
   name */
#undef DECLARE_EXPORT_TABLE
#define DECLARE_EXPORT_TABLE(modname, major, minor) \
	__asm__ (".section\t.text\n\t"		\
		".balign 4\n\t"			\
		".globl _exp_" #modname "\n\t"	\
		".type _exp_" #modname ", @object\n"	\
		"_exp_" #modname ":\n\t"		\
		".word " IRX_CLANG_STR(EXPORT_MAGIC) ", 0\n\t"	\
		".short " IRX_CLANG_STR(IRX_VER(major, minor)) ", 0\n\t"	\
		"9:\n\t.ascii \"" #modname "\"\n\t"	\
		".space 8 - (. - 9b)");

#endif
