// SPDX-License-Identifier: Apache-2.0
#pragma once

// 探针的控制口：设置类请求（Wi-Fi、射频）与 DAP 无关，不占 DAP 的命令号。一个请求一个应答：
//   请求  op(1) + 载荷
//   应答  op(1) + 状态(1) + 载荷
// 请求和 DAP 命令一样由执行上下文一次执行一条（Executor::Control），所以处理函数不需要
// 锁，与采样、DAP 命令串行。协议见 DESIGN-control-channel.md。
//
// The probe's control port: settings requests (Wi-Fi, radio) are not DAP and take no DAP
// command number. One request, one answer:
//   request  op(1) + payload
//   answer   op(1) + status(1) + payload
// Like a DAP command, a request is run by the execution context, one at a time
// (Executor::Control), so a handler needs no lock and is serialised with sampling and DAP
// commands.

#include <cstdint>

namespace XrDap::Control
{

/// 应答里的状态 / Status in an answer.
inline constexpr uint8_t kStatusOk = 0U;
inline constexpr uint8_t kStatusBadRequest = 1U;   ///< 请求格式不对 / Malformed request.
inline constexpr uint8_t kStatusUnsupported = 2U;  ///< 没有这个操作 / No such operation.

/// @brief 处理一个控制请求，返回应答长度 / Handle one control request and return the answer
///        length.
/// @param req 请求，第一个字节是 op / The request; its first byte is the op.
/// @param resp 应答缓冲，应答以 op 开头、第二个字节是状态 / Answer buffer; the answer starts
///        with the op and its second byte is the status.
/// @note 在执行上下文里调用 / Called in the execution context.
using Handler = uint16_t (*)(void* ctx, const uint8_t* req, uint16_t len, uint8_t* resp,
                             uint16_t cap);

/// @brief 只有 op 和状态的应答 / An answer with only the op and the status.
/// @return 应答长度，缓冲放不下时 0 / The answer length, 0 when the buffer is too small.
inline uint16_t StatusOnly(uint8_t op, uint8_t status, uint8_t* resp, uint16_t cap)
{
  if (resp == nullptr || cap < 2U)
  {
    return 0U;
  }
  resp[0] = op;
  resp[1] = status;
  return 2U;
}

}  // namespace XrDap::Control
