// src/common/include/openems/common/string_utils.h
#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <cstdint>

namespace openems::common {

// 去首尾空白
std::string trim(const std::string& s);

// 简单分隔符分割
std::vector<std::string> split(const std::string& line, char delim);

// CSV 行分割（支持双引号字段和转义双引号 ""）
std::vector<std::string> split_csv(const std::string& line, char delim = ',');

// 容器拼接为字符串
template <typename Container, typename Transform>
std::string join(const Container& items, const std::string& delim, Transform fn) {
  std::ostringstream oss;
  bool first = true;
  for (const auto& item : items) {
    if (!first) oss << delim;
    oss << fn(item);
    first = false;
  }
  return oss.str();
}

template <typename Container>
std::string join(const Container& items, const std::string& delim) {
  return join(items, delim, [](const auto& x) {
    std::ostringstream oss;
    oss << x;
    return oss.str();
  });
}

// JSON 字符串转义
std::string json_escape(const std::string& value);

// 布尔值字符串解析（"true"/"1" → true, "false"/"0" → false）
bool parse_bool(const std::string& value, bool default_value = false);

// 去尾零的浮点格式化
std::string format_double(double v, int precision = 6);

// 大小写转换
std::string to_lower(const std::string& s);
std::string to_upper(const std::string& s);

// 路径拼接（处理斜杠分隔符）
std::string join_path(const std::string& dir, const std::string& filename);

} // namespace openems::common