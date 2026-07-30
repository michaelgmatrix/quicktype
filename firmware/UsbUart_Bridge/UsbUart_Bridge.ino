#ifndef USE_TINYUSB_HOST
#define USE_TINYUSB_HOST
#endif

// GhostLever USB-UART native host bridge build: 0.2.34 (2026-07-30)
#include <Arduino.h>
#include "QuickTypeUartProtocol.h"
#include <Adafruit_TinyUSB.h>
#include <Adafruit_NeoPixel.h>
#include <hardware/structs/usb.h>

struct HidInterfaceSnapshot;

static constexpr uint8_t UART_TX_PIN = 4;
static constexpr uint8_t UART_RX_PIN = 5;
static constexpr uint32_t UART_BAUD = 115200;
static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 250;
static constexpr uint32_t HID_SNAPSHOT_INTERVAL_MS = 2000;
static constexpr uint8_t MAX_HID_INTERFACES = 4;
static constexpr uint16_t MAX_HID_DESCRIPTOR_SIZE = 1024;

static constexpr char BRIDGE_FIRMWARE_VERSION[] = "0.2.34";
static constexpr uint32_t PRIMARY_TIMEOUT_MS = 1500;
static uint32_t lastPrimaryPacketMs = 0;
static constexpr uint8_t NEOPIXEL_PIN = 16;

