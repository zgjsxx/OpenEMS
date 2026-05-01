#pragma once

#include <cstddef>
#include <cstdint>

namespace openems::rt_db {

constexpr uint32_t kTableMagic = 0x454D5354;  // "EMST"
constexpr uint16_t kTableVersion = 1;

constexpr uint32_t kMaxTableNameLen = 32;
constexpr uint32_t kMaxShmNameLen = 64;
constexpr uint32_t kMaxPointIdLen = 32;
constexpr uint32_t kMaxDeviceIdLen = 32;
constexpr uint32_t kMaxSiteIdLen = 32;
constexpr uint32_t kMaxSiteNameLen = 64;
constexpr uint32_t kMaxStrategyIdLen = 64;
constexpr uint32_t kMaxAlarmIdLen = 64;
constexpr uint32_t kMaxSeverityLen = 16;
constexpr uint32_t kMaxMessageLen = 128;
constexpr uint32_t kMaxReasonLen = 128;
constexpr uint32_t kMaxTargetPointIdsLen = 128;
constexpr uint32_t kMaxLastErrorLen = 128;
constexpr uint32_t kInvalidSlotIndex = 0xFFFFFFFFu;

enum TableId : uint16_t {
  TableCatalog = 1,
  TablePointIndex = 2,
  TableTelemetry = 3,
  TableTeleindication = 4,
  TableCommand = 5,
  TableStrategyRuntime = 6,
  TableAlarmActive = 7,
};

enum CommandStatus : uint8_t {
  CommandPending = 0,
  CommandExecuting = 1,
  CommandSuccess = 2,
  CommandFailed = 3,
  CommandIdle = 4,
};

struct TableHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t table_id;
  uint32_t header_size;
  uint32_t row_size;
  uint32_t capacity;
  uint32_t row_count;
  uint32_t flags;
  uint64_t last_update_time;
  uint64_t update_seq;
  char table_name[kMaxTableNameLen];
  uint32_t reserved[4];
};

struct CatalogEntry {
  uint16_t table_id;
  uint16_t reserved0;
  uint32_t row_size;
  uint32_t capacity;
  uint32_t row_count;
  uint32_t flags;
  char table_name[kMaxTableNameLen];
  char shm_name[kMaxShmNameLen];
};

struct PointIndexHeader {
  TableHeader base;
  uint32_t telemetry_capacity;
  uint32_t teleindication_capacity;
  uint32_t command_capacity;
  uint32_t telemetry_used;
  uint32_t teleindication_used;
  uint32_t command_used;
  uint32_t reserved0;
  char site_id[kMaxSiteIdLen];
  char site_name[kMaxSiteNameLen];
  uint32_t reserved[4];
};

struct PointIndexEntry {
  char point_id[kMaxPointIdLen];
  char device_id[kMaxDeviceIdLen];
  uint8_t point_category;  // 0=Telemetry, 1=Teleindication, 2=Telecontrol, 3=Setting
  uint8_t data_type;
  uint8_t writable;
  uint8_t reserved0;
  uint32_t telemetry_slot_index;
  uint32_t teleindication_slot_index;
  uint32_t command_slot_index;
  char unit[8];
};

struct TelemetrySlot {
  double value;
  uint64_t timestamp;
  uint8_t quality;
  uint8_t valid;
  uint8_t reserved[6];
};

struct TeleindicationSlot {
  uint16_t state_code;
  uint64_t timestamp;
  uint8_t quality;
  uint8_t valid;
  uint8_t reserved[4];
};

struct CommandSlot {
  char point_id[kMaxPointIdLen];
  char reserved1[8];
  double desired_value;
  double result_value;
  uint64_t submit_time;
  uint64_t complete_time;
  uint8_t status;
  uint8_t error_code;
  char reserved2[6];
};

struct StrategyRuntimeSlot {
  char strategy_id[kMaxStrategyIdLen];
  char target_point_id[kMaxTargetPointIdsLen];
  double target_value;
  uint64_t update_time;
  uint8_t suppressed;
  char suppress_reason[kMaxReasonLen];
  char last_error[kMaxLastErrorLen];
};

struct AlarmActiveSlot {
  char alarm_id[kMaxAlarmIdLen];
  char point_id[kMaxPointIdLen];
  char device_id[kMaxDeviceIdLen];
  char severity[kMaxSeverityLen];
  char message[kMaxMessageLen];
  double value;
  uint64_t trigger_time;
  uint64_t last_update_time;
  uint8_t active;
  char unit[16];
};

constexpr const char* kCatalogTableName = "catalog";
constexpr const char* kPointIndexTableName = "point_index";
constexpr const char* kTelemetryTableName = "telemetry";
constexpr const char* kTeleindicationTableName = "teleindication";
constexpr const char* kCommandTableName = "command";
constexpr const char* kStrategyRuntimeTableName = "strategy_runtime";
constexpr const char* kAlarmActiveTableName = "alarm_active";

inline size_t table_total_size(const TableHeader& header) {
  return static_cast<size_t>(header.header_size)
      + static_cast<size_t>(header.capacity) * static_cast<size_t>(header.row_size);
}

}  // namespace openems::rt_db
