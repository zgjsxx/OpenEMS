// src/apps/src/history_main.cpp
// History process: samples RtDb snapshots and appends point values to daily JSONL files.
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <ctime>
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

static std::string quality_to_string(openems::common::Quality quality) {
  switch (quality) {
    case openems::common::Quality::Good: return "Good";
    case openems::common::Quality::Questionable: return "Questionable";
    case openems::common::Quality::Bad: return "Bad";
    case openems::common::Quality::Invalid: return "Invalid";
    default: return "Unknown";
  }
}

static std::string category_to_string(uint8_t category) {
  switch (category) {
    case 0: return "telemetry";
    case 1: return "teleindication";
    case 2: return "telecontrol";
    case 3: return "teleadjust";
    default: return "unknown";
  }
}

static std::vector<std::string> split_csv_line(const std::string& line) {
  std::vector<std::string> cells;
  std::string current;
  bool in_quotes = false;

  for (size_t i = 0; i < line.size(); ++i) {
    char ch = line[i];
    if (ch == '"') {
      if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
        current.push_back('"');
        ++i;
      } else {
        in_quotes = !in_quotes;
      }
    } else if (ch == ',' && !in_quotes) {
      cells.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  cells.push_back(current);
  return cells;
}

static void load_units_from_table(const std::filesystem::path& path,
                                  std::unordered_map<std::string, std::string>& units) {
  std::ifstream in(path);
  if (!in.is_open()) return;

  std::string header_line;
  if (!std::getline(in, header_line)) return;
  if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();

  auto headers = split_csv_line(header_line);
  int id_idx = -1;
  int unit_idx = -1;
  for (size_t i = 0; i < headers.size(); ++i) {
    if (headers[i] == "id") id_idx = static_cast<int>(i);
    if (headers[i] == "unit") unit_idx = static_cast<int>(i);
  }
  if (id_idx < 0 || unit_idx < 0) return;

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto cells = split_csv_line(line);
    if (static_cast<int>(cells.size()) <= id_idx) continue;
    const std::string& point_id = cells[id_idx];
    if (point_id.empty()) continue;
    std::string unit;
    if (static_cast<int>(cells.size()) > unit_idx) unit = cells[unit_idx];
    units[point_id] = unit;
  }
}

static std::unordered_map<std::string, std::string> load_point_units(
    const std::string& config_dir) {
  std::unordered_map<std::string, std::string> units;
  std::filesystem::path base(config_dir);
  load_units_from_table(base / "telemetry.csv", units);
  load_units_from_table(base / "teleindication.csv", units);
  load_units_from_table(base / "telecontrol.csv", units);
  load_units_from_table(base / "teleadjust.csv", units);
  return units;
}

static std::string date_file_name(uint64_t timestamp_ms) {
  std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
  std::tm local_tm{};
#ifdef _WIN32
  localtime_s(&local_tm, &seconds);
#else
  localtime_r(&seconds, &local_tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&local_tm, "%Y%m%d") << ".jsonl";
  return oss.str();
}

static bool append_snapshot_jsonl(
    const std::string& history_dir,
    const std::unordered_map<std::string, std::string>& units,
    const openems::rt_db::SiteSnapshot& snap) {
  namespace fs = std::filesystem;
  fs::create_directories(history_dir);

  uint64_t row_ts = snap.snapshot_time > 0 ? snap.snapshot_time : now_ms();
  fs::path output_path = fs::path(history_dir) / date_file_name(row_ts);
  std::ofstream out(output_path, std::ios::app);
  if (!out.is_open()) {
    OPENEMS_LOG_W("History", "Failed to open history output: " + output_path.string());
    return false;
  }

  size_t telemetry_idx = 0;
  size_t teleindication_idx = 0;
  for (size_t i = 0; i < snap.point_ids.size(); ++i) {
    uint8_t category = snap.point_categories[i];
    double value = 0.0;
    if (category == 1) {
      if (teleindication_idx >= snap.teleindication_values.size()) continue;
      value = static_cast<double>(snap.teleindication_values[teleindication_idx++]);
    } else {
      if (telemetry_idx >= snap.telemetry_values.size()) continue;
      value = snap.telemetry_values[telemetry_idx++];
    }

    uint64_t ts = i < snap.timestamps.size() && snap.timestamps[i] > 0
        ? snap.timestamps[i]
        : row_ts;
    std::string unit;
    auto unit_it = units.find(snap.point_ids[i]);
    if (unit_it != units.end()) unit = unit_it->second;

    out << "{"
        << "\"ts\":" << ts << ","
        << "\"site_id\":\"" << json_escape(snap.site_id) << "\","
        << "\"point_id\":\"" << json_escape(snap.point_ids[i]) << "\","
        << "\"device_id\":\"" << json_escape(snap.device_ids[i]) << "\","
        << "\"category\":\"" << json_escape(category_to_string(category)) << "\","
        << "\"value\":" << std::fixed << std::setprecision(6) << value << ","
        << "\"unit\":\"" << json_escape(unit) << "\","
        << "\"quality\":\"" << json_escape(quality_to_string(snap.qualities[i])) << "\","
        << "\"valid\":" << (snap.valids[i] ? "true" : "false")
        << "}\n";
  }

  out.flush();
  return true;
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string shm_name = openems::rt_db::default_shm_name();
  std::string history_dir = "runtime/history";
  int interval_ms = 1000;
  if (argc > 1) shm_name = argv[1];
  if (argc > 2) history_dir = argv[2];
  if (argc > 3) interval_ms = std::max(100, std::atoi(argv[3]));

  auto units = load_point_units("config/tables");
  OPENEMS_LOG_I("History", "Loaded units for " + std::to_string(units.size()) + " points");
  OPENEMS_LOG_I("History", "History output: " + history_dir);
  OPENEMS_LOG_I("History", "Sample interval: " + std::to_string(interval_ms) + "ms");

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
    append_snapshot_jsonl(history_dir, units, snap);
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }

  delete db;
  OPENEMS_LOG_I("History", "Shutdown.");
  return 0;
}
