// src/apps/src/alarm_main.cpp
// Alarm process: attaches to RtDb, monitors points, and writes active alarms to JSON.
#include "openems/rt_db/rt_db.h"
#include "openems/config/csv_parser.h"
#include "openems/utils/logger.h"

#include <algorithm>
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

struct AlarmRule {
  std::string id;
  std::string point_id;
  bool enabled = false;
  std::string op;
  double threshold = 0.0;
  std::string severity;
  std::string message;
};

struct PointValue {
  double value = 0.0;
  uint64_t timestamp = 0;
  bool valid = false;
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

static std::string join_path(const std::string& dir_path, const std::string& filename) {
  if (dir_path.empty()) return filename;
  char last = dir_path.back();
  if (last == '/' || last == '\\') return dir_path + filename;
  return dir_path + "/" + filename;
}

static bool parse_bool(const std::string& value, bool default_value = false) {
  if (value == "true" || value == "1") return true;
  if (value == "false" || value == "0") return false;
  return default_value;
}

static bool is_supported_operator(const std::string& op) {
  return op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=";
}

static bool is_supported_severity(const std::string& severity) {
  return severity == "info" || severity == "warning" || severity == "critical";
}

static bool parse_double_strict(const std::string& text, double& out_value) {
  try {
    size_t consumed = 0;
    out_value = std::stod(text, &consumed);
    return consumed == text.size();
  } catch (...) {
    return false;
  }
}

static bool compare_value(double value, const std::string& op, double threshold) {
  if (op == "<") return value < threshold;
  if (op == "<=") return value <= threshold;
  if (op == ">") return value > threshold;
  if (op == ">=") return value >= threshold;
  if (op == "==") return value == threshold;
  if (op == "!=") return value != threshold;
  return false;
}

static std::vector<AlarmRule> load_alarm_rules(const std::string& config_path) {
  std::vector<AlarmRule> rules;
  auto rule_path = join_path(config_path, "alarm_rule.csv");
  auto result = openems::config::parse_csv_file(rule_path);
  if (!result.is_ok()) {
    OPENEMS_LOG_W("Alarm", "Alarm rule file is unavailable: " + result.error_msg());
    return rules;
  }

  const auto& table = result.value();
  auto id_idx = table.col_index("id");
  auto point_idx = table.col_index("point_id");
  auto enabled_idx = table.col_index("enabled");
  auto op_idx = table.col_index("operator");
  auto threshold_idx = table.col_index("threshold");
  auto severity_idx = table.col_index("severity");
  auto message_idx = table.col_index("message");
  if (!id_idx || !point_idx || !enabled_idx || !op_idx || !threshold_idx || !severity_idx || !message_idx) {
    OPENEMS_LOG_W("Alarm", "alarm_rule.csv has invalid headers; no alarm rules loaded.");
    return rules;
  }

  for (const auto& row : table.rows) {
    AlarmRule rule;
    rule.id = openems::config::csv_string(row, *id_idx);
    rule.point_id = openems::config::csv_string(row, *point_idx);
    rule.enabled = parse_bool(openems::config::csv_string(row, *enabled_idx), false);
    rule.op = openems::config::csv_string(row, *op_idx);
    rule.severity = openems::config::csv_string(row, *severity_idx);
    rule.message = openems::config::csv_string(row, *message_idx);
    auto threshold_text = openems::config::csv_string(row, *threshold_idx);

    if (!rule.enabled) continue;
    if (rule.id.empty() || rule.point_id.empty() || rule.message.empty()) {
      OPENEMS_LOG_W("Alarm", "Skipping invalid alarm rule with empty id/point/message.");
      continue;
    }
    if (!is_supported_operator(rule.op)) {
      OPENEMS_LOG_W("Alarm", "Skipping alarm rule with unsupported operator: " + rule.id);
      continue;
    }
    if (!is_supported_severity(rule.severity)) {
      OPENEMS_LOG_W("Alarm", "Skipping alarm rule with unsupported severity: " + rule.id);
      continue;
    }
    if (!parse_double_strict(threshold_text, rule.threshold)) {
      OPENEMS_LOG_W("Alarm", "Skipping alarm rule with invalid threshold: " + rule.id);
      continue;
    }

    rules.push_back(std::move(rule));
  }

  OPENEMS_LOG_I("Alarm", "Loaded enabled alarm rules: " + std::to_string(rules.size()));
  return rules;
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

static PointValue read_point_value(openems::rt_db::RtDb* db, const std::string& point_id) {
  auto telemetry = db->read_telemetry(point_id);
  if (telemetry.is_ok() && telemetry.value().valid) {
    return PointValue{telemetry.value().value, telemetry.value().timestamp, true};
  }

  auto teleindication = db->read_teleindication(point_id);
  if (teleindication.is_ok() && teleindication.value().valid) {
    return PointValue{
        static_cast<double>(teleindication.value().state_code),
        teleindication.value().timestamp,
        true};
  }

  return PointValue{};
}

static std::vector<ActiveAlarm> collect_active_alarms(
    openems::rt_db::RtDb* db,
    const std::vector<AlarmRule>& rules) {
  std::vector<ActiveAlarm> alarms;

  for (const auto& rule : rules) {
    auto point = read_point_value(db, rule.point_id);
    if (!point.valid) continue;

    if (compare_value(point.value, rule.op, rule.threshold)) {
      add_alarm(alarms, rule.id, rule.severity, rule.point_id,
          rule.message, point.value, "", point.timestamp);
      if (rule.severity == "critical") {
        OPENEMS_LOG_E("Alarm", rule.message + ": " + rule.point_id + "=" + std::to_string(point.value));
      } else {
        OPENEMS_LOG_W("Alarm", rule.message + ": " + rule.point_id + "=" + std::to_string(point.value));
      }
    }
  }

  return alarms;
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string shm_name = openems::rt_db::default_shm_name();
  std::string output_path = "runtime/alarms_active.json";
  std::string config_path = "config/tables";
  if (argc > 1) shm_name = argv[1];
  if (argc > 2) output_path = argv[2];
  if (argc > 3) config_path = argv[3];

  OPENEMS_LOG_I("Alarm", "Attaching to RtDb: " + shm_name);
  auto rtdb_result = openems::rt_db::RtDb::attach(shm_name);
  if (!rtdb_result.is_ok()) {
    OPENEMS_LOG_F("Alarm", "Attach failed: " + rtdb_result.error_msg());
    return 1;
  }
  auto* db = rtdb_result.value();

  OPENEMS_LOG_I("Alarm", "Running. Checking every 2 seconds.");
  OPENEMS_LOG_I("Alarm", "Active alarm output: " + output_path);
  OPENEMS_LOG_I("Alarm", "Alarm rule config path: " + config_path);
  auto rules = load_alarm_rules(config_path);

  std::unordered_map<std::string, uint64_t> trigger_times;
  while (g_running.load()) {
    auto alarms = collect_active_alarms(db, rules);
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
