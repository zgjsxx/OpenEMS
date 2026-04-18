// src/rt_data/include/openems/rt_data/realtime_data_manager.h
#pragma once

#include <unordered_map>
#include <vector>
#include <mutex>
#include <memory>
#include <string>
#include <functional>
#include "openems/common/types.h"
#include "openems/model/point.h"
#include "openems/model/device.h"
#include "openems/model/site.h"

namespace openems::rt_data {

// 单个点位实时快照
struct PointSnapshot {
  common::PointId point_id;
  std::string point_name;
  std::string unit;
  model::PointValue value;
  std::string to_string() const;
};

// 单个设备实时快照
struct DeviceSnapshot {
  common::DeviceId device_id;
  std::string device_name;
  common::DeviceStatus status;
  std::vector<PointSnapshot> points;
  std::string to_string() const;
};

// 站点实时快照
struct SiteSnapshot {
  common::SiteId site_id;
  std::string site_name;
  std::vector<DeviceSnapshot> devices;
  std::string to_string() const;
};

// 数据变更回调
using PointChangeCallback = std::function<void(const common::PointId&, const model::PointValue&)>;

class RealtimeDataManager {
public:
  explicit RealtimeDataManager(model::SitePtr site);

  // 更新点位值
  void update_point(const common::DeviceId& device_id,
                    const common::PointId& point_id,
                    const model::PointValue& value);

  // 查询
  model::PointPtr find_point(const common::DeviceId& device_id,
                             const common::PointId& point_id) const;

  PointSnapshot get_point_snapshot(const common::DeviceId& device_id,
                                   const common::PointId& point_id) const;

  DeviceSnapshot get_device_snapshot(const common::DeviceId& device_id) const;
  SiteSnapshot get_site_snapshot() const;

  // 设置点位值变更回调
  void set_point_change_callback(PointChangeCallback cb);

  // 打印站点实时数据快照
  void print_snapshot() const;

private:
  model::SitePtr site_;
  mutable std::mutex mutex_;
  PointChangeCallback change_cb_;
};

using RealtimeDataManagerPtr = std::shared_ptr<RealtimeDataManager>;

inline RealtimeDataManagerPtr RealtimeDataManagerCreate(model::SitePtr site) {
  return std::make_shared<RealtimeDataManager>(std::move(site));
}

} // namespace openems::rt_data