#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  constexpr DWORD kFileMapAllAccess = FILE_MAP_ALL_ACCESS;
  const HANDLE kInvalidHandleValue = INVALID_HANDLE_VALUE;
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace openems::rt_db {

namespace {

uint64_t now_ms() {
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

std::string trim_fixed_string(const char* value, size_t size) {
  std::string out(value, size);
  const size_t zero_pos = out.find('\0');
  if (zero_pos != std::string::npos) {
    out.resize(zero_pos);
  }
  return out;
}

void copy_fixed_string(char* dest, size_t size, const std::string& value) {
  std::memset(dest, 0, size);
  if (size == 0) {
    return;
  }
  std::strncpy(dest, value.c_str(), size - 1);
}

std::string table_shm_name(const std::string& base_name, const std::string& table_name) {
  return base_name + "_" + table_name;
}

void* shm_create_region(const std::string& name, size_t size, void** out_handle) {
#ifdef _WIN32
  HANDLE hmap = CreateFileMappingA(
      kInvalidHandleValue,
      nullptr,
      PAGE_READWRITE,
      static_cast<DWORD>(size >> 32),
      static_cast<DWORD>(size & 0xFFFFFFFF),
      name.c_str());
  if (!hmap) {
    return nullptr;
  }
  void* addr = MapViewOfFile(hmap, kFileMapAllAccess, 0, 0, size);
  if (!addr) {
    CloseHandle(hmap);
    return nullptr;
  }
  *out_handle = hmap;
  return addr;
#else
  int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
  if (fd < 0) {
    return nullptr;
  }
  if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
    close(fd);
    return nullptr;
  }
  void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    close(fd);
    return nullptr;
  }
  *out_handle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
  return addr;
#endif
}

void* shm_attach_region(const std::string& name, size_t* out_size, void** out_handle) {
#ifdef _WIN32
  HANDLE hmap = OpenFileMappingA(kFileMapAllAccess, FALSE, name.c_str());
  if (!hmap) {
    return nullptr;
  }
  void* hdr_addr = MapViewOfFile(hmap, kFileMapAllAccess, 0, 0, sizeof(TableHeader));
  if (!hdr_addr) {
    CloseHandle(hmap);
    return nullptr;
  }
  auto* header = static_cast<TableHeader*>(hdr_addr);
  const size_t total_size = table_total_size(*header);
  UnmapViewOfFile(hdr_addr);

  void* addr = MapViewOfFile(hmap, kFileMapAllAccess, 0, 0, total_size);
  if (!addr) {
    CloseHandle(hmap);
    return nullptr;
  }
  *out_size = total_size;
  *out_handle = hmap;
  return addr;
#else
  int fd = shm_open(name.c_str(), O_RDWR, 0666);
  if (fd < 0) {
    return nullptr;
  }

  TableHeader header{};
  if (pread(fd, &header, sizeof(header), 0) != static_cast<ssize_t>(sizeof(header))) {
    close(fd);
    return nullptr;
  }
  const size_t total_size = table_total_size(header);
  void* addr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    close(fd);
    return nullptr;
  }
  *out_size = total_size;
  *out_handle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
  return addr;
#endif
}

void shm_cleanup_region(void* handle, void* addr, size_t size, bool is_creator, const std::string& name) {
#ifdef _WIN32
  if (addr) {
    UnmapViewOfFile(addr);
  }
  if (handle) {
    CloseHandle(static_cast<HANDLE>(handle));
  }
#else
  if (addr) {
    munmap(addr, size);
  }
  if (handle) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
    if (fd >= 0) {
      close(fd);
    }
  }
  if (is_creator && !name.empty()) {
    shm_unlink(name.c_str());
  }
#endif
}

void init_table_header(
    TableHeader* header,
    uint16_t table_id,
    const std::string& table_name,
    uint32_t header_size,
    uint32_t row_size,
    uint32_t capacity,
    uint32_t flags = 0) {
  std::memset(header, 0, header_size);
  header->magic = kTableMagic;
  header->version = kTableVersion;
  header->table_id = table_id;
  header->header_size = header_size;
  header->row_size = row_size;
  header->capacity = capacity;
  header->row_count = 0;
  header->flags = flags;
  header->last_update_time = now_ms();
  header->update_seq = 0;
  copy_fixed_string(header->table_name, sizeof(header->table_name), table_name);
}

CatalogEntry* find_catalog_entry(CatalogEntry* entries, uint32_t row_count, uint16_t table_id) {
  for (uint32_t i = 0; i < row_count; ++i) {
    if (entries[i].table_id == table_id) {
      return &entries[i];
    }
  }
  return nullptr;
}

const CatalogEntry* find_catalog_entry(
    const CatalogEntry* entries, uint32_t row_count, const std::string& table_name) {
  for (uint32_t i = 0; i < row_count; ++i) {
    if (trim_fixed_string(entries[i].table_name, sizeof(entries[i].table_name)) == table_name) {
      return &entries[i];
    }
  }
  return nullptr;
}

void populate_catalog_entry(
    CatalogEntry* entry,
    uint16_t table_id,
    const std::string& table_name,
    const std::string& shm_name,
    const TableHeader* header) {
  std::memset(entry, 0, sizeof(CatalogEntry));
  entry->table_id = table_id;
  entry->row_size = header->row_size;
  entry->capacity = header->capacity;
  entry->row_count = header->row_count;
  entry->flags = header->flags;
  copy_fixed_string(entry->table_name, sizeof(entry->table_name), table_name);
  copy_fixed_string(entry->shm_name, sizeof(entry->shm_name), shm_name);
}

void update_table_header_on_write(TableHeader* header) {
  header->update_seq++;
  header->update_seq |= 1ULL;
  header->last_update_time = now_ms();
}

void finish_table_header_write(TableHeader* header) {
  header->last_update_time = now_ms();
  header->update_seq++;
}

}  // namespace

std::string default_shm_name() {
#ifdef _WIN32
  return "Local\\openems_rt_db";
#else
  return "/openems_rt_db";
#endif
}

RtDb::RtDb(const std::string& shm_name, bool creator)
    : shm_name_(shm_name), is_creator_(creator) {}

