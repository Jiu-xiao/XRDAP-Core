// SPDX-License-Identifier: Apache-2.0
// 所有公开头文件放在一起能编译（只在 libxr 编译检查里构建）/ All public headers compile
// together (built only by the libxr compile check).
#include "daplink_v2.hpp"
#include "dfu_runtime.hpp"
#include "xrdap/activity/dap_activity.hpp"
#include "xrdap/control/control.hpp"
#include "xrdap/control/usb_control.hpp"
#include "xrdap/light/status_light.hpp"
#include "xrdap/net/tcp_frame.hpp"
#include "xrdap/probe/family.hpp"
#include "xrdap/probe/probe_identity.hpp"
#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/radio/channel.hpp"
#include "xrdap/radio/channel_picker.hpp"
#include "xrdap/radio/forwarding.hpp"
#include "xrdap/radio/host_link.hpp"
#include "xrdap/radio/identity.hpp"
#include "xrdap/radio/packet.hpp"
#include "xrdap/radio/pairing.hpp"
#include "xrdap/radio/radio_link.hpp"
#include "xrdap/radio/radio_port.hpp"
#include "xrdap/radio/remote_link.hpp"
#include "xrdap/radio/session.hpp"
#include "xrdap/radio/settings.hpp"
#include "xrdap/radio/transceiver.hpp"
#include "xrdap/serial/serial_frame.hpp"
#include "xrdap/serial/serial_link.hpp"
#include "xrdap/session/executor.hpp"
#include "xrdap/session/transport.hpp"
#include "xrdap/vendor/dap_vendor.hpp"
#include "xrdap/vendor/mem_ap.hpp"
#include "xrdap/vendor/protocol.hpp"
#include "xrdap/vendor/reset_counting_usb.hpp"
#include "xrdap/vendor/sampler.hpp"
#include "xrdap/vendor/sampling.hpp"
#include "xrdap/vendor/wifi_command.hpp"

// 真正的 libxr DAP 类：执行器对它的调用（包括带长度钩子的 SetCommandHook）都要能编译 /
// The real libxr DAP class: every executor call into it (SetCommandHook with the length
// hook included) must compile.
using RealDap = LibXR::USB::DapLinkV2Class<LibXR::Debug::Swd>;
template class XrDap::Executor<RealDap>;
template class XrDap::Control::UsbControl<RealDap>;
template class XrDap::SerialLink<XrDap::Executor<RealDap>, 64U>;

// 数总线复位的包装能套在端口用的 libxr USB 类上 / The reset-counting wrapper fits the
// libxr USB classes the ports use.
template class XrDap::ResetCountingUsb<RealDap>;
template class XrDap::ResetCountingUsb<LibXR::USB::DfuRuntimeClass>;
