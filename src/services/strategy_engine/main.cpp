// Strategy Engine process: loads strategy config from PostgreSQL,
// attaches to RtDb, runs control strategies in a single-threaded loop.
#include "openems/core/point_handle.h"
#include "openems/core/command_handle.h"
#include "openems/libpq/libpq_api.h"
#include "openems/rt_db/rt_db.h"
#include "openems/strategy/anti_reverse_flow.h"
#include "openems/strategy/soc_protection.h"
#include "openems/strategy/strategy_types.h"
#include "openems/utils/logger.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running = false; }

struct DbContext {
  std::string db_url;
  openems::libpq::LibpqApi api;
  void* conn = nullptr;
  std::string last_error;
};

static uint64_t now_ms() {
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             now.time_since_epoch())
      .count();
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
  oss << std::fixed << std::setprecision(4) << value;
  return oss.str();
}

static std::string json_escape(const std::string& value) {
  std::string result;
  result.reserve(value.size() + 8);
  for (char ch : value) {
    switch (ch) {
      case '\\': result += "\\\\"; break;
      case '"': result += "\\\""; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          std::ostringstream hex;
          hex << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(ch));
          result += hex.str();
        } else {
          result += ch;
        }
        break;
    }
  }
  return result;
}

static std::string json_summary_literal(const std::string& summary) {
  return "'{\"summary\":\"" + json_escape(summary) + "\"}'::jsonb";
}

static bool connect_db(DbContext& db) {
  if (!db.conn) {
    db.conn = db.api.PQconnectdb(db.db_url.c_str());
  }
  if (!db.conn || db.api.PQstatus(db.conn) != 0) {
    db.last_error = "PostgreSQL connect failed: " +
                    std::string(db.conn ? db.api.PQerrorMessage(db.conn)
                                        : "failed to allocate connection");
    if (db.conn) {
      db.api.PQfinish(db.conn);
      db.conn = nullptr;
    }
    return false;
  }
  db.last_error.clear();
  return true;
}

static bool exec_sql(DbContext& db, const std::string& sql,
                     int expected_status = 1) {
  if (!db.conn && !connect_db(db)) return false;
  void* res = db.api.PQexec(db.conn, sql.c_str());
  if (!res || db.api.PQresultStatus(res) != expected_status) {
    db.last_error = db.conn ? db.api.PQerrorMessage(db.conn)
                            : "PostgreSQL command failed";
    if (res) db.api.PQclear(res);
    return false;
  }
  db.api.PQclear(res);
  db.last_error.clear();
  return true;
}

// ---- Strategy Config Loading ----

struct LoadedStrategy {
  openems::strategy::StrategyDefinition def;
  openems::strategy::StrategyParams params;
  std::unique_ptr<openems::strategy::IStrategy> instance;
};

struct PvRecoveryRuntime {
  int confirm_cycles = 0;
};

static std::string pq_get_str(void* res, int row, int col, DbContext& db) {
  if (!res) return "";
  const char* val = db.api.PQgetvalue(res, row, col);
  return val ? std::string(val) : "";
}

static double pq_get_double(void* res, int row, int col, DbContext& db) {
  std::string s = pq_get_str(res, row, col, db);
  if (s.empty()) return 0.0;
  return std::atof(s.c_str());
}

static int pq_get_int(void* res, int row, int col, DbContext& db) {
  std::string s = pq_get_str(res, row, col, db);
  if (s.empty()) return 0;
  return std::atoi(s.c_str());
}

static uint64_t pq_get_ts_ms(void* res, int row, int col, DbContext& db) {
  // Parse ISO8601 timestamp from PostgreSQL to epoch ms
  std::string s = pq_get_str(res, row, col, db);
  if (s.empty()) return 0;
  // Simple parse: "2026-04-29T12:00:00.000+00:00"
  std::tm tm{};
  int frac_ms = 0;
#ifdef _WIN32
  sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%d.%d",
           &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
           &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &frac_ms);
#else
  sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d.%d",
         &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
         &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &frac_ms);
#endif
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  tm.tm_isdst = -1;
  std::time_t secs = std::mktime(&tm);
  return static_cast<uint64_t>(secs) * 1000 + frac_ms;
}

