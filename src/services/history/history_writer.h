#pragma once

#include "openems/libpq/libpq_api.h"
#include "openems/rt_db/rt_db.h"
#include "openems/utils/logger.h"

#include <chrono>
#include <string>
#include <unordered_map>

namespace openems::history {

class HistoryWriter {
public:
  explicit HistoryWriter(const std::unordered_map<std::string, std::string>& units);
  ~HistoryWriter();

  bool is_ready() const { return db_available_; }
  const std::string& last_error() const { return last_error_; }
  bool write(const rt_db::SiteSnapshot& snap);

private:
  bool write_to_db(const rt_db::SiteSnapshot& snap);

  bool connect();
  void try_reconnect();

  // DB state
  std::string db_url_;
  libpq::LibpqApi api_;
  void* conn_ = nullptr;
  bool db_available_ = false;
  int reconnect_delay_ms_ = 2000;
  std::string last_error_;

  std::unordered_map<std::string, std::string> units_;

  // Helpers
  static std::string ts_to_iso8601(uint64_t ms);
  static std::string double_to_string(double v);
  static std::string valid_to_string(bool v);
  static std::string quality_to_string(common::Quality q);
  static std::string category_to_string(uint8_t cat);
  static std::string sql_escape(const std::string& value);
};

}  // namespace openems::history
