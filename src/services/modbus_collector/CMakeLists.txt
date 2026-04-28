# src/services/modbus_collector/CMakeLists.txt
add_executable(openems-modbus-collector main.cpp)
target_link_libraries(openems-modbus-collector PRIVATE
  openems_common
  openems_utils
  openems_model
  openems_modbus
  openems_config
  openems_rt_db
  openems_collector
)
install(TARGETS openems-modbus-collector RUNTIME DESTINATION bin)