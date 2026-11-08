# Pads over USB and Bluetooth: the plan

DualShock 3, DualShock 4 and DualSense (PS3, PS4, PS5 controllers) as pads of mx OS, first
wired over USB, then over Bluetooth through a USB dongle with a Bluetooth host stack of our own.
Chosen 2026-11-08: our own minimal stack (Classic HID host only), USB first.

## Goal

- Any of these pads works in every `libmxk` program as a normal `mx_pad` port, without the
  program knowing where it comes from: SIO2 pads, USB pads, Bluetooth pads side by side.
- The kernel owns them, as it owns the SIO2 pad now ([mx-os.md](mx-os.md)): programs load no
  modules, and the pads (rumble, lights, pairing) outlive programs.
- Extras for programs that want them: analog triggers, touchpad, motion, battery, rumble,
  light bar and player lights, the PS button.
- Pairing, listing and forgetting pads from the kernel's home menu; link keys kept on the
  memory card.

## Where it starts

- `mx_pad_*` goes through the kernel table (`pad_read`, `struct mx_pad_state`,
  [kernel.h](../ee/include/mx/kernel.h)), on `rom0:PADMAN` for ports 0 and 1.
- ps2sdk's prebuilt `usbd.irx` already runs for USB storage (`usbmass_bd`); new USB drivers are
  more LDDs on it.
- Reference only, not code to take over: OPL's pademu (`ds34usb`, `ds34bt`, GPL; DS3/DS4 only,
  one fixed link key, no SSP, pairing over USB only, hooks into PADMAN/SIO2), BTstack
  (BlueKitchen: free for non-commercial use only), the Linux `hid-sony` / `hid-playstation`
  drivers for the report layouts.

## Design

```
EE   program ── mx_pad_* ──> kernel (mxlink): merges the sources, assigns players, home menu
                                ^  one pad record per pad in EE memory (SIF DMA at each change)
IOP  PADMAN (ports 0/1)      mxpad.irx (new, maxsdk iop/usb)
                               ├─ USB HID: DS3 / DS4 / DS5 through usbd
                               └─ mxbt: HCI over USB (H2) -> L2CAP -> HID host -> same pad layer
```

- One IOP module, `mxpad`, with a record per pad: buttons, sticks, triggers, then touchpad,
  motion, battery, PS button. The IOP DMAs a record to the EE when it changes (at most once per
  report), as PADMAN does: reading a pad stays a memory read on the EE.
- No busy waits: usbd callbacks, semaphores, a thread per transport at most.
- Player assignment in the kernel: SIO2 pads first, then USB/Bluetooth pads in the order they
  connect, so programs that only read ports 0 and 1 work with a DualShock 4 alone.
- Output (rumble, lights) through an RPC to `mxpad`; the kernel turns rumble off and puts the
  lights back when a program ends.
- `mxpad` is small and always loaded with usbd; `mxbt` is part of it (or loaded with it), only
  active when a Bluetooth dongle (USB class E0/01/01) is attached.

## Steps

Each step runs on the PS2 on its own. Pads are tested one at a time (DS3, DS4, DS5; several
Bluetooth dongles, one plugged in at a time).

### 0. Probes

- A usbd LDD that dumps descriptors and raw reports of DS3, DS4 and DS5 over USB, and of each
  dongle: HCI Reset, Read BD_ADDR, Read Local Version (chipset, firmware), Read Buffer Size.
- Measure: report rate (DS4 over USB about 250 Hz, DS5 1 kHz), IOP load, latency from report to
  EE record, and USB storage throughput with a pad beside it (now about 900 KiB/s).
- Note which dongles need a firmware download (Realtek) and which work without (CSR8510,
  Broadcom BCM20702).

### 1. Wired USB

- DS3 `054C:0268`: feature report `0xF4` (SET_REPORT) starts its input; input report `0x01`
  (49 bytes); output `0x01` for rumble and the four player lights.
- DS4 `054C:05C4` (v1), `09CC` (v2), `0BA0` (wireless adapter): input `0x01` (64 bytes), output
  `0x05` (rumble, light bar).
