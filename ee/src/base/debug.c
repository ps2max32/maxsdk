#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/hw/regs.h>
#include <mx/sif.h>

/* The tty reaches ps2client on the PS2 and the IOP console on PCSX2. The
   serial port is the fallback when the IOP does not answer. */
static int sinks = MX_SINK_TTY;
extern const int mx__debug_start_sinks __attribute__((weak));
static int hold;
/* the kernel (apps/mxos) builds its maxsdk with a small one: it never holds,
   and its memory below its code is short */
#ifndef MX_DEBUG_HOLD_BYTES
#define MX_DEBUG_HOLD_BYTES 16384
#endif
static char hold_buf[MX_DEBUG_HOLD_BYTES];
static u32 hold_len;

static void sio_write(const char *s, u32 n) {
  for (u32 i = 0; i < n; i++) {
    if (s[i] == '\n') {
      while ((MX_R32(MX_SIO_ISR) & 0xF000) == 0x8000)
        ;
      *(volatile u8 *)MX_SIO_TXFIFO = '\r';
    }
    while ((MX_R32(MX_SIO_ISR) & 0xF000) == 0x8000)
      ;
    *(volatile u8 *)MX_SIO_TXFIFO = (u8)s[i];
  }
}

/* the kernel's log (apps/mxos, klog.c): its own output too */
void mx__debug_tap(const char *s, u32 n) __attribute__((weak));

static void send(const char *s, u32 n) {
  if (mx__debug_tap)
    mx__debug_tap(s, n);
  if (sinks & MX_SINK_SIO)
    sio_write(s, n);
  if ((sinks & MX_SINK_TTY) && mx_tty_write(s, n) < 0) {
    sinks = (sinks & ~MX_SINK_TTY) | MX_SINK_SIO;
    sio_write(s, n);
  }
}

/* Bind the file server now, at the start: binding it at the first output,
   in the middle of a program's DMA and interrupt set-up, lost output on the
   PS2 (the first line, and later ones). */
void mx_debug_init(void) {
  /* mxlink: the IOP a program left may not answer, and its screen comes first */
  if (&mx__debug_start_sinks)
    sinks = mx__debug_start_sinks;
  if (sinks & MX_SINK_TTY)
    mx_tty_write("", 0);
}

void mx_debug_sinks(int s) { sinks = s; }
int mx_debug_get_sinks(void) { return sinks; }

/* Not held: whole lines. ps2link's tty sends each write as one UDP packet,
   and of a line written in several pieces some were lost on the PS2. */
static char line_buf[512];
static u32 line_len;

static void flush_line(void) {
  if (line_len)
    send(line_buf, line_len);
  line_len = 0;
}

void mx_debug_flush(void) {
  flush_line();
  if (hold_len)
    send(hold_buf, hold_len);
  hold_len = 0;
}

void mx_debug_hold(int h) {
  hold = h;
  if (!h)
    mx_debug_flush();
}

void mx_debug_write(const char *s, u32 n) {
  if (!hold) {
    for (u32 i = 0; i < n; i++) {
      line_buf[line_len++] = s[i];
      if (s[i] == '\n' || line_len == sizeof line_buf)
        flush_line();
    }
    return;
  }
  flush_line();
  if (hold_len + n > sizeof hold_buf)
    mx_debug_flush();
  if (n > sizeof hold_buf) {
    send(s, n);
    return;
  }
  for (u32 i = 0; i < n; i++)
    hold_buf[hold_len++] = s[i];
}

void mx_exit(int status) __attribute__((noreturn));

/* MX_E_CHECK (<mx/emit.h>) in MX_DEBUG builds */
void mx__emit_overflow(const char *file, int line) {
  mx_debug_hold(0);
  mx_printf("emit: buffer overflow at %s:%d\n", file, line);
  mx_exit(99);
}
