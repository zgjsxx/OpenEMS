// src/iec104/src/iec104_client.cpp
#include "openems/iec104/iec104_client.h"
#include "openems/utils/logger.h"
#include "openems/rt_db/rt_db.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketType = SOCKET;
  #define CLOSE_SOCKET closesocket
  #define SOCKET_INVALID INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using SocketType = int;
  #define CLOSE_SOCKET close
  #define SOCKET_INVALID (-1)
#endif

#include <cstring>
#include <chrono>

namespace openems::iec104 {

#ifdef _WIN32
bool Iec104Client::wsa_initialized_ = false;
void Iec104Client::ensure_wsa_init() {
  if (!wsa_initialized_) { WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa); wsa_initialized_=true; }
}
#endif

static uint64_t make_dispatch_key(uint8_t type_id, uint32_t ioa) {
  return (static_cast<uint64_t>(type_id) << 24) | ioa;
}

Iec104Client::Iec104Client(const Iec104Config& config) : config_(config) {
#ifdef _WIN32
  ensure_wsa_init();
#endif
}

Iec104Client::~Iec104Client() { stop(); disconnect(); }

common::VoidResult Iec104Client::do_connect() {
#ifdef _WIN32
  ensure_wsa_init();
#endif
  SocketType fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == SOCKET_INVALID) {
    return common::VoidResult::Err(common::ErrorCode::ConnectionFailed, "socket() failed");
  }

#ifdef _WIN32
  DWORD tv = static_cast<DWORD>(config_.timeout_ms);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
  struct timeval tv;
  tv.tv_sec = config_.timeout_ms / 1000;
  tv.tv_usec = (config_.timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(config_.port);
  inet_pton(AF_INET, config_.ip.c_str(), &addr.sin_addr);

  if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    CLOSE_SOCKET(fd);
    return common::VoidResult::Err(common::ErrorCode::ConnectionFailed,
        "connect() to " + config_.ip + ":" + std::to_string(config_.port) + " failed");
  }

  {
    std::lock_guard lock(socket_mutex_);
    socket_fd_ = static_cast<int>(fd);
  }
  connected_ = true;
  send_seq_ = 0;
  recv_seq_ = 0;

  OPENEMS_LOG_I("Iec104Client",
      "Connected to " + config_.ip + ":" + std::to_string(config_.port));

  // Send STARTDT_act
  send_frame(build_startdt_act());

  return common::VoidResult::Ok();
}

common::VoidResult Iec104Client::connect() { return do_connect(); }

void Iec104Client::disconnect() {
  std::lock_guard lock(socket_mutex_);
  if (socket_fd_ != -1) {
    CLOSE_SOCKET(static_cast<SocketType>(socket_fd_));
    socket_fd_ = -1;
  }
  connected_ = false;
  OPENEMS_LOG_I("Iec104Client", "Disconnected from " + config_.ip);
}

bool Iec104Client::is_connected() const { return connected_.load(); }

common::VoidResult Iec104Client::reconnect() {
  disconnect();
  for (uint32_t i = 0; i < config_.max_reconnect_attempts; ++i) {
    OPENEMS_LOG_W("Iec104Client",
        "Reconnect attempt " + std::to_string(i+1) + " to " + config_.ip);
    auto result = do_connect();
    if (result.is_ok()) {
      // Re-send STARTDT and interrogation on reconnect
      send_frame(build_startdt_act());
      send_frame(build_interrogation_cmd(send_seq_++, recv_seq_, config_.common_address));
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.reconnect_interval_ms));
  }
  return common::VoidResult::Err(common::ErrorCode::ConnectionFailed,
      "Max reconnect attempts for " + config_.ip);
}

void Iec104Client::set_asdu_callback(AsduCallback cb) { asdu_cb_ = std::move(cb); }

void Iec104Client::register_point(uint8_t type_id, uint32_t ioa,
                                   const common::PointId& point_id,
                                   common::PointCategory category,
                                   double scale) {
  point_dispatch_[make_dispatch_key(type_id, ioa)] =
      PointDispatch{point_id, category, scale};
}

