// SPDX-License-Identifier: Apache-2.0
#pragma once

// 一对探针的射频身份和配对包。纯逻辑。
//
// 身份是频点、CRC 初值和 GFSK 同步字。频点总是控制信道 F（channel.hpp），数据信道另外
// 协商；CRC 初值和同步字把一对探针与别的对分开。未配对的探针用默认身份；配对后两块探针由
// 各自的芯片 UID 算出同一个链路号，再由链路号算出身份。FLRC 不用身份里的同步字，固定为
// "XRDP"（勘误 16.4 对编码率 1 没有公布要避开的值）。
//
// 配对：两块都在配对中的探针在 F 上用默认身份（GFSK），每 20 ms 左右发一个 HELLO 包：
//   [0] 5（packet.hpp）  [1..12] 自己的芯片 UID  [13..24] 目前听到的对方 UID，没有为 0
// 听到点了自己名字的 HELLO 的探针配对完成，它再应答 1 s，让对方也听到自己的 UID。同样
// 两块探针再次配对得到同一个身份。
//
// Radio identity of a pair of probes, and the pairing packet. Pure logic.
//
// The identity is the frequency, the CRC seed and the GFSK sync word. The frequency is
// always the control channel F (channel.hpp), and the data channel is agreed on
// separately; the CRC seed and the sync word tell a pair apart from other pairs.
// Unpaired probes use the default identity; once paired, both probes compute the same
// link id from their two chip UIDs and the identity from the link id. FLRC does not use
// the identity's sync word and keeps "XRDP" (errata 16.4 publishes no values to avoid at
// coding rate 1).
//
// Pairing: two probes in pairing both use the default identity (GFSK) on F and send a
// HELLO packet every 20 ms or so:
//   [0] 5 (packet.hpp)  [1..12] own chip UID  [13..24] the UID of the peer heard so
//   far, or zeros
// A probe that hears a HELLO naming itself is done; it keeps answering for another second
// so that the other probe hears its own UID as well. Pairing the same two probes again
// gives the same identity.

#include <cstddef>
#include <cstdint>

namespace XrDap::Radio
{

/// 芯片 UID 字节数 / Chip UID size.
inline constexpr size_t kUidSize = 12;
/// HELLO 包长 / HELLO packet size.
inline constexpr uint8_t kHelloSize = 1 + 2 * kUidSize;
/// FLRC 的同步字 / The FLRC sync word.
inline constexpr uint8_t kFlrcSyncWord[4] = {'X', 'R', 'D', 'P'};

/// 一对探针的射频身份 / Radio identity of a pair.
struct Identity
{
  uint32_t frequency_hz;  ///< 载波频率 / Carrier frequency.
  uint16_t crc_seed;      ///< CRC 初值 / CRC seed.
  uint8_t sync_word[4];   ///< GFSK 同步字 / GFSK sync word.
};

/// 未配对探针的身份 / The identity of unpaired probes.
Identity DefaultIdentity();

/// @brief FNV-1a 32 位散列 / 32-bit FNV-1a hash.
uint32_t Fnv1a(const uint8_t* data, size_t size, uint32_t hash = 2166136261U);

/// @brief 两个 UID 的链路号，与顺序无关，不为 0 / The link id of two UIDs: independent
///        of their order, never 0.
uint32_t LinkId(const uint8_t* a, const uint8_t* b);

/// @brief 链路号对应的射频身份 / The radio identity of a link id.
Identity PairIdentity(uint32_t link_id);

/// @brief 写出 HELLO 包，返回包长 / Write a HELLO packet and return its size.
/// @param peer 对方 UID，还不知道时为 nullptr / The peer's UID, nullptr while unknown.
uint8_t BuildHello(const uint8_t* own, const uint8_t* peer, uint8_t* out);

}  // namespace XrDap::Radio
