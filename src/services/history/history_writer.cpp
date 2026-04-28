// src/services/history/history_writer.cpp
#include "history_writer.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>

namespace openems::history {

HistoryWriter::HistoryWriter(const std::unordered_map<std::string, std::string>& units)
    : units_(units) {
  const char* env = std::getenv("OPENEMS_DB_URL");
  db_url_ = env ? std::string(env) : "";

  if (db_url_.empty()) {
    last_error_ = "OPENEMS_DB_URL not set";
    OPENEMS_LOG_E("HistoryWriter", last_error_);
    return;
  }

  auto api_result = libpq::load_libpq();
  if (!api_result.is_ok()) {
    last_error_ = "libpq not available: " + api_result.error_msg();
    OPENEMS_LOG_E("HistoryWriter", last_error_);
    return;
  }

  api_ = std::move(api_result.value());
  if (!connect()) {
    if (last_error_.empty()) last_error_ = "Failed to connect to TimescaleDB";
    return;
  }

  OPENEMS_LOG_I("HistoryWriter", "Connected to TimescaleDB");
}

HistoryWriter::~HistoryWriter() {
  if (conn_) {
    api_.PQfinish(conn_);
    conn_ = nullptr;
  }
}

bool HistoryWriter::write(const rt_db::SiteSnapshot& snap) {
  if (!write_to_db(snap)) {
    if (last_error_.empty()) {
      last_error_ = "Failed to write history samples to PostgreSQL";
    }
    OPENEMS_LOG_E("HistoryWriter", last_error_);
    return false;
  }
  return true;
}

bool HistoryWriter::write_to_db(const rt_db::SiteSnapshot& snap) {
  if (!conn_ || api_.PQstatus(conn_) != 0 /* CONNECTION_OK */) {
    try_reconnect();
    if (!db_available_) return false;
  }

  void* begin_res = api_.PQexec(conn_, "BEGIN");
  if (!begin_res || api_.PQresultStatus(begin_res) != 1 /* PGRES_COMMAND_OK */) {
    if (begin_res) api_.PQclear(begin_res);
    db_available_ = false;
    last_error_ = "Failed to start history transaction: " + std::string(api_.PQerrorMessage(conn_));
    return false;
  }
  api_.PQclear(begin_res);

  const char* sql =
      "INSERT INTO history_samples("
      "ts, site_id, point_id, device_id, category, value, unit, quality, valid"
      ") VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)";

  size_t telemetry_idx = 0;
  size_t teleindication_idx = 0;
  uint64_t row_ts = snap.snapshot_time > 0 ? snap.snapshot_time
      : std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();

  bool all_ok = true;
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
        ? snap.timestamps[i] : row_ts;
    std::string unit;
    auto unit_it = units_.find(snap.point_ids[i]);
    if (unit_it != units_.end()) unit = unit_it->second;

    std::string ts_str = ts_to_iso8601(ts);
    std::string val_str = double_to_string(value);
    std::string cat_str = category_to_string(category);
    std::string qual_str = quality_to_string(snap.qualities[i]);
    std::string valid_str = valid_to_string(snap.valids[i]);

    const char* param_values[9] = {
      ts_str.c_str(),
      snap.site_id.c_str(),
      snap.point_ids[i].c_str(),
      snap.device_ids[i].c_str(),
      cat_str.c_str(),
      val_str.c_str(),
      unit.c_str(),
      qual_str.c_str(),
      valid_str.c_str(),
    };
    int param_lengths[9] = {
      static_cast<int>(ts_str.size()),
      static_cast<int>(snap.site_id.size()),
      static_cast<int>(snap.point_ids[i].size()),
      static_cast<int>(snap.device_ids[i].size()),
      static_cast<int>(cat_str.size()),
      static_cast<int>(val_str.size()),
      static_cast<int>(unit.size()),
      static_cast<int>(qual_str.size()),
      static_cast<int>(valid_str.size()),
    };
    int param_formats[9] = {0};

    void* res = nullptr;
    if (api_.PQexecParams) {
      res = api_.PQexecParams(conn_, sql, 9, nullptr, param_values, param_lengths, param_formats, 0);
    } else {
      std::ostringstream oss;
      oss << "INSERT INTO history_samples(ts, site_id, point_id, device_id, category, value, unit, quality, valid) VALUES ("
          << "'" << sql_escape(ts_str) << "',"
          << "'" << sql_escape(std::string(snap.site_id)) << "',"
          << "'" << sql_escape(snap.point_ids[i]) << "',"
          << "'" << sql_escape(snap.device_ids[i]) << "',"
          << "'" << sql_escape(cat_str) << "',"
          << val_str << ","
          << "'" << sql_escape(unit) << "',"
          << "'" << sql_escape(qual_str) << "',"
          << "'" << sql_escape(valid_str) << "')";
      res = api_.PQexec(conn_, oss.str().c_str());
    }

    if (!res || api_.PQresultStatus(res) != 1 /* PGRES_COMMAND_OK */) {
      if (res) api_.PQclear(res);
      all_ok = false;
      last_error_ = "History INSERT failed: " + std::string(api_.PQerrorMessage(conn_));
      break;
    }
    api_.PQclear(res);
  }

