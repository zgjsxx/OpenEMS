// src/protocols/modbus/src/serial_port.cpp
#include "openems/modbus/serial_port.h"
#include "openems/utils/logger.h"

#ifdef _WIN32
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <termios.h>
  #include <unistd.h>
  #include <sys/select.h>
  #include <errno.h>
#endif

#include <cstring>
#include <chrono>

namespace openems::modbus {

SerialPort::SerialPort(const SerialPortConfig& config) : config_(config) {}

SerialPort::~SerialPort() { close(); }

#ifdef _WIN32

common::VoidResult SerialPort::open() {
  // 打开串口设备
  std::string path = config_.port_name;
  HANDLE h = CreateFileA(
      path.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,  // 禁止共享
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);

  if (h == INVALID_HANDLE_VALUE) {
    return common::VoidResult::Err(
        common::ErrorCode::SerialPortOpenFailed,
        "CreateFile failed for " + path + ": error " + std::to_string(GetLastError()));
  }

  // 配置 DCB
  DCB dcb;
  std::memset(&dcb, 0, sizeof(dcb));
  dcb.DCBlength = sizeof(dcb);

  if (!GetCommState(h, &dcb)) {
    CloseHandle(h);
    return common::VoidResult::Err(
        common::ErrorCode::SerialPortConfigFailed,
        "GetCommState failed: error " + std::to_string(GetLastError()));
  }

  dcb.BaudRate = config_.baud_rate;
  dcb.ByteSize = config_.data_bits;
  dcb.StopBits = (config_.stop_bits == 1) ? ONESTOPBIT : TWOSTOPBITS;

  switch (config_.parity) {
    case 'N': dcb.Parity = NOPARITY;  dcb.fParity = FALSE; break;
    case 'E': dcb.Parity = EVENPARITY; dcb.fParity = TRUE;  break;
    case 'O': dcb.Parity = ODDPARITY;  dcb.fParity = TRUE;  break;
    default: dcb.Parity = NOPARITY;  dcb.fParity = FALSE; break;
  }

  // 禁用硬件流控和软件流控
  dcb.fOutxCtsFlow = FALSE;
  dcb.fRtsControl = RTS_CONTROL_ENABLE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  dcb.fDtrControl = DTR_CONTROL_ENABLE;

  if (!SetCommState(h, &dcb)) {
    CloseHandle(h);
    return common::VoidResult::Err(
        common::ErrorCode::SerialPortConfigFailed,
        "SetCommState failed: error " + std::to_string(GetLastError()));
  }

  // 配置超时：ReadIntervalTimeout 用于帧间静默检测
  COMMTIMEOUTS timeouts;
  std::memset(&timeouts, 0, sizeof(timeouts));

  // 计算帧间字符超时（1.5 字符时间, 单位 ms）
  // 1 字符 = (1 start + data_bits + parity + stop_bits) bits
  // 1.5 字符时间 = 1.5 * char_bits / baud_rate * 1000 ms
  uint32_t char_bits = 1 + config_.data_bits + (config_.parity != 'N' ? 1 : 0) + config_.stop_bits;
  uint32_t inter_char_ms = 0;
  if (config_.inter_char_timeout_ms > 0) {
    inter_char_ms = config_.inter_char_timeout_ms;
  } else {
    // 自动计算: 1.5 字符时间
    inter_char_ms = static_cast<uint32_t>(1500.0 * char_bits / config_.baud_rate);
    if (inter_char_ms < 1) inter_char_ms = 1;
  }

  // ReadIntervalTimeout: 两个字符之间的最大间隔，超过即认为帧结束
  // 对于 RTU，这个值 = 1.5 字符时间
  timeouts.ReadIntervalTimeout = inter_char_ms;

  // ReadTotalTimeoutMultiplier: 每个字符的额外超时
  timeouts.ReadTotalTimeoutMultiplier = inter_char_ms;

  // ReadTotalTimeoutConstant: 总超时常量 = 整帧超时
  timeouts.ReadTotalTimeoutConstant = config_.timeout_ms;

  timeouts.WriteTotalTimeoutMultiplier = inter_char_ms;
  timeouts.WriteTotalTimeoutConstant = config_.timeout_ms;

  if (!SetCommTimeouts(h, &timeouts)) {
    CloseHandle(h);
    return common::VoidResult::Err(
        common::ErrorCode::SerialPortConfigFailed,
        "SetCommTimeouts failed: error " + std::to_string(GetLastError()));
  }

  handle_ = h;
  OPENEMS_LOG_I("SerialPort", "Opened " + path + " at " + std::to_string(config_.baud_rate));
  return common::VoidResult::Ok();
}

void SerialPort::close() {
  if (handle_) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
}

bool SerialPort::is_open() const { return handle_ != nullptr; }

common::VoidResult SerialPort::write(const uint8_t* data, size_t len) {
  if (!handle_) {
    return common::VoidResult::Err(common::ErrorCode::SerialPortWriteFailed, "Port not open");
  }

  DWORD written = 0;
  BOOL ok = WriteFile(static_cast<HANDLE>(handle_), data, static_cast<DWORD>(len), &written, nullptr);
  if (!ok || written != static_cast<DWORD>(len)) {
    return common::VoidResult::Err(
        common::ErrorCode::SerialPortWriteFailed,
        "WriteFile failed: wrote " + std::to_string(written) + "/" + std::to_string(len));
  }

  // 等待发送完成
  FlushFileBuffers(static_cast<HANDLE>(handle_));
  return common::VoidResult::Ok();
}

common::Result<std::vector<uint8_t>> SerialPort::read_with_timeout(size_t min_bytes) {
  if (!handle_) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::SerialPortReadFailed, "Port not open");
  }

  std::vector<uint8_t> result;
  result.reserve(256);

  // 循环读取，直到读到 min_bytes 或帧间静默超时
  auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(config_.timeout_ms);

  while (result.size() < min_bytes) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }

    uint8_t buf[256];
    DWORD read = 0;
    BOOL ok = ReadFile(static_cast<HANDLE>(handle_), buf, static_cast<DWORD>(sizeof(buf)), &read, nullptr);
    if (!ok) {
      return common::Result<std::vector<uint8_t>>::Err(
          common::ErrorCode::SerialPortReadFailed,
          "ReadFile failed: error " + std::to_string(GetLastError()));
    }
    if (read > 0) {
      result.insert(result.end(), buf, buf + read);
    } else {
      // 0 bytes read = timeout or inter-frame silence
      break;
    }
  }

  // 继续读取直到帧间静默（0 字符返回）
  if (result.size() >= min_bytes) {
    uint8_t buf[64];
    DWORD read = 0;
    // 再读一次看有没有更多数据（不超过总超时）
    auto now = std::chrono::steady_clock::now();
    if (now < deadline) {
      BOOL ok = ReadFile(static_cast<HANDLE>(handle_), buf, static_cast<DWORD>(sizeof(buf)), &read, nullptr);
      if (ok && read > 0) {
        result.insert(result.end(), buf, buf + read);
      }
    }
  }

  if (result.empty()) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::Timeout, "Serial read timeout, no data received");
  }

  return common::Result<std::vector<uint8_t>>::Ok(std::move(result));
}

