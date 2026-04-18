// src/config/include/openems/config/csv_parser.h
#pragma once

#include <string>
#include <vector>
#include <optional>
#include "openems/common/result.h"

namespace openems::config {

struct CsvRow {
  std::vector<std::string> fields;
  const std::string& operator[](size_t idx) const { return fields[idx]; }
  size_t size() const { return fields.size(); }
};

struct CsvTable {
  std::vector<std::string> headers;
  std::vector<CsvRow> rows;

  std::optional<size_t> col_index(const std::string& col_name) const;
  const CsvRow* find(const std::string& col_name, const std::string& key) const;
  std::vector<const CsvRow*> find_all(const std::string& col_name, const std::string& key) const;
};

common::Result<CsvTable> parse_csv_file(const std::string& path);

// Field parsing helpers
std::string csv_string(const CsvRow& row, size_t idx, const std::string& default_val = "");
int32_t     csv_int(const CsvRow& row, size_t idx, int32_t default_val = 0);
uint16_t    csv_uint16(const CsvRow& row, size_t idx, uint16_t default_val = 0);
uint8_t     csv_uint8(const CsvRow& row, size_t idx, uint8_t default_val = 0);
double      csv_double(const CsvRow& row, size_t idx, double default_val = 0.0);
bool        csv_bool(const CsvRow& row, size_t idx, bool default_val = false);

} // namespace openems::config