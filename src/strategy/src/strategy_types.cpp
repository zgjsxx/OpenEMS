#include "openems/strategy/strategy_types.h"
#include <cstdlib>

namespace openems::strategy {

bool StrategyParams::parse(const std::string& key, const std::string& value) {
  double v = std::atof(value.c_str());
  if (key == "export_limit_kw")          { export_limit_kw = v; return true; }
  if (key == "max_charge_kw")            { max_charge_kw = v; return true; }
  if (key == "max_discharge_kw")         { max_discharge_kw = v; return true; }
  if (key == "deadband_kw")              { deadband_kw = v; return true; }
  if (key == "ramp_rate_kw_per_s")       { ramp_rate_kw_per_s = v; return true; }
  if (key == "soc_low")                  { soc_low = v; return true; }
  if (key == "soc_high")                 { soc_high = v; return true; }
  if (key == "manual_override_minutes")  { manual_override_minutes = static_cast<int>(v); return true; }
  return false;
}

} // namespace openems::strategy
