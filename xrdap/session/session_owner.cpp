// SPDX-License-Identifier: Apache-2.0
#include "xrdap/session/session_owner.hpp"

namespace XrDap
{
namespace
{
size_t Index(Transport t) { return static_cast<size_t>(t); }

/// 作用域锁 / Scoped lock.
class Hold
{
 public:
  explicit Hold(Port::SpinLock& lock) : lock_(lock) { lock_.Lock(); }
  ~Hold() { lock_.Unlock(); }
  Hold(const Hold&) = delete;
  Hold& operator=(const Hold&) = delete;

 private:
  Port::SpinLock& lock_;
};
}  // namespace

void SessionOwner::SetLease(Transport t, uint32_t lease_ms)
{
  Hold hold(lock_);
  lease_ms_[Index(t)] = lease_ms;
}

void SessionOwner::SetUsbResetSource(ResetCountFn fn, void* ctx)
{
  Hold hold(lock_);
  usb_reset_fn_ = fn;
  usb_reset_ctx_ = ctx;
}

bool SessionOwner::Admit(Transport t, uint32_t now_ms)
{
  if (t == Transport::NONE)
  {
    return false;
  }
  Hold hold(lock_);
  ExpireLocked(now_ms);
  if (owner_ == Transport::NONE)
  {
    owner_ = t;
    session_ = next_session_++;
    if (next_session_ == 0U)
    {
      next_session_ = 1U;  // 0 表示没有会话 / 0 means no session
    }
    if (t == Transport::USB && usb_reset_fn_ != nullptr)
    {
      usb_reset_base_ = usb_reset_fn_(usb_reset_ctx_);
    }
  }
  if (owner_ != t)
  {
    return false;
  }
  last_ms_ = now_ms;
  return true;
}

void SessionOwner::Touch(Transport t, uint32_t now_ms)
{
  Hold hold(lock_);
  if (owner_ == t && t != Transport::NONE)
  {
    last_ms_ = now_ms;
  }
}

void SessionOwner::End(Transport t)
{
  Hold hold(lock_);
  if (owner_ == t && t != Transport::NONE)
  {
    EndLocked();
  }
}

bool SessionOwner::Alive(uint32_t session, uint32_t now_ms)
{
  Hold hold(lock_);
  ExpireLocked(now_ms);
  return session != 0U && session == session_;
}

uint32_t SessionOwner::Session() const
{
  Hold hold(lock_);
  return session_;
}

Transport SessionOwner::Owner() const
{
  Hold hold(lock_);
  return owner_;
}

// 主人的租期过了，或 USB 主人的总线复位了：会话结束。时间按无符号差算，跨越 32 位回绕
// 也对。
// The owner's lease ran out, or the USB owner's bus reset: the session ends. Time is
// compared as an unsigned difference, so a 32-bit wrap is handled.
void SessionOwner::ExpireLocked(uint32_t now_ms)
{
  if (owner_ == Transport::NONE)
  {
    return;
  }
  const uint32_t lease = lease_ms_[Index(owner_)];
  if (lease != 0U && now_ms - last_ms_ > lease)
  {
    EndLocked();
    return;
  }
  if (owner_ == Transport::USB && usb_reset_fn_ != nullptr &&
      usb_reset_fn_(usb_reset_ctx_) != usb_reset_base_)
  {
    EndLocked();
  }
}

void SessionOwner::EndLocked()
{
  owner_ = Transport::NONE;
  session_ = 0U;
}

}  // namespace XrDap
