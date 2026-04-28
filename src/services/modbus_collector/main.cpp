// src/apps/src/collector_main.cpp
// Modbus Collector process: attaches to RtDb, polls Modbus devices, writes to RtDb
// Also handles telecontrol/teleadjust write commands via ControlService
// Supports both Modbus TCP and Modbus RTU protocols
#include "openems/config/config_loader.h"
#include "openems/config/ems_config.h"
#include "openems/model/site.h"
#include "openems/model/device.h"
#include "openems/model/point.h"
#include "openems/modbus/imodbus_client.h"
#include "openems/modbus/modbus_tcp_client.h"
#include "openems/modbus/modbus_rtu_client.h"
#include "openems/collector/polling_service.h"
#include "openems/collector/control_service.h"
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <unordered_map>

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running = false; }

static openems::model::SitePtr build_site(const openems::config::EmsConfig& cfg) {
  auto site = openems::model::SiteCreate(cfg.site.id, cfg.site.name, cfg.site.description);
  for (auto& dc : cfg.site.devices) {
    // 支持 modbus-tcp 和 modbus-rtu，跳过 iec104 等其他协议
    if (dc.protocol != "modbus-tcp" && dc.protocol != "modbus-rtu") continue;

    auto device = openems::model::DeviceCreate(
        dc.id, dc.name, dc.type, dc.ip, dc.port, dc.unit_id, dc.poll_interval_ms,
        dc.protocol, dc.serial_port, dc.baud_rate, dc.parity, dc.data_bits, dc.stop_bits);
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

static openems::rt_db::RtDb* wait_for_rtdb(const std::string& shm_name) {
  while (g_running.load()) {
    auto attach_result = openems::rt_db::RtDb::attach(shm_name);
    if (attach_result.is_ok()) {
      OPENEMS_LOG_I("Collector", "Attached to RtDb: " + shm_name);
      return attach_result.value();
    }

    OPENEMS_LOG_W("Collector",
        "RtDb is not ready, retrying in 2 seconds: " + attach_result.error_msg());
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  return nullptr;
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string config_path = "config/tables";
  std::string shm_name = openems::rt_db::default_shm_name();
  if (argc > 1) config_path = argv[1];
  if (argc > 2) shm_name = argv[2];

  OPENEMS_LOG_I("Collector", "Loading config: " + config_path);
  auto cfg_result = openems::config::ConfigLoader::load(config_path);
  if (!cfg_result.is_ok()) {
    OPENEMS_LOG_F("Collector", "Config failed: " + cfg_result.error_msg());
    return 1;
  }
  auto& cfg = cfg_result.value();

  auto site = build_site(cfg);
  OPENEMS_LOG_I("Collector", site->to_string());

  auto* db = wait_for_rtdb(shm_name);
  if (!db) {
    OPENEMS_LOG_F("Collector", "Shutdown before RtDb became available.");
    return 1;
  }

  // Create shared Modbus clients (TCP or RTU) for both poll and control tasks
  std::unordered_map<openems::common::DeviceId, openems::modbus::IModbusClientPtr> modbus_clients;

  // RTU 串口是独占资源，同一串口的多个设备共享一个 RtuClient
  std::unordered_map<std::string, openems::modbus::ModbusRtuClientPtr> rtu_serial_clients;

  for (auto& device : site->devices()) {
    if (device->protocol() == "modbus-tcp") {
      openems::modbus::ModbusConfig mb_cfg;
      mb_cfg.ip = device->ip();
      mb_cfg.port = device->port();
      mb_cfg.unit_id = device->unit_id();
      mb_cfg.timeout_ms = 3000;

      auto client = openems::modbus::ModbusTcpClientCreate(mb_cfg);
      client->connect();
      modbus_clients[device->id()] = std::static_pointer_cast<openems::modbus::IModbusClient>(client);

    } else if (device->protocol() == "modbus-rtu") {
      // 同一串口共享客户端
      std::string sp_key = device->serial_port();
      auto it = rtu_serial_clients.find(sp_key);
      if (it == rtu_serial_clients.end()) {
        openems::modbus::ModbusRtuConfig rtu_cfg;
        rtu_cfg.serial_port = device->serial_port();
        rtu_cfg.baud_rate = device->baud_rate();
        rtu_cfg.parity = device->parity();
        rtu_cfg.data_bits = device->data_bits();
        rtu_cfg.stop_bits = device->stop_bits();
        rtu_cfg.unit_id = device->unit_id();
        rtu_cfg.timeout_ms = 3000;

        auto rtu_client = openems::modbus::ModbusRtuClientCreate(rtu_cfg);
        rtu_client->connect();
        rtu_serial_clients[sp_key] = rtu_client;
        modbus_clients[device->id()] = std::static_pointer_cast<openems::modbus::IModbusClient>(rtu_client);
      } else {
        // 共享同一串口客户端（注意: unit_id 可能不同，RTU 串口独占）
        modbus_clients[device->id()] = std::static_pointer_cast<openems::modbus::IModbusClient>(it->second);
      }
    }
  }

  // Setup polling tasks — each writes to RtDb
  auto poll_service = openems::collector::PollingServiceCreate();
  poll_service->set_default_interval(cfg.default_poll_interval_ms);

  // Setup control tasks — each reads commands from RtDb and writes to Modbus
  auto ctrl_service = openems::collector::ControlServiceCreate();
  ctrl_service->set_default_interval(100);  // 100ms default for control

  bool has_writable_points = false;

  for (auto& device : site->devices()) {
    auto& client = modbus_clients[device->id()];

    // Polling task for all devices
    auto poll_task = openems::collector::DevicePollTaskCreate(device, client, db);
    poll_service->add_task(std::move(poll_task));

    // Control task for devices with writable points
    bool device_has_writable = false;
    for (auto& pt : device->points()) {
      if (pt->writable() && pt->has_modbus_mapping()) {
        uint8_t fc = pt->modbus_mapping().function_code;
        if (fc == 5 || fc == 6 || fc == 16) {
          device_has_writable = true;
          has_writable_points = true;
        }
      }
    }

    if (device_has_writable) {
      auto ctrl_task = openems::collector::DeviceControlTaskCreate(device, client, db);
      ctrl_service->add_task(std::move(ctrl_task));
    }
  }

  poll_service->start();
  if (has_writable_points) {
    ctrl_service->start();
  }

  OPENEMS_LOG_I("Collector", "Running. Shared memory: " + shm_name);
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  if (has_writable_points) {
    ctrl_service->stop();
  }
  poll_service->stop();
  delete db;
  OPENEMS_LOG_I("Collector", "Shutdown.");
  return 0;
}
