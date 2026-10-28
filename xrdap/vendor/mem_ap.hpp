// SPDX-License-Identifier: Apache-2.0
#pragma once

// 经 SWD 访问 MEM-AP：沿指针链读写目标内存。自己管理 SELECT、CSW、TAR，不走 DAP 的传输
// 流水线。每次访问在一个 Access 里做；Access 结束时让 SWD 的 SELECT 缓存失效，上位机须
// 把 SELECT、CSW、TAR 视为未知。
//
// MEM-AP access over SWD: reads and writes target memory along pointer chains. It manages
// SELECT, CSW and TAR itself, bypassing the DAP transfer pipeline. Every access is made
// inside an Access; the end of the Access invalidates the SWD SELECT cache, so the host must
// treat SELECT, CSW and TAR as unknown.

#include <cstdint>

#include "debug/swd.hpp"
#include "xrdap/vendor/protocol.hpp"

namespace XrDap::Vendor
{

/// 共享 SWD 链路上的 MEM-AP 访问 / MEM-AP access on a shared SWD link.
/// @note 只在执行上下文里用；Access 以外的方法都要在一个 Access 里调用 / Used in the
///       execution context only; every method other than Access is called inside an
///       Access.
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

  /// 一次 SWD 访问的作用域：结束时让 SELECT 缓存失效。SWD 只在执行上下文里访问，所以不加锁 /
  /// Scope of one SWD access: invalidates the SELECT cache at the end. SWD is accessed in the
  /// execution context only, so there is no lock.
  class Access
  {
   public:
    explicit Access(MemAp& mem) : mem_(mem) {}
    ~Access() { mem_.swd_.InvalidateSelectCache(); }
    Access(const Access&) = delete;
    Access& operator=(const Access&) = delete;

   private:
    MemAp& mem_;
  };

  /// @param swd SWD 实现，生命周期需长于本对象 / SWD implementation; must outlive this
  ///        object.
  explicit MemAp(LibXR::Debug::Swd& swd) : swd_(swd) {}

  // 以下方法都在一个 Access 里调用 / Every method below is called inside an Access.

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
  /// @brief 读一个 AP 的 CSW / Read an AP's CSW.
  /// @param ap AP 号 / AP index.
  /// @param csw 读出的 CSW；AHB-AP 的 Prot 位按下面第二条补上 / CSW as read; on an
  ///        AHB-AP the Prot bits are completed as described in the second note.
  /// @return 读到返回 true / true when the CSW was read.
  /// @note 失败时清粘滞错误再试一次：刚重新连接，或上一次访问被打断（例如采样被停掉）
  ///       之后，第一次 AP 访问偶尔会因 DP 里残留的粘滞错误失败 / On failure, clear the
  ///       sticky errors and retry once: right after a reconnect, or after an access was
  ///       cut short (for example sampling being stopped), the first AP access sometimes
  ///       fails on a sticky error left in the DP.
  /// @note AHB-AP 上电后的 Prot 可能是非特权访问，这时访问 PPB（CPUID、DHCSR、AIRCR）
  ///       会出错，所以 AHB-AP 一律改成特权数据访问，与 OpenOCD 的 CSW_AHB_DEFAULT 相同；
  ///       别的 AP 类型这几位含义不同（AXI-AP 是 ARPROT 和缓存属性），保持原值。CSW 已是
  ///       特权访问时不再读 IDR，这一位写过一次就留在 AP 里 / After power-up an AHB-AP's
  ///       Prot field may say unprivileged, and accesses to the PPB (CPUID, DHCSR, AIRCR)
  ///       then fail, so an AHB-AP always gets privileged data access, as in OpenOCD's
  ///       CSW_AHB_DEFAULT. Other AP types use these bits differently (ARPROT and cache
  ///       attributes on an AXI-AP) and keep them. The IDR is not read once the CSW says
  ///       privileged; after one write the bit stays in the AP.
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
  /// No failed target read is replayed. Called inside an Access, count is nonzero.
  uint16_t ReadWords(ApState& ap, uint32_t addr, uint16_t count, uint8_t* out);

 private:
  using Ack = LibXR::Debug::SwdProtocol::Ack;

  /// @brief 判定一次事务是否成功 / Judge whether a transaction succeeded.
  /// @param ec 事务的返回码 / Return code of the transaction.
  /// @param ack 事务的 ack，按引用传入 / The transaction's ack, taken by reference.
  /// @note 实参 `swd_.Xxx(ack)` 和 `ack` 的求值顺序没有规定，传值时不开优化会先读到调用前
  ///       的 NO_ACK（V305-2）/ The evaluation order of `swd_.Xxx(ack)` and `ack` is
  ///       unspecified; passed by value, an unoptimised build reads the NO_ACK from before
  ///       the call (V305-2).
  bool TxnOk(LibXR::ErrorCode ec, const Ack& ack);

  bool SetSize(ApState& st, uint8_t size, bool increment = false);
  bool ReadWord(uint32_t addr, uint32_t& val);
  bool WriteWord(uint32_t addr, uint32_t val);
  /// @brief 检查最近一次写之后 DP 有没有粘滞错误 / Check the DP for sticky errors after
  ///        the last write.
  /// @return 没有错误返回 true；有错误时已清除粘滞位 / true when clean; on an error the
  ///         sticky bits have been cleared.
  /// @note 写是 posted 的，DP 的 CTRL/STAT 读不会等 AP 事务完成：总线故障来晚时这一次读
  ///       看不到，故障被算到下一个条目上（TAR 写回 FAULT），最后一条恢复写也就发不出去。
  ///       所以先读 DP RDBUFF 把这次写等到底（它在写完成前只回 WAIT，写失败时回 FAULT），
  ///       再看 CTRL/STAT 的粘滞错误位 / The write is posted and a DP CTRL/STAT read does
  ///       not wait for the AP transaction: a bus fault that arrives after that read is
  ///       missed and lands on the next item (the TAR write returns FAULT), so the final
  ///       recovery write never happens. DP RDBUFF is read first, which is held off with
  ///       WAIT until the posted write completes and answers FAULT when it failed; then
  ///       the sticky error bits in CTRL/STAT are checked.
  bool CheckCtrlStat();
  void ClearSticky();
  bool Resolve(const Item& it, uint32_t& addr, uint8_t& st);
  uint8_t ReadValue(ApState& ap, uint8_t size, uint32_t addr, uint8_t* out);

  LibXR::Debug::Swd& swd_;
  ApState ap_[kMaxAps];
  uint8_t ap_count_ = 0U;
  /// A transaction failed since the last ResetCache; blocks CSW reuse. Inside an Access.
  bool failed_since_reset_ = false;
};

}  // namespace XrDap::Vendor
