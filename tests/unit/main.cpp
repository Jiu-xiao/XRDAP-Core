// SPDX-License-Identifier: Apache-2.0
#include "check.hpp"

int main()
{
  int failed_cases = 0;
  for (const auto& c : XrDapTest::Cases())
  {
    const int before = XrDapTest::Failures();
    c.fn();
    const bool ok = XrDapTest::Failures() == before;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", c.name);
    failed_cases += ok ? 0 : 1;
  }
  std::printf("%zu cases, %d failed\n", XrDapTest::Cases().size(), failed_cases);
  return failed_cases == 0 ? 0 : 1;
}