static std::vector<LoadedStrategy> load_strategies(
    DbContext& db,
    openems::rt_db::RtDb* rtdb) {
  std::vector<LoadedStrategy> result;
  if (!connect_db(db)) {
    OPENEMS_LOG_E("StrategyEngine", db.last_error);
    return result;
  }

  // Load strategy definitions
  const char* def_sql =
      "SELECT sd.id, sd.name, sd.type, sd.enabled, sd.site_id, sd.device_id, "
      "sd.priority, sd.cycle_ms "
      "FROM strategy_definitions sd "
      "WHERE sd.enabled = TRUE "
      "ORDER BY sd.priority ASC, sd.id ASC";

  void* res = db.api.PQexec(db.conn, def_sql);
  if (!res || db.api.PQresultStatus(res) != 2) {
    OPENEMS_LOG_E("StrategyEngine", "Failed to load strategy definitions");
    if (res) db.api.PQclear(res);
    return result;
  }

  int count = db.api.PQntuples(res);
  OPENEMS_LOG_I("StrategyEngine",
                "Loaded " + std::to_string(count) + " enabled strategies");

  for (int i = 0; i < count; ++i) {
    LoadedStrategy ls;
    ls.def.id = pq_get_str(res, i, 0, db);
    ls.def.name = pq_get_str(res, i, 1, db);
    ls.def.type = openems::strategy::strategy_type_from_string(
        pq_get_str(res, i, 2, db));
    ls.def.enabled = (pq_get_str(res, i, 3, db) == "t");
    ls.def.site_id = pq_get_str(res, i, 4, db);
    ls.def.device_id = pq_get_str(res, i, 5, db);
    ls.def.priority = pq_get_int(res, i, 6, db);
    ls.def.cycle_ms = pq_get_int(res, i, 7, db);
    result.push_back(std::move(ls));
  }
  db.api.PQclear(res);

  // Load bindings and params for each strategy
  for (auto& ls : result) {
    // Load bindings
    std::string bind_sql =
        "SELECT role, point_id FROM strategy_bindings "
        "WHERE strategy_id = '" +
        sql_escape(ls.def.id) + "'";
    res = db.api.PQexec(db.conn, bind_sql.c_str());
    if (res && db.api.PQresultStatus(res) == 2) {
      int n = db.api.PQntuples(res);
      for (int j = 0; j < n; ++j) {
        openems::strategy::StrategyBinding b;
        b.role = pq_get_str(res, j, 0, db);
        b.point_id = pq_get_str(res, j, 1, db);
        ls.def.bindings.push_back(b);
      }
    }
    if (res) db.api.PQclear(res);

    // Load params
    std::string param_sql =
        "SELECT param_key, param_value FROM strategy_params "
        "WHERE strategy_id = '" +
        sql_escape(ls.def.id) + "'";
    res = db.api.PQexec(db.conn, param_sql.c_str());
    if (res && db.api.PQresultStatus(res) == 2) {
      int n = db.api.PQntuples(res);
      for (int j = 0; j < n; ++j) {
        std::string key = pq_get_str(res, j, 0, db);
        std::string val = pq_get_str(res, j, 1, db);
        ls.params.parse(key, val);
      }
    }
    if (res) db.api.PQclear(res);

    // Create strategy instance
    switch (ls.def.type) {
      case openems::strategy::StrategyType::AntiReverseFlow:
        ls.instance = std::make_unique<openems::strategy::AntiReverseFlow>(
            ls.def, ls.params, rtdb);
        break;
      case openems::strategy::StrategyType::SocProtection:
        ls.instance = std::make_unique<openems::strategy::SocProtection>(
            ls.def, ls.params, rtdb);
        break;
    }
  }

  return result;
}

static void load_manual_overrides(DbContext& db,
    std::unordered_map<std::string, uint64_t>& overrides) {
  overrides.clear();
  if (!connect_db(db)) return;

  const char* sql =
      "SELECT strategy_id, manual_override_until "
      "FROM strategy_runtime_state "
      "WHERE manual_override_until IS NOT NULL "
      "AND manual_override_until > NOW()";

  void* res = db.api.PQexec(db.conn, sql);
  if (res && db.api.PQresultStatus(res) == 2) {
    int n = db.api.PQntuples(res);
    uint64_t now = now_ms();
    for (int i = 0; i < n; ++i) {
      std::string sid = pq_get_str(res, i, 0, db);
      uint64_t until = pq_get_ts_ms(res, i, 1, db);
      if (until > now) {
        overrides[sid] = until;
      }
    }
  }
  if (res) db.api.PQclear(res);
}

static std::string find_binding_point_id(
    const openems::strategy::StrategyDefinition& def,
    const std::string& role) {
  for (const auto& binding : def.bindings) {
    if (binding.role == role ||
        openems::strategy::binding_role_base(binding.role) == role) {
      return binding.point_id;
    }
  }
  return {};
}

struct BessBindingGroup {
  std::string key;
  std::string power_point_id;
  std::string soc_point_id;
  std::string run_state_point_id;
  std::string setpoint_point_id;
};

