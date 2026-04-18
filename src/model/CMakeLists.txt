# src/model/CMakeLists.txt
add_library(openems_model
  src/modbus_point_mapping.cpp
  src/iec104_point_mapping.cpp
  src/point_value.cpp
  src/point.cpp
  src/device.cpp
  src/site.cpp
)

target_include_directories(openems_model PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(openems_model PUBLIC openems_common openems_utils)