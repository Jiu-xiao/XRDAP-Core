// SPDX-License-Identifier: Apache-2.0
// RadioLink 的配对：一对探针，射频是内存里的假收发器。判据：identity.hpp 和
// docs/protocol.md 第 8.2 节 / Pairing through RadioLink: a pair of probes over fake
// transceivers in memory. Oracle: identity.hpp and docs/protocol.md section 8.2.
#include <cstring>

#include "check.hpp"
#include "radio_fakes.hpp"

using namespace XrDap;
using namespace XrDap::Radio;
using namespace RadioTest;

// 两块未配对的探针在配对的 10 s 内得到同一个链路号并保存 / Two unpaired probes end with
// the same link id, stored, within the 10 s of pairing.
TEST(radio_link_pairing)
{
  Pair p(false);
  p.radio_a.on_poll = nullptr;
  p.a.RequestPairing();
  p.b.RequestPairing();
  const uint32_t begin = NowUs();
  while ((p.a.Pairing() == PairState::IDLE || p.a.Pairing() == PairState::RUNNING ||
          p.b.Pairing() == PairState::IDLE || p.b.Pairing() == PairState::RUNNING) &&
         NowUs() - begin < 10'000'000U)
  {
    (void)p.a.Service();
    (void)p.b.Service();
  }
  CHECK(p.a.Pairing() == PairState::DONE && p.b.Pairing() == PairState::DONE);
  CHECK(p.store_a.saved.paired == 1 && p.store_b.saved.paired == 1);
  CHECK(p.store_a.saved.link_id == LinkId(kUidA, kUidB));
  CHECK(p.store_b.saved.link_id == p.store_a.saved.link_id);
  CHECK(std::memcmp(p.store_a.saved.peer_uid, kUidB, kUidSize) == 0);
  CHECK(p.a.CurrentRole() == Role::REMOTE);  // 配对后设置生效 / settings applied
}
