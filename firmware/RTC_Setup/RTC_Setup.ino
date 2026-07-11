#include <Arduino.h>
#include <Wire.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include <pio_usb.h>
#include <Adafruit_TinyUSB.h>
#include <tusb.h>

// ============================================================
// Seeed XIAO RP2040 wiring
// ============================================================
//
// Native USB-C port:
//   Acts as USB HID keyboard device to the PC.
//   Also keeps USB Serial available for testing/debug.
//
// RTC DS3231 I2C:
//   XIAO pad D4 / label "4" = GPIO6 = SDA
//   XIAO pad D5 / label "5" = GPIO7 = SCL
//
// USB keypad host port using Pico-PIO-USB:
//   USB GREEN wire = D+ = XIAO pad D6 / label "6" = GPIO0
//   USB WHITE wire = D- = XIAO pad D7 / label "7" = GPIO1
//   USB RED wire   = +5V / VBUS
//   USB BLACK wire = GND
//
// Pico-PIO-USB rule:
//   USB_HOST_DP_GPIO is D+
//   D- is automatically USB_HOST_DP_GPIO + 1

static constexpr int SDA_PIN = 6;
static constexpr int SCL_PIN = 7;

static constexpr int USB_HOST_DP_GPIO = 0; // XIAO pad D6 / label "6" / GPIO0 / USB green D+
static constexpr int USB_HOST_DM_GPIO = 1; // XIAO pad D7 / label "7" / GPIO1 / USB white D-

static constexpr uint8_t RTC_ADDR = 0x68;
static constexpr char TIMESTAMP_FILE[] = "/Timestamp.txt";
static constexpr char CONFIG_FILE[] = "/quicktype-config.json";
static constexpr char CONFIG_TEMP_FILE[] = "/quicktype-config.tmp";
static constexpr char CLOCK_META_FILE[] = "/quicktype-clock.json";
static constexpr char CLOCK_META_TEMP_FILE[] = "/quicktype-clock.tmp";
static constexpr char FIRMWARE_VERSION[] = "0.2.0";
static constexpr uint8_t CONFIG_SCHEMA_VERSION = 1;
static constexpr size_t MAX_CONFIG_BYTES = 32768;
static constexpr size_t MAX_CONFIG_RULES = 24;
static constexpr size_t MAX_RULE_STEPS = 8;
static constexpr size_t MAX_TRIGGER_BUFFER = 64;

// Set this to true if you want Timestamp.txt renamed after a successful RTC set.
static constexpr bool RENAME_TIMESTAMP_FILE_AFTER_SUCCESS = false;

// HID keyboard report descriptor for the native USB-C device side.
uint8_t const hidReportDescriptor[] = {
  TUD_HID_REPORT_DESC_KEYBOARD()
};

// Native USB-C HID keyboard device.
Adafruit_USBD_HID usb_hid;

// PIO USB host object for the external keypad.
Adafruit_USBH_Host USBHost;

struct RtcDateTime {
  int year;
  int month;
  int day;
  int dow;     // 1 = Monday, 7 = Sunday
  int hour;
  int minute;
  int second;
};

struct MacroStep {
  String value;
  uint16_t delayMs;
};

struct ConfigRule {
  String id;
  String type;
  String trigger;
  String triggerPattern;
  String triggerScope;
  String label;
  String text;
  uint16_t preDelayMs;
  uint16_t keyDelayMs;
  uint8_t stepCount;
  MacroStep steps[MAX_RULE_STEPS];
  bool enabled;
};

static hid_keyboard_report_t previousKeyboardReport = {};
static ConfigRule configRules[MAX_CONFIG_RULES];
static size_t configRuleCount = 0;
static bool storedConfigurationLoaded = false;
static String serialInputLine;
static bool serialInputOverflow = false;
static String typedBuffer;
static String typedSources;
static String clockTimezoneName = "Local";
static int clockTimezoneOffsetMinutes = 0;

// Forward declarations
uint8_t decToBcd(int value);
int bcdToDec(uint8_t value);
bool isLeapYear(int year);
int daysInMonth(int year, int month);
int weekdayMonday1(int year, int month, int day);
RtcDateTime datePlusDays(RtcDateTime dt, int daysToAdd);
const char* monthNameFull(int month);
const char* monthNameShort(int month);
const char* weekdayNameFull(int dow);
const char* weekdayNameShort(int dow);
String formatTimezoneOffset(int offsetMinutes);
String formatCustomDateTime(const String& pattern, const RtcDateTime& dt);
void loadClockMetadata();
bool saveClockMetadata(const String& timezoneName, int offsetMinutes);

bool validateDateTime(const RtcDateTime& dt);
String readFirstUsefulLine(const char* filename);
bool parseTimestampFile(const char* filename, RtcDateTime& out);
bool rtcPresent();
uint8_t rtcReadRegister(uint8_t reg);
void rtcWriteRegister(uint8_t reg, uint8_t value);
bool rtcOscillatorStopFlagSet();
void rtcClearOscillatorStopFlag();
void rtcSetDateTime(const RtcDateTime& dt);
RtcDateTime rtcGetDateTime();
void printDateTime(const char* label, const RtcDateTime& dt);
void handleTimestampFile();

void configureUsbDeviceKeyboard();
void configureUsbHost();
bool waitForUsbHidReady(uint32_t timeoutMs);
bool sendHidKey(uint8_t modifier, uint8_t keycode);
bool sendHidKeyWithDelay(uint8_t modifier, uint8_t keycode, uint16_t delayMs);
bool typeAsciiChar(char c);
bool typeAsciiCharWithDelay(char c, uint16_t delayMs);
bool typeAsciiString(const char* text);
bool typeAsciiStringWithDelay(const String& text, uint16_t delayMs);
bool sendLeftArrows(uint8_t count);
bool sendHomeEnterEnterUp();
bool sendHomeEnterEnterUpCtrlB();

void outputSlashMacro();
void outputShortDateInitialsAndMoveCursor();
void outputAppointmentConfirmationMacro(const char* appointmentWindowText);
void outputDateFormatForKey(uint8_t keycode);

const char* keypadUsageName(uint8_t usage);
const char* physicalKeyName(uint8_t usage);
bool keyWasInPreviousReport(uint8_t keycode);
bool forwardKeyboardReport(hid_keyboard_report_t const* report);
bool handleLegacyKey(uint8_t keycode);
char hidKeycodeToAscii(uint8_t keycode, uint8_t modifier, bool& isNumpad);
bool processPhysicalKeyRule(uint8_t keycode);
void recordTypedCharacter(char value, bool isNumpad);
bool processTypedTriggerRules(char currentCharacter);
bool executeConfiguredRule(const ConfigRule& rule, size_t eraseCount, char delimiterToRestore);
bool typeExpansionTemplate(const String& text, uint16_t keyDelayMs);
bool sendShortcut(const String& shortcut, uint16_t keyDelayMs);
uint8_t shortcutKeycode(const String& token);
void handleKeyboardReport(hid_keyboard_report_t const* report);

void clearCompiledConfiguration();
bool compileConfiguration(JsonVariantConst config, String& errorMessage);
size_t activeRuleCount();
bool loadConfigurationFromStorage();
bool saveConfiguration(JsonVariantConst config, String& errorMessage);
void processSerialProtocol();
void handleProtocolLine(const String& line);
void sendProtocolError(uint32_t id, const char* code, const String& message);
void sendProtocolSuccess(uint32_t id, const char* type);
void sendProtocolInfo(uint32_t id);
void sendStoredConfiguration(uint32_t id);
void sendProtocolClock(uint32_t id);

// ============================================================
// Native USB HID keyboard device setup
// ============================================================

void configureUsbDeviceKeyboard() {
  // IMPORTANT:
  // This must be called before Serial.begin() and before delay(),
  // so the HID interface exists when the PC enumerates the USB device.

  usb_hid.setBootProtocol(HID_ITF_PROTOCOL_KEYBOARD);
  usb_hid.setPollInterval(2);
  usb_hid.setReportDescriptor(hidReportDescriptor, sizeof(hidReportDescriptor));
  usb_hid.setStringDescriptor("XIAO Timestamp Keyboard");
  usb_hid.begin();
}

bool waitForUsbHidReady(uint32_t timeoutMs) {
  uint32_t started = millis();

  while (!usb_hid.ready()) {
    if (TinyUSBDevice.suspended()) {
      TinyUSBDevice.remoteWakeup();
    }

    if (millis() - started >= timeoutMs) {
      Serial.println("ERROR: USB HID keyboard is not ready.");
      Serial.print("TinyUSB mounted: ");
      Serial.println(TinyUSBDevice.mounted() ? "yes" : "no");
      return false;
    }

    delay(1);
  }

  return true;
}

bool sendHidKey(uint8_t modifier, uint8_t keycode) {
  return sendHidKeyWithDelay(modifier, keycode, 5);
}

