// SPDX-License-Identifier: Apache-2.0
#pragma once

// 串口 DAP 链路（docs/protocol.md 第 1 节）：从 libxr 的 UART 读字节，按 Framer
// 拼出请求， 经 Executor 以 Transport::SERIAL 执行，再把响应打包写回。原来是 libxr 的
// DapSerialTransport，帧格式是 XRDAP-Host 的约定，所以搬到这里。工作线程由 Start() 用
// libxr Thread 建；要把执行 DAP 的线程固定到某个核时（例如 ESP32-S3 的专用 GPIO 只在
// 创建它的核上有效），平台用 SetThreadStarter() 提供启动器。没有 RTOS 时可以不建线程，
// 在非中断上下文里直接调用 Feed()。
//
// Serial DAP link (docs/protocol.md section 1): reads bytes from a libxr UART, assembles
// requests with Framer, runs them through Executor as Transport::SERIAL and writes the
// packed responses back. This was libxr's DapSerialTransport; the frame format is an
// XRDAP-Host convention, so it lives here. Start() creates the worker with a libxr
// Thread; when the thread that runs DAP must be pinned to one core (for example ESP32-S3
// dedicated GPIO works only on the core that created it), the platform supplies a
// starter through SetThreadStarter(). Without an RTOS no thread is needed: call Feed()
// directly outside an interrupt.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "libxr_def.hpp"
#include "libxr_rw.hpp"
#include "semaphore.hpp"
#include "thread.hpp"
#include "timebase.hpp"
#include "uart.hpp"
#include "xrdap/serial/serial_frame.hpp"
#include "xrdap/session/transport.hpp"

namespace XrDap
{

/// @brief 串口 DAP 链路 / Serial DAP link.
/// @tparam ExecT 命令执行器（Executor<DapT>）/ Command executor (Executor<DapT>).
/// @tparam MaxPacket DAP 包长上限（DAP_Info 报告的包长）/ Maximum DAP packet size (the
///         packet size DAP_Info reports).
template <typename ExecT, uint16_t MaxPacket>
class SerialLink
{
 public:
  /// 工作线程参数 / Worker thread parameters.
  struct ThreadConfig
  {
    const char* name = "dap_serial";  ///< 线程名 / Thread name.
    size_t stack_depth = 8192U;       ///< 栈字节数 / Stack bytes.
    LibXR::Thread::Priority priority =
        LibXR::Thread::Priority::HIGH;  ///< 优先级 / Priority.
    uint8_t core = 0xFFU;  ///< 核绑定提示，0xFF 表示不关心 / Core hint; 0xFF = any.
  };

  /// @brief 平台线程启动器：建一个运行 entry(arg) 的线程 / Platform thread starter:
  ///        create a thread that runs entry(arg).
  using ThreadStarter = void (*)(void* ctx, const ThreadConfig& config,
                                 void (*entry)(void*), void* arg);

  /// @note 初始化时调用；只保存引用 / Call at initialisation; stores references only.
  SerialLink(ExecT& exec, LibXR::UART& uart) : exec_(exec), uart_(uart) {}

  SerialLink(const SerialLink&) = delete;
  SerialLink& operator=(const SerialLink&) = delete;

  /// @brief 注册平台线程启动器；为空时用 LibXR::Thread / Register the platform thread
  ///        starter; null uses LibXR::Thread.
  /// @note 初始化时、Start() 之前调用 / Call at initialisation, before Start().
  void SetThreadStarter(ThreadStarter starter, void* ctx)
  {
    starter_ = starter;
    starter_ctx_ = ctx;
  }

  /// @brief 启动工作线程 / Start the worker thread.
  /// @return OK；重复启动返回 BUSY；串口不可读写返回 NOT_SUPPORT / OK; BUSY when already
  ///         started; NOT_SUPPORT when the UART cannot read or write.
  /// @note 任何任务，但同一时间只能有一个任务调用 Start 或 Stop / Any task, but only
  ///       one task at a time may call Start or Stop.
  LibXR::ErrorCode Start(const ThreadConfig& config = ThreadConfig())
  {
    if (started_)
    {
      return LibXR::ErrorCode::BUSY;
    }
    if (uart_.read_port_ == nullptr || !uart_.read_port_->Readable() ||
        uart_.write_port_ == nullptr)
    {
      return LibXR::ErrorCode::NOT_SUPPORT;
    }
    stop_.store(0U, std::memory_order_release);
    started_ = true;
    if (starter_ != nullptr)
    {
      starter_(starter_ctx_, config, &SerialLink::Entry, this);
    }
    else
    {
      thread_.Create(static_cast<void*>(this), &SerialLink::Entry, config.name,
                     config.stack_depth, config.priority);
    }
    return LibXR::ErrorCode::OK;
  }

