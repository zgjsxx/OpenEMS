#include "openems/strategy/anti_reverse_flow.h"
#include "openems/utils/logger.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace openems::strategy {

namespace {
constexpr int kRecalcConfirmCycles = 2;
}

AntiReverseFlow::AntiReverseFlow(StrategyDefinition def, StrategyParams params,
                                 rt_db::RtDb* rtdb)
    : StrategyBase(std::move(def), params, rtdb)
    , grid_power_(nullptr) {
  for (auto& ph : point_handles_) {
    for (const auto& b : def_.bindings) {
      if (b.point_id == ph->point_id()) {
        if (b.role == "grid_power") grid_power_ = ph.get();
        if (binding_role_base(b.role) == "bess_power") bess_powers_.push_back(ph.get());
        if (binding_role_base(b.role) == "bess_run_state") bess_run_states_.push_back(ph.get());
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
    locked_target_ = result.target_power_kw;
    has_locked_target_ = true;
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

  locked_target_ = result.target_power_kw;
  has_locked_target_ = true;
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
  double p_bess = has_locked_target_ ? locked_target_ : 0.0;

  if (!bess_powers_.empty()) {
    double total_bess_power = 0.0;
    bool has_bess_power = false;
    for (auto* bess_power : bess_powers_) {
      auto bess_result = bess_power->read_value();
      if (bess_result.is_ok() && bess_power->is_valid()) {
        total_bess_power += bess_result.value();
        has_bess_power = true;
      }
    }
    if (has_bess_power) {
      p_bess = total_bess_power;
    }
  }

  if (!bess_run_states_.empty()) {
    bool any_running = false;
    for (auto* bess_run_state : bess_run_states_) {
      auto run_result = bess_run_state->read_value();
      if (run_result.is_ok() && bess_run_state->is_valid() &&
          std::abs(run_result.value()) > 1e-6) {
        any_running = true;
        break;
      }
    }
    if (!any_running) {
      result.suppressed = true;
      result.suppress_reason = "BESS not running (all run_state=0)";
      return result;
    }
  }

  const double export_limit = params_.export_limit_kw;
  const double max_charge = params_.max_charge_kw;
  const double max_discharge = params_.max_discharge_kw;
  const double deadband = params_.deadband_kw;

  const double grid_error = p_grid - export_limit;
  const double track_tolerance = std::max(deadband, 0.2);

  bool should_recalculate = !has_locked_target_;
  if (!should_recalculate) {
    const bool tracking_in_progress =
        std::abs(p_bess - locked_target_) > track_tolerance;
    if (tracking_in_progress) {
      recalc_confirm_cycles_ = 0;
    } else if (std::abs(grid_error) > deadband) {
      recalc_confirm_cycles_ += 1;
      if (recalc_confirm_cycles_ >= kRecalcConfirmCycles) {
        should_recalculate = true;
      }
    } else {
      recalc_confirm_cycles_ = 0;
    }
  }

  if (should_recalculate) {
    double raw_target = p_bess;
    if (std::abs(grid_error) >= deadband) {
      raw_target += grid_error;
    }
    raw_target = std::max(raw_target, -max_charge);
    raw_target = std::min(raw_target, max_discharge);
    if (std::abs(raw_target) < deadband) {
      raw_target = 0.0;
    }
    locked_target_ = raw_target;
    has_locked_target_ = true;
    recalc_confirm_cycles_ = 0;
  }

  result.target_power_kw = locked_target_;
  return result;
}

void AntiReverseFlow::mark_target_applied(double target_kw) {
  locked_target_ = target_kw;
  has_locked_target_ = true;
}

} // namespace openems::strategy
