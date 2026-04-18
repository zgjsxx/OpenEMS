// src/common/include/openems/common/constants.h
#pragma once

#include <cstdint>

namespace openems::common::constants {

constexpr uint16_t DefaultModbusPort = 502;
constexpr uint16_t DefaultModbusUnitId = 1;
constexpr uint32_t DefaultModbusTimeoutMs = 3000;
constexpr uint32_t DefaultPollIntervalMs = 1000;
constexpr uint32_t DefaultReconnectIntervalMs = 5000;
constexpr uint32_t DefaultMaxReconnectAttempts = 3;
constexpr double  DefaultSocMin = 10.0;
constexpr double  DefaultSocMax = 95.0;
constexpr double  DefaultPowerFactor = 1.0;

// Modbus 功能码
constexpr uint8_t ModbusReadCoils             = 1;
constexpr uint8_t ModbusReadDiscreteInputs    = 2;
constexpr uint8_t ModbusReadHoldingRegisters  = 3;
constexpr uint8_t ModbusReadInputRegisters    = 4;
constexpr uint8_t ModbusWriteSingleCoil       = 5;
constexpr uint8_t ModbusWriteSingleRegister   = 6;
constexpr uint8_t ModbusWriteMultipleRegisters = 16;

} // namespace openems::common::constants