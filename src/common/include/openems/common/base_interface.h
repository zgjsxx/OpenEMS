// src/common/include/openems/common/base_interface.h
#pragma once

#include <string>
#include <cstdint>

namespace openems::common {

// 所有可命名模块的基础接口
class IModule {
public:
  virtual ~IModule() = default;
  virtual std::string name() const = 0;
  virtual bool start() = 0;
  virtual bool stop() = 0;
};

// 生命周期状态
enum class LifecycleState : uint8_t {
  Created   = 0,
  Starting  = 1,
  Running   = 2,
  Stopping  = 3,
  Stopped   = 4,
  Fault     = 5,
};

} // namespace openems::common