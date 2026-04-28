#pragma once

#include <string>
#include <cstdint>

namespace openems::iec104 {

struct Iec104Config {
  std::string ip;
  uint16_t port = 2404;
  uint16_t common_address = 1;
  uint32_t timeout_ms = 3000;
  uint32_t reconnect_interval_ms = 5000;
  uint32_t max_reconnect_attempts = 3;
  uint32_t heartbeat_interval_ms = 30000;  // TESTFR interval
  uint32_t interrogation_interval_ms = 60000;  // total interrogation interval
};

} // namespace openems::iec104