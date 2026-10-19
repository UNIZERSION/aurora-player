#ifdef NDEBUG
#undef NDEBUG
#endif
// Host tests for the enclosure tap detector (main/tap_gesture.*), driven by
// BMI270 FIFO frames recorded on the device (tests/fixtures/tap_recordings.h,
// labelled in tools/tap-eval/labels.csv) and by taps cut from them.
//
// The firmware feeds every 400 Hz FIFO frame, stamped 2.5 ms apart in whole
// milliseconds (sleep_tap_input.cpp). Every recorded gesture must be found
// exactly once there; every recorded negative (table knocks, an object put
// down, handling, a 4-tap series while handling, 12 s in a pocket) must stay
// silent there and under the perturbations below, and must never turn into
// the other command. Each rejection rule has a scenario that exercises it.
//
// build: zig c++ -std=c++17 -O2 -Wall -Wextra -Werror -I main -I tests tests/tap_gesture_test.cpp main/tap_gesture.cpp
#include "tap_gesture.h"
#include "fixtures/tap_recordings.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <vector>

using player::TapGesture;
using player::TapGestureConfig;
using player::TapGestureDetector;
using player::TapRejectReason;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

struct Vec { float x, y, z; };
Vec frame_g(const tap_fixtures::Frame &f, float scale = 1.0f / 4096) { return {f.x * scale, f.y * scale, f.z * scale}; }

// Feeds samples as the worker does and records every gesture with its time.
struct Rig {
    explicit Rig(const TapGestureConfig &config = {}, uint32_t start_ms = 1000, uint32_t period_us = 2500)
        : detector(config), ms(start_ms), period(period_us) {}
    TapGestureDetector detector;
    uint32_t ms, us = 0, period;
    std::vector<TapGesture> events;
    std::vector<uint32_t> event_ms;
    void feed(const Vec &a) {
        us += period;
        ms += us / 1000;
        us %= 1000;
        const auto g = detector.feed(ms, a.x, a.y, a.z);
        if (g != TapGesture::None) { events.push_back(g); event_ms.push_back(ms); }
    }
    void still(const Vec &gravity, uint32_t duration_ms) {
        for (uint32_t t = 0; t < duration_ms * 1000; t += period) feed(gravity);
    }
    unsigned count(TapGesture g) const {
        unsigned n = 0;
        for (auto e : events) n += e == g;
        return n;
    }
};

Vec mean_head(const tap_fixtures::Recording &r, unsigned frames = 80)
{
    Vec m{0, 0, 0};
    for (unsigned i = 0; i < frames && i < r.count; ++i) {
        const Vec v = frame_g(r.frames[i]);
        m.x += v.x; m.y += v.y; m.z += v.z;
    }
    const float n = float(frames < r.count ? frames : r.count);
    return {m.x / n, m.y / n, m.z / n};
}

