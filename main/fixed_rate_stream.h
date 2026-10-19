#pragma once

#include <cstdint>
#include <cstdlib>

namespace player {

// The BMI270 FIFO is paced by the sensor clock: every frame reaches the
// detector stamped by its index (period_us apart, in whole ms, so 400 Hz
// gives 2 and 3 ms steps), however late the reading task runs. After a
// restart no frame reaches the detector until one whole window has shown the
// sensor delivering its nominal rate (frames counted against the CPU clock).
// Later windows are checked after the fact: a failed one stops the feed and
// the caller resets the detector, but frames already fed stay fed. Lost
// frames are caught earlier, by the HAL's FIFO backlog limit.
template <unsigned RateHz, int LsbPerG>
class FixedRateStream {
public:
    static constexpr uint32_t period_us = 1000000u / RateHz;
    static_assert(period_us * RateHz == 1000000u, "integral frame period");
    static constexpr int64_t window_us = 1000000;
    // Frames per window: the nominal count within 3 % (the sensor clock ran
    // 0.6 % slow against the CPU clock on hardware) plus 12 frames of read
    // latency, i.e. about +-6 % at 400 Hz.
    static constexpr int32_t tolerance_permille = 30;
    static constexpr int32_t slack_frames = 12;

    void restart(uint32_t now_ms) {
        verified_ = window_open_ = false;
        stream_ms_ = now_ms;
        stream_us_ = 0;
        window_frames_ = 0;
    }
    bool verified() const { return verified_; }

    // One FIFO frame (raw counts). Only a verified stream reaches `feed`.
    template <class Frame, class Feed> void push(const Frame &frame, Feed &&feed) {
        ++window_frames_;
        stream_us_ += period_us;
        stream_ms_ += stream_us_ / 1000;
        stream_us_ %= 1000;
        if (!verified_) return;
        constexpr float scale = 1.0f / float(LsbPerG);
        feed(stream_ms_, float(frame.x) * scale, float(frame.y) * scale, float(frame.z) * scale);
    }

    // Call after each drain with the time the FIFO fill level was read for
    // the last transfer: the frames counted are those stored by then. The
    // first call after a restart opens the window, so frames that waited in
    // the FIFO across the restart are not counted.
    // Returns -1 when a window failed, 1 when it passed, 0 otherwise.
    int check_rate(int64_t level_read_us, int32_t *expected_out, int32_t *frames_out) {
        if (!window_open_) {
            window_open_ = true;
            window_start_us_ = level_read_us;
            window_frames_ = 0;
            return 0;
        }
        const int64_t elapsed = level_read_us - window_start_us_;
        if (elapsed < window_us) return 0;
        const int32_t expected = int32_t(elapsed * RateHz / 1000000);
        const int32_t frames = int32_t(window_frames_);
        *expected_out = expected;
        *frames_out = frames;
        const bool ok = std::abs(frames - expected) <= expected * tolerance_permille / 1000 + slack_frames;
        window_start_us_ = level_read_us;
        window_frames_ = 0;
        verified_ = ok;
        return ok ? 1 : -1;
    }

private:
    int64_t window_start_us_ = 0;
    uint32_t stream_ms_ = 0, stream_us_ = 0, window_frames_ = 0;
    bool verified_ = false, window_open_ = false;
};

}  // namespace player
