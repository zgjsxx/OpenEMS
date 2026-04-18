// src/utils/include/openems/utils/time_utils.h
#pragma once

#include <string>
#include <chrono>
#include <ctime>

namespace openems::utils {

using Timestamp = std::chrono::system_clock::time_point;

Timestamp now();
std::string timestamp_to_string(Timestamp ts);
Timestamp string_to_timestamp(const std::string& str);
double timestamp_diff_ms(Timestamp a, Timestamp b);

} // namespace openems::utils