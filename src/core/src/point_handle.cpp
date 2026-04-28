#include "openems/core/point_handle.h"

namespace openems::core {

PointHandle::PointHandle(common::PointId point_id, rt_db::RtDb* rtdb)
    : point_id_(std::move(point_id)), rtdb_(rtdb) {}

common::Result<rt_db::TelemetryReadResult> PointHandle::read_telemetry() {
  auto result = rtdb_->read_telemetry(point_id_);
  if (result.is_ok()) {
    auto& v = result.value();
    last_valid_ = v.valid;
    last_value_ = v.value;
    last_timestamp_ = v.timestamp;
    last_quality_ = v.quality;
  }
  return result;
}

common::Result<rt_db::TeleindicationReadResult> PointHandle::read_teleindication() {
  auto result = rtdb_->read_teleindication(point_id_);
  if (result.is_ok()) {
    auto& v = result.value();
    last_valid_ = v.valid;
    last_value_ = static_cast<double>(v.state_code);
    last_timestamp_ = v.timestamp;
    last_quality_ = v.quality;
  }
  return result;
}

common::Result<double> PointHandle::read_value() {
  auto telem = rtdb_->read_telemetry(point_id_);
  if (telem.is_ok()) {
    auto& v = telem.value();
    last_valid_ = v.valid;
    last_value_ = v.value;
    last_timestamp_ = v.timestamp;
    last_quality_ = v.quality;
    return v.value;
  }
  auto ti = rtdb_->read_teleindication(point_id_);
  if (ti.is_ok()) {
    auto& v = ti.value();
    last_valid_ = v.valid;
    last_value_ = static_cast<double>(v.state_code);
    last_timestamp_ = v.timestamp;
    last_quality_ = v.quality;
    return static_cast<double>(v.state_code);
  }
  return common::Result<double>::Err(common::ErrorCode::PointNotFound, ti.error_msg());
}

} // namespace openems::core
