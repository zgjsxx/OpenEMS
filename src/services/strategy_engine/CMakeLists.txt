add_executable(openems-strategy-engine
  main.cpp
)

target_link_libraries(openems-strategy-engine PRIVATE
  openems_common
  openems_utils
  openems_core
  openems_strategy
  openems_rt_db
  openems_libpq
)

install(TARGETS openems-strategy-engine DESTINATION bin)
