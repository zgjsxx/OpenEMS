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
add_subdirectory(src)

# 项目级 install 规则

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
