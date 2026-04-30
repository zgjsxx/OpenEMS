#include "openems/strategy/strategy_types.h"
#include <cstdlib>
#include <algorithm>
#include <cctype>

namespace openems::strategy {

bool StrategyParams::parse(const std::string& key, const std::string& value) {
  auto parse_bool = [](std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return text == "1" || text == "true" || text == "yes" || text == "on";
  };
  double v = std::atof(value.c_str());
  if (key == "export_limit_kw")          { export_limit_kw = v; return true; }
  if (key == "max_charge_kw")            { max_charge_kw = v; return true; }
  if (key == "max_discharge_kw")         { max_discharge_kw = v; return true; }
  if (key == "deadband_kw")              { deadband_kw = v; return true; }
  if (key == "ramp_rate_kw_per_s")       { ramp_rate_kw_per_s = v; return true; }
  if (key == "soc_low")                  { soc_low = v; return true; }
  if (key == "soc_high")                 { soc_high = v; return true; }
  if (key == "manual_override_minutes")  { manual_override_minutes = static_cast<int>(v); return true; }
  if (key == "enable_pv_curtailment")    { enable_pv_curtailment = parse_bool(value); return true; }
  if (key == "pv_rated_power_kw")        { pv_rated_power_kw = v; return true; }
  if (key == "pv_limit_min_pct")         { pv_limit_min_pct = v; return true; }
  if (key == "pv_limit_max_pct")         { pv_limit_max_pct = v; return true; }
  if (key == "pv_limit_recovery_step_pct") { pv_limit_recovery_step_pct = v; return true; }
  return false;
}

} // namespace openems::strategy