RtDb::~RtDb() {
  shm_cleanup_region(
      alarm_active_segment_.handle,
      alarm_active_segment_.mapped_addr,
      alarm_active_segment_.mapped_size,
      is_creator_,
      alarm_active_segment_.shm_name);
  shm_cleanup_region(
      strategy_runtime_segment_.handle,
      strategy_runtime_segment_.mapped_addr,
      strategy_runtime_segment_.mapped_size,
      is_creator_,
      strategy_runtime_segment_.shm_name);
  shm_cleanup_region(
      command_segment_.handle,
      command_segment_.mapped_addr,
      command_segment_.mapped_size,
      is_creator_,
      command_segment_.shm_name);
  shm_cleanup_region(
      teleindication_segment_.handle,
      teleindication_segment_.mapped_addr,
      teleindication_segment_.mapped_size,
      is_creator_,
      teleindication_segment_.shm_name);
  shm_cleanup_region(
      telemetry_segment_.handle,
      telemetry_segment_.mapped_addr,
      telemetry_segment_.mapped_size,
      is_creator_,
      telemetry_segment_.shm_name);
  shm_cleanup_region(
      point_index_segment_.handle,
      point_index_segment_.mapped_addr,
      point_index_segment_.mapped_size,
      is_creator_,
      point_index_segment_.shm_name);
  shm_cleanup_region(
      catalog_segment_.handle,
      catalog_segment_.mapped_addr,
      catalog_segment_.mapped_size,
      is_creator_,
      catalog_segment_.shm_name);
}

common::Result<RtDb*> RtDb::create(
    const std::string& shm_name,
    uint32_t telemetry_count,
    uint32_t teleindication_count,
    uint32_t command_count,
    uint32_t strategy_runtime_count,
    uint32_t alarm_active_count) {
  auto* db = new RtDb(shm_name, true);

  db->catalog_segment_.shm_name = table_shm_name(shm_name, kCatalogTableName);
  db->point_index_segment_.shm_name = table_shm_name(shm_name, kPointIndexTableName);
  db->telemetry_segment_.shm_name = table_shm_name(shm_name, kTelemetryTableName);
  db->teleindication_segment_.shm_name = table_shm_name(shm_name, kTeleindicationTableName);
  db->command_segment_.shm_name = table_shm_name(shm_name, kCommandTableName);
  db->strategy_runtime_segment_.shm_name = table_shm_name(shm_name, kStrategyRuntimeTableName);
  db->alarm_active_segment_.shm_name = table_shm_name(shm_name, kAlarmActiveTableName);

  const uint32_t total_point_count = telemetry_count + teleindication_count;
  const size_t catalog_size = sizeof(TableHeader) + 7 * sizeof(CatalogEntry);
  const size_t point_index_size =
      sizeof(PointIndexHeader) + static_cast<size_t>(total_point_count) * sizeof(PointIndexEntry);
  const size_t telemetry_size =
      sizeof(TableHeader) + static_cast<size_t>(telemetry_count) * sizeof(TelemetrySlot);
  const size_t teleindication_size =
      sizeof(TableHeader) + static_cast<size_t>(teleindication_count) * sizeof(TeleindicationSlot);
  const size_t command_size =
      sizeof(TableHeader) + static_cast<size_t>(command_count) * sizeof(CommandSlot);
  const size_t strategy_runtime_size =
      sizeof(TableHeader) + static_cast<size_t>(strategy_runtime_count) * sizeof(StrategyRuntimeSlot);
  const size_t alarm_active_size =
      sizeof(TableHeader) + static_cast<size_t>(alarm_active_count) * sizeof(AlarmActiveSlot);

  auto create_segment = [&](RtDb::Segment& segment, size_t size) -> bool {
    void* handle = nullptr;
    void* addr = shm_create_region(segment.shm_name, size, &handle);
    if (!addr) {
      return false;
    }
    segment.handle = handle;
    segment.mapped_addr = static_cast<uint8_t*>(addr);
    segment.mapped_size = size;
    std::memset(addr, 0, size);
    return true;
  };

  if (!create_segment(db->catalog_segment_, catalog_size) ||
      !create_segment(db->point_index_segment_, point_index_size) ||
      !create_segment(db->telemetry_segment_, telemetry_size) ||
      !create_segment(db->teleindication_segment_, teleindication_size) ||
      !create_segment(db->command_segment_, command_size) ||
      !create_segment(db->strategy_runtime_segment_, strategy_runtime_size) ||
      !create_segment(db->alarm_active_segment_, alarm_active_size)) {
    delete db;
    return common::Result<RtDb*>::Err(
        common::ErrorCode::ConnectionFailed,
        "Failed to create RtDb table segments");
  }

  db->catalog_header_ = reinterpret_cast<TableHeader*>(db->catalog_segment_.mapped_addr);
  db->catalog_entries_ = reinterpret_cast<CatalogEntry*>(
      db->catalog_segment_.mapped_addr + sizeof(TableHeader));
  init_table_header(
      db->catalog_header_,
      TableCatalog,
      kCatalogTableName,
      static_cast<uint32_t>(sizeof(TableHeader)),
      static_cast<uint32_t>(sizeof(CatalogEntry)),
      7);
  db->catalog_header_->row_count = 7;

  db->point_index_header_ = reinterpret_cast<PointIndexHeader*>(db->point_index_segment_.mapped_addr);
  init_table_header(
      &db->point_index_header_->base,
      TablePointIndex,
      kPointIndexTableName,
      static_cast<uint32_t>(sizeof(PointIndexHeader)),
      static_cast<uint32_t>(sizeof(PointIndexEntry)),
      total_point_count);
  db->point_index_header_->telemetry_capacity = telemetry_count;
  db->point_index_header_->teleindication_capacity = teleindication_count;
  db->point_index_header_->command_capacity = command_count;
  db->index_table_ = reinterpret_cast<PointIndexEntry*>(
      db->point_index_segment_.mapped_addr + sizeof(PointIndexHeader));

  db->telemetry_header_ = reinterpret_cast<TableHeader*>(db->telemetry_segment_.mapped_addr);
  init_table_header(
      db->telemetry_header_,
      TableTelemetry,
      kTelemetryTableName,
      static_cast<uint32_t>(sizeof(TableHeader)),
      static_cast<uint32_t>(sizeof(TelemetrySlot)),
      telemetry_count);
  db->telemetry_slots_ = reinterpret_cast<TelemetrySlot*>(
      db->telemetry_segment_.mapped_addr + sizeof(TableHeader));

  db->teleindication_header_ =
      reinterpret_cast<TableHeader*>(db->teleindication_segment_.mapped_addr);
  init_table_header(
      db->teleindication_header_,
      TableTeleindication,
      kTeleindicationTableName,
      static_cast<uint32_t>(sizeof(TableHeader)),
      static_cast<uint32_t>(sizeof(TeleindicationSlot)),
      teleindication_count);
  db->teleindication_slots_ = reinterpret_cast<TeleindicationSlot*>(
      db->teleindication_segment_.mapped_addr + sizeof(TableHeader));

  db->command_header_ = reinterpret_cast<TableHeader*>(db->command_segment_.mapped_addr);
  init_table_header(
      db->command_header_,
      TableCommand,
      kCommandTableName,
      static_cast<uint32_t>(sizeof(TableHeader)),
      static_cast<uint32_t>(sizeof(CommandSlot)),
      command_count);
  db->command_slots_ = reinterpret_cast<CommandSlot*>(
      db->command_segment_.mapped_addr + sizeof(TableHeader));

  db->strategy_runtime_header_ =
      reinterpret_cast<TableHeader*>(db->strategy_runtime_segment_.mapped_addr);
  init_table_header(
      db->strategy_runtime_header_,
      TableStrategyRuntime,
      kStrategyRuntimeTableName,
      static_cast<uint32_t>(sizeof(TableHeader)),
      static_cast<uint32_t>(sizeof(StrategyRuntimeSlot)),
      strategy_runtime_count);
  db->strategy_runtime_slots_ = reinterpret_cast<StrategyRuntimeSlot*>(
      db->strategy_runtime_segment_.mapped_addr + sizeof(TableHeader));

  db->alarm_active_header_ =
      reinterpret_cast<TableHeader*>(db->alarm_active_segment_.mapped_addr);
  init_table_header(
      db->alarm_active_header_,
      TableAlarmActive,
      kAlarmActiveTableName,
      static_cast<uint32_t>(sizeof(TableHeader)),
      static_cast<uint32_t>(sizeof(AlarmActiveSlot)),
      alarm_active_count);
  db->alarm_active_slots_ = reinterpret_cast<AlarmActiveSlot*>(
      db->alarm_active_segment_.mapped_addr + sizeof(TableHeader));

  populate_catalog_entry(
      &db->catalog_entries_[0],
      TableCatalog,
      kCatalogTableName,
      db->catalog_segment_.shm_name,
      db->catalog_header_);
  populate_catalog_entry(
      &db->catalog_entries_[1],
      TablePointIndex,
      kPointIndexTableName,
      db->point_index_segment_.shm_name,
      &db->point_index_header_->base);
  populate_catalog_entry(
      &db->catalog_entries_[2],
      TableTelemetry,
      kTelemetryTableName,
      db->telemetry_segment_.shm_name,
      db->telemetry_header_);
  populate_catalog_entry(
      &db->catalog_entries_[3],
      TableTeleindication,
      kTeleindicationTableName,
      db->teleindication_segment_.shm_name,
      db->teleindication_header_);
  populate_catalog_entry(
      &db->catalog_entries_[4],
      TableCommand,
      kCommandTableName,
      db->command_segment_.shm_name,
      db->command_header_);
  populate_catalog_entry(
      &db->catalog_entries_[5],
      TableStrategyRuntime,
      kStrategyRuntimeTableName,
      db->strategy_runtime_segment_.shm_name,
      db->strategy_runtime_header_);
  populate_catalog_entry(
      &db->catalog_entries_[6],
      TableAlarmActive,
      kAlarmActiveTableName,
      db->alarm_active_segment_.shm_name,
      db->alarm_active_header_);

  OPENEMS_LOG_I(
      "RtDb",
      "Created RtDb table space: base=" + shm_name +
          " tables=7 telem=" + std::to_string(telemetry_count) +
          " ti=" + std::to_string(teleindication_count) +
          " cmd=" + std::to_string(command_count) +
          " strategy=" + std::to_string(strategy_runtime_count) +
          " alarm=" + std::to_string(alarm_active_count));

  return common::Result<RtDb*>::Ok(db);
}

