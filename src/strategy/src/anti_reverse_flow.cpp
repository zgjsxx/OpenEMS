#include "openems/strategy/anti_reverse_flow.h"
#include "openems/utils/logger.h"
#include <cmath>
#include <algorithm>
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
        if (b.role == "grid_power")       grid_power_ = ph.get();
        if (b.role == "bess_power")       bess_power_ = ph.get();
        if (b.role == "bess_run_state")   bess_run_state_ = ph.get();
      }
    }
  }
}

StrategyExecutionResult AntiReverseFlow::execute() {
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

  double export_limit = params_.export_limit_kw;
  double max_charge = params_.max_charge_kw;
  double max_discharge = params_.max_discharge_kw;
  double deadband = params_.deadband_kw;
  double ramp_rate = params_.ramp_rate_kw_per_s;
  double dt = static_cast<double>(def_.cycle_ms) / 1000.0;

  double raw_target = 0.0;

  if (p_grid < export_limit) {
    // Reverse flow: P_grid is below export limit (e.g. P_grid = -10kW, limit = 0)
    // Target = P_grid - limit — negative value means charge BESS to absorb
    raw_target = p_grid - export_limit;
  } else {
    // No reverse flow. Release BESS back to 0 gradually.
    raw_target = 0.0;
  }

  // Clamp to BESS power limits
  raw_target = std::max(raw_target, -max_charge);
  raw_target = std::min(raw_target, max_discharge);

  // Apply ramp rate limit
  double max_step = ramp_rate * dt;
  double target = std::max(prev_target_ - max_step,
                  std::min(prev_target_ + max_step, raw_target));

  // Deadband: if target is within deadband of zero, set to zero
  if (std::abs(target) < deadband) {
    target = 0.0;
  }

  result.target_power_kw = target;

  if (!cmd_handle_) {
    result.suppressed = true;
    result.suppress_reason = "bess_power_setpoint binding not configured";
    prev_target_ = target;
    return result;
  }

  if (std::abs(target - prev_target_) < deadband * 0.5) {
    result.command_sent = false;
    result.suppress_reason = "target within deadband, not re-sending";
  } else {
    auto submit_result = cmd_handle_->submit(target);
    if (submit_result.is_ok()) {
      result.command_sent = (submit_result.value() == "submitted");
      result.command_result = submit_result.value();
      if (!result.command_sent) {
        result.suppressed = true;
        result.suppress_reason = submit_result.value();
      }
    } else {
      result.suppressed = true;
      result.suppress_reason = "command submit failed: " + submit_result.error_msg();
    }
  }

  prev_target_ = target;
  return result;
}

} // namespace openems::strategy
