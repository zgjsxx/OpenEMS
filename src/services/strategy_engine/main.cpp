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
#include <chrono>
#include <csignal>
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
      << "'" << sql_escape(input_summary) << "',"
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
      << "'" << sql_escape(input_summary) << "',"
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

  int reload_counter = 0;
  const int kReloadInterval = 30;  // reload config every N cycles

  OPENEMS_LOG_I("StrategyEngine", "Running. Cycle interval: 1000ms");

  while (g_running.load()) {
    uint64_t cycle_start = now_ms();

    // Periodically reload config from DB
    if (reload_counter % kReloadInterval == 0) {
      strategies = load_strategies(db, rt_db);
      load_manual_overrides(db, manual_overrides);
    }
    ++reload_counter;

    // Find anti-reverse-flow and SOC protection strategies
    openems::strategy::AntiReverseFlow* arf = nullptr;
    openems::strategy::SocProtection* soc = nullptr;

    for (auto& ls : strategies) {
      if (ls.def.type == openems::strategy::StrategyType::AntiReverseFlow) {
        arf = static_cast<openems::strategy::AntiReverseFlow*>(
            ls.instance.get());
      } else if (ls.def.type == openems::strategy::StrategyType::SocProtection) {
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
      // Step 1: Anti-reverse-flow calculates target
      if (arf) {
        auto arf_result = arf->execute();

        input_summary << "ARF:target=" << double_to_string(arf_result.target_power_kw);

        if (arf_result.suppressed) {
          input_summary << ",suppressed=" << arf_result.suppress_reason;
        } else {
          input_summary << ",sent=" << (arf_result.command_sent ? "1" : "0");
          input_summary << ",cmd=" << arf_result.command_result;
        }

        final_target = arf_result.target_power_kw;

        // Write state and log
        std::string setpoint_id;
        for (const auto& b : arf->definition().bindings) {
          if (b.role == "bess_power_setpoint") setpoint_id = b.point_id;
        }
        update_runtime_state(db, arf->definition().id,
                             arf_result.target_power_kw, setpoint_id,
                             arf_result.suppressed,
                             arf_result.suppress_reason,
                             input_summary.str(), "");
        write_action_log(db, arf->definition().id,
                         arf_result.command_sent ? "command" : "suppress",
                         setpoint_id,
                         arf_result.target_power_kw, 0.0,
                         arf_result.suppress_reason,
                         input_summary.str(),
                         arf_result.command_sent ? "ok" : "suppressed",
                         arf_result.command_result);
      }

      // Step 2: SOC protection clamps the target
      if (soc && arf) {
        auto clamp = soc->clamp(final_target);
        input_summary << " | SOC:clamped=" << double_to_string(clamp.clamped_kw);

        if (clamp.suppressed) {
          input_summary << ",suppressed=" << clamp.reason;
          final_target = clamp.clamped_kw;
          final_suppressed = true;
          final_suppress_reason = clamp.reason;

          // Re-submit the clamped target if it changed
          std::string setpoint_id;
          for (const auto& b : soc->definition().bindings) {
            if (b.role == "bess_power_setpoint") setpoint_id = b.point_id;
          }

          // Only write if SOC actually changed the target
          if (clamp.clamped_kw != final_target) {
            update_runtime_state(db, soc->definition().id,
                                 clamp.clamped_kw, setpoint_id,
                                 clamp.suppressed, clamp.reason,
                                 input_summary.str(), "");
            write_action_log(db, soc->definition().id,
                             "suppress", setpoint_id,
                             clamp.clamped_kw, 0.0, clamp.reason,
                             input_summary.str(), "suppressed", "");
          }
        } else {
          update_runtime_state(db, soc->definition().id,
                               final_target, "",
                               false, "SOC in normal range",
                               input_summary.str(), "");
        }
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