common::Result<RtDb*> RtDb::attach(const std::string& shm_name) {
  auto* db = new RtDb(shm_name, false);
  db->catalog_segment_.shm_name = table_shm_name(shm_name, kCatalogTableName);

  void* catalog_handle = nullptr;
  size_t catalog_size = 0;
  void* catalog_addr = shm_attach_region(db->catalog_segment_.shm_name, &catalog_size, &catalog_handle);
  if (!catalog_addr) {
    delete db;
    return common::Result<RtDb*>::Err(
        common::ErrorCode::ConnectionFailed,
        "Failed to attach catalog shared memory: " + db->catalog_segment_.shm_name);
  }
  db->catalog_segment_.handle = catalog_handle;
  db->catalog_segment_.mapped_addr = static_cast<uint8_t*>(catalog_addr);
  db->catalog_segment_.mapped_size = catalog_size;
  db->catalog_header_ = reinterpret_cast<TableHeader*>(catalog_addr);
  db->catalog_entries_ = reinterpret_cast<CatalogEntry*>(
      db->catalog_segment_.mapped_addr + db->catalog_header_->header_size);

  if (db->catalog_header_->magic != kTableMagic ||
      db->catalog_header_->version != kTableVersion ||
      trim_fixed_string(db->catalog_header_->table_name, sizeof(db->catalog_header_->table_name)) !=
          kCatalogTableName) {
    delete db;
    return common::Result<RtDb*>::Err(
        common::ErrorCode::InvalidConfig,
        "Invalid RtDb catalog shared memory");
  }

  auto attach_segment_from_catalog = [&](uint16_t table_id, RtDb::Segment& segment) -> bool {
    CatalogEntry* entry = find_catalog_entry(db->catalog_entries_, db->catalog_header_->row_count, table_id);
    if (!entry) {
      return false;
    }
    segment.shm_name = trim_fixed_string(entry->shm_name, sizeof(entry->shm_name));
    void* handle = nullptr;
    size_t size = 0;
    void* addr = shm_attach_region(segment.shm_name, &size, &handle);
    if (!addr) {
      return false;
    }
    segment.handle = handle;
    segment.mapped_addr = static_cast<uint8_t*>(addr);
    segment.mapped_size = size;
    return true;
  };

  if (!attach_segment_from_catalog(TablePointIndex, db->point_index_segment_) ||
      !attach_segment_from_catalog(TableTelemetry, db->telemetry_segment_) ||
      !attach_segment_from_catalog(TableTeleindication, db->teleindication_segment_) ||
      !attach_segment_from_catalog(TableCommand, db->command_segment_) ||
      !attach_segment_from_catalog(TableStrategyRuntime, db->strategy_runtime_segment_) ||
      !attach_segment_from_catalog(TableAlarmActive, db->alarm_active_segment_)) {
    delete db;
    return common::Result<RtDb*>::Err(
        common::ErrorCode::ConnectionFailed,
        "Failed to attach one or more RtDb table segments");
  }

  db->point_index_header_ = reinterpret_cast<PointIndexHeader*>(db->point_index_segment_.mapped_addr);
  db->index_table_ = reinterpret_cast<PointIndexEntry*>(
      db->point_index_segment_.mapped_addr + db->point_index_header_->base.header_size);
  db->telemetry_header_ = reinterpret_cast<TableHeader*>(db->telemetry_segment_.mapped_addr);
  db->telemetry_slots_ = reinterpret_cast<TelemetrySlot*>(
      db->telemetry_segment_.mapped_addr + db->telemetry_header_->header_size);
  db->teleindication_header_ =
      reinterpret_cast<TableHeader*>(db->teleindication_segment_.mapped_addr);
  db->teleindication_slots_ = reinterpret_cast<TeleindicationSlot*>(
      db->teleindication_segment_.mapped_addr + db->teleindication_header_->header_size);
  db->command_header_ = reinterpret_cast<TableHeader*>(db->command_segment_.mapped_addr);
  db->command_slots_ = reinterpret_cast<CommandSlot*>(
      db->command_segment_.mapped_addr + db->command_header_->header_size);
  db->strategy_runtime_header_ =
      reinterpret_cast<TableHeader*>(db->strategy_runtime_segment_.mapped_addr);
  db->strategy_runtime_slots_ = reinterpret_cast<StrategyRuntimeSlot*>(
      db->strategy_runtime_segment_.mapped_addr + db->strategy_runtime_header_->header_size);
  db->alarm_active_header_ =
      reinterpret_cast<TableHeader*>(db->alarm_active_segment_.mapped_addr);
  db->alarm_active_slots_ = reinterpret_cast<AlarmActiveSlot*>(
      db->alarm_active_segment_.mapped_addr + db->alarm_active_header_->header_size);

  OPENEMS_LOG_I(
      "RtDb",
      "Attached RtDb table space: base=" + shm_name +
          " telem=" + std::to_string(db->telemetry_header_->row_count) +
          " ti=" + std::to_string(db->teleindication_header_->row_count) +
          " cmd=" + std::to_string(db->command_header_->row_count) +
          " strategy=" + std::to_string(db->strategy_runtime_header_->row_count) +
          " alarm=" + std::to_string(db->alarm_active_header_->row_count));

  return common::Result<RtDb*>::Ok(db);
}

