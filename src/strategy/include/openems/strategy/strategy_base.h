#pragma once

#include <string>
#include <vector>
#include "openems/strategy/strategy_types.h"
#include "openems/core/point_handle.h"
#include "openems/core/command_handle.h"

namespace openems::strategy {

class IStrategy {
public:
  virtual ~IStrategy() = default;

  virtual const StrategyDefinition& definition() const = 0;

  // Execute one strategy cycle. Returns the action result.
  virtual StrategyExecutionResult execute() = 0;

  // Check if manual override is active for this strategy's device
  virtual bool is_manual_override() const = 0;
};

class StrategyBase : public IStrategy {
public:
  StrategyBase(StrategyDefinition def, StrategyParams params,
               rt_db::RtDb* rtdb);
  ~StrategyBase() override = default;

  const StrategyDefinition& definition() const override { return def_; }
  bool is_manual_override() const override;

protected:
  common::PointId find_binding(const std::string& role) const;

  StrategyDefinition def_;
  StrategyParams params_;
  rt_db::RtDb* rtdb_;
  std::vector<std::unique_ptr<core::PointHandle>> point_handles_;
  std::unique_ptr<core::CommandHandle> cmd_handle_;
};

} // namespace openems::strategy
