# src/rt_db/CMakeLists.txt
add_library(openems_rt_db
  src/rt_db.cpp
)

target_include_directories(openems_rt_db PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(openems_rt_db PUBLIC openems_common openems_utils)