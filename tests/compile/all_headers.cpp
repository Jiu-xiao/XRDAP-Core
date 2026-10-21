// SPDX-License-Identifier: Apache-2.0
// 每个公开头文件都能单独编译（只在 libxr 编译检查里构建）/ Every public header compiles
// on its own (built only by the libxr compile check).
#include "xrdap/activity/dap_activity.hpp"
#include "xrdap/light/status_light.hpp"
#include "xrdap/probe/chip_uid.hpp"
#include "xrdap/probe/family.hpp"
#include "xrdap/probe/probe_identity.hpp"
#include "xrdap/protocol/dap_ids.hpp"
#include "xrdap/swd/f1_gpio.hpp"
#include "xrdap/vendor/dap_vendor.hpp"
#include "xrdap/vendor/frame_ring.hpp"
#include "xrdap/vendor/mem_ap.hpp"
#include "xrdap/vendor/sampler.hpp"
#include "xrdap/vendor/vendor_protocol.hpp"

// 实例化一个引脚，检查模板本身 / Instantiate one pin to check the template itself.
template class XrDap::F1Gpio<0x40010C00U, 13U>;
