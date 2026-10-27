// SPDX-License-Identifier: Apache-2.0
#pragma once

// XR 厂商扩展的线上格式和请求检查：命令号、状态码、条目、小端工具，DAP_ExecuteCommands
// 里厂商命令的长度，回复放不放得下，SUB_START 请求的检查。协议见 docs/protocol.md 第 3 节。
//
// Wire format and request checks of the XR vendor extension: command bytes, status codes,
// items, little-endian helpers, the length of a vendor command inside
// DAP_ExecuteCommands, whether a reply fits, and the SUB_START request checks. See
// docs/protocol.md section 3.

// ===== Wire format 线上格式 =====

// XR 厂商扩展的线上格式：命令号、状态码、上限、条目和小端字节序工具。协议见
// docs/protocol.md 第 3 节。
//
// Wire format of the XR vendor extension: command bytes, status codes, limits, items and
// little-endian byte helpers. See docs/protocol.md section 3.

#include <cstddef>
#include <cstdint>

#include "xrdap_port.hpp"

namespace XrDap::Vendor
{

/// 厂商命令号 / Vendor command bytes.
enum class Cmd : uint8_t
{
  INFO = 0x80,       ///< 能力和探针时间 / Capabilities and probe time.
  READ = 0x81,       ///< 多条目读 / Multi-item read.
  WRITE = 0x82,      ///< 多条目写 / Multi-item write.
  SUB_START = 0x83,  ///< 带配置开始采样 / Start sampling with its configuration.
  SUB_CTRL = 0x84,   ///< 停止采样、清空缓冲 / Stop sampling, clear the buffer.
  SUB_READ = 0x85,   ///< 取采样帧 / Read sample frames.
};

/// 内置命令的范围 / Range of the built-in commands.
constexpr uint8_t kFirstCmd = static_cast<uint8_t>(Cmd::INFO);
constexpr uint8_t kLastCmd = static_cast<uint8_t>(Cmd::SUB_READ);

/// 协议版本 / Protocol version.
constexpr uint8_t kVersion = 2;
/// 功能位：READ | WRITE | 采样 | 8 字节访问 / Feature bits: READ | WRITE | sampling |
/// 8-byte access.
constexpr uint32_t kFeatures = 0x0F;
/// 一条命令最多的条目数 / Maximum items per command.
constexpr uint16_t kMaxItems = 32;
/// 指针链最大深度 / Maximum pointer-chain depth.
constexpr uint8_t kMaxDepth = 8;
/// 一条命令里最多用到的 AP 数 / Maximum distinct APs in one command.
constexpr size_t kMaxAps = 8;
/// 采样环形缓冲字节数，由平台在 xrdap_port.hpp 里给出，INFO 会报告 / Sampling ring
/// buffer size in bytes, given by the platform in xrdap_port.hpp and reported by INFO.
constexpr uint32_t kRingBytes = Port::kRingBytes;
static_assert((kRingBytes & (kRingBytes - 1U)) == 0U, "Port::kRingBytes: power of two");
/// 一帧采样数据的最大字节数：节拍号 4 + 时间戳 6 + 状态位图 + 值 / Maximum bytes of one
/// sample frame: tick 4 + timestamp 6 + status bitmap + values.
constexpr uint16_t kMaxFrameBytes = 10U + (kMaxItems + 7U) / 8U + kMaxItems * 8U;

/// INFO 回复的字节数 / Bytes of the INFO reply.
constexpr uint16_t kInfoReplyLen = 22U;
/// SUB_READ 回复头的字节数 / Bytes of the SUB_READ reply head.
constexpr uint16_t kSubReadHeaderLen = 24U;

/// 命令状态：成功 / Command status: success.
constexpr uint8_t kStatusOk = 0x00;
/// 命令状态：参数错误 / Command status: bad argument.
constexpr uint8_t kStatusArg = 0x01;
/// 命令状态：当前状态不允许 / Command status: not allowed in the current state.
constexpr uint8_t kStatusState = 0x02;
/// 命令状态：资源不足 / Command status: insufficient resources.
constexpr uint8_t kStatusResource = 0x03;
/// 命令状态：未连接目标 / Command status: no target connected.
constexpr uint8_t kStatusNoTarget = 0x04;

/// 条目状态：成功 / Item status: success.
constexpr uint8_t kItemOk = 0x00;
/// 条目状态 0x10 + k：第 k 级读指针时 SWD 出错或超时 / Item status 0x10 + k: SWD
/// error or timeout reading the pointer at level k.
constexpr uint8_t kItemSwdChain = 0x10;
/// 条目状态：最后一次读或写时 SWD 失败 / Item status: SWD failed on the final access.
constexpr uint8_t kItemSwdLast = 0x1F;
/// 条目状态 0x20 + k：第 k 级的读地址未 4 字节对齐 / Item status 0x20 + k: the read
/// address at level k is not 4-byte aligned.
constexpr uint8_t kItemAlignChain = 0x20;
/// 条目状态：最后访问的地址未按 size 对齐 / Item status: the final address is not
/// aligned to size.
constexpr uint8_t kItemAlignLast = 0x2F;
/// 条目状态 0x30 + k：第 k 级读出的指针为 0，链终止 / Item status 0x30 + k: the pointer
/// read at level k is 0 and the chain stops.
constexpr uint8_t kItemNull = 0x30;

/// 一个读写或采样条目：沿指针链解引用后访问 size 字节 / One read, write or sampling
/// item: dereference along a pointer chain, then access size bytes.
struct Item
{
  /// AP 号 / AP index.
  uint8_t ap = 0;
  /// 主机分配的条目号 / Host-assigned item id.
  uint16_t id = 0;
  /// 访问字节数，1、2、4 或 8 / Bytes to access: 1, 2, 4 or 8.
  uint8_t size = 0;
  /// 起始地址 / Start address.
  uint32_t base = 0;
  /// 指针链深度，不超过 kMaxDepth / Pointer-chain depth, at most kMaxDepth.
  uint8_t n = 0;
  /// 每级解引用前加的偏移 / Offset added before each dereference.
  int16_t deref[kMaxDepth] = {};
  /// 最后一次访问前加的偏移 / Offset added before the final access.
  int16_t final_off = 0;
};

/// 一个采样条目：在 (tick + phase) % div == 0 的拍读一次 / One sampling item, read on
/// ticks where (tick + phase) % div == 0.
struct SubItem
{
  Item item;
  uint16_t div = 1;
  uint16_t phase = 0;
};

inline void Put16(uint8_t* p, uint16_t v)
{
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

/// 写 bytes 个字节的小端整数 / Write a little-endian integer of `bytes` bytes.
inline void PutLe(uint8_t* p, uint64_t v, int bytes)
{
  for (int i = 0; i < bytes; ++i)
  {
    p[i] = static_cast<uint8_t>(v >> (8 * i));
  }
}

inline void Put32(uint8_t* p, uint32_t v) { PutLe(p, v, 4); }
inline void Put48(uint8_t* p, uint64_t v) { PutLe(p, v, 6); }
inline void Put64(uint8_t* p, uint64_t v) { PutLe(p, v, 8); }

inline uint16_t Get16(const uint8_t* p)
{
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t Get32(const uint8_t* p)
{
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// @brief 填只有状态的回复（resp[0] 已是命令号）/ Fill a status-only reply (resp[0]
///        already holds the command byte).
/// @return 总是 true，方便 `return Reply(...)` / Always true, for `return Reply(...)`.
inline bool Reply(uint8_t* resp, uint8_t status, uint16_t& out)
{
  resp[1] = status;
  out = 2U;
  return true;
}

/// 访问字节数是否合法 / Whether an access size is valid.
inline bool SizeValid(uint8_t size)
{
  return size == 1U || size == 2U || size == 4U || size == 8U;
}

/// @brief 解析一个条目，成功时 p 前进到条目之后 / Parse one item; on success p moves past
///        it.
/// @param sub 不为空时条目后还有 div 和 phase（采样条目）/ When not null, div and phase
///        follow the item (sampling item).
/// @return 长度不足或深度超限时返回 false / False when truncated or too deep.
inline bool ParseItem(const uint8_t*& p, const uint8_t* end, Item& it, SubItem* sub)
{
  if (static_cast<size_t>(end - p) < 9U)
  {
    return false;
  }
  it.ap = p[0];
  it.id = Get16(&p[1]);
  it.size = p[3];
  it.base = Get32(&p[4]);
  it.n = p[8];
  if (it.n > kMaxDepth)
  {
    return false;
  }
  p += 9;
  if (static_cast<size_t>(end - p) < static_cast<size_t>(it.n) * 2U + 2U)
  {
    return false;
  }
  for (uint8_t i = 0; i < it.n; ++i)
  {
    it.deref[i] = static_cast<int16_t>(Get16(p));
    p += 2;
  }
  it.final_off = static_cast<int16_t>(Get16(p));
  p += 2;
  if (sub != nullptr)
  {
    if (static_cast<size_t>(end - p) < 4U)
    {
      return false;
    }
    sub->item = it;
    sub->div = Get16(p);
    sub->phase = Get16(p + 2);
    p += 4;
  }
  return true;
}

/// @brief 已校验过的一串采样条目，留在请求字节里按需解析：32 个条目拷成数组要 1 KB 以上，
///        小 RAM 的探针栈放不下 / A validated run of sampling items, left in the request
///        bytes and parsed on demand: copying 32 items into an array takes more than
///        1 KB, too much for the stack of a small-RAM probe.
struct SubItemList
{
  const uint8_t* begin;
  const uint8_t* end;
  uint16_t count;

  /// 依次解析每个条目并调用 fn(const SubItem&) / Parse each item in turn and call
  /// fn(const SubItem&).
  template <typename Fn>
  void ForEach(Fn&& fn) const
  {
    const uint8_t* p = begin;
    for (uint16_t i = 0; i < count; ++i)
    {
      Item it;
      SubItem sub;
      (void)ParseItem(p, end, it, &sub);
      fn(sub);
    }
  }
};

}  // namespace XrDap::Vendor

// ===== Request length 请求长度 =====

// 厂商命令请求的长度（docs/protocol.md 3.2）。DAP_ExecuteCommands 和 DAP_QueueCommands
// 把多条命令首尾相接放在一个包里，没有长度字段，CMSIS-DAP 要求每条命令的长度能从它自己
// 的内容得出。标准命令由 libxr 计算，0x80 到 0x85 由这里计算，平台挂的附加命令自己算，libxr 经长度钩子来问。
//
// Request length of vendor commands (docs/protocol.md 3.2). DAP_ExecuteCommands and
// DAP_QueueCommands place several commands back to back in one packet with no length
// field; CMSIS-DAP requires every command's length to follow from its own content.
// libxr works out standard commands, this works out 0x80 to 0x85, a platform works out its
// own extra command, and libxr asks through its length hook.

#include <cstdint>

namespace XrDap::Vendor
{

/// @brief 算出 req 开头这条厂商命令的请求字节数 / Work out the request bytes of the
///        vendor command at the start of req.
/// @param avail req 里可用的字节数 / Bytes available in req.
/// @param len 成功时为这条命令的字节数，不超过 avail / On success, the bytes of this
///        command, at most avail.
/// @return 命令号不是 0x80 到 0x85 或内容不完整时 false / False when the command is not
///         0x80 to 0x85 or its content is incomplete.
/// @note 纯函数，任何上下文 / Pure function, any context.
bool RequestLength(const uint8_t* req, uint16_t avail, uint16_t& len);

}  // namespace XrDap::Vendor

// ===== Reply space 回复空间 =====

// 厂商命令的回复空间检查，所有命令共用一处（docs/protocol.md 3.2）。执行前先算出这条
// 命令的回复要多少字节：空间不到 2 字节时只回一个 0xFF，不够整条回复时回
// [命令号, 0x03]，都不执行命令，也不写出给定的空间。DAP_ExecuteCommands 里排在后面的
// 命令剩下的空间可能只有 1 字节。
//
// Reply space check for vendor commands, one place for all of them (docs/protocol.md
// 3.2). Before a command runs, the bytes its reply needs are worked out: with less than
// 2 bytes of space the reply is 0xFF alone, with less than the whole reply it is
// [command, 0x03]; the command does not run in either case and nothing is written past
// the given space. A command late in a DAP_ExecuteCommands batch may have only 1 byte
// left.

#include <cstdint>

namespace XrDap::Vendor
{

/// @brief 命令 0x80 到 0x85 的回复最多要多少字节；请求本身不合法时为 2（只回状态）/
///        Bytes the reply of command 0x80 to 0x85 needs at most; 2 (status only) when
///        the request itself is malformed.
/// @note 纯函数，任何上下文 / Pure function, any context.
uint16_t RequiredReply(const uint8_t* req, uint16_t len);

/// @brief 检查回复空间，不够时写好回复 / Check the reply space and write the reply when
///        it is short.
/// @param need 这条命令的回复要的字节数，至少 2 / Bytes this command's reply needs, at
///        least 2.
/// @return 空间够、可以执行时 true / True when there is room and the command may run.
/// @note 纯函数，任何上下文 / Pure function, any context.
bool ReserveReply(uint8_t cmd, uint16_t need, uint8_t* resp, uint16_t cap, uint16_t& out);

}  // namespace XrDap::Vendor

// ===== SUB_START checks SUB_START 检查 =====

// SUB_START（0x83）请求的检查（docs/protocol.md 3.4）：一条命令带全部配置，检查通过就
// 开始采样，探针不保存“已配置未开始”的状态。这里只看请求本身是否合法、放不放得下，不估计
// 耗时；能否跟上在运行时由跳拍、丢帧和缓冲占用判断。
//
// Check of a SUB_START (0x83) request (docs/protocol.md 3.4): one command carries the
// whole configuration and sampling starts once it passes; the probe keeps no "configured
// but not started" state. Only the request itself is checked (valid, fits); no time is
// estimated. Whether sampling keeps up is judged at run time from skipped ticks, dropped
// frames and ring fill.

#include <cstdint>

#include "xrdap/vendor/protocol.hpp"

namespace XrDap::Vendor
{

/// SUB_START 请求的内容 / Contents of a SUB_START request.
struct SubStartRequest
{
  uint32_t period_us = 0U;    ///< 周期 / Period.
  bool stop_on_full = false;  ///< 缓冲满时丢新帧 / Drop new frames when full.
  SubItemList items{nullptr, nullptr, 0U};  ///< 条目，仍在请求字节里 / Items, still in
                                            ///< the request bytes.
};

/// @brief 检查 SUB_START 请求 / Check a SUB_START request.
/// @return kStatusOk 时 out 有效；否则 kStatusArg 或 kStatusResource / out is valid on
///         kStatusOk; otherwise kStatusArg or kStatusResource.
/// @note 纯函数，任何上下文 / Pure function, any context.
uint8_t ParseSubStart(const uint8_t* req, uint16_t len, SubStartRequest& out);

}  // namespace XrDap::Vendor
