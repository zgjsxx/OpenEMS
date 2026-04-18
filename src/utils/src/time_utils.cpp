// src/utils/src/time_utils.cpp
#include "openems/utils/time_utils.h"
#include <iomanip>
#include <sstream>

namespace openems::utils {

Timestamp now() {
  return std::chrono::system_clock::now();
}

std::string timestamp_to_string(Timestamp ts) {
  auto time_t_val = std::chrono::system_clock::to_time_t(ts);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      ts.time_since_epoch()) % 1000;
  std::ostringstream oss;
  oss << std::put_time(std::localtime(&time_t_val), "%Y-%m-%d %H:%M:%S")
      << "." << std::setfill('0') << std::setw(3) << ms.count();
  return oss.str();
}

double timestamp_diff_ms(Timestamp a, Timestamp b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

} // namespace openems::utils