void RtDb::unmap_memory() {}

common::VoidResult RtDb::register_point(
    const common::PointId& pid,
    const common::DeviceId& did,
    uint8_t point_category,
    uint8_t data_type,
    const std::string& unit,
    bool writable) {
  if (!point_index_header_) {
    return common::VoidResult::Err(common::ErrorCode::InvalidConfig, "Not initialized");
  }

  std::lock_guard lock(process_mutex_);

  for (uint32_t i = 0; i < point_index_header_->base.capacity; ++i) {
    auto& entry = index_table_[i];
    if (std::strncmp(entry.point_id, pid.c_str(), kMaxPointIdLen) == 0) {
      return common::VoidResult::Ok();
    }
  }

  for (uint32_t i = 0; i < point_index_header_->base.capacity; ++i) {
    auto& entry = index_table_[i];
    if (entry.point_id[0] != '\0') {
      continue;
    }

    std::memset(&entry, 0, sizeof(entry));
    copy_fixed_string(entry.point_id, sizeof(entry.point_id), pid);
    copy_fixed_string(entry.device_id, sizeof(entry.device_id), did);
    entry.point_category = point_category;
    entry.data_type = data_type;
    entry.writable = writable ? 1 : 0;
    entry.telemetry_slot_index = kInvalidSlotIndex;
    entry.teleindication_slot_index = kInvalidSlotIndex;
    entry.command_slot_index = kInvalidSlotIndex;
    copy_fixed_string(entry.unit, sizeof(entry.unit), unit);

    if (point_category == 1) {
      if (point_index_header_->teleindication_used >= point_index_header_->teleindication_capacity) {
        return common::VoidResult::Err(
            common::ErrorCode::InvalidArgument, "No free teleindication slots");
      }
      const uint32_t slot_index = point_index_header_->teleindication_used++;
      entry.teleindication_slot_index = slot_index;
      auto& slot = teleindication_slots_[slot_index];
      std::memset(&slot, 0, sizeof(slot));
      slot.quality = static_cast<uint8_t>(common::Quality::Bad);
      teleindication_header_->row_count = point_index_header_->teleindication_used;
      if (CatalogEntry* catalog = find_catalog_entry(
              catalog_entries_, catalog_header_->row_count, TableTeleindication)) {
        catalog->row_count = teleindication_header_->row_count;
      }
    } else {
      if (point_index_header_->telemetry_used >= point_index_header_->telemetry_capacity) {
        return common::VoidResult::Err(common::ErrorCode::InvalidArgument, "No free telemetry slots");
      }
      const uint32_t slot_index = point_index_header_->telemetry_used++;
      entry.telemetry_slot_index = slot_index;
      auto& slot = telemetry_slots_[slot_index];
      std::memset(&slot, 0, sizeof(slot));
      slot.quality = static_cast<uint8_t>(common::Quality::Bad);
      telemetry_header_->row_count = point_index_header_->telemetry_used;
      if (CatalogEntry* catalog = find_catalog_entry(
              catalog_entries_, catalog_header_->row_count, TableTelemetry)) {
        catalog->row_count = telemetry_header_->row_count;
      }
    }

    point_index_header_->base.row_count++;
    point_index_header_->base.last_update_time = now_ms();
    if (CatalogEntry* catalog =
            find_catalog_entry(catalog_entries_, catalog_header_->row_count, TablePointIndex)) {
      catalog->row_count = point_index_header_->base.row_count;
    }

    OPENEMS_LOG_D(
        "RtDb",
        "Registered point: " + pid + " category=" + std::to_string(point_category));
    return common::VoidResult::Ok();
  }

  return common::VoidResult::Err(common::ErrorCode::InvalidArgument, "No empty point index slots");
}

common::VoidResult RtDb::register_command_point(const common::PointId& pid) {
  if (!point_index_header_ || !command_slots_) {
    return common::VoidResult::Err(common::ErrorCode::InvalidConfig, "Not initialized");
  }

  std::lock_guard lock(process_mutex_);

  for (uint32_t i = 0; i < command_header_->capacity; ++i) {
    auto& slot = command_slots_[i];
    if (std::strncmp(slot.point_id, pid.c_str(), kMaxPointIdLen) == 0) {
      return common::VoidResult::Ok();
    }
  }

  if (point_index_header_->command_used >= point_index_header_->command_capacity) {
    return common::VoidResult::Err(common::ErrorCode::InvalidArgument, "No empty command slots");
  }

  int32_t point_idx = find_point_index(pid);
  if (point_idx < 0) {
    return common::VoidResult::Err(common::ErrorCode::PointNotFound, "Point not found: " + pid);
  }

  const uint32_t slot_index = point_index_header_->command_used++;
  auto& slot = command_slots_[slot_index];
  std::memset(&slot, 0, sizeof(slot));
  copy_fixed_string(slot.point_id, sizeof(slot.point_id), pid);
  slot.status = CommandIdle;
  index_table_[point_idx].command_slot_index = slot_index;

  command_header_->row_count = point_index_header_->command_used;
  command_header_->last_update_time = now_ms();
  if (CatalogEntry* catalog = find_catalog_entry(catalog_entries_, catalog_header_->row_count, TableCommand)) {
    catalog->row_count = command_header_->row_count;
  }

  OPENEMS_LOG_D("RtDb", "Registered command point: " + pid + " slot=" + std::to_string(slot_index));
  return common::VoidResult::Ok();
}

