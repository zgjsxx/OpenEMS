// src/modbus/src/modbus_tcp_client.cpp
#include "openems/modbus/modbus_tcp_client.h"
#include "openems/utils/logger.h"
#include "openems/utils/time_utils.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using SocketType = SOCKET;
  #define CLOSE_SOCKET closesocket
  #define SOCKET_INVALID INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  using SocketType = int;
  #define CLOSE_SOCKET close
  #define SOCKET_INVALID (-1)
#endif

#include <cstring>
#include <chrono>

namespace openems::modbus {

// ===== Modbus ADU/TDU 构造与解析 =====

static void put_uint16_be(std::vector<uint8_t>& buf, uint16_t val) {
  buf.push_back(static_cast<uint8_t>(val >> 8));
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

static uint16_t get_uint16_be(const uint8_t* p) {
  return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

// 构造 Modbus TCP 请求 ADU
static std::vector<uint8_t> build_request(
    uint16_t trans_id, uint8_t unit_id, uint8_t func_code,
    uint16_t address, uint16_t count) {
  std::vector<uint8_t> req;
  put_uint16_be(req, trans_id);       // Transaction ID
  put_uint16_be(req, 0x0000);         // Protocol ID (Modbus)
  // Length placeholder: will fill after
  size_t length_pos = req.size();
  put_uint16_be(req, 0);              // Length (placeholder)
  req.push_back(unit_id);             // Unit ID
  req.push_back(func_code);           // Function Code
  put_uint16_be(req, address);        // Start Address
  put_uint16_be(req, count);          // Quantity
  // Update length field (unit_id + func_code + address + count = 6 bytes)
  uint16_t length = static_cast<uint16_t>(req.size() - 6);
  req[length_pos] = static_cast<uint8_t>(length >> 8);
  req[length_pos + 1] = static_cast<uint8_t>(length & 0xFF);
  return req;
}

static std::vector<uint8_t> build_write_single_reg_request(
    uint16_t trans_id, uint8_t unit_id,
    uint16_t address, uint16_t value) {
  std::vector<uint8_t> req;
  put_uint16_be(req, trans_id);
  put_uint16_be(req, 0x0000);
  size_t length_pos = req.size();
  put_uint16_be(req, 0);
  req.push_back(unit_id);
  req.push_back(0x06);  // Write Single Register
  put_uint16_be(req, address);
  put_uint16_be(req, value);
  uint16_t length = static_cast<uint16_t>(req.size() - 6);
  req[length_pos] = static_cast<uint8_t>(length >> 8);
  req[length_pos + 1] = static_cast<uint8_t>(length & 0xFF);
  return req;
}

static std::vector<uint8_t> build_write_single_coil_request(
    uint16_t trans_id, uint8_t unit_id,
    uint16_t address, bool value) {
  std::vector<uint8_t> req;
  put_uint16_be(req, trans_id);
  put_uint16_be(req, 0x0000);
  size_t length_pos = req.size();
  put_uint16_be(req, 0);
  req.push_back(unit_id);
  req.push_back(0x05);  // Write Single Coil
  put_uint16_be(req, address);
  put_uint16_be(req, value ? 0xFF00 : 0x0000);
  uint16_t length = static_cast<uint16_t>(req.size() - 6);
  req[length_pos] = static_cast<uint8_t>(length >> 8);
  req[length_pos + 1] = static_cast<uint8_t>(length & 0xFF);
  return req;
}

static std::vector<uint8_t> build_write_multi_reg_request(
    uint16_t trans_id, uint8_t unit_id,
    uint16_t address, const std::vector<uint16_t>& values) {
  std::vector<uint8_t> req;
  put_uint16_be(req, trans_id);
  put_uint16_be(req, 0x0000);
  size_t length_pos = req.size();
  put_uint16_be(req, 0);
  req.push_back(unit_id);
  req.push_back(0x10);  // Write Multiple Registers
  put_uint16_be(req, address);
  put_uint16_be(req, static_cast<uint16_t>(values.size()));
  req.push_back(static_cast<uint8_t>(values.size() * 2));  // Byte count
  for (auto v : values) {
    put_uint16_be(req, v);
  }
  uint16_t length = static_cast<uint16_t>(req.size() - 6);
  req[length_pos] = static_cast<uint8_t>(length >> 8);
  req[length_pos + 1] = static_cast<uint8_t>(length & 0xFF);
  return req;
}

// ===== Socket 初始化（Windows 需要 WSAStartup）=====

static bool wsa_initialized = false;
static void ensure_wsa_init() {
#ifdef _WIN32
  if (!wsa_initialized) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    wsa_initialized = true;
  }
#endif
}

// ===== ModbusTcpClient 实现 =====

ModbusTcpClient::ModbusTcpClient(const ModbusConfig& config)
    : config_(config) {
  ensure_wsa_init();
}

ModbusTcpClient::~ModbusTcpClient() {
  disconnect();
#ifdef _WIN32
  if (wsa_initialized) {
    WSACleanup();
    wsa_initialized = false;
  }
#endif
}

common::VoidResult ModbusTcpClient::do_connect() {
  ensure_wsa_init();

  SocketType fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == SOCKET_INVALID) {
    return common::VoidResult::Err(
        common::ErrorCode::ModbusConnectionFailed, "socket() failed");
  }

  // 设置超时
#ifdef _WIN32
  DWORD tv = static_cast<DWORD>(config_.timeout_ms);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
  struct timeval tv;
  tv.tv_sec = config_.timeout_ms / 1000;
  tv.tv_usec = (config_.timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(config_.port);
  inet_pton(AF_INET, config_.ip.c_str(), &addr.sin_addr);

  if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    CLOSE_SOCKET(fd);
    return common::VoidResult::Err(
        common::ErrorCode::ModbusConnectionFailed,
        "connect() to " + config_.ip + ":" + std::to_string(config_.port) + " failed");
  }

  {
    std::lock_guard lock(socket_mutex_);
    socket_fd_ = static_cast<int>(fd);
  }
  connected_ = true;

  OPENEMS_LOG_I("ModbusTcpClient",
      "Connected to " + config_.ip + ":" + std::to_string(config_.port));

  if (conn_cb_) conn_cb_(true, config_.ip, config_.port);
  return common::VoidResult::Ok();
}

common::VoidResult ModbusTcpClient::connect() {
  return do_connect();
}

void ModbusTcpClient::disconnect() {
  std::lock_guard lock(socket_mutex_);
  if (socket_fd_ != -1) {
    CLOSE_SOCKET(static_cast<SocketType>(socket_fd_));
    socket_fd_ = -1;
  }
  connected_ = false;
  OPENEMS_LOG_I("ModbusTcpClient", "Disconnected from " + config_.ip);
  if (conn_cb_) conn_cb_(false, config_.ip, config_.port);
}

bool ModbusTcpClient::is_connected() const {
  return connected_.load();
}

common::VoidResult ModbusTcpClient::reconnect() {
  disconnect();
  for (uint32_t i = 0; i < config_.max_reconnect_attempts; ++i) {
    OPENEMS_LOG_W("ModbusTcpClient",
        "Reconnect attempt " + std::to_string(i + 1) +
        "/" + std::to_string(config_.max_reconnect_attempts) +
        " to " + config_.ip);
    auto result = do_connect();
    if (result.is_ok()) return result;
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.reconnect_interval_ms));
  }
  return common::VoidResult::Err(
      common::ErrorCode::ModbusConnectionFailed,
      "Max reconnect attempts reached for " + config_.ip);
}

