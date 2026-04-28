# src/services/iec104_collector/CMakeLists.txt
add_executable(openems-iec104-collector main.cpp)
target_link_libraries(openems-iec104-collector PRIVATE
  openems_common
  openems_utils
  openems_model
  openems_iec104
  openems_config
  openems_rt_db
)
install(TARGETS openems-iec104-collector RUNTIME DESTINATION bin)