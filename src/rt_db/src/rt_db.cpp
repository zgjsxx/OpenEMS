// src/rt_db/src/rt_db.cpp
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"
#include "openems/utils/time_utils.h"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <chrono>
#include <mutex>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

namespace openems::rt_db {

static uint64_t now_ms() {
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();
}

// ===== Shared memory platform layer =====

static void* shm_create(const std::string& name, size_t size, void** out_handle) {
#ifdef _WIN32
  // Windows: CreateNamedSharedMemory
  HANDLE hMap = CreateFileMappingA(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
      static_cast<DWORD>(size >> 32), static_cast<DWORD>(size & 0xFFFFFFFF),
      name.c_str());
  if (!hMap) return nullptr;
  void* addr = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!addr) { CloseHandle(hMap); return nullptr; }
  *out_handle = hMap;
  return addr;
#else
  // Linux: POSIX shared memory
  int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
  if (fd < 0) return nullptr;
  if (ftruncate(fd, static_cast<off_t>(size)) != 0) { close(fd); return nullptr; }
  void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) { close(fd); return nullptr; }
  *out_handle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
  return addr;
#endif
}

static void* shm_attach(const std::string& name, size_t* out_size, void** out_handle) {
#ifdef _WIN32
  HANDLE hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
  if (!hMap) return nullptr;
  // First map just the header to read total size
  void* hdr_addr = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmHeader));
  if (!hdr_addr) { CloseHandle(hMap); return nullptr; }
  auto* hdr = static_cast<ShmHeader*>(hdr_addr);
  size_t total_size = sizeof(ShmHeader)
      + hdr->total_point_count * sizeof(PointIndexEntry)
      + hdr->telemetry_count * sizeof(TelemetrySlot)
      + hdr->teleindication_count * sizeof(TeleindicationSlot)
      + hdr->command_count * sizeof(CommandSlot);
  UnmapViewOfFile(hdr_addr);

  // Now map the full region
  void* addr = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, total_size);
  if (!addr) { CloseHandle(hMap); return nullptr; }
  *out_size = total_size;
  *out_handle = hMap;
  return addr;
#else
  int fd = shm_open(name.c_str(), O_RDWR, 0666);
  if (fd < 0) return nullptr;
  // Read header first
  ShmHeader hdr;
  if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) { close(fd); return nullptr; }
  size_t total_size = sizeof(ShmHeader)
      + hdr.total_point_count * sizeof(PointIndexEntry)
      + hdr.telemetry_count * sizeof(TelemetrySlot)
      + hdr.teleindication_count * sizeof(TeleindicationSlot)
      + hdr.command_count * sizeof(CommandSlot);
  void* addr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) { close(fd); return nullptr; }
  *out_size = total_size;
  *out_handle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
  return addr;
#endif
}

static void shm_cleanup(void* handle, void* addr, size_t size, bool is_creator) {
#ifdef _WIN32
  if (addr) UnmapViewOfFile(addr);
  if (handle) CloseHandle(static_cast<HANDLE>(handle));
#else
  if (addr) munmap(addr, size);
  int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
  if (fd >= 0) close(fd);
  if (is_creator) shm_unlink("openems_rt_db");
#endif
}

// ===== RtDb implementation =====

RtDb::RtDb(const std::string& shm_name, bool creator)
    : shm_name_(shm_name), is_creator_(creator) {}

RtDb::~RtDb() {
  unmap_memory();
  shm_cleanup(file_handle_, mapped_addr_, mapped_size_, is_creator_);
}