void ModbusTcpClient::set_connection_callback(ConnectionCallback cb) {
  conn_cb_ = std::move(cb);
}

// ===== 发送请求并接收响应 =====

common::Result<std::vector<uint8_t>> ModbusTcpClient::send_and_receive(
    const std::vector<uint8_t>& request) {
  std::lock_guard lock(socket_mutex_);
  if (socket_fd_ == -1) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusConnectionFailed, "Not connected");
  }

  // 发送
  int sent = ::send(static_cast<SocketType>(socket_fd_),
      reinterpret_cast<const char*>(request.data()),
      static_cast<int>(request.size()), 0);
  if (sent <= 0) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusError, "send() failed");
  }

  // 接收 Modbus TCP ADU header (7 bytes: trans_id[2] + proto_id[2] + length[2] + unit_id[1])
  std::vector<uint8_t> header(7);
  int recv_header = ::recv(static_cast<SocketType>(socket_fd_),
      reinterpret_cast<char*>(header.data()), 7, MSG_WAITALL);
  if (recv_header != 7) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusTimeout, "recv header failed, got " +
        std::to_string(recv_header) + " bytes");
  }

  uint16_t resp_length = get_uint16_be(header.data() + 4);
  if (resp_length < 1) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusInvalidFrame, "Response length too short");
  }

  // 接收 PDU（length - 1 已包含 unit_id，所以剩余 = length - 1）
  uint16_t pdu_remaining = resp_length - 1;
  std::vector<uint8_t> pdu(pdu_remaining);
  int recv_pdu = ::recv(static_cast<SocketType>(socket_fd_),
      reinterpret_cast<char*>(pdu.data()), static_cast<int>(pdu_remaining), MSG_WAITALL);
  if (recv_pdu != static_cast<int>(pdu_remaining)) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusTimeout, "recv PDU failed");
  }

  // 合并完整响应
  std::vector<uint8_t> response;
  response.reserve(header.size() + pdu.size());
  response.insert(response.end(), header.begin(), header.end());
  response.insert(response.end(), pdu.begin(), pdu.end());

  // 检查异常响应（func_code 最高位为 1）
  uint8_t func = pdu[0];
  if (func & 0x80) {
    uint8_t exc_code = pdu.size() > 1 ? pdu[1] : 0;
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::ModbusError,
        "Modbus exception code " + std::to_string(exc_code));
  }

  return common::Result<std::vector<uint8_t>>::Ok(std::move(response));
}

// ===== 读取方法 =====