  if (all_ok) {
    void* commit_res = api_.PQexec(conn_, "COMMIT");
    if (!commit_res || api_.PQresultStatus(commit_res) != 1 /* PGRES_COMMAND_OK */) {
      if (commit_res) api_.PQclear(commit_res);
      db_available_ = false;
      last_error_ = "History COMMIT failed: " + std::string(api_.PQerrorMessage(conn_));
      return false;
    }
    api_.PQclear(commit_res);
    last_error_.clear();
    return true;
  }

  void* rollback_res = api_.PQexec(conn_, "ROLLBACK");
  if (rollback_res) api_.PQclear(rollback_res);
  db_available_ = false;
  return false;
}

bool HistoryWriter::connect() {
  if (db_url_.empty() || !api_.handle) return false;
  conn_ = api_.PQconnectdb(db_url_.c_str());
  if (!conn_ || api_.PQstatus(conn_) != 0) {
    last_error_ = "DB connect failed: " + std::string(conn_ ? api_.PQerrorMessage(conn_) : "failed to allocate connection");
    OPENEMS_LOG_E("HistoryWriter", last_error_);
    if (conn_) {
      api_.PQfinish(conn_);
      conn_ = nullptr;
    }
    db_available_ = false;
    return false;
  }
  db_available_ = true;
  reconnect_delay_ms_ = 2000;
  last_error_.clear();
  return true;
}

void HistoryWriter::try_reconnect() {
  if (conn_) {
    api_.PQfinish(conn_);
    conn_ = nullptr;
  }
  db_available_ = false;

  OPENEMS_LOG_W("HistoryWriter", "Attempting DB reconnect in " + std::to_string(reconnect_delay_ms_) + "ms");
  std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms_));

  if (connect()) {
    OPENEMS_LOG_I("HistoryWriter", "Reconnected to TimescaleDB");
    reconnect_delay_ms_ = 2000;
  } else {
    reconnect_delay_ms_ = (std::min)(reconnect_delay_ms_ * 2, 30000);
  }
}

std::string HistoryWriter::ts_to_iso8601(uint64_t ms) {
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

std::string HistoryWriter::double_to_string(double v) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(6) << v;
  std::string s = oss.str();
  if (s.find('.') != std::string::npos) {
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s += '0';
  }
  return s;
}

std::string HistoryWriter::valid_to_string(bool v) {
  return v ? "t" : "f";
}

std::string HistoryWriter::quality_to_string(common::Quality q) {
  switch (q) {
    case common::Quality::Good: return "Good";
    case common::Quality::Questionable: return "Questionable";
    case common::Quality::Bad: return "Bad";
    case common::Quality::Invalid: return "Invalid";
    default: return "Unknown";
  }
}

std::string HistoryWriter::category_to_string(uint8_t cat) {
  switch (cat) {
    case 0: return "telemetry";
    case 1: return "teleindication";
    case 2: return "telecontrol";
    case 3: return "teleadjust";
    default: return "unknown";
  }
}

std::string HistoryWriter::sql_escape(const std::string& value) {
  std::string result;
  for (char ch : value) {
    if (ch == '\'') result += "''";
    else result += ch;
  }
  return result;
}

}  // namespace openems::history
