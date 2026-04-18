#pragma once

#include "openems/common/error_code.h"
#include <variant>
#include <string>
#include <stdexcept>

namespace openems::common {

template <typename T>
class Result {
public:
  Result(T value) : data_(std::move(value)) {}
  Result(ErrorCode code, std::string msg = "")
      : data_(ErrorInfo{code, std::move(msg)}) {}

  static Result<T> Ok(T value) { return Result(std::move(value)); }
  static Result<T> Err(ErrorCode code, std::string msg = "") {
    return Result(code, std::move(msg));
  }

  bool is_ok() const { return std::holds_alternative<T>(data_); }
  bool has_error() const { return std::holds_alternative<ErrorInfo>(data_); }

  const T& value() const {
    if (!is_ok()) throw std::runtime_error("Result has no value");
    return std::get<T>(data_);
  }

  T& value() {
    if (!is_ok()) throw std::runtime_error("Result has no value");
    return std::get<T>(data_);
  }

  ErrorCode error_code() const {
    if (!has_error()) return ErrorCode::Ok;
    return std::get<ErrorInfo>(data_).code;
  }

  const std::string& error_msg() const {
    if (!has_error()) throw std::runtime_error("Result has no error");
    return std::get<ErrorInfo>(data_).msg;
  }

private:
  struct ErrorInfo {
    ErrorCode code;
    std::string msg;
  };

  std::variant<T, ErrorInfo> data_;
};

class VoidResult {
public:
  VoidResult() : code_(ErrorCode::Ok) {}
  VoidResult(ErrorCode code, std::string msg = "")
      : code_(code), msg_(std::move(msg)) {}

  static VoidResult Ok() { return VoidResult(); }
  static VoidResult Err(ErrorCode code, std::string msg = "") {
    return VoidResult(code, std::move(msg));
  }

  bool is_ok() const { return code_ == ErrorCode::Ok; }
  bool has_error() const { return code_ != ErrorCode::Ok; }

  ErrorCode error_code() const { return code_; }
  const std::string& error_msg() const { return msg_; }

private:
  ErrorCode code_;
  std::string msg_;
};

} // namespace openems::common