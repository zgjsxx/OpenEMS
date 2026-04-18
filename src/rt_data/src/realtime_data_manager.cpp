// src/rt_data/src/realtime_data_manager.cpp
#include "openems/rt_data/realtime_data_manager.h"
#include "openems/utils/logger.h"
#include <iostream>
#include <sstream>

namespace openems::rt_data {

std::string PointSnapshot::to_string() const {
  std::ostringstream oss;
  oss << "  [" << point_id << "] " << point_name
      << " = " << value.to_string()
      << " " << unit;
  return oss.str();
}

std::string DeviceSnapshot::to_string() const {
  std::ostringstream oss;
  oss << "Device[" << device_id << "] " << device_name
      << " status=" << static_cast<int>(status);
  for (auto& p : points) {
    oss << "\n" << p.to_string();
  }
  return oss.str();
}

std::string SiteSnapshot::to_string() const {
  std::ostringstream oss;
  oss << "=== Site[" << site_id << "] " << site_name << " ===";
  for (auto& d : devices) {
    oss << "\n" << d.to_string();
  }
  return oss.str();
}

RealtimeDataManager::RealtimeDataManager(model::SitePtr site)
    : site_(std::move(site)) {}

void RealtimeDataManager::update_point(
    const common::DeviceId& device_id,
    const common::PointId& point_id,
    const model::PointValue& value) {
  std::lock_guard lock(mutex_);
  auto device = site_->find_device(device_id);
  if (!device) {
    OPENEMS_LOG_W("RTDataManager", "Device not found: " + device_id);
    return;
  }
  auto point = device->find_point(point_id);
  if (!point) {
    OPENEMS_LOG_W("RTDataManager", "Point not found: " + point_id);
    return;
  }
  point->set_value(value);
  if (change_cb_) {
    change_cb_(point_id, value);
  }
}

model::PointPtr RealtimeDataManager::find_point(
    const common::DeviceId& device_id,
    const common::PointId& point_id) const {
  std::lock_guard lock(mutex_);
  auto device = site_->find_device(device_id);
  if (!device) return nullptr;
  return device->find_point(point_id);
}

PointSnapshot RealtimeDataManager::get_point_snapshot(
    const common::DeviceId& device_id,
    const common::PointId& point_id) const {
  std::lock_guard lock(mutex_);
  auto point = find_point(device_id, point_id);
  if (!point) {
    return PointSnapshot{point_id, "", "", model::PointValue{}};
  }
  return PointSnapshot{
    point->id(), point->name(), point->unit(), point->get_value()
  };
}

DeviceSnapshot RealtimeDataManager::get_device_snapshot(
    const common::DeviceId& device_id) const {
  std::lock_guard lock(mutex_);
  auto device = site_->find_device(device_id);
  if (!device) {
    return DeviceSnapshot{device_id, "", common::DeviceStatus::Offline, {}};
  }
  DeviceSnapshot snap;
  snap.device_id = device->id();
  snap.device_name = device->name();
  snap.status = device->status();
  for (auto& pt : device->points()) {
    snap.points.push_back(PointSnapshot{
      pt->id(), pt->name(), pt->unit(), pt->get_value()
    });
  }
  return snap;
}

SiteSnapshot RealtimeDataManager::get_site_snapshot() const {
  std::lock_guard lock(mutex_);
  SiteSnapshot snap;
  snap.site_id = site_->id();
  snap.site_name = site_->name();
  for (auto& dev : site_->devices()) {
    auto ds = get_device_snapshot(dev->id());
    snap.devices.push_back(std::move(ds));
  }
  return snap;
}

void RealtimeDataManager::set_point_change_callback(PointChangeCallback cb) {
  std::lock_guard lock(mutex_);
  change_cb_ = std::move(cb);
}

void RealtimeDataManager::print_snapshot() const {
  auto snap = get_site_snapshot();
  std::cout << snap.to_string() << std::endl;
}

} // namespace openems::rt_data