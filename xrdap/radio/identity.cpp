// SPDX-License-Identifier: Apache-2.0
#include "xrdap/radio/identity.hpp"

#include <cstring>

#include "xrdap/radio/packet.hpp"

namespace XrDap::Radio
{

namespace
{

// 32 位字里相邻位变化的次数。 / Bit changes along a 32-bit word.
int Transitions(uint32_t word)
{
  return __builtin_popcount(word ^ (word >> 1)) - static_cast<int>((word >> 31) & 1U);
}

}  // namespace

Identity DefaultIdentity() { return {2'440'000'000U, 0xFFFF, {'X', 'R', 'D', 'P'}}; }

uint32_t Fnv1a(const uint8_t* data, size_t size, uint32_t hash)
{
  for (size_t i = 0; i < size; ++i)
  {
    hash = (hash ^ data[i]) * 16777619U;
  }
  return hash;
}

uint32_t LinkId(const uint8_t* a, const uint8_t* b)
{
  // 小的 UID 在前，两块探针算出同一个号。 / Smaller UID first, so that both probes
  // compute the same id.
  const bool a_first = std::memcmp(a, b, kUidSize) < 0;
  uint32_t hash = Fnv1a(a_first ? a : b, kUidSize);
  hash = Fnv1a(a_first ? b : a, kUidSize, hash);
  return hash != 0U ? hash : 1U;
}

Identity PairIdentity(uint32_t link_id)
{
  Identity identity{};
  // 2403..2479 MHz 中的一个 1 MHz 频点。 / One 1 MHz channel within 2403..2479 MHz.
  identity.frequency_hz = (2403U + link_id % 77U) * 1'000'000U;
  identity.crc_seed = static_cast<uint16_t>(link_id >> 16 ^ link_id);
  // 同步字取相邻位变化 10 到 22 次的散列值，避开过于规则的位型。
  // The sync word is a hash with 10 to 22 bit changes, avoiding too regular patterns.
  uint8_t bytes[4];
  std::memcpy(bytes, &link_id, sizeof(bytes));
  uint32_t word = Fnv1a(bytes, sizeof(bytes));
  while (Transitions(word) < 10 || Transitions(word) > 22)
  {
    std::memcpy(bytes, &word, sizeof(bytes));
    word = Fnv1a(bytes, sizeof(bytes));
  }
  for (int i = 0; i < 4; ++i)
  {
    identity.sync_word[i] = static_cast<uint8_t>(word >> (24 - 8 * i));
  }
  return identity;
}

uint8_t BuildHello(const uint8_t* own, const uint8_t* peer, uint8_t* out)
{
  out[0] = kHello;
  std::memcpy(out + 1, own, kUidSize);
  if (peer != nullptr)
  {
    std::memcpy(out + 1 + kUidSize, peer, kUidSize);
  }
  else
  {
    std::memset(out + 1 + kUidSize, 0, kUidSize);
  }
  return kHelloSize;
}

}  // namespace XrDap::Radio