bool sendHidKeyWithDelay(uint8_t modifier, uint8_t keycode, uint16_t delayMs) {
  if (!waitForUsbHidReady(1000)) {
    return false;
  }

  uint8_t keycodes[6] = { 0 };
  keycodes[0] = keycode;

  if (!usb_hid.keyboardReport(0, modifier, keycodes)) {
    Serial.println("ERROR: Failed to send HID key press.");
    return false;
  }

  delay(delayMs);

  if (!usb_hid.keyboardRelease(0)) {
    Serial.println("ERROR: Failed to send HID key release.");
    return false;
  }

  delay(delayMs);

  return true;
}

bool typeAsciiChar(char c) {
  return typeAsciiCharWithDelay(c, 5);
}

bool typeAsciiCharWithDelay(char c, uint16_t delayMs) {
  if (c == '\n' || c == '\r') return sendHidKeyWithDelay(0, HID_KEY_ENTER, delayMs);
  if (c == '\t') return sendHidKeyWithDelay(0, HID_KEY_TAB, delayMs);
  if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) {
    Serial.print("ERROR: Unsupported non-ASCII character for HID typing: 0x");
    Serial.println((uint8_t)c, HEX);
    return false;
  }

  if (!waitForUsbHidReady(1000)) return false;
  if (!usb_hid.keyboardPress(0, c)) return false;
  delay(delayMs);
  if (!usb_hid.keyboardRelease(0)) return false;
  delay(delayMs);
  return true;
}

bool typeAsciiString(const char* text) {
  while (*text != '\0') {
    if (!typeAsciiChar(*text)) {
      return false;
    }

    text++;
  }

  return true;
}

bool typeAsciiStringWithDelay(const String& text, uint16_t delayMs) {
  for (size_t index = 0; index < text.length(); index++) {
    if (!typeAsciiCharWithDelay(text[index], delayMs)) {
      return false;
    }
  }

  return true;
}

bool sendLeftArrows(uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    if (!sendHidKey(0, HID_KEY_ARROW_LEFT)) {
      return false;
    }
  }

  return true;
}

bool sendHomeEnterEnterUp() {
  if (!sendHidKey(0, HID_KEY_HOME)) {
    return false;
  }

  if (!sendHidKey(0, HID_KEY_ENTER)) {
    return false;
  }

  if (!sendHidKey(0, HID_KEY_ENTER)) {
    return false;
  }

  if (!sendHidKey(0, HID_KEY_ARROW_UP)) {
    return false;
  }

  return true;
}

// Reusable key pattern:
// HOME, ENTER, ENTER, UP Arrow, CTRL+B
bool sendHomeEnterEnterUpCtrlB() {
  Serial.println("Sending reusable pattern: HOME, ENTER, ENTER, UP Arrow, CTRL+B.");

  if (!sendHomeEnterEnterUp()) {
    return false;
  }

  if (!sendHidKey(KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_B)) {
    return false;
  }

  return true;
}

// ============================================================
// Macro outputs
// ============================================================

void outputSlashMacro() {
  RtcDateTime now = rtcGetDateTime();

  if (!validateDateTime(now)) {
    Serial.println("ERROR: RTC date/time read is invalid. HID output skipped.");
    return;
  }

  char buffer[24];

  snprintf(
    buffer,
    sizeof(buffer),
    "%d/%d/%02d:   MM",
    now.month,
    now.day,
    now.year % 100
  );

  Serial.print("Typing slash macro: ");
  Serial.println(buffer);
  Serial.println("Then sending Left Arrow x3.");

  if (typeAsciiString(buffer)) {
    sendLeftArrows(3);
  }
}

void outputShortDateInitialsAndMoveCursor() {
  RtcDateTime now = rtcGetDateTime();

  if (!validateDateTime(now)) {
    Serial.println("ERROR: RTC date/time read is invalid. HID output skipped.");
    return;
  }

  char buffer[16];

  snprintf(
    buffer,
    sizeof(buffer),
    "%d/%d MM",
    now.month,
    now.day
  );

  Serial.print("Typing short date/initials: ");
  Serial.println(buffer);
  Serial.println("Then sending Left Arrow x3.");

  if (typeAsciiString(buffer)) {
    sendLeftArrows(3);
  }
}

void outputAppointmentConfirmationMacro(const char* appointmentWindowText) {
  RtcDateTime now = rtcGetDateTime();

  if (!validateDateTime(now)) {
    Serial.println("ERROR: RTC date/time read is invalid. HID output skipped.");
    return;
  }

  RtcDateTime tomorrow = datePlusDays(now, 1);

  char buffer[80];

  snprintf(
    buffer,
    sizeof(buffer),
    "%d/%d/%02d: %s%d/%d/%02d MM",
    now.month,
    now.day,
    now.year % 100,
    appointmentWindowText,
    tomorrow.month,
    tomorrow.day,
    tomorrow.year % 100
  );

  Serial.print("Typing appointment confirmation macro: ");
  Serial.println(buffer);

  typeAsciiString(buffer);
}

void outputDateFormatForKey(uint8_t keycode) {
  RtcDateTime now = rtcGetDateTime();

  if (!validateDateTime(now)) {
    Serial.println("ERROR: RTC date/time read is invalid. HID output skipped.");
    return;
  }

  char buffer[40];

  switch (keycode) {
    case 0x61: // Keypad 9 / PageUp
      // 07/09/2026
      snprintf(
        buffer,
        sizeof(buffer),
        "%02d/%02d/%04d",
        now.month,
        now.day,
        now.year
      );
      break;

    case 0x5E: // Keypad 6 / Right
      // 7/9/26
      snprintf(
        buffer,
        sizeof(buffer),
        "%d/%d/%02d",
        now.month,
        now.day,
        now.year % 100
      );
      break;

    case 0x5D: // Keypad 5
      // 2026-07-09
      snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02d-%02d",
        now.year,
        now.month,
        now.day
      );
      break;

    case 0x5C: // Keypad 4 / Left
      // 07-09-2026
      snprintf(
        buffer,
        sizeof(buffer),
        "%02d-%02d-%04d",
        now.month,
        now.day,
        now.year
      );
      break;

    case 0x5B: // Keypad 3 / PageDown
      // July 9, 2026
      snprintf(
        buffer,
        sizeof(buffer),
        "%s %d, %04d",
        monthNameFull(now.month),
        now.day,
        now.year
      );
      break;

    case 0x5A: // Keypad 2 / Down
      // 20260709
      snprintf(
        buffer,
        sizeof(buffer),
        "%04d%02d%02d",
        now.year,
        now.month,
        now.day
      );
      break;

    case 0x59: // Keypad 1 / End
      // 070926
      snprintf(
        buffer,
        sizeof(buffer),
        "%02d%02d%02d",
        now.month,
        now.day,
        now.year % 100
      );
      break;

    default:
      Serial.println("ERROR: No date format assigned to this key.");
      return;
  }

  Serial.print("Typing date format: ");
  Serial.println(buffer);

  typeAsciiString(buffer);
}

// ============================================================
// PIO USB host setup
// ============================================================

void configureUsbHost() {
  Serial.println("Configuring PIO USB host...");

  pio_usb_configuration_t pioConfig = PIO_USB_DEFAULT_CONFIG;
  pioConfig.pin_dp = USB_HOST_DP_GPIO;

  USBHost.configure_pio_usb(1, &pioConfig);
  USBHost.begin(1);

  Serial.print("USB host D+ GPIO: ");
  Serial.println(USB_HOST_DP_GPIO);

  Serial.print("USB host D- GPIO: ");
  Serial.println(USB_HOST_DM_GPIO);

  Serial.println("PIO USB host ready.");
}

const char* keypadUsageName(uint8_t usage) {
  switch (usage) {
    case 0x53: return "Keypad NumLock/Clear";
    case 0x54: return "Keypad /";
    case 0x55: return "Keypad *";
    case 0x56: return "Keypad -";
    case 0x57: return "Keypad +";
    case 0x58: return "Keypad Enter";
    case 0x59: return "Keypad 1 / End";
    case 0x5A: return "Keypad 2 / Down";
    case 0x5B: return "Keypad 3 / PageDown";
    case 0x5C: return "Keypad 4 / Left";
    case 0x5D: return "Keypad 5";
    case 0x5E: return "Keypad 6 / Right";
    case 0x5F: return "Keypad 7 / Home";
    case 0x60: return "Keypad 8 / Up";
    case 0x61: return "Keypad 9 / PageUp";
    case 0x62: return "Keypad 0 / Insert";
    case 0xB0: return "Keypad 00";
    case 0x63: return "Keypad . / Delete";
    case 0x67: return "Keypad =";

    case 0x28: return "Enter";
    case 0x2A: return "Backspace";
    case 0x2B: return "Tab";
    case 0x4C: return "Delete";
    case 0x4F: return "Right Arrow";
    case 0x50: return "Left Arrow";
    case 0x51: return "Down Arrow";
    case 0x52: return "Up Arrow";

    default: return nullptr;
  }
}

