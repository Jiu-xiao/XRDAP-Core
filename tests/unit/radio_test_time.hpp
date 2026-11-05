// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "linux_timebase.hpp"

// 无线测试共用的时间基：构造会重置起点，所以整个程序只有一个 / The timebase the radio
// tests share: constructing one resets the reference point, so the program has only one.
inline LibXR::LinuxTimebase& RadioTestTime()
{
  static LibXR::LinuxTimebase timebase;
  return timebase;
}
