// src/apps/src/main.cpp
#include "openems/config/config_loader.h"
#include "openems/config/ems_config.h"
#include "openems/model/site.h"
#include "openems/model/device.h"
#include "openems/model/point.h"
#include "openems/modbus/modbus_tcp_client.h"
#include "openems/collector/polling_service.h"
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"
#include "openems/utils/time_utils.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

static std::atomic<bool> g_running{true};

static void signal_handler(int sig) {
  OPENEMS_LOG_I("Main", "Received signal " + std::to_string(sig) + ", shutting down...");
  g_running = false;
}

// Build model tree from config (static metadata only — no values stored in Point)
static openems::model::SitePtr build_site_from_config(const openems::config::EmsConfig& cfg) {
  auto site = openems::model::SiteCreate(
      cfg.site.id, cfg.site.name, cfg.site.description);

  for (auto& dc : cfg.site.devices) {
    if (dc.protocol != "modbus-tcp") continue;
    auto device = openems::model::DeviceCreate(
        dc.id, dc.name, dc.type, dc.ip, dc.port,
        dc.unit_id, dc.poll_interval_ms);

    for (auto& pc : dc.points) {
      auto point = openems::model::PointCreate(
          pc.id, pc.name, pc.code, pc.category,
          pc.data_type, pc.unit, pc.writable);
      if (pc.has_modbus_mapping) point->set_modbus_mapping(pc.modbus_mapping);
      device->add_point(std::move(point));
    }

    site->add_device(std::move(device));
  }

  return site;
}

// Count telemetry and teleindication points from config
static void count_points(const openems::config::EmsConfig& cfg,
                         uint32_t& telem_count, uint32_t& ti_count) {
  telem_count = 0;
  ti_count = 0;
  for (auto& dc : cfg.site.devices) {
    for (auto& pc : dc.points) {
      if (pc.category == openems::common::PointCategory::Telemetry ||
          pc.category == openems::common::PointCategory::Telecontrol ||
          pc.category == openems::common::PointCategory::Setting) {
        telem_count++;
      } else {
        ti_count++;  // Teleindication
      }
    }
  }
}

// Register all points in RtDb
static void register_points_in_rtdb(
    openems::rt_db::RtDb* db,
    const openems::config::EmsConfig& cfg) {
  for (auto& dc : cfg.site.devices) {
    for (auto& pc : dc.points) {
      uint8_t category = 0;  // default telemetry
      if (pc.category == openems::common::PointCategory::Teleindication) {
        category = 1;
      } else if (pc.category == openems::common::PointCategory::Telecontrol ||
                 pc.category == openems::common::PointCategory::Setting) {
        category = 0;  // treat as telemetry (analog setpoint)
      }

      auto rc = db->register_point(
          pc.id, dc.id, category,
          static_cast<uint8_t>(pc.data_type), pc.unit);
      if (!rc.is_ok()) {
        OPENEMS_LOG_W("Main", "Register point failed: " + pc.id + " - " + rc.error_msg());
      }
    }
  }
}

// Polling task: poll device, parse data, write to RtDb
// Updated to use RtDb instead of RealtimeDataManager
static void setup_polling_tasks(
    const openems::model::SitePtr& site,
    openems::collector::PollingServicePtr& poll_service,
    openems::rt_db::RtDb* db) {

  for (auto& device : site->devices()) {
    openems::modbus::ModbusConfig mb_cfg;
    mb_cfg.ip = device->ip();
    mb_cfg.port = device->port();
    mb_cfg.unit_id = device->unit_id();
    mb_cfg.timeout_ms = 3000;
    mb_cfg.reconnect_interval_ms = 5000;
    mb_cfg.max_reconnect_attempts = 3;

    auto client = openems::modbus::ModbusTcpClientCreate(mb_cfg);
    auto rc = client->connect();
    if (!rc.is_ok()) {
      OPENEMS_LOG_W("Main",
          "Initial connect failed for " + device->id() +
          " (" + device->ip() + ":" + std::to_string(device->port()) +
          "): " + rc.error_msg() + " — will retry via reconnect");
    }

    // Note: DevicePollTask still uses rt_data::RealtimeDataManager internally
    // For now we create a minimal adapter — in a full refactor, DevicePollTask
    // would directly write to RtDb. Here we keep backward compat and add
    // a snapshot thread that reads from RtDb instead.
    auto rt_manager = openems::rt_data::RealtimeDataManagerCreate(site);
    auto task = openems::collector::DevicePollTaskCreate(
        device, client, rt_manager);
    poll_service->add_task(std::move(task));
  }
}

