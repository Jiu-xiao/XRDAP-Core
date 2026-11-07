// SPDX-License-Identifier: Apache-2.0
#pragma once

// 收发器契约：RadioLink 对射频驱动的全部要求。驱动作为模板参数 RadioT 传入，提供下面的
// 方法。
//
//   LibXR::ErrorCode Start(const LinkParams& params)
//       按参数启动并进入接收；可以重复调用，每次都重新校准，可以阻塞几 ms。启动失败由驱动
//       自己恢复。
//   uint8_t MaxPayload() const
//       当前速率下一包的最大字节数，kMinPayload 到 255。只取决于速率，一对探针上相同。
//   LibXR::ErrorCode Send(const uint8_t* data, uint8_t size)
//       开始发一包就返回，size 为 1 到 MaxPayload()。只有上一包还在发射时返回 BUSY，这个
//       状态不靠 Poll()
//       也会结束；其他返回值都表示这一包已被收下。紧接着发的包之间不回接收。
//   bool Sending() const
//       还有一包在发射。
//   void Poll()
//       发射结束时回到接收，也处理收到的包。
//   int Receive(uint8_t* out)
//       取一个收到的包：返回字节数；没有包返回 0；坏包返回负数。out 至少放得下
//       MaxPayload() 字节。
//   LibXR::ErrorCode Tune(uint32_t frequency_hz)
//       只在没有发射时调用。换到最接近的可用频率，留在接收，丢掉没取走的包。
//   int16_t RssiInstant()
//       当前频率上的信号强度，单位 0.5 dBm，不要求校准。
//   void PrepareSend()
//       提示接下来很快要发射，驱动可以提前准备，也可以什么都不做。
//   uint32_t TxPackets() const、TxErrors() const、RxPackets() const、RxErrors() const、
//   Rearms() const、Restarts() const
//       从启动起累计的计数；驱动没有的计数返回 0。
//
// 一包的空中时间加收发切换要远小于 kPacketWaitUs：主机和远端都按它判断下一包还会不会来。
//
// The transceiver contract: all that RadioLink asks of a radio driver. The driver is the
// template parameter RadioT and provides the methods below.
//
//   LibXR::ErrorCode Start(const LinkParams& params)
//       Start with these parameters and enter receive. It may be called again, calibrates
//       each time and may block for a few ms. A failed start recovers in the driver.
//   uint8_t MaxPayload() const
//       Largest packet at the current rate, kMinPayload to 255. It depends on the rate
//       only and is the same on both probes of a pair.
//   LibXR::ErrorCode Send(const uint8_t* data, uint8_t size)
//       Start sending one packet and return; size is 1 to MaxPayload(). BUSY only while
//       the previous packet is on air, and that ends without Poll(); any other result
//       means the packet was taken. Packets sent right after each other do not return
//       to receive in between.
//   bool Sending() const
//       A packet is on air.
//   void Poll()
//       Returns to receive once the transmission is over, and handles received packets.
//   int Receive(uint8_t* out)
//       Fetch one received packet: its size; 0 for none; negative for a bad packet. out
//       holds at least MaxPayload() bytes.
//   LibXR::ErrorCode Tune(uint32_t frequency_hz)
//       Called only when not sending. Move to the nearest usable frequency, stay in
//       receive and drop any packet not yet fetched.
//   int16_t RssiInstant()
//       Signal level at the current frequency, in units of 0.5 dBm; it need not be
//       calibrated.
//   void PrepareSend()
//       A hint that a transmission follows soon; the driver may prepare or do nothing.
//   uint32_t TxPackets() const, TxErrors() const, RxPackets() const, RxErrors() const,
//   Rearms() const, Restarts() const
//       Counters since start; a counter the driver does not have returns 0.
//
// One packet's time on air plus the switch between send and receive must be well below
// kPacketWaitUs: host and remote both use it to decide whether another packet will come.

#include <cstdint>

namespace XrDap::Radio
{

/// 速率档，保存在设置里；每块板子定义它的具体调制 / Rate, stored in the settings; each
/// board defines the modulation behind it.
enum class Rate : uint8_t
{
  kHigh = 0,
  kLow = 1,
};

/// 启动收发器的参数 / The parameters to start a transceiver with.
struct LinkParams
{
  Rate rate = Rate::kHigh;
  uint32_t frequency_hz = 0;
  uint16_t crc_seed = 0;  ///< CRC 初值，一对探针相同 / CRC seed, the same on both probes
  uint8_t address[4] = {};  ///< 链路地址，一对探针相同 / Link address, the same on both
                            ///< probes
};

/// MaxPayload() 的下限 / Lower bound of MaxPayload().
inline constexpr uint8_t kMinPayload = 127;
/// 等下一包多久 / How long to wait for the next packet.
inline constexpr uint32_t kPacketWaitUs = 3'000;

}  // namespace XrDap::Radio
