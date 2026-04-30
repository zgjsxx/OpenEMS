#include "openems/strategy/strategy_base.h"
#include <algorithm>

namespace openems::strategy {

StrategyBase::StrategyBase(StrategyDefinition def, StrategyParams params,
                           rt_db::RtDb* rtdb)
    : def_(std::move(def)), params_(params), rtdb_(rtdb) {
  for (const auto& binding : def_.bindings) {
    point_handles_.push_back(
        std::make_unique<core::PointHandle>(binding.point_id, rtdb));
  }

  auto setpoint_id = find_binding("bess_power_setpoint");
  if (!setpoint_id.empty()) {
    cmd_handle_ = std::make_unique<core::CommandHandle>(
        setpoint_id, rtdb, params_.deadband_kw * 0.5, 1.0);
  }
}

common::PointId StrategyBase::find_binding(const std::string& role) const {
  for (const auto& b : def_.bindings) {
    if (b.role == role || binding_role_base(b.role) == role) return b.point_id;
  }
  return {};
}

bool StrategyBase::is_manual_override() const {
  // Manual override is checked by reading the device's override timestamp
  // from strategy_runtime_state in PostgreSQL. For now, check via RtDb
  // command slot status — if a command was submitted externally (by web
  // console) within the manual_override_minutes window, consider it active.
  //
  // The strategy engine main loop loads manual_override_until from DB
  // and passes it to the strategy. This base implementation returns false;
  // override is managed by the engine's main loop.
  return false;
}

} // namespace openems::strategy
