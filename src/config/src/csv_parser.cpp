// src/config/src/csv_parser.cpp
#include "openems/config/csv_parser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdint>

namespace openems::config {

static std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\r\n");
  auto end = s.find_last_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  return s.substr(start, end - start + 1);
}

static bool is_comment_line(const std::string& line) {
  auto trimmed = trim(line);
  return !trimmed.empty() && trimmed[0] == '#';
}

static std::vector<std::string> split_csv_line(const std::string& line) {
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
      } else if (c == ',') {
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

common::Result<CsvTable> parse_csv_file(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return common::Result<CsvTable>::Err(
        common::ErrorCode::InvalidConfig, "Cannot open CSV: " + path);
  }

  CsvTable table;
  std::string line;

  // First non-empty, non-comment line: headers
  bool header_found = false;
  while (std::getline(file, line)) {
    auto trimmed = trim(line);
    if (trimmed.empty() || is_comment_line(trimmed)) continue;
    table.headers = split_csv_line(line);
    header_found = true;
    break;
  }
  if (!header_found) {
    return common::Result<CsvTable>::Err(
        common::ErrorCode::InvalidConfig, "CSV has no header: " + path);
  }

  // Data rows
  while (std::getline(file, line)) {
    auto trimmed = trim(line);
    if (trimmed.empty() || is_comment_line(trimmed)) continue;
    auto fields = split_csv_line(line);
    // Pad with empty strings if row has fewer fields than headers
    while (fields.size() < table.headers.size()) {
      fields.push_back("");
    }
    table.rows.push_back(CsvRow{std::move(fields)});
  }

  return common::Result<CsvTable>::Ok(std::move(table));
}

std::optional<size_t> CsvTable::col_index(const std::string& col_name) const {
  for (size_t i = 0; i < headers.size(); ++i) {
    if (headers[i] == col_name) return i;
  }
  return std::nullopt;
}

const CsvRow* CsvTable::find(const std::string& col_name, const std::string& key) const {
  auto idx = col_index(col_name);
  if (!idx) return nullptr;
  for (auto& row : rows) {
    if (*idx < row.size() && row[*idx] == key) return &row;
  }
  return nullptr;
}

std::vector<const CsvRow*> CsvTable::find_all(const std::string& col_name, const std::string& key) const {
  std::vector<const CsvRow*> result;
  auto idx = col_index(col_name);
  if (!idx) return result;
  for (auto& row : rows) {
    if (*idx < row.size() && row[*idx] == key) result.push_back(&row);
  }
  return result;
}

// Field parsing helpers
std::string csv_string(const CsvRow& row, size_t idx, const std::string& default_val) {
  if (idx >= row.size() || row[idx].empty()) return default_val;
  return row[idx];
}

int32_t csv_int(const CsvRow& row, size_t idx, int32_t default_val) {
  if (idx >= row.size() || row[idx].empty()) return default_val;
  try { return std::stoi(row[idx]); } catch (...) { return default_val; }
}

uint16_t csv_uint16(const CsvRow& row, size_t idx, uint16_t default_val) {
  if (idx >= row.size() || row[idx].empty()) return default_val;
  try { return static_cast<uint16_t>(std::stoul(row[idx])); } catch (...) { return default_val; }
}

uint8_t csv_uint8(const CsvRow& row, size_t idx, uint8_t default_val) {
  if (idx >= row.size() || row[idx].empty()) return default_val;
  try { return static_cast<uint8_t>(std::stoul(row[idx])); } catch (...) { return default_val; }
}

double csv_double(const CsvRow& row, size_t idx, double default_val) {
  if (idx >= row.size() || row[idx].empty()) return default_val;
  try { return std::stod(row[idx]); } catch (...) { return default_val; }
}

bool csv_bool(const CsvRow& row, size_t idx, bool default_val) {
  if (idx >= row.size() || row[idx].empty()) return default_val;
  auto s = row[idx];
  if (s == "true" || s == "1") return true;
  if (s == "false" || s == "0") return false;
  return default_val;
}

} // namespace openems::config
