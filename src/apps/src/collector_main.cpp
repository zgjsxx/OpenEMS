// src/apps/src/collector_main.cpp
// Modbus Collector process: creates RtDb, polls Modbus devices, writes to RtDb
#include "openems/config/config_loader.h"
#include "openems/config/ems_config.h"
#include "openems/model/site.h"
#include "openems/model/device.h"
#include "openems/model/point.h"
#include "openems/modbus/modbus_tcp_client.h"
#include "openems/collector/polling_service.h"
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running = false; }

static openems::model::SitePtr build_site(const openems::config::EmsConfig& cfg) {
  auto site = openems::model::SiteCreate(cfg.site.id, cfg.site.name, cfg.site.description);
  for (auto& dc : cfg.site.devices) {
    auto device = openems::model::DeviceCreate(
        dc.id, dc.name, dc.type, dc.ip, dc.port, dc.unit_id, dc.poll_interval_ms);
    for (auto& pc : dc.points) {
      auto point = openems::model::PointCreate(
          pc.id, pc.name, pc.code, pc.category, pc.data_type, pc.unit, pc.writable);
      if (pc.has_modbus_mapping) point->set_modbus_mapping(pc.modbus_mapping);
      device->add_point(std::move(point));
    }
    site->add_device(std::move(device));
  }
  return site;
}

static uint32_t count_category(const openems::config::EmsConfig& cfg,
                               openems::common::PointCategory cat) {
  uint32_t count = 0;
  for (auto& dc : cfg.site.devices)
    for (auto& pc : dc.points)
      if (pc.category == cat) count++;
  return count;
}

static void register_points(openems::rt_db::RtDb* db, const openems::config::EmsConfig& cfg) {
  for (auto& dc : cfg.site.devices) {
    for (auto& pc : dc.points) {
      uint8_t category = 0;
      if (pc.category == openems::common::PointCategory::Teleindication) category = 1;
      db->register_point(pc.id, dc.id, category, static_cast<uint8_t>(pc.data_type), pc.unit);
    }
  }
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string config_path = "config/ems.json";
  if (argc > 1) config_path = argv[1];

  OPENEMS_LOG_I("Collector", "Loading config: " + config_path);
  auto cfg_result = openems::config::ConfigLoader::load(config_path);
  if (!cfg_result.is_ok()) {
    OPENEMS_LOG_F("Collector", "Config failed: " + cfg_result.error_msg());
    return 1;
  }
  auto& cfg = cfg_result.value();

  auto site = build_site(cfg);
  OPENEMS_LOG_I("Collector", site->to_string());

  // Create shared memory
  std::string shm_name = "Global\\openems_rt_db";
  uint32_t ti_count = count_category(cfg, openems::common::PointCategory::Teleindication);
  uint32_t telem_count = 0;
  for (auto& dc : cfg.site.devices) for (auto& pc : dc.points)
    if (pc.category != openems::common::PointCategory::Teleindication) telem_count++;

  auto rtdb_result = openems::rt_db::RtDb::create(shm_name, telem_count, ti_count);
  if (!rtdb_result.is_ok()) {
    OPENEMS_LOG_F("Collector", "RtDb create failed: " + rtdb_result.error_msg());
    return 1;
  }
  auto* db = rtdb_result.value();
  register_points(db, cfg);

  // Setup polling tasks — each writes to RtDb
  auto poll_service = openems::collector::PollingServiceCreate();
  poll_service->set_default_interval(cfg.default_poll_interval_ms);

  for (auto& device : site->devices()) {
    openems::modbus::ModbusConfig mb_cfg;
    mb_cfg.ip = device->ip();
    mb_cfg.port = device->port();
    mb_cfg.unit_id = device->unit_id();
    mb_cfg.timeout_ms = 3000;

    auto client = openems::modbus::ModbusTcpClientCreate(mb_cfg);
    client->connect();

    auto task = openems::collector::DevicePollTaskCreate(device, client, db);
    poll_service->add_task(std::move(task));
  }
  poll_service->start();

  OPENEMS_LOG_I("Collector", "Running. Shared memory: " + shm_name);
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  poll_service->stop();
  delete db;
  OPENEMS_LOG_I("Collector", "Shutdown.");
  return 0;
}