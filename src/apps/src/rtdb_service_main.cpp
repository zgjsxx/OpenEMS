// RtDb service process: owns shared memory lifetime and registers configured points.
#include "openems/config/config_loader.h"
#include "openems/config/ems_config.h"
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <thread>

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running = false; }

static uint8_t point_category_code(openems::common::PointCategory category) {
  if (category == openems::common::PointCategory::Teleindication) return 1;
  if (category == openems::common::PointCategory::Telecontrol) return 2;
  if (category == openems::common::PointCategory::Setting) return 3;
  return 0;
}

static bool is_command_point(const openems::config::PointConfig& point) {
  if (!point.writable || !point.has_modbus_mapping) return false;
  uint8_t fc = point.modbus_mapping.function_code;
  return fc == 5 || fc == 6 || fc == 16;
}

static void count_rtdb_capacity(const openems::config::EmsConfig& cfg,
                                uint32_t& telemetry_count,
                                uint32_t& teleindication_count,
                                uint32_t& command_count) {
  telemetry_count = 0;
  teleindication_count = 0;
  command_count = 0;

  for (const auto& device : cfg.site.devices) {
    for (const auto& point : device.points) {
      if (point.category == openems::common::PointCategory::Teleindication) {
        ++teleindication_count;
      } else {
        ++telemetry_count;
      }

      if (is_command_point(point)) {
        ++command_count;
      }
    }
  }
}

static openems::common::VoidResult register_config_points(
    openems::rt_db::RtDb* db,
    const openems::config::EmsConfig& cfg) {
  db->set_site_info(cfg.site.id, cfg.site.name);

  for (const auto& device : cfg.site.devices) {
    for (const auto& point : device.points) {
      auto result = db->register_point(
          point.id,
          device.id,
          point_category_code(point.category),
          static_cast<uint8_t>(point.data_type),
          point.unit,
          point.writable);
      if (!result.is_ok()) {
        return result;
      }

      if (is_command_point(point)) {
        auto command_result = db->register_command_point(point.id);
        if (!command_result.is_ok()) {
          return command_result;
        }
      }
    }
  }

  return openems::common::VoidResult::Ok();
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string config_path = "config/tables";
  std::string shm_name = openems::rt_db::default_shm_name();
  if (argc > 1) config_path = argv[1];
  if (argc > 2) shm_name = argv[2];

  OPENEMS_LOG_I("RtDbService", "Loading config: " + config_path);
  auto cfg_result = openems::config::ConfigLoader::load(config_path);
  if (!cfg_result.is_ok()) {
    OPENEMS_LOG_F("RtDbService", "Config failed: " + cfg_result.error_msg());
    return 1;
  }
  auto& cfg = cfg_result.value();

  uint32_t telemetry_count = 0;
  uint32_t teleindication_count = 0;
  uint32_t command_count = 0;
  count_rtdb_capacity(cfg, telemetry_count, teleindication_count, command_count);

  OPENEMS_LOG_I("RtDbService",
      "Creating RtDb: " + shm_name +
      " telemetry=" + std::to_string(telemetry_count) +
      " teleindication=" + std::to_string(teleindication_count) +
      " command=" + std::to_string(command_count));

  auto rtdb_result = openems::rt_db::RtDb::create(
      shm_name, telemetry_count, teleindication_count, command_count);
  if (!rtdb_result.is_ok()) {
    OPENEMS_LOG_F("RtDbService", "RtDb create failed: " + rtdb_result.error_msg());
    return 1;
  }
  auto* db = rtdb_result.value();

  auto register_result = register_config_points(db, cfg);
  if (!register_result.is_ok()) {
    OPENEMS_LOG_F("RtDbService", "Point registration failed: " + register_result.error_msg());
    delete db;
    return 1;
  }

  OPENEMS_LOG_I("RtDbService", "Running. Shared memory: " + shm_name);
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  delete db;
  OPENEMS_LOG_I("RtDbService", "Shutdown.");
  return 0;
}