void SerialPort::drain() {
  if (!handle_) return;
  // 清空接收缓冲区
  PurgeComm(static_cast<HANDLE>(handle_), PURGE_RXCLEAR | PURGE_TXCLEAR);
}

#else  // Linux

common::VoidResult SerialPort::open() {
  int fd = ::open(config_.port_name.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);
  if (fd < 0) {
    return common::VoidResult::Err(
        common::ErrorCode::SerialPortOpenFailed,
        "open() failed for " + config_.port_name + ": " + strerror(errno));
  }

  // 设为阻塞模式
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

  struct termios tio;
  std::memset(&tio, 0, sizeof(tio));

  if (tcgetattr(fd, &tio) != 0) {
    ::close(fd);
    return common::VoidResult::Err(
        common::ErrorCode::SerialPortConfigFailed,
        std::string("tcgetattr failed: ") + strerror(errno));
  }

  // 配置波特率
  speed_t speed;
  switch (config_.baud_rate) {
    case 1200:   speed = B1200; break;
    case 2400:   speed = B2400; break;
    case 4800:   speed = B4800; break;
    case 9600:   speed = B9600; break;
    case 19200:  speed = B19200; break;
    case 38400:  speed = B38400; break;
    case 57600:  speed = B57600; break;
    case 115200: speed = B115200; break;
    default:
      ::close(fd);
      return common::VoidResult::Err(
          common::ErrorCode::SerialPortConfigFailed,
          "Unsupported baud rate: " + std::to_string(config_.baud_rate));
  }
  cfsetispeed(&tio, speed);
  cfsetospeed(&tio, speed);

  // 数据位
  tio.c_cflag &= ~CSIZE;
  switch (config_.data_bits) {
    case 7: tio.c_cflag |= CS7; break;
    case 8: tio.c_cflag |= CS8; break;
    default:
      ::close(fd);
      return common::VoidResult::Err(
          common::ErrorCode::SerialPortConfigFailed,
          "Unsupported data bits: " + std::to_string(config_.data_bits));
  }

  // 校验位
  switch (config_.parity) {
    case 'N': tio.c_cflag &= ~PARENB; break;
    case 'E': tio.c_cflag |= PARENB; tio.c_cflag &= ~PARODD; break;
    case 'O': tio.c_cflag |= PARENB; tio.c_cflag |= PARODD; break;
  }

  // 停止位
  if (config_.stop_bits == 2) {
    tio.c_cflag |= CSTOPB;
  } else {
    tio.c_cflag &= ~CSTOPB;
  }

  // 启用接收，忽略控制线
  tio.c_cflag |= (CLOCAL | CREAD);

  // 禁用软件流控
  tio.c_iflag &= ~(IXON | IXOFF | IXANY);
  tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

  // 原始输出模式
  tio.c_oflag &= ~OPOST;
  tio.c_oflag &= ~ONLCR;

  // 原始输入模式
  tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

  // 计算帧间字符超时（1.5 字符时间）
  uint32_t char_bits = 1 + config_.data_bits + (config_.parity != 'N' ? 1 : 0) + config_.stop_bits;
  uint32_t inter_char_ds = 0;  // VTIME 单位: 0.1秒
  if (config_.inter_char_timeout_ms > 0) {
    inter_char_ds = config_.inter_char_timeout_ms / 100;  // ms → deciseconds
    if (inter_char_ds == 0) inter_char_ds = 1;
  } else {
    // 自动: 1.5 字符时间
    double ms = 1500.0 * char_bits / config_.baud_rate;
    inter_char_ds = static_cast<uint32_t>(ms / 100);
    if (inter_char_ds == 0) inter_char_ds = 1;
  }

  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = static_cast<cc_t>(inter_char_ds);

  if (tcsetattr(fd, TCSANOW, &tio) != 0) {
    ::close(fd);
    return common::VoidResult::Err(
        common::ErrorCode::SerialPortConfigFailed,
        std::string("tcsetattr failed: ") + strerror(errno));
  }

  tcflush(fd, TCIOFLUSH);
  fd_ = fd;
  OPENEMS_LOG_I("SerialPort", "Opened " + config_.port_name + " at " + std::to_string(config_.baud_rate));
  return common::VoidResult::Ok();
}

