# src/rt_data/CMakeLists.txt
add_library(openems_rt_data
  src/realtime_data_manager.cpp
)

target_include_directories(openems_rt_data PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(openems_rt_data PUBLIC openems_common openems_model openems_utils)