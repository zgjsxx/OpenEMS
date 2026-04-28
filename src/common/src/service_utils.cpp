// src/common/src/service_utils.cpp
#include "openems/common/service_utils.h"

#include <csignal>
#include <atomic>

namespace openems::common {

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
  g_running = false;
}

void run_until_signal(std::function<void()> fn) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
  fn();
}

} // namespace openems::common