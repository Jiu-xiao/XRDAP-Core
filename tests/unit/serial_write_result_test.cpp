// SPDX-License-Identifier: Apache-2.0
#include "check.hpp"
#include "xrdap/serial/serial_link.hpp"

namespace
{
struct NoExecutor
{
};
using Link = XrDap::SerialLink<NoExecutor, 64U>;
}  // namespace

// 串口链路不带完成通知地写：OK 表示已进队列、一定发出，不重试；FULL 和 BUSY 表示没进
// 队列，重试；别的返回值丢帧。阻塞写的 TIMEOUT 含义不定，链路不用它 / The serial link
// writes without completion notification: OK means queued and it will be sent, no retry;
// FULL and BUSY mean nothing was queued, retry; any other value drops the frame. A
// blocking write's TIMEOUT is ambiguous, and the link does not use it.
TEST(SerialWriteResultsAreUnambiguous)
{
  CHECK_EQ(Link::TxStepFor(LibXR::ErrorCode::OK), Link::TxStep::QUEUED);
  CHECK_EQ(Link::TxStepFor(LibXR::ErrorCode::FULL), Link::TxStep::RETRY);
  CHECK_EQ(Link::TxStepFor(LibXR::ErrorCode::BUSY), Link::TxStep::RETRY);
  CHECK_EQ(Link::TxStepFor(LibXR::ErrorCode::NOT_SUPPORT), Link::TxStep::FAILED);
}
