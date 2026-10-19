#ifdef NDEBUG
#undef NDEBUG
#endif
// Host test for main/fixed_rate_stream.h: frame stamping and the sensor rate
// window that gates the tap detector (sleep_tap_input.cpp).
#include "fixed_rate_stream.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
struct Frame { int16_t x, y, z; };
using Stream = player::FixedRateStream<400, 4096>;
struct Fed { uint32_t ms; float x; };

// Pushes `frames` frames, the FIFO being read every 20 ms of `clock_us`
// (sensor running at `ratio` of nominal), and checks the window after each
// drain. Returns the window results seen.
struct Sim {
    Stream s;
    std::vector<Fed> fed;
    int64_t clock_us = 0;
    double sensor_frames = 0;
    int16_t next = 0;
    std::vector<int> windows;
    void run(int64_t duration_us, double ratio = 1.0, int64_t stall_at = -1, int64_t stall_us = 0, unsigned drop = 0) {
        const int64_t end = clock_us + duration_us;
        while (clock_us < end) {
            clock_us += 20000;
            if (stall_at >= 0 && clock_us >= stall_at && stall_us) { clock_us += stall_us; stall_us = 0; }
            const double due = sensor_frames + (20000.0 * ratio) / 2500.0;
            unsigned n = unsigned(due) - unsigned(sensor_frames);
            sensor_frames = due;
            const unsigned lost = n < drop ? n : drop;  // a hole of `drop` frames
            n -= lost;
            drop -= lost;
            for (unsigned k = 0; k < n; ++k) {
                const Frame f{next++, 0, 4096};
                s.push(f, [&](uint32_t ms, float x, float, float) { fed.push_back({ms, x}); });
            }
            int32_t e = 0, g = 0;
            const int r = s.check_rate(clock_us, &e, &g);
            if (r) windows.push_back(r);
        }
    }
};
}  // namespace

int main()
{
    {   // Nothing reaches the detector before one passing window.
        Sim m;
        m.s.restart(1000);
        m.run(990000);
        assert(m.fed.empty() && !m.s.verified());
        m.run(100000);
        assert(m.windows.size() == 1 && m.windows[0] == 1 && m.s.verified());
        const size_t before = m.fed.size();
        m.run(200000);
        assert(m.fed.size() > before + 70);
    }
    {   // Stamps: 400 Hz frames in whole ms, 2 and 3 ms steps, 2.5 ms average,
        // raw counts scaled to g.
        Stream s;
        s.restart(5000);
        int32_t e, g;
        s.check_rate(0, &e, &g);
        for (int k = 0; k < 400; ++k) s.push(Frame{0, 0, 0}, [](uint32_t, float, float, float) {});
        assert(s.check_rate(1000000, &e, &g) == 1 && e == 400 && g == 400);
        std::vector<uint32_t> ms;
        float x = 0;
        for (int k = 0; k < 8; ++k) s.push(Frame{2048, 0, 0}, [&](uint32_t t, float fx, float, float) { ms.push_back(t); x = fx; });
        assert(ms.size() == 8 && x == 0.5f);
        for (size_t k = 1; k < ms.size(); ++k) assert(ms[k] - ms[k - 1] == 2 || ms[k] - ms[k - 1] == 3);
        assert(ms.back() - ms.front() == 17 || ms.back() - ms.front() == 18);
        assert(ms.front() == 5000 + 1000 + 2 || ms.front() == 5000 + 1000 + 3);
    }
    {   // The measured hardware clock offset (0.6 % slow) passes; 5 % and a
        // 40-frame hole (100 ms) fail the window and stop the feed.
        Sim slow;
        slow.s.restart(0);
        slow.run(3000000, 0.994);
        for (int r : slow.windows) assert(r == 1);
        Sim fast;
        fast.s.restart(0);
        fast.run(3000000, 1.08);
        assert(!fast.windows.empty() && fast.windows[0] == -1 && fast.fed.empty());
        Sim hole;
        hole.s.restart(0);
        hole.run(1100000);
        assert(hole.s.verified());
        hole.run(1000000, 1.0, -1, 0, 40);
        assert(hole.windows.back() == -1 && !hole.s.verified());
        const size_t fed = hole.fed.size();
        hole.run(500000);
        assert(hole.fed.size() == fed);  // held until a window passes again
        hole.run(600000);
        assert(hole.s.verified());
    }
    {   // Frames left in the FIFO across a restart are not counted: the
        // window opens at the first drain after it.
        Sim m;
        m.s.restart(0);
        m.run(1100000);
        assert(m.s.verified());
        for (int k = 0; k < 60; ++k) m.s.push(Frame{0, 0, 0}, [](uint32_t, float, float, float) {});  // backlog
        m.s.restart(0);
        int32_t e, g;
        assert(m.s.check_rate(m.clock_us, &e, &g) == 0);  // opens the window
        m.run(1050000);
        assert(m.windows.back() == 1 && m.s.verified());
    }
    std::puts("PASS fixed-rate stream: no frame before a passing window, 2/3 ms stamps averaging 2.5 ms, raw-to-g scaling, "
              "0.6% clock offset passes, 8% and a 40-frame hole fail and hold the feed, restart backlog not counted");
    return 0;
}
