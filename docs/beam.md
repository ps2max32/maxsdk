# Where the beam is

Beam racing (`<mx/hires.h>`, `ee/src/gs/hires.c`) draws a strip of the picture right after the
beam has shown it. For that it must know at which H-blank count after the vblank interrupt the
beam starts each strip. This page says how that count is reckoned, how it was measured, and how
to measure it again: on another console, in another video mode, or after a change to the
display or the timing code.

## What a wrong line looks like

A pass first clears its strip. If it starts while the beam still shows the strip's last lines,
those lines are shown cleared, and with what the pass has drawn so far:

* dark lines where two strips meet, and surfaces missing there;
* the bottom of the picture cut off (the last strip's pass);
* worse at small widths: the clear of a strip 960 wide takes about 6 lines of the beam, one
  1920 wide twice that, so a wide picture hides an error of some lines.

A line reckoned too late costs nothing but time: the passes start later than they could.

## The rule

In H-blanks counted from the vblank interrupt, the picture's first line is at

    top = DY + 4 + front porch + 1        (in lines of a field; see the units below)

* The vblank interrupt comes at the end of the timing's active lines (`vdp` of `struct mx_crtc`),
  not at the end of the picture.
* `front porch` is `vfpe + vfp` of the mode's `struct mx_crtc`.
* `DY` is DISPLAY2's DY of the picture as it is shown. `+ 4` is what gsKit's hires code adds.
* Units: DY and the 4 are frame lines in an interlaced mode (so halved for a field); the
  timing's numbers are half lines in the TV modes (NTSC, PAL, also 240p and 288p) and in every
  interlaced mode, whole lines otherwise. `hires.c` reckons in half lines.

Strip `j` then starts at `top + j * (the strip's lines)`, and the picture ends at `top + its
lines`, which is after the next interrupt when that is more than a field's lines.

`mx_hires_margin` (8 lines by default) is added to each of these: it covers what the rule is
off by, and the interrupt's delay.

## The measurement

`examples/beam` shows where the beam really is. It is read from a TV; nothing in the console
can tell the beam's line.

* The passes start **early**: 40 lines (12 in strips of 64 lines) before the line `hires.c`
  reckons the strip's end at (`mx_hires_margin` below 0).
* A pass clears its strip to magenta, fills it some more times, then draws it black with a
  scale. So each strip's last lines show a magenta band from the line the beam was at when the
  pass started.
* The scale's numbers are the lines the beam is later than reckoned. **The number at the top
  edge of each band is the error.** The green line is 0: the rule is right.
* The clear takes a few lines to reach the band's line; the scale is drawn lower by that (from
  the fills' time, measured in the first three seconds, and the mode's time a line).
* There is a band at the end of every strip. The last one is the picture's end: it tests the
  pass that waits for the last strip.

To run it, on a PS2 under mx-kernel:

    beam.elf m<mode> [e<lines early>] [s<seconds>]

with the mode one of `ntsc pal 240p 288p 480p 576p 720p 720p50 1080i 1080i50 1080p30 1080p25
1080p24` (1080i without). Start on the pad ends it; 120 seconds do too. The first line of the
screen names the mode, the fills and their time, a pass's time, and the passes that were late.

What to check on the screen:

* The same number at every band. If they differ, the strips' spacing is wrong, not the top.
* `LATE 0`. A pass that had to wait for the one before starts late and its band says nothing.
* A band whose top edge is above the scale, or no band: the error is larger than the scale
  shows. Run again with another `e`.
* A band's top edge moving up and down by more than a line: the interrupt or the timer is
  delayed (something holds interrupts off).

## What was measured

2026-11-20, a SCPH-90006 on a flat TV through component, every TV mode. First with DY read as 0
(see below), which gave the errors in the second column; from those and each mode's DY the
picture's first line, which the rule gives within the reading's two lines (PAL three):

| Mode | Error with DY 0 | First line, measured | The rule |
|---|---|---|---|
| 1080i, 1080i50 | 22 | 24 | 24 |
| 720p, 720p50 | 30 | 34 | 34 |
| 480p | 46 | 50 | 49 |
| 576p | 48 | 52 | 54 |
| 1080p30, 25, 24 | 45 | 49 | 49 |
| NTSC | 30 | 32 | 31 |
| 240p | 31 | 35 | 34 |
| PAL | 38 | 40 | 42 |
| 288p | 38 | 42 | 45 |

With the rule in `hires.c` all thirteen modes were shown once more: the bands start at the green
line, as far as the eye reads it from the TV. By the table PAL and 288p are reckoned two or
three lines late, the safe side. Only 1080i was read to the line; the others from the scale's
numbers, to two lines or so.

A fill of a strip 960 x 192, 16 bits with Z: 181 us (6 lines of 1080i's beam).

## Two mistakes this found

* **DY read from a display that is off.** `mx_hires_init` leaves the display off until the first
  record was played (kernel 41, 2026-11-18). It read DY from the registers it had just written
  for the display off, which are 0. So every strip's line was reckoned a blanking too early
  (22 lines in 1080i), the passes cleared lines the beam had still to show, and a margin of 32
  lines was needed to hide it. DY is now taken from the display as it will be shown
  (`mx_display_regs` of the circuits on).
* **The last strip had no margin.** Its pass started at the vblank interrupt. It now waits for
  the picture's end and the margin, as the others wait for their strip's.

## Changing it

* The rule is in `mx_hires_init` (`ee/src/gs/hires.c`), where `top` is reckoned; the events of a
  field (which pass may run at which count) are made right below it.
* A mode that reads off: add its lines there for that mode, then run `beam` again; the band
  must come to the green line.
* A new video mode: add it to `modes[]` in `examples/beam/beam.c`. The buffer's height is the
  mode's lines of a field or frame, its width 640 (960 for 1080 lines).
* Another console or another TV standard converter between: run all modes once. The rule
  hangs on the GS's timing alone, so it should hold; this is not measured.
* The picture moved by the user (the kernel's screen position): DY moves with it only if the
  program's display has the offset. Not measured.
