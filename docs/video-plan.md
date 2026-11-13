# Video and pictures: the plan

Decoding of pictures and video for every maxsdk program, on the EE's IPU (EE User's Manual,
ch. 8) where it helps. All of it is new code in C; nothing is taken from Sony's libmpeg or
ps2sdk's. What is measured about the IPU: [ipu.md](ipu.md).

## What the hardware gives

The IPU decodes MPEG-1/2 macroblocks: the variable-length codes, the inverse quantization, the
IDCT, and the conversion to RGB32 or RGB16 (with an ordered dither), or to 4-bit indices of a
given CLUT. Its limits decide the plan:

- **No motion compensation.** A picture of intra macroblocks only (an I picture) is decoded by
  the IPU alone (`IDEC`, a slice a command). For P and B pictures it decodes a macroblock's
  blocks (`BDEC`); the EE adds the prediction and has the IPU convert the result (`CSC`).
- **4:2:0 only**, sizes in whole macroblocks (16 x 16).
- **The codes are MPEG's.** JPEG has the same transform but its own Huffman tables, and
  its samples use the full range where the IPU's conversion is BT.601's 16–235: a JPEG is
  decoded on the EE (`<mx/pic.h>`).
- Output comes a macroblock at a time (16 x 16 pixels each), not in the picture's lines.

| What | Path | Expected limit (to be measured) |
|---|---|---|
| MPEG-2/MPEG-1 video, I pictures only | IPU alone | measured: 720 x 576 decodes in 5.3 ms (`ipu.md`), so 50/60 a second with time to spare; 1280 x 720 (3600 macroblocks, about 12 ms) into a CT16 frame buffer to be tried |
| MPEG-2/MPEG-1 with P and B pictures | IPU + EE | MP@ML, 720 x 576 at 25, 720 x 480 at 30 |
| JPEG (baseline), PNG | EE | any size that fits; not at video rates |
| MPEG-4, H.264 and later | not planned | no help from the IPU beyond the colour conversion |

The 4 MiB of GS memory, not the IPU, ends it: 1280 x 720 fits as CT16 only when decoded
straight into the buffer shown; 1920 x 1080 does not fit. I pictures only take about 1–1.5
MB/s at 720 x 576: more than a USB stick gives on the PS2 (about 0.9 MB/s), fine from the
HDD or the network.

## Files

Standard files, made with ffmpeg; no converter of our own:

- video: an MPEG-2 (or MPEG-1) video elementary stream, `.m2v`
  (`ffmpeg -i in -c:v mpeg2video -g 1 -q:v 3 -s 640x448 out.m2v` for I pictures only);
- with sound: an MPEG program stream, the video with PCM (or MPEG audio layer II) beside it;
- a still picture: one I picture (the same decoder), or a JPEG.