// A recording between two still periods at its own resting orientation.
// gain scales the dynamic part around that resting vector; box200 averages
// frame pairs (phase 0 or 1) into a 200 Hz stream instead of 400 Hz.
std::vector<TapGesture> replay(const tap_fixtures::Recording &r, const TapGestureConfig &config = {}, float gain = 1.0f,
                               int box200 = -1)
{
    const Vec g = mean_head(r);
    Rig rig(config, 1000, box200 >= 0 ? 5000 : 2500);
    rig.still(g, 1500);
    auto scaled = [&](unsigned i) {
        const Vec v = frame_g(r.frames[i]);
        return Vec{g.x + gain * (v.x - g.x), g.y + gain * (v.y - g.y), g.z + gain * (v.z - g.z)};
    };
    if (box200 < 0) {
        for (unsigned i = 0; i < r.count; ++i) rig.feed(scaled(i));
    } else {
        for (unsigned i = unsigned(box200) + 1; i < r.count; i += 2) {
            const Vec a = scaled(i - 1), b = scaled(i);
            rig.feed({(a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2});
        }
    }
    rig.still(g, 1500);
    return rig.events;
}

TapGesture expected_gesture(int expected)
{
    return expected == 3 ? TapGesture::Triple : expected == 2 ? TapGesture::Double : TapGesture::None;
}

void recorded_gestures()
{
    for (const auto &r : tap_fixtures::recordings) {
        const auto events = replay(r);
        const TapGesture want = expected_gesture(r.expected);
        if (want == TapGesture::None) {
            if (!events.empty()) std::fprintf(stderr, "  %s: %zu gesture(s)\n", r.name, events.size());
            CHECK(events.empty());
        } else {
            if (events.size() != 1 || events[0] != want) std::fprintf(stderr, "  %s: %zu gesture(s)\n", r.name, events.size());
            CHECK(events.size() == 1 && events[0] == want);
        }
    }
}

// Perturbed replays: a negative never fires; a gesture is either found or
// missed, but never reported as the other command or twice.
void perturbed_recordings()
{
    for (const auto &r : tap_fixtures::recordings) {
        const TapGesture want = expected_gesture(r.expected);
        for (int phase : {0, 1})
            for (float gain : {0.8f, 1.0f, 1.25f, 1.5f}) {
                const auto events = replay(r, {}, gain, phase);
                if (want == TapGesture::None) CHECK(events.empty());
                else CHECK(events.empty() || (events.size() == 1 && events[0] == want));
            }
        for (float gain : {0.8f, 0.9f, 1.1f, 1.25f, 1.5f}) {
            const auto events = replay(r, {}, gain);
            if (want == TapGesture::None) CHECK(events.empty());
            else CHECK(events.empty() || (events.size() == 1 && events[0] == want));
        }
    }
}

// One recorded tap (its 230 ms burst) cut out of a recording, for composing
// series at chosen cadences. Returns the dynamic part around the resting vector.
struct Tap { std::vector<Vec> d; Vec gravity; };
Tap cut_tap(const tap_fixtures::Recording &r, unsigned nth)
{
    const Vec g = mean_head(r);
    unsigned seen = 0;
    for (unsigned i = 1; i < r.count; ++i) {
        const Vec v = frame_g(r.frames[i]);
        const float dx = v.x - g.x, dy = v.y - g.y, dz = v.z - g.z;
        if (dx * dx + dy * dy + dz * dz < 0.25f * 0.25f) continue;
        if (seen++ < nth) { i += 160; continue; }  // skip the rest of this tap
        Tap t{{}, g};
        for (unsigned k = i > 2 ? i - 2 : 0; k < r.count && k < i + 92; ++k) {
            const Vec u = frame_g(r.frames[k]);
            t.d.push_back({u.x - g.x, u.y - g.y, u.z - g.z});
        }
        return t;
    }
    return {};
}

std::vector<TapGesture> compose(const Tap &tap, const std::vector<uint32_t> &onsets_ms, const std::vector<float> &gains = {},
                                uint32_t start_ms = 1000)
{
    Rig rig({}, start_ms);
    rig.still(tap.gravity, 1500);
    const uint32_t origin = rig.ms;
    size_t next = 0;
    std::vector<int> playing;  // per series tap: next frame index, or -1
    const uint32_t total = (onsets_ms.empty() ? 0 : onsets_ms.back()) + 2500;
    std::vector<size_t> pos(onsets_ms.size(), size_t(-1));
    while (rig.ms - origin < total) {
        const uint32_t now = rig.ms - origin;
        while (next < onsets_ms.size() && now >= onsets_ms[next]) pos[next++] = 0;
        Vec a = tap.gravity;
        for (size_t k = 0; k < pos.size(); ++k) {
            if (pos[k] == size_t(-1) || pos[k] >= tap.d.size()) continue;
            const float gain = k < gains.size() ? gains[k] : 1.0f;
            a.x += gain * tap.d[pos[k]].x; a.y += gain * tap.d[pos[k]].y; a.z += gain * tap.d[pos[k]].z;
            ++pos[k];
        }
        rig.feed(a);
    }
    return rig.events;
}

std::vector<uint32_t> cadence(unsigned taps, uint32_t interval)
{
    std::vector<uint32_t> v;
    for (unsigned k = 0; k < taps; ++k) v.push_back(k * interval);
    return v;
}

void composed_series()
{
    const Tap tap = cut_tap(tap_fixtures::recordings[2], 0);  // double_stand, first tap
    CHECK(tap.d.size() > 80);
    for (uint32_t interval : {270u, 350u, 450u, 540u})
        for (unsigned taps = 1; taps <= 7; ++taps) {
            const auto ev = compose(tap, cadence(taps, interval));
            if (taps == 2) CHECK(ev.size() == 1 && ev[0] == TapGesture::Double);
            else if (taps == 3) CHECK(ev.size() == 1 && ev[0] == TapGesture::Triple);
            else CHECK(ev.empty());  // a single tap does nothing; four or more cancel
        }
    // Finger drumming faster than the minimum interval, with every recorded
    // kind of tap (top, side, lighter, along and across gravity) at 0.5-1.5x,
    // 3-8 beats: never a command.
    for (const auto &r : tap_fixtures::recordings) {
        if (r.expected <= 0) continue;
        const Tap beat = cut_tap(r, 0);
        if (beat.d.size() < 80) continue;
        for (float gain : {0.5f, 1.0f, 1.5f})
            for (uint32_t interval : {100u, 130u, 160u, 190u, 215u})
                for (unsigned taps = 3; taps <= 8; ++taps) {
                    const auto ev = compose(beat, cadence(taps, interval), std::vector<float>(taps, gain));
                    if (!ev.empty())
                        std::fprintf(stderr, "  drumming %s x%.1f %u ms %u beats: %zu gesture(s)\n", r.name, double(gain),
                                     unsigned(interval), taps, ev.size());
                    CHECK(ev.empty());
                }
    }
    // Taps too far apart to belong together.
    for (unsigned taps : {2u, 3u}) CHECK(compose(tap, cadence(taps, 800)).empty());
    // Four taps whose first or last is light, down to 5 % of the others:
    // never a command, in particular never the triple of the three clear ones.
    for (float light : {0.05f, 0.08f, 0.1f, 0.15f, 0.3f, 0.5f, 0.7f}) {
        CHECK(compose(tap, cadence(4, 350), {light, 1, 1, 1}).empty());
        CHECK(compose(tap, cadence(4, 350), {1, 1, 1, light}).empty());
        const auto three = compose(tap, cadence(3, 350), {1, 1, light});  // never the double of the clear two
        CHECK(three.empty() || (three.size() == 1 && three[0] == TapGesture::Triple));
    }
    // A double and a triple in a row each count once.
    {
        std::vector<uint32_t> on = {0, 350, 2500, 2850, 3200};
        const auto ev = compose(tap, on);
        CHECK(ev.size() == 2 && ev[0] == TapGesture::Double && ev[1] == TapGesture::Triple);
    }
    // Never eager: nothing before the last tap's completion period.
    {
        Rig rig;
        rig.still(tap.gravity, 1500);
        const uint32_t origin = rig.ms;
        std::vector<uint32_t> on = cadence(3, 350);
        uint32_t last_event_at = 0;
        size_t next = 0;
        std::vector<size_t> pos(on.size(), size_t(-1));
        while (rig.ms < origin + 3000) {
            const uint32_t now = rig.ms - origin;
            while (next < on.size() && now >= on[next]) pos[next++] = 0;
            Vec a = tap.gravity;
            for (auto &p : pos)
                if (p != size_t(-1) && p < tap.d.size()) { a.x += tap.d[p].x; a.y += tap.d[p].y; a.z += tap.d[p].z; ++p; }
            const size_t before = rig.events.size();
            rig.feed(a);
            if (rig.events.size() != before) last_event_at = rig.ms - origin;
        }
        CHECK(rig.events.size() == 1 && rig.events[0] == TapGesture::Triple);
        CHECK(last_event_at >= 700 + 600 && last_event_at <= 700 + 1000);  // ~0.65-0.9 s after the last onset
    }
    // Clock rollover inside a series changes nothing.
    {
        const auto ev = compose(tap, cadence(3, 350), {}, std::numeric_limits<uint32_t>::max() - 1700);
        CHECK(ev.size() == 1 && ev[0] == TapGesture::Triple);
    }
}

// Rule scenarios. Each is a near-miss built from synthetic taps: with every
// rule disabled it produces a command, and the named rule alone (all others
// disabled) stops it. Together this proves that each rule works, and the
// default configuration must keep every scenario silent.
struct Scene {
    explicit Scene(uint32_t rules) {
        TapGestureConfig c;
        c.rules = rules;
        rig = Rig(c);
        rig.still(g, 1500);
    }
    Rig rig;
    const Vec g = norm({0.0f, 0.15f, -0.99f});
    static Vec norm(Vec v) { const float n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); return {v.x / n, v.y / n, v.z / n}; }
    Vec along() const { return g; }
    Vec cross() const { return norm({0.0f, -g.z, g.y}); }   // perpendicular to gravity, device Y/Z plane
    Vec axial() const { return {1.0f, 0.0f, 0.0f}; }         // the device's long axis
    uint32_t elapsed = 0;
    void at(uint32_t t) { while (elapsed < t) step({0, 0, 0}); }
    unsigned ticks = 0;
    void step(Vec d) { rig.feed({g.x + d.x, g.y + d.y, g.z + d.z}); ++ticks; elapsed = ticks * 5 / 2; }
    // A biphasic impact with decaying ringing (230 ms at 400 Hz), scaled so its
    // RMS over the detector's 40 ms feature window is `rms`; `push` adds a
    // unipolar component (a push, not a tap).
    // `ring` adds an 85 Hz tail decaying over ~150 ms, as a knock on the
    // table reaches the device through its feet.
    void tap(Vec dir, float rms, float push = 0, float ring = 0) {
        float s[92] = {};
        s[0] = 1.0f; s[1] = -0.5f;
        for (int k = 2; k < 40; ++k) s[k] = 0.18f * ((k % 2) ? -1.0f : 1.0f) * std::exp(-float(k - 2) / 8);
        for (int k = 0; k < 24 && push > 0; ++k) s[k] += push;
        for (int k = 4; k < 80 && ring > 0; ++k) s[k] += ring * std::sin(float(k) * 2.5e-3f * 85 * 6.2832f) * std::exp(-float(k) / 40);
        float energy = 0;
        for (int k = 0; k < 16; ++k) energy += s[k] * s[k] * 2.5f;
        const float scale = rms * std::sqrt(40.0f / energy);
        for (float v : s) step({dir.x * v * scale, dir.y * v * scale, dir.z * v * scale});
    }
    void series(Vec dir, float rms, std::initializer_list<uint32_t> onsets) {
        const uint32_t base = elapsed;
        for (uint32_t t : onsets) { at(base + t); tap(dir, rms); }
    }
    std::vector<TapGesture> finish() { for (int k = 0; k < 800; ++k) step({0, 0, 0}); return rig.events; }
};

