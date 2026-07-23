// QuickType firmware version: 0.2.88 (2026-07-23)
#include <Arduino.h>
#include <Wire.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include <pio_usb.h>
#include <Adafruit_TinyUSB.h>
#include <tusb.h>
#include <hardware/watchdog.h>
#include <hardware/clocks.h>

// ============================================================
// RP2040-Zero wiring
// ============================================================
//
// Native USB-C port:
//   Acts as USB HID keyboard device to the PC.
//   Also keeps USB Serial available for testing/debug.
//
// RTC DS3231 I2C:
//   GPIO6 = SDA
//   GPIO7 = SCL
//
// USB keypad host port using Pico-PIO-USB:
//   USB GREEN wire = D+ = GPIO0
//   USB WHITE wire = D- = GPIO1
//   USB RED wire   = +5V / VBUS
//   USB BLACK wire = GND
//
// Pico-PIO-USB rule:
//   USB_HOST_DP_GPIO is D+
//   D- is automatically USB_HOST_DP_GPIO + 1

static constexpr int SDA_PIN = 6;
static constexpr int SCL_PIN = 7;

static constexpr int USB_HOST_DP_GPIO = 0; // GPIO0 / USB green D+
static constexpr int USB_HOST_DM_GPIO = 1; // GPIO1 / USB white D-

static constexpr uint8_t RTC_ADDR = 0x68;
static constexpr char TIMESTAMP_FILE[] = "/Timestamp.txt";
static constexpr char CONFIG_FILE[] = "/quicktype-config.json";
static constexpr char CONFIG_TEMP_FILE[] = "/quicktype-config.tmp";
static constexpr char CONFIG_BACKUP_FILE[] = "/quicktype-config.bak";
static constexpr char CLOCK_META_FILE[] = "/quicktype-clock.json";
static constexpr char CLOCK_META_TEMP_FILE[] = "/quicktype-clock.tmp";
static constexpr char FIRMWARE_VERSION[] = "0.2.88"; // v0.2.88: Set explicit CDC Serial string descriptor to "QuickType Serial"
//
    //          "QuickType v0.2.84 requires the PR #206-tested 240 MHz PIO host clock");
static constexpr uint8_t CONFIG_SCHEMA_VERSION = 1;
static constexpr size_t MAX_CONFIG_BYTES = 32768;
static constexpr size_t MAX_CONFIG_RULES = 48;
static constexpr size_t MAX_RULE_STEPS = 8;
static constexpr size_t MAX_CONFIG_PLACEHOLDERS = 32;
static constexpr size_t MAX_TRIGGER_BUFFER = 64;
static constexpr size_t MAX_HOST_HID_INTERFACES = 8;
static constexpr size_t MAX_CONSUMER_BIT_USAGES = 32;
static constexpr bool ENABLE_SERIAL_DEBUG_LOGS = false;
static constexpr uint32_t SERIAL_STATE_POLL_MS = 250;
static constexpr uint32_t WATCHDOG_TIMEOUT_MS = 8000;
static constexpr uint32_t HOST_KEYBOARD_RECEIVE_TIMEOUT_MS = 5000;
static constexpr uint32_t HOST_KEYBOARD_REARM_DELAY_MS = 10;

enum HidReportId : uint8_t {
  RID_KEYBOARD = 1,
  RID_CONSUMER_CONTROL = 2
};

// Set this to true if you want Timestamp.txt renamed after a successful RTC set.
static constexpr bool RENAME_TIMESTAMP_FILE_AFTER_SUCCESS = false;

// HID report descriptor for the native USB-C device side.
uint8_t const hidReportDescriptor[] = {
  TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KEYBOARD)),
  TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER_CONTROL))
};

// Native USB-C HID keyboard device.
Adafruit_USBD_HID usb_hid;

// Track the active host keyboard LED report status (Num Lock, Caps Lock, etc.)
static uint8_t hostLedsState = 0xFF;

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

struct ConfigPlaceholder {
  String name;
  String value;
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

struct HostHidInterfaceInfo {
  uint8_t devAddr;
  uint8_t instance;
  uint8_t keyboardReportId;
  uint8_t consumerReportId;
  uint32_t nextReportRequestMs;
  uint32_t lastReportMs;
  uint32_t lastRecoveryMs;
  uint32_t rearmAtMs;
  uint16_t consumerBitOffset;
  uint16_t consumerBitUsages[MAX_CONSUMER_BIT_USAGES];
  uint8_t consumerBitCount;
  bool mounted;
  bool keyboard;
  bool consumerControl;
  bool consumerBitmask;
};

struct TelemetryState {
  uint32_t loopCount;
  uint32_t bridgeServiceCount;
  uint32_t hostReportRequestCount;
  uint32_t hostReportRequestFailCount;
  uint32_t hostReportCallbackCount;
  uint32_t hostZeroLengthReportCount;
  uint32_t keyboardReportCount;
  uint32_t keyboardDecodeFailCount;
  uint32_t consumerReportCount;
  uint32_t forwardedKeyboardCount;
  uint32_t forwardedConsumerCount;
  uint32_t keyboardSendFailCount;
  uint32_t consumerSendFailCount;
  uint32_t hidNotReadyCount;
  uint32_t hostQuiesceCount;
  uint32_t hostRecoverCount;
  uint32_t hostReceiveAbortCount;
  uint32_t hostReceiveAbortFailCount;
  uint32_t configWriteCount;
  uint32_t configWriteFailCount;
  uint32_t protocolCommandCount;
  uint32_t serialDisconnectCount;
  uint32_t keyPressCount;
  uint32_t hostMountCount;
  uint32_t hostUnmountCount;
  uint32_t lastLoopMs;
  uint32_t maxLoopGapMs;
  uint32_t lastHostReportMs;
  uint32_t lastKeyboardReportMs;
  uint32_t lastConsumerReportMs;
  uint32_t lastForwardedKeyboardMs;
  uint32_t lastForwardedConsumerMs;
  uint32_t lastHostQuiesceMs;
  uint32_t lastHostRecoverMs;
  uint32_t lastConfigWriteMs;
  uint32_t lastProtocolCommandMs;
  uint32_t lastHostMountMs;
  uint32_t lastHostUnmountMs;
  uint32_t lastHeartbeatMs;
  uint16_t lastConsumerUsage;
  uint8_t lastKeyUsage;
  uint8_t lastModifier;
  bool serialWasConnected;
};

void serviceNativeUsb();

class CooperativeFileWriter : public Print {
public:
  explicit CooperativeFileWriter(File& file) : fileRef(file) {}

  size_t write(uint8_t value) override {
    size_t written = fileRef.write(value);
    serviceProgress();
    return written;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    size_t totalWritten = 0;
    while (totalWritten < size) {
      size_t chunkSize = min((size_t)64, size - totalWritten);
      size_t written = fileRef.write(buffer + totalWritten, chunkSize);
      totalWritten += written;
      serviceNativeUsb();
      if (written != chunkSize) {
        break;
      }
    }
    return totalWritten;
  }

private:
  File& fileRef;
  size_t bytesSinceService = 0;

