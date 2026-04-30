#include "openems/strategy/soc_protection.h"
#include "openems/utils/logger.h"
#include <cmath>
#include <algorithm>
#include <sstream>

namespace openems::strategy {

SocProtection::SocProtection(StrategyDefinition def, StrategyParams params,
                             rt_db::RtDb* rtdb)
    : StrategyBase(std::move(def), params, rtdb)
    , bess_soc_(nullptr)
    , bess_power_(nullptr)
    , bess_run_state_(nullptr) {
  for (auto& ph : point_handles_) {
    for (const auto& b : def_.bindings) {
      if (b.point_id == ph->point_id()) {
        if (b.role == "bess_soc")         bess_soc_ = ph.get();
        if (b.role == "bess_power")       bess_power_ = ph.get();
        if (b.role == "bess_run_state")   bess_run_state_ = ph.get();
      }
    }
  }
}

SocProtection::SocClampResult SocProtection::clamp(double target_kw) {
  SocClampResult result;
  result.clamped_kw = target_kw;
  result.suppressed = false;

  if (!bess_soc_) {
    result.suppressed = true;
    result.reason = "bess_soc binding not configured";
    return result;
  }

  auto soc_result = bess_soc_->read_value();
  if (!soc_result.is_ok() || !bess_soc_->is_valid()) {
    result.suppressed = true;
    result.reason = "bess_soc read failed or invalid";
    return result;
  }
  double soc = soc_result.value();

  double soc_low = params_.soc_low;
  double soc_high = params_.soc_high;

  // SOC ≤ low limit: forbid discharge.
  // Allow charge only (clamp target ≤ 0).
  // If target > 0 (trying to discharge), clamp to 0.
  if (soc <= soc_low) {
    if (target_kw > 0.0) {
      result.clamped_kw = 0.0;
      result.suppressed = true;
      std::ostringstream oss;
      oss << "SOC(" << soc << "%) <= low limit(" << soc_low
          << "%), discharge suppressed";
      result.reason = oss.str();
      return result;
    }
  }

  // SOC ≥ high limit: forbid charge.
  // Allow discharge only (clamp target ≥ 0).
  // If target < 0 (trying to charge), clamp to 0.
  if (soc >= soc_high) {
    if (target_kw < 0.0) {
      result.clamped_kw = 0.0;
      result.suppressed = true;
      std::ostringstream oss;
      oss << "SOC(" << soc << "%) >= high limit(" << soc_high
          << "%), charge suppressed";
      result.reason = oss.str();
      return result;
    }
  }

  return result;
}

StrategyExecutionResult SocProtection::execute() {
  StrategyExecutionResult result;
  result.target_power_kw = 0.0;
  bool corrective_action = false;

  if (!bess_soc_ || !bess_power_) {
    result.suppressed = true;
    result.suppress_reason = "required bindings not configured";
    return result;
  }

  if (bess_run_state_) {
    auto run_result = bess_run_state_->read_value();
    if (run_result.is_ok() && bess_run_state_->is_valid()) {
      if (run_result.value() == 0.0) {
        result.suppressed = true;
        result.suppress_reason = "BESS not running";
        return result;
      }
    }
  }

  // Read current power to determine direction
  auto power_result = bess_power_->read_value();
  double p_bess = 0.0;
  if (power_result.is_ok() && bess_power_->is_valid()) {
    p_bess = power_result.value();
  }

  auto soc_result = bess_soc_->read_value();
  if (!soc_result.is_ok() || !bess_soc_->is_valid()) {
    result.suppressed = true;
    result.suppress_reason = "bess_soc read failed or invalid";
    return result;
  }
  double soc = soc_result.value();

  double soc_low = params_.soc_low;
  double soc_high = params_.soc_high;

  // When SOC is at limits, issue corrective action.
  // This is a standalone execution (without anti-reverse-flow output).
  // When called via clamp(), the upstream target is used instead.

  double target = 0.0;

  if (soc <= soc_low) {
    // SOC too low: if currently discharging (p_bess > 0), stop it
    if (p_bess > 0.0) {
      target = 0.0;  // Stop discharge
      result.suppress_reason = "SOC below low limit, stopping discharge";
      corrective_action = true;
    }
  } else if (soc >= soc_high) {
    // SOC too high: if currently charging (p_bess < 0), stop it
    if (p_bess < 0.0) {
      target = 0.0;  // Stop charge
      result.suppress_reason = "SOC above high limit, stopping charge";
      corrective_action = true;
    }
  } else {
    // SOC in normal range — nothing to do (upstream strategy controls)
    result.suppress_reason = "SOC in normal range, no action";
    result.suppressed = true;
    return result;
  }

  result.target_power_kw = target;

  if (cmd_handle_ && corrective_action) {
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
      result.suppress_reason =
          "command submit failed: " + submit_result.error_msg();
    }
  }

  return result;
}

} // namespace openems::strategy
