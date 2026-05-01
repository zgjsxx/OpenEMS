# src/rt_db/CMakeLists.txt
add_library(openems_rt_db
  src/rt_db.cpp
)

target_include_directories(openems_rt_db PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(openems_rt_db PUBLIC openems_common openems_utils)

if(BUILD_TESTING)
  add_executable(openems-rt-db-test
    tests/rt_db_table_space_test.cpp
  )

  target_link_libraries(openems-rt-db-test PRIVATE openems_rt_db)

  add_test(
    NAME openems-rt-db-test
    COMMAND $<TARGET_FILE:openems-rt-db-test>
  )
endif()