Sony's `.ipu` and `.pss` are not needed: `.ipu` exists to have the macroblocks in columns
(so that the IPU's output is a strip of the picture), which the upload chain below makes
unnecessary.

## Layers

| Layer | Header | What |
|---|---|---|
| The unit | `<mx/ipu.h>` | every IPU command, its two DMA channels, waiting asleep (interrupts), the stream's position |
| MPEG video | `<mx/mpeg.h>` | headers parsed on the EE, pictures decoded: I by `IDEC`; later P/B by `BDEC` + motion compensation in C (MMI through the compiler's vectors) + `CSC` |
| Into GS memory | `<mx/mpeg.h>` | a DMA chain, built once for a picture size, that sends the decoded macroblocks to their places in a texture or the frame buffer (one IMAGE transfer a macroblock, the data by reference) |
| The program stream | `<mx/mpegps.h>` | a file's packets taken apart: video, LPCM sound, time stamps |
| A movie | `<mx/movie.h>` | a file as the source, a thread that reads ahead, the pictures decoded when due, sound through `mx_snd`'s stream in step with them |
| Pictures | `<mx/pic.h>` | a still from a JPEG or PNG file to pixels the GS takes, on the EE |
| libgl | | a decoded picture as a texture (video in a 3D scene) |

Under mx OS the IPU belongs to the program in front, as the GS, VIF and VUs do (the kernel
resets it when a program ends). The decoder is a library in the program, not a service. For
the home button (mx OS plan, step 5) a movie stops at a picture and loads the IPU's tables
again when it goes on; the shell may use the IPU in between.

Rules that hold throughout: no busy waits (the IPU's interrupt and its channels' wake a
semaphore); streams and pictures in VM are pinned in one piece for the DMAC; a retail PS2's
32 MiB.

## Steps

Each step counts once it ran on the PS2.

| Step | What | State |
|---|---|---|
| V0 | `<mx/ipu.h>`, `tests/ps2/ipu.c` (every command on streams made in the test), the registers in `ee/gen/hw_desc.py` | done: PS2 and PCSX2, 2026-11-13 |
| V1 | `<mx/mpeg.h>`: sequence and picture headers, I pictures by `IDEC`, the upload chain; `tests/ps2/mpeg.c` on streams made by ffmpeg from Big Buck Bunny (`tools/mpeg-fixtures.sh`) against ffmpeg's decoding; `examples/movie` plays an `.m2v` from `host:` as it reads it, `examples/moviehd` one of 1920 x 1080 in 1080i | decodes right on the PS2 and PCSX2; the example ran on the PS2 (its picture on the TV not yet confirmed); open: the upload's time, RGB16 into a CT16 buffer, 1280 x 720 |
| V2 | `<mx/movie.h>`: a movie from a file for any program: the program stream taken apart (`<mx/mpegps.h>`), a reading thread, LPCM sound through `mx_snd`'s stream with its rate steered to the picture (a linear interpolation on the EE, 3 parts in a thousand at most; a jump when far off), buffers from the pinned and the pageable heap. `mx_movie_update` once a frame for a frame loop (`examples/movie`), or `mx_movie_decode` / `mx_movie_shown` / `mx_movie_poll` for a player that times pictures itself (`examples/moviehd`) | done on the PS2, 2026-11-13: 1920 x 1080 at 24 a second with sound, no picture late, the sound within -3 .. 9 ms of the picture by the stream's count (SCPH-90006); 640 x 384 in a 50 Hz frame loop, 720 pictures in 1502 frames (SCPH-70004). Since: `MX_MOVIE_LOOP`, `mx_movie_pause`, `mx_movie_rewind` (a 6 s clip three times over: 432 pictures in 906 frames of 50 Hz), and a movie is over when its last sound has played. To do: MPEG audio layer II (its synthesis window is a table of the standard's: to be taken from there, not written from memory); time stamps beyond the start; seeking |
| V3 | P and B pictures (`mx_mpeg_refs`): the macroblock's header and vectors by `VDEC`/`FDEC`, its blocks by `BDEC` (the IPU's interrupts off, its commands looked at: `mx_ipu_direct`), the prediction in C eight pixels at a time in 64-bit registers while the IPU decodes, three pictures in Y, Cb, Cr, the picture to show converted a row of macroblocks a `CSC`; frame pictures with frame and field prediction, MPEG-2 and MPEG-1; `<mx/movie.h>` finds such a stream out by its second picture | decodes right on the PS2 (three streams against ffmpeg's decoding, mean difference 0.05 of 255) and PCSX2. Speed (`ipu.md`): 640 x 384 in 12.4 ms a picture, 720 x 576 in 20.7 ms: both play at 24 a second with sound, no picture late. To do: field pictures, dual prime, concealment vectors |
| V4 | `<mx/pic.h>`: stills to pixels for the GS (CT32, pinned memory): JPEG (baseline and extended sequential, grey and colour with any of the usual chroma sizes, restart intervals) and PNG (all colour types and depths, not interlaced; its own inflate), both in C on the EE; `tests/ps2/pic.c` against ffmpeg's decoding (PNG the same pixels, JPEG within 3 of 255); `examples/picture` shows one | done on the PS2 and PCSX2, 2026-11-13: 640 x 384 read from `host:` and decoded in 78 ms (JPEG, 42 KB) and 292 ms (PNG, 336 KB; 454 ms before inflate looked codes of up to 9 bits up in a table). To do: progressive JPEG; RGB16 with dither and the IPU's 4-bit CLUT (`PACK`) for textures that must be small; a still of I-picture MPEG is `<mx/movie.h>`'s |
| V5 | For a graphics library: `mx_movie_raster` / `mx_mpeg_raster` give the picture as lines of pixels (`glTexSubImage2D` takes them); to do: libgl taking the macroblocks without the copy, stop and go on for the home button (with the kernel's step 5), the shell's use (backgrounds, previews) | the copy is there and tested; the rest is open |

## 1920 x 1080 and 1280 x 720

Film in full HD plays as I pictures (`examples/moviehd`, `ipu.md`): decoded to RGB16 with
dither into the one buffer that fits GS memory (1920 x 1088 in CT16, 510 of the 512 pages),
which is the buffer shown: no second buffer, no textures, nothing else on the screen.

- A picture is sent from the vblank it is due on: the transfer goes down the buffer in 6.7 ms,
  the screen reads it in a frame's time, so the read-out never passes what is being written.
  Sent from anywhere else in the frame it tears (seen on the TV with the first version, where
  most pictures went out late).
- One thread is enough: the file is read while a picture is sent, then the next is decoded:
  35 ms of the 41.7 ms a picture has at 24 a second (decoding 23.5 ms, sending with the
  reading 11.7 ms). 720 pictures, none late (SCPH-90006, 2026-11-13).
- 1080i (the BIOS's mode, FIELD: a film frame 2 and 3 fields at 60 Hz) and 1080p24
  (`mx_video_1080p24`, with `mx_video_1080p25`: 1080p30's timing with a line of 2750 and 2640
  clocks) both give a picture on the TV tried, over component.
- 1280 x 720 in 720p (60 Hz, a picture 2 and 3 vblanks): decoding 10.8 ms. Its buffer is 240
  pages, so two would fit.
- No 720p at 24 Hz: its line is 4125 clocks of 74.25 MHz with the picture in the first third.
  The front porch field holds 2047, and the CRTC's description (`mx_crtc_regs`) wants the
  middle of the line inside the picture; moving the picture there puts it 39 % to the right
  of where a TV looks for it. Not tried on the GS.
- The data: 90 KB a picture here (a 480p source scaled up); real 1080 lines will be 150-300
  KB, 4-7 MB/s: the network's fast path (about 9 MB/s) or the HDD.
- The program's core memory: the picture (4.2 MiB), its chain (0.8 MiB) and the stream's
  buffer. A first version with 7.3 MiB of core did not start: the kernel stopped while
  loading it (after "VM pool", before "loaded") and the console had to be switched off. With
  5.5 MiB it runs. For the kernel's side to look at.

## P and B pictures: where the time went

640 x 384 (960 macroblocks), by counters in the first version of the decoder: of 26.3 ms a
picture, the prediction took 8.3 ms, reading the macroblocks' headers and vectors from the IPU
about 6.5 ms, the conversion for showing 6.5 ms, the blocks into the picture 3.6 ms, BDEC 2.0
ms. It is 12.4 ms now (720 x 576: 20.7 ms), by:

- pictures kept as macroblocks (the IPU's RAW8) and not as planes: `CSC` takes a picture as
  it lies (no copy for showing), and a prediction reads about half the cache lines (its
  reference block is in two to four macroblocks side by side, fetched into a small buffer on
  the stack; the prediction is bound by memory, the references are not in the 8 KiB cache);
- `FDEC` and `VDEC` written and read in line (the result and its busy bit are one register),
  not through a call and a wait's loop;
- the blocks added and clipped sixteen at a time in 128-bit registers (the compiler's
  vectors: `pextlb`, `paddh`, `pmaxh`, `pminh`, `ppacb`);
- the IPU decoding a macroblock's blocks while the EE makes its prediction.

Left to try: a prefetch of the reference as soon as the vectors are known; the headers parsed
on the EE from memory.

Open points:

- P/B decoding uses the scratchpad for the prediction (toSPR, fromSPR), as Sony's did; a
  program that uses the scratchpad itself has to share it. Decide in V3, after measuring
  whether main memory through the cache is fast enough.
- With sound, a picture needs what belongs to it read well ahead: mxsnd's stream holds a
  tenth of a second, so the samples must be on the EE about a fifth of a second before they
  are heard. With 224 KiB of video read ahead (100 ms at 1080 lines) the sound ran dry and
  stayed 100 ms late; with 448 KiB it holds. `<mx/movie.h>` should read ahead by time, not
  by bytes.
- Reading in the main thread does not fit 1080 lines once sound is on (a read took 10-11
  ms beside the sound, 7 without): a picture missed its vblank every other time. The
  reading thread is needed, not a refinement.
- The interrupts of a picture (FDEC and IDEC a slice: about 110 for 576 lines): whether
  polling FDEC, which ends within cycles, saves time worth having.
- A stream held in VM: pinned in one piece now; in pieces would need the position kept
  across `mx_ipu_more`.
