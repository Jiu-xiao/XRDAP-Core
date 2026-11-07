// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

// docs/protocol.md 3.3 的条目，不解引用：ap、id 1、size、base 0x20000000、n 0、final 0，
// 共 11 字节 / An item of docs/protocol.md 3.3 without dereference: ap, id 1, size, base
// 0x20000000, n 0, final 0, 11 bytes in all.
inline void PutItem(std::vector<uint8_t>& out, uint8_t size, uint8_t ap = 0U)
{
  const uint8_t item[] = {ap, 0x01, 0x00, size, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00};
  out.insert(out.end(), item, item + sizeof(item));
}
