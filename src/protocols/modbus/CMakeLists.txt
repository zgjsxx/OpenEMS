# src/modbus/CMakeLists.txt
add_library(openems_modbus
  src/modbus_tcp_client.cpp
  src/modbus_data_parser.cpp
  src/modbus_rtu_client.cpp
  src/serial_port.cpp
)

target_include_directories(openems_modbus PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(openems_modbus PUBLIC openems_common openems_model openems_utils)

# Windows 需要链接 ws2_32
if(WIN32)
  target_link_libraries(openems_modbus PRIVATE ws2_32)
endif()