bool keyWasInPreviousReport(uint8_t keycode) {
  if (keycode == 0) {
    return true;
  }

  for (uint8_t i = 0; i < 6; i++) {
    if (previousKeyboardReport.keycode[i] == keycode) {
      return true;
    }
  }

  return false;
}

const char* physicalKeyName(uint8_t usage) {
  static char generatedName[8];
  switch (usage) {
    case 0x53: return "KP_NUMLOCK";
    case 0x54: return "KP_SLASH";
    case 0x55: return "KP_ASTERISK";
    case 0x56: return "KP_MINUS";
    case 0x57: return "KP_PLUS";
    case 0x58: return "KP_ENTER";
    case 0x59: return "KP_1";
    case 0x5A: return "KP_2";
    case 0x5B: return "KP_3";
    case 0x5C: return "KP_4";
    case 0x5D: return "KP_5";
    case 0x5E: return "KP_6";
    case 0x5F: return "KP_7";
    case 0x60: return "KP_8";
    case 0x61: return "KP_9";
    case 0x62: return "KP_0";
    case 0x63: return "KP_PERIOD";
    case 0x67: return "KP_EQUALS";
    case 0xB0: return "KP_00";
    case 0x28: return "ENTER";
    case 0x29: return "ESCAPE";
    case 0x2A: return "BACKSPACE";
    case 0x2B: return "TAB";
    case 0x4A: return "HOME";
    case 0x4B: return "PAGE_UP";
    case 0x4C: return "DELETE";
    case 0x4D: return "END";
    case 0x4E: return "PAGE_DOWN";
    case 0x4F: return "RIGHT";
    case 0x50: return "LEFT";
    case 0x51: return "DOWN";
    case 0x52: return "UP";
    default: break;
  }

  if (usage >= 0x04 && usage <= 0x1D) {
    generatedName[0] = 'A' + (usage - 0x04);
    generatedName[1] = '\0';
    return generatedName;
  }
  if (usage >= 0x1E && usage <= 0x26) {
    generatedName[0] = '1' + (usage - 0x1E);
    generatedName[1] = '\0';
    return generatedName;
  }
  if (usage == 0x27) return "0";
  if (usage >= 0x3A && usage <= 0x45) {
    snprintf(generatedName, sizeof(generatedName), "F%d", usage - 0x39);
    return generatedName;
  }
  return nullptr;
}

bool forwardKeyboardReport(hid_keyboard_report_t const* report) {
  if (!waitForUsbHidReady(1000)) {
    return false;
  }

  uint8_t keycodes[6];
  memcpy(keycodes, report->keycode, sizeof(keycodes));
  return usb_hid.keyboardReport(0, report->modifier, keycodes);
}

bool handleLegacyKey(uint8_t keycode) {
  switch (keycode) {
    case 0x53:
      return typeAsciiString("232175");
    case 0x54:
      outputSlashMacro();
      return true;
    case 0x55:
      outputShortDateInitialsAndMoveCursor();
      return true;
    case 0x56:
      return typeAsciiString("--------------------");
    case 0x58:
    case 0x28:
      return sendHidKey(0, HID_KEY_ENTER);
    case 0x2A:
      return sendHomeEnterEnterUpCtrlB();
    case 0x5F:
      outputAppointmentConfirmationMacro("conf 8-12 appt. on ");
      return true;
    case 0x60:
      outputAppointmentConfirmationMacro("conf 1-5 appt. on ");
      return true;
    case 0x62:
      return typeAsciiString("Customer");
    case 0xB0:
      return typeAsciiString("LVM to");
    case 0x61:
    case 0x5E:
    case 0x5D:
    case 0x5C:
    case 0x5B:
    case 0x5A:
    case 0x59:
      outputDateFormatForKey(keycode);
      return true;
    default:
      return false;
  }
}

char hidKeycodeToAscii(uint8_t keycode, uint8_t modifier, bool& isNumpad) {
  isNumpad = false;
  bool shifted = (modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;

  if (keycode >= 0x04 && keycode <= 0x1D) {
    char letter = 'a' + (keycode - 0x04);
    return shifted ? letter - ('a' - 'A') : letter;
  }

  if (keycode >= 0x1E && keycode <= 0x26) {
    static const char normal[] = "123456789";
    static const char shiftedValues[] = "!@#$%^&*(";
    return shifted ? shiftedValues[keycode - 0x1E] : normal[keycode - 0x1E];
  }

  if (keycode == 0x27) return shifted ? ')' : '0';

  switch (keycode) {
    case 0x28: return '\n';
    case 0x2B: return '\t';
    case 0x2C: return ' ';
    case 0x2D: return shifted ? '_' : '-';
    case 0x2E: return shifted ? '+' : '=';
    case 0x2F: return shifted ? '{' : '[';
    case 0x30: return shifted ? '}' : ']';
    case 0x31: return shifted ? '|' : '\\';
    case 0x33: return shifted ? ':' : ';';
    case 0x34: return shifted ? '"' : '\'';
    case 0x35: return shifted ? '~' : '`';
    case 0x36: return shifted ? '<' : ',';
    case 0x37: return shifted ? '>' : '.';
    case 0x38: return shifted ? '?' : '/';
    case 0x54: isNumpad = true; return '/';
    case 0x55: isNumpad = true; return '*';
    case 0x56: isNumpad = true; return '-';
    case 0x57: isNumpad = true; return '+';
    case 0x58: isNumpad = true; return '\n';
    case 0x59: isNumpad = true; return '1';
    case 0x5A: isNumpad = true; return '2';
    case 0x5B: isNumpad = true; return '3';
    case 0x5C: isNumpad = true; return '4';
    case 0x5D: isNumpad = true; return '5';
    case 0x5E: isNumpad = true; return '6';
    case 0x5F: isNumpad = true; return '7';
    case 0x60: isNumpad = true; return '8';
    case 0x61: isNumpad = true; return '9';
    case 0x62: isNumpad = true; return '0';
    case 0x63: isNumpad = true; return '.';
    default: return 0;
  }
}

bool processPhysicalKeyRule(uint8_t keycode) {
  const char* keyName = physicalKeyName(keycode);
  if (keyName == nullptr) {
    return false;
  }

  for (size_t index = 0; index < configRuleCount; index++) {
    const ConfigRule& rule = configRules[index];
    if (rule.enabled && rule.trigger.equalsIgnoreCase("key") && rule.triggerPattern.equalsIgnoreCase(keyName)) {
      Serial.print("Matched physical rule ");
      Serial.print(rule.id);
      Serial.print(": ");
      Serial.println(rule.label);
      return executeConfiguredRule(rule, 0, 0);
    }
  }

  return false;
}

void recordTypedCharacter(char value, bool isNumpad) {
  typedBuffer += value;
  typedSources += isNumpad ? 'n' : 'k';

  while (typedBuffer.length() > MAX_TRIGGER_BUFFER) {
    typedBuffer.remove(0, 1);
    typedSources.remove(0, 1);
  }
}

bool processTypedTriggerRules(char currentCharacter) {
  for (size_t index = 0; index < configRuleCount; index++) {
    const ConfigRule& rule = configRules[index];
    if (!rule.enabled || rule.trigger.equalsIgnoreCase("key") || rule.triggerPattern.length() == 0) {
      continue;
    }

    bool delimiterMode = rule.trigger.equalsIgnoreCase("delimiter");
    bool isDelimiter = currentCharacter == ' ' || currentCharacter == '\t' || currentCharacter == '\n';
    size_t patternLength = rule.triggerPattern.length();
    size_t suffixLength = delimiterMode ? patternLength + 1 : patternLength;

    if ((delimiterMode && !isDelimiter) || typedBuffer.length() < suffixLength) {
      continue;
    }

    size_t patternStart = typedBuffer.length() - suffixLength;
    if (typedBuffer.substring(patternStart, patternStart + patternLength) != rule.triggerPattern) {
      continue;
    }

    bool sourceMatches = true;
    for (size_t sourceIndex = patternStart; sourceIndex < patternStart + patternLength; sourceIndex++) {
      if (rule.triggerScope == "keyboard" && typedSources[sourceIndex] != 'k') sourceMatches = false;
      if (rule.triggerScope == "numpad" && typedSources[sourceIndex] != 'n') sourceMatches = false;
    }
    if (!sourceMatches) {
      continue;
    }

    Serial.print("Matched typed rule ");
    Serial.print(rule.id);
    Serial.print(": ");
    Serial.println(rule.label);
    bool result = executeConfiguredRule(rule, suffixLength, delimiterMode ? currentCharacter : 0);
    typedBuffer = "";
    typedSources = "";
    return result;
  }

  return false;
}

void handleKeyboardReport(hid_keyboard_report_t const* report) {
  bool interceptedPhysicalKey = false;

  for (uint8_t i = 0; i < 6; i++) {
    uint8_t keycode = report->keycode[i];

    if (keycode == 0) {
      continue;
    }

    if (keyWasInPreviousReport(keycode)) {
      continue;
    }

    const char* name = keypadUsageName(keycode);

    Serial.print("USB keypad key pressed: ");

    if (name != nullptr) {
      Serial.print(name);
    } else {
      Serial.print("HID usage 0x");
      if (keycode < 0x10) {
        Serial.print("0");
      }
      Serial.print(keycode, HEX);
    }

    Serial.print("  modifier=0x");
    if (report->modifier < 0x10) {
      Serial.print("0");
    }
    Serial.println(report->modifier, HEX);

    if (storedConfigurationLoaded) {
      if (processPhysicalKeyRule(keycode)) {
        interceptedPhysicalKey = true;
        break;
      }
    } else if (handleLegacyKey(keycode)) {
      interceptedPhysicalKey = true;
      break;
    }
  }

  if (interceptedPhysicalKey) {
    usb_hid.keyboardRelease(0);
    previousKeyboardReport = *report;
    return;
  }

  forwardKeyboardReport(report);

  if (storedConfigurationLoaded) {
    for (uint8_t i = 0; i < 6; i++) {
      uint8_t keycode = report->keycode[i];
      if (keycode == 0 || keyWasInPreviousReport(keycode)) {
        continue;
      }

      if (keycode == HID_KEY_BACKSPACE) {
        if (typedBuffer.length() > 0) typedBuffer.remove(typedBuffer.length() - 1);
        if (typedSources.length() > 0) typedSources.remove(typedSources.length() - 1);
        continue;
      }

      if (keycode == HID_KEY_ESCAPE || keycode == HID_KEY_DELETE || keycode == HID_KEY_HOME ||
          keycode == HID_KEY_END || keycode == HID_KEY_ARROW_LEFT || keycode == HID_KEY_ARROW_RIGHT ||
          keycode == HID_KEY_ARROW_UP || keycode == HID_KEY_ARROW_DOWN) {
        typedBuffer = "";
        typedSources = "";
        continue;
      }

      if (keycode == 0xB0) {
        recordTypedCharacter('0', true);
        recordTypedCharacter('0', true);
        if (processTypedTriggerRules('0')) break;
        continue;
      }

      bool isNumpad = false;
      char typedCharacter = hidKeycodeToAscii(keycode, report->modifier, isNumpad);
      if (typedCharacter != 0) {
        recordTypedCharacter(typedCharacter, isNumpad);
        if (processTypedTriggerRules(typedCharacter)) break;
      }
    }
  }

  previousKeyboardReport = *report;
}

// ============================================================
// TinyUSB host callbacks for external USB keypad
// ============================================================

extern "C" void tuh_hid_mount_cb(
  uint8_t dev_addr,
  uint8_t instance,
  uint8_t const* desc_report,
  uint16_t desc_len
) {
  (void)desc_report;
  (void)desc_len;

  uint8_t protocol = tuh_hid_interface_protocol(dev_addr, instance);

  Serial.print("HID device mounted. addr=");
  Serial.print(dev_addr);
  Serial.print(" instance=");
  Serial.print(instance);
  Serial.print(" protocol=");
  Serial.println(protocol);

  if (protocol == HID_ITF_PROTOCOL_KEYBOARD) {
    Serial.println("Boot keyboard/keypad detected.");
  } else {
    Serial.println("HID device is not a boot keyboard. Reports may need custom parsing.");
  }

  if (!tuh_hid_receive_report(dev_addr, instance)) {
    Serial.println("ERROR: Could not request first HID report.");
  }
}

extern "C" void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  Serial.print("HID device unmounted. addr=");
  Serial.print(dev_addr);
  Serial.print(" instance=");
  Serial.println(instance);

  memset(&previousKeyboardReport, 0, sizeof(previousKeyboardReport));
}