struct PvBindingGroup {
  std::string key;
  std::string power_point_id;
  std::string run_state_point_id;
  std::string limit_setpoint_point_id;
};

static std::string join_strings(const std::vector<std::string>& values,
                                const std::string& sep) {
  std::ostringstream oss;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) oss << sep;
    oss << values[i];
  }
  return oss.str();
}

static std::vector<BessBindingGroup> collect_bess_groups(
    const openems::strategy::StrategyDefinition* def_a,
    const openems::strategy::StrategyDefinition* def_b) {
  std::unordered_map<std::string, BessBindingGroup> groups;
  auto collect = [&](const openems::strategy::StrategyDefinition* def) {
    if (!def) return;
    for (const auto& binding : def->bindings) {
      const std::string base = openems::strategy::binding_role_base(binding.role);
      if (base != "bess_power" && base != "bess_soc" &&
          base != "bess_run_state" && base != "bess_power_setpoint") {
        continue;
      }
      const std::string key = openems::strategy::binding_role_group(binding.role);
      auto& group = groups[key];
      group.key = key;
      if (base == "bess_power") group.power_point_id = binding.point_id;
      if (base == "bess_soc") group.soc_point_id = binding.point_id;
      if (base == "bess_run_state") group.run_state_point_id = binding.point_id;
      if (base == "bess_power_setpoint") group.setpoint_point_id = binding.point_id;
    }
  };
  collect(def_a);
  collect(def_b);
  std::vector<BessBindingGroup> result;
  for (auto& entry : groups) result.push_back(entry.second);
  std::sort(result.begin(), result.end(),
            [](const BessBindingGroup& a, const BessBindingGroup& b) {
              return a.key < b.key;
            });
  return result;
}

static std::vector<PvBindingGroup> collect_pv_groups(
    const openems::strategy::StrategyDefinition* def) {
  std::unordered_map<std::string, PvBindingGroup> groups;
  if (def) {
    for (const auto& binding : def->bindings) {
      const std::string base = openems::strategy::binding_role_base(binding.role);
      if (base != "pv_power" && base != "pv_run_state" &&
          base != "pv_power_limit_setpoint") {
        continue;
      }
      const std::string key = openems::strategy::binding_role_group(binding.role);
      auto& group = groups[key];
      group.key = key;
      if (base == "pv_power") group.power_point_id = binding.point_id;
      if (base == "pv_run_state") group.run_state_point_id = binding.point_id;
      if (base == "pv_power_limit_setpoint") group.limit_setpoint_point_id = binding.point_id;
    }
  }
  std::vector<PvBindingGroup> result;
  for (auto& entry : groups) result.push_back(entry.second);
  std::sort(result.begin(), result.end(),
            [](const PvBindingGroup& a, const PvBindingGroup& b) {
              return a.key < b.key;
            });
  return result;
}

static bool read_point_value(openems::rt_db::RtDb* rt_db,
                             const std::string& point_id,
                             double& value_out,
                             std::string& error_out) {
  if (point_id.empty()) {
    error_out = "binding not configured";
    return false;
  }

  openems::core::PointHandle handle(point_id, rt_db);
  auto result = handle.read_value();
  if (!result.is_ok() || !handle.is_valid()) {
    error_out = "point read failed or invalid: " + point_id;
    return false;
  }

  value_out = result.value();
  error_out.clear();
  return true;
}

static std::pair<bool, std::string> submit_target(openems::rt_db::RtDb* rt_db,
                                                  const std::string& point_id,
                                                  double target_value,
                                                  double deadband) {
  if (point_id.empty()) {
    return {false, "target binding not configured"};
  }
  openems::core::CommandHandle handle(point_id, rt_db, deadband, 0.1);
  auto submit_result = handle.submit(target_value);
  if (!submit_result.is_ok()) {
    return {false, "command submit failed: " + submit_result.error_msg()};
  }
  return {submit_result.value() == "submitted", submit_result.value()};
}

static void write_action_log(DbContext& db,
                             const std::string& strategy_id,
                             const std::string& action_type,
                             const std::string& target_point_id,
                             double desired_value,
                             double result_value,
                             const std::string& suppress_reason,
                             const std::string& input_summary,
                             const std::string& result_status,
                             const std::string& details) {
  if (!connect_db(db)) return;
  std::ostringstream sql;
  sql << "INSERT INTO strategy_action_logs("
      << "strategy_id, action_type, target_point_id, desired_value, "
      << "result_value, suppress_reason, input_summary, result, details"
      << ") VALUES ("
      << "'" << sql_escape(strategy_id) << "',"
      << "'" << sql_escape(action_type) << "',"
      << (target_point_id.empty() ? "NULL" : "'" + sql_escape(target_point_id) + "'") << ","
      << double_to_string(desired_value) << ","
      << double_to_string(result_value) << ","
      << "'" << sql_escape(suppress_reason) << "',"
      << json_summary_literal(input_summary) << ","
      << "'" << sql_escape(result_status) << "',"
      << "'" << sql_escape(details) << "'"
      << ")";
  exec_sql(db, sql.str());
}

