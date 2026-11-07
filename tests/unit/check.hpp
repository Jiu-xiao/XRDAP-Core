// SPDX-License-Identifier: Apache-2.0
#pragma once

// 最小的测试框架：TEST 注册用例；CHECK 和 CHECK_EQ 失败时记下用例、文件和行号并继续，
// 可以在任何线程里调用 / Minimal test harness: TEST registers a case; CHECK and CHECK_EQ
// record the case, file and line on failure and go on, and may be called from any thread.

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <type_traits>
#include <utility>
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

inline std::atomic<int>& Failures()
{
  static std::atomic<int> failures{0};
  return failures;
}

// 正在跑的用例 / The case now running.
inline std::atomic<const char*>& CurrentCase()
{
  static std::atomic<const char*> name{"?"};
  return name;
}

struct Register
{
  Register(const char* name, void (*fn)()) { Cases().push_back({name, fn}); }
};

template <typename T>
inline constexpr bool kIntegerLike =
    std::is_integral_v<T> || (std::is_enum_v<T> && std::is_convertible_v<T, long long>);

// 整数按值比较，不管有没有符号；一元加号把布尔和字符提升成 cmp_equal 接受的类型 /
// Integers compare by value whatever their signs; unary plus promotes bool and char to
// types cmp_equal accepts.
template <typename A, typename B>
bool Equal(const A& a, const B& b)
{
  if constexpr (kIntegerLike<A> && kIntegerLike<B>)
  {
    return std::cmp_equal(+a, +b);
  }
  else
  {
    return a == b;
  }
}

// 把一个值写成文字；不认识的类型写成问号 / Writes a value as text; a type it does not
// know becomes a question mark.
template <typename T>
void Format(const T& v, char* text, size_t cap)
{
  if constexpr (std::is_same_v<T, bool>)
  {
    std::snprintf(text, cap, "%s", v ? "true" : "false");
  }
  else if constexpr (std::is_enum_v<T>)
  {
    Format(static_cast<std::underlying_type_t<T>>(v), text, cap);
  }
  else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
  {
    std::snprintf(text, cap, "%lld", static_cast<long long>(v));
  }
  else if constexpr (std::is_integral_v<T>)
  {
    const auto u = static_cast<unsigned long long>(v);
    std::snprintf(text, cap, "%llu (0x%llX)", u, u);
  }
  else if constexpr (std::is_floating_point_v<T>)
  {
    std::snprintf(text, cap, "%g", static_cast<double>(v));
  }
  else if constexpr (std::is_pointer_v<T>)
  {
    std::snprintf(text, cap, "%p", reinterpret_cast<const void*>(v));
  }
  else
  {
    std::snprintf(text, cap, "?");
  }
}

template <typename A, typename B>
void CheckEq(const A& a, const B& b, const char* a_text, const char* b_text,
             const char* file, int line)
{
  if (Equal(a, b))
  {
    return;
  }
  char a_value[48];
  char b_value[48];
  Format(a, a_value, sizeof(a_value));
  Format(b, b_value, sizeof(b_value));
  std::printf("  FAIL %s %s:%d: %s == %s: %s vs %s\n", CurrentCase().load(), file, line,
              a_text, b_text, a_value, b_value);
  ++Failures();
}

}  // namespace XrDapTest

#define TEST(name)                                     \
  static void name();                                  \
  static XrDapTest::Register reg_##name(#name, &name); \
  static void name()
#define CHECK(cond)                                                                   \
  do                                                                                  \
  {                                                                                   \
    if (!(cond))                                                                      \
    {                                                                                 \
      std::printf("  FAIL %s %s:%d: %s\n", XrDapTest::CurrentCase().load(), __FILE__, \
                  __LINE__, #cond);                                                   \
      ++XrDapTest::Failures();                                                        \
    }                                                                                 \
  } while (0)
#define CHECK_EQ(a, b) XrDapTest::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)
