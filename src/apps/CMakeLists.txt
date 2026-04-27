# src/apps/CMakeLists.txt

# Process 1: Modbus Collector — creates RtDb, polls Modbus devices, writes to shared memory
add_executable(openems-rtdb-service
  src/rtdb_service_main.cpp
)
target_link_libraries(openems-rtdb-service PRIVATE
  openems_common
  openems_utils
  openems_config
  openems_rt_db
)

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
  openems_config
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

set(OPENEMS_APP_TARGETS
  openems-rtdb-service
  openems-modbus-collector
  openems-viewer
  openems-alarm
  openems-iec104-collector
  openems-history
)

install(TARGETS ${OPENEMS_APP_TARGETS}
  RUNTIME DESTINATION bin
)

if(WIN32)
  file(GLOB OPENEMS_LIBPQ_RUNTIME_FILES
    "${CMAKE_SOURCE_DIR}/third_party/postgresql/windows/x64/bin/*.dll"
  )
  if(OPENEMS_LIBPQ_RUNTIME_FILES)
    install(FILES ${OPENEMS_LIBPQ_RUNTIME_FILES}
      DESTINATION bin
    )
  endif()
else()
  file(GLOB OPENEMS_LIBPQ_RUNTIME_FILES
    "${CMAKE_SOURCE_DIR}/third_party/postgresql/linux/x64/lib/libpq.so*"
  )
  if(OPENEMS_LIBPQ_RUNTIME_FILES)
    install(FILES ${OPENEMS_LIBPQ_RUNTIME_FILES}
      DESTINATION lib
    )
  endif()
endif()

install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/
  DESTINATION config
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/web/
  DESTINATION web
  FILES_MATCHING
    PATTERN "*.py"
    PATTERN "*.html"
    PATTERN "*.css"
    PATTERN "*.js"
    PATTERN "*.sql"
    PATTERN "*.txt"
    PATTERN "*.md"
    PATTERN "*.ps1"
    PATTERN "*.sh"
    PATTERN "__pycache__" EXCLUDE
    PATTERN "install_scripts" EXCLUDE
    PATTERN "*_local.ps1" EXCLUDE
    PATTERN "init_admin_portal_db.ps1" EXCLUDE
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/web/install_scripts/
  DESTINATION .
  FILES_MATCHING
    PATTERN "*.ps1"
    PATTERN "*.sh"
)

install(CODE [[
  file(MAKE_DIRECTORY "${CMAKE_INSTALL_PREFIX}/runtime")
  file(MAKE_DIRECTORY "${CMAKE_INSTALL_PREFIX}/runtime/history")
  file(MAKE_DIRECTORY "${CMAKE_INSTALL_PREFIX}/runtime/config_backups")
]])

# Copy config files to build directory
file(COPY ${CMAKE_SOURCE_DIR}/config DESTINATION ${CMAKE_BINARY_DIR})