  void serviceProgress() {
    bytesSinceService++;
    if (bytesSinceService >= 64) {
      serviceNativeUsb();
      bytesSinceService = 0;
    }
  }
};

static hid_keyboard_report_t previousKeyboardReport = {};
static hid_keyboard_report_t pendingKeyboardReport = {};
static HostHidInterfaceInfo hostHidInterfaces[MAX_HOST_HID_INTERFACES];
static TelemetryState telemetry = {};
static uint16_t activeTopRowConsumerUsage = 0;
static uint16_t pendingConsumerUsage = 0;
static volatile bool pendingKeyboardReportValid = false;
static uint32_t lastWakeupAttemptMs = 0;
static volatile bool deviceConnectionResetPending = false;
static volatile bool hostStackSuspended = false;
static volatile bool hostRemoteWakeupEnabled = false;
static constexpr size_t CONSUMER_QUEUE_SIZE = 8;
static volatile uint16_t consumerUsageQueue[CONSUMER_QUEUE_SIZE];
static volatile size_t consumerQueueHead = 0;
static volatile size_t consumerQueueTail = 0;
static constexpr size_t KEYBOARD_REPORT_QUEUE_SIZE = 16;
static hid_keyboard_report_t keyboardReportQueue[KEYBOARD_REPORT_QUEUE_SIZE];
static volatile size_t keyboardQueueHead = 0;
static volatile size_t keyboardQueueTail = 0;
static ConfigRule configRules[MAX_CONFIG_RULES];
static size_t configRuleCount = 0;
static ConfigPlaceholder configPlaceholders[MAX_CONFIG_PLACEHOLDERS];
static size_t configPlaceholderCount = 0;
static bool storedConfigurationLoaded = false;
static bool expansionsEnabled = true;
static bool keypadExpansionsEnabled = true;
static String serialInputLine;
static bool serialInputOverflow = false;
static String typedBuffer;
static String typedSources;
static String clockTimezoneName = "Local";
static int clockTimezoneOffsetMinutes = 0;
static uint32_t lastSerialStatePollMs = 0;
static bool watchdogStarted = false;

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
bool serialDebugConnected();
bool serialProtocolConnected();
void resetSerialProtocolInput();
bool serialWriteBytes(uint8_t const* data, size_t size);
bool serialWriteText(const char* text);
bool serialWriteUInt(uint32_t value);
bool sendProtocolJson(JsonDocument& response);
void serviceNativeUsb();
void cooperativeDelay(uint32_t delayMs);
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
bool hasShortcutModifier(uint8_t modifier);
bool decodeKeyboardReport(uint8_t const* report, uint16_t len, uint8_t expectedReportId, hid_keyboard_report_t& out);
bool usbHidReadyNow();
bool sendKeyboardReportNow(hid_keyboard_report_t const* report);
bool sendConsumerUsageNow(uint16_t usage);
void servicePendingHidReports();
bool forwardKeyboardReport(hid_keyboard_report_t const* report);
uint16_t topRowConsumerUsage(uint8_t keycode);
bool translateTopRowConsumerKeys(hid_keyboard_report_t const* report, hid_keyboard_report_t& keyboardOut);
HostHidInterfaceInfo* hostHidInterfaceInfo(uint8_t dev_addr, uint8_t instance);
void clearHostHidInterface(uint8_t dev_addr, uint8_t instance);
void parseHostHidDescriptor(
  uint8_t const* desc_report,
  uint16_t desc_len,
  bool& keyboard,
  uint8_t& keyboardReportId,
  bool& consumerControl,
  uint8_t& consumerReportId
);
void rememberHostHidInterface(
  uint8_t dev_addr,
  uint8_t instance,
  bool keyboard,
  uint8_t keyboardReportId,
  bool consumerControl,
  uint8_t consumerReportId
);
void parseConsumerBitmaskDescriptor(uint8_t const* desc_report, uint16_t desc_len, HostHidInterfaceInfo& info);
bool sendConsumerUsage(uint16_t usage);
bool forwardConsumerControlReport(uint8_t const* report, uint16_t len, const HostHidInterfaceInfo& info);
bool handleLegacyKey(uint8_t keycode);
char hidKeycodeToAscii(uint8_t keycode, uint8_t modifier, bool& isNumpad);
bool processPhysicalKeyRule(uint8_t keycode);
void recordTypedCharacter(char value, bool isNumpad);
bool outputExpansionListTable();
bool outputDiagnosticInformation();
bool processTypedTriggerRules(char currentCharacter, bool delimiterRecorded = true);
bool executeConfiguredRule(const ConfigRule& rule, size_t eraseCount, char delimiterToRestore);
bool typeExpansionTemplate(const String& text, uint16_t keyDelayMs);
String customPlaceholderValue(const String& name);
bool sendShortcut(const String& shortcut, uint16_t keyDelayMs);
uint8_t shortcutKeycode(const String& token);
void handleKeyboardReport(hid_keyboard_report_t const* report);

void clearCompiledConfiguration();
bool compileConfiguration(JsonVariantConst config, String& errorMessage);
size_t activeRuleCount();
void recoverConfigurationStorage();
bool verifyConfigurationFile(const char* filename, size_t expectedSize);
bool loadConfigurationFromStorage();
bool saveConfiguration(JsonVariantConst config, String& errorMessage);
void processSerialProtocol();
void handleProtocolLine(const String& line);
void sendProtocolError(uint32_t id, const char* code, const String& message);
void sendProtocolSuccess(uint32_t id, const char* type);
void sendProtocolInfo(uint32_t id);
void sendStoredConfiguration(uint32_t id);
void sendProtocolClock(uint32_t id);
void sendProtocolTelemetry(uint32_t id);
void addTelemetryToJson(JsonObject target);
void emitTelemetryHeartbeat();
size_t mountedHostInterfaceCount();
size_t mountedKeyboardInterfaceCount();
size_t mountedConsumerInterfaceCount();
void requestNextHidReport(uint8_t dev_addr, uint8_t instance);
void pollMountedHidReports();
void serviceUsbBridge();
void resetBridgeStateAfterConfigurationChange();

// ============================================================
// Native USB HID keyboard device setup
// ============================================================

void hid_report_callback(uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
  if (report_type != HID_REPORT_TYPE_OUTPUT || buffer == nullptr || bufsize == 0) return;

  if (report_id == RID_KEYBOARD || report_id == 0) {
    uint8_t leds = buffer[0];

    if (leds != hostLedsState) {
      hostLedsState = leds;

      if (serialDebugConnected()) {
        Serial.print("Received keyboard LED report from PC: 0x");
        Serial.println(hostLedsState, HEX);
      }
      // Keep the PC's LED state for Alt-code handling, but do not send a host
      // control transfer to the Logitech receiver. Its keyboard input path is
      // intentionally interrupt-IN only for PIO USB stability.
    }
  }
}

void configureUsbDeviceKeyboard() {
  // IMPORTANT:
  // This must be called before Serial.begin() and before delay(),
  // so the HID and Serial interfaces exist when the PC enumerates the USB device.

  TinyUSBDevice.setID(0x2E8A, 0x5154); // Custom RP2040 VID:0x2E8A (11914), PID:0x5154 (20820 - 'QT')
  TinyUSBDevice.setManufacturerDescriptor("QuickType");
  TinyUSBDevice.setProductDescriptor("QuickType Configurator");
  Serial.setStringDescriptor("QuickType Serial");

  usb_hid.setPollInterval(2);
  usb_hid.setReportDescriptor(hidReportDescriptor, sizeof(hidReportDescriptor));
  usb_hid.setStringDescriptor("QuickType Keyboard");
  usb_hid.setReportCallback(NULL, hid_report_callback);
  usb_hid.begin();
}

bool serialDebugConnected() {
  return ENABLE_SERIAL_DEBUG_LOGS && serialProtocolConnected();
}

bool serialProtocolConnected() {
  return Serial && Serial.dtr();
}

void resetSerialProtocolInput() {
  serialInputLine = "";
  serialInputOverflow = false;
}

bool serialWriteBytes(uint8_t const* data, size_t size) {
  if (data == nullptr && size > 0) {
    return false;
  }

  size_t written = 0;
  uint32_t blockedSince = millis();
  while (written < size) {
    serviceNativeUsb();

    if (!serialProtocolConnected()) {
      resetSerialProtocolInput();
      return false;
    }

    int available = Serial.availableForWrite();
    if (available <= 0) {
      if (millis() - blockedSince > 25) {
        resetSerialProtocolInput();
        return false;
      }
      delay(1);
      continue;
    }

    blockedSince = millis();
    size_t chunkSize = min((size_t)available, size - written);
    size_t chunkWritten = Serial.write(data + written, chunkSize);
    if (chunkWritten == 0) {
      delay(1);
      continue;
    }
    written += chunkWritten;
  }

  return true;
}

bool serialWriteText(const char* text) {
  if (text == nullptr) {
    return false;
  }
  return serialWriteBytes((uint8_t const*)text, strlen(text));
}

bool serialWriteUInt(uint32_t value) {
  char buffer[11];
  snprintf(buffer, sizeof(buffer), "%lu", (unsigned long)value);
  return serialWriteText(buffer);
}

bool sendProtocolJson(JsonDocument& response) {
  if (!serialProtocolConnected()) {
    resetSerialProtocolInput();
    return false;
  }

  String output;
  serializeJson(response, output);
  output += '\n';
  return serialWriteBytes((uint8_t const*)output.c_str(), output.length());
}

void serviceNativeUsb() {
  if (watchdogStarted) {
    watchdog_update();
  }
  TinyUSB_Device_Task();
}

void disableWatchdog() {
  if (watchdogStarted) {
    watchdog_hw->ctrl &= ~WATCHDOG_CTRL_ENABLE_BITS;
  }
}

void enableWatchdog() {
  if (watchdogStarted) {
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
  }
}

void suspendHostStack() {
  hostStackSuspended = true;
  // Allow Core 1 to finish its current PIO USB task before storage writes.
  delay(10);
}

void resumeHostStack() {
  hostStackSuspended = false;
}

size_t mountedHostInterfaceCount() {
  size_t count = 0;
  for (size_t index = 0; index < MAX_HOST_HID_INTERFACES; index++) {
    if (hostHidInterfaces[index].mounted) count++;
  }
  return count;
}

size_t mountedKeyboardInterfaceCount() {
  size_t count = 0;
  for (size_t index = 0; index < MAX_HOST_HID_INTERFACES; index++) {
    if (hostHidInterfaces[index].mounted && hostHidInterfaces[index].keyboard) count++;
  }
  return count;
}

size_t mountedConsumerInterfaceCount() {
  size_t count = 0;
  for (size_t index = 0; index < MAX_HOST_HID_INTERFACES; index++) {
    if (hostHidInterfaces[index].mounted && hostHidInterfaces[index].consumerControl) count++;
  }
  return count;
}

void addTelemetryToJson(JsonObject target) {
  target["uptimeMs"] = millis();
  target["freeHeap"] = rp2040.getFreeHeap();
  target["serialConnected"] = serialDebugConnected();
  target["tinyUsbMounted"] = TinyUSBDevice.mounted();
  target["tinyUsbSuspended"] = TinyUSBDevice.suspended();
  target["nativeHidReady"] = usb_hid.ready();
  target["storedConfig"] = storedConfigurationLoaded;
  target["expansionsEnabled"] = expansionsEnabled;
  target["keypadExpansionsEnabled"] = keypadExpansionsEnabled;
  target["activeRules"] = activeRuleCount();
  target["compiledRules"] = configRuleCount;
  target["hostInterfaces"] = mountedHostInterfaceCount();
  target["keyboardInterfaces"] = mountedKeyboardInterfaceCount();
  target["consumerInterfaces"] = mountedConsumerInterfaceCount();
  target["pendingKeyboard"] = pendingKeyboardReportValid;
  target["pendingConsumer"] = (consumerQueueTail != consumerQueueHead);
  target["typedBufferLength"] = typedBuffer.length();

  JsonObject counters = target["counters"].to<JsonObject>();
  counters["loop"] = telemetry.loopCount;
  counters["bridgeService"] = telemetry.bridgeServiceCount;
  counters["hostReportRequests"] = telemetry.hostReportRequestCount;
  counters["hostReportRequestFails"] = telemetry.hostReportRequestFailCount;
  counters["hostReportCallbacks"] = telemetry.hostReportCallbackCount;
  counters["zeroLengthReports"] = telemetry.hostZeroLengthReportCount;
  counters["keyboardReports"] = telemetry.keyboardReportCount;
  counters["keyboardDecodeFails"] = telemetry.keyboardDecodeFailCount;
  counters["consumerReports"] = telemetry.consumerReportCount;
  counters["forwardedKeyboard"] = telemetry.forwardedKeyboardCount;
  counters["forwardedConsumer"] = telemetry.forwardedConsumerCount;
  counters["keyboardSendFails"] = telemetry.keyboardSendFailCount;
  counters["consumerSendFails"] = telemetry.consumerSendFailCount;
  counters["hidNotReady"] = telemetry.hidNotReadyCount;
  counters["hostQuiesces"] = telemetry.hostQuiesceCount;
  counters["hostRecovers"] = telemetry.hostRecoverCount;
  counters["hostReceiveAborts"] = telemetry.hostReceiveAbortCount;
  counters["hostReceiveAbortFails"] = telemetry.hostReceiveAbortFailCount;
  counters["configWrites"] = telemetry.configWriteCount;
  counters["configWriteFails"] = telemetry.configWriteFailCount;
  counters["protocolCommands"] = telemetry.protocolCommandCount;
  counters["serialDisconnects"] = telemetry.serialDisconnectCount;
  counters["keyPresses"] = telemetry.keyPressCount;
  counters["hostMounts"] = telemetry.hostMountCount;
  counters["hostUnmounts"] = telemetry.hostUnmountCount;

  JsonObject last = target["last"].to<JsonObject>();
  last["loopMs"] = telemetry.lastLoopMs;
  last["maxLoopGapMs"] = telemetry.maxLoopGapMs;
  last["hostReportMs"] = telemetry.lastHostReportMs;
  last["keyboardReportMs"] = telemetry.lastKeyboardReportMs;
  last["consumerReportMs"] = telemetry.lastConsumerReportMs;
  last["forwardedKeyboardMs"] = telemetry.lastForwardedKeyboardMs;
  last["forwardedConsumerMs"] = telemetry.lastForwardedConsumerMs;
  last["hostQuiesceMs"] = telemetry.lastHostQuiesceMs;
  last["hostRecoverMs"] = telemetry.lastHostRecoverMs;
  last["configWriteMs"] = telemetry.lastConfigWriteMs;
  last["protocolCommandMs"] = telemetry.lastProtocolCommandMs;
  last["keyUsage"] = telemetry.lastKeyUsage;
  last["modifier"] = telemetry.lastModifier;
  last["consumerUsage"] = telemetry.lastConsumerUsage;
  last["hostMountMs"] = telemetry.lastHostMountMs;
  last["hostUnmountMs"] = telemetry.lastHostUnmountMs;
}

void emitTelemetryHeartbeat() {
  // Telemetry is pull-only. Serial connection state is serviced in
  // processSerialProtocol() so idle disconnects do not touch CDC every loop.
}

void cooperativeDelay(uint32_t delayMs) {
  if (delayMs == 0) {
    return;
  }

  uint32_t started = millis();
  do {
    serviceNativeUsb();
    delay(1);
  } while (millis() - started < delayMs);
}

bool waitForUsbHidReady(uint32_t timeoutMs) {
  uint32_t started = millis();

  while (!usb_hid.ready()) {
    serviceNativeUsb();

    if (TinyUSBDevice.suspended()) {
      uint32_t now = millis();
      if (now - lastWakeupAttemptMs >= 2000) {
        lastWakeupAttemptMs = now;
        if (serialDebugConnected()) {
          Serial.println("Triggering USB remote wakeup...");
        }
        TinyUSBDevice.remoteWakeup();
      }
    }

    if (millis() - started >= timeoutMs) {
      if (serialDebugConnected()) {
        Serial.println("ERROR: USB HID keyboard is not ready.");
        Serial.print("TinyUSB mounted: ");
        Serial.println(TinyUSBDevice.mounted() ? "yes" : "no");
      }
      return false;
    }

    cooperativeDelay(1);
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

  if (!usb_hid.keyboardReport(RID_KEYBOARD, modifier, keycodes)) {
    if (serialDebugConnected()) {
      Serial.println("ERROR: Failed to send HID key press.");
    }
    return false;
  }

  cooperativeDelay(delayMs);

  if (!usb_hid.keyboardRelease(RID_KEYBOARD)) {
    if (serialDebugConnected()) {
      Serial.println("ERROR: Failed to send HID key release.");
    }
    return false;
  }

  cooperativeDelay(delayMs);

  return true;
}

bool typeAsciiChar(char c) {
  return typeAsciiCharWithDelay(c, 5);
}

bool typeAsciiCharWithDelay(char c, uint16_t delayMs) {
  if (c == '\n' || c == '\r') return sendHidKeyWithDelay(0, HID_KEY_ENTER, delayMs);
  if (c == '\t') return sendHidKeyWithDelay(0, HID_KEY_TAB, delayMs);
  if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) {
    if (serialDebugConnected()) {
      Serial.print("WARNING: Skipping unsupported non-ASCII character for HID typing: 0x");
      Serial.println((uint8_t)c, HEX);
    }
    return true; // Skip character instead of failing the expansion
  }

  if (!waitForUsbHidReady(1000)) return false;
  if (!usb_hid.keyboardPress(RID_KEYBOARD, c)) return false;
  cooperativeDelay(delayMs);
  if (!usb_hid.keyboardRelease(RID_KEYBOARD)) return false;
  cooperativeDelay(delayMs);
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

bool sendRawKeyboardReport(uint8_t modifier, uint8_t keycode, uint16_t delayMs) {
  if (!waitForUsbHidReady(1000)) return false;
  uint8_t keys[6] = {keycode, 0, 0, 0, 0, 0};
  if (!usb_hid.keyboardReport(RID_KEYBOARD, modifier, keys)) return false;
  cooperativeDelay(delayMs);
  return true;
}

bool typeAltCode(const char* digits, uint16_t delayMs) {
  bool toggleNumLock = (hostLedsState != 0xFF) && ((hostLedsState & 0x01) == 0);
  uint16_t altDelay = max((uint16_t)20, delayMs);

  if (toggleNumLock) {
    if (!sendRawKeyboardReport(0, HID_KEY_NUM_LOCK, altDelay)) return false;
    if (!sendRawKeyboardReport(0, 0, altDelay)) return false;
  }

  // Hold ALT
  if (!sendRawKeyboardReport(KEYBOARD_MODIFIER_LEFTALT, 0, altDelay)) return false;

  while (*digits) {
    uint8_t keycode = 0;
    switch (*digits) {
      case '0': keycode = HID_KEY_KEYPAD_0; break;
      case '1': keycode = HID_KEY_KEYPAD_1; break;
      case '2': keycode = HID_KEY_KEYPAD_2; break;
      case '3': keycode = HID_KEY_KEYPAD_3; break;
      case '4': keycode = HID_KEY_KEYPAD_4; break;
      case '5': keycode = HID_KEY_KEYPAD_5; break;
      case '6': keycode = HID_KEY_KEYPAD_6; break;
      case '7': keycode = HID_KEY_KEYPAD_7; break;
      case '8': keycode = HID_KEY_KEYPAD_8; break;
      case '9': keycode = HID_KEY_KEYPAD_9; break;
    }
    if (keycode != 0) {
      if (!sendRawKeyboardReport(KEYBOARD_MODIFIER_LEFTALT, keycode, altDelay)) return false;
      if (!sendRawKeyboardReport(KEYBOARD_MODIFIER_LEFTALT, 0, altDelay)) return false;
    }
    digits++;
  }

  // Release ALT
  if (!waitForUsbHidReady(1000)) return false;
  if (!usb_hid.keyboardRelease(RID_KEYBOARD)) return false;
  cooperativeDelay(altDelay);

  if (toggleNumLock) {
    if (!sendRawKeyboardReport(0, HID_KEY_NUM_LOCK, altDelay)) return false;
    if (!sendRawKeyboardReport(0, 0, altDelay)) return false;
  }

  return true;
}

bool typeAsciiStringWithDelay(const String& text, uint16_t delayMs) {
  for (size_t index = 0; index < text.length();) {
    if (index + 2 < text.length() && (uint8_t)text[index] == 0xE2) {
      uint8_t b1 = (uint8_t)text[index+1];
      uint8_t b2 = (uint8_t)text[index+2];
      const char* altDigits = nullptr;

      if (b1 == 0x80 && b2 == 0xA2) altDigits = "0149"; // •
      else if (b1 == 0x97 && b2 == 0xA6) altDigits = "9702"; // ◦
      else if (b1 == 0x96 && b2 == 0xAA) altDigits = "9642"; // ▪
      else if (b1 == 0x96 && b2 == 0xA1) altDigits = "9633"; // □
      else if (b1 == 0x97 && b2 == 0xBE) altDigits = "9726"; // ◾
      else if (b1 == 0x99 && b2 == 0xA6) altDigits = "4";    // ♦
      else if (b1 == 0x80 && b2 == 0xA3) altDigits = "8227"; // ‣
      else if (b1 == 0x97 && b2 == 0x86) altDigits = "9670"; // ◆
      else if (b1 == 0x97 && b2 == 0x8F) altDigits = "9679"; // ●
      else if (b1 == 0x96 && b2 == 0xA0) altDigits = "9632"; // ■

      if (altDigits != nullptr) {
        if (!typeAltCode(altDigits, delayMs)) return false;
        index += 3;
        continue;
      }
    }

    if (!typeAsciiCharWithDelay(text[index], delayMs)) {
      return false;
    }
    index++;
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
  if (serialDebugConnected()) {
    Serial.println("Sending reusable pattern: HOME, ENTER, ENTER, UP Arrow, CTRL+B.");
  }

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
    if (serialDebugConnected()) {
      Serial.println("ERROR: RTC date/time read is invalid. HID output skipped.");
    }
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

  if (serialDebugConnected()) {
    Serial.print("Typing slash macro: ");
    Serial.println(buffer);
    Serial.println("Then sending Left Arrow x3.");
  }

  if (typeAsciiString(buffer)) {
    sendLeftArrows(3);
  }
}

void outputShortDateInitialsAndMoveCursor() {
  RtcDateTime now = rtcGetDateTime();

  if (!validateDateTime(now)) {
    if (serialDebugConnected()) {
      Serial.println("ERROR: RTC date/time read is invalid. HID output skipped.");
    }
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

  if (serialDebugConnected()) {
    Serial.print("Typing short date/initials: ");
    Serial.println(buffer);
    Serial.println("Then sending Left Arrow x3.");
  }

  if (typeAsciiString(buffer)) {
    sendLeftArrows(3);
  }
}

void outputAppointmentConfirmationMacro(const char* appointmentWindowText) {
  RtcDateTime now = rtcGetDateTime();

  if (!validateDateTime(now)) {
    if (serialDebugConnected()) {
      Serial.println("ERROR: RTC date/time read is invalid. HID output skipped.");
    }
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

  if (serialDebugConnected()) {
    Serial.print("Typing appointment confirmation macro: ");
    Serial.println(buffer);
  }

  typeAsciiString(buffer);
}

void outputDateFormatForKey(uint8_t keycode) {
  RtcDateTime now = rtcGetDateTime();

  if (!validateDateTime(now)) {
    if (serialDebugConnected()) {
      Serial.println("ERROR: RTC date/time read is invalid. HID output skipped.");
    }
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
      if (serialDebugConnected()) {
        Serial.println("ERROR: No date format assigned to this key.");
      }
      return;
  }

  if (serialDebugConnected()) {
    Serial.print("Typing date format: ");
    Serial.println(buffer);
  }

  typeAsciiString(buffer);
}

// ============================================================
// PIO USB host setup
// ============================================================

void configureUsbHost() {
  delay(1000); // Wait 1 second for power and keyboard boot stabilization
  pio_usb_configuration_t pioConfig = PIO_USB_DEFAULT_CONFIG;
  pioConfig.pin_dp = USB_HOST_DP_GPIO;

  // Match the known-working v0.2.68 receiver initialization exactly.
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
  USBHost.configure_pio_usb(1, &pioConfig);
  USBHost.begin(1);
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

bool hasShortcutModifier(uint8_t modifier) {
  static constexpr uint8_t shortcutModifiers =
    KEYBOARD_MODIFIER_LEFTCTRL |
    KEYBOARD_MODIFIER_RIGHTCTRL |
    KEYBOARD_MODIFIER_LEFTALT |
    KEYBOARD_MODIFIER_RIGHTALT |
    KEYBOARD_MODIFIER_LEFTGUI |
    KEYBOARD_MODIFIER_RIGHTGUI;

  return (modifier & shortcutModifiers) != 0;
}

bool decodeKeyboardReport(
  uint8_t const* report,
  uint16_t len,
  uint8_t expectedReportId,
  hid_keyboard_report_t& out
) {
  if (report == nullptr) {
    return false;
  }

  uint8_t const* payload = report;
  uint16_t payloadLen = len;

  if (expectedReportId != 0) {
    if (len < 1 || report[0] != expectedReportId) {
      return false;
    }

    payload = report + 1;
    payloadLen = len - 1;
  }

  if (payloadLen >= sizeof(hid_keyboard_report_t)) {
    memcpy(&out, payload, sizeof(out));
    return true;
  }

  return false;
}

bool usbHidReadyNow() {
  return usb_hid.ready();
}

bool sendKeyboardReportNow(hid_keyboard_report_t const* report) {
  if (report == nullptr) {
    telemetry.keyboardSendFailCount++;
    return false;
  }
  if (!usbHidReadyNow()) {
    telemetry.hidNotReadyCount++;
    telemetry.keyboardSendFailCount++;
    return false;
  }

  uint8_t keycodes[6];
  memcpy(keycodes, report->keycode, sizeof(keycodes));
  if (!usb_hid.keyboardReport(RID_KEYBOARD, report->modifier, keycodes)) {
    telemetry.keyboardSendFailCount++;
    return false;
  }

  telemetry.forwardedKeyboardCount++;
  telemetry.lastForwardedKeyboardMs = millis();
  return true;
}

bool sendConsumerUsageNow(uint16_t usage) {
  if (!usbHidReadyNow()) {
    telemetry.hidNotReadyCount++;
    telemetry.consumerSendFailCount++;
    return false;
  }

  if (serialDebugConnected()) {
    Serial.print("Device sending consumer usage: 0x");
    Serial.print(usage, HEX);
  }

  if (!usb_hid.sendReport16(RID_CONSUMER_CONTROL, usage)) {
    telemetry.consumerSendFailCount++;
    if (serialDebugConnected()) {
      Serial.println(" -> FAILED");
    }
    return false;
  }

  if (serialDebugConnected()) {
    Serial.println(" -> SUCCESS");
  }

  telemetry.forwardedConsumerCount++;
  telemetry.lastForwardedConsumerMs = millis();
  telemetry.lastConsumerUsage = usage;
  return true;
}

void servicePendingHidReports() {
  if (TinyUSBDevice.suspended()) {
    if (pendingKeyboardReportValid || consumerQueueTail != consumerQueueHead) {
      uint32_t now = millis();
      if (now - lastWakeupAttemptMs >= 2000) {
        lastWakeupAttemptMs = now;
        if (serialDebugConnected()) {
          Serial.println("Triggering USB remote wakeup...");
        }
        TinyUSBDevice.remoteWakeup();
      }
    }
    return;
  }

  if (!usbHidReadyNow()) {
    return;
  }

  if (consumerQueueTail != consumerQueueHead) {
    __asm__ volatile("dmb" : : : "memory");
    if (!sendConsumerUsageNow(consumerUsageQueue[consumerQueueTail])) {
      return;
    }
    consumerQueueTail = (consumerQueueTail + 1) % CONSUMER_QUEUE_SIZE;
  }

  if (pendingKeyboardReportValid) {
    if (!sendKeyboardReportNow(&pendingKeyboardReport)) {
      return;
    }
    pendingKeyboardReportValid = false;
  }
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
  hid_keyboard_report_t keyboardOut = {};
  if (!translateTopRowConsumerKeys(report, keyboardOut)) {
    return false;
  }

  pendingKeyboardReport = keyboardOut;
  pendingKeyboardReportValid = true;
  return true;
}

uint16_t topRowConsumerUsage(uint8_t keycode) {
  switch (keycode) {
    case HID_KEY_F7: return HID_USAGE_CONSUMER_SCAN_PREVIOUS_TRACK;
    case HID_KEY_F8: return HID_USAGE_CONSUMER_PLAY_PAUSE;
    case HID_KEY_F9: return HID_USAGE_CONSUMER_SCAN_NEXT_TRACK;
    case HID_KEY_F10: return HID_USAGE_CONSUMER_MUTE;
    case HID_KEY_F11: return HID_USAGE_CONSUMER_VOLUME_DECREMENT;
    case HID_KEY_F12: return HID_USAGE_CONSUMER_VOLUME_INCREMENT;
    default: return 0;
  }
}

bool translateTopRowConsumerKeys(hid_keyboard_report_t const* report, hid_keyboard_report_t& keyboardOut) {
  if (report == nullptr) {
    return false;
  }

  keyboardOut = *report;

  if (report->modifier != 0) {
    if (activeTopRowConsumerUsage != 0) {
      if (!sendConsumerUsage(0)) return false;
      activeTopRowConsumerUsage = 0;
    }
    return true;
  }

  uint16_t consumerUsage = 0;
  for (uint8_t index = 0; index < 6; index++) {
    uint16_t mappedUsage = topRowConsumerUsage(report->keycode[index]);
    if (mappedUsage != 0 && consumerUsage == 0) {
      consumerUsage = mappedUsage;
    }
    if (mappedUsage != 0) {
      keyboardOut.keycode[index] = 0;
    }
  }

  if (consumerUsage != activeTopRowConsumerUsage) {
    if (!sendConsumerUsage(consumerUsage)) return false;
    activeTopRowConsumerUsage = consumerUsage;
  }

  return true;
}

HostHidInterfaceInfo* hostHidInterfaceInfo(uint8_t dev_addr, uint8_t instance) {
  for (size_t index = 0; index < MAX_HOST_HID_INTERFACES; index++) {
    HostHidInterfaceInfo& info = hostHidInterfaces[index];
    if (info.mounted && info.devAddr == dev_addr && info.instance == instance) {
      return &info;
    }
  }

  return nullptr;
}

void clearHostHidInterface(uint8_t dev_addr, uint8_t instance) {
  for (size_t index = 0; index < MAX_HOST_HID_INTERFACES; index++) {
    HostHidInterfaceInfo& info = hostHidInterfaces[index];
    if (info.mounted && info.devAddr == dev_addr && info.instance == instance) {
      info = HostHidInterfaceInfo();
    }
  }
}

void parseHostHidDescriptor(
  uint8_t const* desc_report,
  uint16_t desc_len,
  bool& keyboard,
  uint8_t& keyboardReportId,
  bool& consumerControl,
  uint8_t& consumerReportId
) {
  keyboard = false;
  keyboardReportId = 0;
  consumerControl = false;
  consumerReportId = 0;

  if (desc_report == nullptr || desc_len == 0) {
    return;
  }

  tuh_hid_report_info_t reports[8];
  uint8_t reportCount = tuh_hid_parse_report_descriptor(reports, 8, desc_report, desc_len);
  for (uint8_t index = 0; index < reportCount; index++) {
    if (reports[index].usage_page == HID_USAGE_PAGE_DESKTOP &&
        reports[index].usage == HID_USAGE_DESKTOP_KEYBOARD) {
      keyboard = true;
      keyboardReportId = reports[index].report_id;
    } else if (reports[index].usage_page == HID_USAGE_PAGE_CONSUMER &&
        reports[index].usage == HID_USAGE_CONSUMER_CONTROL) {
      consumerControl = true;
      consumerReportId = reports[index].report_id;
    }
  }
}

void rememberHostHidInterface(
  uint8_t dev_addr,
  uint8_t instance,
  bool keyboard,
  uint8_t keyboardReportId,
  bool consumerControl,
  uint8_t consumerReportId
) {
  clearHostHidInterface(dev_addr, instance);

  for (size_t index = 0; index < MAX_HOST_HID_INTERFACES; index++) {
    HostHidInterfaceInfo& info = hostHidInterfaces[index];
    if (!info.mounted) {
      info = HostHidInterfaceInfo();
      info.devAddr = dev_addr;
      info.instance = instance;
      info.keyboardReportId = keyboardReportId;
      info.consumerReportId = consumerReportId;
      info.lastReportMs = millis();
      info.lastRecoveryMs = millis();
      info.keyboard = keyboard;
      info.consumerControl = consumerControl;
      info.mounted = true;
      return;
    }
  }

  // No Serial logging on Core 1
}

void parseConsumerBitmaskDescriptor(uint8_t const* desc_report, uint16_t desc_len, HostHidInterfaceInfo& info) {
  if (desc_report == nullptr || desc_len == 0 || !info.consumerControl) {
    return;
  }

  uint16_t usagePage = 0;
  uint8_t reportId = 0;
  uint32_t reportSize = 0;
  uint32_t reportCount = 0;
  uint16_t usageMin = 0;
  uint16_t usageMax = 0;
  uint16_t localUsages[MAX_CONSUMER_BIT_USAGES];
  uint8_t localUsageCount = 0;
  uint8_t trackedReportIds[8] = { 0 };
  uint16_t trackedBitOffsets[8] = { 0 };
  uint8_t trackedReportCount = 1;
  trackedReportIds[0] = 0;

  auto itemValue = [](uint8_t const* data, uint8_t size) -> uint32_t {
    uint32_t value = 0;
    for (uint8_t index = 0; index < size; index++) {
      value |= ((uint32_t)data[index]) << (8 * index);
    }
    return value;
  };

  auto currentBitOffset = [&]() -> uint16_t& {
    for (uint8_t index = 0; index < trackedReportCount; index++) {
      if (trackedReportIds[index] == reportId) {
        return trackedBitOffsets[index];
      }
    }

    if (trackedReportCount < 8) {
      trackedReportIds[trackedReportCount] = reportId;
      trackedBitOffsets[trackedReportCount] = 0;
      trackedReportCount++;
      return trackedBitOffsets[trackedReportCount - 1];
    }

    return trackedBitOffsets[0];
  };

  for (uint16_t index = 0; index < desc_len;) {
    uint8_t prefix = desc_report[index++];
    if (prefix == 0xFE) {
      if (index + 1 >= desc_len) break;
      uint8_t longSize = desc_report[index++];
      index++;
      index += longSize;
      continue;
    }

    uint8_t sizeCode = prefix & 0x03;
    uint8_t size = sizeCode == 3 ? 4 : sizeCode;
    uint8_t type = (prefix >> 2) & 0x03;
    uint8_t tag = (prefix >> 4) & 0x0F;
    if (index + size > desc_len) break;
    uint32_t value = itemValue(desc_report + index, size);
    index += size;

    if (type == 0) {
      if (tag == 8) {
        uint8_t inputFlags = value & 0xFF;
        uint16_t& bitOffset = currentBitOffset();
        bool dataField = (inputFlags & HID_CONSTANT) == 0;
        bool variableField = (inputFlags & HID_VARIABLE) != 0;

        if (dataField && variableField &&
            usagePage == HID_USAGE_PAGE_CONSUMER &&
            reportId == info.consumerReportId &&
            reportSize == 1 &&
            reportCount > 0) {
          info.consumerBitmask = true;
          info.consumerBitOffset = bitOffset;
          info.consumerBitCount = (uint8_t)min((uint32_t)MAX_CONSUMER_BIT_USAGES, reportCount);

          for (uint8_t bit = 0; bit < info.consumerBitCount; bit++) {
            if (bit < localUsageCount) {
              info.consumerBitUsages[bit] = localUsages[bit];
            } else if (usageMax >= usageMin) {
              info.consumerBitUsages[bit] = usageMin + bit;
            } else {
              info.consumerBitUsages[bit] = 0;
            }
          }
        }

        bitOffset += reportSize * reportCount;
        localUsageCount = 0;
        usageMin = 0;
        usageMax = 0;
      }
    } else if (type == 1) {
      switch (tag) {
        case 0: usagePage = value & 0xFFFF; break;
        case 7: reportSize = value; break;
        case 8: reportId = value & 0xFF; break;
        case 9: reportCount = value; break;
        default: break;
      }
    } else if (type == 2) {
      switch (tag) {
        case 0:
          if (localUsageCount < MAX_CONSUMER_BIT_USAGES) {
            localUsages[localUsageCount++] = value & 0xFFFF;
          }
          break;
        case 1:
          usageMin = value & 0xFFFF;
          break;
        case 2:
          usageMax = value & 0xFFFF;
          break;
        default:
          break;
      }
    }
  }
}

bool sendConsumerUsage(uint16_t usage) {
  size_t nextHead = (consumerQueueHead + 1) % CONSUMER_QUEUE_SIZE;
  if (nextHead == consumerQueueTail) {
    return false;
  }
  consumerUsageQueue[consumerQueueHead] = usage;
  __asm__ volatile("dmb" : : : "memory");
  consumerQueueHead = nextHead;
  return true;
}

bool forwardConsumerControlReport(uint8_t const* report, uint16_t len, const HostHidInterfaceInfo& info) {
  if (report == nullptr) {
    return false;
  }

  uint8_t const* payload = report;
  uint16_t payloadLen = len;

  if (info.consumerReportId != 0) {
    if (len < 1 || report[0] != info.consumerReportId) {
      return false;
    }

    payload = report + 1;
    payloadLen = len - 1;
  }

  if (info.consumerBitmask) {
    uint16_t usage = 0;
    for (uint8_t bit = 0; bit < info.consumerBitCount; bit++) {
      uint16_t reportBit = info.consumerBitOffset + bit;
      uint16_t byteIndex = reportBit / 8;
      uint8_t bitMask = 1 << (reportBit % 8);
      if (byteIndex < payloadLen && (payload[byteIndex] & bitMask) != 0) {
        usage = info.consumerBitUsages[bit];
        break;
      }
    }

    return sendConsumerUsage(usage);
  }

  if (payloadLen < 1) {
    return false;
  }

  uint16_t usage = 0;
  if (payloadLen >= 2) {
    usage = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
  } else {
    usage = payload[0];
  }

  return sendConsumerUsage(usage);
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
      if (serialDebugConnected()) {
        Serial.print("Matched physical rule ");
        Serial.print(rule.id);
        Serial.print(": ");
        Serial.println(rule.label);
      }
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

bool outputExpansionListTable() {
  const uint16_t keyDelayMs = 5;

  // The committing key is intercepted, so erase the three trigger characters
  // that have already reached the computer.
  for (uint8_t count = 0; count < 3; count++) {
    if (!sendHidKeyWithDelay(0, HID_KEY_BACKSPACE, keyDelayMs)) return false;
  }

  size_t longestTriggerLength = 0;
  for (size_t index = 0; index < configRuleCount; index++) {
    const ConfigRule& rule = configRules[index];
    if (!rule.enabled || rule.trigger.equalsIgnoreCase("key")) {
      continue;
    }
    if (rule.triggerPattern.length() > longestTriggerLength) {
      longestTriggerLength = rule.triggerPattern.length();
    }
  }

  if (!typeAsciiStringWithDelay("QuickType expansions\n", keyDelayMs)) return false;

  size_t listedCount = 0;
  for (size_t index = 0; index < configRuleCount; index++) {
    const ConfigRule& rule = configRules[index];
    if (!rule.enabled || rule.trigger.equalsIgnoreCase("key")) {
      continue;
    }

    if (!typeAsciiStringWithDelay(rule.triggerPattern, keyDelayMs)) return false;
    for (size_t spaceCount = rule.triggerPattern.length(); spaceCount < longestTriggerLength; spaceCount++) {
      if (!typeAsciiCharWithDelay(' ', keyDelayMs)) return false;
    }
    if (!typeAsciiStringWithDelay(" - ", keyDelayMs)) return false;
    if (!typeAsciiStringWithDelay(rule.label, keyDelayMs)) return false;
    if (!typeAsciiCharWithDelay('\n', keyDelayMs)) return false;
    listedCount++;
  }

  if (listedCount == 0) {
    if (!typeAsciiStringWithDelay("(No active typed expansions saved.)\n", keyDelayMs)) return false;
  }

  if (!typeAsciiStringWithDelay("\nKeypad actions\n", keyDelayMs)) return false;

  size_t keypadListedCount = 0;
  for (size_t index = 0; index < configRuleCount; index++) {
    const ConfigRule& rule = configRules[index];
    if (!rule.enabled || !rule.trigger.equalsIgnoreCase("key")) {
      continue;
    }

    const char* keyLabel = rule.triggerPattern.c_str();
    if (rule.triggerPattern.equalsIgnoreCase("KP_NUMLOCK")) keyLabel = "Num Lock";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_SLASH")) keyLabel = "/";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_ASTERISK")) keyLabel = "*";
    else if (rule.triggerPattern.equalsIgnoreCase("BACKSPACE")) keyLabel = "Backspace";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_MINUS")) keyLabel = "-";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_PLUS")) keyLabel = "+";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_ENTER")) keyLabel = "Enter";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_0")) keyLabel = "0";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_00")) keyLabel = "00";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_1")) keyLabel = "1";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_2")) keyLabel = "2";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_3")) keyLabel = "3";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_4")) keyLabel = "4";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_5")) keyLabel = "5";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_6")) keyLabel = "6";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_7")) keyLabel = "7";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_8")) keyLabel = "8";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_9")) keyLabel = "9";
    else if (rule.triggerPattern.equalsIgnoreCase("KP_PERIOD")) keyLabel = ".";

    if (!typeAsciiStringWithDelay(keyLabel, keyDelayMs)) return false;
    if (!typeAsciiStringWithDelay(" - ", keyDelayMs)) return false;
    if (!typeAsciiStringWithDelay(rule.label, keyDelayMs)) return false;
    if (!typeAsciiCharWithDelay('\n', keyDelayMs)) return false;
    keypadListedCount++;
  }

  if (keypadListedCount == 0) {
    if (!typeAsciiStringWithDelay("(No active keypad actions saved.)\n", keyDelayMs)) return false;
  }

  return true;
}

