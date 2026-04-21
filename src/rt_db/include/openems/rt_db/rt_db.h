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

class RtDb {
public:
  // Create shared memory region (collector process)
  static common::Result<RtDb*> create(const std::string& shm_name,
                                       uint32_t telemetry_count,
                                       uint32_t teleindication_count,
                                       uint32_t command_count);

  // Attach to existing shared memory (other processes)
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

  // Platform-specific handles stored as void* for portability
  void* shm_handle_ = nullptr;
  void* file_handle_ = nullptr;
  uint8_t* mapped_addr_ = nullptr;
  size_t mapped_size_ = 0;

  // Pointers into mapped memory (set after map)
  ShmHeader* header_ = nullptr;
  PointIndexEntry* index_table_ = nullptr;
  TelemetrySlot* telemetry_slots_ = nullptr;
  TeleindicationSlot* teleindication_slots_ = nullptr;
  CommandSlot* command_slots_ = nullptr;

  // Lock for in-process access to the shared memory
  // Cross-process locking is handled via update_seq (seqlock pattern)
  std::mutex process_mutex_;
};

} // namespace openems::rt_db