#pragma once

#include "openems/strategy/strategy_base.h"

namespace openems::strategy {

class AntiReverseFlow : public StrategyBase {
public:
  AntiReverseFlow(StrategyDefinition def, StrategyParams params,
                  rt_db::RtDb* rtdb);

  StrategyExecutionResult execute() override;

private:
  core::PointHandle* grid_power_;
  core::PointHandle* bess_power_;
  core::PointHandle* bess_run_state_;
  double prev_target_ = 0.0;
};

} // namespace openems::strategy
