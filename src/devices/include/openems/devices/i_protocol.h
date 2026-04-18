#pragma once

#include <string>
#include "openems/common/result.h"

namespace openems::devices {

class IProtocol {
public:
  virtual ~IProtocol() = default;
  virtual std::string protocol_name() const = 0;
  virtual common::VoidResult connect() = 0;
  virtual void disconnect() = 0;
  virtual bool is_connected() const = 0;
};

} // namespace openems::devices