const char* getManufacturerName(uint16_t vid) {
  switch (vid) {
    case 0x046D: return "Logitech";
    case 0x05AC: return "Apple";
    case 0x045E: return "Microsoft";
    case 0x1532: return "Razer";
    case 0x1B1C: return "Corsair";
    case 0x1038: return "SteelSeries";
    case 0x3434: return "Keychron";
    case 0x288A: return "Seeed Studio";
    case 0x2E8A: return "Raspberry Pi / Pico";
    default: return "Unknown Device";
  }
}

bool outputDiagnosticInformation() {
  const uint16_t keyDelayMs = 5;

  // The committing key is intercepted, so erase the three trigger characters
  // that have already reached the computer.
  for (uint8_t count = 0; count < 3; count++) {
    if (!sendHidKeyWithDelay(0, HID_KEY_BACKSPACE, keyDelayMs)) return false;
  }

  if (!typeAsciiStringWithDelay("\n--- QUICKTYPE DIAGNOSTIC REPORT ---\n", keyDelayMs)) return false;
  
  if (!typeAsciiStringWithDelay("Firmware Version: ", keyDelayMs)) return false;
  if (!typeAsciiStringWithDelay(FIRMWARE_VERSION, keyDelayMs)) return false;
  if (!typeAsciiStringWithDelay("\n", keyDelayMs)) return false;

  if (!typeAsciiStringWithDelay("RTC Present: ", keyDelayMs)) return false;
  if (!typeAsciiStringWithDelay(rtcPresent() ? "YES" : "NO", keyDelayMs)) return false;
  if (!typeAsciiStringWithDelay("\n", keyDelayMs)) return false;

  if (rtcPresent()) {
    if (!typeAsciiStringWithDelay("RTC Oscillator Stop Flag: ", keyDelayMs)) return false;
    if (!typeAsciiStringWithDelay(rtcOscillatorStopFlagSet() ? "SET (Time invalid/lost power)" : "CLEAR (Valid)", keyDelayMs)) return false;
    if (!typeAsciiStringWithDelay("\n", keyDelayMs)) return false;

    RtcDateTime dt = rtcGetDateTime();
    const char* dowNames[] = {"", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    const char* dowStr = (dt.dow >= 1 && dt.dow <= 7) ? dowNames[dt.dow] : "Unknown";

    char buf[128];
    snprintf(buf, sizeof(buf), "RTC Date/Time: %04d-%02d-%02d %02d:%02d:%02d (%s)\n",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, dowStr);
    if (!typeAsciiStringWithDelay(buf, keyDelayMs)) return false;
  }

  if (!typeAsciiStringWithDelay("USB Host Status:\n", keyDelayMs)) return false;
  size_t mountedCount = 0;
  for (size_t index = 0; index < MAX_HOST_HID_INTERFACES; index++) {
    HostHidInterfaceInfo const& info = hostHidInterfaces[index];
    if (info.mounted) {
      uint16_t vid = 0, pid = 0;
      tuh_vid_pid_get(info.devAddr, &vid, &pid);
      
      char bufDev[256];
      const char* mfg = getManufacturerName(vid);
      
      if (info.keyboard) {
        snprintf(bufDev, sizeof(bufDev), "* Keyboard Interface (Device Addr %u, Instance %u, %s [VID: 0x%04X, PID: 0x%04X])\n", 
                 info.devAddr, info.instance, mfg, vid, pid);
      } else if (info.consumerControl) {
        snprintf(bufDev, sizeof(bufDev), "* Media Keys Interface (Device Addr %u, Instance %u, Report ID %u, %s [VID: 0x%04X, PID: 0x%04X])\n", 
                 info.devAddr, info.instance, info.consumerReportId, mfg, vid, pid);
      } else {
        snprintf(bufDev, sizeof(bufDev), "* Generic HID Interface (Device Addr %u, Instance %u, %s [VID: 0x%04X, PID: 0x%04X])\n", 
                 info.devAddr, info.instance, mfg, vid, pid);
      }
      if (!typeAsciiStringWithDelay(bufDev, keyDelayMs)) return false;
      mountedCount++;
    }
  }
  if (mountedCount == 0) {
    if (!typeAsciiStringWithDelay("* No USB devices mounted.\n", keyDelayMs)) return false;
  }

  if (!typeAsciiStringWithDelay("System Performance:\n", keyDelayMs)) return false;
  char bufTel[256];
  snprintf(bufTel, sizeof(bufTel), 
           "* Active Rules: %u of %u\n"
           "* Reports Received from Keyboard: %u\n"
           "* Standard Keys Forwarded: %u\n"
           "* Media Keys Forwarded: %u\n"
           "* Key Parsing Errors: %u\n"
           "* USB Transmission Failures: %u\n",
           (unsigned int)activeRuleCount(), (unsigned int)MAX_CONFIG_RULES,
           (unsigned int)telemetry.hostReportCallbackCount,
           (unsigned int)telemetry.forwardedKeyboardCount,
           (unsigned int)telemetry.forwardedConsumerCount,
           (unsigned int)telemetry.keyboardDecodeFailCount,
           (unsigned int)(telemetry.keyboardSendFailCount + telemetry.consumerSendFailCount));
  if (!typeAsciiStringWithDelay(bufTel, keyDelayMs)) return false;

  if (!typeAsciiStringWithDelay("------------------------------------\n", keyDelayMs)) return false;

  return true;
}

bool processTypedTriggerRules(char currentCharacter, bool delimiterRecorded) {
  const bool restoreSpace = delimiterRecorded && currentCharacter == ' ';
  const bool consumeDelimiter = !delimiterRecorded &&
    (currentCharacter == '\t' || currentCharacter == '\n' || currentCharacter == '\x1b');
  if (!restoreSpace && !consumeDelimiter) {
    return false;
  }

  const String expansionListSuffix = restoreSpace ? ";;; " : ";;;";
  if (typedBuffer.endsWith(expansionListSuffix)) {
    if (serialDebugConnected()) {
      Serial.println("Matched hidden expansion list trigger.");
    }
    typedBuffer = "";
    typedSources = "";
    return outputExpansionListTable();
  }

  const String diagnosticsSuffix = restoreSpace ? ";;! " : ";;!";
  if (typedBuffer.endsWith(diagnosticsSuffix)) {
    if (serialDebugConnected()) {
      Serial.println("Matched hidden diagnostics trigger.");
    }
    typedBuffer = "";
    typedSources = "";
    return outputDiagnosticInformation();
  }

  for (size_t index = 0; index < configRuleCount; index++) {
    const ConfigRule& rule = configRules[index];
    if (!rule.enabled || rule.trigger.equalsIgnoreCase("key") || rule.triggerPattern.length() == 0) {
      continue;
    }

    size_t patternLength = rule.triggerPattern.length();
    size_t suffixLength = patternLength + (restoreSpace ? 1 : 0);

    if (typedBuffer.length() < suffixLength) {
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

    if (serialDebugConnected()) {
      Serial.print("Matched typed rule ");
      Serial.print(rule.id);
      Serial.print(": ");
      Serial.println(rule.label);
    }
    memset(&pendingKeyboardReport, 0, sizeof(pendingKeyboardReport));
    pendingKeyboardReportValid = false;
    size_t eraseCount = patternLength;
    bool result = executeConfiguredRule(rule, eraseCount, restoreSpace ? ' ' : 0);
    typedBuffer = "";
    typedSources = "";
    return result;
  }

  return false;
}

void handleKeyboardReport(hid_keyboard_report_t const* report) {
  bool interceptedPhysicalKey = false;
  bool shortcutModifierActive = hasShortcutModifier(report->modifier);
  bool anyModifierActive = report->modifier != 0;
  bool expansionTriggered = false;

  for (uint8_t i = 0; i < 6; i++) {
    uint8_t keycode = report->keycode[i];

    if (keycode == 0) {
      continue;
    }

    if (keyWasInPreviousReport(keycode)) {
      continue;
    }

    telemetry.lastKeyUsage = keycode;
    telemetry.lastModifier = report->modifier;
    telemetry.keyPressCount++;

    if (serialDebugConnected()) {
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
    }

    if (anyModifierActive) {
      continue;
    }

    if (storedConfigurationLoaded && expansionsEnabled && keypadExpansionsEnabled) {
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
    usb_hid.keyboardRelease(RID_KEYBOARD);
    previousKeyboardReport = *report;
    return;
  }

  forwardKeyboardReport(report);

  if (shortcutModifierActive) {
    typedBuffer = "";
    typedSources = "";
    previousKeyboardReport = *report;
    return;
  }

  if (storedConfigurationLoaded && expansionsEnabled) {
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

      // Tab, Enter, and Escape commit an enabled typed trigger, but are not
      // included in its expansion output. If no rule matches, they continue
      // through their existing handling below and are forwarded normally.
      if (keycode == HID_KEY_TAB || keycode == HID_KEY_ENTER || keycode == HID_KEY_ESCAPE) {
        char delimiter = keycode == HID_KEY_TAB ? '\t' : (keycode == HID_KEY_ENTER ? '\n' : '\x1b');
        if (processTypedTriggerRules(delimiter, false)) {
          expansionTriggered = true;
          break;
        }
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
        if (processTypedTriggerRules('0')) {
          expansionTriggered = true;
          break;
        }
        continue;
      }

      bool isNumpad = false;
      char typedCharacter = hidKeycodeToAscii(keycode, report->modifier, isNumpad);
      if (typedCharacter != 0) {
        recordTypedCharacter(typedCharacter, isNumpad);
        if (processTypedTriggerRules(typedCharacter)) {
          expansionTriggered = true;
          break;
        }
      }
    }
  }

  if (expansionTriggered) {
    memset(&pendingKeyboardReport, 0, sizeof(pendingKeyboardReport));
    pendingKeyboardReportValid = false;
  }

  previousKeyboardReport = *report;
}

void requestNextHidReport(uint8_t dev_addr, uint8_t instance) {
  HostHidInterfaceInfo* info = hostHidInterfaceInfo(dev_addr, instance);
  uint32_t now = millis();
  if (info != nullptr && now < info->nextReportRequestMs) {
    return;
  }
  if (info != nullptr && info->rearmAtMs != 0 && (int32_t)(now - info->rearmAtMs) < 0) {
    return;
  }

  if (!tuh_hid_receive_ready(dev_addr, instance)) {
    // An interrupt-IN transfer remaining pending while a keyboard is idle is
    // normal USB behavior. Aborting it on a timer can strand wireless receivers.
    return;
  }

  if (!tuh_hid_receive_report(dev_addr, instance)) {
    telemetry.hostReportRequestFailCount++;
    if (info != nullptr) {
      info->nextReportRequestMs = now + 25;
    }
  } else {
    telemetry.hostReportRequestCount++;
    if (info != nullptr) {
      info->nextReportRequestMs = 0;
      info->rearmAtMs = 0;
    }
  }
}

void pollMountedHidReports() {
  for (size_t index = 0; index < MAX_HOST_HID_INTERFACES; index++) {
    HostHidInterfaceInfo& info = hostHidInterfaces[index];
    if (!info.mounted) {
      continue;
    }

    requestNextHidReport(info.devAddr, info.instance);
  }
}

void serviceUsbBridge() {
  telemetry.bridgeServiceCount++;
  serviceNativeUsb();
  // USB Host tasks run continuously on Core 1, including while Core 0 emits
  // an expansion to the computer.
  serviceNativeUsb();

  if (deviceConnectionResetPending) {
    deviceConnectionResetPending = false;
    if (serialDebugConnected()) {
      Serial.println("USB Device connection state changed. Resetting bridge state.");
    }
    // Safely clear reports, queues, buffers and LEDs on Core 0 thread
    memset(&pendingKeyboardReport, 0, sizeof(pendingKeyboardReport));
    if (TinyUSBDevice.mounted()) {
      pendingKeyboardReportValid = true; // Send release report
    } else {
      pendingKeyboardReportValid = false;
    }
    memset(&previousKeyboardReport, 0, sizeof(previousKeyboardReport));
    activeTopRowConsumerUsage = 0;
    consumerQueueTail = 0;
    consumerQueueHead = 0;
    keyboardQueueTail = 0;
    keyboardQueueHead = 0;
    typedBuffer = "";
    typedSources = "";
    hostLedsState = 0xFF;
  }

  // If the PC has suspended the USB connection, do NOT write to endpoints or process keyboard reports!
  // Instead, queue the reports and trigger a spec-compliant remote wakeup if enabled by the host.
  if (TinyUSBDevice.suspended()) {
    if (hostRemoteWakeupEnabled && (keyboardQueueTail != keyboardQueueHead || consumerQueueTail != consumerQueueHead)) {
      uint32_t now = millis();
      if (now - lastWakeupAttemptMs >= 2000) {
        lastWakeupAttemptMs = now;
        if (serialDebugConnected()) {
          Serial.println("USB Device suspended. Triggering remote wakeup...");
        }
        TinyUSBDevice.remoteWakeup();
      }
    }
    return;
  }

  // Preserve key-down/key-up ordering. Do not consume another host report
  // until the prior transformed report has been accepted by the laptop-facing
  // HID endpoint; otherwise a fast release overwrites its pending key press.
  if (!pendingKeyboardReportValid && keyboardQueueTail != keyboardQueueHead) {
    __asm__ volatile("dmb" : : : "memory");
    hid_keyboard_report_t report = keyboardReportQueue[keyboardQueueTail];
    keyboardQueueTail = (keyboardQueueTail + 1) % KEYBOARD_REPORT_QUEUE_SIZE;
    telemetry.keyboardReportCount++;
    telemetry.lastKeyboardReportMs = millis();
    handleKeyboardReport(&report);
  }

  servicePendingHidReports();
}

void resetBridgeStateAfterConfigurationChange() {
  typedBuffer = "";
  typedSources = "";
  memset(&previousKeyboardReport, 0, sizeof(previousKeyboardReport));
  memset(&pendingKeyboardReport, 0, sizeof(pendingKeyboardReport));
  pendingKeyboardReportValid = false;
  consumerQueueHead = 0;
  consumerQueueTail = 0;

  if (usbHidReadyNow()) {
    usb_hid.keyboardRelease(RID_KEYBOARD);
    usb_hid.sendReport16(RID_CONSUMER_CONTROL, 0);
  }

  activeTopRowConsumerUsage = 0;
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
  telemetry.hostMountCount++;
  telemetry.lastHostMountMs = millis();
  uint8_t protocol = tuh_hid_interface_protocol(dev_addr, instance);
  bool keyboard = protocol == HID_ITF_PROTOCOL_KEYBOARD;
  uint8_t keyboardReportId = 0;
  uint8_t consumerReportId = 0;
  bool consumerControl = false;

  parseHostHidDescriptor(
    desc_report,
    desc_len,
    keyboard,
    keyboardReportId,
    consumerControl,
    consumerReportId
  );

  if (protocol == HID_ITF_PROTOCOL_KEYBOARD) {
    keyboard = true;
    // Boot keyboard reports use the standard 8-byte modifier/key array and do
    // not include a report ID, even when report protocol advertises one.
    keyboardReportId = 0;
  }

  rememberHostHidInterface(
    dev_addr,
    instance,
    keyboard,
    keyboardReportId,
    consumerControl,
    consumerReportId
  );
  HostHidInterfaceInfo* mountedInfo = hostHidInterfaceInfo(dev_addr, instance);
  if (mountedInfo != nullptr) {
    parseConsumerBitmaskDescriptor(desc_report, desc_len, *mountedInfo);
  }

  if (protocol == HID_ITF_PROTOCOL_KEYBOARD &&
      tuh_hid_get_protocol(dev_addr, instance) != HID_PROTOCOL_BOOT) {
    memset(&previousKeyboardReport, 0, sizeof(previousKeyboardReport));
    if (tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT)) {
      return;
    }
  }

  requestNextHidReport(dev_addr, instance);
}

extern "C" void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t instance, uint8_t protocol) {
  requestNextHidReport(dev_addr, instance);
}

