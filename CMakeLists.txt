# 顶层 CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(OpenEMS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/utf-8)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install path prefix" FORCE)
endif()

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 第三方依赖：nlohmann/json（单头文件）
add_subdirectory(third_party)

# 子模块
add_subdirectory(src/common)
add_subdirectory(src/utils)
add_subdirectory(src/model)
add_subdirectory(src/modbus)
add_subdirectory(src/config)
add_subdirectory(src/devices)
add_subdirectory(src/rt_data)
add_subdirectory(src/rt_db)
add_subdirectory(src/iec104)
add_subdirectory(src/collector)
add_subdirectory(src/apps)
