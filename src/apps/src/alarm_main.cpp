// src/apps/src/alarm_main.cpp
// Alarm process: attaches to RtDb, monitors telemetry/teleindication for threshold violations
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running = false; }

// Simple alarm rule: check SOC and power thresholds
static void check_alarms(openems::rt_db::RtDb* db) {
  // SOC too low
  auto soc_result = db->read_telemetry("bess-soc");
  if (soc_result.is_ok() && soc_result.value().valid) {
    double soc = soc_result.value().value;
    if (soc < 10.0) {
      OPENEMS_LOG_E("Alarm", "SOC too low: " + std::to_string(soc) + "%");
    } else if (soc > 95.0) {
      OPENEMS_LOG_W("Alarm", "SOC too high: " + std::to_string(soc) + "%");
    }
  }

  // Grid state alarm
  auto grid_result = db->read_teleindication("bess-grid-state");
  if (grid_result.is_ok() && grid_result.value().valid) {
    uint16_t state = grid_result.value().state_code;
    // state 1 = OffGrid
    if (state == 1) {
      OPENEMS_LOG_W("Alarm", "BESS is off-grid! state_code=" + std::to_string(state));
    }
  }

  // PV running status
  auto pv_status = db->read_teleindication("pv-running-status");
  if (pv_status.is_ok() && pv_status.value().valid) {
    uint16_t status = pv_status.value().state_code;
    // status 0 = stopped, 3 = fault
    if (status == 0) {
      OPENEMS_LOG_W("Alarm", "PV is stopped. state_code=0");
    } else if (status == 3) {
      OPENEMS_LOG_E("Alarm", "PV fault! state_code=3");
    }
  }
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string shm_name = "Global\\openems_rt_db";
  if (argc > 1) shm_name = argv[1];

  OPENEMS_LOG_I("Alarm", "Attaching to RtDb: " + shm_name);
  auto rtdb_result = openems::rt_db::RtDb::attach(shm_name);
  if (!rtdb_result.is_ok()) {
    OPENEMS_LOG_F("Alarm", "Attach failed: " + rtdb_result.error_msg());
    return 1;
  }
  auto* db = rtdb_result.value();

  OPENEMS_LOG_I("Alarm", "Running. Checking every 2 seconds.");

  while (g_running.load()) {
    check_alarms(db);
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }

  delete db;
  OPENEMS_LOG_I("Alarm", "Shutdown.");
  return 0;
}