extern "C" void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  telemetry.hostUnmountCount++;
  telemetry.lastHostUnmountMs = millis();
  if (activeTopRowConsumerUsage != 0) {
    sendConsumerUsage(0);
    activeTopRowConsumerUsage = 0;
  }
  memset(&previousKeyboardReport, 0, sizeof(previousKeyboardReport));
  clearHostHidInterface(dev_addr, instance);
}

// TinyUSB Device stack callbacks to handle KVM switching / PC host disconnects
extern "C" void tud_mount_cb(void) {
  deviceConnectionResetPending = true;
}

extern "C" void tud_umount_cb(void) {
  deviceConnectionResetPending = true;
}

extern "C" void tud_suspend_cb(bool remote_wakeup_en) {
  hostRemoteWakeupEnabled = remote_wakeup_en;
  deviceConnectionResetPending = true;
}

extern "C" void tud_resume_cb(void) {
  hostRemoteWakeupEnabled = false;
  deviceConnectionResetPending = true;
}

extern "C" void tuh_hid_report_received_cb(
  uint8_t dev_addr,
  uint8_t instance,
  uint8_t const* report,
  uint16_t len
) {
  uint8_t protocol = tuh_hid_interface_protocol(dev_addr, instance);
  HostHidInterfaceInfo* info = hostHidInterfaceInfo(dev_addr, instance);
  telemetry.hostReportCallbackCount++;
  telemetry.lastHostReportMs = millis();
  if (info != nullptr) {
    info->lastReportMs = millis();
    info->rearmAtMs = 0;
  }

  if (len == 0) {
    telemetry.hostZeroLengthReportCount++;
    if (info != nullptr) {
      info->nextReportRequestMs = millis() + 25;
    }
    requestNextHidReport(dev_addr, instance);
    return;
  }

  // A Logitech receiver can advertise keyboard and consumer-control reports on
  // the same boot-capable interface. Once that interface is in boot protocol,
  // every 8-byte report is a standard keyboard report and must be decoded as
  // such before considering consumer-control descriptor metadata.
  if (protocol == HID_ITF_PROTOCOL_KEYBOARD) {
    hid_keyboard_report_t decodedReport = {};
    if (decodeKeyboardReport(report, len, 0, decodedReport)) {
      // Queue the report to be processed by Core 0
      size_t nextHead = (keyboardQueueHead + 1) % KEYBOARD_REPORT_QUEUE_SIZE;
      if (nextHead != keyboardQueueTail) {
        keyboardReportQueue[keyboardQueueHead] = decodedReport;
        __asm__ volatile("dmb" : : : "memory");
        keyboardQueueHead = nextHead;
      }
    } else {
      telemetry.keyboardDecodeFailCount++;
    }
  } else if (info != nullptr && info->consumerControl &&
             (info->consumerReportId == 0 || report[0] == info->consumerReportId)) {
    telemetry.consumerReportCount++;
    telemetry.lastConsumerReportMs = millis();
    forwardConsumerControlReport(report, len, *info);
  } else if (info != nullptr && info->keyboard) {
    hid_keyboard_report_t decodedReport = {};
    if (decodeKeyboardReport(report, len, info->keyboardReportId, decodedReport)) {
      size_t nextHead = (keyboardQueueHead + 1) % KEYBOARD_REPORT_QUEUE_SIZE;
      if (nextHead != keyboardQueueTail) {
        keyboardReportQueue[keyboardQueueHead] = decodedReport;
        __asm__ volatile("dmb" : : : "memory");
        keyboardQueueHead = nextHead;
      }
    } else {
      telemetry.keyboardDecodeFailCount++;
    }
  }

  requestNextHidReport(dev_addr, instance);
}