  /// @brief 停止工作线程，最多等 1 s；串口的会话随之结束 / Stop the worker, waiting at
  ///        most 1 s; the serial session ends with it.
  /// @return 工作线程已退出时 OK；1 s 内没退出时 TIMEOUT，这时仍算已启动，Start() 回
  ///         BUSY，不会再建第二个工作线程 / OK once the worker exited; TIMEOUT when it
  ///         did not exit within 1 s, in which case the link still counts as started and
  ///         Start() returns BUSY instead of creating a second worker.
  /// @note 任何任务，不能在工作线程里调用；同一时间只能有一个任务调用 Start 或 Stop /
  ///       Any task except the worker itself; only one task at a time may call Start or
  ///       Stop.
  LibXR::ErrorCode Stop()
  {
    if (!started_)
    {
      return LibXR::ErrorCode::OK;
    }
    stop_.store(1U, std::memory_order_release);
    exec_.LinkLost(Transport::SERIAL);
    if (exited_.Wait(kStopTimeoutMs) != LibXR::ErrorCode::OK)
    {
      return LibXR::ErrorCode::TIMEOUT;
    }
    started_ = false;
    return LibXR::ErrorCode::OK;
  }

  /// @brief 送入已收到的字节，整帧在这里执行并回复 / Feed received bytes; whole frames
  ///        run and are answered here.
  /// @return 这次执行的请求数 / Requests run by this call.
  /// @note 工作线程用它；没有线程时在非中断上下文里调用，同一时间只能有一个调用者 / The
  ///       worker uses it; without a worker call it outside an interrupt, one caller at a
  ///       time.
  uint32_t Feed(const uint8_t* data, size_t len)
  {
    if (data == nullptr || len == 0U)
    {
      return 0U;
    }
    return framer_.Feed(data, len,
                        [this](const uint8_t* req, uint16_t req_len, bool control)
                        { OnRequest(req, req_len, control); });
  }

  /// @brief 等一会儿、取走串口里已有的字节并处理 / Wait briefly, take the bytes queued in
  ///        the UART and process them.
  /// @return 继续运行时 true，收到停止请求时 false / True to keep running, false once a
  ///         stop was requested.
  /// @note 工作线程的循环体，也可以由协作调度的平台在自己的任务里循环调用 / The worker's
  ///       loop body; a cooperatively scheduled platform may also loop it in its own
  ///       task.
  /// @note 先用零长度的阻塞读等“队列非空”（不取字节），再按队列里的量一次取走；直接发
  ///       定长读会等满长度，重新带来几十毫秒的攒批延迟 / Waits for a nonempty queue with
  ///       a zero-length blocking read (it takes no bytes), then takes what is queued in
  ///       one read; a fixed-length read would wait for the whole length and bring back
  ///       tens of milliseconds of batching.
  bool Poll()
  {
    if (stop_.load(std::memory_order_acquire) != 0U)
    {
      return false;
    }
    CheckLease();
    if (uart_.read_port_ == nullptr || !uart_.read_port_->Readable())
    {
      LibXR::Thread::Sleep(1U);
      return true;
    }
    // 零长度读只等队列非空，不取字节 / A zero-length read only waits for a nonempty
    // queue and takes no bytes.
    LibXR::ReadOperation wait_op(wait_sem_, kWaitTimeoutMs);
    const LibXR::ErrorCode wait_ec =
        uart_.Read(LibXR::RawData(static_cast<void*>(nullptr), 0U), wait_op);
    if (wait_ec == LibXR::ErrorCode::TIMEOUT)
    {
      return true;
    }
    if (wait_ec != LibXR::ErrorCode::OK)
    {
      LibXR::Thread::Sleep(1U);
      return true;
    }
    const size_t queued = uart_.read_port_->Size();
    if (queued == 0U)
    {
      return true;
    }
    const size_t take = (queued < kChunkSize) ? queued : kChunkSize;
    LibXR::ReadOperation read_op(read_sem_);
    if (uart_.Read(LibXR::RawData(rx_chunk_, take), read_op) != LibXR::ErrorCode::OK)
    {
      return true;
    }
    (void)Feed(rx_chunk_, take);
    return true;
  }

  /// 一次不带完成通知的写的结果 / Outcome of one write without completion notification.
  enum class TxStep : uint8_t
  {
    QUEUED,  ///< 已复制进队列，会发出 / Copied into the queue; it will be sent.
    RETRY,   ///< 没进队列，稍后重试 / Not queued; retry later.
    FAILED,  ///< 写不了，丢掉这一帧 / Cannot write; the frame is dropped.
  };

  /// @brief 把写端口的返回值归类 / Classify the write port's return value.
  /// @note 纯函数，任何上下文 / Pure function, any context.
  static constexpr TxStep TxStepFor(LibXR::ErrorCode ec)
  {
    if (ec == LibXR::ErrorCode::OK)
    {
      return TxStep::QUEUED;
    }
    if (ec == LibXR::ErrorCode::FULL || ec == LibXR::ErrorCode::BUSY)
    {
      return TxStep::RETRY;
    }
    return TxStep::FAILED;
  }