extern "C" void tuh_hid_report_received_cb(
  uint8_t dev_addr,
  uint8_t instance,
  uint8_t const* report,
  uint16_t len
) {
  uint8_t protocol = tuh_hid_interface_protocol(dev_addr, instance);

  if (protocol == HID_ITF_PROTOCOL_KEYBOARD && len >= sizeof(hid_keyboard_report_t)) {
    handleKeyboardReport((hid_keyboard_report_t const*)report);
  } else {
    Serial.print("HID report received. protocol=");
    Serial.print(protocol);
    Serial.print(" len=");
    Serial.println(len);
  }

  if (!tuh_hid_receive_report(dev_addr, instance)) {
    Serial.println("ERROR: Could not request next HID report.");
  }
}

// ============================================================
// Configurable rules and browser serial protocol
// ============================================================

void clearCompiledConfiguration() {
  for (size_t index = 0; index < MAX_CONFIG_RULES; index++) {
    configRules[index] = ConfigRule();
  }
  configRuleCount = 0;
  storedConfigurationLoaded = false;
  typedBuffer = "";
  typedSources = "";
}

bool compileConfiguration(JsonVariantConst config, String& errorMessage) {
  if (!config.is<JsonObjectConst>()) {
    errorMessage = "Configuration must be a JSON object.";
    return false;
  }

  int schemaVersion = config["version"] | 1;
  if (schemaVersion != CONFIG_SCHEMA_VERSION) {
    errorMessage = "Unsupported configuration schema version.";
    return false;
  }

  JsonObjectConst rules = config["rules"].as<JsonObjectConst>();
  if (rules.isNull()) {
    errorMessage = "Configuration is missing the rules object.";
    return false;
  }

  clearCompiledConfiguration();

  for (JsonPairConst pair : rules) {
    if (configRuleCount >= MAX_CONFIG_RULES) {
      errorMessage = "Configuration contains too many rules.";
      clearCompiledConfiguration();
      return false;
    }

    JsonObjectConst object = pair.value().as<JsonObjectConst>();
    if (object.isNull()) {
      continue;
    }

    ConfigRule& rule = configRules[configRuleCount];
    rule.id = pair.key().c_str();
    rule.type = String(object["type"] | "expansion");
    rule.trigger = String(object["trigger"] | "key");
    rule.triggerPattern = String(object["triggerPattern"] | "");
    rule.triggerScope = String(object["triggerScope"] | "any");
    rule.label = String(object["label"] | "Unassigned");
    rule.text = String(object["text"] | "");

    long preDelay = object["preDelay"] | 0;
    long keyDelay = object["keyDelay"] | 15;
    rule.preDelayMs = (uint16_t)constrain(preDelay, 0L, 5000L);
    rule.keyDelayMs = (uint16_t)constrain(keyDelay, 0L, 1000L);
    rule.enabled = !rule.type.equalsIgnoreCase("disabled") && rule.triggerPattern.length() > 0;
    rule.stepCount = 0;

    if (rule.triggerPattern.length() > 48) {
      errorMessage = "A trigger exceeds the 48-character limit.";
      clearCompiledConfiguration();
      return false;
    }

    JsonArrayConst steps = object["steps"].as<JsonArrayConst>();
    if (!steps.isNull()) {
      for (JsonVariantConst stepValue : steps) {
        if (rule.stepCount >= MAX_RULE_STEPS) {
          break;
        }
        JsonObjectConst stepObject = stepValue.as<JsonObjectConst>();
        if (stepObject.isNull()) {
          continue;
        }
        MacroStep& step = rule.steps[rule.stepCount];
        step.value = String(stepObject["value"] | "");
        long stepDelay = stepObject["delay"] | 15;
        step.delayMs = (uint16_t)constrain(stepDelay, 0L, 5000L);
        rule.stepCount++;
      }
    }

    configRuleCount++;
  }

  storedConfigurationLoaded = true;
  return true;
}

size_t activeRuleCount() {
  size_t count = 0;
  for (size_t index = 0; index < configRuleCount; index++) {
    if (configRules[index].enabled) {
      count++;
    }
  }
  return count;
}

bool loadConfigurationFromStorage() {
  if (!LittleFS.exists(CONFIG_FILE)) {
    clearCompiledConfiguration();
    Serial.println("No stored QuickType configuration. Legacy keypad mappings are active.");
    return false;
  }

  File file = LittleFS.open(CONFIG_FILE, "r");
  if (!file) {
    clearCompiledConfiguration();
    Serial.println("ERROR: Could not open stored QuickType configuration.");
    return false;
  }

  if (file.size() > MAX_CONFIG_BYTES) {
    file.close();
    clearCompiledConfiguration();
    Serial.println("ERROR: Stored QuickType configuration is too large.");
    return false;
  }

  JsonDocument document;
  DeserializationError jsonError = deserializeJson(document, file);
  file.close();

  if (jsonError) {
    clearCompiledConfiguration();
    Serial.print("ERROR: Stored QuickType configuration is invalid: ");
    Serial.println(jsonError.c_str());
    return false;
  }

  String validationError;
  if (!compileConfiguration(document.as<JsonVariantConst>(), validationError)) {
    Serial.print("ERROR: Stored QuickType configuration was rejected: ");
    Serial.println(validationError);
    return false;
  }

  Serial.print("Loaded QuickType configuration with ");
  Serial.print(activeRuleCount());
  Serial.print(" active rules (");
  Serial.print(configRuleCount);
  Serial.println(" compiled slots).");
  return true;
}

