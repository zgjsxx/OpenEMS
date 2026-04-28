// src/config/src/config_loader.cpp
#include "openems/config/config_loader.h"
#include "openems/config/csv_parser.h"
#include "openems/utils/logger.h"
#include "openems/libpq/libpq_api.h"

#include "nlohmann/json.hpp"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace openems::config {

namespace {

using TableMap = std::map<std::string, CsvTable>;

static const std::vector<std::string> kRuntimeTables = {
    "ems_config",
    "site",
    "device",
    "telemetry",
    "teleindication",
    "telecontrol",
    "teleadjust",
    "modbus_mapping",
    "iec104_mapping",
};

static const std::map<std::string, std::vector<std::string>> kTableHeaders = {
    {"ems_config", {"log_level", "default_poll_interval_ms", "site_id"}},
    {"site", {"id", "name", "description"}},
    {"device", {"id", "site_id", "name", "type", "protocol", "ip", "port", "unit_id", "poll_interval_ms", "common_address"}},
    {"telemetry", {"id", "device_id", "name", "code", "data_type", "unit", "writable"}},
    {"teleindication", {"id", "device_id", "name", "code", "data_type", "unit", "writable"}},
    {"telecontrol", {"id", "device_id", "name", "code", "data_type", "unit", "writable"}},
    {"teleadjust", {"id", "device_id", "name", "code", "data_type", "unit", "writable"}},
    {"modbus_mapping", {"point_id", "function_code", "register_address", "register_count", "data_type", "scale", "offset"}},
    {"iec104_mapping", {"point_id", "type_id", "ioa", "common_address", "scale", "cot"}},
};

enum {
  kConnectionOk = 0,
  kTuplesOk = 2,
};

static std::string csv_path(const std::string& dir_path, const std::string& filename) {
  std::string p = dir_path;
  if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
  return p + filename;
}

static std::string json_value_to_string(const nlohmann::json& value) {
  if (value.is_null()) return "";
  if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
  if (value.is_string()) return value.get<std::string>();
  if (value.is_number_integer()) return std::to_string(value.get<long long>());
  if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
  if (value.is_number_float()) {
    auto text = std::to_string(value.get<double>());
    while (text.size() > 1 && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
  }
  return value.dump();
}

static common::PointCategory parse_point_category(const std::string& s) {
  if (s == "telemetry") return common::PointCategory::Telemetry;
  if (s == "teleindication") return common::PointCategory::Teleindication;
  if (s == "telecontrol") return common::PointCategory::Telecontrol;
  if (s == "teleadjust") return common::PointCategory::Setting;
  return common::PointCategory::Telemetry;
}

static common::DataType parse_data_type(const std::string& s) {
  if (s == "bool") return common::DataType::Bool;
  if (s == "int16") return common::DataType::Int16;
  if (s == "uint16") return common::DataType::Uint16;
  if (s == "int32") return common::DataType::Int32;
  if (s == "uint32") return common::DataType::Uint32;
  if (s == "float32") return common::DataType::Float32;
  if (s == "int64") return common::DataType::Int64;
  if (s == "uint64") return common::DataType::Uint64;
  if (s == "double") return common::DataType::Double;
  return common::DataType::Uint16;
}

static common::DeviceType parse_device_type(const std::string& s) {
  if (s == "PV") return common::DeviceType::PV;
  if (s == "BESS") return common::DeviceType::BESS;
  if (s == "Meter") return common::DeviceType::Meter;
  if (s == "Inverter") return common::DeviceType::Inverter;
  if (s == "Grid") return common::DeviceType::Grid;
  if (s == "Transformer") return common::DeviceType::Transformer;
  return common::DeviceType::Unknown;
}

static common::Result<TableMap> load_csv_tables(const std::string& dir_path) {
  TableMap tables;
  for (const auto& table_name : kRuntimeTables) {
    auto result = parse_csv_file(csv_path(dir_path, table_name + ".csv"));
    if (!result.is_ok()) {
      if (table_name == "telecontrol" || table_name == "teleadjust") {
        CsvTable empty;
        empty.headers = kTableHeaders.at(table_name);
        tables[table_name] = std::move(empty);
        OPENEMS_LOG_D("ConfigLoader", table_name + ".csv not found or empty - skipping");
        continue;
      }
      return common::Result<TableMap>::Err(result.error_code(), result.error_msg());
    }
    tables[table_name] = std::move(result.value());
  }
  return common::Result<TableMap>::Ok(std::move(tables));
}

static common::Result<TableMap> load_postgres_tables(const std::string& db_url) {
  if (db_url.empty()) {
    return common::Result<TableMap>::Err(common::ErrorCode::InvalidConfig, "OPENEMS_DB_URL is empty");
  }

  auto api_result = openems::libpq::load_libpq();
  if (!api_result.is_ok()) {
    return common::Result<TableMap>::Err(api_result.error_code(), api_result.error_msg());
  }
  auto& api = api_result.value();

  void* conn = api.PQconnectdb(db_url.c_str());
  if (!conn || api.PQstatus(conn) != kConnectionOk) {
    std::string error = conn ? api.PQerrorMessage(conn) : "failed to allocate PostgreSQL connection";
    if (conn) api.PQfinish(conn);
    return common::Result<TableMap>::Err(common::ErrorCode::ConnectionFailed, "PostgreSQL connect failed: " + error);
  }

  TableMap tables;
  auto load_query_table = [&](const std::string& table_name,
                              const std::string& sql,
                              const std::vector<std::string>& headers) -> common::VoidResult {
    void* res = api.PQexec(conn, sql.c_str());
    if (!res || api.PQresultStatus(res) != kTuplesOk) {
      std::string error = api.PQerrorMessage(conn);
      if (res) api.PQclear(res);
      return common::VoidResult::Err(common::ErrorCode::InvalidConfig,
          "PostgreSQL structured config query failed for " + table_name + ": " + error);
    }
    CsvTable table;
    table.headers = headers;
    const int rows = api.PQntuples(res);
    for (int row_index = 0; row_index < rows; ++row_index) {
      CsvRow row;
      for (size_t col = 0; col < headers.size(); ++col) {
        row.fields.push_back(api.PQgetvalue(res, row_index, static_cast<int>(col)));
      }
      table.rows.push_back(std::move(row));
    }
    tables[table_name] = std::move(table);
    api.PQclear(res);
    return common::VoidResult::Ok();
  };

  std::vector<std::pair<std::string, std::string>> queries = {
      {"ems_config", "SELECT log_level, default_poll_interval_ms::text, site_id FROM ems_config ORDER BY singleton LIMIT 1"},
      {"site", "SELECT id, name, description FROM sites ORDER BY id"},
      {"device", "SELECT id, site_id, name, type, protocol, ip, port::text, unit_id::text, poll_interval_ms::text, COALESCE(common_address::text, '') FROM devices ORDER BY id"},
      {"telemetry", "SELECT id, device_id, name, code, data_type, unit, CASE WHEN writable THEN 'true' ELSE 'false' END FROM points WHERE category = 'telemetry' ORDER BY id"},
      {"teleindication", "SELECT id, device_id, name, code, data_type, unit, CASE WHEN writable THEN 'true' ELSE 'false' END FROM points WHERE category = 'teleindication' ORDER BY id"},
      {"telecontrol", "SELECT id, device_id, name, code, data_type, unit, CASE WHEN writable THEN 'true' ELSE 'false' END FROM points WHERE category = 'telecontrol' ORDER BY id"},
      {"teleadjust", "SELECT id, device_id, name, code, data_type, unit, CASE WHEN writable THEN 'true' ELSE 'false' END FROM points WHERE category = 'teleadjust' ORDER BY id"},
      {"modbus_mapping", "SELECT point_id, function_code::text, register_address::text, register_count::text, data_type, scale::text, offset_value::text FROM modbus_mappings ORDER BY point_id"},
      {"iec104_mapping", "SELECT point_id, type_id::text, ioa::text, common_address::text, scale::text, cot::text FROM iec104_mappings ORDER BY point_id"},
  };

  for (const auto& item : queries) {
    auto header_it = kTableHeaders.find(item.first);
    auto query_result = load_query_table(item.first, item.second, header_it->second);
    if (!query_result.is_ok()) {
      api.PQfinish(conn);
      return common::Result<TableMap>::Err(query_result.error_code(), query_result.error_msg());
    }
  }
  api.PQfinish(conn);

  for (const auto& table_name : kRuntimeTables) {
    if (tables.find(table_name) == tables.end()) {
      if (table_name == "telecontrol" || table_name == "teleadjust") {
        CsvTable empty;
        empty.headers = kTableHeaders.at(table_name);
        tables[table_name] = std::move(empty);
        continue;
      }
      return common::Result<TableMap>::Err(common::ErrorCode::InvalidConfig, "PostgreSQL config table missing: " + table_name);
    }
  }

  return common::Result<TableMap>::Ok(std::move(tables));
}

static common::Result<EmsConfig> build_config_from_tables(const TableMap& tables) {
  auto require_table = [&](const std::string& name) -> common::Result<const CsvTable*> {
    auto it = tables.find(name);
    if (it == tables.end()) {
      return common::Result<const CsvTable*>::Err(common::ErrorCode::InvalidConfig, "Missing config table: " + name);
    }
    return common::Result<const CsvTable*>::Ok(&it->second);
  };

  auto ems_table_result = require_table("ems_config");
  if (!ems_table_result.is_ok()) return common::Result<EmsConfig>::Err(ems_table_result.error_code(), ems_table_result.error_msg());
  const auto& ems_cfg_table = *ems_table_result.value();
  if (ems_cfg_table.rows.empty()) return common::Result<EmsConfig>::Err(common::ErrorCode::InvalidConfig, "ems_config has no data rows");

  EmsConfig config;
  const auto& ems_row = ems_cfg_table.rows[0];
  auto log_level_idx = ems_cfg_table.col_index("log_level");
  auto poll_idx = ems_cfg_table.col_index("default_poll_interval_ms");
  auto site_id_idx = ems_cfg_table.col_index("site_id");

  config.log_level = csv_string(ems_row, log_level_idx.value_or(0), "info");
  config.default_poll_interval_ms = static_cast<uint32_t>(csv_int(ems_row, poll_idx.value_or(1), 1000));
  std::string site_id = csv_string(ems_row, site_id_idx.value_or(2), "site-001");

  auto site_table_result = require_table("site");
  if (!site_table_result.is_ok()) return common::Result<EmsConfig>::Err(site_table_result.error_code(), site_table_result.error_msg());
  const auto& site_table = *site_table_result.value();
  auto* site_row = site_table.find("id", site_id);
  if (!site_row) return common::Result<EmsConfig>::Err(common::ErrorCode::InvalidConfig, "site_id '" + site_id + "' not found in site");

  auto site_name_idx = site_table.col_index("name");
  auto site_desc_idx = site_table.col_index("description");
  config.site.id = site_id;
  config.site.name = csv_string(*site_row, site_name_idx.value_or(1), "Default Site");
  config.site.description = csv_string(*site_row, site_desc_idx.value_or(2), "");

  auto device_table_result = require_table("device");
  if (!device_table_result.is_ok()) return common::Result<EmsConfig>::Err(device_table_result.error_code(), device_table_result.error_msg());
  const auto& device_table = *device_table_result.value();

  auto d_id_idx = device_table.col_index("id");
  auto d_name_idx = device_table.col_index("name");
  auto d_type_idx = device_table.col_index("type");
  auto d_proto_idx = device_table.col_index("protocol");
  auto d_ip_idx = device_table.col_index("ip");
  auto d_port_idx = device_table.col_index("port");
  auto d_unit_idx = device_table.col_index("unit_id");
  auto d_poll_idx = device_table.col_index("poll_interval_ms");
  auto d_ca_idx = device_table.col_index("common_address");

  auto device_rows = device_table.find_all("site_id", site_id);
  for (auto* drow : device_rows) {
    DeviceConfig dc;
    dc.id = csv_string(*drow, d_id_idx.value_or(0));
    dc.name = csv_string(*drow, d_name_idx.value_or(2), "Device");
    dc.type = parse_device_type(csv_string(*drow, d_type_idx.value_or(3), "Unknown"));
    dc.protocol = csv_string(*drow, d_proto_idx.value_or(4), "modbus-tcp");
    dc.ip = csv_string(*drow, d_ip_idx.value_or(5), "127.0.0.1");
    dc.port = csv_uint16(*drow, d_port_idx.value_or(6), 502);
    dc.unit_id = csv_uint8(*drow, d_unit_idx.value_or(7), 1);
    dc.poll_interval_ms = static_cast<uint32_t>(csv_int(*drow, d_poll_idx.value_or(8), 1000));
    dc.iec104_common_address = csv_uint16(*drow, d_ca_idx.value_or(9), 1);
    config.site.devices.push_back(std::move(dc));
  }

  auto load_point_table = [&](const std::string& table_name, common::PointCategory cat) -> common::VoidResult {
    auto table_result = require_table(table_name);
    if (!table_result.is_ok()) return common::VoidResult::Err(table_result.error_code(), table_result.error_msg());
    const auto& table = *table_result.value();

    auto p_id_idx = table.col_index("id");
    auto p_name_idx = table.col_index("name");
    auto p_code_idx = table.col_index("code");
    auto p_dt_idx = table.col_index("data_type");
    auto p_unit_idx = table.col_index("unit");
    auto p_wr_idx = table.col_index("writable");

    for (auto& dc : config.site.devices) {
      auto point_rows = table.find_all("device_id", dc.id);
      for (auto* prow : point_rows) {
        PointConfig pc;
        pc.id = csv_string(*prow, p_id_idx.value_or(0));
        pc.name = csv_string(*prow, p_name_idx.value_or(2), "Point");
        pc.code = csv_string(*prow, p_code_idx.value_or(3), "");
        pc.category = cat;
        pc.data_type = parse_data_type(csv_string(*prow, p_dt_idx.value_or(4), "uint16"));
        pc.unit = csv_string(*prow, p_unit_idx.value_or(5), "");
        pc.writable = csv_bool(*prow, p_wr_idx.value_or(6), false);
        dc.points.push_back(std::move(pc));
      }
    }
    return common::VoidResult::Ok();
  };

  for (const auto& point_table : std::vector<std::pair<std::string, common::PointCategory>>{
           {"telemetry", common::PointCategory::Telemetry},
           {"teleindication", common::PointCategory::Teleindication},
           {"telecontrol", common::PointCategory::Telecontrol},
           {"teleadjust", common::PointCategory::Setting},
       }) {
    auto point_result = load_point_table(point_table.first, point_table.second);
    if (!point_result.is_ok()) return common::Result<EmsConfig>::Err(point_result.error_code(), point_result.error_msg());
  }

  auto mb_table_result = require_table("modbus_mapping");
  if (!mb_table_result.is_ok()) return common::Result<EmsConfig>::Err(mb_table_result.error_code(), mb_table_result.error_msg());
  const auto& mb_table = *mb_table_result.value();

  auto m_fc_idx = mb_table.col_index("function_code");
  auto m_ra_idx = mb_table.col_index("register_address");
  auto m_rc_idx = mb_table.col_index("register_count");
  auto m_dt_idx = mb_table.col_index("data_type");
  auto m_sc_idx = mb_table.col_index("scale");
  auto m_of_idx = mb_table.col_index("offset");

  for (auto& dc : config.site.devices) {
    for (auto& pc : dc.points) {
      auto* mb_row = mb_table.find("point_id", pc.id);
      if (mb_row) {
        pc.has_modbus_mapping = true;
        pc.modbus_mapping.function_code = csv_uint8(*mb_row, m_fc_idx.value_or(1), 3);
        pc.modbus_mapping.register_address = csv_uint16(*mb_row, m_ra_idx.value_or(2), 0);
        pc.modbus_mapping.register_count = csv_uint16(*mb_row, m_rc_idx.value_or(3), 1);
        pc.modbus_mapping.data_type = parse_data_type(csv_string(*mb_row, m_dt_idx.value_or(4), "uint16"));
        pc.modbus_mapping.scale = csv_double(*mb_row, m_sc_idx.value_or(5), 1.0);
        pc.modbus_mapping.offset = csv_double(*mb_row, m_of_idx.value_or(6), 0.0);
      }
    }
  }

  auto ic_table_result = require_table("iec104_mapping");
  if (!ic_table_result.is_ok()) return common::Result<EmsConfig>::Err(ic_table_result.error_code(), ic_table_result.error_msg());
  const auto& ic_table = *ic_table_result.value();

  auto i_tid_idx = ic_table.col_index("type_id");
  auto i_ioa_idx = ic_table.col_index("ioa");
  auto i_ca_idx = ic_table.col_index("common_address");
  auto i_sc_idx = ic_table.col_index("scale");
  auto i_cot_idx = ic_table.col_index("cot");

  for (auto& dc : config.site.devices) {
    for (auto& pc : dc.points) {
      auto* ic_row = ic_table.find("point_id", pc.id);
      if (ic_row) {
        pc.has_iec104_mapping = true;
        pc.iec104_mapping.type_id = csv_uint8(*ic_row, i_tid_idx.value_or(1), 0);
        pc.iec104_mapping.ioa = static_cast<uint32_t>(csv_int(*ic_row, i_ioa_idx.value_or(2), 0));
        pc.iec104_mapping.common_address = csv_uint16(*ic_row, i_ca_idx.value_or(3), 1);
        pc.iec104_mapping.scale = csv_double(*ic_row, i_sc_idx.value_or(4), 1.0);
        pc.iec104_mapping.cot = csv_uint8(*ic_row, i_cot_idx.value_or(5), 3);
      }
    }
  }

  return common::Result<EmsConfig>::Ok(std::move(config));
}

static std::string env_or_empty(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

}  // namespace

common::Result<EmsConfig> ConfigLoader::load(const std::string& dir_path) {
  return load_from_csv(dir_path);
}

common::Result<EmsConfig> ConfigLoader::load_from_csv(const std::string& dir_path) {
  auto tables_result = load_csv_tables(dir_path);
  if (!tables_result.is_ok()) {
    return common::Result<EmsConfig>::Err(tables_result.error_code(), tables_result.error_msg());
  }
  return build_config_from_tables(tables_result.value());
}

common::Result<EmsConfig> ConfigLoader::load_from_postgres(const std::string& db_url) {
  auto tables_result = load_postgres_tables(db_url);
  if (!tables_result.is_ok()) {
    return common::Result<EmsConfig>::Err(tables_result.error_code(), tables_result.error_msg());
  }
  return build_config_from_tables(tables_result.value());
}

common::Result<EmsConfig> ConfigLoader::load(const std::string& source,
                                             const std::string& dir_path,
                                             const std::string& db_url) {
  if (source == "csv") {
    return load_from_csv(dir_path);
  }

  const std::string effective_db_url = db_url.empty() ? env_or_empty("OPENEMS_DB_URL") : db_url;
  auto pg_result = load_from_postgres(effective_db_url);
  if (pg_result.is_ok()) {
    return pg_result;
  }

  OPENEMS_LOG_W("ConfigLoader", "PostgreSQL config load failed: " + pg_result.error_msg() + "; falling back to CSV: " + dir_path);
  return load_from_csv(dir_path);
}

}  // namespace openems::config
