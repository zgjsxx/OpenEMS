#pragma once

#include "openems/common/result.h"

#include <filesystem>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace openems::libpq {

struct LibpqApi {
#ifdef _WIN32
  HMODULE handle = nullptr;
#else
  void* handle = nullptr;
#endif
  void* (*PQconnectdb)(const char*) = nullptr;
  int (*PQstatus)(void*) = nullptr;
  char* (*PQerrorMessage)(void*) = nullptr;
  void* (*PQexec)(void*, const char*) = nullptr;
  void* (*PQexecParams)(void*, const char*, int, const unsigned int*, const char* const*, const int*, const int*, int) = nullptr;
  int (*PQresultStatus)(void*) = nullptr;
  int (*PQntuples)(void*) = nullptr;
  char* (*PQgetvalue)(void*, int, int) = nullptr;
  void (*PQclear)(void*) = nullptr;
  void (*PQfinish)(void*) = nullptr;

  LibpqApi() = default;
  LibpqApi(const LibpqApi&) = delete;
  LibpqApi& operator=(const LibpqApi&) = delete;
  LibpqApi(LibpqApi&& other) noexcept {
    *this = std::move(other);
  }
  LibpqApi& operator=(LibpqApi&& other) noexcept {
    if (this == &other) return *this;
    handle = other.handle;
    PQconnectdb = other.PQconnectdb;
    PQstatus = other.PQstatus;
    PQerrorMessage = other.PQerrorMessage;
    PQexec = other.PQexec;
    PQexecParams = other.PQexecParams;
    PQresultStatus = other.PQresultStatus;
    PQntuples = other.PQntuples;
    PQgetvalue = other.PQgetvalue;
    PQclear = other.PQclear;
    PQfinish = other.PQfinish;
    other.handle = nullptr;
    return *this;
  }

  ~LibpqApi() {
#ifdef _WIN32
    if (handle) FreeLibrary(handle);
#else
    if (handle) dlclose(handle);
#endif
  }
};

common::Result<LibpqApi> load_libpq();
std::vector<std::filesystem::path> libpq_search_dirs();

}  // namespace openems::libpq