using Build = void (*)(Scene &);
struct RuleCase { player::TapRule rule; const char *name; Build build; };

const RuleCase rule_cases[] = {
    {player::TapRuleSmooth, "Smooth", [](Scene &s) {          // two pushes shaped like taps
        const uint32_t b = s.elapsed;
        s.tap(s.cross(), 0.5f, 0.8f); s.at(b + 350); s.tap(s.cross(), 0.5f, 0.8f); }},
    {player::TapRuleVibration, "Vibration", [](Scene &s) {    // table knocks: along gravity, light
        s.series(s.along(), 0.2f, {0, 350, 700}); }},
    {player::TapRuleAxial, "Axial", [](Scene &s) {            // knocks on the long axis (edge buttons)
        s.series(s.axial(), 0.5f, {0, 350}); }},
    {player::TapRuleLeadQuiet, "LeadQuiet", [](Scene &s) {    // vibration right up to the first tap
        for (int k = 0; k < 400; ++k) s.step({0, 0.17f * std::sin(float(k) * 0.47f), 0});
        s.series(s.cross(), 0.6f, {0, 350}); }},
    {player::TapRuleIsolation, "Isolation", [](Scene &s) {    // faint motion 400 ms before the first tap
        const uint32_t b = s.elapsed;
        for (int k = 0; k < 8; ++k) s.step({0, 0.06f, 0});
        s.at(b + 400); s.series(s.cross(), 0.6f, {0, 350}); }},
    {player::TapRuleInterval, "Interval", [](Scene &s) {      // 232 ms: after the tap window, too fast
        s.series(s.cross(), 0.6f, {0, 232}); }},
    {player::TapRuleSplit, "Split", [](Scene &s) {            // finger drumming at 150 ms
        s.series(s.cross(), 0.6f, {0, 150, 300}); }},
    {player::TapRuleTrailing, "Trailing", [](Scene &s) {      // a third, too-weak tap
        const uint32_t b = s.elapsed;
        s.series(s.cross(), 0.6f, {0, 350}); s.at(b + 700); s.tap(s.cross(), 0.07f); }},
    {player::TapRuleTilt, "Tilt", [](Scene &s) {              // turned by 15 degrees between the taps
        const uint32_t b = s.elapsed;
        s.tap(s.cross(), 0.6f);
        const float a = 0.26f;
        for (int k = 1; k <= 100; ++k) {
            const float f = a * float(k) / 100;
            const Vec t{s.g.x, std::cos(f) * s.g.y - std::sin(f) * s.g.z, std::sin(f) * s.g.y + std::cos(f) * s.g.z};
            s.step({t.x - s.g.x, t.y - s.g.y, t.z - s.g.z});
        }
        const Vec t{s.g.x, std::cos(a) * s.g.y - std::sin(a) * s.g.z, std::sin(a) * s.g.y + std::cos(a) * s.g.z};
        const Vec off{t.x - s.g.x, t.y - s.g.y, t.z - s.g.z};
        while (s.elapsed < b + 520) s.step(off);
        float sh[92] = {};
        sh[0] = 1.0f; sh[1] = -0.5f;
        for (int k = 2; k < 40; ++k) sh[k] = 0.18f * ((k % 2) ? -1.0f : 1.0f) * std::exp(-float(k - 2) / 8);
        const Vec c = s.cross();
        for (float v : sh) s.step({off.x + c.x * v * 1.4f, off.y + c.y * v * 1.4f, off.z + c.z * v * 1.4f});
        for (int k = 0; k < 800; ++k) s.step(off); }},
    {player::TapRuleSustain, "Sustain", [](Scene &s) {        // a slow push building up after the taps
        s.series(s.cross(), 0.6f, {0, 350});
        for (int k = 0; k < 80; ++k) s.step({0.5f * float(k) / 80, 0, 0});
        for (int k = 0; k < 200; ++k) s.step({0.5f, 0, 0}); }},
    {player::TapRuleStrongTap, "StrongTap", [](Scene &s) {    // clear along-gravity taps, none strong
        s.series(s.along(), 0.42f, {0, 350, 700}); }},
    {player::TapRuleConsistency, "Consistency", [](Scene &s) {// one tap across, the next along gravity
        const uint32_t b = s.elapsed;
        s.tap(s.cross(), 0.6f); s.at(b + 350); s.tap(s.along(), 0.8f); }},
    {player::TapRuleCensus, "Census", [](Scene &s) {          // an impulse 400 ms before the first tap
        const uint32_t b = s.elapsed;
        s.tap(s.cross(), 0.07f); s.at(b + 400); s.series(s.cross(), 0.6f, {0, 350}); }},
    {player::TapRuleSettle, "Settle", [](Scene &s) {          // a light knock, then the device lifted and dropped
        const uint32_t b = s.elapsed;
        s.tap(s.cross(), 0.25f);
        s.at(b + 230);
        for (int k = 0; k < 160; ++k) {  // lifted, easing in (no sharp onset): partial free fall
            const float f = float(k) / 160;
            s.step({0, 0, -0.6f * f * f});
        }
        s.tap(s.along(), 1.5f); }},
    {player::TapRuleRinging, "Ringing", [](Scene &s) {        // strong knocks that keep ringing: the table
        const uint32_t b = s.elapsed;
        for (uint32_t t : {0u, 350u, 700u}) { s.at(b + t); s.tap(s.along(), 0.8f, 0, 0.3f); } }},
    {player::TapRuleSharp, "Sharp", [](Scene &s) {            // two slow presses (a button, a push)
        const uint32_t b = s.elapsed;
        for (uint32_t t : {0u, 350u}) {
            s.at(b + t);
            for (int k = 0; k < 24; ++k) {
                const float v = 0.9f * std::sin(3.14159f * float(k) / 24);
                const Vec c = s.cross();
                s.step({c.x * v, c.y * v, c.z * v});
            }
        } }},
};
static_assert(sizeof rule_cases / sizeof rule_cases[0] == player::TapRuleCount, "every rule needs a scenario");

