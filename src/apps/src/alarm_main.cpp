// src/apps/src/alarm_main.cpp
// Alarm process: attaches to RtDb, monitors points, and writes active alarms to JSON.
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running = false; }

struct ActiveAlarm {
  std::string id;
  std::string level;
  std::string point_id;
  std::string message;
  double value = 0.0;
  std::string unit;
  uint64_t trigger_time = 0;
  uint64_t last_update_time = 0;
};

static uint64_t now_ms() {
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();
}

static std::string json_escape(const std::string& value) {
  std::ostringstream oss;
  for (char ch : value) {
    switch (ch) {
      case '\\': oss << "\\\\"; break;
      case '"': oss << "\\\""; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default: oss << ch; break;
    }
  }
  return oss.str();
}

static std::string alarm_to_json(const ActiveAlarm& alarm) {
  std::ostringstream oss;
  oss << "{"
      << "\"id\":\"" << json_escape(alarm.id) << "\","
      << "\"level\":\"" << json_escape(alarm.level) << "\","
      << "\"point_id\":\"" << json_escape(alarm.point_id) << "\","
      << "\"message\":\"" << json_escape(alarm.message) << "\","
      << "\"value\":" << std::fixed << std::setprecision(6) << alarm.value << ","
      << "\"unit\":\"" << json_escape(alarm.unit) << "\","
      << "\"trigger_time\":" << alarm.trigger_time << ","
      << "\"last_update_time\":" << alarm.last_update_time
      << "}";
  return oss.str();
}

static void write_active_alarms_json(const std::string& output_path,
                                     const std::vector<ActiveAlarm>& alarms) {
  namespace fs = std::filesystem;

  fs::path path(output_path);
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path());
  }

  fs::path tmp_path = path;
  tmp_path += ".tmp";

  std::ofstream out(tmp_path, std::ios::trunc);
  if (!out.is_open()) {
    OPENEMS_LOG_W("Alarm", "Failed to open alarm output: " + tmp_path.string());
    return;
  }

  uint64_t ts = now_ms();
  out << "{\n";
  out << "  \"generated_at\": " << ts << ",\n";
  out << "  \"count\": " << alarms.size() << ",\n";
  out << "  \"alarms\": [\n";
  for (size_t i = 0; i < alarms.size(); ++i) {
    out << "    " << alarm_to_json(alarms[i]);
    if (i + 1 < alarms.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  out.close();

  std::error_code ec;
  fs::rename(tmp_path, path, ec);
  if (ec) {
    fs::remove(path, ec);
    ec.clear();
    fs::rename(tmp_path, path, ec);
  }
  if (ec) {
    OPENEMS_LOG_W("Alarm", "Failed to replace alarm output: " + ec.message());
  }
}

static void add_alarm(std::vector<ActiveAlarm>& alarms,
                      const std::string& id,
                      const std::string& level,
                      const std::string& point_id,
                      const std::string& message,
                      double value,
                      const std::string& unit,
                      uint64_t source_time) {
  uint64_t ts = now_ms();
  alarms.push_back(ActiveAlarm{id, level, point_id, message, value, unit, ts, source_time});
}

static std::vector<ActiveAlarm> collect_active_alarms(openems::rt_db::RtDb* db) {
  std::vector<ActiveAlarm> alarms;

  auto soc_result = db->read_telemetry("bess-soc");
  if (soc_result.is_ok() && soc_result.value().valid) {
    double soc = soc_result.value().value;
    if (soc < 10.0) {
      add_alarm(alarms, "bess-soc-low", "critical", "bess-soc",
          "SOC too low", soc, "%", soc_result.value().timestamp);
      OPENEMS_LOG_E("Alarm", "SOC too low: " + std::to_string(soc) + "%");
    } else if (soc > 95.0) {
      add_alarm(alarms, "bess-soc-high", "warning", "bess-soc",
          "SOC too high", soc, "%", soc_result.value().timestamp);
      OPENEMS_LOG_W("Alarm", "SOC too high: " + std::to_string(soc) + "%");
    }
  }

  auto grid_result = db->read_teleindication("bess-grid-state");
  if (grid_result.is_ok() && grid_result.value().valid) {
    uint16_t state = grid_result.value().state_code;
    if (state == 1) {
      add_alarm(alarms, "bess-off-grid", "warning", "bess-grid-state",
          "BESS is off-grid", static_cast<double>(state), "", grid_result.value().timestamp);
      OPENEMS_LOG_W("Alarm", "BESS is off-grid! state_code=" + std::to_string(state));
    }
  }

  auto pv_status = db->read_teleindication("pv-running-status");
  if (pv_status.is_ok() && pv_status.value().valid) {
    uint16_t status = pv_status.value().state_code;
    if (status == 0) {
      add_alarm(alarms, "pv-stopped", "warning", "pv-running-status",
          "PV is stopped", static_cast<double>(status), "", pv_status.value().timestamp);
      OPENEMS_LOG_W("Alarm", "PV is stopped. state_code=0");
    } else if (status == 3) {
      add_alarm(alarms, "pv-fault", "critical", "pv-running-status",
          "PV fault", static_cast<double>(status), "", pv_status.value().timestamp);
      OPENEMS_LOG_E("Alarm", "PV fault! state_code=3");
    }
  }

  return alarms;
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string shm_name = "Local\\openems_rt_db";
  std::string output_path = "runtime/alarms_active.json";
  if (argc > 1) shm_name = argv[1];
  if (argc > 2) output_path = argv[2];

  OPENEMS_LOG_I("Alarm", "Attaching to RtDb: " + shm_name);
  auto rtdb_result = openems::rt_db::RtDb::attach(shm_name);
  if (!rtdb_result.is_ok()) {
    OPENEMS_LOG_F("Alarm", "Attach failed: " + rtdb_result.error_msg());
    return 1;
  }
  auto* db = rtdb_result.value();

  OPENEMS_LOG_I("Alarm", "Running. Checking every 2 seconds.");
  OPENEMS_LOG_I("Alarm", "Active alarm output: " + output_path);

  std::unordered_map<std::string, uint64_t> trigger_times;
  while (g_running.load()) {
    auto alarms = collect_active_alarms(db);
    std::unordered_map<std::string, uint64_t> active_trigger_times;
    for (auto& alarm : alarms) {
      auto it = trigger_times.find(alarm.id);
      if (it != trigger_times.end()) {
        alarm.trigger_time = it->second;
      }
      active_trigger_times[alarm.id] = alarm.trigger_time;
    }
    trigger_times.swap(active_trigger_times);
    write_active_alarms_json(output_path, alarms);
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }

  write_active_alarms_json(output_path, {});
  delete db;
  OPENEMS_LOG_I("Alarm", "Shutdown.");
  return 0;
}
