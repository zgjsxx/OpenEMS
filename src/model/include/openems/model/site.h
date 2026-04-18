// src/model/include/openems/model/site.h
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include "openems/common/types.h"
#include "openems/model/device.h"

namespace openems::model {

class Site {
public:
  Site(common::SiteId id, std::string name, std::string description);

  const common::SiteId& id() const { return id_; }
  const std::string& name() const { return name_; }
  const std::string& description() const { return description_; }

  void add_device(DevicePtr device);
  const std::vector<DevicePtr>& devices() const { return devices_; }
  DevicePtr find_device(const common::DeviceId& did) const;

  std::string to_string() const;

private:
  common::SiteId id_;
  std::string name_;
  std::string description_;
  std::vector<DevicePtr> devices_;
  mutable std::mutex mutex_;
};

using SitePtr = std::shared_ptr<Site>;

inline SitePtr SiteCreate(common::SiteId id, std::string name, std::string description) {
  return std::make_shared<Site>(std::move(id), std::move(name), std::move(description));
}

} // namespace openems::model