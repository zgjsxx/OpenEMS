#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "openems/common/types.h"

namespace openems::strategy {

enum class StrategyType : uint8_t {
  AntiReverseFlow = 0,
  SocProtection   = 1,
};

inline StrategyType strategy_type_from_string(const std::string& s) {
  if (s == "anti_reverse_flow") return StrategyType::AntiReverseFlow;
  if (s == "soc_protection")    return StrategyType::SocProtection;
  return StrategyType::AntiReverseFlow;
}

inline std::string strategy_type_to_string(StrategyType t) {
  switch (t) {
    case StrategyType::AntiReverseFlow: return "anti_reverse_flow";
    case StrategyType::SocProtection:   return "soc_protection";
  }
  return "unknown";
}

struct StrategyBinding {
  std::string role;     // "grid_power", "bess_power", "bess_soc", "bess_run_state",
                        // "bess_power_setpoint", "pv_power", "pv_power_limit_setpoint",
                        // "pv_run_state"
  common::PointId point_id;
};

struct StrategyDefinition {
  std::string id;
  std::string name;
  StrategyType type;
  bool enabled = true;
  common::SiteId site_id;
  common::DeviceId device_id;
  int priority = 0;
  int cycle_ms = 1000;
  std::vector<StrategyBinding> bindings;
};

struct StrategyParams {
  // Anti-reverse-flow
  double export_limit_kw  = 0.0;     // grid export limit, 0 = no export
  double max_charge_kw    = 100.0;   // max charge power
  double max_discharge_kw = 100.0;   // max discharge power
  double deadband_kw      = 0.5;     // deadband for anti-reverse-flow
  double ramp_rate_kw_per_s = 20.0;  // max ramp rate

  // SOC protection
  double soc_low          = 20.0;    // SOC lower limit (%)
  double soc_high         = 80.0;    // SOC upper limit (%)

  // Manual override
  int manual_override_minutes = 30;

  // Optional PV curtailment compensation, used as the third layer after
  // anti-reverse-flow target calculation and SOC clamping.
  bool enable_pv_curtailment = false;
  double pv_rated_power_kw = 0.0;
  double pv_limit_min_pct = 0.0;
  double pv_limit_max_pct = 100.0;
  double pv_limit_recovery_step_pct = 10.0;

  bool parse(const std::string& key, const std::string& value);
};

struct StrategyExecutionResult {
  double target_power_kw = 0.0;
  bool suppressed = false;
  std::string suppress_reason;
  bool command_sent = false;
  std::string command_result;  // "submitted", "debounced", "busy"
};

} // namespace openems::strategy
