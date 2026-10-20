#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../lib/ESP32-A2DP/src/SourceMediaState.h"
#include "../src/RemoteControl.hpp"
#include <cassert>
#include <cstdio>

using C=SourceMediaState::Command;
static void start(SourceMediaState &media,uint32_t now) {
    assert(media.next(now,true)==C::Check);
    assert(media.acknowledge(C::Check,true,now+1));
    assert(media.next(now+1,true)==C::Start);
    assert(media.acknowledge(C::Start,true,now+2));
    media.observe(true);assert(media.next(now+2,true)==C::None);
}
int main() {
    SourceMediaState media;
    start(media,0);
    assert(media.next(100,false)==C::Suspend);
    assert(media.next(101,true)==C::None); // Do not race START with in-flight SUSPEND.
    media.observe(false);assert(media.next(102,true)==C::None);
    assert(media.acknowledge(C::Suspend,true,103));
    start(media,104);

    // Hold paused across heartbeat/tick calls: no automatic restart or disconnect.
    assert(media.next(200,false)==C::Suspend);
    assert(media.acknowledge(C::Suspend,true,201));
    for(unsigned ms=202;ms<300000;ms+=100)assert(media.next(ms,false)==C::None);
    start(media,300001);

    // STOP during CHECK/START must win once that command completes.
    media.reset();assert(media.next(0,true)==C::Check);
    assert(media.next(1,false)==C::None);
    assert(media.acknowledge(C::Check,true,2));assert(media.next(3,false)==C::None);
    assert(media.next(4,true)==C::Start);assert(media.next(5,false)==C::None);
    assert(media.acknowledge(C::Start,true,6));assert(media.next(6,false)==C::Suspend);
    assert(media.acknowledge(C::Suspend,true,7));assert(media.next(7,false)==C::None);

    // SDK failure/BUSY and immediate API failure back off; a lost ACK retries.
    media.reset();assert(media.next(0,true)==C::Check);
    assert(media.acknowledge(C::Check,false,1));assert(media.next(100,true)==C::None);
    assert(media.next(101,true)==C::Check);assert(!media.acknowledge(C::Start,true,102));
    assert(media.next(1100,true)==C::None);assert(media.next(1101,true)==C::Check);
    assert(media.acknowledge(C::Check,true,1102));assert(media.next(1102,true)==C::Start);
    assert(media.acknowledge(C::Start,false,1103));assert(media.next(1203,true)==C::Check);
    assert(media.acknowledge(C::Check,true,1204));assert(media.next(1204,true)==C::Start);
    assert(media.acknowledge(C::Start,true,1205));assert(media.next(1300,false)==C::Suspend);
    assert(media.acknowledge(C::Suspend,false,1301));assert(media.next(1400,false)==C::None);
    assert(media.next(1401,false)==C::Suspend);assert(media.acknowledge(C::Suspend,true,1402));

    // Disconnect cancels in-flight commands; delayed unrelated ACK cannot start audio.
    media.reset();assert(media.next(0,true)==C::Check);media.reset();
    assert(!media.acknowledge(C::Check,true,1));assert(media.next(2,false)==C::None);
    start(media,3);
    // Wrapping milliseconds must not create a retry storm.
    media.reset();assert(media.next(0xfffffff0u,true)==C::Check);
    assert(media.next(30,true)==C::None);assert(media.next(984,true)==C::Check);

    // The transport-state contract lets a one-button receiver alternate its
    // explicit PAUSE and PLAY, each press/release pair delivered exactly once.
    remote::Inbox inbox;inbox.connected(true);media.reset();start(media,0);
    for(unsigned i=0;i<100;i++) {
        const uint32_t now=100+i*100;
        assert(inbox.key(0x46,false,now));assert(!inbox.key(0x46,false,now));
        assert(!inbox.key(0x46,true,now+1));assert(inbox.pop(now+1)==remote::Action::Pause);
        assert(inbox.pop(now+1)==remote::Action::None);
        assert(media.next(now,false)==C::Suspend);assert(media.acknowledge(C::Suspend,true,now+2));
        assert(inbox.key(0x44,false,now+10));assert(!inbox.key(0x44,true,now+11));
        assert(inbox.pop(now+11)==remote::Action::Play);start(media,now+12);
    }
    std::puts("PASS A2DP media state: pause retains session; CHECK/START/SUSPEND, 100 press/release cycles, 5min pause, rapid reversal, failed/busy/lost/stale ACK, disconnect and clock wrap");
}
