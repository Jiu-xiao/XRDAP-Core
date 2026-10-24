// SPDX-License-Identifier: Apache-2.0
#include <cstring>

#include "check.hpp"
#include "xrdap/probe/probe_identity.hpp"

// 序列号 = 前缀 + 标识的大写十六进制（docs/protocol.md 4.1）/ Serial = prefix + the id
// in upper-case hex (docs/protocol.md 4.1).
TEST(FormatSerialUpperHex)
{
  const uint8_t id[3] = {0x0A, 0xBC, 0x01};
  char out[XrDap::SerialBufferSize(3)];
  XrDap::FormatSerial(id, 3, out);
  CHECK(std::strcmp(out, "XRobot-XRDAP-0ABC01") == 0);
}
