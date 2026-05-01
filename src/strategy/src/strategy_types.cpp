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
  auto parse_group_key = [](const std::string& text, const std::string& prefix,
                            std::string& group_out) {
    if (text.rfind(prefix, 0) != 0) {
      return false;
    }
    if (text.size() <= prefix.size()) {
      return false;
    }
    group_out = text.substr(prefix.size());
    return !group_out.empty();
  };

  std::string group;
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
  if (parse_group_key(key, "bess_max_charge_kw#", group)) {
    bess_max_charge_kw_by_group[group] = v;
    return true;
  }
  if (parse_group_key(key, "bess_max_discharge_kw#", group)) {
    bess_max_discharge_kw_by_group[group] = v;
    return true;
  }
  if (parse_group_key(key, "pv_rated_power_kw#", group)) {
    pv_rated_power_kw_by_group[group] = v;
    return true;
  }
  return false;
}

double StrategyParams::bess_charge_power_limit_for_group(const std::string& group) const {
  const auto it = bess_max_charge_kw_by_group.find(group);
  if (it != bess_max_charge_kw_by_group.end() && it->second > 0.0) {
    return it->second;
  }
  return max_charge_kw;
}

double StrategyParams::bess_discharge_power_limit_for_group(const std::string& group) const {
  const auto it = bess_max_discharge_kw_by_group.find(group);
  if (it != bess_max_discharge_kw_by_group.end() && it->second > 0.0) {
    return it->second;
  }
  return max_discharge_kw;
}

double StrategyParams::pv_rated_power_for_group(const std::string& group) const {
  const auto it = pv_rated_power_kw_by_group.find(group);
  if (it != pv_rated_power_kw_by_group.end() && it->second > 0.0) {
    return it->second;
  }
  return pv_rated_power_kw;
}

} // namespace openems::strategy
