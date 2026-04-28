// src/services/history/main.cpp
// History process: samples RtDb snapshots and writes to TimescaleDB (with JSONL fallback).
#include "history_writer.h"
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running = false; }

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

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string shm_name = openems::rt_db::default_shm_name();
  std::string history_dir = "runtime/history";
  int interval_ms = 1000;
  if (argc > 1) shm_name = argv[1];
  if (argc > 2) history_dir = argv[2];
  if (argc > 3) interval_ms = (std::max)(100, std::atoi(argv[3]));

  auto units = load_point_units("config/tables");
  OPENEMS_LOG_I("History", "Loaded units for " + std::to_string(units.size()) + " points");
  OPENEMS_LOG_I("History", "History output: " + history_dir);
  OPENEMS_LOG_I("History", "Sample interval: " + std::to_string(interval_ms) + "ms");

  openems::history::HistoryWriter writer(history_dir, units);

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
    writer.write(snap);
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }

  delete db;
  OPENEMS_LOG_I("History", "Shutdown.");
  return 0;
}