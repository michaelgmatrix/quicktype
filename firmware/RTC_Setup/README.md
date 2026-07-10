# QuickType firmware

This sketch targets the Seeed Studio XIAO RP2040. It acts as:

- a native USB keyboard connected to the computer;
- a PIO USB host for the external keyboard/keypad; and
- a USB serial device used by the QuickType web configurator.

## Arduino requirements

- Raspberry Pi Pico/RP2040 board package with the Seeed XIAO RP2040 board
- Adafruit TinyUSB Library
- Pico-PIO-USB
- ArduinoJson 7

Use these board menu settings:

- **USB Stack:** Adafruit TinyUSB
- **Flash Size:** 2 MB with a LittleFS partition (the 256 KB option is recommended)

The DS3231 uses `GPIO6`/`GPIO7` for RTC tokens. A build with **2 MB (no FS)** cannot save website configuration.

## Website programming

1. Flash `RTC_Setup.ino` to the XIAO RP2040.
2. Open the repository website in Chrome or Edge over HTTPS or localhost.
3. Choose **Connect Device** and select the XIAO/RP2040 serial port.
4. Use **Read Configuration**, **Write Configuration**, or **Sync Clock to This Computer**.

Until the first website configuration is written, the original hard-coded keypad mappings remain active. After a configuration is saved, unmatched keys pass through and configured physical-key or typed-trigger rules run from `/quicktype-config.json` in LittleFS.

## Serial protocol

The site and firmware exchange one minified JSON object per line at 115200 baud. Every request contains:

```json
{"qt":1,"id":1,"command":"ping"}
```

Supported commands are `ping`, `get-config`, `set-config`, `set-clock`, and `factory-reset`. Responses repeat `qt` and `id`, set `ok`, and contain either `data` or an `error` object. The maximum request/configuration size is 32 KB.

## Rule behavior

- `key`: intercepts a named physical key such as `KP_0`, `KP_ENTER`, or `BACKSPACE`.
- `suffix` / `instant`: replaces a typed trigger as soon as it matches.
- `delimiter`: replaces a trigger when followed by space, tab, or Enter and restores that delimiter after the expansion.
- `keyboard`, `numpad`, and `any` scopes restrict the source of typed triggers.

Templates support RTC values, cursor repositioning, Tab, Enter, Ctrl+V clipboard paste, and bracketed prompt placeholders. Shortcut rules accept values such as `CTRL+B`, `CTRL+SHIFT+S`, `ALT+TAB`, `HOME`, and `ENTER`. Output steps may contain `type expansion`, `resolve placeholders`, `key:<shortcut>`, or literal template text.