common::Result<RtDb*> RtDb::create(const std::string& shm_name,
                                    uint32_t telemetry_count,
                                    uint32_t teleindication_count,
                                    uint32_t command_count) {
  auto* db = new RtDb(shm_name, true);

  uint32_t total = telemetry_count + teleindication_count;
  size_t shm_size = sizeof(ShmHeader)
      + total * sizeof(PointIndexEntry)
      + telemetry_count * sizeof(TelemetrySlot)
      + teleindication_count * sizeof(TeleindicationSlot)
      + command_count * sizeof(CommandSlot);

  void* handle = nullptr;
  void* addr = shm_create(shm_name, shm_size, &handle);
  if (!addr) {
    delete db;
    return common::Result<RtDb*>::Err(
        common::ErrorCode::ConnectionFailed,
        "Failed to create shared memory: " + shm_name);
  }

  db->file_handle_ = handle;
  db->mapped_addr_ = static_cast<uint8_t*>(addr);
  db->mapped_size_ = shm_size;

  // Initialize header
  std::memset(addr, 0, shm_size);
  auto* hdr = static_cast<ShmHeader*>(addr);
  hdr->magic = kMagic;
  hdr->version = kVersion;
  hdr->telemetry_count = telemetry_count;
  hdr->teleindication_count = teleindication_count;
  hdr->total_point_count = total;
  hdr->command_count = command_count;
  hdr->index_table_offset = sizeof(ShmHeader);
  hdr->telemetry_offset = sizeof(ShmHeader) + total * sizeof(PointIndexEntry);
  hdr->teleindication_offset = hdr->telemetry_offset + telemetry_count * sizeof(TelemetrySlot);
  hdr->command_offset = hdr->teleindication_offset + teleindication_count * sizeof(TeleindicationSlot);
  hdr->update_seq = 0;
  hdr->last_update_time = now_ms();

  db->header_ = hdr;
  db->index_table_ = reinterpret_cast<PointIndexEntry*>(db->mapped_addr_ + hdr->index_table_offset);
  db->telemetry_slots_ = reinterpret_cast<TelemetrySlot*>(db->mapped_addr_ + hdr->telemetry_offset);
  db->teleindication_slots_ = reinterpret_cast<TeleindicationSlot*>(db->mapped_addr_ + hdr->teleindication_offset);
  db->command_slots_ = reinterpret_cast<CommandSlot*>(db->mapped_addr_ + hdr->command_offset);

  OPENEMS_LOG_I("RtDb", "Created shared memory: " + shm_name +
      " size=" + std::to_string(shm_size) +
      " telem=" + std::to_string(telemetry_count) +
      " ti=" + std::to_string(teleindication_count) +
      " cmd=" + std::to_string(command_count));

  return common::Result<RtDb*>::Ok(db);
}

common::Result<RtDb*> RtDb::attach(const std::string& shm_name) {
  auto* db = new RtDb(shm_name, false);

  size_t shm_size = 0;
  void* handle = nullptr;
  void* addr = shm_attach(shm_name, &shm_size, &handle);
  if (!addr) {
    delete db;
    return common::Result<RtDb*>::Err(
        common::ErrorCode::ConnectionFailed,
        "Failed to attach shared memory: " + shm_name);
  }

  db->file_handle_ = handle;
  db->mapped_addr_ = static_cast<uint8_t*>(addr);
  db->mapped_size_ = shm_size;

  auto* hdr = static_cast<ShmHeader*>(addr);
  if (hdr->magic != kMagic || hdr->version != kVersion) {
    shm_cleanup(handle, addr, shm_size, false);
    delete db;
    return common::Result<RtDb*>::Err(
        common::ErrorCode::InvalidConfig,
        "Shared memory header mismatch");
  }

  db->header_ = hdr;
  db->index_table_ = reinterpret_cast<PointIndexEntry*>(db->mapped_addr_ + hdr->index_table_offset);
  db->telemetry_slots_ = reinterpret_cast<TelemetrySlot*>(db->mapped_addr_ + hdr->telemetry_offset);
  db->teleindication_slots_ = reinterpret_cast<TeleindicationSlot*>(db->mapped_addr_ + hdr->teleindication_offset);
  db->command_slots_ = reinterpret_cast<CommandSlot*>(db->mapped_addr_ + hdr->command_offset);

  OPENEMS_LOG_I("RtDb", "Attached shared memory: " + shm_name +
      " telem=" + std::to_string(hdr->telemetry_count) +
      " ti=" + std::to_string(hdr->teleindication_count) +
      " cmd=" + std::to_string(hdr->command_count));

  return common::Result<RtDb*>::Ok(db);
}

