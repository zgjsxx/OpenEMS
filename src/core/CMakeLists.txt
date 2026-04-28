add_library(openems_core STATIC
  src/point_handle.cpp
  src/command_handle.cpp
)

target_include_directories(openems_core PUBLIC include)
target_link_libraries(openems_core PUBLIC openems_common openems_rt_db)
