#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>
#include "openems/common/types.h"
#include "openems/common/result.h"
#include "openems/rt_db/rt_db_layout.h"

namespace openems::rt_db {

std::string default_shm_name();

// Read result for a single point
struct TelemetryReadResult {
  double   value;
  uint64_t timestamp;  // epoch ms
  common::Quality quality;
  bool valid;
};

struct TeleindicationReadResult {
  uint16_t state_code;
  uint64_t timestamp;
  common::Quality quality;
  bool valid;
};

// Command read result
struct CommandReadResult {
  double   desired_value;
  double   result_value;
  uint64_t submit_time;
  uint64_t complete_time;
  CommandStatus status;
  uint8_t error_code;
};

// Snapshot of all telemetry values
struct TelemetrySnapshot {
  std::vector<TelemetryReadResult> values;
  uint64_t snapshot_time;  // epoch ms when snapshot was taken
};

struct TeleindicationSnapshot {
  std::vector<TeleindicationReadResult> values;
  uint64_t snapshot_time;
};

struct SiteSnapshot {
  common::SiteId site_id;
  std::string site_name;
  uint64_t snapshot_time = 0;  // epoch ms
  std::vector<std::string> point_ids;
  std::vector<std::string> device_ids;
  std::vector<uint8_t> point_categories;  // 0=telemetry, 1=teleindication, 2=telecontrol, 3=setting
  std::vector<double> telemetry_values;
  std::vector<uint16_t> teleindication_values;
  std::vector<common::Quality> qualities;
  std::vector<bool> valids;
  std::vector<uint64_t> timestamps;
  std::string to_string() const;
};

struct TableInfo {
  uint16_t table_id = 0;
  std::string table_name;
  std::string shm_name;
  uint32_t row_size = 0;
  uint32_t capacity = 0;
  uint32_t row_count = 0;
  uint32_t flags = 0;
};

struct TableView {
  TableInfo info;
  TableHeader* header = nullptr;
  void* rows = nullptr;
};

struct StrategyRuntimeRecord {
  std::string strategy_id;
  std::string target_point_id;
  double target_value = 0.0;
  bool suppressed = false;
  std::string suppress_reason;
  std::string last_error;
  uint64_t update_time = 0;
};

struct AlarmActiveRecord {
  std::string alarm_id;
  std::string point_id;
  std::string device_id;
  std::string severity;
  std::string message;
  double value = 0.0;
  std::string unit;
  uint64_t trigger_time = 0;
  uint64_t last_update_time = 0;
  bool active = true;
};

class RtDb {
public:
  // 由 RtDb service 进程调用，创建整个共享内存表空间。
  // 该接口会一次性创建 catalog、point_index、telemetry、
  // teleindication、command 以及运行态镜像表，并初始化表头。
  static common::Result<RtDb*> create(const std::string& shm_name,
                                       uint32_t telemetry_count,
                                       uint32_t teleindication_count,
                                       uint32_t command_count,
                                       uint32_t strategy_runtime_count = 64,
                                       uint32_t alarm_active_count = 256);

  // 由其他进程调用，attach 已存在的共享内存表空间。
  static common::Result<RtDb*> attach(const std::string& shm_name);

  ~RtDb();

  // ---- Write API (collector process) ----

  // Register a point in the index table (must be called before write)
  common::VoidResult register_point(const common::PointId& pid,
                                    const common::DeviceId& did,
                                    uint8_t point_category,
                                    uint8_t data_type,
                                    const std::string& unit,
                                    bool writable = false);

  // Register a command slot for a writable point
  common::VoidResult register_command_point(const common::PointId& pid);

  void set_site_info(const common::SiteId& site_id, const std::string& site_name);

  // Write telemetry value (categories 0, 2, 3)
  void write_telemetry(const common::PointId& pid,
                       double value,
                       common::Quality quality,
                       bool valid);

  // Write teleindication state
  void write_teleindication(const common::PointId& pid,
                            uint16_t state_code,
                            common::Quality quality,
                            bool valid);

  // Batch write telemetry (reduces lock overhead)
  void write_telemetry_batch(const std::vector<common::PointId>& pids,
                             const std::vector<double>& values,
                             const std::vector<common::Quality>& qualities);

  // Batch write teleindication
  void write_teleindication_batch(const std::vector<common::PointId>& pids,
                                  const std::vector<uint16_t>& state_codes,
                                  const std::vector<common::Quality>& qualities);

