# src/apps/CMakeLists.txt

# Process 1: Modbus Collector — creates RtDb, polls Modbus devices, writes to shared memory
add_executable(openems-modbus-collector
  src/collector_main.cpp
)
target_link_libraries(openems-modbus-collector PRIVATE
  openems_common
  openems_utils
  openems_model
  openems_modbus
  openems_config
  openems_rt_db
  openems_collector
)

# Process 2: Viewer — attaches to RtDb, prints real-time snapshot
add_executable(openems-viewer
  src/viewer_main.cpp
)
target_link_libraries(openems-viewer PRIVATE
  openems_common
  openems_utils
  openems_rt_db
)

# Process 3: Alarm — attaches to RtDb, checks thresholds and logs alarms
add_executable(openems-alarm
  src/alarm_main.cpp
)
target_link_libraries(openems-alarm PRIVATE
  openems_common
  openems_utils
  openems_rt_db
)

# Process 4: IEC104 Collector — connects IEC104 devices, receives ASDU, writes to RtDb
add_executable(openems-iec104-collector
  src/iec104_collector_main.cpp
)
target_link_libraries(openems-iec104-collector PRIVATE
  openems_common
  openems_utils
  openems_model
  openems_iec104
  openems_config
  openems_rt_db
)

# Process 5: History sampler - attaches to RtDb and writes daily JSONL history files
add_executable(openems-history
  src/history_main.cpp
)
target_link_libraries(openems-history PRIVATE
  openems_common
  openems_utils
  openems_rt_db
)

# Copy config files to build directory
file(COPY ${CMAKE_SOURCE_DIR}/config DESTINATION ${CMAKE_BINARY_DIR})
