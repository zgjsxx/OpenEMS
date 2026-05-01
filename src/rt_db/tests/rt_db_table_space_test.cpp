#include "openems/rt_db/rt_db.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace {

std::string make_test_shm_name() {
#ifdef _WIN32
  return "Local\\openems_rt_db_test_" + std::to_string(GetCurrentProcessId());
#else
  return "/openems_rt_db_test_" + std::to_string(getpid());
#endif
}

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[rt_db_test] " << message << std::endl;
    return false;
  }
  return true;
}

}  // namespace

int main() {
  using namespace openems;

  const std::string shm_name = make_test_shm_name();

  auto create_result = rt_db::RtDb::create(shm_name, 3, 1, 1);
  if (!create_result.is_ok()) {
    std::cerr << "[rt_db_test] create failed: " << create_result.error_msg() << std::endl;
    return 1;
  }
  std::unique_ptr<rt_db::RtDb> creator(create_result.value());

  creator->set_site_info("site-test-001", "Test Site");

  if (!expect(
          creator->register_point(
                     "pv-active-power",
                     "pv-001",
                     0,
                     static_cast<uint8_t>(common::DataType::Float32),
                     "kW",
                     false)
              .is_ok(),
          "register pv-active-power failed")) {
    return 1;
  }

  if (!expect(
          creator->register_point(
                     "bess-target-power",
                     "bess-001",
                     3,
                     static_cast<uint8_t>(common::DataType::Float32),
                     "kW",
                     true)
              .is_ok(),
          "register bess-target-power failed")) {
    return 1;
  }

  if (!expect(
          creator->register_point(
                     "bess-run-mode",
                     "bess-001",
                     1,
                     static_cast<uint8_t>(common::DataType::Uint16),
                     "",
                     false)
              .is_ok(),
          "register bess-run-mode failed")) {
    return 1;
  }

  if (!expect(creator->register_command_point("bess-target-power").is_ok(), "register command point failed")) {
    return 1;
  }

  auto tables = creator->list_tables();
  if (!expect(tables.size() == 7, "catalog table count should be 7")) {
    return 1;
  }

  auto point_index_info = creator->get_table_info("point_index");
  if (!expect(point_index_info.is_ok(), "point_index table not found")) {
    return 1;
  }
  if (!expect(point_index_info.value().row_count == 3, "point_index row_count should be 3")) {
    return 1;
  }

  auto command_info = creator->get_table_info(static_cast<uint16_t>(rt_db::TableCommand));
  if (!expect(command_info.is_ok(), "command table id not found")) {
    return 1;
  }
  if (!expect(command_info.value().row_count == 1, "command row_count should be 1")) {
    return 1;
  }

  auto strategy_view = creator->open_table("strategy_runtime");
  if (!expect(strategy_view.is_ok(), "open strategy_runtime failed")) {
    return 1;
  }
  auto alarm_view = creator->open_table(static_cast<uint16_t>(rt_db::TableAlarmActive));
  if (!expect(alarm_view.is_ok(), "open alarm_active failed")) {
    return 1;
  }

  creator->write_telemetry("pv-active-power", 36.5, common::Quality::Good, true);
  creator->write_telemetry("bess-target-power", -12.0, common::Quality::Good, true);
  creator->write_teleindication("bess-run-mode", 1, common::Quality::Good, true);

  auto attach_result = rt_db::RtDb::attach(shm_name);
  if (!attach_result.is_ok()) {
    std::cerr << "[rt_db_test] attach failed: " << attach_result.error_msg() << std::endl;
    return 1;
  }
  std::unique_ptr<rt_db::RtDb> reader(attach_result.value());

  auto telem = reader->read_telemetry("pv-active-power");
  if (!expect(telem.is_ok(), "read telemetry failed")) {
    return 1;
  }
  if (!expect(telem.value().valid, "telemetry should be valid")) {
    return 1;
  }
  if (!expect(telem.value().value == 36.5, "telemetry value mismatch")) {
    return 1;
  }

  auto setting = reader->read_telemetry("bess-target-power");
  if (!expect(setting.is_ok(), "read setting failed")) {
    return 1;
  }
  if (!expect(setting.value().value == -12.0, "setting value mismatch")) {
    return 1;
  }

  auto ti = reader->read_teleindication("bess-run-mode");
  if (!expect(ti.is_ok(), "read teleindication failed")) {
    return 1;
  }
  if (!expect(ti.value().state_code == 1, "teleindication state mismatch")) {
    return 1;
  }

  if (!expect(reader->submit_command("bess-target-power", -15.5).is_ok(), "submit command failed")) {
    return 1;
  }

  common::PointId pending_pid;
  double pending_value = 0.0;
  if (!expect(creator->read_pending_command(pending_pid, pending_value), "pending command not found")) {
    return 1;
  }
  if (!expect(pending_pid == "bess-target-power", "pending command point mismatch")) {
    return 1;
  }
  if (!expect(pending_value == -15.5, "pending command value mismatch")) {
    return 1;
  }

  creator->complete_command("bess-target-power", rt_db::CommandSuccess, -15.4, 0);
  auto command_status = reader->read_command_status("bess-target-power");
  if (!expect(command_status.is_ok(), "read command status failed")) {
    return 1;
  }
  if (!expect(command_status.value().status == rt_db::CommandSuccess, "command status mismatch")) {
    return 1;
  }
  if (!expect(command_status.value().result_value == -15.4, "command result value mismatch")) {
    return 1;
  }

  if (!expect(
          creator->upsert_strategy_runtime(
                     {"anti-reverse-flow", "bess-target-power", -12.0, false, "", "", 123456789})
              .is_ok(),
          "upsert strategy runtime failed")) {
    return 1;
  }
  auto strategy_rows = reader->read_strategy_runtime();
  if (!expect(strategy_rows.size() == 1, "strategy runtime row count mismatch")) {
    return 1;
  }
  if (!expect(strategy_rows[0].target_point_id == "bess-target-power",
              "strategy runtime target point mismatch")) {
    return 1;
  }

  std::vector<rt_db::AlarmActiveRecord> alarms{
      {"pv-active-power-high", "pv-active-power", "pv-001", "warning",
       "PV active power high", 36.5, "kW", 1000, 2000, true}};
  if (!expect(creator->replace_active_alarms(alarms).is_ok(), "replace active alarms failed")) {
    return 1;
  }
  auto active_alarms = reader->read_active_alarms();
  if (!expect(active_alarms.size() == 1, "active alarm row count mismatch")) {
    return 1;
  }
  if (!expect(active_alarms[0].alarm_id == "pv-active-power-high",
              "active alarm id mismatch")) {
    return 1;
  }

  auto snapshot = reader->snapshot();
  if (!expect(snapshot.site_id == "site-test-001", "snapshot site id mismatch")) {
    return 1;
  }
  if (!expect(snapshot.site_name == "Test Site", "snapshot site name mismatch")) {
    return 1;
  }
  if (!expect(snapshot.point_ids.size() == 3, "snapshot point count mismatch")) {
    return 1;
  }

  std::cout << "[rt_db_test] ok" << std::endl;
  return 0;
}
