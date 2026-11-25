# MIDI music on the IOP: plan

Status: decided and built 2026-11-25. Passing on the PC, PCSX2 and a PS2 (ROM 2.00) (`tests/host/test_midi.c`,
`test_midiseq.c`, `tests/ps2/midi.c`, `k90_music.c`; Duke's level 1 heard on the PS2).
The EE's synthesiser is removed. The only user is the Duke
Nukem 3D port (changed to the new interface). Doom (OPL music mixed on the EE) is the likely second.

## Why

The SPU2 already plays the notes and `mxsnd.irx` already writes its registers. What is on the EE:

- the sequencer, run by the game once a frame. A note is up to a frame late, and while the EE
  is busy (a level's load, page faults) the music stops and held notes go on sounding;
- the synthesiser: a MIDI message to voices, pitches and volumes.

Moving the synthesiser alone gains nothing, as the messages would still come once a frame.
**Sequencer and synthesiser go together, into mxsnd.**

## Measured (TimGM6mb, `sf2bank -r 32000`)

| | Bytes |
|---|---:|
| Samples (SPU RAM) | 1,707,952 |
| SPU RAM for samples in all (0x5040 to 0x1C7000) | 1,843,136 |
| Left for the game's own samples | 135,184 |
| Bank tables: 521 samples, 136 presets, 2063 zones | 50,752 |
| mxsnd.irx in IOP memory before (78,368 of it .bss) | 85,215 |
| with ownership and music (81,812 of it .bss) | 99,892 |

- The tables in IOP memory cost about 50 KiB while music is loaded. Reading only the presets a
  song uses makes that smaller; not measured.
- A game with effects in SPU RAM needs a smaller bank. `sf2bank` gets an option for it (the
  programs to keep, or a lower `-r`). Duke and Doom mix their effects on the EE into the stream
  and do not have the problem.
- Songs: Duke's 30 MIDIs are 7,185 to 61,838 bytes (format 1, 6 to 23 tracks, 67 programs in
  all); Doom's 45 and Doom II's 35 MUS files at most 59,535 and 64,808 (as MUS, before
  `mus2mid`). With the bank's tables a Duke song takes at most 112,590 bytes of IOP memory.
- Duke's notes: at most 18 keys down at once and 23 notes starting within one 10.67 ms step,
  counted over all tracks (EMIDI leaves some out), a key as one voice: zones layered on a key
  and releases come on top.
- Not measured: free IOP memory with all services loaded, the synthesiser's
  time on the IOP.

## Design

1. **In mxsnd, not a module of its own.** It needs mxsnd's lock and its key-on/key-off batch.
   Song and tables are allocated when loaded, so a program without music pays the code only.
2. **Clock: the play thread.** It wakes every 512 frames (10.67 ms) with the audio; each time
   the sequencer does the events due. Notes are quantised to 10.67 ms, no drift. If that is
   heard, an IOP timer is the next step, as an addition.
3. **Fixed point.** The R3000A has no FPU: `iop/sound/mxsnd/src/midi_fix.h`, pitch and attenuation
   from 2^(n/12) and 2^(n/1200) in Q16 plus an octave shift, 32x32 multiplications otherwise.
   `tests/host/test_midi.c` compares it with the same formulas in floating point.
4. **Voices: a fixed share.** While a bank is loaded, music owns the last N voices (24 by
   default), and the EE's allocator uses the rest. Two allocators over the same voices would
   need a round trip. Voice stealing for music works on N only: the released longest ago, then
   the oldest note.
5. **Songs: standard MIDI, format 0 and 1**, parsed on the IOP with a read position per track.
   Looping is a flag. EMIDI's controllers
   are needed: all 30 of Duke's songs have some (110, 111 include and exclude a track, 112
   program change, 116, 117 loop start and end, 118, 119 the same for the song). Doom's MUS becomes MIDI through its own `mus2mid`.

## Interface

    int  mx_midi_load(const void *bank, u32 size);       /* as now; the tables go to the IOP */
    void mx_midi_free(void);
    int  mx_midi_song(const void *smf, u32 size);        /* upload; replaces the song */
    int  mx_midi_play(int loop);
    void mx_midi_stop(void);
    void mx_midi_pause(int on);
    void mx_midi_volume(int volume);
    int  mx_midi_playing(void);

Set-up (bank, song) by RPC, through the upload buffer. Play, stop, pause and volume are
mxsnd commands, in order with the rest. `mx_midi_send`, `mx_midi_reset` and the EE's
synthesiser go: a game hands its song over. `MXSND_VERSION` becomes 2. No old interface stays
beside the new one: every user changes with it.

The status record's last word holds the song's state (`MXSND_F_SONG`).
Built: `mx_midi_stop` and the rest as above; a song and a bank stay readable while they are
in use, as they are handed over again after another program had music.

## Lifetime

Agreed and built 2026-11-25 (`kernel.md`, version 48); the first point says how it was before:

- **The gap before, without music too.** A second INIT replaces mxsnd's owner (`ee_status`); the
  first program still counts as ready, so it never does INIT again. Its status is no longer
  written, its command ring looks full, and its old write counts can make `commands()`
  (`r != w`) replay old entries.
- **Owner: status area + process id.** INIT claims. A new `MXSND_RESUME` claims again and
  returns the counts as INIT does, without resetting the song. `MXSND_STOP` takes the process
  id in place of the memory range. Two kernel points to check: the range after a front switch
  (`exec_end_range`), and `side_end_now`, which does not call `sound_off`.
- **Song.** It keeps its own owner. Another program's INIT pauses it; its owner's STOP stops
  it and frees its memory, whoever `ee_status` is.
- **SPU RAM: the front process's, contents not kept** (as GS memory). mxsnd counts uploads by
  another owner; RESUME tells whether SPU RAM was touched, and only then maxsdk uploads the
  bank again. The bank's source stays in VM. Plain `mx_snd_load` samples need a "lost" callback
  or a kept source.
- **Return to the front.** `mx__snd_quiet` stays and pauses the song too. A new weak
  `mx__snd_front()` does RESUME, the reload, then the song. It must run only in the front
  process (a parent runs beside its child), which needs a small kernel service and so a new
  kernel version. Voices do not restart; the program plays again.
- **Bank free:** the song stops on the IOP, and the answer comes back, before the SPU RAM is
  reused.

## Steps

1. Measure: the songs (done), free IOP memory under the kernel.
2. Ownership, on its own, with a test: a child calls `mx_snd_init`, then the parent plays and
   checks `mx__snd_dropped` and `cmd_r`.
3. The return hook and the reload.
4. Fixed-point tables and the PC test.
5. Synthesiser and sequencer in mxsnd, `ee/src/base/midi.c` only the calls (the EE's synthesiser
   deleted in the same change).
6. `tests/ps2/midi.c` on songs with known note times, checked by the voices' mix.
7. Duke on it (JFAudioLib's driver hands the song over); heard on a PS2, the other model is open.
8. Doom: decided again after Duke has been heard.

`README.md`, `bios.md`, `mx-os.md` and `kernel.md` change with the code they describe, per step.

Kept: `tools/sf2bank.c` and the bank format. `mx_snd_ch_play_env` and `mx_snd_sample_part` were
added for the EE's synthesiser: they go with it unless another user is found.

## Decisions

- Music owns 24 of the 48 voices while a bank is loaded.
- 10.67 ms steps first, an IOP timer only if the steps are heard.
- Doom keeps OPL until Duke's music is good.
- No legacy interface: all users are in our own repos and are changed.
- The kernel parts of Lifetime: agreed (kernel 48).
