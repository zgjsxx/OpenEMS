add_library(openems_strategy STATIC
  src/strategy_types.cpp
  src/strategy_base.cpp
  src/anti_reverse_flow.cpp
  src/soc_protection.cpp
)

target_include_directories(openems_strategy PUBLIC include)
target_link_libraries(openems_strategy PUBLIC openems_common openems_core openems_rt_db)