void RtDb::unmap_memory() {
#ifdef _WIN32
  if (mapped_addr_) UnmapViewOfFile(mapped_addr_);
  if (file_handle_) CloseHandle(static_cast<HANDLE>(file_handle_));
#else
  if (mapped_addr_) munmap(mapped_addr_, mapped_size_);
  int fd = static_cast<int>(reinterpret_cast<intptr_t>(file_handle_));
  if (fd >= 0) close(fd);
#endif
  mapped_addr_ = nullptr;
  file_handle_ = nullptr;
}

// ===== Point registration =====

common::VoidResult RtDb::register_point(const common::PointId& pid,
                                         const common::DeviceId& did,
                                         uint8_t point_category,
                                         uint8_t data_type,
                                         const std::string& unit,
                                         bool writable) {
  if (!header_) return common::VoidResult::Err(common::ErrorCode::InvalidConfig, "Not initialized");

  std::lock_guard lock(process_mutex_);

  // Find first empty slot in index table
  for (uint32_t i = 0; i < header_->total_point_count; ++i) {
    auto& entry = index_table_[i];
    if (entry.point_id[0] == '\0') {
      // Empty slot — register here
      std::memset(entry.point_id, 0, kMaxPointIdLen);
      std::memset(entry.device_id, 0, kMaxDeviceIdLen);
      std::strncpy(entry.point_id, pid.c_str(), kMaxPointIdLen - 1);
      std::strncpy(entry.device_id, did.c_str(), kMaxDeviceIdLen - 1);
      entry.point_category = point_category;
      entry.data_type = data_type;
      entry.writable = writable ? 1 : 0;
      entry.reserved1 = 0;

      // Compute slot offset based on category
      if (point_category == 0 || point_category == 2 || point_category == 3) {  // Telemetry / Telecontrol / Setting
        uint32_t telem_idx = 0;
        for (uint32_t j = 0; j < i; ++j) {
          if (index_table_[j].point_category == 0 ||
              index_table_[j].point_category == 2 ||
              index_table_[j].point_category == 3) telem_idx++;
        }
        entry.slot_offset = telem_idx * sizeof(TelemetrySlot);
        // Initialize slot
        auto& slot = telemetry_slots_[telem_idx];
        slot.value = 0.0;
        slot.timestamp = 0;
        slot.quality = static_cast<uint8_t>(common::Quality::Bad);
        slot.valid = 0;
      } else if (point_category == 1) {  // Teleindication
        uint32_t ti_idx = 0;
        for (uint32_t j = 0; j < i; ++j) {
          if (index_table_[j].point_category == 1) ti_idx++;
        }
        entry.slot_offset = ti_idx * sizeof(TeleindicationSlot);
        auto& slot = teleindication_slots_[ti_idx];
        slot.state_code = 0;
        slot.timestamp = 0;
        slot.quality = static_cast<uint8_t>(common::Quality::Bad);
        slot.valid = 0;
      }

      std::memset(entry.unit, 0, 8);
      std::strncpy(entry.unit, unit.c_str(), 7);

      OPENEMS_LOG_D("RtDb", "Registered point: " + pid +
          " category=" + std::to_string(point_category) +
          " writable=" + std::to_string(entry.writable) +
          " idx=" + std::to_string(i));
      return common::VoidResult::Ok();
    }
    // Already registered? Check for duplicate
    if (std::strncmp(entry.point_id, pid.c_str(), kMaxPointIdLen) == 0) {
      return common::VoidResult::Ok();  // Already registered, no error
    }
  }

  return common::VoidResult::Err(common::ErrorCode::InvalidArgument, "No empty slots in index table");
}

