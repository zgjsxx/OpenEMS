#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>
#include <unordered_map>
#include "openems/common/result.h"
#include "openems/common/types.h"
#include "openems/iec104/iec104_config.h"
#include "openems/iec104/iec104_asdu.h"
#include "openems/model/iec104_point_mapping.h"

namespace openems::iec104 {

// Decoded point update produced from a registered IEC104 mapping.

struct PointUpdate {
  common::PointId point_id;
  common::PointCategory category;
  double value = 0.0;
  common::Quality quality = common::Quality::Good;
  bool valid = false;
};

// Callback when a registered point is decoded from an ASDU.
using AsduCallback = std::function<void(const PointUpdate&)>;

class Iec104Client {
public:
  explicit Iec104Client(const Iec104Config& config);
  ~Iec104Client();

  common::VoidResult connect();
  void disconnect();
  bool is_connected() const;
  common::VoidResult reconnect();

  void set_asdu_callback(AsduCallback cb);

  // Register a point mapping so received ASDU data can be dispatched
  void register_point(uint8_t type_id, uint32_t ioa,
                      const common::PointId& point_id,
                      common::PointCategory category,
                      double scale);

  // Start background threads (receive, heartbeat, interrogation)
  void start();
  void stop();

private:
  void receive_thread_func();
  void heartbeat_thread_func();
  void interrogation_thread_func();

  common::VoidResult do_connect();
  common::Result<std::vector<uint8_t>> receive_apdu();
  void send_frame(const std::vector<uint8_t>& frame);
  void handle_i_frame(const uint8_t* ctrl, const uint8_t* asdu_data, size_t asdu_len);
  void handle_u_frame(FrameType type);

  // Dispatch parsed ASDU data to registered points
  void dispatch_asdu(const AsduData& data);

  Iec104Config config_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> running_{false};
  int socket_fd_ = -1;
  mutable std::mutex socket_mutex_;

  // Sequence counters
  uint16_t send_seq_ = 0;
  uint16_t recv_seq_ = 0;

  // Background threads
  std::thread receive_thread_;
  std::thread heartbeat_thread_;
  std::thread interrogation_thread_;

  // Point dispatch: map (type_id, ioa) → point info
  struct PointDispatch {
    common::PointId point_id;
    common::PointCategory category;
    double scale;
  };
  std::unordered_map<uint64_t, PointDispatch> point_dispatch_;
  // Key = (type_id << 24) | ioa

  AsduCallback asdu_cb_;

#ifdef _WIN32
  // WSA init tracking
  static bool wsa_initialized_;
  static void ensure_wsa_init();
#endif
};

using Iec104ClientPtr = std::shared_ptr<Iec104Client>;

inline Iec104ClientPtr Iec104ClientCreate(const Iec104Config& config) {
  return std::make_shared<Iec104Client>(config);
}

} // namespace openems::iec104
