// src/model/src/site.cpp
#include "openems/model/site.h"
#include <sstream>

namespace openems::model {

Site::Site(common::SiteId id, std::string name, std::string description)
    : id_(std::move(id)), name_(std::move(name)),
      description_(std::move(description)) {}

void Site::add_device(DevicePtr device) {
  std::lock_guard lock(mutex_);
  devices_.push_back(std::move(device));
}

DevicePtr Site::find_device(const common::DeviceId& did) const {
  std::lock_guard lock(mutex_);
  for (auto& d : devices_) {
    if (d->id() == did) return d;
  }
  return nullptr;
}

std::string Site::to_string() const {
  std::ostringstream oss;
  oss << "Site[" << id_ << "] " << name_
      << " desc=" << description_
      << " devices=" << devices_.size();
  return oss.str();
}

} // namespace openems::model