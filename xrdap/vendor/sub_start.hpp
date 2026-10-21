// SPDX-License-Identifier: Apache-2.0
#pragma once

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

#include "xrdap/vendor/vendor_protocol.hpp"

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
