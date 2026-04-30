#include "openems/strategy/anti_reverse_flow.h"
#include "openems/utils/logger.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace openems::strategy {

AntiReverseFlow::AntiReverseFlow(StrategyDefinition def, StrategyParams params,
                                 rt_db::RtDb* rtdb)
    : StrategyBase(std::move(def), params, rtdb)
    , grid_power_(nullptr)
    , bess_power_(nullptr)
    , bess_run_state_(nullptr) {
  for (auto& ph : point_handles_) {
    for (const auto& b : def_.bindings) {
      if (b.point_id == ph->point_id()) {
        if (b.role == "grid_power") grid_power_ = ph.get();
        if (b.role == "bess_power") bess_power_ = ph.get();
        if (b.role == "bess_run_state") bess_run_state_ = ph.get();
      }
    }
  }
}

StrategyExecutionResult AntiReverseFlow::execute() {
  StrategyExecutionResult result;
  result = calculate_target();

  if (result.suppressed) {
    return result;
  }

  if (!cmd_handle_) {
    result.suppressed = true;
    result.suppress_reason = "bess_power_setpoint binding not configured";
    prev_target_ = result.target_power_kw;
    return result;
  }

  auto submit_result = cmd_handle_->submit(result.target_power_kw);
  if (submit_result.is_ok()) {
    result.command_sent = (submit_result.value() == "submitted");
    result.command_result = submit_result.value();
    if (!result.command_sent) {
      result.suppressed = true;
      result.suppress_reason = submit_result.value();
    }
  } else {
    result.suppressed = true;
    result.suppress_reason =
        "command submit failed: " + submit_result.error_msg();
  }

  prev_target_ = result.target_power_kw;
  return result;
}

StrategyExecutionResult AntiReverseFlow::calculate_target() {
  StrategyExecutionResult result;
  result.target_power_kw = 0.0;

  if (!grid_power_) {
    result.suppressed = true;
    result.suppress_reason = "grid_power binding not configured";
    return result;
  }

  auto grid_result = grid_power_->read_value();
  if (!grid_result.is_ok() || !grid_power_->is_valid()) {
    result.suppressed = true;
    result.suppress_reason = "grid_power read failed or invalid";
    return result;
  }

  double p_grid = grid_result.value();
  double p_bess = prev_target_;

  if (bess_power_) {
    auto bess_result = bess_power_->read_value();
    if (bess_result.is_ok() && bess_power_->is_valid()) {
      p_bess = bess_result.value();
    }
  }

  if (bess_run_state_) {
    auto run_result = bess_run_state_->read_value();
    if (run_result.is_ok() && bess_run_state_->is_valid()) {
      if (run_result.value() == 0.0) {
        result.suppressed = true;
        result.suppress_reason = "BESS not running (run_state=0)";
        return result;
      }
    }
  }

  const double export_limit = params_.export_limit_kw;
  const double max_charge = params_.max_charge_kw;
  const double max_discharge = params_.max_discharge_kw;
  const double deadband = params_.deadband_kw;
  const double ramp_rate = params_.ramp_rate_kw_per_s;
  const double dt = static_cast<double>(def_.cycle_ms) / 1000.0;

  const double grid_error = p_grid - export_limit;
  double raw_target = 0.0;

  // Close the loop around the current BESS operating point. With the current
  // sign convention, charging power is negative, so reverse flow naturally
  // pushes the target further negative instead of bouncing back to zero.
  if (std::abs(grid_error) < deadband) {
    raw_target = p_bess;
  } else {
    raw_target = p_bess + grid_error;
  }

  raw_target = std::max(raw_target, -max_charge);
  raw_target = std::min(raw_target, max_discharge);

  const double max_step = ramp_rate * dt;
  double target = std::max(prev_target_ - max_step,
                           std::min(prev_target_ + max_step, raw_target));

  if (std::abs(target) < deadband) {
    target = 0.0;
  }

  result.target_power_kw = target;
  return result;
}

void AntiReverseFlow::mark_target_applied(double target_kw) {
  prev_target_ = target_kw;
}

} // namespace openems::strategy