static uint8_t packetSequence = 0;
static uint32_t lastHeartbeatMs = 0;
static uint32_t lastHidSnapshotMs = 0;
Adafruit_USBH_Host USBHost;
static Adafruit_NeoPixel bridgeLed(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

void setBridgeLed(uint8_t r, uint8_t g, uint8_t b) {
  bridgeLed.begin();
  uint8_t dimR = (uint16_t)r * 40 / 255;
  uint8_t dimG = (uint16_t)g * 40 / 255;
  uint8_t dimB = (uint16_t)b * 40 / 255;
  bridgeLed.setPixelColor(0, bridgeLed.Color(dimR, dimG, dimB));
  bridgeLed.show();
}

struct HidInterfaceSnapshot {
  uint8_t devAddr;
  uint8_t instance;
  uint8_t protocol;
  uint16_t descriptorLength;
  uint8_t descriptor[MAX_HID_DESCRIPTOR_SIZE];
  bool mounted;
};

static HidInterfaceSnapshot hidInterfaces[MAX_HID_INTERFACES] = {};
static bool hostBeginOk = false;
static uint32_t receiveSubmitCount = 0;
static uint32_t receiveSubmitFailCount = 0;
static uint32_t reportCallbackCount = 0;
static uint32_t reportForwardCount = 0;
static uint32_t reportDropCount = 0;
static uint8_t lastReportDevAddr = 0;
static uint8_t lastReportInstance = 0;
static uint16_t lastReportLength = 0;
static uint8_t lastReportPrefix[4] = {};

void updateBridgeLedState() {
  bool primaryInContact = (lastPrimaryPacketMs != 0 && (millis() - lastPrimaryPacketMs <= PRIMARY_TIMEOUT_MS));
  bool deviceMounted = false;
  for (uint8_t i = 0; i < MAX_HID_INTERFACES; i++) {
    if (hidInterfaces[i].mounted) {
      deviceMounted = true;
      break;
    }
  }

  if (deviceMounted) {
    setBridgeLed(0, 255, 0); // GREEN = Keyboard connected & mounted
  } else if (hostBeginOk || primaryInContact) {
    setBridgeLed(0, 0, 255); // BLUE = UART Bridge Host Active & Ready
  } else {
    setBridgeLed(255, 0, 0); // RED = Host Initialization Failed
  }
}

void writePacket(uint8_t type, const uint8_t* payload, uint8_t length) {
  uint8_t sequence = packetSequence++;
  uint8_t header[] = {
    QuickTypeUart::MAGIC_0,
    QuickTypeUart::MAGIC_1,
    QuickTypeUart::VERSION,
    type,
    sequence,
    length,
    QuickTypeUart::checksum(type, sequence, payload, length)
  };

  Serial2.write(header, sizeof(header));
  if (length > 0) {
    Serial2.write(payload, length);
  }
}

void sendBridgeVersion() {
  writePacket(
    QuickTypeUart::TYPE_VERSION,
    reinterpret_cast<const uint8_t*>(BRIDGE_FIRMWARE_VERSION),
    strlen(BRIDGE_FIRMWARE_VERSION)
  );
}

void writeHidMount(
  uint8_t devAddr,
  uint8_t instance,
  uint8_t protocol,
  const uint8_t* descriptor,
  uint16_t descriptorLength
) {
  uint8_t mountPayload[] = {
    devAddr,
    instance,
    protocol,
    static_cast<uint8_t>(descriptorLength & 0xFF),
    static_cast<uint8_t>(descriptorLength >> 8)
  };
  writePacket(QuickTypeUart::TYPE_HID_MOUNT, mountPayload, sizeof(mountPayload));

  uint16_t offset = 0;
  while (offset < descriptorLength) {
    uint8_t chunkLength = min(
      static_cast<uint16_t>(QuickTypeUart::DESCRIPTOR_CHUNK_SIZE),
      static_cast<uint16_t>(descriptorLength - offset)
    );
    uint8_t payload[4 + QuickTypeUart::DESCRIPTOR_CHUNK_SIZE] = {
      devAddr,
      instance,
      static_cast<uint8_t>(offset & 0xFF),
      static_cast<uint8_t>(offset >> 8)
    };
    memcpy(payload + 4, descriptor + offset, chunkLength);
    writePacket(QuickTypeUart::TYPE_HID_DESCRIPTOR, payload, chunkLength + 4);
    offset += chunkLength;
  }
}

HidInterfaceSnapshot* findHidInterface(uint8_t devAddr, uint8_t instance) {
  for (uint8_t index = 0; index < MAX_HID_INTERFACES; index++) {
    HidInterfaceSnapshot& info = hidInterfaces[index];
    if (info.mounted && info.devAddr == devAddr && info.instance == instance) {
      return &info;
    }
  }
  return nullptr;
}

HidInterfaceSnapshot* cacheHidInterface(
  uint8_t devAddr,
  uint8_t instance,
  uint8_t protocol,
  const uint8_t* descriptor,
  uint16_t descriptorLength
) {
  HidInterfaceSnapshot* info = findHidInterface(devAddr, instance);
  if (info == nullptr) {
    for (uint8_t index = 0; index < MAX_HID_INTERFACES; index++) {
      if (!hidInterfaces[index].mounted) {
        info = &hidInterfaces[index];
        break;
      }
    }
  }
  if (info == nullptr || descriptorLength > MAX_HID_DESCRIPTOR_SIZE) {
    return nullptr;
  }

  *info = HidInterfaceSnapshot();
  info->devAddr = devAddr;
  info->instance = instance;
  info->protocol = protocol;
  info->descriptorLength = descriptorLength;
  info->mounted = true;
  if (descriptorLength > 0 && descriptor != nullptr) {
    memcpy(info->descriptor, descriptor, descriptorLength);
  }
  return info;
}

void writeMountedHidSnapshots() {
  for (uint8_t index = 0; index < MAX_HID_INTERFACES; index++) {
    HidInterfaceSnapshot const& info = hidInterfaces[index];
    if (info.mounted) {
      writeHidMount(
        info.devAddr,
        info.instance,
        info.protocol,
        info.descriptor,
        info.descriptorLength
      );
    }
  }
}

void writeLog(const char* message) {
  if (message == nullptr) return;
  uint8_t len = min(static_cast<size_t>(QuickTypeUart::MAX_PAYLOAD_SIZE), strlen(message));
  writePacket(QuickTypeUart::TYPE_LOG, reinterpret_cast<const uint8_t*>(message), len);
}

bool writeHidReport(
  uint8_t devAddr,
  uint8_t instance,
  const uint8_t* report,
  uint16_t length
) {
  if (report == nullptr || length > QuickTypeUart::MAX_PAYLOAD_SIZE - 2) {
    return false;
  }

  uint8_t payload[QuickTypeUart::MAX_PAYLOAD_SIZE] = { devAddr, instance };
  memcpy(payload + 2, report, length);
  writePacket(QuickTypeUart::TYPE_HID_REPORT, payload, length + 2);
  return true;
}

void checkHardwareHostDiagnostics() {
  uint32_t sieStatus = usb_hw->sie_status;
  uint32_t mainCtrl = usb_hw->main_ctrl;
  char buf[QuickTypeUart::MAX_PAYLOAD_SIZE] = {};
  snprintf(buf, sizeof(buf),
           "Host Status: ok=%d ctrl=0x%02X sie=0x%04X submit=%lu fail=%lu reports=%lu uart=%lu drop=%lu last=%u/%u:%u %02X%02X%02X%02X",
           hostBeginOk ? 1 : 0,
           static_cast<unsigned int>(mainCtrl & 0xFF),
           static_cast<unsigned int>(sieStatus & 0xFFFF),
           static_cast<unsigned long>(receiveSubmitCount),
           static_cast<unsigned long>(receiveSubmitFailCount),
           static_cast<unsigned long>(reportCallbackCount),
           static_cast<unsigned long>(reportForwardCount),
           static_cast<unsigned long>(reportDropCount),
           lastReportDevAddr,
           lastReportInstance,
           lastReportLength,
           lastReportPrefix[0],
           lastReportPrefix[1],
           lastReportPrefix[2],
           lastReportPrefix[3]);
  writeLog(buf);
}

void setup() {
  setBridgeLed(255, 0, 0); delay(200); // Boot test Red
  setBridgeLed(0, 255, 0); delay(200); // Boot test Green
  setBridgeLed(0, 0, 255); delay(200); // Boot test Blue
  setBridgeLed(255, 0, 0); // Red on boot

  Serial2.setTX(UART_TX_PIN);
  Serial2.setRX(UART_RX_PIN);
  Serial2.begin(UART_BAUD);
  writePacket(QuickTypeUart::TYPE_HEARTBEAT, nullptr, 0);
  sendBridgeVersion();
  writeLog("⚡ UART Bridge Board Booted (v0.2.33)");

  // Keep every interface in report protocol. Composite wireless receivers can
  // expose a boot-capable keyboard interface but only deliver their full input
  // stream reliably in report mode. Wired keyboards are decoded from their
  // forwarded report descriptor, so they do not need a boot-protocol switch.
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

  hostBeginOk = USBHost.begin(0);
  writeLog(hostBeginOk ? "USBHost.begin(0) SUCCESS" : "USBHost.begin(0) FAILED");
  checkHardwareHostDiagnostics();

  if (hostBeginOk) {
    setBridgeLed(0, 0, 255); // BLUE = UART Bridge Host Active & Ready!
  } else {
    setBridgeLed(255, 0, 0); // RED = Host Initialization Failed
  }
}

void loop() {
  // Bound TinyUSB servicing so the receive-recovery poll below runs even when
  // there is no queued host event to wake an unbounded tuh_task_ext() call.
  USBHost.task(1);

  if (Serial2.available()) {
    lastPrimaryPacketMs = millis();
    while (Serial2.available()) Serial2.read();
  }

  updateBridgeLedState();

  for (uint8_t i = 0; i < MAX_HID_INTERFACES; i++) {
    if (hidInterfaces[i].mounted) {
      if (tuh_hid_receive_ready(hidInterfaces[i].devAddr, hidInterfaces[i].instance)) {
        if (tuh_hid_receive_report(hidInterfaces[i].devAddr, hidInterfaces[i].instance)) {
          receiveSubmitCount++;
        } else {
          receiveSubmitFailCount++;
        }
      }
    }
  }

  uint32_t now = millis();
  if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    writePacket(QuickTypeUart::TYPE_HEARTBEAT, nullptr, 0);
  }
  if (now - lastHidSnapshotMs >= HID_SNAPSHOT_INTERVAL_MS) {
    lastHidSnapshotMs = now;
    sendBridgeVersion();
    checkHardwareHostDiagnostics();
    writeMountedHidSnapshots();
  }
}

