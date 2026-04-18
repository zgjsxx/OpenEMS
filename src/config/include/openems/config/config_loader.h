#pragma once

#include <string>
#include "openems/common/result.h"
#include "openems/config/ems_config.h"

namespace openems::config {

class ConfigLoader {
public:
  static common::Result<EmsConfig> load(const std::string& dir_path);
};

} // namespace openems::config