#ifndef MX_DEBUG_H
#define MX_DEBUG_H

/* Debug output. Text goes to the enabled sinks:
   - MX_SINK_SIO: the EE serial port (PCSX2 shows it on its EE console);
   - MX_SINK_TTY: the IOP tty through the file server (ps2client on the PS2).
   With mx_debug_hold(1), text is only collected and sent by mx_debug_flush(),
   so timed code is not disturbed by SIF traffic. */

#include <mx/types.h>
#include <stdarg.h>

#define MX_SINK_SIO 1
#define MX_SINK_TTY 2

void mx_debug_init(void);
void mx_debug_sinks(int sinks);
int mx_debug_get_sinks(void);
void mx_debug_hold(int hold);
void mx_debug_flush(void);
void mx_debug_write(const char *s, u32 n);

/* Report CPU exceptions (address and bus errors, TLB misses, reserved
   instructions, ...) on the debug output with the registers, HI/LO, the FPU,
   the top of the stack and a "trace:" line (EPC, ra and the likely return
   addresses on the stack), then exit with status 255. tools/symbolize.py
   turns the trace into functions and source lines. The startup installs it
   before main. */
void mx_exception_init(void);

/* The debug screen: where a program is when it hangs, on the TV, for when no
   text comes back. A background colour and 32 hex words (8 lines of 4,
   slot 0 at the top left), written straight to the GS registers and the
   GIF FIFO: after mx_dbg_screen() they work with interrupts off, in
   interrupt handlers and exception handlers (no DMA, no interrupts, no
   allocation). A word is not drawn while the GIF channel runs (it waits
   up to 30 ms) or PATH3 is masked. */

/* Show the debug screen instead of the picture (mx_gs_init(640, 448) first
   if the GS is not set up). Thread code, once. */
void mx_dbg_screen(void);
/* The background colour, 0xBBGGRR. */
void mx_dbg_color(u32 rgb);
/* Hex word v in slot 0..31. */
void mx_dbg_word(u32 slot, u32 v);
/* Step n: slot 0, and a background colour of 8 that it picks
   (n & 7: red, green, blue, olive, purple, teal, grey, orange; dark, so
   the words stay readable). */
void mx_dbg_step(u32 n);
/* A heartbeat from the vblank-end interrupt: slot 31 counts up about 4
   times a second and slot 30 is where the program was (the EPC of that
   interrupt). It stops when interrupts stay off. Thread code. */
void mx_dbg_heartbeat(int on);

int mx_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int mx_vprintf(const char *fmt, va_list ap);
int mx_snprintf(char *buf, u32 size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int mx_vsnprintf(char *buf, u32 size, const char *fmt, va_list ap);

#endif