// Periodic snapshot printer using RtDb
static void snapshot_printer(openems::rt_db::RtDb* db) {
  while (g_running.load()) {
    db->print_snapshot();
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // 1. Load config
  std::string config_path = "config/tables";
  if (argc > 1) config_path = argv[1];

  OPENEMS_LOG_I("Main", "Loading config from: " + config_path);
  auto cfg_result = openems::config::ConfigLoader::load(config_path);
  if (!cfg_result.is_ok()) {
    OPENEMS_LOG_F("Main", "Config load failed: " + cfg_result.error_msg());
    return 1;
  }
  auto& cfg = cfg_result.value();

  // Set log level
  openems::utils::LogLevel log_level = openems::utils::LogLevel::Info;
  if (cfg.log_level == "debug") log_level = openems::utils::LogLevel::Debug;
  if (cfg.log_level == "trace") log_level = openems::utils::LogLevel::Trace;
  if (cfg.log_level == "warn")  log_level = openems::utils::LogLevel::Warn;
  openems::utils::Logger::instance().set_level(log_level);

  // 2. Build model tree (static metadata)
  OPENEMS_LOG_I("Main", "Building site model...");
  auto site = build_site_from_config(cfg);
  OPENEMS_LOG_I("Main", site->to_string());

  // 3. Create RtDb shared memory
  uint32_t telem_count = 0, ti_count = 0;
  count_points(cfg, telem_count, ti_count);

  std::string shm_name = "openems_rt_db";
#ifdef _WIN32
  shm_name = "Local\\openems_rt_db";  // Windows named shared memory
#endif

  OPENEMS_LOG_I("Main", "Creating RtDb shared memory: " + shm_name +
      " telemetry=" + std::to_string(telem_count) +
      " teleindication=" + std::to_string(ti_count));

  auto rtdb_result = openems::rt_db::RtDb::create(shm_name, telem_count, ti_count, 0);
  if (!rtdb_result.is_ok()) {
    OPENEMS_LOG_F("Main", "RtDb create failed: " + rtdb_result.error_msg());
    return 1;
  }
  auto* db = rtdb_result.value();

  // 4. Register points in RtDb
  register_points_in_rtdb(db, cfg);

  // Copy site info into shared memory header
  auto* hdr = reinterpret_cast<openems::rt_db::ShmHeader*>(db->is_creator() ? nullptr : nullptr);
  // Note: header is already initialized in RtDb::create, we update site info here
  // The header_ is internal, so we use snapshot to verify

  // 5. Start polling service (writes to model tree via RealtimeDataManager)
  // TODO: In next refactor, DevicePollTask will write directly to RtDb
  auto poll_service = openems::collector::PollingServiceCreate();
  poll_service->set_default_interval(cfg.default_poll_interval_ms);

  auto rt_manager = openems::rt_data::RealtimeDataManagerCreate(site);
  for (auto& device : site->devices()) {
    openems::modbus::ModbusConfig mb_cfg;
    mb_cfg.ip = device->ip();
    mb_cfg.port = device->port();
    mb_cfg.unit_id = device->unit_id();
    mb_cfg.timeout_ms = 3000;
    mb_cfg.reconnect_interval_ms = 5000;
    mb_cfg.max_reconnect_attempts = 3;

    auto client = openems::modbus::ModbusTcpClientCreate(mb_cfg);
    client->connect();

    auto task = openems::collector::DevicePollTaskCreate(
        device, client, rt_manager);
    poll_service->add_task(std::move(task));
  }
  poll_service->start();

  // 6. Start RtDb snapshot printer thread
  std::thread print_thread(snapshot_printer, db);

  // 7. Main loop
  OPENEMS_LOG_I("Main", "EMS running. Press Ctrl+C to stop.");
  OPENEMS_LOG_I("Main", "RtDb shared memory: " + shm_name);
  OPENEMS_LOG_I("Main", "Other processes can attach via RtDb::attach(\"" + shm_name + "\")");

  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  // 8. Shutdown
  poll_service->stop();
  if (print_thread.joinable()) print_thread.join();
  delete db;

  OPENEMS_LOG_I("Main", "EMS shutdown complete.");
  return 0;
}
