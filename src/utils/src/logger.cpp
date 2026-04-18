// src/utils/src/logger.cpp
#include "openems/utils/logger.h"
#include <iostream>
#include <cstdio>

namespace openems::utils {

static std::string level_to_string(LogLevel level) {
  switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    default:              return "UNKNOWN";
  }
}

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

Logger::Logger() {
  // 默认终端输出 sink
  add_sink([](LogLevel level, const std::string& tag, const std::string& msg) {
    std::printf("[%s] [%s] %s\n",
        level_to_string(level).c_str(), tag.c_str(), msg.c_str());
  });
}

void Logger::set_level(LogLevel level) {
  std::lock_guard lock(mutex_);
  level_ = level;
}

void Logger::add_sink(Sink sink) {
  std::lock_guard lock(mutex_);
  sinks_.push_back(std::move(sink));
}

void Logger::log(LogLevel level, const std::string& tag, const std::string& msg) {
  std::lock_guard lock(mutex_);
  if (level < level_) return;
  for (auto& sink : sinks_) {
    sink(level, tag, msg);
  }
}

} // namespace openems::utils