bool saveConfiguration(JsonVariantConst config, String& errorMessage) {
  size_t configSize = measureJson(config);
  if (configSize == 0 || configSize > MAX_CONFIG_BYTES) {
    errorMessage = "Configuration is empty or exceeds 32 KB.";
    return false;
  }

  if (!compileConfiguration(config, errorMessage)) {
    loadConfigurationFromStorage();
    return false;
  }

  LittleFS.remove(CONFIG_TEMP_FILE);
  File file = LittleFS.open(CONFIG_TEMP_FILE, "w");
  if (!file) {
    errorMessage = "Could not create the temporary configuration file.";
    loadConfigurationFromStorage();
    return false;
  }

  size_t written = serializeJson(config, file);
  file.close();
  if (written != configSize) {
    LittleFS.remove(CONFIG_TEMP_FILE);
    errorMessage = "Configuration write was incomplete.";
    loadConfigurationFromStorage();
    return false;
  }

  LittleFS.remove(CONFIG_FILE);
  if (!LittleFS.rename(CONFIG_TEMP_FILE, CONFIG_FILE)) {
    LittleFS.remove(CONFIG_TEMP_FILE);
    errorMessage = "Could not activate the new configuration file.";
    loadConfigurationFromStorage();
    return false;
  }

  storedConfigurationLoaded = true;
  return true;
}

uint8_t shortcutKeycode(const String& token) {
  if (token.length() == 1) {
    char value = token[0];
    if (value >= 'A' && value <= 'Z') return HID_KEY_A + (value - 'A');
    if (value >= '1' && value <= '9') return HID_KEY_1 + (value - '1');
    if (value == '0') return HID_KEY_0;
  }

  if (token == "ENTER") return HID_KEY_ENTER;
  if (token == "TAB") return HID_KEY_TAB;
  if (token == "ESC" || token == "ESCAPE") return HID_KEY_ESCAPE;
  if (token == "BACKSPACE") return HID_KEY_BACKSPACE;
  if (token == "DELETE" || token == "DEL") return HID_KEY_DELETE;
  if (token == "HOME") return HID_KEY_HOME;
  if (token == "END") return HID_KEY_END;
  if (token == "PAGEUP" || token == "PAGE_UP") return HID_KEY_PAGE_UP;
  if (token == "PAGEDOWN" || token == "PAGE_DOWN") return HID_KEY_PAGE_DOWN;
  if (token == "LEFT") return HID_KEY_ARROW_LEFT;
  if (token == "RIGHT") return HID_KEY_ARROW_RIGHT;
  if (token == "UP") return HID_KEY_ARROW_UP;
  if (token == "DOWN") return HID_KEY_ARROW_DOWN;
  if (token == "SPACE") return HID_KEY_SPACE;

  if (token.length() >= 2 && token[0] == 'F') {
    int functionNumber = token.substring(1).toInt();
    if (functionNumber >= 1 && functionNumber <= 12) {
      return HID_KEY_F1 + (functionNumber - 1);
    }
  }

  return 0;
}

bool sendShortcut(const String& shortcut, uint16_t keyDelayMs) {
  String input = shortcut;
  input.trim();
  input.toUpperCase();
  uint8_t modifier = 0;
  uint8_t keycode = 0;
  int start = 0;

  while (start <= (int)input.length()) {
    int separator = input.indexOf('+', start);
    String token = separator >= 0 ? input.substring(start, separator) : input.substring(start);
    token.trim();

    if (token == "CTRL" || token == "CONTROL") modifier |= KEYBOARD_MODIFIER_LEFTCTRL;
    else if (token == "SHIFT") modifier |= KEYBOARD_MODIFIER_LEFTSHIFT;
    else if (token == "ALT") modifier |= KEYBOARD_MODIFIER_LEFTALT;
    else if (token == "GUI" || token == "WIN" || token == "WINDOWS" || token == "CMD") modifier |= KEYBOARD_MODIFIER_LEFTGUI;
    else keycode = shortcutKeycode(token);

    if (separator < 0) break;
    start = separator + 1;
  }

  if (keycode == 0) {
    Serial.print("ERROR: Unsupported shortcut: ");
    Serial.println(shortcut);
    return false;
  }

  return sendHidKeyWithDelay(modifier, keycode, keyDelayMs);
}

String rtcTokenValue(const String& token) {
  bool isClockToken = token == "date" || token == "date_short" || token == "iso_date" ||
                      token == "time" || token == "time_seconds" || token == "time_24" ||
                      token == "time_24_seconds" || token == "datetime" || token == "iso_datetime" ||
                      token == "iso_datetime_tz" || token == "tomorrow_short" || token == "weekday" ||
                      token == "weekday_short" || token == "weekday_number" || token == "month" ||
                      token == "month_padded" || token == "month_name" || token == "day" ||
                      token == "day_padded" || token == "year" || token == "year_short" ||
                      token == "hour_24" || token == "hour_12" || token == "minute" ||
                      token == "second" || token == "ampm" || token == "timezone" ||
                      token == "timezone_offset" || token.startsWith("date:");
  if (!isClockToken) {
    return "";
  }

  RtcDateTime now = rtcGetDateTime();
  if (!validateDateTime(now)) {
    return "[RTC unavailable]";
  }

  if (token.startsWith("date:")) {
    return formatCustomDateTime(token.substring(5), now);
  }

  char buffer[80];
  int dow = now.dow;
  if (dow < 1 || dow > 7) {
    dow = weekdayMonday1(now.year, now.month, now.day);
  }
  int hour12 = now.hour % 12;
  if (hour12 == 0) hour12 = 12;

  if (token == "date") snprintf(buffer, sizeof(buffer), "%02d/%02d/%04d", now.month, now.day, now.year);
  else if (token == "date_short") snprintf(buffer, sizeof(buffer), "%d/%d/%02d", now.month, now.day, now.year % 100);
  else if (token == "iso_date") snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", now.year, now.month, now.day);
  else if (token == "time") snprintf(buffer, sizeof(buffer), "%d:%02d %s", hour12, now.minute, now.hour >= 12 ? "PM" : "AM");
  else if (token == "time_seconds") snprintf(buffer, sizeof(buffer), "%d:%02d:%02d %s", hour12, now.minute, now.second, now.hour >= 12 ? "PM" : "AM");
  else if (token == "time_24") snprintf(buffer, sizeof(buffer), "%02d:%02d", now.hour, now.minute);
  else if (token == "time_24_seconds") snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", now.hour, now.minute, now.second);
  else if (token == "datetime") snprintf(buffer, sizeof(buffer), "%02d/%02d/%04d %d:%02d %s", now.month, now.day, now.year, hour12, now.minute, now.hour >= 12 ? "PM" : "AM");
  else if (token == "iso_datetime") snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d", now.year, now.month, now.day, now.hour, now.minute, now.second);
  else if (token == "iso_datetime_tz") snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d%s", now.year, now.month, now.day, now.hour, now.minute, now.second, formatTimezoneOffset(clockTimezoneOffsetMinutes).c_str());
  else if (token == "tomorrow_short") {
    RtcDateTime tomorrow = datePlusDays(now, 1);
    snprintf(buffer, sizeof(buffer), "%d/%d/%02d", tomorrow.month, tomorrow.day, tomorrow.year % 100);
  }
  else if (token == "weekday") snprintf(buffer, sizeof(buffer), "%s", weekdayNameFull(dow));
  else if (token == "weekday_short") snprintf(buffer, sizeof(buffer), "%s", weekdayNameShort(dow));
  else if (token == "weekday_number") snprintf(buffer, sizeof(buffer), "%d", dow);
  else if (token == "month") snprintf(buffer, sizeof(buffer), "%d", now.month);
  else if (token == "month_padded") snprintf(buffer, sizeof(buffer), "%02d", now.month);
  else if (token == "month_name") snprintf(buffer, sizeof(buffer), "%s", monthNameFull(now.month));
  else if (token == "day") snprintf(buffer, sizeof(buffer), "%d", now.day);
  else if (token == "day_padded") snprintf(buffer, sizeof(buffer), "%02d", now.day);
  else if (token == "year") snprintf(buffer, sizeof(buffer), "%04d", now.year);
  else if (token == "year_short") snprintf(buffer, sizeof(buffer), "%02d", now.year % 100);
  else if (token == "hour_24") snprintf(buffer, sizeof(buffer), "%02d", now.hour);
  else if (token == "hour_12") snprintf(buffer, sizeof(buffer), "%d", hour12);
  else if (token == "minute") snprintf(buffer, sizeof(buffer), "%02d", now.minute);
  else if (token == "second") snprintf(buffer, sizeof(buffer), "%02d", now.second);
  else if (token == "ampm") snprintf(buffer, sizeof(buffer), "%s", now.hour >= 12 ? "PM" : "AM");
  else if (token == "timezone") snprintf(buffer, sizeof(buffer), "%s", clockTimezoneName.c_str());
  else if (token == "timezone_offset") snprintf(buffer, sizeof(buffer), "%s", formatTimezoneOffset(clockTimezoneOffsetMinutes).c_str());
  else return "";

  return String(buffer);
}

