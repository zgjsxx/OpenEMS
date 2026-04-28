// src/common/include/openems/common/time_utils.h
#pragma once

#include <cstdint>
#include <string>
#include <ctime>

namespace openems::common {

// epoch 毫秒时间戳
uint64_t now_ms();

// epoch 毫秒转 ISO 8601 UTC 字符串（带毫秒）
std::string ts_to_iso8601(uint64_t ms);

// epoch 毫秒转本地日期字符串 YYYYMMDD
std::string ts_to_date_str(uint64_t ms);

// 平台安全的 localtime/gmtime（Windows 用 _s，Linux 用 _r）
std::tm localtime_safe(std::time_t ts);
std::tm gmtime_safe(std::time_t ts);

} // namespace openems::common