void Iec104Client::start() {
  if (running_.load()) return;
  running_ = true;

  // Send total interrogation
  send_frame(build_interrogation_cmd(send_seq_++, recv_seq_, config_.common_address));

  receive_thread_ = std::thread(&Iec104Client::receive_thread_func, this);
  heartbeat_thread_ = std::thread(&Iec104Client::heartbeat_thread_func, this);
  interrogation_thread_ = std::thread(&Iec104Client::interrogation_thread_func, this);

  OPENEMS_LOG_I("Iec104Client", "Started: receive + heartbeat + interrogation threads");
}

void Iec104Client::stop() {
  if (!running_.load()) return;
  running_ = false;
  if (receive_thread_.joinable()) receive_thread_.join();
  if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
  if (interrogation_thread_.joinable()) interrogation_thread_.join();
  OPENEMS_LOG_I("Iec104Client", "Stopped");
}

void Iec104Client::send_frame(const std::vector<uint8_t>& frame) {
  std::lock_guard lock(socket_mutex_);
  if (socket_fd_ == -1) return;
  ::send(static_cast<SocketType>(socket_fd_),
      reinterpret_cast<const char*>(frame.data()),
      static_cast<int>(frame.size()), 0);
}

common::Result<std::vector<uint8_t>> Iec104Client::receive_apdu() {
  std::lock_guard lock(socket_mutex_);
  if (socket_fd_ == -1) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ConnectionFailed, "Not connected");
  }

  // Read start byte (0x68) and length
  uint8_t header[2];
  int r1 = ::recv(static_cast<SocketType>(socket_fd_),
      reinterpret_cast<char*>(header), 2, MSG_WAITALL);
  if (r1 != 2) {
    connected_ = false;
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusTimeout, "recv header failed");
  }
  if (header[0] != 0x68) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusInvalidFrame, "Invalid start byte");
  }

  uint8_t apdu_len = header[1];
  if (apdu_len < 4) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusInvalidFrame, "APDU too short");
  }

  std::vector<uint8_t> apdu(apdu_len);
  int r2 = ::recv(static_cast<SocketType>(socket_fd_),
      reinterpret_cast<char*>(apdu.data()), apdu_len, MSG_WAITALL);
  if (r2 != apdu_len) {
    connected_ = false;
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusTimeout, "recv APDU body failed");
  }

  // Full frame: start + length + apdu body
  std::vector<uint8_t> frame;
  frame.push_back(header[0]);
  frame.push_back(header[1]);
  frame.insert(frame.end(), apdu.begin(), apdu.end());
  return common::Result<std::vector<uint8_t>>::Ok(std::move(frame));
}

void Iec104Client::handle_i_frame(const uint8_t* ctrl,
                                   const uint8_t* asdu_data, size_t asdu_len) {
  // Update receive sequence
  recv_seq_++;

  // Send S-frame acknowledgment
  send_frame(build_s_frame(recv_seq_));

  // Parse ASDU
  auto parsed = parse_asdu(asdu_data, asdu_len);
  if (!parsed) {
    OPENEMS_LOG_W("Iec104Client", "ASDU parse failed, len=" + std::to_string(asdu_len));
    return;
  }

  dispatch_asdu(*parsed);
}

void Iec104Client::handle_u_frame(FrameType type) {
  switch (type) {
    case FrameType::U_StartdtCon:
      OPENEMS_LOG_I("Iec104Client", "STARTDT confirmed");
      break;
    case FrameType::U_TestfrCon:
      OPENEMS_LOG_D("Iec104Client", "TESTFR confirmed");
      break;
    case FrameType::U_StartdtAct:
      send_frame(build_startdt_con());
      break;
    case FrameType::U_TestfrAct:
      send_frame(build_testfr_con());
      break;
    default:
      break;
  }
}

