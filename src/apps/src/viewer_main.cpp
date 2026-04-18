// src/apps/src/viewer_main.cpp
// Viewer process: attaches to RtDb shared memory, prints real-time snapshot
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running = false; }

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string shm_name = "Global\\openems_rt_db";
  if (argc > 1) shm_name = argv[1];

  OPENEMS_LOG_I("Viewer", "Attaching to RtDb: " + shm_name);
  auto rtdb_result = openems::rt_db::RtDb::attach(shm_name);
  if (!rtdb_result.is_ok()) {
    OPENEMS_LOG_F("Viewer", "Attach failed: " + rtdb_result.error_msg());
    return 1;
  }
  auto* db = rtdb_result.value();

  OPENEMS_LOG_I("Viewer", "Attached. Points: " + std::to_string(db->total_point_count()) +
      " Telemetry: " + std::to_string(db->telemetry_count()) +
      " Teleindication: " + std::to_string(db->teleindication_count()));

  while (g_running.load()) {
    db->print_snapshot();
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }

  delete db;
  OPENEMS_LOG_I("Viewer", "Shutdown.");
  return 0;
}