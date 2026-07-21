# QuickType Pico-PIO-USB provenance

This directory starts from Pico-PIO-USB 0.7.2 and carries the exact three source
files from upstream pull request #206 at commit:

`38cf1166c13999d6316850643c21c5796f503414`

Source: <https://github.com/sekigon-gonnoc/Pico-PIO-USB/pull/206>

Pinned Git blob IDs:

- `src/pio_usb.c`: `f8e1e7ff3b08f3ce0834e24143ad7c042a370616`
- `src/pio_usb_host.c`: `3f978114870c6bcf992e3e059fec414d3b212575`
- `src/pio_usb_ll.h`: `738eb4bd759af06558f0d020982823624a1b5eda`

The patch bounds TX and RX waits, replaces a racy EOP program-counter poll with
a deterministic delay, reinitializes the EOP state machine for each receive,
adds an absolute receive-packet deadline, and debounces short SE0 glitches.
Upstream reports #192, #197, and #203 describe the hangs addressed by the patch.

PR #206 remains open as of 2026-07-21. Its author reports a 62-hour, 6.4-million
packet full-speed USB MIDI soak at 240 MHz. QuickType must still pass its own
240 MHz Logitech K360 idle/wake, reconnect, and fault-containment tests before
this patch is treated as qualified.

Firmware builds must select this vendored library explicitly rather than an
unpatched global Arduino copy. Verify the blob IDs above after every build-source
change.