// ============================================================
// Configurable rules and browser serial protocol
// ============================================================

void clearCompiledConfiguration() {
  for (size_t index = 0; index < MAX_CONFIG_RULES; index++) {
    configRules[index] = ConfigRule();
  }
  configRuleCount = 0;
  for (size_t index = 0; index < MAX_CONFIG_PLACEHOLDERS; index++) {
    configPlaceholders[index] = ConfigPlaceholder();
  }
  configPlaceholderCount = 0;
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

  JsonObjectConst placeholders = config["placeholders"].as<JsonObjectConst>();
  if (!placeholders.isNull()) {
    for (JsonPairConst pair : placeholders) {
      if (configPlaceholderCount >= MAX_CONFIG_PLACEHOLDERS) {
        errorMessage = "Configuration contains too many placeholders.";
        clearCompiledConfiguration();
        return false;
      }

      String name = pair.key().c_str();
      String value = String(pair.value() | "");
      if (name.length() == 0 || name.length() > 24 || value.length() == 0 || value.length() > 80) {
        errorMessage = "A placeholder name or value is outside the supported length.";
        clearCompiledConfiguration();
        return false;
      }

      configPlaceholders[configPlaceholderCount].name = name;
      configPlaceholders[configPlaceholderCount].value = value;
      configPlaceholderCount++;
    }
  }

  for (JsonPairConst pair : rules) {
    serviceNativeUsb();

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
    long keyDelay = object["keyDelay"] | 5;
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
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.println("No stored QuickType configuration. Legacy keypad mappings are active.");
    }
    return false;
  }

  File file = LittleFS.open(CONFIG_FILE, "r");
  if (!file) {
    clearCompiledConfiguration();
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.println("ERROR: Could not open stored QuickType configuration.");
    }
    return false;
  }

  if (file.size() > MAX_CONFIG_BYTES) {
    file.close();
    clearCompiledConfiguration();
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.println("ERROR: Stored QuickType configuration is too large.");
    }
    return false;
  }

  JsonDocument document;
  DeserializationError jsonError = deserializeJson(document, file);
  file.close();

  if (jsonError) {
    clearCompiledConfiguration();
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.print("ERROR: Stored QuickType configuration is invalid: ");
      Serial.println(jsonError.c_str());
    }
    return false;
  }

  String validationError;
  if (!compileConfiguration(document.as<JsonVariantConst>(), validationError)) {
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.print("ERROR: Stored QuickType configuration was rejected: ");
      Serial.println(validationError);
    }
    return false;
  }

  if (ENABLE_SERIAL_DEBUG_LOGS) {
    Serial.print("Loaded QuickType configuration with ");
    Serial.print(activeRuleCount());
    Serial.print(" active rules (");
    Serial.print(configRuleCount);
    Serial.println(" compiled slots).");
  }
  return true;
}

