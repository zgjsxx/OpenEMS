# src/libpq/CMakeLists.txt
add_library(openems_libpq src/libpq_api.cpp)
target_include_directories(openems_libpq PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(openems_libpq PUBLIC openems_common ${CMAKE_DL_LIBS})