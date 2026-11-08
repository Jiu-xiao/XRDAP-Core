// SPDX-License-Identifier: Apache-2.0
// 所有 C++ 测试程序共用的入口。不带参数跑全部用例，带一个用例名只跑那一个 / The entry
// point every C++ test program shares. Without arguments it runs every case; with a case
// name it runs that one.
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>

#include "check.hpp"

namespace
{
// 一个用例的时限。产品里有不带超时的等待（Executor::Execute 等应答），测试代码取消
// 不了，到了时限就算这个用例失败，程序结束 / The limit for one case. Production has
// waits without a timeout (Executor::Execute waiting for its reply) that test code cannot
// cancel; past the limit the case counts as failed and the program ends.
constexpr unsigned kCaseLimitSeconds = 60U;

void Write(const char* text)
{
  if (write(STDOUT_FILENO, text, std::strlen(text)) < 0)
  {
    std::_Exit(1);
  }
}

void OnCaseTooLong(int)
{
  Write("FAIL ");
  Write(XrDapTest::CurrentCase().load());
  Write(": still running at the time limit\n");
  std::_Exit(1);
}

void PrintUsage(const char* program)
{
  std::printf("usage: %s [case]\ncases:\n", program);
  for (const auto& c : XrDapTest::Cases())
  {
    std::printf("  %s\n", c.name);
  }
}
}  // namespace

int main(int argc, char** argv)
{
  // 输出进管道时也按行刷新，时限到了也留得下前面的结果 / Flush by line even into a pipe,
  // so the results so far survive the time limit.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--help") == 0))
  {
    PrintUsage(argv[0]);
    return argc > 2 ? 2 : 0;
  }
  const char* only = argc == 2 ? argv[1] : nullptr;
  std::signal(SIGALRM, OnCaseTooLong);
  int ran = 0;
  int failed_cases = 0;
  for (const auto& c : XrDapTest::Cases())
  {
    if (only != nullptr && std::strcmp(only, c.name) != 0)
    {
      continue;
    }
    XrDapTest::CurrentCase() = c.name;
    const int before = XrDapTest::Failures();
    alarm(kCaseLimitSeconds);
    c.fn();
    alarm(0U);
    const bool ok = XrDapTest::Failures() == before;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", c.name);
    ++ran;
    failed_cases += ok ? 0 : 1;
  }
  if (only != nullptr && ran == 0)
  {
    std::printf("no case named %s\n", only);
    PrintUsage(argv[0]);
    return 2;
  }
  std::printf("%d cases, %d failed\n", ran, failed_cases);
  return failed_cases == 0 ? 0 : 1;
}
