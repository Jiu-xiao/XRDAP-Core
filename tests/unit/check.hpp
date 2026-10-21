// SPDX-License-Identifier: Apache-2.0
#pragma once

// 最小的测试框架：TEST 注册用例，CHECK 失败时记下文件和行号并继续 / Minimal test
// harness: TEST registers a case, CHECK records file and line on failure and goes on.

#include <cstdio>
#include <functional>
#include <vector>

namespace XrDapTest
{

struct Case
{
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& Cases()
{
  static std::vector<Case> cases;
  return cases;
}

inline int& Failures()
{
  static int failures = 0;
  return failures;
}

struct Register
{
  Register(const char* name, void (*fn)()) { Cases().push_back({name, fn}); }
};

}  // namespace XrDapTest

#define XRDAP_CAT2(a, b) a##b
#define XRDAP_CAT(a, b) XRDAP_CAT2(a, b)
#define TEST(name)                                                \
  static void name();                                             \
  static XrDapTest::Register XRDAP_CAT(reg_, name)(#name, &name); \
  static void name()
#define CHECK(cond)                                                 \
  do                                                                \
  {                                                                 \
    if (!(cond))                                                    \
    {                                                               \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++XrDapTest::Failures();                                      \
    }                                                               \
  } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))
