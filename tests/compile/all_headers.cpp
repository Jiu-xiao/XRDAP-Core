// SPDX-License-Identifier: Apache-2.0
// 每个公开头文件都能单独编译（只在 libxr 编译检查里构建）/ Every public header compiles
// on its own (built only by the libxr compile check).
#include "xrdap/activity/dap_activity.hpp"
#include "xrdap/light/status_light.hpp"
#include "xrdap/probe/chip_uid.hpp"
#include "xrdap/probe/family.hpp"
#include "xrdap/probe/probe_identity.hpp"
#include "xrdap/protocol/dap_ids.hpp"
#include "xrdap/session/busy_reply.hpp"
#include "xrdap/session/executor.hpp"
#include "xrdap/session/session_owner.hpp"
#include "xrdap/swd/f1_gpio.hpp"
#include "xrdap/vendor/dap_vendor.hpp"
#include "xrdap/vendor/frame_ring.hpp"
#include "xrdap/vendor/mem_ap.hpp"
#include "xrdap/vendor/sampler.hpp"
#include "xrdap/vendor/vendor_protocol.hpp"

// 实例化一个引脚，检查模板本身 / Instantiate one pin to check the template itself.
template class XrDap::F1Gpio<0x40010C00U, 13U>;

namespace
{
// 有 Executor 用到的 libxr DAP 接口的最小类型 / Minimal type with the libxr DAP
// interface Executor uses.
struct FakeDap
{
  using Hook = bool (*)(void*, const uint8_t*, uint16_t, uint8_t*, uint16_t, uint16_t&);
  void SetCommandHook(Hook, void*) {}
  LibXR::ErrorCode Step(uint32_t) { return LibXR::ErrorCode::OK; }
  LibXR::ErrorCode ExecuteExternal(const uint8_t*, uint16_t, uint8_t*, uint16_t,
                                   uint16_t& out)
  {
    out = 0U;
    return LibXR::ErrorCode::OK;
  }
};
}  // namespace

template class XrDap::Executor<FakeDap>;
