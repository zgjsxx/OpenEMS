// src/config/src/config_loader.cpp
#include "openems/config/config_loader.h"
#include "openems/config/csv_parser.h"
#include "openems/utils/logger.h"

namespace openems::config {

static common::PointCategory parse_point_category(const std::string& s) {
  if (s == "telemetry")      return common::PointCategory::Telemetry;
  if (s == "teleindication") return common::PointCategory::Teleindication;
  if (s == "telecontrol")    return common::PointCategory::Telecontrol;
  if (s == "setting")        return common::PointCategory::Setting;
  return common::PointCategory::Telemetry;
}

static common::DataType parse_data_type(const std::string& s) {
  if (s == "bool")    return common::DataType::Bool;
  if (s == "int16")   return common::DataType::Int16;
  if (s == "uint16")  return common::DataType::Uint16;
  if (s == "int32")   return common::DataType::Int32;
  if (s == "uint32")  return common::DataType::Uint32;
  if (s == "float32") return common::DataType::Float32;
  if (s == "int64")   return common::DataType::Int64;
  if (s == "uint64")  return common::DataType::Uint64;
  if (s == "double")  return common::DataType::Double;
  return common::DataType::Uint16;
}

static common::DeviceType parse_device_type(const std::string& s) {
  if (s == "PV")          return common::DeviceType::PV;
  if (s == "BESS")        return common::DeviceType::BESS;
  if (s == "Meter")       return common::DeviceType::Meter;
  if (s == "Inverter")    return common::DeviceType::Inverter;
  if (s == "Grid")        return common::DeviceType::Grid;
  if (s == "Transformer") return common::DeviceType::Transformer;
  return common::DeviceType::Unknown;
}

common::Result<EmsConfig> ConfigLoader::load(const std::string& dir_path) {
  auto csv_path = [&](const std::string& filename) {
    std::string p = dir_path;
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
    return p + filename;
  };

  // 1. Read ems_config.csv
  auto ems_cfg_result = parse_csv_file(csv_path("ems_config.csv"));
  if (!ems_cfg_result.is_ok()) return common::Result<EmsConfig>::Err(ems_cfg_result.error_code(), ems_cfg_result.error_msg());
  auto& ems_cfg_table = ems_cfg_result.value();
  if (ems_cfg_table.rows.empty()) return common::Result<EmsConfig>::Err(common::ErrorCode::InvalidConfig, "ems_config.csv has no data rows");

  EmsConfig config;
  auto& ems_row = ems_cfg_table.rows[0];
  auto log_level_idx = ems_cfg_table.col_index("log_level");
  auto poll_idx = ems_cfg_table.col_index("default_poll_interval_ms");
  auto site_id_idx = ems_cfg_table.col_index("site_id");

  config.log_level = csv_string(ems_row, log_level_idx.value_or(0), "info");
  config.default_poll_interval_ms = static_cast<uint32_t>(csv_int(ems_row, poll_idx.value_or(1), 1000));
  std::string site_id = csv_string(ems_row, site_id_idx.value_or(2), "site-001");

  // 2. Read site.csv
  auto site_result = parse_csv_file(csv_path("site.csv"));
  if (!site_result.is_ok()) return common::Result<EmsConfig>::Err(site_result.error_code(), site_result.error_msg());
  auto& site_table = site_result.value();
  auto* site_row = site_table.find("id", site_id);
  if (!site_row) return common::Result<EmsConfig>::Err(common::ErrorCode::InvalidConfig, "site_id '" + site_id + "' not found in site.csv");

  auto site_name_idx = site_table.col_index("name");
  auto site_desc_idx = site_table.col_index("description");
  config.site.id = site_id;
  config.site.name = csv_string(*site_row, site_name_idx.value_or(1), "Default Site");
  config.site.description = csv_string(*site_row, site_desc_idx.value_or(2), "");

  // 3. Read device.csv
  auto device_result = parse_csv_file(csv_path("device.csv"));
  if (!device_result.is_ok()) return common::Result<EmsConfig>::Err(device_result.error_code(), device_result.error_msg());
  auto& device_table = device_result.value();

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

  // 4. Read telemetry.csv and teleindication.csv
  auto load_point_table = [&](const std::string& filename, common::PointCategory cat) -> common::Result<CsvTable> {
    auto result = parse_csv_file(csv_path(filename));
    if (!result.is_ok()) return result;
    auto& table = result.value();

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
    return result;
  };

  auto telem_result = load_point_table("telemetry.csv", common::PointCategory::Telemetry);
  if (!telem_result.is_ok()) return common::Result<EmsConfig>::Err(telem_result.error_code(), telem_result.error_msg());

  auto ti_result = load_point_table("teleindication.csv", common::PointCategory::Teleindication);
  if (!ti_result.is_ok()) return common::Result<EmsConfig>::Err(ti_result.error_code(), ti_result.error_msg());

  auto tc_result = load_point_table("telecontrol.csv", common::PointCategory::Telecontrol);
  // telecontrol.csv is optional — ignore if not found
  if (!tc_result.is_ok()) {
    OPENEMS_LOG_D("ConfigLoader", "telecontrol.csv not found or empty — skipping");
  }

  auto st_result = load_point_table("setting.csv", common::PointCategory::Setting);
  // setting.csv is optional — ignore if not found
  if (!st_result.is_ok()) {
    OPENEMS_LOG_D("ConfigLoader", "setting.csv not found or empty — skipping");
  }

  // 5. Read modbus_mapping.csv
  auto mb_result = parse_csv_file(csv_path("modbus_mapping.csv"));
  if (!mb_result.is_ok()) return common::Result<EmsConfig>::Err(mb_result.error_code(), mb_result.error_msg());
  auto& mb_table = mb_result.value();

  auto m_pid_idx = mb_table.col_index("point_id");
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

  // 6. Read iec104_mapping.csv
  auto ic_result = parse_csv_file(csv_path("iec104_mapping.csv"));
  if (!ic_result.is_ok()) return common::Result<EmsConfig>::Err(ic_result.error_code(), ic_result.error_msg());
  auto& ic_table = ic_result.value();

  auto i_pid_idx = ic_table.col_index("point_id");
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

} // namespace openems::config