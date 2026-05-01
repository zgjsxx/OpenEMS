// src/services/alarm/main.cpp
// Alarm process: attaches to RtDb, evaluates PostgreSQL alarm rules, and writes active alarms to PostgreSQL.
#include "openems/libpq/libpq_api.h"
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
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
  std::string device_id;
  std::string message;
  double value = 0.0;
  std::string unit;
  uint64_t trigger_time = 0;
  uint64_t last_update_time = 0;
};

struct AlarmRule {
  std::string id;
  std::string point_id;
  std::string device_id;
  std::string unit;
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

struct DbContext {
  std::string db_url;
  openems::libpq::LibpqApi api;
  void* conn = nullptr;
  std::string last_error;
};

static uint64_t now_ms() {
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

static std::string sql_escape(const std::string& value) {
  std::string result;
  result.reserve(value.size());
  for (char ch : value) {
    if (ch == '\'') result += "''";
    else result += ch;
  }
  return result;
}

static std::string ts_to_iso8601(uint64_t ms) {
  std::time_t seconds = static_cast<std::time_t>(ms / 1000);
  int frac_ms = static_cast<int>(ms % 1000);
  std::tm utc_tm{};
#ifdef _WIN32
  gmtime_s(&utc_tm, &seconds);
#else
  gmtime_r(&seconds, &utc_tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%S");
  oss << "." << std::setfill('0') << std::setw(3) << frac_ms << "+00:00";
  return oss.str();
}

static std::string double_to_string(double value) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(6) << value;
  std::string text = oss.str();
  while (text.size() > 1 && text.back() == '0') text.pop_back();
  if (!text.empty() && text.back() == '.') text += '0';
  return text;
}

static bool connect_db(DbContext& db) {
  if (!db.conn) {
    db.conn = db.api.PQconnectdb(db.db_url.c_str());
  }
  if (!db.conn || db.api.PQstatus(db.conn) != 0) {
    db.last_error = "PostgreSQL connect failed: " + std::string(db.conn ? db.api.PQerrorMessage(db.conn) : "failed to allocate connection");
    if (db.conn) {
      db.api.PQfinish(db.conn);
      db.conn = nullptr;
    }
    return false;
  }
  db.last_error.clear();
  return true;
}

static bool reconnect_db(DbContext& db) {
  if (db.conn) {
    db.api.PQfinish(db.conn);
    db.conn = nullptr;
  }
  return connect_db(db);
}

static bool exec_sql(DbContext& db, const std::string& sql, int expected_status = 1 /* PGRES_COMMAND_OK */) {
  if (!db.conn && !connect_db(db)) return false;
  void* res = db.api.PQexec(db.conn, sql.c_str());
  if (!res || db.api.PQresultStatus(res) != expected_status) {
    db.last_error = db.conn ? db.api.PQerrorMessage(db.conn) : "PostgreSQL command failed";
    if (res) db.api.PQclear(res);
    return false;
  }
  db.api.PQclear(res);
  db.last_error.clear();
  return true;
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

static std::vector<AlarmRule> load_alarm_rules(DbContext& db) {
  std::vector<AlarmRule> rules;
  if (!connect_db(db)) {
    OPENEMS_LOG_E("Alarm", db.last_error);
    return rules;
  }

  const char* sql =
      "SELECT ar.id, ar.point_id, p.device_id, p.unit, ar.operator, ar.threshold, ar.severity, ar.message "
      "FROM alarm_rules ar "
      "JOIN points p ON p.id = ar.point_id "
      "WHERE ar.enabled = TRUE "
      "ORDER BY ar.id";
  void* res = db.api.PQexec(db.conn, sql);
  if (!res || db.api.PQresultStatus(res) != 2 /* PGRES_TUPLES_OK */) {
    db.last_error = db.conn ? db.api.PQerrorMessage(db.conn) : "PostgreSQL query failed";
    if (res) db.api.PQclear(res);
    OPENEMS_LOG_E("Alarm", "Failed to load alarm rules: " + db.last_error);
    return rules;
  }

  int count = db.api.PQntuples(res);
  for (int i = 0; i < count; ++i) {
    AlarmRule rule;
    rule.id = db.api.PQgetvalue(res, i, 0);
    rule.point_id = db.api.PQgetvalue(res, i, 1);
    rule.device_id = db.api.PQgetvalue(res, i, 2);
    rule.unit = db.api.PQgetvalue(res, i, 3);
    rule.op = db.api.PQgetvalue(res, i, 4);
    rule.threshold = std::atof(db.api.PQgetvalue(res, i, 5));
    rule.severity = db.api.PQgetvalue(res, i, 6);
    rule.message = db.api.PQgetvalue(res, i, 7);
    rules.push_back(std::move(rule));
  }
  db.api.PQclear(res);
  OPENEMS_LOG_I("Alarm", "Loaded enabled alarm rules from PostgreSQL: " + std::to_string(rules.size()));
  return rules;
}

static std::vector<ActiveAlarm> collect_active_alarms(
    openems::rt_db::RtDb* db,
    const std::vector<AlarmRule>& rules) {
  std::vector<ActiveAlarm> alarms;
  for (const auto& rule : rules) {
    auto point = read_point_value(db, rule.point_id);
    if (!point.valid) continue;
    if (!compare_value(point.value, rule.op, rule.threshold)) continue;

    alarms.push_back(ActiveAlarm{
        rule.id,
        rule.severity,
        rule.point_id,
        rule.device_id,
        rule.message,
        point.value,
        rule.unit,
        0,
        point.timestamp,
    });
  }
  return alarms;
}

static std::vector<openems::rt_db::AlarmActiveRecord> build_alarm_runtime_records(
    const std::vector<ActiveAlarm>& alarms) {
  std::vector<openems::rt_db::AlarmActiveRecord> records;
  records.reserve(alarms.size());
  for (const auto& alarm : alarms) {
    records.push_back(openems::rt_db::AlarmActiveRecord{
        alarm.id,
        alarm.point_id,
        alarm.device_id,
        alarm.level,
        alarm.message,
        alarm.value,
        alarm.unit,
        alarm.trigger_time,
        alarm.last_update_time,
        true});
  }
  return records;
}

static bool sync_active_alarms(DbContext& db, const std::vector<ActiveAlarm>& alarms) {
  if (!connect_db(db)) return false;
  if (!exec_sql(db, "BEGIN")) return false;

  for (const auto& alarm : alarms) {
    std::ostringstream value_text;
    value_text << double_to_string(alarm.value);
    if (!alarm.unit.empty()) value_text << " " << alarm.unit;

    std::ostringstream sql;
    sql << "INSERT INTO alarm_events("
        << "alarm_id, point_id, device_id, severity, message, value_text, active_since, last_seen_at, status"
        << ") VALUES ("
        << "'" << sql_escape(alarm.id) << "',"
        << "'" << sql_escape(alarm.point_id) << "',"
        << "'" << sql_escape(alarm.device_id) << "',"
        << "'" << sql_escape(alarm.level) << "',"
        << "'" << sql_escape(alarm.message) << "',"
        << "'" << sql_escape(value_text.str()) << "',"
        << "'" << sql_escape(ts_to_iso8601(alarm.trigger_time)) << "',"
        << "NOW(),"
        << "'active'"
        << ") ON CONFLICT (alarm_id) DO UPDATE SET "
        << "point_id = EXCLUDED.point_id, "
        << "device_id = EXCLUDED.device_id, "
        << "severity = EXCLUDED.severity, "
        << "message = EXCLUDED.message, "
        << "value_text = EXCLUDED.value_text, "
        << "last_seen_at = NOW(), "
        << "cleared_at = NULL, "
        << "status = CASE "
        << "WHEN alarm_events.status IN ('acked', 'silenced') THEN alarm_events.status "
        << "ELSE 'active' END";
    if (!exec_sql(db, sql.str())) {
      exec_sql(db, "ROLLBACK");
      return false;
    }
  }

  std::ostringstream clear_sql;
  clear_sql << "UPDATE alarm_events "
            << "SET cleared_at = NOW(), "
            << "status = CASE WHEN status = 'active' THEN 'cleared' ELSE status END "
            << "WHERE cleared_at IS NULL";
  if (!alarms.empty()) {
    clear_sql << " AND alarm_id NOT IN (";
    for (size_t i = 0; i < alarms.size(); ++i) {
      if (i > 0) clear_sql << ",";
      clear_sql << "'" << sql_escape(alarms[i].id) << "'";
    }
    clear_sql << ")";
  }
  if (!exec_sql(db, clear_sql.str())) {
    exec_sql(db, "ROLLBACK");
    return false;
  }

  if (!exec_sql(db, "COMMIT")) {
    exec_sql(db, "ROLLBACK");
    return false;
  }
  return true;
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::string shm_name = openems::rt_db::default_shm_name();
  if (argc > 1) shm_name = argv[1];

  DbContext db;
  const char* env_db_url = std::getenv("OPENEMS_DB_URL");
  db.db_url = env_db_url ? std::string(env_db_url) : "";
  if (db.db_url.empty()) {
    OPENEMS_LOG_F("Alarm", "OPENEMS_DB_URL is not set.");
    return 1;
  }

  auto api_result = openems::libpq::load_libpq();
  if (!api_result.is_ok()) {
    OPENEMS_LOG_F("Alarm", "libpq not available: " + api_result.error_msg());
    return 1;
  }
  db.api = std::move(api_result.value());
  if (!connect_db(db)) {
    OPENEMS_LOG_F("Alarm", db.last_error);
    return 1;
  }

  OPENEMS_LOG_I("Alarm", "Attaching to RtDb: " + shm_name);
  auto rtdb_result = openems::rt_db::RtDb::attach(shm_name);
  if (!rtdb_result.is_ok()) {
    OPENEMS_LOG_F("Alarm", "Attach failed: " + rtdb_result.error_msg());
    return 1;
  }
  auto* rt_db = rtdb_result.value();

  auto rules = load_alarm_rules(db);
  if (rules.empty()) {
    OPENEMS_LOG_W("Alarm", "No enabled alarm rules found in PostgreSQL.");
  }

  OPENEMS_LOG_I("Alarm", "Running. Checking every 2 seconds, reloading rules every 30 seconds.");
  std::unordered_map<std::string, uint64_t> trigger_times;
  int reload_cycle = 0;
  int exit_code = 0;
  while (g_running.load()) {
    if (!db.conn || db.api.PQstatus(db.conn) != 0) {
      if (!reconnect_db(db)) {
        OPENEMS_LOG_F("Alarm", "PostgreSQL reconnect failed: " + db.last_error);
        exit_code = 1;
        break;
      }
      rules = load_alarm_rules(db);
    }

    // Periodic reload: re-read alarm rules from PostgreSQL every 30 seconds
    if (reload_cycle % 15 == 0) {
      auto new_rules = load_alarm_rules(db);
      if (!new_rules.empty()) {
        rules = std::move(new_rules);
      }
    }
    ++reload_cycle;

    auto alarms = collect_active_alarms(rt_db, rules);
    std::unordered_map<std::string, uint64_t> active_trigger_times;
    for (auto& alarm : alarms) {
      auto it = trigger_times.find(alarm.id);
      alarm.trigger_time = (it != trigger_times.end()) ? it->second : now_ms();
      active_trigger_times[alarm.id] = alarm.trigger_time;
      if (alarm.level == "critical") {
        OPENEMS_LOG_E("Alarm", alarm.message + ": " + alarm.point_id + "=" + double_to_string(alarm.value));
      } else {
        OPENEMS_LOG_W("Alarm", alarm.message + ": " + alarm.point_id + "=" + double_to_string(alarm.value));
      }
    }
    trigger_times.swap(active_trigger_times);

    if (!sync_active_alarms(db, alarms)) {
      OPENEMS_LOG_F("Alarm", "Failed to sync alarms to PostgreSQL: " + db.last_error);
      exit_code = 1;
      break;
    }
    auto runtime_records = build_alarm_runtime_records(alarms);
    auto shm_result = rt_db->replace_active_alarms(runtime_records);
    if (!shm_result.is_ok()) {
      OPENEMS_LOG_E("Alarm", "Failed to update alarm_active table: " + shm_result.error_msg());
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }

  if (db.conn && db.api.PQstatus(db.conn) == 0) {
    sync_active_alarms(db, {});
  }
  rt_db->replace_active_alarms({});
  if (db.conn) {
    db.api.PQfinish(db.conn);
    db.conn = nullptr;
  }
  delete rt_db;
  OPENEMS_LOG_I("Alarm", "Shutdown.");
  return exit_code;
}
