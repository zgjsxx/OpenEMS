#pragma once

#include <string>
#include <cstdint>

namespace openems::rt_db {

// Shared memory data layout constants
constexpr uint32_t kMagic = 0x454D5300;  // "EMS\0"
constexpr uint32_t kVersion = 2;
constexpr uint32_t kMaxPointIdLen = 32;
constexpr uint32_t kMaxDeviceIdLen = 32;
constexpr uint32_t kMaxSiteNameLen = 64;

// Command status lifecycle: Idle -> Pending -> Executing -> Success/Failed
enum CommandStatus : uint8_t {
  CommandPending   = 0,
  CommandExecuting = 1,
  CommandSuccess   = 2,
  CommandFailed    = 3,
  CommandIdle      = 4,
};

// Telemetry: analog measurement (power, voltage, SOC, etc.)
// Stored as double (8 bytes)
struct TelemetrySlot {
  double   value;       // 8 bytes — engineering value
  uint64_t timestamp;   // 8 bytes — epoch milliseconds
  uint8_t  quality;     // 1 byte  — 0=Good,1=Questionable,2=Bad,3=Invalid
  uint8_t  valid;       // 1 byte
  uint8_t  reserved[6]; // padding
};
// sizeof = 24, aligned

// Teleindication: discrete state (run mode, on/off grid, switch position)
// Stored as uint16 state code
struct TeleindicationSlot {
  uint16_t state_code;  // 2 bytes — enumerated state value
  uint64_t timestamp;   // 8 bytes — epoch milliseconds
  uint8_t  quality;     // 1 byte
  uint8_t  valid;       // 1 byte
  uint8_t  reserved[4]; // padding
};
// sizeof = 16, aligned

// Command slot for telecontrol/teleadjust write operations
// Cross-process command delivery via shared memory
struct CommandSlot {
  char     point_id[kMaxPointIdLen]; // 32 bytes — target point
  char     reserved1[8];             // 8 bytes — padding
  double   desired_value;            // 8 bytes — engineering value to write
  double   result_value;             // 8 bytes — actual value after write (read-back)
  uint64_t submit_time;              // 8 bytes — epoch ms when command submitted
  uint64_t complete_time;            // 8 bytes — epoch ms when command completed
  uint8_t  status;                   // 1 byte  — CommandStatus enum
  uint8_t  error_code;               // 1 byte  — 0=no error
  char     reserved2[6];             // 6 bytes — padding
};
// sizeof = 80, 8-aligned

// Point index entry — maps PointId to a slot in the data section
struct PointIndexEntry {
  char     point_id[kMaxPointIdLen];   // fixed-length point identifier
  char     device_id[kMaxDeviceIdLen]; // which device this point belongs to
  uint8_t  point_category;             // 0=Telemetry, 1=Teleindication, 2=Telecontrol, 3=Setting
  uint8_t  data_type;                  // original DataType enum for reference
  uint8_t  writable;                   // 1=writable (telecontrol/teleadjust), 0=read-only
  uint8_t  reserved1;                  // padding
  uint32_t slot_offset;                // byte offset into DataSection
  char     unit[8];                    // e.g. "W", "V", "A", "%"
};
// sizeof = 80, aligned

constexpr uint32_t kMaxSiteIdLen = 32;

// Shared memory header
struct ShmHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t telemetry_count;    // number of telemetry slots (categories 0, 2, 3)
  uint32_t teleindication_count; // number of teleindication slots (category 1)
  uint32_t total_point_count;  // all points
  uint32_t index_table_offset; // byte offset to PointIndexEntry array
  uint32_t telemetry_offset;   // byte offset to TelemetrySlot array
  uint32_t teleindication_offset; // byte offset to TeleindicationSlot array
  uint32_t command_count;      // number of command slots
  uint32_t command_offset;     // byte offset to CommandSlot array
  uint64_t last_update_time;   // epoch ms of last write
  uint64_t update_seq;         // monotonically increasing sequence number
  char     site_id[kMaxSiteIdLen]; // 32 bytes
  char     site_name[kMaxSiteNameLen]; // 64 bytes
  uint32_t reserved[2];
};
// sizeof = 160 (unchanged)

} // namespace openems::rt_db
