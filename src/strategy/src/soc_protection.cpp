#include "openems/strategy/soc_protection.h"
#include "openems/utils/logger.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace openems::strategy {

SocProtection::SocProtection(StrategyDefinition def, StrategyParams params,
                             rt_db::RtDb* rtdb)
    : StrategyBase(std::move(def), params, rtdb) {
  for (auto& ph : point_handles_) {
    for (const auto& b : def_.bindings) {
      if (b.point_id != ph->point_id()) continue;
      if (binding_role_base(b.role) == "bess_soc") bess_socs_.push_back(ph.get());
      if (binding_role_base(b.role) == "bess_power") bess_powers_.push_back(ph.get());
      if (binding_role_base(b.role) == "bess_run_state") bess_run_states_.push_back(ph.get());
    }
  }
}

SocProtection::SocClampResult SocProtection::clamp(double target_kw) {
  SocClampResult result;
  result.clamped_kw = target_kw;
  result.suppressed = false;

  if (bess_socs_.empty()) {
    result.suppressed = true;
    result.reason = "bess_soc binding not configured";
    return result;
  }

  const double soc_low = params_.soc_low;
  const double soc_high = params_.soc_high;
  bool any_soc_valid = false;
  bool any_eligible = (std::abs(target_kw) < 1e-6);

  for (size_t i = 0; i < bess_socs_.size(); ++i) {
    auto* bess_soc = bess_socs_[i];
    auto soc_result = bess_soc->read_value();
    if (!soc_result.is_ok() || !bess_soc->is_valid()) continue;
    any_soc_valid = true;

    bool running = true;
    if (i < bess_run_states_.size()) {
      auto* bess_run_state = bess_run_states_[i];
      auto run_result = bess_run_state->read_value();
      running = run_result.is_ok() && bess_run_state->is_valid() &&
                std::abs(run_result.value()) > 1e-6;
    }
    if (!running) continue;

    const double soc = soc_result.value();
    if (target_kw < 0.0 && soc < soc_high) {
      any_eligible = true;
      break;
    }
    if (target_kw > 0.0 && soc > soc_low) {
      any_eligible = true;
      break;
    }
  }

  if (!any_soc_valid) {
    result.suppressed = true;
    result.reason = "bess_soc read failed or invalid";
    return result;
  }

  if (!any_eligible) {
    result.clamped_kw = 0.0;
    result.suppressed = true;
    std::ostringstream oss;
    if (target_kw < 0.0) {
      oss << "all BESS SOC >= high limit(" << soc_high << "%), charge suppressed";
    } else if (target_kw > 0.0) {
      oss << "all BESS SOC <= low limit(" << soc_low << "%), discharge suppressed";
    } else {
      oss << "no eligible BESS available";
    }
    result.reason = oss.str();
  }

  return result;
}

StrategyExecutionResult SocProtection::execute() {
  StrategyExecutionResult result;
  result.target_power_kw = 0.0;
  bool corrective_action = false;

  if (bess_socs_.empty() || bess_powers_.empty()) {
    result.suppressed = true;
    result.suppress_reason = "required bindings not configured";
    return result;
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
      result.suppress_reason = "BESS not running";
      return result;
    }
  }

  double p_bess = 0.0;
  for (auto* bess_power : bess_powers_) {
    auto power_result = bess_power->read_value();
    if (power_result.is_ok() && bess_power->is_valid()) {
      p_bess += power_result.value();
    }
  }

  double soc_sum = 0.0;
  int soc_count = 0;
  for (auto* bess_soc : bess_socs_) {
    auto soc_result = bess_soc->read_value();
    if (soc_result.is_ok() && bess_soc->is_valid()) {
      soc_sum += soc_result.value();
      ++soc_count;
    }
  }
  if (soc_count == 0) {
    result.suppressed = true;
    result.suppress_reason = "bess_soc read failed or invalid";
    return result;
  }
  const double soc = soc_sum / static_cast<double>(soc_count);

  const double soc_low = params_.soc_low;
  const double soc_high = params_.soc_high;
  double target = 0.0;

  if (soc <= soc_low) {
    if (p_bess > 0.0) {
      target = 0.0;
      result.suppress_reason = "SOC below low limit, stopping discharge";
      corrective_action = true;
    }
  } else if (soc >= soc_high) {
    if (p_bess < 0.0) {
      target = 0.0;
      result.suppress_reason = "SOC above high limit, stopping charge";
      corrective_action = true;
    }
  } else {
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
