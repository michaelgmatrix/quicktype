# QuickType USB-UART Bridge

This sketch is for the RP2040-Zero board that acts as a native USB host for the
external keypad, keyboard, or receiver. It forwards HID report descriptors and
raw HID input reports to the primary board over hardware UART. It does not use
Pico-PIO-USB or GPIO0/GPIO1 for USB.

The bridge keeps HID interfaces in report protocol so keyboard report IDs and
Consumer Control reports are preserved. This includes media keys such as play,
pause, mute, volume, and track controls. All HID interfaces are transported;
the primary firmware currently emits keyboard and Consumer Control inputs to
the computer.

## Wiring

- USB-C: native USB host connection to keypad, keyboard, or receiver
- GPIO4: UART TX to primary GPIO5
- GPIO5: UART RX from primary GPIO4
- GND: common ground with primary board

The bridge board's USB-C connector is the downstream USB host port. Use host/OTG
wiring that supplies VBUS to the attached USB device.

Build with the board profile used for the RP2040-Zero hardware and set
**USB Stack** to **Adafruit TinyUSB Host (native)**. The current command-line
build is verified with the Seeed XIAO RP2040 profile used by the primary
firmware.

The sketch intentionally rejects a device-mode build because this board is
always the keyboard-facing USB host.
