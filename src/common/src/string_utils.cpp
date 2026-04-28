// src/common/src/string_utils.cpp
#include "openems/common/string_utils.h"

#include <algorithm>
#include <iomanip>

namespace openems::common {

std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\r\n");
  auto end = s.find_last_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  return s.substr(start, end - start + 1);
}

std::vector<std::string> split(const std::string& line, char delim) {
  std::vector<std::string> parts;
  std::string part;
  for (char c : line) {
    if (c == delim) {
      parts.push_back(std::move(part));
      part.clear();
    } else {
      part += c;
    }
  }
  parts.push_back(std::move(part));
  return parts;
}

std::vector<std::string> split_csv(const std::string& line, char delim) {
  std::vector<std::string> fields;
  std::string field;
  bool in_quotes = false;

  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          field += '"';
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        field += c;
      }
    } else {
      if (c == '"') {
        in_quotes = true;
      } else if (c == delim) {
        fields.push_back(trim(field));
        field.clear();
      } else {
        field += c;
      }
    }
  }
  fields.push_back(trim(field));
  return fields;
}

std::string json_escape(const std::string& value) {
  std::ostringstream oss;
  for (char ch : value) {
    switch (ch) {
      case '\\': oss << "\\\\"; break;
      case '"':  oss << "\\\""; break;
      case '\n': oss << "\\n";  break;
      case '\r': oss << "\\r";  break;
      case '\t': oss << "\\t";  break;
      default:   oss << ch;     break;
    }
  }
  return oss.str();
}

bool parse_bool(const std::string& value, bool default_value) {
  if (value == "true" || value == "1") return true;
  if (value == "false" || value == "0") return false;
  return default_value;
}

std::string format_double(double v, int precision) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << v;
  std::string s = oss.str();
  if (s.find('.') != std::string::npos) {
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s += '0';
  }
  return s;
}

std::string to_lower(const std::string& s) {
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

std::string to_upper(const std::string& s) {
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return result;
}

std::string join_path(const std::string& dir, const std::string& filename) {
  if (dir.empty()) return filename;
  char last = dir.back();
  if (last == '/' || last == '\\') return dir + filename;
  return dir + "/" + filename;
}

} // namespace openems::common