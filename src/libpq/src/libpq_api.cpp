// src/libpq/src/libpq_api.cpp
#include "openems/libpq/libpq_api.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <dlfcn.h>
  #include <limits.h>
  #include <unistd.h>
#endif

namespace openems::libpq {

namespace {

static void* load_symbol(LibpqApi& api, const char* name) {
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(api.handle, name));
#else
  return dlsym(api.handle, name);
#endif
}

template <typename T>
static bool bind_symbol(LibpqApi& api, T& target, const char* name) {
  target = reinterpret_cast<T>(load_symbol(api, name));
  return target != nullptr;
}

static std::filesystem::path current_working_dir() {
  std::error_code ec;
  auto path = std::filesystem::current_path(ec);
  return ec ? std::filesystem::path{} : path;
}

static std::filesystem::path executable_dir() {
#ifdef _WIN32
  char buffer[MAX_PATH] = {};
  const DWORD size = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (size == 0 || size >= MAX_PATH) return {};
  return std::filesystem::path(buffer).parent_path();
#else
  char buffer[PATH_MAX] = {};
  const ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (size <= 0) return {};
  buffer[size] = '\0';
  return std::filesystem::path(buffer).parent_path();
#endif
}

static void add_unique_path(std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
  if (path.empty()) return;
  std::error_code ec;
  auto normalized = std::filesystem::weakly_canonical(path, ec);
  if (ec) normalized = path.lexically_normal();
  for (const auto& existing : paths) {
    if (existing == normalized) return;
  }
  paths.push_back(normalized);
}

#ifdef _WIN32
static HMODULE try_load_libpq_from_dir(const std::filesystem::path& dir) {
  const auto dll_path = dir / "libpq.dll";
  if (!std::filesystem::exists(dll_path)) return nullptr;

  const auto old_dir_size = GetDllDirectoryA(0, nullptr);
  std::string old_dir;
  if (old_dir_size > 0) {
    old_dir.resize(old_dir_size);
    const auto copied = GetDllDirectoryA(old_dir_size, old_dir.data());
    old_dir.resize(copied);
  }

  const auto dir_text = dir.string();
  SetDllDirectoryA(dir_text.c_str());
  HMODULE handle = LoadLibraryA(dll_path.string().c_str());
  SetDllDirectoryA(old_dir.empty() ? nullptr : old_dir.c_str());
  return handle;
}
#else
static void* try_load_libpq_from_dir(const std::filesystem::path& dir) {
  const std::vector<std::string> names = {"libpq.so.5", "libpq.so"};
  for (const auto& name : names) {
    const auto so_path = dir / name;
    if (!std::filesystem::exists(so_path)) continue;
    if (auto* handle = dlopen(so_path.string().c_str(), RTLD_NOW)) return handle;
  }
  return nullptr;
}
#endif

}  // namespace

std::vector<std::filesystem::path> libpq_search_dirs() {
  std::vector<std::filesystem::path> dirs;
  const auto exe_dir = executable_dir();
  const auto cwd = current_working_dir();

  add_unique_path(dirs, exe_dir);
  add_unique_path(dirs, exe_dir.parent_path() / "lib");
  add_unique_path(dirs, cwd / "bin");
  add_unique_path(dirs, cwd / "lib");

#ifdef _WIN32
  const auto third_party_libpq = std::filesystem::path("third_party") / "postgresql" / "windows" / "x64" / "bin";
#else
  const auto third_party_libpq = std::filesystem::path("third_party") / "postgresql" / "linux" / "x64" / "lib";
#endif

  add_unique_path(dirs, cwd / third_party_libpq);
  add_unique_path(dirs, exe_dir.parent_path().parent_path() / third_party_libpq);
  add_unique_path(dirs, exe_dir.parent_path() / third_party_libpq);
  return dirs;
}

common::Result<LibpqApi> load_libpq() {
  LibpqApi api;
  for (const auto& dir : libpq_search_dirs()) {
    api.handle = try_load_libpq_from_dir(dir);
    if (api.handle) break;
  }

#ifdef _WIN32
  if (!api.handle) api.handle = LoadLibraryA("libpq.dll");
#else
  if (!api.handle) api.handle = dlopen("libpq.so.5", RTLD_NOW);
  if (!api.handle) api.handle = dlopen("libpq.so", RTLD_NOW);
#endif
  if (!api.handle) {
    return common::Result<LibpqApi>::Err(
        common::ErrorCode::ConnectionFailed,
        "libpq runtime library not found in install/bin, install/lib, third_party/postgresql, or system library paths");
  }

  bool ok = true;
  ok = ok && bind_symbol(api, api.PQconnectdb, "PQconnectdb");
  ok = ok && bind_symbol(api, api.PQstatus, "PQstatus");
  ok = ok && bind_symbol(api, api.PQerrorMessage, "PQerrorMessage");
  ok = ok && bind_symbol(api, api.PQexec, "PQexec");
  ok = ok && bind_symbol(api, api.PQresultStatus, "PQresultStatus");
  ok = ok && bind_symbol(api, api.PQntuples, "PQntuples");
  ok = ok && bind_symbol(api, api.PQgetvalue, "PQgetvalue");
  ok = ok && bind_symbol(api, api.PQclear, "PQclear");
  ok = ok && bind_symbol(api, api.PQfinish, "PQfinish");
  if (!ok) {
    return common::Result<LibpqApi>::Err(common::ErrorCode::ConnectionFailed, "libpq runtime library is missing required symbols");
  }

  // PQexecParams is optional — not all callers need it
  bind_symbol(api, api.PQexecParams, "PQexecParams");

  return common::Result<LibpqApi>::Ok(std::move(api));
}

}  // namespace openems::libpq