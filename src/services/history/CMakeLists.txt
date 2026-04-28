# src/services/history/CMakeLists.txt
add_executable(openems-history main.cpp history_writer.cpp)
target_link_libraries(openems-history PRIVATE
  openems_common
  openems_utils
  openems_rt_db
  openems_libpq
)
install(TARGETS openems-history RUNTIME DESTINATION bin)