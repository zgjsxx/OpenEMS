// src/services/history/main.cpp
// History process: samples RtDb snapshots and writes to TimescaleDB only.
#include "history_writer.h"
#include "openems/config/config_loader.h"
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running = false; }

static std::unordered_map<std::string, std::string> load_point_units_from_postgres() {
  std::unordered_map<std::string, std::string> units;
  auto cfg_result = openems::config::ConfigLoader::load("postgresql", "", "");
  if (!cfg_result.is_ok()) {
    OPENEMS_LOG_F("History", "Failed to load PostgreSQL config for point units: " + cfg_result.error_msg());
    return units;
  }

  for (const auto& device : cfg_result.value().site.devices) {
    for (const auto& point : device.points) {
      units[point.id] = point.unit;
    }
  }
  return units;
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string shm_name = openems::rt_db::default_shm_name();
  int interval_ms = 1000;
  if (argc > 1) shm_name = argv[1];
  if (argc > 2) interval_ms = (std::max)(100, std::atoi(argv[2]));

  auto units = load_point_units_from_postgres();
  if (units.empty()) {
    OPENEMS_LOG_F("History", "No point metadata loaded from PostgreSQL. Refusing to start.");
    return 1;
  }
  OPENEMS_LOG_I("History", "Loaded units for " + std::to_string(units.size()) + " points");
  OPENEMS_LOG_I("History", "Sample interval: " + std::to_string(interval_ms) + "ms");

  openems::history::HistoryWriter writer(units);
  if (!writer.is_ready()) {
    OPENEMS_LOG_F("History", "TimescaleDB initialization failed: " + writer.last_error());
    return 1;
  }

  openems::rt_db::RtDb* db = nullptr;
  while (g_running.load()) {
    if (db == nullptr) {
      auto attach_result = openems::rt_db::RtDb::attach(shm_name);
      if (attach_result.is_ok()) {
        db = attach_result.value();
        OPENEMS_LOG_I("History", "Attached to RtDb: " + shm_name);
      } else {
        OPENEMS_LOG_W("History", "RtDb not available, retrying: " + attach_result.error_msg());
        std::this_thread::sleep_for(std::chrono::seconds(2));
        continue;
      }
    }

    auto snap = db->snapshot();
    if (!writer.write(snap)) {
      OPENEMS_LOG_F("History", "History write failed: " + writer.last_error());
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }

  delete db;
  OPENEMS_LOG_I("History", "Shutdown.");
  return g_running.load() ? 1 : 0;
}