void SerialPort::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool SerialPort::is_open() const { return fd_ >= 0; }

common::VoidResult SerialPort::write(const uint8_t* data, size_t len) {
  if (fd_ < 0) {
    return common::VoidResult::Err(common::ErrorCode::SerialPortWriteFailed, "Port not open");
  }

  ssize_t written = ::write(fd_, data, len);
  if (written < 0) {
    return common::VoidResult::Err(
        common::ErrorCode::SerialPortWriteFailed,
        std::string("write() failed: ") + strerror(errno));
  }
  if (static_cast<size_t>(written) != len) {
    return common::VoidResult::Err(
        common::ErrorCode::SerialPortWriteFailed,
        "Partial write: " + std::to_string(written) + "/" + std::to_string(len));
  }

  tcdrain(fd_);
  return common::VoidResult::Ok();
}

common::Result<std::vector<uint8_t>> SerialPort::read_with_timeout(size_t min_bytes) {
  if (fd_ < 0) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::SerialPortReadFailed, "Port not open");
  }

  std::vector<uint8_t> result;
  result.reserve(256);

  auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(config_.timeout_ms);

  while (true) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;

    auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd_, &read_fds);

    struct timeval tv;
    tv.tv_sec = remaining_ms / 1000;
    tv.tv_usec = (remaining_ms % 1000) * 1000;

    int sel = select(fd_ + 1, &read_fds, nullptr, nullptr, &tv);
    if (sel < 0) {
      return common::Result<std::vector<uint8_t>>::Err(
          common::ErrorCode::SerialPortReadFailed,
          std::string("select() failed: ") + strerror(errno));
    }
    if (sel == 0) break;  // timeout

    uint8_t buf[256];
    ssize_t n = ::read(fd_, buf, sizeof(buf));
    if (n < 0) {
      return common::Result<std::vector<uint8_t>>::Err(
          common::ErrorCode::SerialPortReadFailed,
          std::string("read() failed: ") + strerror(errno));
    }
    if (n == 0) break;  // inter-frame silence
    result.insert(result.end(), buf, buf + n);

    if (result.size() >= min_bytes) {
      // 读到足够字节后再尝试一次看有无更多数据
      now = std::chrono::steady_clock::now();
      if (now < deadline) {
        remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        // 等待一个字符时间看有没有后续数据
        FD_ZERO(&read_fds);
        FD_SET(fd_, &read_fds);
        tv.tv_sec = 0;
        tv.tv_usec = (remaining_ms < 100 ? remaining_ms : 100) * 1000;
        sel = select(fd_ + 1, &read_fds, nullptr, nullptr, &tv);
        if (sel > 0) {
          n = ::read(fd_, buf, sizeof(buf));
          if (n > 0) result.insert(result.end(), buf, buf + n);
        }
      }
      break;
    }
  }

  if (result.empty()) {
    return common::Result<std::vector<uint8_t>>::Err(
        common::ErrorCode::Timeout, "Serial read timeout, no data received");
  }

  return common::Result<std::vector<uint8_t>>::Ok(std::move(result));
}

void SerialPort::drain() {
  if (fd_ < 0) return;
  tcflush(fd_, TCIOFLUSH);
}

#endif  // _WIN32 / Linux

} // namespace openems::modbus
