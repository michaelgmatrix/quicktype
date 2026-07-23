// Minimal RP2040-Zero transparent USB keyboard bridge diagnostic.
#include <Arduino.h>
#include <hardware/gpio.h>
#include <pio_usb.h>
#include <Adafruit_TinyUSB.h>
#include <tusb.h>

static constexpr uint8_t USB_HOST_DP_GPIO = 0;
static constexpr uint8_t REPORT_ID_KEYBOARD = 1;
static constexpr size_t QUEUE_SIZE = 32;

static_assert(F_CPU == 120000000UL, "Build this bridge at 120 MHz.");

uint8_t const deviceReportDescriptor[] = {
  TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD))
};

Adafruit_USBD_HID deviceHid;
Adafruit_USBH_Host USBHost;

static hid_keyboard_report_t reportQueue[QUEUE_SIZE];
static volatile size_t queueHead = 0;
static volatile size_t queueTail = 0;
static volatile bool keyboardMounted = false;
static volatile uint8_t keyboardDevAddr = 0;
static volatile uint8_t keyboardInstance = 0;
static volatile uint32_t lastKeyboardReportMs = 0;
static volatile uint32_t hostReportCount = 0;
static volatile uint32_t changedReportCount = 0;
static volatile uint32_t nonzeroReportCount = 0;
static volatile uint32_t forwardedReportCount = 0;
static volatile uint32_t sendFailCount = 0;
static volatile uint32_t queueDropCount = 0;
static volatile uint32_t receiveSubmitCount = 0;
static volatile uint32_t receiveSubmitFailCount = 0;
static volatile uint32_t hostLoopCount = 0;
static volatile uint32_t mountCount = 0;
static volatile uint32_t unmountCount = 0;
static hid_keyboard_report_t lastHostReport = {};
static bool lastHostReportValid = false;

void setup() {
  deviceHid.setPollInterval(2);
  deviceHid.setReportDescriptor(deviceReportDescriptor, sizeof(deviceReportDescriptor));
  deviceHid.setStringDescriptor("QuickType Bridge Test");
  deviceHid.begin();
  Serial.begin(115200);
}

void loop() {
  TinyUSBDevice.task();

  static uint32_t lastStatusMs = 0;
  uint32_t now = millis();
  if (Serial && Serial.dtr() && now - lastStatusMs >= 1000 && Serial.availableForWrite() >= 128) {
    lastStatusMs = now;
    size_t depth = (queueHead + QUEUE_SIZE - queueTail) % QUEUE_SIZE;
    Serial.print("BRIDGE uptime=");
    Serial.print(now);
    Serial.print(" mounted=");
    Serial.print(TinyUSBDevice.mounted());
    Serial.print(" suspended=");
    Serial.print(TinyUSBDevice.suspended());
    Serial.print(" ready=");
    Serial.print(deviceHid.ready());
    Serial.print(" keyboardMounted=");
    Serial.print(keyboardMounted);
    Serial.print(" queue=");
    Serial.print(depth);
    Serial.print(" hostReports=");
    Serial.print(hostReportCount);
    Serial.print(" changed=");
    Serial.print(changedReportCount);
    Serial.print(" nonzero=");
    Serial.print(nonzeroReportCount);
    Serial.print(" forwarded=");
    Serial.print(forwardedReportCount);
    Serial.print(" sendFail=");
    Serial.print(sendFailCount);
    Serial.print(" dropped=");
    Serial.print(queueDropCount);
    Serial.print(" submits=");
    Serial.print(receiveSubmitCount);
    Serial.print(" submitFail=");
    Serial.print(receiveSubmitFailCount);
    Serial.print(" hostLoops=");
    Serial.print(hostLoopCount);
    Serial.print(" frames=");
    Serial.print(pio_usb_host_get_frame_number());
    Serial.print(" dpRaw=");
    Serial.print(gpio_get(USB_HOST_DP_GPIO));
    Serial.print(" dmRaw=");
    Serial.print(gpio_get(USB_HOST_DP_GPIO + 1));
    Serial.print(" line=");
    Serial.print(pio_usb_host_get_line_state(0));
    Serial.print(" rootState=");
    Serial.print(pio_usb_host_get_root_state(0));
    Serial.print(" pioTxRecover=");
    Serial.print(pio_usb_host_get_tx_recovery_count());
    Serial.print(" pioRxRecover=");
    Serial.print(pio_usb_host_get_rx_recovery_count());
    Serial.print(" pioRxOverflow=");
    Serial.print(pio_usb_host_get_rx_overflow_count());
    Serial.print(" lastReportMs=");
    Serial.print(lastKeyboardReportMs);
    Serial.print(" mounts=");
    Serial.print(mountCount);
    Serial.print(" unmounts=");
    Serial.println(unmountCount);
  }

  if (queueTail == queueHead) {
    delay(1);
    return;
  }

  if (TinyUSBDevice.suspended()) {
    TinyUSBDevice.remoteWakeup();
    delay(1);
    return;
  }

  if (!deviceHid.ready()) {
    delay(1);
    return;
  }

  __asm__ volatile("dmb" : : : "memory");
  hid_keyboard_report_t report = reportQueue[queueTail];
  uint8_t keycodes[6];
  memcpy(keycodes, report.keycode, sizeof(keycodes));
  if (deviceHid.keyboardReport(REPORT_ID_KEYBOARD, report.modifier, keycodes)) {
    forwardedReportCount++;
    queueTail = (queueTail + 1) % QUEUE_SIZE;
  } else {
    sendFailCount++;
  }
}

