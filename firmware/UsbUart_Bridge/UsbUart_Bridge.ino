// QuickType USB-UART bridge build: 0.1.0 (2026-07-25)
#include <Arduino.h>
#include "QuickTypeUartProtocol.h"

#include <Adafruit_TinyUSB.h>

#ifndef USE_TINYUSB
#error This keyboard-facing firmware requires the RP2040 native TinyUSB stack.
#endif

static constexpr uint8_t UART_TX_PIN = 4;
static constexpr uint8_t UART_RX_PIN = 5;
static constexpr uint32_t UART_BAUD = 1000000;
static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 250;
static constexpr uint32_t HID_SNAPSHOT_INTERVAL_MS = 2000;
static constexpr uint8_t MAX_HID_INTERFACES = 4;
static constexpr uint16_t MAX_HID_DESCRIPTOR_SIZE = 1024;

static uint8_t packetSequence = 0;
static uint32_t lastHeartbeatMs = 0;
static uint32_t lastHidSnapshotMs = 0;
Adafruit_USBH_Host USBHost;

struct HidInterfaceSnapshot {
  uint8_t devAddr;
  uint8_t instance;
  uint8_t protocol;
  uint16_t descriptorLength;
  uint8_t descriptor[MAX_HID_DESCRIPTOR_SIZE];
  bool mounted;
};

static HidInterfaceSnapshot hidInterfaces[MAX_HID_INTERFACES] = {};

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

void writeHidReport(
  uint8_t devAddr,
  uint8_t instance,
  const uint8_t* report,
  uint16_t length
) {
  if (report == nullptr || length > QuickTypeUart::MAX_PAYLOAD_SIZE - 2) {
    return;
  }

  uint8_t payload[QuickTypeUart::MAX_PAYLOAD_SIZE] = { devAddr, instance };
  memcpy(payload + 2, report, length);
  writePacket(QuickTypeUart::TYPE_HID_REPORT, payload, length + 2);
}

void setup() {
  Serial2.setTX(UART_TX_PIN);
  Serial2.setRX(UART_RX_PIN);
  Serial2.begin(UART_BAUD);
  writePacket(QuickTypeUart::TYPE_HEARTBEAT, nullptr, 0);

  tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);
  USBHost.begin(0);
}

void loop() {
  USBHost.task(0);

  uint32_t now = millis();
  if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    writePacket(QuickTypeUart::TYPE_HEARTBEAT, nullptr, 0);
  }
  if (now - lastHidSnapshotMs >= HID_SNAPSHOT_INTERVAL_MS) {
    lastHidSnapshotMs = now;
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

  if (protocol == HID_ITF_PROTOCOL_KEYBOARD &&
      tuh_hid_get_protocol(devAddr, instance) != HID_PROTOCOL_BOOT) {
    if (tuh_hid_set_protocol(devAddr, instance, HID_PROTOCOL_BOOT)) {
      return;
    }
  }

  tuh_hid_receive_report(devAddr, instance);
}

extern "C" void tuh_hid_set_protocol_complete_cb(uint8_t devAddr, uint8_t instance, uint8_t protocol) {
  (void)protocol;
  tuh_hid_receive_report(devAddr, instance);
}

extern "C" void tuh_hid_umount_cb(uint8_t devAddr, uint8_t instance) {
  HidInterfaceSnapshot* info = findHidInterface(devAddr, instance);
  if (info != nullptr) {
    *info = HidInterfaceSnapshot();
  }
  uint8_t payload[] = { devAddr, instance };
  writePacket(QuickTypeUart::TYPE_HID_UNMOUNT, payload, sizeof(payload));
}

extern "C" void tuh_hid_report_received_cb(
  uint8_t devAddr,
  uint8_t instance,
  uint8_t const* report,
  uint16_t length
) {
  writeHidReport(devAddr, instance, report, length);
  tuh_hid_receive_report(devAddr, instance);
}