void recoverConfigurationStorage() {
  if (!LittleFS.exists(CONFIG_FILE) && LittleFS.exists(CONFIG_BACKUP_FILE)) {
    LittleFS.rename(CONFIG_BACKUP_FILE, CONFIG_FILE);
  }

  if (LittleFS.exists(CONFIG_FILE)) {
    LittleFS.remove(CONFIG_BACKUP_FILE);
  }
  LittleFS.remove(CONFIG_TEMP_FILE);
}

bool verifyConfigurationFile(const char* filename, size_t expectedSize) {
  File file = LittleFS.open(filename, "r");
  if (!file || file.size() != expectedSize) {
    if (file) file.close();
    return false;
  }

  JsonDocument verification;
  DeserializationError jsonError = deserializeJson(verification, file);
  file.close();
  return !jsonError && verification.is<JsonObject>();
}

bool saveConfiguration(JsonVariantConst config, String& errorMessage) {
  serviceNativeUsb();

  size_t configSize = measureJson(config);
  if (configSize == 0 || configSize > MAX_CONFIG_BYTES) {
    errorMessage = "Configuration is empty or exceeds 32 KB.";
    return false;
  }

  if (!compileConfiguration(config, errorMessage)) {
    loadConfigurationFromStorage();
    return false;
  }

  // Suspend Core 1 and disable watchdog during blocking LittleFS compaction/writes
  suspendHostStack();
  disableWatchdog();

  LittleFS.remove(CONFIG_TEMP_FILE);
  serviceNativeUsb();

  File file = LittleFS.open(CONFIG_TEMP_FILE, "w");
  if (!file) {
    errorMessage = "Could not create the temporary configuration file.";
    enableWatchdog();
    resumeHostStack();
    loadConfigurationFromStorage();
    return false;
  }

  CooperativeFileWriter cooperativeFile(file);
  size_t written = serializeJson(config, cooperativeFile);
  serviceNativeUsb();
  file.close();
  serviceNativeUsb();

  if (written != configSize) {
    LittleFS.remove(CONFIG_TEMP_FILE);
    errorMessage = "Configuration write was incomplete.";
    enableWatchdog();
    resumeHostStack();
    loadConfigurationFromStorage();
    return false;
  }

  if (!verifyConfigurationFile(CONFIG_TEMP_FILE, configSize)) {
    LittleFS.remove(CONFIG_TEMP_FILE);
    errorMessage = "Configuration verification failed.";
    enableWatchdog();
    resumeHostStack();
    loadConfigurationFromStorage();
    return false;
  }

  serviceNativeUsb();
  LittleFS.remove(CONFIG_BACKUP_FILE);
  if (LittleFS.exists(CONFIG_FILE) && !LittleFS.rename(CONFIG_FILE, CONFIG_BACKUP_FILE)) {
    LittleFS.remove(CONFIG_TEMP_FILE);
    errorMessage = "Could not preserve the previous configuration.";
    enableWatchdog();
    resumeHostStack();
    loadConfigurationFromStorage();
    return false;
  }
  serviceNativeUsb();

  if (!LittleFS.rename(CONFIG_TEMP_FILE, CONFIG_FILE)) {
    if (LittleFS.exists(CONFIG_BACKUP_FILE)) {
      LittleFS.rename(CONFIG_BACKUP_FILE, CONFIG_FILE);
    }
    errorMessage = "Could not activate the new configuration file.";
    enableWatchdog();
    resumeHostStack();
    loadConfigurationFromStorage();
    return false;
  }

  // Re-enable watchdog and resume Core 1 after LittleFS compaction/writes complete
  enableWatchdog();
  resumeHostStack();

  LittleFS.remove(CONFIG_BACKUP_FILE);

  storedConfigurationLoaded = true;
  telemetry.configWriteCount++;
  telemetry.lastConfigWriteMs = millis();
  resetBridgeStateAfterConfigurationChange();
  serviceNativeUsb();
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
  bool invalidToken = false;
  int start = 0;

  while (start <= (int)input.length()) {
    int separator = input.indexOf('+', start);
    String token = separator >= 0 ? input.substring(start, separator) : input.substring(start);
    token.trim();

    if (token == "CTRL" || token == "CONTROL" || token == "LCTRL" || token == "LCONTROL") modifier |= KEYBOARD_MODIFIER_LEFTCTRL;
    else if (token == "RCTRL" || token == "RCONTROL") modifier |= KEYBOARD_MODIFIER_RIGHTCTRL;
    else if (token == "SHIFT" || token == "LSHIFT") modifier |= KEYBOARD_MODIFIER_LEFTSHIFT;
    else if (token == "RSHIFT") modifier |= KEYBOARD_MODIFIER_RIGHTSHIFT;
    else if (token == "ALT" || token == "LALT") modifier |= KEYBOARD_MODIFIER_LEFTALT;
    else if (token == "RALT" || token == "ALTGR") modifier |= KEYBOARD_MODIFIER_RIGHTALT;
    else if (token == "GUI" || token == "WIN" || token == "WINDOWS" || token == "CMD" || token == "COMMAND" || token == "META" || token == "LGUI" || token == "LWIN" || token == "LCMD" || token == "LMETA") modifier |= KEYBOARD_MODIFIER_LEFTGUI;
    else if (token == "RGUI" || token == "RWIN" || token == "RCMD" || token == "RMETA") modifier |= KEYBOARD_MODIFIER_RIGHTGUI;
    else {
      uint8_t parsedKeycode = shortcutKeycode(token);
      if (parsedKeycode == 0 || keycode != 0) invalidToken = true;
      else keycode = parsedKeycode;
    }

    if (separator < 0) break;
    start = separator + 1;
  }

  if (invalidToken || (keycode == 0 && modifier == 0)) {
    if (serialDebugConnected()) {
      Serial.print("ERROR: Unsupported shortcut: ");
      Serial.println(shortcut);
    }
    return false;
  }

  return sendHidKeyWithDelay(modifier, keycode, keyDelayMs);
}