- DS5 `054C:0CE6`, Edge `0DF2`: input `0x01` (64 bytes), output `0x02` (rumble, light bar,
  player lights, trigger effects later).
- Hot plug, several pads (through a hub too).
- Kernel: the merged ports, an extended pad record, the RPC for output. New kernel table
  version; `mx-os.md` and `kernel.md` in the same commit.

State (2026-11-08): **1a done**, `iop/usb/mxpad` (`common/include/mxpad.h`): the three pads on
USB, a slot each (up to 4), 32-byte records by SIF DMA into an area of EE memory the EE gives
(`MXPAD_AREA`), only when a pad's state changes; the slot's light at connect (DS3 LED n, DS4 and
DualSense light bar blue, red, green, pink, DualSense player lights); rumble and light bar by
`MXPAD_OUTPUT`. `probe/p36_mxpad.c` reads the area as the kernel will: a DS4 on the PS2 with
buttons, d-pad (diagonals as two bits), sticks, battery, rumble, and unplugged and plugged in
again, back in slot 0 each time. Lessons: a record must be a multiple of 16 bytes (SIF DMA
writes 16 at a time: a 36-byte record wrote over the next); usbd does not call back a pad's
pending transfers after it is unplugged, so a slot is freed at once and callbacks carry the
connection's generation.

**1b done** (2026-11-08): mxlink loads usbd and mxpad at the boot (optional: a card without
them boots as before) and gives mxpad its area above the code (`.padarea`: the upper half of
its TLB entry is read-only, so it is written by DMA and through the uncached alias only). A
program's ports: the SIO2 pads first, then the USB pads in the order they came. The home button
`MX_PAD_HOME` (`<mx/pad.h>`): a USB pad's PS button, and a SIO2 pad's Select + Start + L1 + R1
(translated in `mx__pad_read`), from any pad to every port. On the PS2 with `tests/ps2/vm27_pads.c`:
a DS4 and a SIO2 pad, the DS4 moves up when the SIO2 pad comes; Share and Options as Select and
Start; PS ends a program without the menu. Open: a SIO2 pad is found a moment after a program
opens the ports, so a USB pad is port 0 until then, and a SIO2 port that is plugged in flickers
for a moment; the home menu itself fails on kernel version 10 (the program lends no GS memory:
not the pads).

### 2. Bluetooth transport (HCI)

- USB class E0/01/01: commands on the control pipe, events on the interrupt pipe, ACL data on
  bulk in/out.
- HCI: reset and setup (event mask, class of device, local name, page scan, SSP mode), accept
  incoming connections, link key request and notification, IO capability and user confirmation
  for SSP "just works". With Classic SSP the controller does the cryptography: the host needs no
  ECDH.
- Inquiry and outgoing connections for pairing new pads (step 3).
- Dongles without firmware first; Realtek firmware from `mc0:/MXOS` later, if a dongle needs it.

### 3. L2CAP and HID host

- L2CAP basic mode: signalling, PSM `0x11` (HID control) and `0x13` (HID interrupt); a minimal
  SDP server only if a pad turns out to ask for one.
