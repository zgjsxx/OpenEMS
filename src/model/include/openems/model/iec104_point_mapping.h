#pragma once

#include <cstdint>
#include <string>

namespace openems::model {

struct Iec104PointMapping {
  uint8_t  type_id = 0;        // ASDU TypeID: 1=SP, 3=DP, 9=NA, 11=NB, 13=NC
  uint32_t ioa = 0;            // Information Object Address (3 bytes, 0~16777215)
  uint16_t common_address = 1; // Station Common Address (2 bytes)
  double   scale = 1.0;        // for M_ME_NB_1 (scaled value)
  uint8_t  cot = 3;            // Cause Of Transmission: 1=periodic, 3=spontaneous, 20=interrogated

  std::string to_string() const;
};

} // namespace openems::model