common::VoidResult RtDb::register_command_point(const common::PointId& pid) {
  if (!header_ || !command_slots_) return common::VoidResult::Err(common::ErrorCode::InvalidConfig, "Not initialized");

  std::lock_guard lock(process_mutex_);

  // Find empty command slot
  for (uint32_t i = 0; i < header_->command_count; ++i) {
    auto& slot = command_slots_[i];
    if (slot.point_id[0] == '\0') {
      std::memset(slot.point_id, 0, kMaxPointIdLen);
      std::strncpy(slot.point_id, pid.c_str(), kMaxPointIdLen - 1);
      slot.status = CommandIdle;
      slot.desired_value = 0.0;
      slot.result_value = 0.0;
      slot.error_code = 0;
      OPENEMS_LOG_D("RtDb", "Registered command point: " + pid + " slot=" + std::to_string(i));
      return common::VoidResult::Ok();
    }
    if (std::strncmp(slot.point_id, pid.c_str(), kMaxPointIdLen) == 0) {
      return common::VoidResult::Ok();  // Already registered
    }
  }
  return common::VoidResult::Err(common::ErrorCode::InvalidArgument, "No empty command slots");
}

int32_t RtDb::find_point_index(const common::PointId& pid) const {
  for (uint32_t i = 0; i < header_->total_point_count; ++i) {
    if (std::strncmp(index_table_[i].point_id, pid.c_str(), kMaxPointIdLen) == 0) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

int32_t RtDb::find_command_slot(const common::PointId& pid) const {
  for (uint32_t i = 0; i < header_->command_count; ++i) {
    if (std::strncmp(command_slots_[i].point_id, pid.c_str(), kMaxPointIdLen) == 0) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

// ===== Write API =====

void RtDb::write_telemetry(const common::PointId& pid,
                            double value,
                            common::Quality quality,
                            bool valid) {
  std::lock_guard lock(process_mutex_);

  int32_t idx = find_point_index(pid);
  if (idx < 0) return;
  auto cat = index_table_[idx].point_category;
  if (cat != 0 && cat != 2 && cat != 3) return;

  // Count which telemetry slot this maps to
  uint32_t telem_idx = 0;
  for (uint32_t j = 0; j < static_cast<uint32_t>(idx); ++j) {
    auto jc = index_table_[j].point_category;
    if (jc == 0 || jc == 2 || jc == 3) telem_idx++;
  }

  // Seqlock write: increment sequence (odd = writing)
  header_->update_seq++;
  header_->update_seq |= 1ULL;  // mark as "writing"

  auto& slot = telemetry_slots_[telem_idx];
  slot.value = value;
  slot.timestamp = now_ms();
  slot.quality = static_cast<uint8_t>(quality);
  slot.valid = valid ? 1 : 0;

  header_->last_update_time = now_ms();
  // Seqlock: increment again (even = done)
  header_->update_seq++;
  // update_seq is now even, meaning write is complete
}

void RtDb::write_teleindication(const common::PointId& pid,
                                 uint16_t state_code,
                                 common::Quality quality,
                                 bool valid) {
  std::lock_guard lock(process_mutex_);

  int32_t idx = find_point_index(pid);
  if (idx < 0 || index_table_[idx].point_category != 1) return;

  uint32_t ti_idx = 0;
  for (uint32_t j = 0; j < static_cast<uint32_t>(idx); ++j) {
    if (index_table_[j].point_category == 1) ti_idx++;
  }

  header_->update_seq++;
  header_->update_seq |= 1ULL;

  auto& slot = teleindication_slots_[ti_idx];
  slot.state_code = state_code;
  slot.timestamp = now_ms();
  slot.quality = static_cast<uint8_t>(quality);
  slot.valid = valid ? 1 : 0;

  header_->last_update_time = now_ms();
  header_->update_seq++;
}

void RtDb::write_telemetry_batch(
    const std::vector<common::PointId>& pids,
    const std::vector<double>& values,
    const std::vector<common::Quality>& qualities) {
  std::lock_guard lock(process_mutex_);

  header_->update_seq++;
  header_->update_seq |= 1ULL;

  for (size_t i = 0; i < pids.size(); ++i) {
    int32_t idx = find_point_index(pids[i]);
    if (idx < 0) continue;
    auto cat = index_table_[idx].point_category;
    if (cat != 0 && cat != 2 && cat != 3) continue;

    uint32_t telem_idx = 0;
    for (uint32_t j = 0; j < static_cast<uint32_t>(idx); ++j) {
      auto jc = index_table_[j].point_category;
      if (jc == 0 || jc == 2 || jc == 3) telem_idx++;
    }

    auto& slot = telemetry_slots_[telem_idx];
    slot.value = values[i];
    slot.timestamp = now_ms();
    slot.quality = static_cast<uint8_t>(qualities[i]);
    slot.valid = 1;
  }

  header_->last_update_time = now_ms();
  header_->update_seq++;
}

void RtDb::write_teleindication_batch(
    const std::vector<common::PointId>& pids,
    const std::vector<uint16_t>& state_codes,
    const std::vector<common::Quality>& qualities) {
  std::lock_guard lock(process_mutex_);

  header_->update_seq++;
  header_->update_seq |= 1ULL;

  for (size_t i = 0; i < pids.size(); ++i) {
    int32_t idx = find_point_index(pids[i]);
    if (idx < 0 || index_table_[idx].point_category != 1) continue;

    uint32_t ti_idx = 0;
    for (uint32_t j = 0; j < static_cast<uint32_t>(idx); ++j) {
      if (index_table_[j].point_category == 1) ti_idx++;
    }

    auto& slot = teleindication_slots_[ti_idx];
    slot.state_code = state_codes[i];
    slot.timestamp = now_ms();
    slot.quality = static_cast<uint8_t>(qualities[i]);
    slot.valid = 1;
  }

  header_->last_update_time = now_ms();
  header_->update_seq++;
}

// ===== Command API =====

common::VoidResult RtDb::submit_command(const common::PointId& pid, double desired_value) {
  std::lock_guard lock(process_mutex_);

  int32_t slot_idx = find_command_slot(pid);
  if (slot_idx < 0) {
    return common::VoidResult::Err(common::ErrorCode::PointNotFound, "Command slot not found: " + pid);
  }

  auto& slot = command_slots_[slot_idx];
  slot.desired_value = desired_value;
  slot.submit_time = now_ms();
  slot.status = CommandPending;
  slot.error_code = 0;
  slot.result_value = 0.0;
  slot.complete_time = 0;

  header_->update_seq++;
  header_->update_seq |= 1ULL;
  header_->last_update_time = now_ms();
  header_->update_seq++;

  OPENEMS_LOG_I("RtDb", "Command submitted: " + pid + " value=" + std::to_string(desired_value));
  return common::VoidResult::Ok();
}

bool RtDb::read_pending_command(common::PointId& out_pid, double& out_value) {
  std::lock_guard lock(process_mutex_);

  for (uint32_t i = 0; i < header_->command_count; ++i) {
    auto& slot = command_slots_[i];
    if (slot.status == CommandPending) {
      out_pid = std::string(slot.point_id, kMaxPointIdLen);
      out_pid.erase(out_pid.find('\0'));
      out_value = slot.desired_value;
      slot.status = CommandExecuting;
      return true;
    }
  }
  return false;
}

void RtDb::complete_command(const common::PointId& pid,
                             CommandStatus status,
                             double result_value,
                             uint8_t error_code) {
  std::lock_guard lock(process_mutex_);

  int32_t slot_idx = find_command_slot(pid);
  if (slot_idx < 0) return;

  header_->update_seq++;
  header_->update_seq |= 1ULL;

  auto& slot = command_slots_[slot_idx];
  slot.status = static_cast<uint8_t>(status);
  slot.result_value = result_value;
  slot.error_code = error_code;
  slot.complete_time = now_ms();

  header_->last_update_time = now_ms();
  header_->update_seq++;

  OPENEMS_LOG_I("RtDb", "Command completed: " + pid +
      " status=" + std::to_string(static_cast<uint8_t>(status)) +
      " result=" + std::to_string(result_value));
}

common::Result<CommandReadResult> RtDb::read_command_status(const common::PointId& pid) {
  int32_t slot_idx = find_command_slot(pid);
  if (slot_idx < 0) {
    return common::Result<CommandReadResult>::Err(
        common::ErrorCode::PointNotFound, "Command slot not found: " + pid);
  }

  auto& slot = command_slots_[slot_idx];
  CommandReadResult result;
  result.desired_value = slot.desired_value;
  result.result_value = slot.result_value;
  result.submit_time = slot.submit_time;
  result.complete_time = slot.complete_time;
  result.status = static_cast<CommandStatus>(slot.status);
  result.error_code = slot.error_code;
  return common::Result<CommandReadResult>::Ok(result);
}

// ===== Read API =====

common::Result<TelemetryReadResult> RtDb::read_telemetry(const common::PointId& pid) {
  int32_t idx = find_point_index(pid);
  if (idx < 0) {
    return common::Result<TelemetryReadResult>::Err(
        common::ErrorCode::PointNotFound, "Point not found: " + pid);
  }
  auto cat = index_table_[idx].point_category;
  if (cat != 0 && cat != 2 && cat != 3) {
    return common::Result<TelemetryReadResult>::Err(
        common::ErrorCode::InvalidArgument, "Point is not telemetry/telecontrol/teleadjust: " + pid);
  }

  uint32_t telem_idx = 0;
  for (uint32_t j = 0; j < static_cast<uint32_t>(idx); ++j) {
    auto jc = index_table_[j].point_category;
    if (jc == 0 || jc == 2 || jc == 3) telem_idx++;
  }

  // Seqlock read: retry if sequence was odd (write in progress) or changed
  uint64_t seq1, seq2;
  TelemetryReadResult result;
  do {
    seq1 = header_->update_seq;
    if (seq1 & 1ULL) continue;  // write in progress, retry

    auto& slot = telemetry_slots_[telem_idx];
    result.value = slot.value;
    result.timestamp = slot.timestamp;
    result.quality = static_cast<common::Quality>(slot.quality);
    result.valid = slot.valid != 0;

    seq2 = header_->update_seq;
  } while (seq1 != seq2);

  return common::Result<TelemetryReadResult>::Ok(result);
}

common::Result<TeleindicationReadResult> RtDb::read_teleindication(const common::PointId& pid) {
  int32_t idx = find_point_index(pid);
  if (idx < 0) {
    return common::Result<TeleindicationReadResult>::Err(
        common::ErrorCode::PointNotFound, "Point not found: " + pid);
  }
  if (index_table_[idx].point_category != 1) {
    return common::Result<TeleindicationReadResult>::Err(
        common::ErrorCode::InvalidArgument, "Point is not teleindication: " + pid);
  }

  uint32_t ti_idx = 0;
  for (uint32_t j = 0; j < static_cast<uint32_t>(idx); ++j) {
    if (index_table_[j].point_category == 1) ti_idx++;
  }

  uint64_t seq1, seq2;
  TeleindicationReadResult result;
  do {
    seq1 = header_->update_seq;
    if (seq1 & 1ULL) continue;

    auto& slot = teleindication_slots_[ti_idx];
    result.state_code = slot.state_code;
    result.timestamp = slot.timestamp;
    result.quality = static_cast<common::Quality>(slot.quality);
    result.valid = slot.valid != 0;

    seq2 = header_->update_seq;
  } while (seq1 != seq2);

  return common::Result<TeleindicationReadResult>::Ok(result);
}

// ===== Snapshot =====

SiteSnapshot RtDb::snapshot() {
  std::lock_guard lock(process_mutex_);

  SiteSnapshot snap;
  snap.site_id = std::string(header_->site_id, kMaxSiteIdLen);
  snap.site_id.erase(snap.site_id.find('\0'));
  snap.site_name = std::string(header_->site_name, kMaxSiteNameLen);
  snap.site_name.erase(snap.site_name.find('\0'));

  uint64_t seq1 = header_->update_seq;
  if (seq1 & 1ULL) {
    // Write in progress, wait briefly
  }

  for (uint32_t i = 0; i < header_->total_point_count; ++i) {
    auto& entry = index_table_[i];
    if (entry.point_id[0] == '\0') continue;

    std::string pid(entry.point_id, kMaxPointIdLen);
    pid.erase(pid.find('\0'));
    std::string did(entry.device_id, kMaxDeviceIdLen);
    did.erase(did.find('\0'));

    snap.point_ids.push_back(pid);
    snap.device_ids.push_back(did);
    snap.point_categories.push_back(entry.point_category);

    auto cat = entry.point_category;
    if (cat == 0 || cat == 2 || cat == 3) {  // Telemetry / Telecontrol / Setting
      uint32_t telem_idx = 0;
      for (uint32_t j = 0; j < i; ++j) {
        auto jc = index_table_[j].point_category;
        if (jc == 0 || jc == 2 || jc == 3) telem_idx++;
      }
      auto& slot = telemetry_slots_[telem_idx];
      snap.telemetry_values.push_back(slot.value);
      snap.qualities.push_back(static_cast<common::Quality>(slot.quality));
      snap.valids.push_back(slot.valid != 0);
      snap.timestamps.push_back(slot.timestamp);
    } else if (cat == 1) {  // Teleindication
      uint32_t ti_idx = 0;
      for (uint32_t j = 0; j < i; ++j) {
        if (index_table_[j].point_category == 1) ti_idx++;
      }
      auto& slot = teleindication_slots_[ti_idx];
      snap.teleindication_values.push_back(slot.state_code);
      snap.qualities.push_back(static_cast<common::Quality>(slot.quality));
      snap.valids.push_back(slot.valid != 0);
      snap.timestamps.push_back(slot.timestamp);
    }
  }

  snap.snapshot_time = now_ms();
  return snap;
}

std::string SiteSnapshot::to_string() const {
  std::ostringstream oss;
  oss << "=== Site[" << site_id << "] " << site_name << " ===" << "\n";

  for (size_t i = 0; i < point_ids.size(); ++i) {
    oss << "  [" << point_ids[i] << "] "
        << "dev=" << device_ids[i] << " ";

    auto cat = point_categories[i];
    if (cat == 0 || cat == 2 || cat == 3) {
      // Telemetry / Telecontrol / Setting
      size_t telem_idx = 0;
      for (size_t j = 0; j <= i; ++j) {
        if ((point_categories[j] == 0 || point_categories[j] == 2 || point_categories[j] == 3) && j < i) telem_idx++;
      }
      oss << "TELEM val=" << telemetry_values[telem_idx];
    } else {
      // Teleindication
      size_t ti_idx = 0;
      for (size_t j = 0; j <= i; ++j) {
        if (point_categories[j] == 1 && j < i) ti_idx++;
      }
      oss << "TI state=" << teleindication_values[ti_idx];
    }

    oss << " Q=" << static_cast<int>(qualities[i])
        << " valid=" << valids[i]
        << " ts=" << timestamps[i] << "\n";
  }

  return oss.str();
}

void RtDb::print_snapshot() {
  auto snap = snapshot();
  std::cout << snap.to_string() << std::flush;
}

uint32_t RtDb::telemetry_count() const { return header_ ? header_->telemetry_count : 0; }
uint32_t RtDb::teleindication_count() const { return header_ ? header_->teleindication_count : 0; }
uint32_t RtDb::total_point_count() const { return header_ ? header_->total_point_count : 0; }
uint32_t RtDb::command_count() const { return header_ ? header_->command_count : 0; }
uint64_t RtDb::update_sequence() const { return header_ ? header_->update_seq : 0; }

} // namespace openems::rt_db