extern "C" void tuh_hid_mount_cb(
  uint8_t devAddr,
  uint8_t instance,
  uint8_t const* reportDescriptor,
  uint16_t descriptorLength
) {
  uint8_t protocol = tuh_hid_interface_protocol(devAddr, instance);

  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(devAddr, &vid, &pid);
  char mountLogBuf[80];
  snprintf(mountLogBuf, sizeof(mountLogBuf), "USB HID Mount: addr=%d inst=%d vid=0x%04X pid=0x%04X proto=%d",
           devAddr, instance, vid, pid, protocol);
  writeLog(mountLogBuf);

  if (protocol != HID_ITF_PROTOCOL_KEYBOARD && reportDescriptor != nullptr && descriptorLength > 0) {
    tuh_hid_report_info_t reports[8];
    uint8_t reportCount = tuh_hid_parse_report_descriptor(reports, 8, reportDescriptor, descriptorLength);
    for (uint8_t i = 0; i < reportCount; i++) {
      if (reports[i].usage_page == HID_USAGE_PAGE_DESKTOP && reports[i].usage == HID_USAGE_DESKTOP_KEYBOARD) {
        protocol = HID_ITF_PROTOCOL_KEYBOARD;
        break;
      }
    }
  }

  HidInterfaceSnapshot* snapshot = cacheHidInterface(
    devAddr,
    instance,
    protocol,
    reportDescriptor,
    descriptorLength
  );
  if (snapshot != nullptr) {
    writeHidMount(
      snapshot->devAddr,
      snapshot->instance,
      snapshot->protocol,
      snapshot->descriptor,
      snapshot->descriptorLength
    );
  } else {
    writeHidMount(devAddr, instance, protocol, reportDescriptor, descriptorLength);
  }

  if (protocol == HID_ITF_PROTOCOL_MOUSE &&
      tuh_hid_get_protocol(devAddr, instance) != HID_PROTOCOL_BOOT) {
    // Use the standard Boot Mouse report as the bridge wire format. The
    // primary emits the equivalent composite HID mouse report to the computer.
    if (tuh_hid_set_protocol(devAddr, instance, HID_PROTOCOL_BOOT)) {
      return;
    }
  }

  if (tuh_hid_receive_ready(devAddr, instance)) {
    if (tuh_hid_receive_report(devAddr, instance)) {
      receiveSubmitCount++;
    } else {
      receiveSubmitFailCount++;
    }
  }

  setBridgeLed(0, 255, 0); // GREEN = Keyboard Device Mounted & Ready!
}