bool typeExpansionTemplate(const String& text, uint16_t keyDelayMs) {
  bool cursorSeen = false;
  size_t charactersAfterCursor = 0;

  for (size_t index = 0; index < text.length();) {
    if (text[index] != '{') {
      if (!typeAsciiCharWithDelay(text[index], keyDelayMs)) return false;
      if (cursorSeen) charactersAfterCursor++;
      index++;
      continue;
    }

    int tokenEnd = text.indexOf('}', index + 1);
    if (tokenEnd < 0) {
      if (!typeAsciiCharWithDelay(text[index], keyDelayMs)) return false;
      if (cursorSeen) charactersAfterCursor++;
      index++;
      continue;
    }

    String token = text.substring(index + 1, tokenEnd);
    String rtcValue = rtcTokenValue(token);
    if (rtcValue.length() > 0) {
      if (!typeAsciiStringWithDelay(rtcValue, keyDelayMs)) return false;
      if (cursorSeen) charactersAfterCursor += rtcValue.length();
    } else if (token == "cursor") {
      cursorSeen = true;
      charactersAfterCursor = 0;
    } else if (token == "tab") {
      if (!sendHidKeyWithDelay(0, HID_KEY_TAB, keyDelayMs)) return false;
    } else if (token == "enter") {
      if (!sendHidKeyWithDelay(0, HID_KEY_ENTER, keyDelayMs)) return false;
    } else if (token == "clipboard") {
      if (!sendHidKeyWithDelay(KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_V, keyDelayMs)) return false;
    } else {
      String literalToken = "{" + token + "}";
      if (!typeAsciiStringWithDelay(literalToken, keyDelayMs)) return false;
      if (cursorSeen) charactersAfterCursor += literalToken.length();
    }

    index = tokenEnd + 1;
  }

  while (charactersAfterCursor > 0) {
    if (!sendHidKeyWithDelay(0, HID_KEY_ARROW_LEFT, keyDelayMs)) return false;
    charactersAfterCursor--;
  }

  return true;
}

bool executeConfiguredRule(const ConfigRule& rule, size_t eraseCount, char delimiterToRestore) {
  usb_hid.keyboardRelease(0);
  if (rule.preDelayMs > 0) delay(rule.preDelayMs);

  for (size_t count = 0; count < eraseCount; count++) {
    if (!sendHidKeyWithDelay(0, HID_KEY_BACKSPACE, rule.keyDelayMs)) return false;
  }

  bool result = true;
  if (rule.stepCount > 0) {
    for (uint8_t index = 0; index < rule.stepCount; index++) {
      String stepValue = rule.steps[index].value;
      stepValue.trim();
      if (stepValue.equalsIgnoreCase("resolve placeholders")) {
        result = true;
      } else if (stepValue.equalsIgnoreCase("type expansion")) {
        result = typeExpansionTemplate(rule.text, rule.keyDelayMs);
      } else if (stepValue.startsWith("key:")) {
        result = sendShortcut(stepValue.substring(4), rule.keyDelayMs);
      } else {
        result = typeExpansionTemplate(stepValue, rule.keyDelayMs);
      }

      if (!result) return false;
      if (rule.steps[index].delayMs > 0) delay(rule.steps[index].delayMs);
    }
  } else if (rule.type.equalsIgnoreCase("shortcut")) {
    result = sendShortcut(rule.text, rule.keyDelayMs);
  } else {
    result = typeExpansionTemplate(rule.text, rule.keyDelayMs);
  }

  if (result && delimiterToRestore != 0) {
    result = typeAsciiCharWithDelay(delimiterToRestore, rule.keyDelayMs);
  }
  return result;
}

void sendProtocolError(uint32_t id, const char* code, const String& message) {
  JsonDocument response;
  response["qt"] = CONFIG_SCHEMA_VERSION;
  response["id"] = id;
  response["ok"] = false;
  response["type"] = "error";
  response["error"]["code"] = code;
  response["error"]["message"] = message;
  serializeJson(response, Serial);
  Serial.println();
}

void sendProtocolSuccess(uint32_t id, const char* type) {
  JsonDocument response;
  response["qt"] = CONFIG_SCHEMA_VERSION;
  response["id"] = id;
  response["ok"] = true;
  response["type"] = type;
  serializeJson(response, Serial);
  Serial.println();
}

void sendProtocolInfo(uint32_t id) {
  JsonDocument response;
  response["qt"] = CONFIG_SCHEMA_VERSION;
  response["id"] = id;
  response["ok"] = true;
  response["type"] = "hello";
  response["data"]["device"] = "QuickType XIAO RP2040";
  response["data"]["firmwareVersion"] = FIRMWARE_VERSION;
  response["data"]["configSchema"] = CONFIG_SCHEMA_VERSION;
  response["data"]["hasConfiguration"] = storedConfigurationLoaded;
  response["data"]["ruleCount"] = activeRuleCount();
  serializeJson(response, Serial);
  Serial.println();
}

void sendStoredConfiguration(uint32_t id) {
  if (!storedConfigurationLoaded || !LittleFS.exists(CONFIG_FILE)) {
    sendProtocolError(id, "NO_CONFIG", "The device has no saved configuration.");
    return;
  }

  File file = LittleFS.open(CONFIG_FILE, "r");
  if (!file) {
    sendProtocolError(id, "READ_FAILED", "Could not open the saved configuration.");
    return;
  }

  Serial.print("{\"qt\":1,\"id\":");
  Serial.print(id);
  Serial.print(",\"ok\":true,\"type\":\"config\",\"data\":");
  while (file.available()) {
    Serial.write((uint8_t)file.read());
  }
  file.close();
  Serial.println("}");
}

void sendProtocolClock(uint32_t id) {
  if (!rtcPresent()) {
    sendProtocolError(id, "RTC_UNAVAILABLE", "The DS3231 RTC is unavailable.");
    return;
  }

  RtcDateTime now = rtcGetDateTime();
  if (!validateDateTime(now)) {
    sendProtocolError(id, "INVALID_RTC", "The RTC returned an invalid date/time.");
    return;
  }

  JsonDocument response;
  response["qt"] = CONFIG_SCHEMA_VERSION;
  response["id"] = id;
  response["ok"] = true;
  response["type"] = "clock";
  response["data"]["clock"]["year"] = now.year;
  response["data"]["clock"]["month"] = now.month;
  response["data"]["clock"]["day"] = now.day;
  response["data"]["clock"]["dow"] = now.dow;
  response["data"]["clock"]["hour"] = now.hour;
  response["data"]["clock"]["minute"] = now.minute;
  response["data"]["clock"]["second"] = now.second;
  response["data"]["clock"]["timezoneName"] = clockTimezoneName;
  response["data"]["clock"]["timezoneOffsetMinutes"] = clockTimezoneOffsetMinutes;
  response["data"]["clock"]["timezoneOffset"] = formatTimezoneOffset(clockTimezoneOffsetMinutes);
  response["data"]["oscillatorStop"] = rtcOscillatorStopFlagSet();
  serializeJson(response, Serial);
  Serial.println();
}

