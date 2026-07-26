#pragma once

#include <Arduino.h>

namespace QuickTypeUart {

static constexpr uint8_t MAGIC_0 = 'Q';
static constexpr uint8_t MAGIC_1 = 'T';
static constexpr uint8_t VERSION = 2;
static constexpr uint8_t TYPE_HEARTBEAT = 1;
static constexpr uint8_t TYPE_HID_MOUNT = 2;
static constexpr uint8_t TYPE_HID_DESCRIPTOR = 3;
static constexpr uint8_t TYPE_HID_REPORT = 4;
static constexpr uint8_t TYPE_HID_UNMOUNT = 5;
static constexpr uint8_t HEADER_FIELD_COUNT = 5;
static constexpr uint8_t MAX_PAYLOAD_SIZE = 72;
static constexpr uint8_t DESCRIPTOR_CHUNK_SIZE = 60;

inline uint8_t checksum(
  uint8_t type,
  uint8_t sequence,
  const uint8_t* payload,
  uint8_t length
) {
  uint8_t value = MAGIC_0 ^ MAGIC_1 ^ VERSION ^ type ^ sequence ^ length;
  for (uint8_t index = 0; index < length; index++) {
    value ^= payload[index];
  }
  return value;
}

}  // namespace QuickTypeUart