  /// 串口多久没有请求就当链路丢了；上位机至少每 kLeaseMs 发一个请求（SUB_READ 即可）/
  /// How long without a request before the serial link counts as lost; the host sends a
  /// request (SUB_READ will do) at least every kLeaseMs.
  static constexpr uint32_t kLeaseMs = 5000U;

 private:
  static constexpr size_t kChunkSize = 512U;
  static constexpr uint32_t kWaitTimeoutMs = 50U;
  static constexpr uint32_t kTxTimeoutMs = 200U;
  static constexpr uint32_t kStopTimeoutMs = 1000U;
  using FramerT = Serial::Framer<MaxPacket>;

  static void Entry(void* arg)
  {
    auto* self = static_cast<SerialLink*>(arg);
    while (self->Poll())
    {
    }
    self->exited_.Post();
  }

  // 串口没有连接和断开：kLeaseMs 内没有请求就当链路丢了 / A serial port has no connect or
  // disconnect: no request within kLeaseMs counts as the link being lost.
  void CheckLease()
  {
    if (active_.load() == 0U)
    {
      return;
    }
    const auto now = static_cast<uint32_t>(LibXR::Timebase::GetMilliseconds());
    if (now - last_request_ms_.load() > kLeaseMs)
    {
      active_.store(0U);
      exec_.LinkLost(Transport::SERIAL);
    }
  }

  void OnRequest(const uint8_t* req, uint16_t len, bool control)
  {
    uint16_t out = 0U;
    if (control)
    {
      // 控制请求不算 DAP 链路在用,不续租期 / A control request does not count as the DAP
      // link being in use and does not renew the lease.
      exec_.Control(Transport::SERIAL, req, len, resp_, MaxPacket, out);
    }
    else
    {
      last_request_ms_.store(static_cast<uint32_t>(LibXR::Timebase::GetMilliseconds()));
      active_.store(1U);
      exec_.Execute(Transport::SERIAL, req, len, resp_, MaxPacket, out);
    }
    const size_t frame = FramerT::PackResponse(
        resp_, out, static_cast<uint64_t>(LibXR::Timebase::GetMicroseconds()), tx_frame_,
        control);
    WriteFrame(FramerT::AppendPad(tx_frame_, frame));
  }

  /// @brief 把 tx_frame_ 里的一帧写出，等写端口有空位，最多等 kTxTimeoutMs / Write the
  ///        frame in tx_frame_, waiting for room in the write port for up to
  ///        kTxTimeoutMs.
  /// @param len 帧长 / Frame length.
  /// @note 不带完成通知地写：返回 OK 时这一帧已复制进写端口的队列、一定会发出；FULL、
  ///       BUSY 时什么都没进队列，可以重试。阻塞写的 TIMEOUT 含义不定（可能已排队、也可能
  ///       没交出），重试会重发、当作已发会丢帧，所以不用它 / Writes without completion
  ///       notification: on OK the frame has been copied into the write port's queue and
  ///       will be sent; on FULL or BUSY nothing was queued, so a retry is safe. A
  ///       blocking write's TIMEOUT is ambiguous (maybe queued, maybe not submitted):
  ///       retrying would send twice and assuming sent would lose the frame, so it is not
  ///       used.
  void WriteFrame(size_t len)
  {
    for (uint32_t waited = 0U;; ++waited)
    {
      LibXR::WriteOperation op;
      const LibXR::ErrorCode ec = uart_.Write(LibXR::ConstRawData(tx_frame_, len), op);
      const TxStep step = TxStepFor(ec);
      if (step == TxStep::QUEUED || step == TxStep::FAILED || waited >= kTxTimeoutMs)
      {
        return;
      }
      LibXR::Thread::Sleep(1U);
    }
  }

  ExecT& exec_;
  LibXR::UART& uart_;
  ThreadStarter starter_ = nullptr;
  void* starter_ctx_ = nullptr;

  LibXR::Thread thread_{};
  bool started_ = false;
  std::atomic<uint32_t> stop_{0U};
  std::atomic<uint32_t> last_request_ms_{0U};
  std::atomic<uint32_t> active_{0U};
  LibXR::Semaphore wait_sem_{0U};
  LibXR::Semaphore read_sem_{0U};
  LibXR::Semaphore exited_{0U};

  FramerT framer_;
  uint8_t rx_chunk_[kChunkSize] = {};
  alignas(8) uint8_t resp_[MaxPacket] = {};
  alignas(8) uint8_t tx_frame_[FramerT::kTxCapacity] = {};
};

}  // namespace XrDap