void RtDb::set_site_info(const common::SiteId& site_id, const std::string& site_name) {
  if (!point_index_header_) {
    return;
  }

  std::lock_guard lock(process_mutex_);
  copy_fixed_string(point_index_header_->site_id, sizeof(point_index_header_->site_id), site_id);
  copy_fixed_string(point_index_header_->site_name, sizeof(point_index_header_->site_name), site_name);
  point_index_header_->base.last_update_time = now_ms();
}

int32_t RtDb::find_point_index(const common::PointId& pid) const {
  if (!point_index_header_) {
    return -1;
  }

  for (uint32_t i = 0; i < point_index_header_->base.capacity; ++i) {
    if (std::strncmp(index_table_[i].point_id, pid.c_str(), kMaxPointIdLen) == 0) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

int32_t RtDb::find_command_slot(const common::PointId& pid) const {
  if (!command_header_) {
    return -1;
  }

  const int32_t point_idx = find_point_index(pid);
  if (point_idx >= 0) {
    const uint32_t slot_index = index_table_[point_idx].command_slot_index;
    if (slot_index != kInvalidSlotIndex && slot_index < command_header_->row_count) {
      return static_cast<int32_t>(slot_index);
    }
  }

  for (uint32_t i = 0; i < command_header_->row_count; ++i) {
    if (std::strncmp(command_slots_[i].point_id, pid.c_str(), kMaxPointIdLen) == 0) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

void RtDb::write_telemetry(
    const common::PointId& pid,
    double value,
    common::Quality quality,
    bool valid) {
  std::lock_guard lock(process_mutex_);

  const int32_t idx = find_point_index(pid);
  if (idx < 0) {
    return;
  }
  const uint32_t slot_index = index_table_[idx].telemetry_slot_index;
  if (slot_index == kInvalidSlotIndex || slot_index >= telemetry_header_->row_count) {
    return;
  }

  update_table_header_on_write(telemetry_header_);
  auto& slot = telemetry_slots_[slot_index];
  slot.value = value;
  slot.timestamp = now_ms();
  slot.quality = static_cast<uint8_t>(quality);
  slot.valid = valid ? 1 : 0;
  finish_table_header_write(telemetry_header_);
}

void RtDb::write_teleindication(
    const common::PointId& pid,
    uint16_t state_code,
    common::Quality quality,
    bool valid) {
  std::lock_guard lock(process_mutex_);

  const int32_t idx = find_point_index(pid);
  if (idx < 0) {
    return;
  }
  const uint32_t slot_index = index_table_[idx].teleindication_slot_index;
  if (slot_index == kInvalidSlotIndex || slot_index >= teleindication_header_->row_count) {
    return;
  }

  update_table_header_on_write(teleindication_header_);
  auto& slot = teleindication_slots_[slot_index];
  slot.state_code = state_code;
  slot.timestamp = now_ms();
  slot.quality = static_cast<uint8_t>(quality);
  slot.valid = valid ? 1 : 0;
  finish_table_header_write(teleindication_header_);
}

void RtDb::write_telemetry_batch(
    const std::vector<common::PointId>& pids,
    const std::vector<double>& values,
    const std::vector<common::Quality>& qualities) {
  std::lock_guard lock(process_mutex_);

  update_table_header_on_write(telemetry_header_);
  for (size_t i = 0; i < pids.size(); ++i) {
    const int32_t idx = find_point_index(pids[i]);
    if (idx < 0) {
      continue;
    }
    const uint32_t slot_index = index_table_[idx].telemetry_slot_index;
    if (slot_index == kInvalidSlotIndex || slot_index >= telemetry_header_->row_count) {
      continue;
    }
    auto& slot = telemetry_slots_[slot_index];
    slot.value = values[i];
    slot.timestamp = now_ms();
    slot.quality = static_cast<uint8_t>(qualities[i]);
    slot.valid = 1;
  }
  finish_table_header_write(telemetry_header_);
}

void RtDb::write_teleindication_batch(
    const std::vector<common::PointId>& pids,
    const std::vector<uint16_t>& state_codes,
    const std::vector<common::Quality>& qualities) {
  std::lock_guard lock(process_mutex_);

  update_table_header_on_write(teleindication_header_);
  for (size_t i = 0; i < pids.size(); ++i) {
    const int32_t idx = find_point_index(pids[i]);
    if (idx < 0) {
      continue;
    }
    const uint32_t slot_index = index_table_[idx].teleindication_slot_index;
    if (slot_index == kInvalidSlotIndex || slot_index >= teleindication_header_->row_count) {
      continue;
    }
    auto& slot = teleindication_slots_[slot_index];
    slot.state_code = state_codes[i];
    slot.timestamp = now_ms();
    slot.quality = static_cast<uint8_t>(qualities[i]);
    slot.valid = 1;
  }
  finish_table_header_write(teleindication_header_);
}

common::VoidResult RtDb::submit_command(const common::PointId& pid, double desired_value) {
  std::lock_guard lock(process_mutex_);

  const int32_t slot_idx = find_command_slot(pid);
  if (slot_idx < 0) {
    return common::VoidResult::Err(common::ErrorCode::PointNotFound, "Command slot not found: " + pid);
  }

  update_table_header_on_write(command_header_);
  auto& slot = command_slots_[slot_idx];
  slot.desired_value = desired_value;
  slot.submit_time = now_ms();
  slot.status = CommandPending;
  slot.error_code = 0;
  slot.result_value = 0.0;
  slot.complete_time = 0;
  finish_table_header_write(command_header_);

  OPENEMS_LOG_I("RtDb", "Command submitted: " + pid + " value=" + std::to_string(desired_value));
  return common::VoidResult::Ok();
}

bool RtDb::read_pending_command(common::PointId& out_pid, double& out_value) {
  std::lock_guard lock(process_mutex_);

  for (uint32_t i = 0; i < command_header_->row_count; ++i) {
    auto& slot = command_slots_[i];
    if (slot.status == CommandPending) {
      out_pid = trim_fixed_string(slot.point_id, sizeof(slot.point_id));
      out_value = slot.desired_value;
      slot.status = CommandExecuting;
      command_header_->last_update_time = now_ms();
      command_header_->update_seq += 2;
      return true;
    }
  }
  return false;
}

bool RtDb::read_pending_command_for_points(
    const std::vector<common::PointId>& point_ids,
    common::PointId& out_pid,
    double& out_value) {
  std::lock_guard lock(process_mutex_);

  for (uint32_t i = 0; i < command_header_->row_count; ++i) {
    auto& slot = command_slots_[i];
    if (slot.status != CommandPending) {
      continue;
    }

    const std::string pid = trim_fixed_string(slot.point_id, sizeof(slot.point_id));
    if (std::find(point_ids.begin(), point_ids.end(), pid) == point_ids.end()) {
      continue;
    }

    out_pid = pid;
    out_value = slot.desired_value;
    slot.status = CommandExecuting;
    command_header_->last_update_time = now_ms();
    command_header_->update_seq += 2;
    return true;
  }
  return false;
}

void RtDb::complete_command(
    const common::PointId& pid,
    CommandStatus status,
    double result_value,
    uint8_t error_code) {
  std::lock_guard lock(process_mutex_);

  const int32_t slot_idx = find_command_slot(pid);
  if (slot_idx < 0) {
    return;
  }

  update_table_header_on_write(command_header_);
  auto& slot = command_slots_[slot_idx];
  slot.status = static_cast<uint8_t>(status);
  slot.result_value = result_value;
  slot.error_code = error_code;
  slot.complete_time = now_ms();
  finish_table_header_write(command_header_);

  OPENEMS_LOG_I(
      "RtDb",
      "Command completed: " + pid + " status=" +
          std::to_string(static_cast<uint8_t>(status)) +
          " result=" + std::to_string(result_value));
}

common::Result<CommandReadResult> RtDb::read_command_status(const common::PointId& pid) {
  const int32_t slot_idx = find_command_slot(pid);
  if (slot_idx < 0) {
    return common::Result<CommandReadResult>::Err(
        common::ErrorCode::PointNotFound, "Command slot not found: " + pid);
  }

  const auto& slot = command_slots_[slot_idx];
  CommandReadResult result;
  result.desired_value = slot.desired_value;
  result.result_value = slot.result_value;
  result.submit_time = slot.submit_time;
  result.complete_time = slot.complete_time;
  result.status = static_cast<CommandStatus>(slot.status);
  result.error_code = slot.error_code;
  return common::Result<CommandReadResult>::Ok(result);
}

common::Result<TelemetryReadResult> RtDb::read_telemetry(const common::PointId& pid) {
  const int32_t idx = find_point_index(pid);
  if (idx < 0) {
    return common::Result<TelemetryReadResult>::Err(
        common::ErrorCode::PointNotFound, "Point not found: " + pid);
  }

  const uint32_t slot_index = index_table_[idx].telemetry_slot_index;
  if (slot_index == kInvalidSlotIndex || slot_index >= telemetry_header_->row_count) {
    return common::Result<TelemetryReadResult>::Err(
        common::ErrorCode::InvalidArgument, "Point is not telemetry/telecontrol/teleadjust: " + pid);
  }

  uint64_t seq1 = 0;
  uint64_t seq2 = 0;
  TelemetryReadResult result{};
  do {
    seq1 = telemetry_header_->update_seq;
    if (seq1 & 1ULL) {
      continue;
    }
    const auto& slot = telemetry_slots_[slot_index];
    result.value = slot.value;
    result.timestamp = slot.timestamp;
    result.quality = static_cast<common::Quality>(slot.quality);
    result.valid = slot.valid != 0;
    seq2 = telemetry_header_->update_seq;
  } while (seq1 != seq2);

  return common::Result<TelemetryReadResult>::Ok(result);
}

common::Result<TeleindicationReadResult> RtDb::read_teleindication(const common::PointId& pid) {
  const int32_t idx = find_point_index(pid);
  if (idx < 0) {
    return common::Result<TeleindicationReadResult>::Err(
        common::ErrorCode::PointNotFound, "Point not found: " + pid);
  }

  const uint32_t slot_index = index_table_[idx].teleindication_slot_index;
  if (slot_index == kInvalidSlotIndex || slot_index >= teleindication_header_->row_count) {
    return common::Result<TeleindicationReadResult>::Err(
        common::ErrorCode::InvalidArgument, "Point is not teleindication: " + pid);
  }

  uint64_t seq1 = 0;
  uint64_t seq2 = 0;
  TeleindicationReadResult result{};
  do {
    seq1 = teleindication_header_->update_seq;
    if (seq1 & 1ULL) {
      continue;
    }
    const auto& slot = teleindication_slots_[slot_index];
    result.state_code = slot.state_code;
    result.timestamp = slot.timestamp;
    result.quality = static_cast<common::Quality>(slot.quality);
    result.valid = slot.valid != 0;
    seq2 = teleindication_header_->update_seq;
  } while (seq1 != seq2);

  return common::Result<TeleindicationReadResult>::Ok(result);
}

SiteSnapshot RtDb::snapshot() {
  std::lock_guard lock(process_mutex_);

  SiteSnapshot snap;
  snap.site_id = trim_fixed_string(point_index_header_->site_id, sizeof(point_index_header_->site_id));
  snap.site_name = trim_fixed_string(point_index_header_->site_name, sizeof(point_index_header_->site_name));

  for (uint32_t i = 0; i < point_index_header_->base.capacity; ++i) {
    auto& entry = index_table_[i];
    if (entry.point_id[0] == '\0') {
      continue;
    }

    snap.point_ids.push_back(trim_fixed_string(entry.point_id, sizeof(entry.point_id)));
    snap.device_ids.push_back(trim_fixed_string(entry.device_id, sizeof(entry.device_id)));
    snap.point_categories.push_back(entry.point_category);

    if (entry.point_category == 1) {
      const auto& slot = teleindication_slots_[entry.teleindication_slot_index];
      snap.teleindication_values.push_back(slot.state_code);
      snap.qualities.push_back(static_cast<common::Quality>(slot.quality));
      snap.valids.push_back(slot.valid != 0);
      snap.timestamps.push_back(slot.timestamp);
    } else {
      const auto& slot = telemetry_slots_[entry.telemetry_slot_index];
      snap.telemetry_values.push_back(slot.value);
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
  oss << "=== Site[" << site_id << "] " << site_name << " ===\n";

  size_t telemetry_index = 0;
  size_t teleindication_index = 0;
  for (size_t i = 0; i < point_ids.size(); ++i) {
    oss << "  [" << point_ids[i] << "] "
        << "dev=" << device_ids[i] << " ";

    const auto category = point_categories[i];
    if (category == 1) {
      oss << "TI state=" << teleindication_values[teleindication_index++];
    } else {
      oss << "TELEM val=" << telemetry_values[telemetry_index++];
    }

    oss << " Q=" << static_cast<int>(qualities[i])
        << " valid=" << valids[i]
        << " ts=" << timestamps[i] << "\n";
  }

  return oss.str();
}

void RtDb::print_snapshot() {
  std::cout << snapshot().to_string() << std::flush;
}

uint32_t RtDb::telemetry_count() const {
  return telemetry_header_ ? telemetry_header_->row_count : 0;
}

uint32_t RtDb::teleindication_count() const {
  return teleindication_header_ ? teleindication_header_->row_count : 0;
}

uint32_t RtDb::total_point_count() const {
  return point_index_header_ ? point_index_header_->base.row_count : 0;
}

uint32_t RtDb::command_count() const {
  return command_header_ ? command_header_->row_count : 0;
}

uint64_t RtDb::update_sequence() const {
  uint64_t seq = 0;
  if (point_index_header_) {
    seq = (std::max)(seq, point_index_header_->base.update_seq);
  }
  if (telemetry_header_) {
    seq = (std::max)(seq, telemetry_header_->update_seq);
  }
  if (teleindication_header_) {
    seq = (std::max)(seq, teleindication_header_->update_seq);
  }
  if (command_header_) {
    seq = (std::max)(seq, command_header_->update_seq);
  }
  if (strategy_runtime_header_) {
    seq = (std::max)(seq, strategy_runtime_header_->update_seq);
  }
  if (alarm_active_header_) {
    seq = (std::max)(seq, alarm_active_header_->update_seq);
  }
  return seq;
}

std::vector<TableInfo> RtDb::list_tables() const {
  std::vector<TableInfo> tables;
  if (!catalog_header_ || !catalog_entries_) {
    return tables;
  }
  tables.reserve(catalog_header_->row_count);
  for (uint32_t i = 0; i < catalog_header_->row_count; ++i) {
    const auto& entry = catalog_entries_[i];
    TableInfo info;
    info.table_id = entry.table_id;
    info.table_name = trim_fixed_string(entry.table_name, sizeof(entry.table_name));
    info.shm_name = trim_fixed_string(entry.shm_name, sizeof(entry.shm_name));
    info.row_size = entry.row_size;
    info.capacity = entry.capacity;
    info.row_count = entry.row_count;
    info.flags = entry.flags;
    tables.push_back(std::move(info));
  }
  return tables;
}

common::Result<TableInfo> RtDb::get_table_info(const std::string& table_name) const {
  // catalog 是整个共享内存表空间的目录入口；如果它都没有 attach，
  // 后续就无法按表名发现和定位其他业务表。
  if (!catalog_header_ || !catalog_entries_) {
    return common::Result<TableInfo>::Err(common::ErrorCode::InvalidConfig, "Catalog not attached");
  }

  // 在 catalog 表中按逻辑表名查找对应的目录项。
  const CatalogEntry* entry =
      find_catalog_entry(catalog_entries_, catalog_header_->row_count, table_name);
  if (!entry) {
    return common::Result<TableInfo>::Err(
        common::ErrorCode::PointNotFound, "Table not found: " + table_name);
  }

  // 将共享内存中的固定长度目录项，转换成上层更易用的 TableInfo。
  TableInfo info;
  info.table_id = entry->table_id;
  info.table_name = trim_fixed_string(entry->table_name, sizeof(entry->table_name));
  info.shm_name = trim_fixed_string(entry->shm_name, sizeof(entry->shm_name));
  info.row_size = entry->row_size;
  info.capacity = entry->capacity;
  info.row_count = entry->row_count;
  info.flags = entry->flags;
  return common::Result<TableInfo>::Ok(info);
}

common::Result<TableInfo> RtDb::get_table_info(uint16_t table_id) const {
  // 先确保 catalog 已经可用；按表号查询本质上也是一次目录表查询。
  if (!catalog_header_ || !catalog_entries_) {
    return common::Result<TableInfo>::Err(common::ErrorCode::InvalidConfig, "Catalog not attached");
  }

  // 在 catalog 中按 table_id 查找目标表的元信息。
  CatalogEntry* entry = find_catalog_entry(catalog_entries_, catalog_header_->row_count, table_id);
  if (!entry) {
    return common::Result<TableInfo>::Err(
        common::ErrorCode::PointNotFound, "Table id not found: " + std::to_string(table_id));
  }

  // 这里只返回表描述信息，不会读取表内业务行数据。
  TableInfo info;
  info.table_id = entry->table_id;
  info.table_name = trim_fixed_string(entry->table_name, sizeof(entry->table_name));
  info.shm_name = trim_fixed_string(entry->shm_name, sizeof(entry->shm_name));
  info.row_size = entry->row_size;
  info.capacity = entry->capacity;
  info.row_count = entry->row_count;
  info.flags = entry->flags;
  return common::Result<TableInfo>::Ok(info);
}

common::Result<TableView> RtDb::open_table(const std::string& table_name) {
  auto info_result = get_table_info(table_name);
  if (!info_result.is_ok()) {
    return common::Result<TableView>::Err(info_result.error_code(), info_result.error_msg());
  }
  return open_table(info_result.value().table_id);
}

common::Result<TableView> RtDb::open_table(uint16_t table_id) {
  auto info_result = get_table_info(table_id);
  if (!info_result.is_ok()) {
    return common::Result<TableView>::Err(info_result.error_code(), info_result.error_msg());
  }

  Segment* segment = nullptr;
  TableHeader* header = nullptr;
  void* rows = nullptr;
  switch (table_id) {
    case TableCatalog:
      segment = &catalog_segment_;
      header = catalog_header_;
      rows = catalog_entries_;
      break;
    case TablePointIndex:
      segment = &point_index_segment_;
      header = &point_index_header_->base;
      rows = index_table_;
      break;
    case TableTelemetry:
      segment = &telemetry_segment_;
      header = telemetry_header_;
      rows = telemetry_slots_;
      break;
    case TableTeleindication:
      segment = &teleindication_segment_;
      header = teleindication_header_;
      rows = teleindication_slots_;
      break;
    case TableCommand:
      segment = &command_segment_;
      header = command_header_;
      rows = command_slots_;
      break;
    case TableStrategyRuntime:
      segment = &strategy_runtime_segment_;
      header = strategy_runtime_header_;
      rows = strategy_runtime_slots_;
      break;
    case TableAlarmActive:
      segment = &alarm_active_segment_;
      header = alarm_active_header_;
      rows = alarm_active_slots_;
      break;
    default:
      break;
  }

  if (!segment || !header || !rows) {
    return common::Result<TableView>::Err(
        common::ErrorCode::InvalidArgument,
        "Table is not attached: " + std::to_string(table_id));
  }

  TableView view;
  view.info = info_result.value();
  view.header = header;
  view.rows = rows;
  return common::Result<TableView>::Ok(view);
}

common::VoidResult RtDb::upsert_strategy_runtime(const StrategyRuntimeRecord& record) {
  if (!strategy_runtime_header_ || !strategy_runtime_slots_) {
    return common::VoidResult::Err(common::ErrorCode::InvalidConfig, "strategy_runtime table not attached");
  }

  std::lock_guard lock(process_mutex_);
  uint32_t slot_index = strategy_runtime_header_->row_count;
  for (uint32_t i = 0; i < strategy_runtime_header_->row_count; ++i) {
    if (std::strncmp(strategy_runtime_slots_[i].strategy_id,
                     record.strategy_id.c_str(),
                     kMaxStrategyIdLen) == 0) {
      slot_index = i;
      break;
    }
  }

  if (slot_index >= strategy_runtime_header_->capacity) {
    return common::VoidResult::Err(common::ErrorCode::InvalidArgument, "No free strategy_runtime slots");
  }

  update_table_header_on_write(strategy_runtime_header_);
  auto& slot = strategy_runtime_slots_[slot_index];
  if (slot_index == strategy_runtime_header_->row_count) {
    std::memset(&slot, 0, sizeof(slot));
    strategy_runtime_header_->row_count++;
    if (CatalogEntry* catalog = find_catalog_entry(
            catalog_entries_, catalog_header_->row_count, TableStrategyRuntime)) {
      catalog->row_count = strategy_runtime_header_->row_count;
    }
  }
  copy_fixed_string(slot.strategy_id, sizeof(slot.strategy_id), record.strategy_id);
  copy_fixed_string(slot.target_point_id, sizeof(slot.target_point_id), record.target_point_id);
  slot.target_value = record.target_value;
  slot.update_time = record.update_time ? record.update_time : now_ms();
  slot.suppressed = record.suppressed ? 1 : 0;
  copy_fixed_string(slot.suppress_reason, sizeof(slot.suppress_reason), record.suppress_reason);
  copy_fixed_string(slot.last_error, sizeof(slot.last_error), record.last_error);
  finish_table_header_write(strategy_runtime_header_);
  return common::VoidResult::Ok();
}

std::vector<StrategyRuntimeRecord> RtDb::read_strategy_runtime() const {
  std::vector<StrategyRuntimeRecord> rows;
  if (!strategy_runtime_header_ || !strategy_runtime_slots_) {
    return rows;
  }
  rows.reserve(strategy_runtime_header_->row_count);
  for (uint32_t i = 0; i < strategy_runtime_header_->row_count; ++i) {
    const auto& slot = strategy_runtime_slots_[i];
    if (slot.strategy_id[0] == '\0') {
      continue;
    }
    rows.push_back(StrategyRuntimeRecord{
        trim_fixed_string(slot.strategy_id, sizeof(slot.strategy_id)),
        trim_fixed_string(slot.target_point_id, sizeof(slot.target_point_id)),
        slot.target_value,
        slot.suppressed != 0,
        trim_fixed_string(slot.suppress_reason, sizeof(slot.suppress_reason)),
        trim_fixed_string(slot.last_error, sizeof(slot.last_error)),
        slot.update_time});
  }
  return rows;
}

common::VoidResult RtDb::replace_active_alarms(const std::vector<AlarmActiveRecord>& alarms) {
  if (!alarm_active_header_ || !alarm_active_slots_) {
    return common::VoidResult::Err(common::ErrorCode::InvalidConfig, "alarm_active table not attached");
  }
  if (alarms.size() > alarm_active_header_->capacity) {
    return common::VoidResult::Err(common::ErrorCode::InvalidArgument, "alarm_active capacity exceeded");
  }

  std::lock_guard lock(process_mutex_);
  update_table_header_on_write(alarm_active_header_);
  std::memset(alarm_active_slots_, 0,
              static_cast<size_t>(alarm_active_header_->capacity) * sizeof(AlarmActiveSlot));
  alarm_active_header_->row_count = static_cast<uint32_t>(alarms.size());
  for (uint32_t i = 0; i < alarm_active_header_->row_count; ++i) {
    const auto& src = alarms[i];
    auto& dst = alarm_active_slots_[i];
    copy_fixed_string(dst.alarm_id, sizeof(dst.alarm_id), src.alarm_id);
    copy_fixed_string(dst.point_id, sizeof(dst.point_id), src.point_id);
    copy_fixed_string(dst.device_id, sizeof(dst.device_id), src.device_id);
    copy_fixed_string(dst.severity, sizeof(dst.severity), src.severity);
    copy_fixed_string(dst.message, sizeof(dst.message), src.message);
    copy_fixed_string(dst.unit, sizeof(dst.unit), src.unit);
    dst.value = src.value;
    dst.trigger_time = src.trigger_time;
    dst.last_update_time = src.last_update_time;
    dst.active = src.active ? 1 : 0;
  }
  if (CatalogEntry* catalog = find_catalog_entry(
          catalog_entries_, catalog_header_->row_count, TableAlarmActive)) {
    catalog->row_count = alarm_active_header_->row_count;
  }
  finish_table_header_write(alarm_active_header_);
  return common::VoidResult::Ok();
}

std::vector<AlarmActiveRecord> RtDb::read_active_alarms() const {
  std::vector<AlarmActiveRecord> rows;
  if (!alarm_active_header_ || !alarm_active_slots_) {
    return rows;
  }
  rows.reserve(alarm_active_header_->row_count);
  for (uint32_t i = 0; i < alarm_active_header_->row_count; ++i) {
    const auto& slot = alarm_active_slots_[i];
    if (slot.alarm_id[0] == '\0') {
      continue;
    }
    rows.push_back(AlarmActiveRecord{
        trim_fixed_string(slot.alarm_id, sizeof(slot.alarm_id)),
        trim_fixed_string(slot.point_id, sizeof(slot.point_id)),
        trim_fixed_string(slot.device_id, sizeof(slot.device_id)),
        trim_fixed_string(slot.severity, sizeof(slot.severity)),
        trim_fixed_string(slot.message, sizeof(slot.message)),
        slot.value,
        trim_fixed_string(slot.unit, sizeof(slot.unit)),
        slot.trigger_time,
        slot.last_update_time,
        slot.active != 0});
  }
  return rows;
}

common::VoidResult RtDb::init_memory(uint32_t, uint32_t) {
  return common::VoidResult::Ok();
}

common::VoidResult RtDb::map_memory() {
  return common::VoidResult::Ok();
}

}  // namespace openems::rt_db
