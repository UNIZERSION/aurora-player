#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
// Next/Previous transport policy (main/transport_policy.h).
#include "transport_policy.h"

#include <cassert>
#include <cstdio>

int main()
{
    const auto keeps = [](audio_state_t state, bool resume_pending = false) {
        return player::skip_keeps_paused(state, resume_pending, false, false);
    };
    // No load outstanding: a paused, stopped or finished track stays paused.
    assert(keeps(AUDIO_PAUSED) && keeps(AUDIO_STOPPED) && keeps(AUDIO_ENDED));
    // Playback under way keeps playing, including a second quick press while
    // the first new track is LOADING or a seek is running (audit 1.3.18).
    assert(!keeps(AUDIO_PLAYING) && !keeps(AUDIO_LOADING) && !keeps(AUDIO_SEEKING));
    // Play pressed on a paused track, not yet confirmed by the engine.
    assert(!keeps(AUDIO_PAUSED, true));
    // After a decode error the user asked for another track: play it.
    assert(!keeps(AUDIO_ERROR));

    // A load is outstanding and the cached state still describes the previous
    // track (one loop behind). Its own intent decides, whatever that state is.
    const audio_state_t stale[] = {AUDIO_STOPPED, AUDIO_LOADING, AUDIO_PLAYING, AUDIO_PAUSED,
                                   AUDIO_SEEKING, AUDIO_ENDED, AUDIO_ERROR};
    for (auto state : stale) {
        // ENDED -> automatic next starts N+1 playing -> manual Next: N+2 plays.
        // STOPPED/PAUSED -> Play -> quick Next: the new track plays.
        assert(!player::skip_keeps_paused(state, false, true, false));
        assert(!player::skip_keeps_paused(state, true, true, false));
        // A paused load, or Pause pressed while LOADING: the next track stays paused.
        assert(player::skip_keeps_paused(state, false, true, true));
    }
    std::puts("PASS: Next/Previous keep paused only for PAUSED/STOPPED/ENDED or a paused outstanding load; "
              "LOADING, SEEKING, pending resume, ERROR and outstanding playing loads play");
    return 0;
}