void Iec104Client::dispatch_asdu(const AsduData& data) {
  for (size_t i = 0; i < data.sequence_ioas.size(); ++i) {
    uint32_t ioa = data.sequence_ioas[i];
    uint8_t tid = static_cast<uint8_t>(data.type_id);

    auto key = make_dispatch_key(tid, ioa);
    auto it = point_dispatch_.find(key);
    if (it == point_dispatch_.end()) {
      continue;  // Unknown point, skip
    }

    auto& dispatch = it->second;
    double eng_value = 0.0;
    common::Quality quality = common::Quality::Good;

    if (i < data.sequence_data.size()) {
      auto& d = data.sequence_data[i];
      if (std::holds_alternative<SinglePointData>(d)) {
        eng_value = std::get<SinglePointData>(d).value ? 1.0 : 0.0;
      } else if (std::holds_alternative<DoublePointData>(d)) {
        eng_value = static_cast<double>(std::get<DoublePointData>(d).value);
      } else if (std::holds_alternative<NormalizedValueData>(d)) {
        eng_value = static_cast<double>(std::get<NormalizedValueData>(d).value) * dispatch.scale;
      } else if (std::holds_alternative<ScaledValueData>(d)) {
        eng_value = static_cast<double>(std::get<ScaledValueData>(d).raw) * dispatch.scale;
      } else if (std::holds_alternative<ShortFloatData>(d)) {
        eng_value = static_cast<double>(std::get<ShortFloatData>(d).value) * dispatch.scale;
      }
    }

    // Call external callback (which writes to RtDb)
    if (asdu_cb_) {
      // We pass the parsed data through callback
      // The callback knows how to write to RtDb based on category
      asdu_cb_(data);
    }

    OPENEMS_LOG_D("Iec104Client",
        "Dispatch: TID=" + std::to_string(tid) +
        " IOA=" + std::to_string(ioa) +
        " Point=" + dispatch.point_id +
        " Value=" + std::to_string(eng_value));
  }
}

void Iec104Client::receive_thread_func() {
  OPENEMS_LOG_I("Iec104Client", "Receive thread started");
  while (running_.load() && connected_.load()) {
    auto frame_result = receive_apdu();
    if (!frame_result.is_ok()) {
      OPENEMS_LOG_W("Iec104Client", "Receive failed: " + frame_result.error_msg());
      // Try reconnect
      auto rc = reconnect();
      if (!rc.is_ok()) {
        OPENEMS_LOG_E("Iec104Client", "Reconnect failed");
        break;
      }
      continue;
    }

    auto& frame = frame_result.value();
    if (frame.size() < 6) continue;  // 0x68 + len + 4-byte ctrl field

    // Control field starts at offset 2
    auto type = detect_frame_type(frame.data() + 2);

    if (type == FrameType::I) {
      // I-frame: ctrl(4 bytes) + ASDU
      size_t asdu_len = frame.size() - 6;  // minus start+len+ctrl
      handle_i_frame(frame.data() + 2, frame.data() + 6, asdu_len);
    } else {
      handle_u_frame(type);
    }
  }
  OPENEMS_LOG_I("Iec104Client", "Receive thread exiting");
}

void Iec104Client::heartbeat_thread_func() {
  OPENEMS_LOG_I("Iec104Client", "Heartbeat thread started");
  while (running_.load()) {
    if (connected_.load()) {
      send_frame(build_testfr_act());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.heartbeat_interval_ms));
  }
  OPENEMS_LOG_I("Iec104Client", "Heartbeat thread exiting");
}

void Iec104Client::interrogation_thread_func() {
  OPENEMS_LOG_I("Iec104Client", "Interrogation thread started");
  while (running_.load()) {
    if (connected_.load()) {
      send_frame(build_interrogation_cmd(send_seq_++, recv_seq_, config_.common_address));
      OPENEMS_LOG_D("Iec104Client", "Sent total interrogation");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.interrogation_interval_ms));
  }
  OPENEMS_LOG_I("Iec104Client", "Interrogation thread exiting");
}

} // namespace openems::iec104