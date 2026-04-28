#pragma once

#include <string>
#include "openems/common/types.h"
#include "openems/common/result.h"
#include "openems/rt_db/rt_db.h"

namespace openems::core {

class PointHandle {
public:
  PointHandle(common::PointId point_id, rt_db::RtDb* rtdb);

  const common::PointId& point_id() const { return point_id_; }

  common::Result<rt_db::TelemetryReadResult> read_telemetry();
  common::Result<rt_db::TeleindicationReadResult> read_teleindication();

  // Read as double regardless of underlying type
  common::Result<double> read_value();

  bool is_valid() const { return last_valid_; }
  double last_value() const { return last_value_; }
  uint64_t last_timestamp() const { return last_timestamp_; }
  common::Quality last_quality() const { return last_quality_; }

private:
  common::PointId point_id_;
  rt_db::RtDb* rtdb_;

  bool last_valid_ = false;
  double last_value_ = 0.0;
  uint64_t last_timestamp_ = 0;
  common::Quality last_quality_ = common::Quality::Invalid;
};

} // namespace openems::core
