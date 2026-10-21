// SPDX-License-Identifier: Apache-2.0
#include "xrdap/serial/serial_frame.hpp"

#include <array>

namespace XrDap::Serial
{
namespace
{
template <typename T, T POLY>
constexpr std::array<T, 256> ReflectedTable()
{
  std::array<T, 256> table{};
  for (size_t i = 0; i < table.size(); ++i)
  {
    T crc = static_cast<T>(i);
    for (int j = 0; j < 8; ++j)
    {
      crc = (crc & 1U) ? static_cast<T>((crc >> 1U) ^ POLY) : static_cast<T>(crc >> 1U);
    }
    table[i] = crc;
  }
  return table;
}

constexpr auto kCrc8Table = ReflectedTable<uint8_t, 0x8CU>();
constexpr auto kCrc32Table = ReflectedTable<uint32_t, 0xEDB88320U>();
}  // namespace

uint8_t Crc8(const uint8_t* data, size_t len)
{
  uint8_t crc = 0xFFU;
  while (len-- > 0U)
  {
    crc = kCrc8Table[crc ^ *data++];
  }
  return crc;
}

uint32_t Crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = 0xFFFFFFFFU;
  while (len-- > 0U)
  {
    crc = kCrc32Table[(crc ^ *data++) & 0xFFU] ^ (crc >> 8U);
  }
  return crc;
}

}  // namespace XrDap::Serial