- DS3: no security. Paired over USB (feature report `0xF5` = the host's BD_ADDR); it connects to
  the host when its PS button is pressed; `0xF4` over Bluetooth starts its input.
- DS4: paired over USB (feature report `0x13`: host BD_ADDR and link key) or over Bluetooth
  (Share + PS, SSP). It starts with the short report `0x01`; reading feature report `0x02`
  switches it to the full `0x11`. Output `0x11` carries a CRC32.
- DS5: paired over Bluetooth (Create + PS, SSP); whether it can be paired over USB is to be
  checked. Full input report `0x31`; output `0x31` with a CRC32.
- Link keys and pad addresses in `mc0:/MXOS/BT.CFG` (through mxio).
- Reconnect: a paired pad connects to the host when its PS button is pressed.

State (2026-11-08): steps 2 and 3 work for the **DualShock 4** with the CSR8510 dongle, in
`iop/usb/mxpad/src/bt.c`. Paired over USB (feature `0x13`: the dongle's address and a new link
key, kept in the IOP's memory only so far), the DS4 on battery connects to the dongle when its
PS button is pressed; it asks for no link key until the host authenticates, sends an L2CAP
Connection Request for **SDP** (PSM `0x0001`) first, and hangs up when that is refused. Left
unanswered (as a PS4 does), with the host opening the HID control and then the interrupt
channel itself, it authenticates with the key, turns encryption on, and after GET_REPORT `0x02`
sends report `0x11` (the USB report's bytes from offset 3): its slot then reads as a USB pad's.
Linux (`hid-playstation`, `hid-sony`; BlueZ's input profile, `sixaxis` plugin and SDP server)
is the reference from here on, for the facts: BlueZ answers the SDP request and the pad opens
the channels itself, likely what the DualSense needs.

### 4. Kernel and home menu

- The PS button opens the home menu (beside Select + Start + L1 + R1).
- A "Controllers" page: pair a new pad, the list (type, connection, battery), forget a pad,
  swap players.
- A long press of the PS button switches a Bluetooth pad off (HCI Disconnect).
- At a program's end: rumble off, lights to the kernel's defaults.

### 5. Old programs (optional, last)

- PADMAN/SIO2 emulation (as pademu) for ps2sdk programs that talk to PADMAN themselves.

## Measurements (step 0)

`probe/p35_hid.c` with `probe/iop/hidprobe.c`: `p35_hid host:hidprobe.irx [seconds]`, on ps2sdk's
usbd, SCPH-70004.

| Device | What it shows |
|---|---|
| DualShock 4 v2 `054C:09CC`, 2026-11-08 | composite: audio on interfaces 0-2 (iso), HID on interface 3 (report descriptor 507 bytes), interrupt in `0x84` and out `0x03`, 64 bytes, interval 5. Feature `0x12` gives the pad's and the paired host's address, `0xA3` the firmware date. Input report `0x01`, 64 bytes, 250 a second (3.99 ms apart, 3.7-4.3 ms: the OHCI's interrupt tree rounds interval 5 down to 4), no errors in 20 s. Output report `0x05` over interrupt out accepted. IOP idle 91% (as without a pad). |

DualShock 4 report `0x01` over USB, checked button by button on the PS2 (2026-11-08): sticks
at 1-4 (LX, LY, RX, RY; 0..255, rest about 123-135), byte 5 the d-pad (low nibble, 8 = none)
and square/cross/circle/triangle (0x10/0x20/0x40/0x80), byte 6 L1 R1 L2 R2 Share Options L3 R3
(bit 0..7), byte 7 PS (bit 0) and touchpad click (bit 1), the upper 6 bits a counter, triggers
at 8 and 9 (0..255), battery at 30 (low nibble, 11 on the cable; bit 4 cable), first finger at
35 (bit 7 set: no touch) and 36-38 (12-bit x 0..1919, y 0..942). 45 s of it: 10772 reports, no
errors, IOP idle 98%, beside a USB card reader that usbd leaves to usbmass_bd.

DualShock 3 (Sixaxis `054C:0268`) over USB, 2026-11-08: one HID interface, interrupt in `0x81`
and out `0x02`, 64 bytes, interval 1, but it sends only 100 reports a second (9.96 ms apart),
and only after feature report `0xF4` (`42 0C 00 00`). Feature `0xF2` gives the pad's address
(4-9), `0xF5` the host it connects to (2-7, not reversed): what pairing over USB writes. Report
`0x01`, 49 bytes, checked button by button: byte 2 Select L3 R3 Start Up Right Down Left (bit
0..7), byte 3 L2 R2 L1 R1 Triangle Circle Cross Square, byte 4 PS (bit 0), sticks at 6-9 (LX
LY RX RY; at rest about 124/121/144/129: wider than the DS4's), pressure 0..255 at 14-25 (Up
Right Down Left L2 R2 L1 R1 Triangle Circle Cross Square: the triggers' analog values are 18 and
19), 29-31 charge and cable state (`02 EE 10` on the cable), motion sensors from 41. Output
report 1 (SET_REPORT, 48 bytes) accepted. 45 s: 4301 reports, no errors. (The probe's IOP idle
figure reads over 100% here: its 200 ms calibration at the start is too short; treat it as
"no measurable load".)

DualSense `054C:0CE6` over USB, 2026-11-08: composite like the DS4 v2 (audio on 0-2, HID on
interface 3, report descriptor 289 bytes), interrupt in `0x84` and out `0x03`, 64 bytes,
interval 4: 250 reports a second at full speed (3.99 ms apart; the 1 kHz is high speed only).
Feature `0x09` gives the pad's address (1-6, reversed), `0x20` the firmware (Jul 4 2025). Report
`0x01`, 64 bytes, checked button by button: sticks 1-4, triggers 5-6, counter 7, byte 8 the
d-pad and face buttons as the DS4's byte 5, byte 9 L1 R1 L2 R2 Create Options L3 R3, byte 10 PS
(bit 0), touchpad click (bit 1), mute (bit 2), first finger at 33-36 as the DS4's at 35-38.
Byte 53 read as battery level gave 0 on the cable (its high nibble, the charge state, non-zero):
to be checked against the raw byte. Output report `0x02` (63 bytes, interrupt out): light bar,
player light and rumble accepted. 45 s: 10865 reports, no errors, IOP idle 90%.

Bluetooth dongle 1, `0A12:0001` (CSR, bcdDevice 3164), 2026-11-08: class E0/01/01, events on
interrupt in `0x81` (16 bytes), ACL on bulk `0x02`/`0x82` (64 bytes), SCO on interface 1 (iso,
not needed). It sends a vendor event (`FF`) and a Hardware Error (`10`, code `37`) before the
first command; HCI Reset answers in 129 ms. HCI 3 / LMP 3 (Bluetooth 2.0 + EDR), revision and
subversion `0C5C`, maker 10 (CSR), ACL 310 bytes x 10, name "BT2.0". **No SSP** (2.1 and later):
enough for the DS3 (no security) and the DS4 (link key written over USB), likely not for the
DualSense. Inquiry mode 2 (with names) is refused (status `12`); mode 1 (with RSSI) works and
reports every answer, so one pad in pairing mode gives some 40 results in 5 s: a driver keeps
a list. The DS4 in pairing mode (Share + PS) was found at the address its USB report gives,
class `002508` (peripheral, gamepad), RSSI about -60 dBm.

Bluetooth dongle 2, `0A12:0001` (bcdDevice 8891), 2026-11-08: the same endpoints as dongle 1.
HCI 6 / LMP 6 (Bluetooth 4.0), revision and subversion `22BB`, maker 10, name "CSR8510 A10",
ACL 310 bytes x 10, HCI Reset 115 ms. **SSP yes**, LE yes: the dongle for the DualSense.
Inquiry needs Set Event Mask first: the default mask leaves out Inquiry Result with RSSI (bit
33) and Extended Inquiry Result (bit 46), so mode 2 gave nothing at all; a mask with reserved
bits set is refused (status `12`); Linux's `0x3DBFF807FFFBFFFF` works. Then mode 2 gives the
names: the DS4 in pairing mode answers as "Wireless Controller", class `002508`, RSSI -50 to
-67 dBm; two TVs nearby answer too (class `08043C`, `08243C`). The DualSense in pairing mode (Create + PS) answers the same way: "DualSense Wireless
Controller" at the address its USB feature `0x09` gives, class `002508`, RSSI -51 to -60 dBm;
in the last round its class turned `000508` (bit 13, limited discoverable, cleared: the pairing
window closing). A pad is to be picked by class
(major `05` peripheral, minor `08` gamepad; bit 13 may be either) and name, not by being the only answer.

A pad driver has to pick the HID interface, not the first one: the DualShock 4 v2 (and the
DualSense) put audio interfaces first.

## Open points

- Clones (DS3/DS4 copies) with their own quirks: after the probes, by what turns up.
- IOP memory and load beside mxnet and usbmass_bd: from the probe.
- Two USB sticks at once made the PS2 fail once (cause unknown): try a dongle beside a stick on
  its own.
- Whether the DS5 can be paired over USB, and whether any pad wants an SDP server on the host.
- Bluetooth LE pads (Xbox, others) are out of scope; they would need much more stack.