void handleProtocolLine(const String& line) {
  JsonDocument request;
  DeserializationError jsonError = deserializeJson(request, line);
  if (jsonError) {
    sendProtocolError(0, "BAD_JSON", String("Invalid request JSON: ") + jsonError.c_str());
    return;
  }

  uint32_t id = request["id"] | 0;
  int protocolVersion = request["qt"] | 0;
  const char* command = request["command"] | "";
  if (protocolVersion != CONFIG_SCHEMA_VERSION) {
    sendProtocolError(id, "BAD_PROTOCOL", "Unsupported protocol version.");
    return;
  }

  if (strcmp(command, "ping") == 0) {
    sendProtocolInfo(id);
    return;
  }

  if (strcmp(command, "get-config") == 0) {
    sendStoredConfiguration(id);
    return;
  }

  if (strcmp(command, "get-clock") == 0) {
    sendProtocolClock(id);
    return;
  }

  if (strcmp(command, "set-config") == 0) {
    JsonVariantConst config = request["config"];
    String errorMessage;
    if (!saveConfiguration(config, errorMessage)) {
      sendProtocolError(id, "INVALID_CONFIG", errorMessage);
      return;
    }

    JsonDocument response;
    response["qt"] = CONFIG_SCHEMA_VERSION;
    response["id"] = id;
    response["ok"] = true;
    response["type"] = "config-saved";
    response["data"]["ruleCount"] = activeRuleCount();
    serializeJson(response, Serial);
    Serial.println();
    return;
  }

  if (strcmp(command, "set-clock") == 0) {
    JsonObjectConst clock = request["clock"].as<JsonObjectConst>();
    RtcDateTime value = {};
    value.year = clock["year"] | 0;
    value.month = clock["month"] | 0;
    value.day = clock["day"] | 0;
    value.dow = clock["dow"] | weekdayMonday1(value.year, value.month, value.day);
    value.hour = clock["hour"] | 0;
    value.minute = clock["minute"] | 0;
    value.second = clock["second"] | 0;
    if (clock.isNull() || !validateDateTime(value) || !rtcPresent()) {
      sendProtocolError(id, "INVALID_CLOCK", "RTC data is invalid or the DS3231 is unavailable.");
      return;
    }
    rtcSetDateTime(value);
    rtcClearOscillatorStopFlag();
    const char* timezoneName = request["timezoneName"] | clockTimezoneName.c_str();
    int timezoneOffsetMinutes = request["timezoneOffsetMinutes"] | clockTimezoneOffsetMinutes;
    saveClockMetadata(String(timezoneName), timezoneOffsetMinutes);
    sendProtocolSuccess(id, "clock-set");
    return;
  }

  if (strcmp(command, "factory-reset") == 0) {
    LittleFS.remove(CONFIG_FILE);
    LittleFS.remove(CONFIG_TEMP_FILE);
    clearCompiledConfiguration();
    sendProtocolSuccess(id, "factory-reset");
    return;
  }

  sendProtocolError(id, "UNKNOWN_COMMAND", String("Unknown command: ") + command);
}

void processSerialProtocol() {
  while (Serial.available()) {
    char value = (char)Serial.read();
    if (value == '\r') continue;

    if (value == '\n') {
      if (serialInputOverflow) {
        sendProtocolError(0, "REQUEST_TOO_LARGE", "Request exceeds 32 KB.");
      } else if (serialInputLine.length() > 0) {
        handleProtocolLine(serialInputLine);
      }
      serialInputLine = "";
      serialInputOverflow = false;
      continue;
    }

    if (serialInputOverflow) continue;
    if (serialInputLine.length() >= MAX_CONFIG_BYTES) {
      serialInputOverflow = true;
      continue;
    }
    serialInputLine += value;
  }
}

// ============================================================
// RTC / timestamp code
// ============================================================

uint8_t decToBcd(int value) {
  return ((value / 10) << 4) | (value % 10);
}

int bcdToDec(uint8_t value) {
  return ((value >> 4) * 10) + (value & 0x0F);
}

bool isLeapYear(int year) {
  return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

int daysInMonth(int year, int month) {
  if (month == 2) {
    return isLeapYear(year) ? 29 : 28;
  }

  if (month == 4 || month == 6 || month == 9 || month == 11) {
    return 30;
  }

  return 31;
}

int weekdayMonday1(int year, int month, int day) {
  // Returns 1..7:
  // 1 = Monday
  // 7 = Sunday

  if (month < 3) {
    month += 12;
    year -= 1;
  }

  int k = year % 100;
  int j = year / 100;

  int h = (day + ((13 * (month + 1)) / 5) + k + (k / 4) + (j / 4) + (5 * j)) % 7;

  // Zeller:
  // 0 = Saturday, 1 = Sunday, 2 = Monday, ... 6 = Friday
  switch (h) {
    case 0: return 6; // Saturday
    case 1: return 7; // Sunday
    case 2: return 1; // Monday
    case 3: return 2; // Tuesday
    case 4: return 3; // Wednesday
    case 5: return 4; // Thursday
    case 6: return 5; // Friday
  }

  return 1;
}

RtcDateTime datePlusDays(RtcDateTime dt, int daysToAdd) {
  while (daysToAdd > 0) {
    dt.day++;

    if (dt.day > daysInMonth(dt.year, dt.month)) {
      dt.day = 1;
      dt.month++;

      if (dt.month > 12) {
        dt.month = 1;
        dt.year++;
      }
    }

    dt.dow++;

    if (dt.dow > 7) {
      dt.dow = 1;
    }

    daysToAdd--;
  }

  while (daysToAdd < 0) {
    dt.day--;

    if (dt.day < 1) {
      dt.month--;

      if (dt.month < 1) {
        dt.month = 12;
        dt.year--;
      }

      dt.day = daysInMonth(dt.year, dt.month);
    }

    dt.dow--;

    if (dt.dow < 1) {
      dt.dow = 7;
    }

    daysToAdd++;
  }

  return dt;
}

const char* monthNameFull(int month) {
  switch (month) {
    case 1: return "January";
    case 2: return "February";
    case 3: return "March";
    case 4: return "April";
    case 5: return "May";
    case 6: return "June";
    case 7: return "July";
    case 8: return "August";
    case 9: return "September";
    case 10: return "October";
    case 11: return "November";
    case 12: return "December";
    default: return "Invalid";
  }
}

const char* monthNameShort(int month) {
  switch (month) {
    case 1: return "Jan";
    case 2: return "Feb";
    case 3: return "Mar";
    case 4: return "Apr";
    case 5: return "May";
    case 6: return "Jun";
    case 7: return "Jul";
    case 8: return "Aug";
    case 9: return "Sep";
    case 10: return "Oct";
    case 11: return "Nov";
    case 12: return "Dec";
    default: return "Invalid";
  }
}

const char* weekdayNameFull(int dow) {
  switch (dow) {
    case 1: return "Monday";
    case 2: return "Tuesday";
    case 3: return "Wednesday";
    case 4: return "Thursday";
    case 5: return "Friday";
    case 6: return "Saturday";
    case 7: return "Sunday";
    default: return "Invalid";
  }
}

const char* weekdayNameShort(int dow) {
  switch (dow) {
    case 1: return "Mon";
    case 2: return "Tue";
    case 3: return "Wed";
    case 4: return "Thu";
    case 5: return "Fri";
    case 6: return "Sat";
    case 7: return "Sun";
    default: return "Inv";
  }
}

String formatTimezoneOffset(int offsetMinutes) {
  char buffer[8];
  char sign = offsetMinutes >= 0 ? '+' : '-';
  int absolute = abs(offsetMinutes);
  snprintf(buffer, sizeof(buffer), "%c%02d:%02d", sign, absolute / 60, absolute % 60);
  return String(buffer);
}

String formatCustomDateTime(const String& pattern, const RtcDateTime& dt) {
  String output;
  int dow = dt.dow;
  if (dow < 1 || dow > 7) {
    dow = weekdayMonday1(dt.year, dt.month, dt.day);
  }
  int hour12 = dt.hour % 12;
  if (hour12 == 0) hour12 = 12;

  for (int index = 0; index < pattern.length();) {
    if (pattern.startsWith("YYYY", index)) { output += String(dt.year); index += 4; }
    else if (pattern.startsWith("YY", index)) { if (dt.year % 100 < 10) output += "0"; output += String(dt.year % 100); index += 2; }
    else if (pattern.startsWith("MMMM", index)) { output += monthNameFull(dt.month); index += 4; }
    else if (pattern.startsWith("MMM", index)) { output += monthNameShort(dt.month); index += 3; }
    else if (pattern.startsWith("MM", index)) { if (dt.month < 10) output += "0"; output += String(dt.month); index += 2; }
    else if (pattern.startsWith("M", index)) { output += String(dt.month); index += 1; }
    else if (pattern.startsWith("DD", index)) { if (dt.day < 10) output += "0"; output += String(dt.day); index += 2; }
    else if (pattern.startsWith("D", index)) { output += String(dt.day); index += 1; }
    else if (pattern.startsWith("dddd", index)) { output += weekdayNameFull(dow); index += 4; }
    else if (pattern.startsWith("ddd", index)) { output += weekdayNameShort(dow); index += 3; }
    else if (pattern.startsWith("HH", index)) { if (dt.hour < 10) output += "0"; output += String(dt.hour); index += 2; }
    else if (pattern.startsWith("H", index)) { output += String(dt.hour); index += 1; }
    else if (pattern.startsWith("hh", index)) { if (hour12 < 10) output += "0"; output += String(hour12); index += 2; }
    else if (pattern.startsWith("h", index)) { output += String(hour12); index += 1; }
    else if (pattern.startsWith("mm", index)) { if (dt.minute < 10) output += "0"; output += String(dt.minute); index += 2; }
    else if (pattern.startsWith("ss", index)) { if (dt.second < 10) output += "0"; output += String(dt.second); index += 2; }
    else if (pattern.startsWith("A", index)) { output += dt.hour >= 12 ? "PM" : "AM"; index += 1; }
    else if (pattern.startsWith("a", index)) { output += dt.hour >= 12 ? "pm" : "am"; index += 1; }
    else if (pattern.startsWith("Z", index)) { output += formatTimezoneOffset(clockTimezoneOffsetMinutes); index += 1; }
    else if (pattern.startsWith("z", index)) { output += clockTimezoneName; index += 1; }
    else { output += pattern[index]; index += 1; }
  }

  return output;
}

void loadClockMetadata() {
  if (!LittleFS.exists(CLOCK_META_FILE)) {
    return;
  }

  File file = LittleFS.open(CLOCK_META_FILE, "r");
  if (!file) {
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    return;
  }

  clockTimezoneName = doc["timezoneName"] | "Local";
  clockTimezoneOffsetMinutes = doc["timezoneOffsetMinutes"] | 0;
}

bool saveClockMetadata(const String& timezoneName, int offsetMinutes) {
  clockTimezoneName = timezoneName.length() ? timezoneName : "Local";
  clockTimezoneOffsetMinutes = offsetMinutes;

  JsonDocument doc;
  doc["timezoneName"] = clockTimezoneName;
  doc["timezoneOffsetMinutes"] = clockTimezoneOffsetMinutes;

  LittleFS.remove(CLOCK_META_TEMP_FILE);
  File file = LittleFS.open(CLOCK_META_TEMP_FILE, "w");
  if (!file) {
    return false;
  }

  serializeJson(doc, file);
  file.close();
  LittleFS.remove(CLOCK_META_FILE);
  return LittleFS.rename(CLOCK_META_TEMP_FILE, CLOCK_META_FILE);
}

bool validateDateTime(const RtcDateTime& dt) {
  if (dt.year < 2000 || dt.year > 2099) return false;
  if (dt.month < 1 || dt.month > 12) return false;
  if (dt.day < 1 || dt.day > daysInMonth(dt.year, dt.month)) return false;
  if (dt.hour < 0 || dt.hour > 23) return false;
  if (dt.minute < 0 || dt.minute > 59) return false;
  if (dt.second < 0 || dt.second > 59) return false;

  return true;
}

String readFirstUsefulLine(const char* filename) {
  File file = LittleFS.open(filename, "r");

  if (!file) {
    return "";
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) {
      continue;
    }

    if (line.startsWith("#")) {
      continue;
    }

    int commentIndex = line.indexOf('#');
    if (commentIndex >= 0) {
      line = line.substring(0, commentIndex);
      line.trim();
    }

    if (line.length() > 0) {
      file.close();
      return line;
    }
  }

  file.close();
  return "";
}

