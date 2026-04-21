# src/collector/CMakeLists.txt
add_library(openems_collector
  src/polling_service.cpp
  src/control_service.cpp
)

target_include_directories(openems_collector PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(openems_collector PUBLIC
  openems_common
  openems_model
  openems_modbus
  openems_rt_db
  openems_utils
)