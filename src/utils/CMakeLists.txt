# src/utils/CMakeLists.txt
add_library(openems_utils
  src/logger.cpp
  src/time_utils.cpp
)

target_include_directories(openems_utils PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(openems_utils PUBLIC openems_common)