extern "C" void tuh_hid_set_protocol_complete_cb(uint8_t devAddr, uint8_t instance, uint8_t protocol) {
  (void)protocol;
  writeLog("HID Protocol Complete");
  tuh_hid_receive_report(devAddr, instance);
}

extern "C" void tuh_hid_umount_cb(uint8_t devAddr, uint8_t instance) {
  writeLog("USB HID Unmount Event");
  HidInterfaceSnapshot* info = findHidInterface(devAddr, instance);
  if (info != nullptr) {
    *info = HidInterfaceSnapshot();
  }
  uint8_t payload[] = { devAddr, instance };
  writePacket(QuickTypeUart::TYPE_HID_UNMOUNT, payload, sizeof(payload));

  setBridgeLed(0, 0, 255); // BLUE = Primary Board connected, waiting for USB keyboard
}

extern "C" void tuh_hid_report_received_cb(
  uint8_t devAddr,
  uint8_t instance,
  uint8_t const* report,
  uint16_t length
) {
  reportCallbackCount++;
  lastReportDevAddr = devAddr;
  lastReportInstance = instance;
  lastReportLength = length;
  memset(lastReportPrefix, 0, sizeof(lastReportPrefix));
  if (report != nullptr) {
    memcpy(lastReportPrefix, report, min(static_cast<uint16_t>(sizeof(lastReportPrefix)), length));
  }
  setBridgeLed(0, 255, 255); // FLASH CYAN on Keypress Report!
  if (writeHidReport(devAddr, instance, report, length)) {
    reportForwardCount++;
  } else {
    reportDropCount++;
  }
  if (tuh_hid_receive_report(devAddr, instance)) {
    receiveSubmitCount++;
  } else {
    receiveSubmitFailCount++;
  }
  setBridgeLed(0, 255, 0); // Return to GREEN
}
