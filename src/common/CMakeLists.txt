# src/common/CMakeLists.txt
add_library(openems_common
  src/error_code.cpp
  src/string_utils.cpp
  src/byte_utils.cpp
  src/thread_pool.cpp
  src/time_utils.cpp
  src/crc.cpp
  src/service_utils.cpp
)

target_include_directories(openems_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)