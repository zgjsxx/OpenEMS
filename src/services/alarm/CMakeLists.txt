# src/services/alarm/CMakeLists.txt
add_executable(openems-alarm main.cpp)
target_link_libraries(openems-alarm PRIVATE
  openems_common
  openems_utils
  openems_config
  openems_rt_db
)
install(TARGETS openems-alarm RUNTIME DESTINATION bin)