String rtcTokenValue(const String& token) {
  bool isClockToken = token == "date" || token == "date_short" || token == "iso_date" ||
                      token == "time" || token == "time_12_compact" || token == "time_seconds" || token == "time_24" ||
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
  else if (token == "time_12_compact") snprintf(buffer, sizeof(buffer), "%d:%02d%s", hour12, now.minute, now.hour >= 12 ? "pm" : "am");
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

String customPlaceholderValue(const String& name) {
  for (size_t index = 0; index < configPlaceholderCount; index++) {
    if (configPlaceholders[index].name == name) {
      return configPlaceholders[index].value;
    }
  }
  return "";
}

bool typeExpansionTemplate(const String& text, uint16_t keyDelayMs) {
  bool cursorSeen = false;
  size_t charactersAfterCursor = 0;

  for (size_t index = 0; index < text.length();) {
    if (index + 1 < text.length() && text[index] == '{' && text[index + 1] == '{') {
      int chordEnd = text.indexOf("}}", index + 2);
      if (chordEnd >= 0) {
        String chord = text.substring(index + 2, chordEnd);
        if (!sendShortcut(chord, keyDelayMs)) return false;
        index = chordEnd + 2;
        continue;
      }
    }

    if (index + 2 < text.length() && (uint8_t)text[index] == 0xE2) {
      uint8_t b1 = (uint8_t)text[index+1];
      uint8_t b2 = (uint8_t)text[index+2];
      const char* altDigits = nullptr;

      if (b1 == 0x80 && b2 == 0xA2) altDigits = "0149"; // •
      else if (b1 == 0x97 && b2 == 0xA6) altDigits = "9702"; // ◦
      else if (b1 == 0x96 && b2 == 0xAA) altDigits = "9642"; // ▪
      else if (b1 == 0x96 && b2 == 0xA1) altDigits = "9633"; // □
      else if (b1 == 0x97 && b2 == 0xBE) altDigits = "9726"; // ◾
      else if (b1 == 0x99 && b2 == 0xA6) altDigits = "4";    // ♦
      else if (b1 == 0x80 && b2 == 0xA3) altDigits = "8227"; // ‣
      else if (b1 == 0x97 && b2 == 0x86) altDigits = "9670"; // ◆
      else if (b1 == 0x97 && b2 == 0x8F) altDigits = "9679"; // ●
      else if (b1 == 0x96 && b2 == 0xA0) altDigits = "9632"; // ■

      if (altDigits != nullptr) {
        if (!typeAltCode(altDigits, keyDelayMs)) return false;
        if (cursorSeen) charactersAfterCursor++;
        index += 3;
        continue;
      }
    }

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
    String customValue = customPlaceholderValue(token);
    if (rtcValue.length() > 0) {
      if (!typeAsciiStringWithDelay(rtcValue, keyDelayMs)) return false;
      if (cursorSeen) charactersAfterCursor += rtcValue.length();
    } else if (customValue.length() > 0) {
      if (!typeAsciiStringWithDelay(customValue, keyDelayMs)) return false;
      if (cursorSeen) charactersAfterCursor += customValue.length();
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
  usb_hid.keyboardRelease(RID_KEYBOARD);
  if (rule.preDelayMs > 0) cooperativeDelay(rule.preDelayMs);

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
      if (rule.steps[index].delayMs > 0) cooperativeDelay(rule.steps[index].delayMs);
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
  sendProtocolJson(response);
}

void sendProtocolSuccess(uint32_t id, const char* type) {
  JsonDocument response;
  response["qt"] = CONFIG_SCHEMA_VERSION;
  response["id"] = id;
  response["ok"] = true;
  response["type"] = type;
  sendProtocolJson(response);
}

void sendProtocolInfo(uint32_t id) {
  JsonDocument response;
  response["qt"] = CONFIG_SCHEMA_VERSION;
  response["id"] = id;
  response["ok"] = true;
  response["type"] = "hello";
  response["data"]["device"] = "QuickType RP2040 Zero";
  response["data"]["firmwareVersion"] = FIRMWARE_VERSION;
  response["data"]["configSchema"] = CONFIG_SCHEMA_VERSION;
  response["data"]["hasConfiguration"] = storedConfigurationLoaded;
  response["data"]["ruleCount"] = activeRuleCount();
  response["data"]["expansionsEnabled"] = expansionsEnabled;
  response["data"]["keypadExpansionsEnabled"] = keypadExpansionsEnabled;
  sendProtocolJson(response);
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

  if (!serialWriteText("{\"qt\":1,\"id\":") ||
      !serialWriteUInt(id) ||
      !serialWriteText(",\"ok\":true,\"type\":\"config\",\"data\":")) {
    file.close();
    return;
  }
  while (file.available()) {
    uint8_t value = (uint8_t)file.read();
    if (!serialWriteBytes(&value, 1)) {
      file.close();
      return;
    }
  }
  file.close();
  serialWriteText("}\n");
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
  sendProtocolJson(response);
}

void sendProtocolTelemetry(uint32_t id) {
  JsonDocument response;
  response["qt"] = CONFIG_SCHEMA_VERSION;
  response["id"] = id;
  response["ok"] = true;
  response["type"] = "telemetry";
  addTelemetryToJson(response["data"].to<JsonObject>());
  sendProtocolJson(response);
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
  telemetry.protocolCommandCount++;
  telemetry.lastProtocolCommandMs = millis();
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

  if (strcmp(command, "get-telemetry") == 0) {
    sendProtocolTelemetry(id);
    return;
  }

  if (strcmp(command, "set-expansions-enabled") == 0) {
    if (!request["enabled"].is<bool>()) {
      sendProtocolError(id, "INVALID_STATE", "The enabled value must be true or false.");
      return;
    }
    expansionsEnabled = request["enabled"].as<bool>();
    typedBuffer = "";
    typedSources = "";
    JsonDocument response;
    response["qt"] = CONFIG_SCHEMA_VERSION;
    response["id"] = id;
    response["ok"] = true;
    response["type"] = "expansions-state";
    response["data"]["expansionsEnabled"] = expansionsEnabled;
    sendProtocolJson(response);
    return;
  }

  if (strcmp(command, "set-keypad-expansions-enabled") == 0) {
    if (!request["enabled"].is<bool>()) {
      sendProtocolError(id, "INVALID_STATE", "The enabled value must be true or false.");
      return;
    }
    keypadExpansionsEnabled = request["enabled"].as<bool>();
    JsonDocument response;
    response["qt"] = CONFIG_SCHEMA_VERSION;
    response["id"] = id;
    response["ok"] = true;
    response["type"] = "keypad-expansions-state";
    response["data"]["keypadExpansionsEnabled"] = keypadExpansionsEnabled;
    sendProtocolJson(response);
    return;
  }

  if (strcmp(command, "set-config") == 0) {
    JsonVariantConst config = request["config"];
    String errorMessage;
    if (!saveConfiguration(config, errorMessage)) {
      telemetry.configWriteFailCount++;
      sendProtocolError(id, "INVALID_CONFIG", errorMessage);
      return;
    }

    JsonDocument response;
    response["qt"] = CONFIG_SCHEMA_VERSION;
    response["id"] = id;
    response["ok"] = true;
    response["type"] = "config-saved";
    response["data"]["ruleCount"] = activeRuleCount();
    sendProtocolJson(response);
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
    LittleFS.remove(CONFIG_BACKUP_FILE);
    clearCompiledConfiguration();
    resetBridgeStateAfterConfigurationChange();
    sendProtocolSuccess(id, "factory-reset");
    return;
  }

  if (strcmp(command, "reset-usb") == 0) {
    sendProtocolSuccess(id, "usb-resetting");
    Serial.flush();
    delay(50);
    tud_disconnect();
    delay(1000);
    tud_connect();
    return;
  }

  sendProtocolError(id, "UNKNOWN_COMMAND", String("Unknown command: ") + command);
}

void processSerialProtocol() {
  uint32_t now = millis();
  bool shouldPollConnection =
    telemetry.serialWasConnected ||
    lastSerialStatePollMs == 0 ||
    now - lastSerialStatePollMs >= SERIAL_STATE_POLL_MS;

  if (shouldPollConnection) {
    lastSerialStatePollMs = now;
    if (!serialProtocolConnected()) {
      if (telemetry.serialWasConnected) {
        telemetry.serialDisconnectCount++;
      }
      telemetry.serialWasConnected = false;
      resetSerialProtocolInput();
      return;
    }
    telemetry.serialWasConnected = true;
  } else if (!telemetry.serialWasConnected) {
    return;
  }

  while (Serial.available()) {
    if (!serialProtocolConnected()) {
      resetSerialProtocolInput();
      return;
    }

    serviceNativeUsb();

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

    if ((serialInputLine.length() & 0x3F) == 0) {
      serviceNativeUsb();
    }
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
  int boundedOffset = constrain(offsetMinutes, -1439, 1439);
  char sign = boundedOffset >= 0 ? '+' : '-';
  int absolute = abs(boundedOffset);
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

  for (unsigned int index = 0; index < pattern.length();) {
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

  // Suspend Core 1 and disable watchdog during blocking LittleFS compaction/writes
  suspendHostStack();
  disableWatchdog();

  LittleFS.remove(CLOCK_META_TEMP_FILE);
  File file = LittleFS.open(CLOCK_META_TEMP_FILE, "w");
  if (!file) {
    enableWatchdog();
    resumeHostStack();
    return false;
  }

  serializeJson(doc, file);
  file.close();
  LittleFS.remove(CLOCK_META_FILE);
  bool success = LittleFS.rename(CLOCK_META_TEMP_FILE, CLOCK_META_FILE);

  // Re-enable watchdog and resume Core 1 after LittleFS compaction/writes complete
  enableWatchdog();
  resumeHostStack();
  return success;
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
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.println("Timestamp file is empty or only contains comments.");
    }
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
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.print("Could not parse timestamp line: ");
      Serial.println(line);
    }
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
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.print("Parsed timestamp is invalid: ");
      Serial.println(line);
    }
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
  if (!ENABLE_SERIAL_DEBUG_LOGS) {
    return;
  }

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
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.println("Timestamp.txt not found. RTC was not changed.");
    }
    return;
  }

  if (ENABLE_SERIAL_DEBUG_LOGS) {
    Serial.println("Found Timestamp.txt");
  }

  RtcDateTime dt = {};

  if (!parseTimestampFile(TIMESTAMP_FILE, dt)) {
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.println("ERROR: Timestamp.txt was found but could not be parsed.");
    }
    return;
  }

  rtcSetDateTime(dt);
  rtcClearOscillatorStopFlag();

  printDateTime("RTC set from Timestamp.txt", dt);

  if (RENAME_TIMESTAMP_FILE_AFTER_SUCCESS) {
    LittleFS.remove("/Timestamp.set");

    if (LittleFS.rename(TIMESTAMP_FILE, "/Timestamp.set")) {
      if (ENABLE_SERIAL_DEBUG_LOGS) {
        Serial.println("Timestamp.txt renamed to Timestamp.set");
      }
    } else {
      if (ENABLE_SERIAL_DEBUG_LOGS) {
        Serial.println("WARNING: Could not rename Timestamp.txt");
      }
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

  cooperativeDelay(1500);

  if (ENABLE_SERIAL_DEBUG_LOGS) {
    Serial.println();
    Serial.println("Booting...");
    Serial.println("Native USB HID keyboard configured.");
  }

  uint32_t mountStart = millis();
  while (!TinyUSBDevice.mounted() && millis() - mountStart < 5000) {
    cooperativeDelay(10);
  }

  if (ENABLE_SERIAL_DEBUG_LOGS) {
    Serial.print("TinyUSB mounted: ");
    Serial.println(TinyUSBDevice.mounted() ? "yes" : "no");
  }

  // USB Host is configured on Core 1 in setup1().

  Wire.setSDA(SDA_PIN);
  Wire.setSCL(SCL_PIN);
  Wire.begin();

  if (!LittleFS.begin()) {
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.println("ERROR: LittleFS mount failed.");
    }
    return;
  }

  if (ENABLE_SERIAL_DEBUG_LOGS) {
    Serial.println("LittleFS mounted.");
  }
  recoverConfigurationStorage();
  loadConfigurationFromStorage();
  loadClockMetadata();

  if (!rtcPresent()) {
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.println("ERROR: RTC not found at I2C address 0x68.");
    }
    return;
  }

  if (ENABLE_SERIAL_DEBUG_LOGS) {
    Serial.println("RTC found at 0x68.");
  }

  if (rtcOscillatorStopFlagSet()) {
    if (ENABLE_SERIAL_DEBUG_LOGS) {
      Serial.println("WARNING: RTC oscillator stop flag is set. Time may be invalid.");
    }
  }

  handleTimestampFile();

  RtcDateTime now = rtcGetDateTime();
  printDateTime("RTC time", now);

  if (ENABLE_SERIAL_DEBUG_LOGS) {
    Serial.println("Ready. Open Notepad and test keypad macros.");
  }

  watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
  watchdogStarted = true;
}

void loop() {
  uint32_t now = millis();
  if (telemetry.lastLoopMs != 0) {
    uint32_t gap = now - telemetry.lastLoopMs;
    if (gap > telemetry.maxLoopGapMs) {
      telemetry.maxLoopGapMs = gap;
    }
  }
  telemetry.lastLoopMs = now;
  telemetry.loopCount++;

  serviceUsbBridge();
  processSerialProtocol();
  emitTelemetryHeartbeat();
}

// ============================================================
// Core 1 Execution (Dedicated to USB Host / Pico-PIO-USB)
// ============================================================

void setup1() {
  configureUsbHost();
}

void loop1() {
  if (hostStackSuspended) {
    delay(1);
    return;
  }
  // Return periodically so hostStackSuspended is observed promptly during
  // flash writes. Idle interrupt-IN transfers remain pending in the HCD.
  USBHost.task(1);

  pollMountedHidReports();
}