static void update_runtime_state(DbContext& db,
                                 const std::string& strategy_id,
                                 double target_value,
                                 const std::string& target_point_id,
                                 bool suppressed,
                                 const std::string& suppress_reason,
                                 const std::string& input_summary,
                                 const std::string& error) {
  if (!connect_db(db)) return;
  std::ostringstream sql;
  sql << "INSERT INTO strategy_runtime_state("
      << "strategy_id, last_execution_time, current_target_value, "
      << "current_target_point_id, suppressed, suppress_reason, "
      << "last_error, input_summary, updated_at"
      << ") VALUES ("
      << "'" << sql_escape(strategy_id) << "',"
      << "NOW(),"
      << double_to_string(target_value) << ","
      << (target_point_id.empty() ? "NULL" : "'" + sql_escape(target_point_id) + "'") << ","
      << (suppressed ? "TRUE" : "FALSE") << ","
      << "'" << sql_escape(suppress_reason) << "',"
      << "'" << sql_escape(error) << "',"
      << json_summary_literal(input_summary) << ","
      << "NOW()"
      << ") ON CONFLICT (strategy_id) DO UPDATE SET "
      << "last_execution_time = EXCLUDED.last_execution_time, "
      << "current_target_value = EXCLUDED.current_target_value, "
      << "current_target_point_id = EXCLUDED.current_target_point_id, "
      << "suppressed = EXCLUDED.suppressed, "
      << "suppress_reason = EXCLUDED.suppress_reason, "
      << "last_error = EXCLUDED.last_error, "
      << "input_summary = EXCLUDED.input_summary, "
      << "updated_at = NOW()";
  exec_sql(db, sql.str());
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
    OPENEMS_LOG_F("StrategyEngine", "OPENEMS_DB_URL is not set.");
    return 1;
  }

  auto api_result = openems::libpq::load_libpq();
  if (!api_result.is_ok()) {
    OPENEMS_LOG_F("StrategyEngine",
                  "libpq not available: " + api_result.error_msg());
    return 1;
  }
  db.api = std::move(api_result.value());
  if (!connect_db(db)) {
    OPENEMS_LOG_F("StrategyEngine", db.last_error);
    return 1;
  }

  OPENEMS_LOG_I("StrategyEngine", "Attaching to RtDb: " + shm_name);
  auto rtdb_result = openems::rt_db::RtDb::attach(shm_name);
  if (!rtdb_result.is_ok()) {
    OPENEMS_LOG_F("StrategyEngine",
                  "Attach failed: " + rtdb_result.error_msg());
    return 1;
  }
  auto* rt_db = rtdb_result.value();

  // Initial strategy load
  auto strategies = load_strategies(db, rt_db);
  if (strategies.empty()) {
    OPENEMS_LOG_W("StrategyEngine",
                  "No enabled strategies found in PostgreSQL.");
  }

  std::unordered_map<std::string, uint64_t> manual_overrides;
  load_manual_overrides(db, manual_overrides);
  std::unordered_map<std::string, PvRecoveryRuntime> pv_recovery_state;

  int reload_counter = 0;
  int reload_interval = 30;
  if (const char* env_reload = std::getenv("OPENEMS_STRATEGY_RELOAD_INTERVAL")) {
    int parsed = std::atoi(env_reload);
    if (parsed > 0) reload_interval = parsed;
  }

  OPENEMS_LOG_I("StrategyEngine", "Running. Cycle interval: 1000ms");

  constexpr double kPvRecoveryDeadbandKw = 2.0;
  constexpr int kPvRecoveryConfirmCycles = 3;

  while (g_running.load()) {
    uint64_t cycle_start = now_ms();

    // Periodically reload config from DB
    if (reload_counter % reload_interval == 0) {
      strategies = load_strategies(db, rt_db);
      load_manual_overrides(db, manual_overrides);
    }
    ++reload_counter;

    // Find anti-reverse-flow and SOC protection strategies
    LoadedStrategy* arf_strategy = nullptr;
    LoadedStrategy* soc_strategy = nullptr;
    openems::strategy::AntiReverseFlow* arf = nullptr;
    openems::strategy::SocProtection* soc = nullptr;

    for (auto& ls : strategies) {
      if (ls.def.type == openems::strategy::StrategyType::AntiReverseFlow) {
        arf_strategy = &ls;
        arf = static_cast<openems::strategy::AntiReverseFlow*>(
            ls.instance.get());
      } else if (ls.def.type == openems::strategy::StrategyType::SocProtection) {
        soc_strategy = &ls;
        soc = static_cast<openems::strategy::SocProtection*>(
            ls.instance.get());
      }
    }

    if (!arf && !soc) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }

    // Check manual override
    uint64_t now = now_ms();
    bool overridden = false;
    std::string override_device;

    for (auto& ls : strategies) {
      auto it = manual_overrides.find(ls.def.id);
      if (it != manual_overrides.end() && it->second > now) {
        overridden = true;
        override_device = ls.def.device_id;
        break;
      }
    }

    double final_target = 0.0;
    std::string target_point_id;
    std::ostringstream input_summary;
    bool final_suppressed = false;
    std::string final_suppress_reason;

    if (overridden) {
      final_suppressed = true;
      final_suppress_reason =
          "manual override active for device: " + override_device;
    } else {
      std::string arf_setpoint_id;
      bool arf_suppressed = false;
      std::string arf_suppress_reason;
      bool soc_charge_clamped = false;
      bool soc_runtime_suppressed = false;
      std::string soc_runtime_reason = "SOC in normal range";
      bool pv_action_active = false;
      bool pv_recovery_active = false;
      std::string pv_action_reason;
      std::string pv_setpoint_id;
      double pv_target_limit_pct = 100.0;
      bool pv_command_submitted = false;
      std::string pv_submit_message;
      const auto bess_groups = collect_bess_groups(
          arf_strategy ? &arf_strategy->def : nullptr,
          soc_strategy ? &soc_strategy->def : nullptr);
      const auto pv_groups = collect_pv_groups(
          arf_strategy ? &arf_strategy->def : nullptr);

      // Step 1: Anti-reverse-flow calculates target only.
      if (arf) {
        auto arf_result = arf->calculate_target();

        input_summary << "ARF:target=" << double_to_string(arf_result.target_power_kw);

        final_target = arf_result.target_power_kw;
        arf_suppressed = arf_result.suppressed;
        arf_suppress_reason = arf_result.suppress_reason;
        final_suppressed = arf_suppressed;
        final_suppress_reason = arf_suppress_reason;

        for (const auto& b : arf->definition().bindings) {
          if (b.role == "bess_power_setpoint") arf_setpoint_id = b.point_id;
        }

        if (arf_result.suppressed) {
          input_summary << ",suppressed=" << arf_result.suppress_reason;
        }
      }

      // Step 2: SOC protection clamps the target before any command is sent.
      if (soc && arf) {
        double arf_target = final_target;
        auto clamp = soc->clamp(final_target);
        input_summary << " | SOC:clamped=" << double_to_string(clamp.clamped_kw);

        if (clamp.suppressed) {
          input_summary << ",suppressed=" << clamp.reason;
          final_target = clamp.clamped_kw;
          final_suppressed = true;
          final_suppress_reason = clamp.reason;
          soc_runtime_suppressed = true;
          soc_runtime_reason = clamp.reason;
          if (arf_target < 0.0 &&
              clamp.reason.find("high limit") != std::string::npos &&
              std::abs(clamp.clamped_kw) < 1e-6) {
            soc_charge_clamped = true;
          }
        }
      }

      if (arf) {
        if (arf_strategy && arf_strategy->params.enable_pv_curtailment) {
          auto& pv_runtime = pv_recovery_state[arf_strategy->def.id];
          double grid_power_kw = 0.0;
          std::string read_error;
          if (read_point_value(rt_db, find_binding_point_id(arf_strategy->def, "grid_power"),
                               grid_power_kw, read_error)) {
            bool reverse_flow_active =
                grid_power_kw < (arf_strategy->params.export_limit_kw - 1e-6);
            bool bess_unavailable =
                arf_suppressed &&
                arf_suppress_reason.find("BESS not running") != std::string::npos;

            bool pv_running = false;
            double total_pv_power_kw = 0.0;
            double current_limit_sum = 0.0;
            int current_limit_count = 0;
            std::vector<std::string> pv_setpoint_ids;

            for (const auto& pv_group : pv_groups) {
              bool group_running = true;
              if (!pv_group.run_state_point_id.empty()) {
                double pv_run_state = 0.0;
                if (!read_point_value(rt_db, pv_group.run_state_point_id, pv_run_state, read_error)) {
                  group_running = false;
                } else {
                  group_running = std::abs(pv_run_state) > 1e-6;
                }
              }
              if (!group_running) {
                continue;
              }
              pv_running = true;

              if (!pv_group.limit_setpoint_point_id.empty()) {
                pv_setpoint_ids.push_back(pv_group.limit_setpoint_point_id);
                double current_limit = arf_strategy->params.pv_limit_max_pct;
                if (read_point_value(rt_db, pv_group.limit_setpoint_point_id, current_limit, read_error)) {
                  current_limit_sum += current_limit;
                  ++current_limit_count;
                }
              }

              if (!pv_group.power_point_id.empty()) {
                double current_pv_power_value = 0.0;
                if (read_point_value(rt_db, pv_group.power_point_id, current_pv_power_value, read_error)) {
                  double current_pv_power_kw = current_pv_power_value;
                  if (std::abs(current_pv_power_kw) >
                      (arf_strategy->params.pv_rated_power_kw * 2.0)) {
                    current_pv_power_kw /= 1000.0;
                  }
                  total_pv_power_kw += current_pv_power_kw;
                }
              }
            }

            if (pv_running && !pv_setpoint_ids.empty() &&
                arf_strategy->params.pv_rated_power_kw > 0.0) {
              pv_setpoint_id = join_strings(pv_setpoint_ids, ",");
              double current_limit_pct =
                  current_limit_count > 0
                      ? (current_limit_sum / static_cast<double>(current_limit_count))
                      : arf_strategy->params.pv_limit_max_pct;
              {
                double current_pv_power_kw = total_pv_power_kw;
                const double min_limit =
                    (arf_strategy->params.pv_limit_min_pct > 0.0)
                        ? arf_strategy->params.pv_limit_min_pct
                        : 0.0;
                const double max_limit =
                    (arf_strategy->params.pv_limit_max_pct > min_limit)
                        ? arf_strategy->params.pv_limit_max_pct
                        : min_limit;
                const double recovery_ready_grid_kw =
                    arf_strategy->params.export_limit_kw + kPvRecoveryDeadbandKw;
                const bool recovery_headroom_available =
                    grid_power_kw > recovery_ready_grid_kw;

                if (reverse_flow_active && (soc_charge_clamped || bess_unavailable)) {
                  pv_runtime.confirm_cycles = 0;
                  const double excess_export_kw =
                      arf_strategy->params.export_limit_kw - grid_power_kw;
                  const double desired_pv_power_kw =
                      (current_pv_power_kw - excess_export_kw > 0.0)
                          ? (current_pv_power_kw - excess_export_kw)
                          : 0.0;
                  double desired_limit_pct =
                      (desired_pv_power_kw / arf_strategy->params.pv_rated_power_kw) * 100.0;
                  if (desired_limit_pct < min_limit) desired_limit_pct = min_limit;
                  if (desired_limit_pct > max_limit) desired_limit_pct = max_limit;

                  if (std::abs(desired_limit_pct - current_limit_pct) > 0.1) {
                    pv_target_limit_pct = desired_limit_pct;
                    int submit_ok_count = 0;
                    std::vector<std::string> submit_parts;
                    for (const auto& setpoint_id : pv_setpoint_ids) {
                      auto [submitted, submit_message] = submit_target(
                          rt_db, setpoint_id, pv_target_limit_pct, 0.2);
                      if (submitted || submit_message == "debounced") {
                        ++submit_ok_count;
                      }
                      submit_parts.push_back(setpoint_id + ":" + submit_message);
                    }
                    pv_command_submitted = submit_ok_count > 0;
                    pv_submit_message = join_strings(submit_parts, ";");
                  } else {
                    pv_target_limit_pct = current_limit_pct;
                    pv_submit_message = "debounced";
                  }

                  pv_action_active = true;
                  pv_action_reason = soc_charge_clamped
                      ? "PV curtailment compensation active after SOC high clamp"
                      : "PV curtailment compensation active because BESS is unavailable";
                  input_summary << " | PV:limit="
                                << double_to_string(pv_target_limit_pct)
                                << "%,grid=" << double_to_string(grid_power_kw)
                                << ",reason=" << pv_action_reason;
                } else if (current_limit_pct < (max_limit - 0.1)) {
                  if (recovery_headroom_available) {
                    pv_runtime.confirm_cycles += 1;
                  } else {
                    pv_runtime.confirm_cycles = 0;
                  }

                  if (pv_runtime.confirm_cycles >= kPvRecoveryConfirmCycles) {
                    const double grid_headroom_kw =
                        grid_power_kw - recovery_ready_grid_kw;
                    double allowed_recovery_pct =
                        (grid_headroom_kw / arf_strategy->params.pv_rated_power_kw) * 100.0;
                    if (allowed_recovery_pct < 0.0) allowed_recovery_pct = 0.0;

                    double recovery_step_pct =
                        arf_strategy->params.pv_limit_recovery_step_pct;
                    if (allowed_recovery_pct < recovery_step_pct) {
                      recovery_step_pct = allowed_recovery_pct;
                    }

                    if (recovery_step_pct > 0.1) {
                      pv_target_limit_pct = current_limit_pct + recovery_step_pct;
                      if (pv_target_limit_pct > max_limit) {
                        pv_target_limit_pct = max_limit;
                      }
                      int submit_ok_count = 0;
                      std::vector<std::string> submit_parts;
                      for (const auto& setpoint_id : pv_setpoint_ids) {
                        auto [submitted, submit_message] = submit_target(
                            rt_db, setpoint_id, pv_target_limit_pct, 0.2);
                        if (submitted || submit_message == "debounced") {
                          ++submit_ok_count;
                        }
                        submit_parts.push_back(setpoint_id + ":" + submit_message);
                      }
                      pv_command_submitted = submit_ok_count > 0;
                      pv_submit_message = join_strings(submit_parts, ";");
                      pv_recovery_active = true;
                      pv_action_reason = "PV curtailment recovering toward 100%";
                      input_summary << " | PV:recover="
                                    << double_to_string(pv_target_limit_pct)
                                    << "%,grid=" << double_to_string(grid_power_kw)
                                    << ",headroom=" << double_to_string(grid_headroom_kw)
                                    << ",confirm=" << pv_runtime.confirm_cycles;
                    } else {
                      pv_submit_message = "recovery headroom too small";
                      input_summary << " | PV:hold="
                                    << double_to_string(current_limit_pct)
                                    << "%,grid=" << double_to_string(grid_power_kw)
                                    << ",headroom=0";
                    }
                  } else {
                    pv_submit_message = "recovery confirmation pending";
                    input_summary << " | PV:hold="
                                  << double_to_string(current_limit_pct)
                                  << "%,grid=" << double_to_string(grid_power_kw)
                                  << ",confirm=" << pv_runtime.confirm_cycles
                                  << "/" << kPvRecoveryConfirmCycles;
                  }
                } else {
                  pv_runtime.confirm_cycles = 0;
                }
              }
            }
          }
        }

        bool submitted = false;
        std::string submit_message;
        double applied_bess_target = final_target;
        std::vector<std::string> bess_target_point_ids;

        if (!bess_groups.empty()) {
          std::vector<BessBindingGroup> eligible_bess_groups;
          std::string read_error;
          for (const auto& group : bess_groups) {
            if (group.setpoint_point_id.empty()) continue;

            bool running = true;
            if (!group.run_state_point_id.empty()) {
              double run_state = 0.0;
              if (!read_point_value(rt_db, group.run_state_point_id, run_state, read_error) ||
                  std::abs(run_state) < 1e-6) {
                running = false;
              }
            }
            if (!running) continue;

            bool eligible = true;
            if (!group.soc_point_id.empty()) {
              double soc_value = 0.0;
              if (read_point_value(rt_db, group.soc_point_id, soc_value, read_error)) {
                if (soc_strategy && final_target < 0.0 &&
                    soc_value >= soc_strategy->params.soc_high) {
                  eligible = false;
                }
                if (soc_strategy && final_target > 0.0 &&
                    soc_value <= soc_strategy->params.soc_low) {
                  eligible = false;
                }
              }
            }
            if (eligible) eligible_bess_groups.push_back(group);
          }

          if (!eligible_bess_groups.empty()) {
            const double per_device_target =
                final_target / static_cast<double>(eligible_bess_groups.size());
            std::vector<std::string> submit_parts;
            int submit_ok_count = 0;
            applied_bess_target = 0.0;
            for (const auto& group : eligible_bess_groups) {
              auto [group_submitted, group_message] = submit_target(
                  rt_db, group.setpoint_point_id, per_device_target, 0.1);
              if (group_submitted || group_message == "debounced") {
                ++submit_ok_count;
              }
              submit_parts.push_back(group.setpoint_point_id + ":" + group_message);
              bess_target_point_ids.push_back(group.setpoint_point_id);
              applied_bess_target += per_device_target;
            }
            submitted = submit_ok_count > 0;
            submit_message = join_strings(submit_parts, ";");
          } else {
            applied_bess_target = 0.0;
            submit_message = "no eligible BESS setpoint available";
            if (std::abs(final_target) > 1e-6 && !final_suppressed) {
              final_suppressed = true;
              final_suppress_reason = submit_message;
            }
          }
        } else {
          auto submit_result = submit_target(rt_db, arf_setpoint_id, final_target, 0.1);
          submitted = submit_result.first;
          submit_message = submit_result.second;
          if (!arf_setpoint_id.empty()) {
            bess_target_point_ids.push_back(arf_setpoint_id);
          }
        }

        final_target = applied_bess_target;
        target_point_id = join_strings(bess_target_point_ids, ",");
        if (submitted) {
          input_summary << ",sent=1,cmd=" << submit_message;
        } else {
          input_summary << ",sent=0,cmd=" << submit_message;
          if (!final_suppressed) {
            final_suppressed = true;
            final_suppress_reason = submit_message;
          }
        }

        if (pv_action_active || pv_recovery_active) {
          if (pv_command_submitted) {
            final_suppressed = false;
            final_suppress_reason = pv_action_reason;
          } else if (!pv_submit_message.empty() && pv_submit_message != "debounced") {
            final_suppressed = true;
            final_suppress_reason = pv_submit_message;
          } else {
            final_suppressed = false;
            final_suppress_reason = pv_action_reason;
          }
        }

        arf->mark_target_applied(final_target);
        update_runtime_state(db, arf->definition().id,
                             pv_action_active || pv_recovery_active ? pv_target_limit_pct : final_target,
                             pv_action_active || pv_recovery_active ? pv_setpoint_id : target_point_id,
                             final_suppressed,
                             final_suppress_reason,
                             input_summary.str(), "");
        write_action_log(db, arf->definition().id,
                         pv_action_active ? "pv_curtailment" :
                         (pv_recovery_active ? "pv_recovery" : (submitted ? "command" : "suppress")),
                         pv_action_active || pv_recovery_active ? pv_setpoint_id : target_point_id,
                         pv_action_active || pv_recovery_active ? pv_target_limit_pct : final_target,
                         0.0,
                         final_suppress_reason,
                         input_summary.str(),
                         (pv_action_active || pv_recovery_active)
                             ? ((pv_command_submitted || pv_submit_message == "debounced") ? "ok" : "suppressed")
                             : (submitted ? "ok" : "suppressed"),
                         (pv_action_active || pv_recovery_active) ? pv_submit_message : submit_message);

        if (soc) {
          update_runtime_state(db, soc->definition().id,
                               final_target, target_point_id,
                               soc_runtime_suppressed,
                               soc_runtime_reason,
                               input_summary.str(), "");
          write_action_log(db, soc->definition().id,
                           submitted ? "command" : "suppress",
                           target_point_id,
                           final_target, 0.0,
                           soc_runtime_reason,
                           input_summary.str(),
                           soc_runtime_suppressed ? "suppressed" : (submitted ? "ok" : "suppressed"),
                           submit_message);
        }
      } else if (soc) {
        auto soc_result = soc->execute();
        std::string setpoint_id;
        for (const auto& b : soc->definition().bindings) {
          if (b.role == "bess_power_setpoint") {
            setpoint_id = b.point_id;
            break;
          }
        }
        final_target = soc_result.target_power_kw;
        final_suppressed = soc_result.suppressed;
        final_suppress_reason = soc_result.suppress_reason;
        input_summary << "SOC:target=" << double_to_string(soc_result.target_power_kw)
                      << ",suppressed=" << (soc_result.suppressed ? "1" : "0")
                      << ",cmd=" << soc_result.command_result;

        update_runtime_state(db, soc->definition().id,
                             soc_result.target_power_kw, setpoint_id,
                             soc_result.suppressed, soc_result.suppress_reason,
                             input_summary.str(), "");
        write_action_log(db, soc->definition().id,
                         soc_result.command_sent ? "command" : "suppress",
                         setpoint_id,
                         soc_result.target_power_kw, 0.0,
                         soc_result.suppress_reason,
                         input_summary.str(),
                         soc_result.command_sent ? "ok" : "suppressed",
                         soc_result.command_result);
      }
    }

    // Calculate sleep time to maintain fixed cycle
    uint64_t elapsed = now_ms() - cycle_start;
    int cycle_ms = 1000;
    int64_t sleep_ms = static_cast<int64_t>(cycle_ms) - static_cast<int64_t>(elapsed);
    if (sleep_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
  }

  delete rt_db;
  if (db.conn) {
    db.api.PQfinish(db.conn);
    db.conn = nullptr;
  }
  OPENEMS_LOG_I("StrategyEngine", "Shutdown.");
  return 0;
}
