// src/apps/src/iec104_collector_main.cpp
// IEC104 Collector process: attaches to RtDb, connects IEC104 devices, receives ASDU data, writes to RtDb
#include "openems/config/config_loader.h"
#include "openems/config/ems_config.h"
#include "openems/model/site.h"
#include "openems/model/device.h"
#include "openems/model/point.h"
#include "openems/iec104/iec104_client.h"
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <vector>
#include <cmath>

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running = false; }

static openems::model::SitePtr build_iec104_site(const openems::config::EmsConfig& cfg) {
  auto site = openems::model::SiteCreate(cfg.site.id, cfg.site.name, cfg.site.description);
  for (auto& dc : cfg.site.devices) {
    if (dc.protocol != "iec104") continue;  // Only IEC104 devices
    auto device = openems::model::DeviceCreate(
        dc.id, dc.name, dc.type, dc.ip, dc.port, dc.unit_id, dc.poll_interval_ms);
    for (auto& pc : dc.points) {
      auto point = openems::model::PointCreate(
          pc.id, pc.name, pc.code, pc.category, pc.data_type, pc.unit, pc.writable);
      point->set_protocol("iec104");
      if (pc.has_iec104_mapping) point->set_iec104_mapping(pc.iec104_mapping);
      device->add_point(std::move(point));
    }
    site->add_device(std::move(device));
  }
  return site;
}

static openems::rt_db::RtDb* wait_for_rtdb(const std::string& shm_name) {
  while (g_running.load()) {
    auto attach_result = openems::rt_db::RtDb::attach(shm_name);
    if (attach_result.is_ok()) {
      OPENEMS_LOG_I("IEC104Collector", "Attached to RtDb: " + shm_name);
      return attach_result.value();
    }

    OPENEMS_LOG_W("IEC104Collector",
        "RtDb is not ready, retrying in 2 seconds: " + attach_result.error_msg());
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  return nullptr;
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string shm_name = openems::rt_db::default_shm_name();
  if (argc > 1) {
    std::string first = argv[1];
    if (first.find("config/tables") != std::string::npos || first.find(".csv") != std::string::npos) {
      OPENEMS_LOG_W("IEC104Collector", "CSV config path argument is ignored. Runtime config now loads from PostgreSQL only.");
      if (argc > 2) shm_name = argv[2];
    } else {
      shm_name = first;
    }
  }

  OPENEMS_LOG_I("IEC104Collector", "Loading runtime config from PostgreSQL");
  auto cfg_result = openems::config::ConfigLoader::load("postgresql", "", "");
  if (!cfg_result.is_ok()) {
    OPENEMS_LOG_F("IEC104Collector", "Config failed: " + cfg_result.error_msg());
    return 1;
  }
  auto& cfg = cfg_result.value();

  auto site = build_iec104_site(cfg);
  OPENEMS_LOG_I("IEC104Collector", site->to_string());
  for (auto& dev : site->devices()) {
    OPENEMS_LOG_I("IEC104Collector", dev->to_string());
    for (auto& pt : dev->points()) {
      OPENEMS_LOG_D("IEC104Collector", pt->to_string());
    }
  }

  auto* db = wait_for_rtdb(shm_name);
  if (!db) {
    OPENEMS_LOG_F("IEC104Collector", "Shutdown before RtDb became available.");
    return 1;
  }

  // Create IEC104 clients for each device
  std::vector<openems::iec104::Iec104ClientPtr> clients;
  for (auto& device : site->devices()) {
    openems::iec104::Iec104Config ic_cfg;
    ic_cfg.ip = device->ip();
    ic_cfg.port = device->port();
    ic_cfg.common_address = 1;  // from device config later
    ic_cfg.timeout_ms = 3000;
    ic_cfg.heartbeat_interval_ms = 30000;
    ic_cfg.interrogation_interval_ms = 60000;

    auto client = openems::iec104::Iec104ClientCreate(ic_cfg);

    // Register points in client dispatch
    for (auto& pt : device->points()) {
      auto& mapping = pt->iec104_mapping();
      client->register_point(mapping.type_id, mapping.ioa,
          pt->id(), pt->category(), mapping.scale);
    }

    client->set_asdu_callback([db](const openems::iec104::PointUpdate& update) {
      if (!update.valid) {
        OPENEMS_LOG_D("IEC104Collector",
            "Skip invalid IEC104 update for point " + update.point_id);
        return;
      }

      if (update.category == openems::common::PointCategory::Teleindication) {
        db->write_teleindication(update.point_id,
            static_cast<uint16_t>(std::round(update.value)),
            update.quality, update.valid);
      } else {
        db->write_telemetry(update.point_id,
            update.value, update.quality, update.valid);
      }
    });

    auto rc = client->connect();
    if (!rc.is_ok()) {
      OPENEMS_LOG_W("IEC104Collector",
          "Connect failed for " + device->id() + ": " + rc.error_msg());
    }

    client->start();
    clients.push_back(std::move(client));
  }

  OPENEMS_LOG_I("IEC104Collector", "Running. Shared memory: " + shm_name);
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  for (auto& client : clients) {
    client->stop();
  }
  delete db;
  OPENEMS_LOG_I("IEC104Collector", "Shutdown.");
  return 0;
}
