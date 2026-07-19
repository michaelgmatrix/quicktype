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

Until the first website configuration is written, the original hard-coded keypad mappings remain active. After a configuration is saved, unmatched keys pass through and configured physical-key or typed-trigger rules run from `/quicktype-config.json` in LittleFS. The configurator keeps up to 24 typed-trigger rules and 19 dedicated numeric-keypad assignments active together.

## Serial protocol

The site and firmware exchange one minified JSON object per line at 115200 baud. Every request contains:

```json
{"qt":1,"id":1,"command":"ping"}
```

Supported commands are `ping`, `get-config`, `set-config`, `get-clock`, `set-clock`, `set-expansions-enabled`, `set-keypad-expansions-enabled`, and `factory-reset`. `set-expansions-enabled` temporarily pauses or resumes all configured rules. `set-keypad-expansions-enabled` independently disables or enables configured physical keypad actions while typed full-keyboard expansions remain active. Both runtime states return to enabled after a device restart and do not change the saved configuration. Responses repeat `qt` and `id`, set `ok`, and contain either `data` or an `error` object. The maximum request/configuration size is 32 KB.

## Rule behavior

- `key`: intercepts a named physical key such as `KP_0`, `KP_ENTER`, or `BACKSPACE`.
- `suffix` / `instant`: replaces a typed trigger as soon as it matches.
- `delimiter`: replaces a trigger when followed by space, tab, or Enter and restores that delimiter after the expansion.
- `keyboard`, `numpad`, and `any` scopes restrict the source of typed triggers.

The 19-key keypad editor supports Num Lock, `/`, `*`, Backspace, `-`, `+`, Enter, `0` through `9`, `00`, and `.`. Assigned keys run their configured action; unassigned keys continue to the computer normally. While the web configurator is connected, pressing a physical keypad key selects its editor through telemetry polling.

Templates support reusable custom placeholders, clock fields such as `{date}`, `{weekday}`, `{time_24}`, `{time_12_compact}`, `{timezone_offset}`, `{iso_datetime_tz}`, and custom formats such as `{date:MM/D/YY}`. `{time_12_compact}` produces values such as `11:45am` or `3:07pm`. Custom placeholder tokens remain in the stored template and are resolved by the firmware when an expansion runs. Templates also support cursor repositioning, Tab, Enter, and Ctrl+V clipboard paste.

Double braces send inline keystrokes, allowing text and HID keys in the same template. `{{WIN}}` taps the Windows key, `{{ENTER}}` presses Enter, and `{{WIN+ALT+K}}` sends a combined chord. Supported modifiers are `CTRL`, `SHIFT`, `ALT`, and `WIN`/`GUI`/`CMD`/`META`, with explicit left/right forms including `LCTRL`, `RCTRL`, `LSHIFT`, `RSHIFT`, `LALT`, `RALT`/`ALTGR`, `LWIN`/`LGUI`/`LCMD`/`LMETA`, and `RWIN`/`RGUI`/`RCMD`/`RMETA`. Existing shortcut-type rules remain backward-compatible and are converted to double-brace syntax when edited. Output steps may contain `type expansion`, `resolve placeholders`, `key:<shortcut>`, or literal template text.

New rules and rules without an explicit `keyDelay` use a 5 ms key delay by default. Each configured rule can override that value.

The configurator bullet buttons insert `•`, `■`, `□`, `●`, and `◆`. Firmware converts these UTF-8 symbols to Windows Alt-key sequences when an expansion runs; application and font support can vary, so the variants are provided for compatibility testing.

Typing the hidden `;;;` trigger outputs the active typed expansions followed by the configured keypad actions. Each keypad action is shown as its key and label.

After every typed expansion, the USB-host core aborts and re-arms any stale HID receive transfer. This keeps the inline physical keyboard in transparent passthrough if a receive request stalls while QuickType is emitting an expansion.
