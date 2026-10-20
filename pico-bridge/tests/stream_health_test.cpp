#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/StreamHealth.hpp"
#include "../lib/ESP32-A2DP/src/SourceMediaState.h"
#include <cassert>
#include <cstdio>
using A = StreamHealth::Action;
using C = SourceMediaState::Command;
int main() {
  // 12 hours of PCM, including all-zero samples, counter and millis wrap.
  StreamHealth health;
  uint32_t rx=0xffffff00u, callbacks=0xffffff00u, consumed=0xffff0000u;
  uint32_t now=0xffffff00u;
  for (unsigned i=0;i<12*60*60*10;i++,now+=100) {
    rx+=17;callbacks+=9;consumed+=4410;
    assert(health.update(now,true,true,false,true,rx,callbacks,consumed)==A::None);
  }
  // Bluetooth callbacks freeze while clocks and zero-valued PCM continue.
  bool recovered=false;
  for (unsigned i=0;i<40;i++,now+=100) {
    rx+=17;
    const auto a=health.update(now,true,true,false,true,rx,callbacks,consumed);
    if(a==A::RecoverTransport){assert(!recovered);recovered=true;}
    else assert(a==A::None);
  }
  assert(recovered);
  // Cooldown prevents recovery storms. Resuming counters restores health.
  for(unsigned i=0;i<100;i++,now+=100) {
    rx+=17;callbacks+=9;consumed+=4410;
    assert(health.update(now,true,true,false,true,rx,callbacks,consumed)==A::None);
  }
  // Missing I2S input is reported separately, never treated as BT failure.
  bool input=false;
  for(unsigned i=0;i<40;i++,now+=100) {
    callbacks+=9;
    const auto a=health.update(now,true,true,false,true,rx,callbacks,consumed);
    if(a==A::ReportInputStall){assert(!input);input=true;}
    else assert(a==A::None);
  }
  assert(input);
  for(unsigned i=0;i<3000;i++,now+=100)
    assert(health.update(now,false,true,false,false,rx,callbacks,consumed)==A::None);
  for(unsigned i=0;i<40;i++,now+=100)
    assert(health.update(now,true,false,false,false,rx,callbacks,consumed)==A::None);
  for(unsigned i=0;i<40;i++,now+=100)
    assert(health.update(now,true,true,true,true,rx,callbacks,consumed)==A::None);
  // ACK says START, but the actual audio-start event never arrives.
  health.session();bool eventRecovered=false;
  for(unsigned i=0;i<40;i++,now+=100) {
    rx+=17;
    auto a=health.update(now,true,true,false,false,rx,callbacks,consumed);
    if(a==A::RecoverTransport){assert(!eventRecovered);eventRecovered=true;}
    else assert(a==A::None);
  }
  assert(eventRecovered);
  // A persistent fault gets three attempts, then a latched failure, not a loop.
  health.session();unsigned recoveries=0,failures=0;
  for(unsigned i=0;i<1200;i++,now+=100) {
    rx+=17;
    const auto a=health.update(now,true,true,false,true,rx,callbacks,consumed);
    recoveries+=a==A::RecoverTransport;failures+=a==A::TransportFailed;
    assert(a!=A::ReportInputStall);
  }
  assert(recoveries==3&&failures==1);
  health.session();recoveries=0;
  for(unsigned i=0;i<40;i++,now+=100) {
    rx+=17;
    const auto a=health.update(now,true,true,false,true,rx,callbacks,consumed);
    recoveries+=a==A::RecoverTransport;assert(a!=A::TransportFailed);
  }
  assert(recoveries==1);
  // Recovery queued behind START never sends simultaneous control commands.
  SourceMediaState media;
  assert(media.next(0,true)==C::Check);media.acknowledge(C::Check,true,1);
  assert(media.next(1,true)==C::Start);media.request_recovery();
  assert(media.next(2,true)==C::None);media.acknowledge(C::Start,true,3);
  assert(media.next(3,true)==C::Suspend);media.acknowledge(C::Suspend,true,4);
  assert(media.next(4,true)==C::Check);media.acknowledge(C::Check,true,5);
  assert(media.next(5,true)==C::Start);media.acknowledge(C::Start,true,6);
  media.request_recovery();assert(media.next(7,false)==C::Suspend);
  media.acknowledge(C::Suspend,true,8);assert(media.next(8,false)==C::None);
  media.request_recovery();assert(media.next(9,true)==C::Suspend);
  media.acknowledge(C::Suspend,false,10,true);
  assert(media.next(109,true)==C::None);assert(media.next(110,true)==C::Suspend);
  media.acknowledge(C::Suspend,false,111,false);
  assert(media.next(211,true)==C::Check);
  std::puts("PASS stream health: 12h PCM including silence and counter/timer wrap; callback stall, input stall, stale started state, recovery cooldown; pause/disconnect/test exemption; serialized recovery and pause precedence.");
}
