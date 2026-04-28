# src/services/history/CMakeLists.txt
add_executable(openems-history main.cpp)
target_link_libraries(openems-history PRIVATE
  openems_common
  openems_utils
  openems_rt_db
)
install(TARGETS openems-history RUNTIME DESTINATION bin)