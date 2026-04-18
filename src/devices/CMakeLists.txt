# src/devices/CMakeLists.txt
add_library(openems_devices INTERFACE)
target_include_directories(openems_devices INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)