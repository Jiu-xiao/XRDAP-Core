// SPDX-License-Identifier: Apache-2.0
// 这个测试程序里读 libxr 微秒时间基的调用都到这里：链接时用 --wrap 换掉（
// tests/CMakeLists.txt）。静态库里 Linux 驱动的定义还在，libxr_system.cpp 要用它，再定义
// 一份会重复。
//
// Every call that reads libxr's microsecond timebase in this test program ends up here:
// the link replaces them with --wrap (tests/CMakeLists.txt). The Linux driver's
// definition stays in the static library because libxr_system.cpp needs it, so a second
// one would clash.
#include "radio_fakes.hpp"
#include "timebase.hpp"

LibXR::MicrosecondTimestamp VirtualMicroseconds() __asm__(
    "__wrap__ZN5LibXR8Timebase15GetMicrosecondsEv");

LibXR::MicrosecondTimestamp VirtualMicroseconds()
{
  return LibXR::MicrosecondTimestamp(RadioTest::VirtualClock::Read());
}