common::Result<ModbusReadResult> ModbusTcpClient::read_registers(
    uint8_t func_code, uint16_t address, uint16_t count) {
  if (!is_connected()) {
    auto rc = reconnect();
    if (!rc.is_ok()) {
      return common::Result<ModbusReadResult>::Err(rc.error_code(), rc.error_msg());
    }
  }

  uint16_t tid = ++transaction_id_;
  auto request = build_request(tid, config_.unit_id, func_code, address, count);
  auto resp_result = send_and_receive(request);
  if (!resp_result.is_ok()) {
    connected_ = false;
    return common::Result<ModbusReadResult>::Err(
        resp_result.error_code(), resp_result.error_msg());
  }

  auto& response = resp_result.value();
  // PDU: func_code(1) + byte_count(1) + data(N*2)
  // Header 7 bytes, PDU starts at offset 7
  if (response.size() < 9) {
    return common::Result<ModbusReadResult>::Err(
        common::ErrorCode::ModbusInvalidFrame, "Response too short");
  }

  uint8_t byte_count = response[8];
  ModbusReadResult result;
  for (uint8_t i = 0; i < byte_count; i += 2) {
    uint16_t reg_val = get_uint16_be(response.data() + 9 + i);
    result.registers.push_back(reg_val);
  }
  return common::Result<ModbusReadResult>::Ok(std::move(result));
}

common::Result<ModbusReadResult> ModbusTcpClient::read_bits(
    uint8_t func_code, uint16_t address, uint16_t count) {
  if (!is_connected()) {
    auto rc = reconnect();
    if (!rc.is_ok()) {
      return common::Result<ModbusReadResult>::Err(rc.error_code(), rc.error_msg());
    }
  }

  uint16_t tid = ++transaction_id_;
  auto request = build_request(tid, config_.unit_id, func_code, address, count);
  auto resp_result = send_and_receive(request);
  if (!resp_result.is_ok()) {
    connected_ = false;
    return common::Result<ModbusReadResult>::Err(
        resp_result.error_code(), resp_result.error_msg());
  }

  auto& response = resp_result.value();
  if (response.size() < 9) {
    return common::Result<ModbusReadResult>::Err(
        common::ErrorCode::ModbusInvalidFrame, "Response too short");
  }

  uint8_t byte_count = response[8];
  ModbusReadResult result;
  for (uint8_t byte_idx = 0; byte_idx < byte_count; ++byte_idx) {
    uint8_t byte_val = response[9 + byte_idx];
    for (uint8_t bit_idx = 0; bit_idx < 8 && result.coils.size() < count; ++bit_idx) {
      result.coils.push_back((byte_val & (1 << bit_idx)) != 0);
    }
  }
  return common::Result<ModbusReadResult>::Ok(std::move(result));
}

common::Result<ModbusReadResult> ModbusTcpClient::read_holding_registers(
    uint16_t address, uint16_t count) {
  return read_registers(0x03, address, count);
}

common::Result<ModbusReadResult> ModbusTcpClient::read_input_registers(
    uint16_t address, uint16_t count) {
  return read_registers(0x04, address, count);
}

common::Result<ModbusReadResult> ModbusTcpClient::read_coils(
    uint16_t address, uint16_t count) {
  return read_bits(0x01, address, count);
}

common::Result<ModbusReadResult> ModbusTcpClient::read_discrete_inputs(
    uint16_t address, uint16_t count) {
  return read_bits(0x02, address, count);
}

// ===== 写入方法 =====

common::VoidResult ModbusTcpClient::write_single_register(
    uint16_t address, uint16_t value) {
  if (!is_connected()) {
    auto rc = reconnect();
    if (!rc.is_ok()) return rc;
  }
  uint16_t tid = ++transaction_id_;
  auto request = build_write_single_reg_request(tid, config_.unit_id, address, value);
  auto resp_result = send_and_receive(request);
  if (!resp_result.is_ok()) {
    connected_ = false;
    return common::VoidResult::Err(resp_result.error_code(), resp_result.error_msg());
  }
  return common::VoidResult::Ok();
}

common::VoidResult ModbusTcpClient::write_multiple_registers(
    uint16_t address, const std::vector<uint16_t>& values) {
  if (!is_connected()) {
    auto rc = reconnect();
    if (!rc.is_ok()) return rc;
  }
  uint16_t tid = ++transaction_id_;
  auto request = build_write_multi_reg_request(tid, config_.unit_id, address, values);
  auto resp_result = send_and_receive(request);
  if (!resp_result.is_ok()) {
    connected_ = false;
    return common::VoidResult::Err(resp_result.error_code(), resp_result.error_msg());
  }
  return common::VoidResult::Ok();
}

common::VoidResult ModbusTcpClient::write_single_coil(
    uint16_t address, bool value) {
  if (!is_connected()) {
    auto rc = reconnect();
    if (!rc.is_ok()) return rc;
  }
  uint16_t tid = ++transaction_id_;
  auto request = build_write_single_coil_request(tid, config_.unit_id, address, value);
  auto resp_result = send_and_receive(request);
  if (!resp_result.is_ok()) {
    connected_ = false;
    return common::VoidResult::Err(resp_result.error_code(), resp_result.error_msg());
  }
  return common::VoidResult::Ok();
}

} // namespace openems::modbus