void rule_scenarios()
{
    for (const auto &rc : rule_cases) {
        Scene none(0);
        rc.build(none);
        const auto unguarded = none.finish();
        Scene only(1u << rc.rule);
        rc.build(only);
        const auto alone = only.finish();
        Scene all(player::TapAllRules);
        rc.build(all);
        const auto defaults = all.finish();
        if (unguarded.size() != 1 || !alone.empty() || !defaults.empty())
            std::fprintf(stderr, "  rule %s: without rules %zu, alone %zu, defaults %zu gesture(s)\n", rc.name, unguarded.size(),
                         alone.size(), defaults.size());
        CHECK(unguarded.size() == 1);  // a real near-miss
        CHECK(alone.empty());          // the rule alone stops it
        CHECK(defaults.empty());
    }
}

// While music plays a series needs one strong tap: the two triples recorded
// during playback still pause, a light series across gravity (the player's
// own speaker can shake an upright device like that) does not.
void playing_mode()
{
    for (const auto *name : {"triple_stand_music", "triple_stand_lighter"})
        for (const auto &r : tap_fixtures::recordings) {
            if (std::string_view(r.name) != name) continue;
            const Vec g = mean_head(r);
            Rig rig;
            rig.detector.set_playing(true);
            rig.still(g, 1500);
            for (unsigned i = 0; i < r.count; ++i) rig.feed(frame_g(r.frames[i]));
            rig.still(g, 1500);
            CHECK(rig.events.size() == 1 && rig.events[0] == TapGesture::Triple);
        }
    for (bool playing : {false, true}) {
        Scene s(player::TapAllRules);
        s.rig.detector.set_playing(playing);
        s.series(s.cross(), 0.25f, {0, 350, 700});
        const auto events = s.finish();
        if (playing) CHECK(events.empty());
        else CHECK(events.size() == 1 && events[0] == TapGesture::Triple);
    }
}