bool parseTimestampFile(const char* filename, RtcDateTime& out) {
  String line = readFirstUsefulLine(filename);

  if (line.length() == 0) {
    Serial.println("Timestamp file is empty or only contains comments.");
    return false;
  }

  line.replace('T', ' ');
  line.replace('/', '-');
  line.trim();

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;

  int matched = sscanf(
    line.c_str(),
    "%d-%d-%d %d:%d:%d",
    &year,
    &month,
    &day,
    &hour,
    &minute,
    &second
  );

  if (matched == 3) {
    hour = 0;
    minute = 0;
    second = 0;
  } else if (matched == 5) {
    second = 0;
  } else if (matched != 6) {
    Serial.print("Could not parse timestamp line: ");
    Serial.println(line);
    return false;
  }

  out.year = year;
  out.month = month;
  out.day = day;
  out.hour = hour;
  out.minute = minute;
  out.second = second;
  out.dow = weekdayMonday1(year, month, day);

  if (!validateDateTime(out)) {
    Serial.print("Parsed timestamp is invalid: ");
    Serial.println(line);
    return false;
  }

  return true;
}

bool rtcPresent() {
  Wire.beginTransmission(RTC_ADDR);
  return Wire.endTransmission() == 0;
}

uint8_t rtcReadRegister(uint8_t reg) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);

  Wire.requestFrom(RTC_ADDR, (uint8_t)1);

  if (Wire.available()) {
    return Wire.read();
  }

  return 0;
}

void rtcWriteRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

bool rtcOscillatorStopFlagSet() {
  uint8_t status = rtcReadRegister(0x0F);
  return (status & 0x80) != 0;
}

void rtcClearOscillatorStopFlag() {
  uint8_t status = rtcReadRegister(0x0F);
  status &= ~0x80;
  rtcWriteRegister(0x0F, status);
}

void rtcSetDateTime(const RtcDateTime& dt) {
  Wire.beginTransmission(RTC_ADDR);

  Wire.write(0x00); // Start at seconds register

  Wire.write(decToBcd(dt.second));
  Wire.write(decToBcd(dt.minute));
  Wire.write(decToBcd(dt.hour));          // 24-hour mode
  Wire.write(decToBcd(dt.dow));           // 1..7
  Wire.write(decToBcd(dt.day));
  Wire.write(decToBcd(dt.month));
  Wire.write(decToBcd(dt.year - 2000));

  Wire.endTransmission();
}

RtcDateTime rtcGetDateTime() {
  RtcDateTime dt = {};

  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x00);
  Wire.endTransmission(false);

  Wire.requestFrom(RTC_ADDR, (uint8_t)7);

  if (Wire.available() < 7) {
    return dt;
  }

  uint8_t rawSecond = Wire.read();
  uint8_t rawMinute = Wire.read();
  uint8_t rawHour = Wire.read();
  uint8_t rawDow = Wire.read();
  uint8_t rawDay = Wire.read();
  uint8_t rawMonth = Wire.read();
  uint8_t rawYear = Wire.read();

  dt.second = bcdToDec(rawSecond & 0x7F);
  dt.minute = bcdToDec(rawMinute & 0x7F);
  dt.hour = bcdToDec(rawHour & 0x3F);
  dt.dow = bcdToDec(rawDow & 0x07);
  dt.day = bcdToDec(rawDay & 0x3F);
  dt.month = bcdToDec(rawMonth & 0x1F);
  dt.year = 2000 + bcdToDec(rawYear);

  return dt;
}

void printDateTime(const char* label, const RtcDateTime& dt) {
  Serial.print(label);
  Serial.print(": ");

  char buffer[40];

  snprintf(
    buffer,
    sizeof(buffer),
    "%04d-%02d-%02d %02d:%02d:%02d DOW=%d",
    dt.year,
    dt.month,
    dt.day,
    dt.hour,
    dt.minute,
    dt.second,
    dt.dow
  );

  Serial.println(buffer);
}

void handleTimestampFile() {
  if (!LittleFS.exists(TIMESTAMP_FILE)) {
    Serial.println("Timestamp.txt not found. RTC was not changed.");
    return;
  }

  Serial.println("Found Timestamp.txt");

  RtcDateTime dt = {};

  if (!parseTimestampFile(TIMESTAMP_FILE, dt)) {
    Serial.println("ERROR: Timestamp.txt was found but could not be parsed.");
    return;
  }

  rtcSetDateTime(dt);
  rtcClearOscillatorStopFlag();

  printDateTime("RTC set from Timestamp.txt", dt);

  if (RENAME_TIMESTAMP_FILE_AFTER_SUCCESS) {
    LittleFS.remove("/Timestamp.set");

    if (LittleFS.rename(TIMESTAMP_FILE, "/Timestamp.set")) {
      Serial.println("Timestamp.txt renamed to Timestamp.set");
    } else {
      Serial.println("WARNING: Could not rename Timestamp.txt");
    }
  }
}

// ============================================================
// Arduino setup / loop
// ============================================================

void setup() {
  // Critical ordering:
  // Configure HID first so the PC sees the keyboard interface
  // when the USB device enumerates.
  configureUsbDeviceKeyboard();

  Serial.begin(115200);
  serialInputLine.reserve(MAX_CONFIG_BYTES);
  typedBuffer.reserve(MAX_TRIGGER_BUFFER);
  typedSources.reserve(MAX_TRIGGER_BUFFER);

  delay(1500);

  Serial.println();
  Serial.println("Booting...");
  Serial.println("Native USB HID keyboard configured.");

  uint32_t mountStart = millis();
  while (!TinyUSBDevice.mounted() && millis() - mountStart < 5000) {
    delay(10);
  }

  Serial.print("TinyUSB mounted: ");
  Serial.println(TinyUSBDevice.mounted() ? "yes" : "no");

  configureUsbHost();

  Wire.setSDA(SDA_PIN);
  Wire.setSCL(SCL_PIN);
  Wire.begin();

  if (!LittleFS.begin()) {
    Serial.println("ERROR: LittleFS mount failed.");
    return;
  }

  Serial.println("LittleFS mounted.");
  loadConfigurationFromStorage();
  loadClockMetadata();

  if (!rtcPresent()) {
    Serial.println("ERROR: RTC not found at I2C address 0x68.");
    return;
  }

  Serial.println("RTC found at 0x68.");

  if (rtcOscillatorStopFlagSet()) {
    Serial.println("WARNING: RTC oscillator stop flag is set. Time may be invalid.");
  }

  handleTimestampFile();

  RtcDateTime now = rtcGetDateTime();
  printDateTime("RTC time", now);

  Serial.println("Ready. Open Notepad and test keypad macros.");
}

void loop() {
  USBHost.task();
  processSerialProtocol();
}
