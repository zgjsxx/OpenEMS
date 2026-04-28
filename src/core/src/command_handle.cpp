#include "openems/core/command_handle.h"
#include <cmath>
#include <algorithm>

namespace openems::core {

CommandHandle::CommandHandle(common::PointId point_id, rt_db::RtDb* rtdb,
                             double deadband, double min_change)
    : point_id_(std::move(point_id)), rtdb_(rtdb),
      deadband_(deadband), min_change_(min_change) {}

common::Result<std::string> CommandHandle::submit(double desired_value) {
  if (ever_sent_ && std::abs(desired_value - last_sent_value_) < deadband_) {
    return std::string("debounced");
  }

  auto status_result = rtdb_->read_command_status(point_id_);
  if (status_result.is_ok()) {
    auto& cmd = status_result.value();
    if (cmd.status == rt_db::CommandPending ||
        cmd.status == rt_db::CommandExecuting) {
      return std::string("busy");
    }
  }

  auto result = rtdb_->submit_command(point_id_, desired_value);
  if (!result.is_ok()) {
    return common::Result<std::string>::Err(common::ErrorCode::Unknown, result.error_msg());
  }

  last_sent_value_ = desired_value;
  ever_sent_ = true;
  return std::string("submitted");
}

bool CommandHandle::has_pending() const {
  auto result = rtdb_->read_command_status(point_id_);
  if (!result.is_ok()) return false;
  auto& cmd = result.value();
  return cmd.status == rt_db::CommandPending ||
         cmd.status == rt_db::CommandExecuting;
}

void CommandHandle::reset() {
  ever_sent_ = false;
  last_sent_value_ = 0.0;
}

} // namespace openems::core
