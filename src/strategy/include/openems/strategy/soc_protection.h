#pragma once

#include "openems/strategy/strategy_base.h"

namespace openems::strategy {

class SocProtection : public StrategyBase {
public:
  SocProtection(StrategyDefinition def, StrategyParams params,
                rt_db::RtDb* rtdb);

  StrategyExecutionResult execute() override;

  // Clamp a target power from upstream strategy (e.g. AntiReverseFlow)
  // based on SOC limits. Returns the clamped value and a suppress reason.
  struct SocClampResult {
    double clamped_kw;
    bool suppressed;
    std::string reason;
  };

  SocClampResult clamp(double target_kw);

private:
  std::vector<core::PointHandle*> bess_socs_;
  std::vector<core::PointHandle*> bess_powers_;
  std::vector<core::PointHandle*> bess_run_states_;
};

} // namespace openems::strategy
