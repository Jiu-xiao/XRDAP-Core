// SPDX-License-Identifier: Apache-2.0
// 探针序列号。判据：docs/protocol.md 4.1（前缀加标识的大写十六进制，及其例子）/ The probe
// serial. Oracle: docs/protocol.md 4.1 (the prefix plus the id in upper-case hex, and its
// example).
#include "xrdap/probe/probe_identity.hpp"

#include <cstring>

#include "check.hpp"

TEST(FormatSerialMatchesTheDocumentExample)
{
  const uint8_t id[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
  // 先填满别的值，结尾的 0 也要由它写 / Filled with something else first: it has to write
  // the terminating 0 too.
  char out[XrDap::SerialBufferSize(6)];
  std::memset(out, 'x', sizeof(out));
  XrDap::FormatSerial(id, 6, out);
  CHECK(std::strcmp(out, "XRobot-XRDAP-0123456789AB") == 0);
}
