// SPDX-License-Identifier: Apache-2.0
#pragma once

// 经 SWD 访问 MEM-AP：沿指针链读写目标内存。自己管理 SELECT、CSW、TAR，不走 DAP 的传输
// 流水线。命令和采样共用一条 SWD 链路，访问前要持有 Lock；Lock 释放时让 SWD 的 SELECT
// 缓存失效，上位机须把 SELECT、CSW、TAR 视为未知。
//
// MEM-AP access over SWD: reads and writes target memory along pointer chains. It manages
// SELECT, CSW and TAR itself, bypassing the DAP transfer pipeline. Commands and sampling
// share one SWD link and must hold a Lock while accessing it; releasing the Lock
// invalidates the SWD SELECT cache, so the host must treat SELECT, CSW and TAR as
// unknown.

#include <cstdint>

#include "debug/swd.hpp"
#include "mutex.hpp"
#include "xrdap/vendor/vendor_protocol.hpp"

namespace XrDap::Vendor
{

/// 共享 SWD 链路上的 MEM-AP 访问 / MEM-AP access on a shared SWD link.
/// @note 在 DAP 执行上下文或采样的执行上下文里用；Lock 以外的方法都要求调用方持有
///       Lock / Used in the DAP execution context or the sampling context; every method
///       other than Lock requires the caller to hold a Lock.
class MemAp
{
 public:
  /// 一个 AP 的 CSW 状态 / CSW state of one AP.
  struct ApState
  {
    /// AP 号；0xFF 表示还没有选 AP / AP index; 0xFF before any AP is chosen.
    uint8_t ap = 0xFF;
    /// 读出的 CSW 原值，保留 Size、AddrInc 以外的位 / CSW as read; bits other than Size
    /// and AddrInc are preserved.
    uint32_t base_csw = 0;
    /// base_csw 读成功 / base_csw was read successfully.
    bool valid = false;
    /// 最近一次写入的 CSW，用来省掉重复写 / Last CSW written, to skip repeated writes.
    uint32_t written_csw = 0;
    /// written_csw 有效 / written_csw is valid.
    bool written = false;
  };

  /// 持有 SWD 链路的作用域锁 / Scoped lock on the SWD link.
  class Lock
  {
   public:
    /// 取得链路，等到别的持有者释放为止 / Take the link, waiting for any other holder.
    explicit Lock(MemAp& mem) : mem_(mem) { mem_.mutex_.Lock(); }
    /// 释放链路并让 SELECT 缓存失效 / Release the link and invalidate the SELECT cache.
    ~Lock()
    {
      mem_.swd_.InvalidateSelectCache();
      mem_.mutex_.Unlock();
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

   private:
    MemAp& mem_;
  };

  /// @param swd SWD 实现，生命周期需长于本对象 / SWD implementation; must outlive this
  ///        object.
  explicit MemAp(LibXR::Debug::Swd& swd) : swd_(swd) {}

  // 以下方法都要求调用方持有 Lock / Every method below requires the caller to hold a
  // Lock.

  /// @brief 开始一条命令时清除 AP 写入状态 / Clear AP write state at command start.
  /// @param reuse_csw 采样独占 SWD 时保留已读成功的 CSW 基值；发生过传输失败时仍清空。
  ///        Keep successful CSW base values while sampling owns SWD exclusively;
  ///        any intervening transfer failure still clears them.
  /// @note 每次都清除 written，命令须重新设置硬件访问宽度和自增方式。目标数据不缓存。
  ///       Always clears written: access width and increment mode must be programmed
  ///       again. Target data is never cached.
  void ResetCache(bool reuse_csw = false);
  /// @brief 取得 AP 状态；没有有效 CSW 基值时读取硬件 / Get an AP's state, reading
  ///        hardware when no valid CSW base is cached. Reuse follows ResetCache.
  /// @return 读不到 CSW 时返回 nullptr；缓存满时复用最后一格 / nullptr when the CSW
  ///         cannot be read; the last slot is reused when the cache is full.
  ApState* EnsureAp(uint8_t ap);
  /// 读一个 AP 的 CSW，失败时清粘滞错误再试一次 / Read an AP's CSW; on failure clear the
  /// sticky errors and retry once.
  bool ReadCsw(uint8_t ap, uint32_t& csw);
  /// Restore SWD framing and clear DP errors; never resets or halts the target CPU.
  bool Resync();
  /// 读一个条目，返回条目状态 / Read one item; returns the item status.
  uint8_t Read(const Item& it, uint8_t* out);
  /// 写一个条目，返回条目状态 / Write one item; returns the item status.
  uint8_t Write(const Item& it, const uint8_t* value);
  /// 用给定的 AP 状态读一个条目（采样用），返回条目状态 / Read one item with a given AP
  /// state (used by sampling); returns the item status.
  uint8_t ReadItem(ApState& ap, const Item& it, uint8_t* out);

  /// Read consecutive aligned u32 words within one 1 KiB TAR window. Returns
  /// the number of confirmed values; the remaining values must be marked failed.
  /// No failed target read is replayed. Caller holds Lock, count is nonzero.
  uint16_t ReadWords(ApState& ap, uint32_t addr, uint16_t count, uint8_t* out);

 private:
  using Ack = LibXR::Debug::SwdProtocol::Ack;

  // Bind ack by reference: the transaction argument must finish updating it before
  // its value is inspected, regardless of the compiler's argument evaluation order.
  bool TxnOk(LibXR::ErrorCode ec, const Ack& ack);

  bool SetSize(ApState& st, uint8_t size, bool increment = false);
  bool ReadWord(uint32_t addr, uint32_t& val);
  bool WriteWord(uint32_t addr, uint32_t val);
  bool CheckCtrlStat();
  void ClearSticky();
  bool Resolve(const Item& it, uint32_t& addr, uint8_t& st);
  uint8_t ReadValue(ApState& ap, uint8_t size, uint32_t addr, uint8_t* out);

  LibXR::Debug::Swd& swd_;
  LibXR::Mutex mutex_;
  ApState ap_[kMaxAps];
  uint8_t ap_count_ = 0U;
  /// A transaction failed since the last ResetCache; blocks CSW reuse. Under Lock.
  bool failed_since_reset_ = false;
};

}  // namespace XrDap::Vendor
