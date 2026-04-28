#pragma once

#include <string>
#include <cstdint>
#include "openems/common/types.h"
#include "openems/common/result.h"
#include "openems/rt_db/rt_db.h"

namespace openems::core {

class CommandHandle {
public:
  CommandHandle(common::PointId point_id, rt_db::RtDb* rtdb,
                double deadband = 0.0, double min_change = 1.0);

  const common::PointId& point_id() const { return point_id_; }

  // Submit a command value. Returns:
  //   Ok("submitted")  — command was written to the slot
  //   Ok("debounced")  — skipped because change < deadband
  //   Ok("busy")       — skipped because slot is Pending/Executing
  //   Err(...)         — point not found or other error
  common::Result<std::string> submit(double desired_value);

  double last_sent_value() const { return last_sent_value_; }
  bool has_pending() const;

  // Reset debounce state (e.g. after manual override clears)
  void reset();

private:
  common::PointId point_id_;
  rt_db::RtDb* rtdb_;
  double deadband_;
  double min_change_;
  double last_sent_value_ = 0.0;
  bool ever_sent_ = false;
};

} // namespace openems::core
