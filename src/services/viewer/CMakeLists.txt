# src/services/viewer/CMakeLists.txt
add_executable(openems-viewer main.cpp)
target_link_libraries(openems-viewer PRIVATE
  openems_common
  openems_utils
  openems_rt_db
)
install(TARGETS openems-viewer RUNTIME DESTINATION bin)