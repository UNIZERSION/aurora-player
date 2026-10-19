#pragma once

#include "audio_engine.h"

namespace player {

// Next/Previous keep the transport as the user left it. The cached engine
// state can lag one loop behind a request, so an outstanding load decides by
// its own intent (pending_paused): paused load, or Play/Pause pressed while it
// was loading. Otherwise a paused load sets AUDIO_PAUSED at once, LOADING and
// SEEKING mean playback is under way, a pending resume counts as playing, and
// ERROR plays because the user asked for another track.
inline bool skip_keeps_paused(audio_state_t state, bool resume_pending, bool play_pending, bool pending_paused)
{
    if (play_pending) return pending_paused;
    return (state == AUDIO_PAUSED && !resume_pending) || state == AUDIO_STOPPED || state == AUDIO_ENDED;
}

}  // namespace player
