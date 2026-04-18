// src/utils/include/openems/utils/logger.h
#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <functional>
#include <cstdint>

namespace openems::utils {

enum class LogLevel : uint8_t {
  Trace = 0,
  Debug = 1,
  Info  = 2,
  Warn  = 3,
  Error = 4,
  Fatal = 5,
};

class Logger {
public:
  using Sink = std::function<void(LogLevel, const std::string&, const std::string&)>;

  static Logger& instance();

  void set_level(LogLevel level);
  void add_sink(Sink sink);
  void log(LogLevel level, const std::string& tag, const std::string& msg);

private:
  Logger();
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;
  Logger(Logger&&) = delete;
  Logger& operator=(Logger&&) = delete;

  LogLevel level_ = LogLevel::Info;
  std::vector<Sink> sinks_;
  std::mutex mutex_;
};

#define OPENEMS_LOG(level, tag, msg) \
  openems::utils::Logger::instance().log(openems::utils::LogLevel::level, tag, msg)

#define OPENEMS_LOG_T(tag, msg) OPENEMS_LOG(Trace, tag, msg)
#define OPENEMS_LOG_D(tag, msg) OPENEMS_LOG(Debug, tag, msg)
#define OPENEMS_LOG_I(tag, msg) OPENEMS_LOG(Info,  tag, msg)
#define OPENEMS_LOG_W(tag, msg) OPENEMS_LOG(Warn,  tag, msg)
#define OPENEMS_LOG_E(tag, msg) OPENEMS_LOG(Error, tag, msg)
#define OPENEMS_LOG_F(tag, msg) OPENEMS_LOG(Fatal, tag, msg)

} // namespace openems::utils