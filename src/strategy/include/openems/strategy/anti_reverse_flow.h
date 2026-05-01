#pragma once

#include "openems/strategy/strategy_base.h"

namespace openems::strategy {

class AntiReverseFlow : public StrategyBase {
public:
  AntiReverseFlow(StrategyDefinition def, StrategyParams params,
                  rt_db::RtDb* rtdb);

  StrategyExecutionResult calculate_target();
  StrategyExecutionResult execute() override;
  void mark_target_applied(double target_kw);

private:
  core::PointHandle* grid_power_;
  std::vector<core::PointHandle*> bess_powers_;
  std::vector<core::PointHandle*> bess_run_states_;
  double locked_target_ = 0.0;
  bool has_locked_target_ = false;
  int recalc_confirm_cycles_ = 0;
};

} // namespace openems::strategy
