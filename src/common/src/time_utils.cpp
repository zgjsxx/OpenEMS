// src/common/src/time_utils.cpp
#include "openems/common/time_utils.h"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace openems::common {

uint64_t now_ms() {
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();
}

std::tm localtime_safe(std::time_t ts) {
  std::tm result{};
#ifdef _WIN32
  localtime_s(&result, &ts);
#else
  localtime_r(&ts, &result);
#endif
  return result;
}

std::tm gmtime_safe(std::time_t ts) {
  std::tm result{};
#ifdef _WIN32
  gmtime_s(&result, &ts);
#else
  gmtime_r(&ts, &result);
#endif
  return result;
}

std::string ts_to_iso8601(uint64_t ms) {
  std::time_t seconds = static_cast<std::time_t>(ms / 1000);
  int frac_ms = static_cast<int>(ms % 1000);
  std::tm utc_tm = gmtime_safe(seconds);
  std::ostringstream oss;
  oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%S");
  oss << "." << std::setfill('0') << std::setw(3) << frac_ms << "+00:00";
  return oss.str();
}

std::string ts_to_date_str(uint64_t ms) {
  std::time_t seconds = static_cast<std::time_t>(ms / 1000);
  std::tm local_tm = localtime_safe(seconds);
  std::ostringstream oss;
  oss << std::put_time(&local_tm, "%Y%m%d");
  return oss.str();
}

} // namespace openems::common