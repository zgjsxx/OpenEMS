// src/iec104/src/iec104_asdu.cpp
#include "openems/iec104/iec104_asdu.h"
#include <cstring>
#include <cmath>

namespace openems::iec104 {

// ===== Frame building =====

static void put_byte(std::vector<uint8_t>& buf, uint8_t v) { buf.push_back(v); }
static void put_uint16_le(std::vector<uint8_t>& buf, uint16_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
  buf.push_back(static_cast<uint8_t>(v >> 8));
}

static std::vector<uint8_t> build_u_frame(uint8_t type_bits) {
  std::vector<uint8_t> frame;
  put_byte(frame, 0x68);  // Start
  put_byte(frame, 4);     // APDU length = 4 (control field only)
  // Control field: U-frame format
  // Byte 1: type_bits, Byte 2-4: 0
  put_byte(frame, type_bits);
  put_byte(frame, 0);
  put_byte(frame, 0);
  put_byte(frame, 0);
  return frame;
}

std::vector<uint8_t> build_startdt_act() {
  std::vector<uint8_t> f;
  f.push_back(0x68); f.push_back(4);
  f.push_back(0x07); f.push_back(0); f.push_back(0); f.push_back(0);
  return f;
}

std::vector<uint8_t> build_startdt_con() {
  std::vector<uint8_t> f;
  f.push_back(0x68); f.push_back(4);
  f.push_back(0x0B); f.push_back(0); f.push_back(0); f.push_back(0);
  return f;
}

std::vector<uint8_t> build_stopdt_act() {
  std::vector<uint8_t> f;
  f.push_back(0x68); f.push_back(4);
  f.push_back(0x13); f.push_back(0); f.push_back(0); f.push_back(0);
  return f;
}

std::vector<uint8_t> build_testfr_act() {
  std::vector<uint8_t> f;
  f.push_back(0x68); f.push_back(4);
  f.push_back(0x43); f.push_back(0); f.push_back(0); f.push_back(0);
  return f;
}

std::vector<uint8_t> build_testfr_con() {
  std::vector<uint8_t> f;
  f.push_back(0x68); f.push_back(4);
  f.push_back(0x83); f.push_back(0); f.push_back(0); f.push_back(0);
  return f;
}

std::vector<uint8_t> build_s_frame(uint16_t rx) {
  std::vector<uint8_t> f;
  f.push_back(0x68); f.push_back(4);
  f.push_back(0x01);  // S-frame bit
  f.push_back(0);
  put_uint16_le(f, rx);
  return f;
}

std::vector<uint8_t> build_interrogation_cmd(uint16_t tx, uint16_t rx,
                                              uint16_t common_address,
                                              uint8_t cot) {
  std::vector<uint8_t> f;
  // ASDU: TypeID=100, VSQ=1, COT=cot, CA=common_address, IOA=0, QOI=20
  std::vector<uint8_t> asdu;
  asdu.push_back(100);  // TypeID: C_IC_NA_1
  asdu.push_back(1);    // VSQ=1 (single)
  asdu.push_back(cot);  // COT
  asdu.push_back(0);    // COT (high byte, OA bit = 0)
  put_uint16_le(asdu, common_address);
  // IOA (3 bytes)
  asdu.push_back(0); asdu.push_back(0); asdu.push_back(0);
  // QOI = 20 (station interrogation)
  asdu.push_back(20);

  // APDU header
  f.push_back(0x68);
  uint16_t apdu_len = 4 + static_cast<uint16_t>(asdu.size());
  f.push_back(static_cast<uint8_t>(apdu_len));
  // Control field: I-frame
  put_uint16_le(f, tx);  // send sequence
  put_uint16_le(f, rx);  // receive sequence
  // ASDU
  for (auto b : asdu) f.push_back(b);
  return f;
}

// ===== Frame type detection =====

FrameType detect_frame_type(const uint8_t* ctrl) {
  // U-frame: bit 0 of byte0 = 1, bit 1 = 1
  if ((ctrl[0] & 0x03) == 0x03) {
    uint8_t type_bits = ctrl[0] & 0xFC;
    if (type_bits == 0x04) return FrameType::U_StartdtAct;
    if (type_bits == 0x08) return FrameType::U_StartdtCon;
    if (type_bits == 0x10) return FrameType::U_StopdtAct;
    if (type_bits == 0x14) return FrameType::U_StopdtCon;
    if (type_bits == 0x40) return FrameType::U_TestfrAct;
    if (type_bits == 0x80) return FrameType::U_TestfrCon;
  }
  // S-frame: bit 0 = 1, bit 1 = 0
  if ((ctrl[0] & 0x03) == 0x01) return FrameType::S;
  // I-frame: bit 0 = 0
  if ((ctrl[0] & 0x01) == 0x00) return FrameType::I;
  return FrameType::I;  // default
}

// ===== ASDU parsing =====

static uint32_t get_ioa_3byte(const uint8_t* p) {
  return static_cast<uint32_t>(p[0])
       | (static_cast<uint32_t>(p[1]) << 8)
       | (static_cast<uint32_t>(p[2]) << 16);
}

static int16_t get_int16_le(const uint8_t* p) {
  return static_cast<int16_t>(p[0] | (p[1] << 8));
}

static float get_float_le(const uint8_t* p) {
  float v;
  std::memcpy(&v, p, sizeof(float));
  return v;
}

std::optional<AsduData> parse_asdu(const uint8_t* data, size_t len) {
  // Minimum ASDU length: TypeID(1) + VSQ(1) + COT(1) + COT_hi(1) + CA(2) + IOA(3) = 9
  if (len < 9) return std::nullopt;

  AsduData asdu;
  asdu.type_id = static_cast<TypeId>(data[0]);
  uint8_t vsq = data[1];
  asdu.vsq = vsq;
  asdu.sq = (vsq & 0x80) != 0;
  uint8_t num_objects = vsq & 0x7F;

  uint8_t cot_low = data[2];
  uint8_t cot_hi_byte = data[3];
  asdu.cot = static_cast<Cot>(cot_low & 0x3F);
  // OA bit in cot_hi_byte[0] ignored for now
  asdu.common_address = static_cast<uint16_t>(data[4] | (data[5] << 8));

  size_t offset = 6;  // after TypeID..CA

  auto type_id_val = static_cast<uint8_t>(asdu.type_id);

  if (!asdu.sq) {
    // SQ=0: each object has its own IOA
    for (uint8_t i = 0; i < num_objects && offset < len; ++i) {
      if (offset + 3 > len) break;
      uint32_t ioa = get_ioa_3byte(data + offset);
      offset += 3;

      switch (type_id_val) {
        case 1:  // M_SP_NA_1: Single point, 1 byte SPI + SIQ
        case 30: // M_SP_TB_1: with CP56Time2a
          if (offset + 1 > len) break;
          asdu.sequence_ioas.push_back(ioa);
          asdu.sequence_data.push_back(SinglePointData{(data[offset] & 0x01) != 0});
          offset += 1;
          if (type_id_val == 30) offset += 7;  // CP56Time2a
          break;

        case 3:  // M_DP_NA_1: Double point, 1 byte DPI + DIQ
        case 31:
          if (offset + 1 > len) break;
          asdu.sequence_ioas.push_back(ioa);
          asdu.sequence_data.push_back(DoublePointData{static_cast<uint8_t>(data[offset] & 0x03)});
          offset += 1;
          if (type_id_val == 31) offset += 7;
          break;

        case 9:  // M_ME_NA_1: Normalized value, 2 bytes + QDS
          if (offset + 3 > len) break;
          asdu.sequence_ioas.push_back(ioa);
          asdu.sequence_data.push_back(NormalizedValueData{
              static_cast<float>(get_int16_le(data + offset)) / 32768.0f});
          offset += 3;
          break;

        case 11: // M_ME_NB_1: Scaled value, 2 bytes + QDS
          if (offset + 3 > len) break;
          asdu.sequence_ioas.push_back(ioa);
          asdu.sequence_data.push_back(ScaledValueData{get_int16_le(data + offset)});
          offset += 3;
          break;

        case 13: // M_ME_NC_1: Short float, 4 bytes + QDS
        case 34:
          if (offset + 5 > len) break;
          asdu.sequence_ioas.push_back(ioa);
          asdu.sequence_data.push_back(ShortFloatData{get_float_le(data + offset)});
          offset += 5;
          if (type_id_val == 34) offset += 3;  // CP56Time2a partial
          break;

        default:
          // Unknown type — skip IOA+1 byte minimum
          asdu.sequence_ioas.push_back(ioa);
          offset += 1;
          break;
      }
    }
  } else {
    // SQ=1: sequence of objects starting at first IOA, each without separate IOA
    if (offset + 3 > len) return std::nullopt;
    asdu.ioa = get_ioa_3byte(data + offset);
    offset += 3;

    for (uint8_t i = 0; i < num_objects && offset < len; ++i) {
      uint32_t seq_ioa = asdu.ioa + i;
      asdu.sequence_ioas.push_back(seq_ioa);

      switch (type_id_val) {
        case 1:  // 1 byte each
          asdu.sequence_data.push_back(SinglePointData{(data[offset] & 0x01) != 0});
          offset += 1;
          break;
        case 3:
          asdu.sequence_data.push_back(DoublePointData{static_cast<uint8_t>(data[offset] & 0x03)});
          offset += 1;
          break;
        case 9:
          asdu.sequence_data.push_back(NormalizedValueData{
              static_cast<float>(get_int16_le(data + offset)) / 32768.0f});
          offset += 3;
          break;
        case 11:
          asdu.sequence_data.push_back(ScaledValueData{get_int16_le(data + offset)});
          offset += 3;
          break;
        case 13:
          asdu.sequence_data.push_back(ShortFloatData{get_float_le(data + offset)});
          offset += 5;
          break;
        default:
          offset += 1;
          break;
      }
    }
  }

  return asdu;
}

} // namespace openems::iec104