# src/iec104/CMakeLists.txt
add_library(openems_iec104
  src/iec104_asdu.cpp
  src/iec104_client.cpp
)

target_include_directories(openems_iec104 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(openems_iec104 PUBLIC openems_common openems_model openems_utils openems_rt_db)

if(WIN32)
  target_link_libraries(openems_iec104 PRIVATE ws2_32)
endif()