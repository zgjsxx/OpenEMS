# src/config/CMakeLists.txt
add_library(openems_config
  src/config_loader.cpp
  src/csv_parser.cpp
)

target_include_directories(openems_config PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(openems_config PUBLIC
  openems_common
  openems_model
  openems_utils
)