void setup1() {
  pio_usb_configuration_t config = PIO_USB_DEFAULT_CONFIG;
  config.pin_dp = USB_HOST_DP_GPIO;
  tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);
  USBHost.configure_pio_usb(1, &config);
  USBHost.begin(1);
}

void loop1() {
  // The default timeout is UINT32_MAX and can leave this core dormant. A 1 ms
  // bound services TinyUSB without a wasteful tight spin and still checks the
  // receive endpoint on every USB frame.
  USBHost.task(1);
  hostLoopCount++;

  if (!keyboardMounted) {
    return;
  }

  // A busy interrupt-IN endpoint is the normal idle state: Pico-PIO-USB keeps
  // polling it through NAK responses. Never abort it merely because the
  // keyboard is idle. Submit only when TinyUSB says no receive is pending.
  if (tuh_hid_receive_ready(keyboardDevAddr, keyboardInstance)) {
    if (tuh_hid_receive_report(keyboardDevAddr, keyboardInstance)) {
      receiveSubmitCount++;
    } else {
      receiveSubmitFailCount++;
    }
  }
}

extern "C" void tuh_hid_mount_cb(
  uint8_t devAddr,
  uint8_t instance,
  uint8_t const* reportDescriptor,
  uint16_t descriptorLength
) {
  (void)reportDescriptor;
  (void)descriptorLength;

  bool isKeyboard = tuh_hid_interface_protocol(devAddr, instance) == HID_ITF_PROTOCOL_KEYBOARD;
  mountCount++;
  if (isKeyboard) {
    keyboardMounted = true;
    keyboardDevAddr = devAddr;
    keyboardInstance = instance;
    lastKeyboardReportMs = millis();
    lastHostReportValid = false;
  }

  if (isKeyboard && tuh_hid_get_protocol(devAddr, instance) != HID_PROTOCOL_BOOT) {
    if (tuh_hid_set_protocol(devAddr, instance, HID_PROTOCOL_BOOT)) {
      return;
    }
  }

  tuh_hid_receive_report(devAddr, instance);
}

extern "C" void tuh_hid_set_protocol_complete_cb(
  uint8_t devAddr,
  uint8_t instance,
  uint8_t protocol
) {
  (void)protocol;
  tuh_hid_receive_report(devAddr, instance);
}

extern "C" void tuh_hid_umount_cb(uint8_t devAddr, uint8_t instance) {
  unmountCount++;
  if (keyboardMounted && devAddr == keyboardDevAddr && instance == keyboardInstance) {
    keyboardMounted = false;
    lastHostReportValid = false;
  }
  queueHead = 0;
  queueTail = 0;
}

extern "C" void tuh_hid_report_received_cb(
  uint8_t devAddr,
  uint8_t instance,
  uint8_t const* report,
  uint16_t length
) {
  if (tuh_hid_interface_protocol(devAddr, instance) == HID_ITF_PROTOCOL_KEYBOARD &&
      report != nullptr && length >= sizeof(hid_keyboard_report_t)) {
    hostReportCount++;
    lastKeyboardReportMs = millis();
    hid_keyboard_report_t decoded = {};
    memcpy(&decoded, report, sizeof(decoded));

    bool nonzero = decoded.modifier != 0;
    for (uint8_t keycode : decoded.keycode) {
      if (keycode != 0) {
        nonzero = true;
        break;
      }
    }
    if (nonzero) {
      nonzeroReportCount++;
    }

    // Some wireless receivers repeat an unchanged report after waking. A USB
    // keyboard report is state, not an event, so forwarding identical states
    // only floods the inter-core queue and can discard the later key release.
    bool changed = !lastHostReportValid ||
                   memcmp(&decoded, &lastHostReport, sizeof(decoded)) != 0;
    if (changed) {
      changedReportCount++;
      lastHostReport = decoded;
      lastHostReportValid = true;
      size_t nextHead = (queueHead + 1) % QUEUE_SIZE;
      if (nextHead != queueTail) {
        reportQueue[queueHead] = decoded;
        __asm__ volatile("dmb" : : : "memory");
        queueHead = nextHead;
      } else {
        queueDropCount++;
      }
    }
  }

  if (tuh_hid_receive_report(devAddr, instance)) {
    receiveSubmitCount++;
  } else {
    receiveSubmitFailCount++;
  }
}