  // ---- Command API (cross-process command delivery) ----

  // Submit a command: sets slot to Pending with desired_value
  common::VoidResult submit_command(const common::PointId& pid, double desired_value);

  // Read next pending command (for collector ControlService)
  // Returns true if a pending command was found
  bool read_pending_command(common::PointId& out_pid, double& out_value);
  bool read_pending_command_for_points(const std::vector<common::PointId>& point_ids,
                                       common::PointId& out_pid,
                                       double& out_value);

  // Complete a command: set status and result
  void complete_command(const common::PointId& pid,
                        CommandStatus status,
                        double result_value,
                        uint8_t error_code);

  // Read command status for a point
  common::Result<CommandReadResult> read_command_status(const common::PointId& pid);

  // ---- Read API (all processes) ----

  common::Result<TelemetryReadResult> read_telemetry(const common::PointId& pid);
  common::Result<TeleindicationReadResult> read_teleindication(const common::PointId& pid);

  // Full site snapshot (copies entire data section)
  SiteSnapshot snapshot();

  // ---- Info ----

  uint32_t telemetry_count() const;
  uint32_t teleindication_count() const;
  uint32_t total_point_count() const;
  uint32_t command_count() const;
  uint64_t update_sequence() const;
  bool is_creator() const { return is_creator_; }
  std::vector<TableInfo> list_tables() const;
  // 按逻辑表名从 catalog 目录表中查询共享内存表的元信息。
  // 这里只返回表描述信息，例如 shm_name、row_size、capacity、
  // row_count 和 flags，不会读取表内业务行数据。
  common::Result<TableInfo> get_table_info(const std::string& table_name) const;
  // 按表号从 catalog 目录表中查询共享内存表的元信息。
  // 这是共享内存表空间中的“元数据查询”接口，常用于 open_table(table_id)
  // 前的表发现、诊断和调试。
  common::Result<TableInfo> get_table_info(uint16_t table_id) const;
  common::Result<TableView> open_table(const std::string& table_name);
  common::Result<TableView> open_table(uint16_t table_id);

  common::VoidResult upsert_strategy_runtime(const StrategyRuntimeRecord& record);
  std::vector<StrategyRuntimeRecord> read_strategy_runtime() const;
  common::VoidResult replace_active_alarms(const std::vector<AlarmActiveRecord>& alarms);
  std::vector<AlarmActiveRecord> read_active_alarms() const;

  // Print snapshot to stdout
  void print_snapshot();

private:
  RtDb(const std::string& shm_name, bool creator);

  common::VoidResult init_memory(uint32_t telem_count, uint32_t ti_count);
  common::VoidResult map_memory();
  void unmap_memory();

  // Find point index by PointId
  int32_t find_point_index(const common::PointId& pid) const;

  // Find command slot index by PointId
  int32_t find_command_slot(const common::PointId& pid) const;

  std::string shm_name_;
  bool is_creator_;

  struct Segment {
    std::string shm_name;
    void* handle = nullptr;
    uint8_t* mapped_addr = nullptr;
    size_t mapped_size = 0;
  };

  Segment catalog_segment_;
  Segment point_index_segment_;
  Segment telemetry_segment_;
  Segment teleindication_segment_;
  Segment command_segment_;
  Segment strategy_runtime_segment_;
  Segment alarm_active_segment_;

  TableHeader* catalog_header_ = nullptr;
  CatalogEntry* catalog_entries_ = nullptr;
  PointIndexHeader* point_index_header_ = nullptr;
  PointIndexEntry* index_table_ = nullptr;
  TableHeader* telemetry_header_ = nullptr;
  TelemetrySlot* telemetry_slots_ = nullptr;
  TableHeader* teleindication_header_ = nullptr;
  TeleindicationSlot* teleindication_slots_ = nullptr;
  TableHeader* command_header_ = nullptr;
  CommandSlot* command_slots_ = nullptr;
  TableHeader* strategy_runtime_header_ = nullptr;
  StrategyRuntimeSlot* strategy_runtime_slots_ = nullptr;
  TableHeader* alarm_active_header_ = nullptr;
  AlarmActiveSlot* alarm_active_slots_ = nullptr;

  // Lock for in-process access to the shared memory
  // Cross-process locking is handled via update_seq (seqlock pattern)
  std::mutex process_mutex_;
};

} // namespace openems::rt_db
