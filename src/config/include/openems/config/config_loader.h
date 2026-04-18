// src/config/include/openems/config/config_loader.h
#pragma once

#include <string>
#include "openems/common/result.h"
#include "openems/config/ems_config.h"

namespace openems::config {

class ConfigLoader {
public:
  static common::Result<EmsConfig> load_from_json(const std::string& path);
  static common::Result<EmsConfig> load_from_yaml(const std::string& path);
  static common::Result<EmsConfig> load_from_csv_dir(const std::string& dir_path);
  static common::Result<EmsConfig> load(const std::string& path);
};

} // namespace openems::config