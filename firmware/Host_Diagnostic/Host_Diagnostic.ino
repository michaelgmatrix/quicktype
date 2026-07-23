// Temporary QuickType RP2040-Zero PIO USB host diagnostic.
#include <Arduino.h>
#include <pio_usb.h>
#include <Adafruit_TinyUSB.h>
#include <tusb.h>
#include <hardware/clocks.h>

static constexpr uint8_t USB_HOST_DP_GPIO = 0;
static_assert(F_CPU == 120000000UL, "Build this diagnostic at 120 MHz.");

Adafruit_USBH_Host USBHost;
static volatile uint32_t reportCount = 0;

void setup() {
  Serial.begin(115200);
}

void loop() {
  static uint32_t lastStatusMs = 0;
  if (Serial && millis() - lastStatusMs >= 1000) {
    lastStatusMs = millis();
    Serial.print("HOST-DIAGNOSTIC cpuHz=");
    Serial.print(clock_get_hz(clk_sys));
    Serial.print(" reports=");
    Serial.println(reportCount);
  }
  delay(1);
}

void setup1() {
  pio_usb_configuration_t config = PIO_USB_DEFAULT_CONFIG;
  config.pin_dp = USB_HOST_DP_GPIO;
  tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);
  USBHost.configure_pio_usb(1, &config);
  USBHost.begin(1);
}

void loop1() {
  USBHost.task();
}

extern "C" void tuh_hid_mount_cb(
  uint8_t devAddr,
  uint8_t instance,
  uint8_t const* reportDescriptor,
  uint16_t descriptorLength
) {
  (void)reportDescriptor;
  (void)descriptorLength;
  Serial.print("MOUNT addr=");
  Serial.print(devAddr);
  Serial.print(" instance=");
  Serial.print(instance);
  Serial.print(" protocol=");
  Serial.println(tuh_hid_interface_protocol(devAddr, instance));
  if (!tuh_hid_receive_report(devAddr, instance)) {
    Serial.println("RECEIVE-REQUEST-FAILED");
  }
}

extern "C" void tuh_hid_umount_cb(uint8_t devAddr, uint8_t instance) {
  Serial.print("UNMOUNT addr=");
  Serial.print(devAddr);
  Serial.print(" instance=");
  Serial.println(instance);
}

extern "C" void tuh_hid_report_received_cb(
  uint8_t devAddr,
  uint8_t instance,
  uint8_t const* report,
  uint16_t length
) {
  reportCount++;
  Serial.print("REPORT addr=");
  Serial.print(devAddr);
  Serial.print(" instance=");
  Serial.print(instance);
  Serial.print(" len=");
  Serial.print(length);
  Serial.print(" data=");
  for (uint16_t index = 0; index < length; index++) {
    if (index > 0) Serial.print(' ');
    if (report[index] < 0x10) Serial.print('0');
    Serial.print(report[index], HEX);
  }
  Serial.println();
  if (!tuh_hid_receive_report(devAddr, instance)) {
    Serial.println("RECEIVE-REQUEST-FAILED");
  }
}
