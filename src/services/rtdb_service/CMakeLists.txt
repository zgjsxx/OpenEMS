# src/services/rtdb_service/CMakeLists.txt
add_executable(openems-rtdb-service main.cpp)
target_link_libraries(openems-rtdb-service PRIVATE
  openems_common
  openems_utils
  openems_config
  openems_rt_db
)
install(TARGETS openems-rtdb-service RUNTIME DESTINATION bin)