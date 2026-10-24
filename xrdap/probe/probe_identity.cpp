// SPDX-License-Identifier: Apache-2.0
#include "xrdap/probe/probe_identity.hpp"

namespace XrDap
{

void FormatSerial(const uint8_t* id, size_t id_bytes, char* out)
{
  constexpr char kHex[] = "0123456789ABCDEF";
  for (const char* s = kSerialPrefix; *s != '\0'; ++s)
  {
    *out++ = *s;
  }
  for (size_t i = 0; i < id_bytes; ++i)
  {
    *out++ = kHex[id[i] >> 4];
    *out++ = kHex[id[i] & 0x0FU];
  }
  *out = '\0';
}

}  // namespace XrDap