void stream_faults()
{
    const Tap tap = cut_tap(tap_fixtures::recordings[2], 0);
    const Vec g = tap.gravity;
    auto two_taps_then = [&](auto &&fault) {
        Rig rig;
        rig.still(g, 1500);
        for (const auto &d : tap.d) rig.feed({g.x + d.x, g.y + d.y, g.z + d.z});
        rig.still(g, 120);
        for (const auto &d : tap.d) rig.feed({g.x + d.x, g.y + d.y, g.z + d.z});
        fault(rig);
        rig.still(g, 2000);
        return rig.events.size();
    };
    CHECK(two_taps_then([](Rig &) {}) == 1);                                   // control
    CHECK(two_taps_then([](Rig &r) { r.detector.reset(); }) == 0);             // reset discards
    CHECK(two_taps_then([](Rig &r) { r.ms += 60; }) == 0);                     // a gap discards
    CHECK(two_taps_then([](Rig &r) { r.ms -= 30; }) == 0);                     // time going back discards
    CHECK(two_taps_then([&](Rig &r) { r.feed({std::nanf(""), g.y, g.z}); }) == 0);
    CHECK(two_taps_then([&](Rig &r) { r.feed({std::numeric_limits<float>::infinity(), g.y, g.z}); }) == 0);
    CHECK(two_taps_then([&](Rig &r) {                                           // duplicates are ignored
        const uint32_t at = r.ms;
        for (int k = 0; k < 5; ++k) (void)r.detector.feed(at, g.x, g.y, g.z);
    }) == 1);
    {   // Settling: taps right after arming are not used.
        Rig rig;
        rig.still(g, 100);
        for (int k = 0; k < 2; ++k) {
            for (const auto &d : tap.d) rig.feed({g.x + d.x, g.y + d.y, g.z + d.z});
            rig.still(g, 120);
        }
        rig.still(g, 2000);
        CHECK(rig.events.empty());
    }
    {   // Snapshot counters.
        Rig rig;
        rig.still(g, 1500);
        for (int k = 0; k < 3; ++k) {
            for (const auto &d : tap.d) rig.feed({g.x + d.x, g.y + d.y, g.z + d.z});
            rig.still(g, 120);
        }
        rig.still(g, 2000);
        const auto s = rig.detector.snapshot();
        CHECK(rig.events.size() == 1 && s.triple_events == 1 && s.accepted_taps == 3 && s.impulses >= 3);
        CHECK(s.peak_rms_g > 0.3f && s.taps == 0 && !s.blocked);
    }
}
}  // namespace

int main()
{
    recorded_gestures();
    perturbed_recordings();
    composed_series();
    rule_scenarios();
    playing_mode();
    stream_faults();
    if (failures) {
        std::fprintf(stderr, "tap_gesture_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS tap gesture: %zu recorded gestures and negatives at 400 Hz, 200 Hz box phases and 0.8-1.5x gain; "
                "composed 1-7 tap series, fast/slow cadences, light first/last of four, rollover; one scenario per rule; "
                "playing mode; stream faults\n",
                sizeof tap_fixtures::recordings / sizeof tap_fixtures::recordings[0]);
    return 0;
}
