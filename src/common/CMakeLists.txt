# src/common/CMakeLists.txt
add_library(openems_common
  src/error_code.cpp
)

target_include_directories(openems_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)