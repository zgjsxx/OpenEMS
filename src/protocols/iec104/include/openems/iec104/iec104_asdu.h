#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <variant>
#include "openems/common/types.h"

namespace openems::iec104 {

// IEC104 frame type identifiers
enum class TypeId : uint8_t {
  M_SP_NA_1 = 1,    // Single-point information
  M_SP_TB_1 = 30,   // Single-point with CP56Time2a
  M_DP_NA_1 = 3,    // Double-point information
  M_DP_TB_1 = 31,   // Double-point with CP56Time2a
  M_ME_NA_1 = 9,    // Measured value, normalized
  M_ME_NB_1 = 11,   // Measured value, scaled
  M_ME_NC_1 = 13,   // Measured value, short float
  M_ME_TD_1 = 34,   // Short float with CP56Time2a
  M_IT_NA_1 = 15,   // Integrated totals
  C_SC_NA_1 = 45,   // Single command
  C_DC_NA_1 = 46,   // Double command
  C_SE_NA_1 = 48,   // Setpoint, normalized
  C_SE_NB_1 = 49,   // Setpoint, scaled
  C_SE_NC_1 = 50,   // Setpoint, short float
  C_IC_NA_1 = 100,  // Interrogation command
  C_CI_NA_1 = 101,  // Counter interrogation
  C_CS_NA_1 = 103,  // Clock synchronization
};

// Cause of Transmission
enum class Cot : uint8_t {
  Periodic            = 1,
  Spontaneous         = 3,
  InterrogatedByGI    = 20,
  Activation          = 6,
  ActivationCon       = 7,
  Deactivation        = 8,
  DeactivationCon     = 9,
};

// Frame type detection
enum class FrameType {
  I, S,
  U_StartdtAct, U_StartdtCon,
  U_StopdtAct, U_StopdtCon,
  U_TestfrAct, U_TestfrCon
};

// ASDU parsed result
struct SinglePointData { bool value; };
struct DoublePointData { uint8_t value; };
struct NormalizedValueData { float value; };
struct ScaledValueData { int16_t raw; };
struct ShortFloatData { float value; };

using AsduValue = std::variant<SinglePointData, DoublePointData,
                               NormalizedValueData, ScaledValueData, ShortFloatData>;

struct AsduData {
  TypeId type_id;
  uint8_t vsq;
  Cot cot;
  uint16_t common_address;
  uint32_t ioa;
  bool sq;

  std::vector<uint32_t> sequence_ioas;
  std::vector<AsduValue> sequence_data;
};

// Parse ASDU from raw bytes (after control field)
std::optional<AsduData> parse_asdu(const uint8_t* data, size_t len);

// Build APDU frames
std::vector<uint8_t> build_startdt_act();
std::vector<uint8_t> build_startdt_con();
std::vector<uint8_t> build_stopdt_act();
std::vector<uint8_t> build_testfr_act();
std::vector<uint8_t> build_testfr_con();
std::vector<uint8_t> build_s_frame(uint16_t rx);
std::vector<uint8_t> build_interrogation_cmd(uint16_t tx, uint16_t rx,
                                              uint16_t common_address,
                                              uint8_t cot = 6);

FrameType detect_frame_type(const uint8_t* ctrl_field);

} // namespace openems::iec104