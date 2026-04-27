#pragma once

#include <string>
#include "openems/common/result.h"
#include "openems/config/ems_config.h"

namespace openems::config {

class ConfigLoader {
public:
  static common::Result<EmsConfig> load(const std::string& dir_path);
  static common::Result<EmsConfig> load_from_csv(const std::string& dir_path);
  static common::Result<EmsConfig> load_from_postgres(const std::string& db_url);
  static common::Result<EmsConfig> load(const std::string& source,
                                        const std::string& dir_path,
                                        const std::string& db_url);
};

} // namespace openems::config
