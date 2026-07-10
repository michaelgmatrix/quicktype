#include <Arduino.h>
#include <Wire.h>
#include <LittleFS.h>

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

static hid_keyboard_report_t previousKeyboardReport = {};

// Forward declarations
uint8_t decToBcd(int value);
int bcdToDec(uint8_t value);
bool isLeapYear(int year);
int daysInMonth(int year, int month);
int weekdayMonday1(int year, int month, int day);
RtcDateTime datePlusDays(RtcDateTime dt, int daysToAdd);
const char* monthNameFull(int month);

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
bool typeAsciiChar(char c);
bool typeAsciiString(const char* text);
bool sendLeftArrows(uint8_t count);
bool sendHomeEnterEnterUp();
bool sendHomeEnterEnterUpCtrlB();

void outputSlashMacro();
void outputShortDateInitialsAndMoveCursor();
void outputAppointmentConfirmationMacro(const char* appointmentWindowText);
void outputDateFormatForKey(uint8_t keycode);

const char* keypadUsageName(uint8_t usage);
bool keyWasInPreviousReport(uint8_t keycode);
void handleKeyboardReport(hid_keyboard_report_t const* report);

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
  if (!waitForUsbHidReady(1000)) {
    return false;
  }

  uint8_t keycodes[6] = { 0 };
  keycodes[0] = keycode;

  if (!usb_hid.keyboardReport(0, modifier, keycodes)) {
    Serial.println("ERROR: Failed to send HID key press.");
    return false;
  }

  delay(5);

  if (!usb_hid.keyboardRelease(0)) {
    Serial.println("ERROR: Failed to send HID key release.");
    return false;
  }

  delay(5);

  return true;
}

bool typeAsciiChar(char c) {
  uint8_t modifier = 0;
  uint8_t keycode = 0;

  if (c >= 'a' && c <= 'z') {
    keycode = HID_KEY_A + (c - 'a');
  } else if (c >= 'A' && c <= 'Z') {
    modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
    keycode = HID_KEY_A + (c - 'A');
  } else if (c >= '1' && c <= '9') {
    keycode = HID_KEY_1 + (c - '1');
  } else if (c == '0') {
    keycode = HID_KEY_0;
  } else {
    switch (c) {
      case ' ':
        keycode = HID_KEY_SPACE;
        break;

      case '-':
        keycode = HID_KEY_MINUS;
        break;

      case '/':
        keycode = HID_KEY_SLASH;
        break;

      case ':':
        modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
        keycode = HID_KEY_SEMICOLON;
        break;

      case '.':
        keycode = HID_KEY_PERIOD;
        break;

      case ',':
        keycode = HID_KEY_COMMA;
        break;

      case '@':
        modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
        keycode = HID_KEY_2;
        break;

      default:
        Serial.print("ERROR: Unsupported character for HID typing: ");
        Serial.println(c);
        return false;
    }
  }

  return sendHidKey(modifier, keycode);
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

void handleKeyboardReport(hid_keyboard_report_t const* report) {
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

    switch (keycode) {
      case 0x53: // Keypad NumLock/Clear
        typeAsciiString("232175");
        break;

      case 0x54: // Keypad /
        outputSlashMacro();
        break;

      case 0x55: // Keypad *
        outputShortDateInitialsAndMoveCursor();
        break;

      case 0x56: // Keypad -
        typeAsciiString("--------------------");
        break;

      case 0x58: // Keypad Enter
      case 0x28: // Regular Enter
        sendHidKey(0, HID_KEY_ENTER);
        break;

      case 0x2A: // Backspace
        sendHomeEnterEnterUpCtrlB();
        break;

      case 0x5F: // Keypad 7 / Home
        outputAppointmentConfirmationMacro("conf 8-12 appt. on ");
        break;

      case 0x60: // Keypad 8 / Up
        outputAppointmentConfirmationMacro("conf 1-5 appt. on ");
        break;

      case 0x62: // Keypad 0 / Insert
        typeAsciiString("Customer");
        break;

      case 0xB0: // Keypad 00
        typeAsciiString("LVM to");
        break;

      case 0x61: // Keypad 9 / PageUp
      case 0x5E: // Keypad 6 / Right
      case 0x5D: // Keypad 5
      case 0x5C: // Keypad 4 / Left
      case 0x5B: // Keypad 3 / PageDown
      case 0x5A: // Keypad 2 / Down
      case 0x59: // Keypad 1 / End
        outputDateFormatForKey(keycode);
        break;

      default:
        break;
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
}