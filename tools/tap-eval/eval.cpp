// Tap detector evaluation and robustness harness (tap-detector-v2).
//
// usage: eval LABELS BASE_DIR [--suite quick|full|base] [--ablate] [--stress]
//             [--json OUT] [--list] [key=value ...]
//
// The detector under test is any tap_gesture.h/.cpp pair that provides the
// public API (TapGestureDetector{cfg}.feed(ms,x,y,z) -> None/Double/Triple)
// and two macros:
//   TAP_GESTURE_TUNABLES(X)  X(type, field) for every TapGestureConfig field
//                            the harness may override with key=value
//   TAP_GESTURE_RULES(X)     X(Name) for every independent rejection rule; bit
//                            i of TapGestureConfig::rules enables rule i
//
// Scoring: a label is correct when
// [start, end + 1500 ms] in ORIGINAL recording time holds exactly the expected
// gesture (or none for a negative); any gesture outside every label window is
// an unlabelled false positive. Labels with expected -1 are not scored.
// Positives whose name contains "light" are reported separately.
//
// Objective families (every one must give 0 false positives): the recordings
// as captured, the other FIFO box phase and the raw 400 Hz FIFO frames for the
// FIFO recordings, +-2 ms jitter, 200/150 Hz box/linear/Lanczos resampling at
// 8 phases, decimation 1/4 and 1/5, random loss 1/2/5 %, amplitude 0.8-1.5x,
// rotations to other poses, and all sessions concatenated. Report-only
// families (100/125 Hz, 20 % loss, +-3 ms jitter with duplicates) show where
// the detector degrades but are not part of the objective.
#include "tap_gesture.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Sample { uint32_t t; float x, y, z; int src; int64_t ot; };
using Rec = std::vector<Sample>;
struct Label { int src; uint32_t start, end; int expected; std::string name; bool light; };
struct Hit { int src; int64_t ot; int kind; };
struct Score {
    int pos = 0, ok = 0, pos_light = 0, ok_light = 0, missed = 0, wrong = 0, fp_lab = 0, fp_unlab = 0;
    std::vector<std::string> bad;
    int fp() const { return fp_lab + fp_unlab; }
};

std::vector<std::string> g_sources;
std::vector<Label> g_labels;
std::vector<Rec> g_data;                  // per source, as recorded
std::map<int, Rec> g_ph0, g_ph1;          // raw 400 Hz FIFO sources: frame pairs averaged at both phases
player::TapGestureConfig g_cfg{};
bool g_playing = false;  // --playing: TapGestureDetector::set_playing(true) for every stream

const char *kind(int k) { return k == 3 ? "TRIPLE" : k == 2 ? "DOUBLE" : "none"; }

const char *const g_rule_names[] = {
#define TAP_EVAL_RULE_NAME(name) #name,
    TAP_GESTURE_RULES(TAP_EVAL_RULE_NAME)
#undef TAP_EVAL_RULE_NAME
};
constexpr unsigned g_rule_count = sizeof g_rule_names / sizeof g_rule_names[0];

bool set_field(player::TapGestureConfig &c, const std::string &key, double v)
{
#define TAP_EVAL_SET(type, name) if (key == #name) { c.name = static_cast<type>(v); return true; }
    TAP_GESTURE_TUNABLES(TAP_EVAL_SET)
#undef TAP_EVAL_SET
    if (key == "rules") { c.rules = static_cast<uint32_t>(v); return true; }
    return false;
}
void print_fields(const player::TapGestureConfig &c)
{
#define TAP_EVAL_PRINT(type, name) std::printf(" %s=%g", #name, double(c.name));
    TAP_GESTURE_TUNABLES(TAP_EVAL_PRINT)
#undef TAP_EVAL_PRINT
    std::printf(" rules=0x%x\n", unsigned(c.rules));
}

bool load(const std::string &path, int src, Rec &out)
{
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char line[128];
    if (!std::fgets(line, sizeof line, f)) { std::fclose(f); return false; }
    unsigned t; int x, y, z;
    while (std::fscanf(f, "%u,%d,%d,%d", &t, &x, &y, &z) == 4)
        out.push_back({t, x / 1000.f, y / 1000.f, z / 1000.f, src, (int64_t)t});
    std::fclose(f);
    return true;
}

std::vector<Hit> run(const Rec &s, const player::TapGestureConfig &cfg)
{
    player::TapGestureDetector d{cfg};
    d.set_playing(g_playing);
    std::vector<Hit> hits;
    for (const auto &p : s) {
        const auto g = d.feed(p.t, p.x, p.y, p.z);
        if (g != player::TapGesture::None) hits.push_back({p.src, p.ot, g == player::TapGesture::Triple ? 3 : 2});
    }
    return hits;
}

Score score_streams(const std::vector<Rec> &streams, const player::TapGestureConfig &cfg)
{
    std::vector<Hit> hits;
    for (const auto &s : streams) { auto h = run(s, cfg); hits.insert(hits.end(), h.begin(), h.end()); }
    Score sc;
    std::vector<bool> used(hits.size());
    for (const auto &l : g_labels) {
        int got = 0, count = 0;
        for (size_t i = 0; i < hits.size(); ++i)
            if (hits[i].src == l.src && hits[i].ot >= l.start && hits[i].ot <= (int64_t)l.end + 1500) {
                got = hits[i].kind; ++count; used[i] = true;
            }
        if (l.expected < 0) continue;
        const bool ok = l.expected ? (count == 1 && got == l.expected) : count == 0;
        if (l.expected) { ++sc.pos; if (l.light) ++sc.pos_light; }
        if (ok) { if (l.expected) { ++sc.ok; if (l.light) ++sc.ok_light; } continue; }
        if (l.expected && !count) ++sc.missed;
        else if (!l.expected) sc.fp_lab += count;
        else if (count > 1 || got != l.expected) {
            // A wrong command is a false positive for the user: count both.
            ++sc.wrong;
        }
        char buf[300];
        std::snprintf(buf, sizeof buf, "%s: exp %s got %s x%d", l.name.c_str(), kind(l.expected), kind(got), count);
        sc.bad.push_back(buf);
    }
    for (size_t i = 0; i < hits.size(); ++i)
        if (!used[i]) {
            ++sc.fp_unlab;
            char buf[300];
            std::snprintf(buf, sizeof buf, "UNLABELLED %s at %s %lld", kind(hits[i].kind), hits[i].src < 0 ? "<bridge>" : g_sources[hits[i].src].c_str(), (long long)hits[i].ot);
            sc.bad.push_back(buf);
        }
    return sc;
}

// ---------- perturbations (each maps one source recording to a new one) ----------
Rec jitter(const Rec &v, unsigned seed, int amp, bool strict)
{
    std::mt19937 r(seed);
    std::uniform_int_distribution<int> U(-amp, amp);
    Rec o = v;
    int64_t last = -1;
    for (auto &p : o) {
        int64_t t = (int64_t)p.t + U(r);
        if (strict) { if (t <= last) t = last + 1; }
        else { if (t < last) t = last; }
        p.t = (uint32_t)t; last = t;
    }
    return o;
}
// Uniform grid, segment by segment (a recording gap > 40 ms stays a gap).
// mode 0: linear, 1: nearest, 2: box average over the preceding period.
Rec resample(const Rec &v, double period, double phase, int mode)
{
    Rec o;
    size_t a = 0;
    while (a < v.size()) {
        size_t b = a;
        while (b + 1 < v.size() && v[b + 1].t - v[b].t <= 40) ++b;
        const double t0 = v[a].t, t1 = v[b].t;
        size_t j = a;
        for (double t = t0 + phase; t <= t1; t += period) {
            while (j + 1 <= b && v[j + 1].t <= t) ++j;
            Sample s = v[j];
            if (mode == 0 || mode == 1) {
                if (j < b && t > v[j].t) {
                    const double f = (t - v[j].t) / double(v[j + 1].t - v[j].t);
                    if (mode == 0) {
                        s.x = float(v[j].x + f * (v[j + 1].x - v[j].x));
                        s.y = float(v[j].y + f * (v[j + 1].y - v[j].y));
                        s.z = float(v[j].z + f * (v[j + 1].z - v[j].z));
                    } else if (f >= 0.5) s = v[j + 1];
                }
            } else {
                double sx = 0, sy = 0, sz = 0; int n = 0;
                for (size_t k = j + 1; k-- > a;) {
                    if (v[k].t <= t - period) break;
                    sx += v[k].x; sy += v[k].y; sz += v[k].z; ++n;
                }
                if (n) { s.x = float(sx / n); s.y = float(sy / n); s.z = float(sz / n); }
            }
            s.t = (uint32_t)std::llround(t);
            s.ot = s.t;
            s.src = v[a].src;
            if (!o.empty() && s.t <= o.back().t) continue;
            o.push_back(s);
        }
        a = b + 1;
    }
    return o;
}
Rec drop_every(const Rec &v, int n, int k)
{
    Rec o;
    for (size_t i = 0; i < v.size(); ++i) if (int(i % n) != k) o.push_back(v[i]);
    return o;
}
Rec drop_random(const Rec &v, double p, unsigned seed)
{
    std::mt19937 r(seed);
    std::uniform_real_distribution<double> U(0, 1);
    Rec o;
    for (const auto &s : v) if (U(r) >= p) o.push_back(s);
    return o;
}
Rec lanczos(const Rec &v, double period, double phase)
{
    auto L = [](double x) {
        if (std::fabs(x) < 1e-9) return 1.0;
        if (std::fabs(x) >= 3) return 0.0;
        const double px = 3.14159265358979 * x;
        return 3 * std::sin(px) * std::sin(px / 3) / (px * px);
    };
    Rec o;
    size_t a = 0;
    while (a < v.size()) {
        size_t b = a;
        while (b + 1 < v.size() && v[b + 1].t - v[b].t <= 40) ++b;
        size_t j = a;
        // Downsampling stretches the kernel (anti-aliasing); upsampling does not.
        const double native = b > a ? double(v[b].t - v[a].t) / double(b - a) : period;
        const double stretch = std::max(1.0, period / native);
        for (double t = v[a].t + phase; t <= v[b].t; t += period) {
            while (j + 1 <= b && v[j + 1].t <= t) ++j;
            const double f = j < b ? (t - v[j].t) / double(v[j + 1].t - v[j].t) : 0.0;
            const double pos = double(j) + f;
            double sx = 0, sy = 0, sz = 0, sw = 0;
            const long reach = long(std::ceil(3 * stretch));
            for (long k = long(j) - reach + 1; k <= long(j) + reach; ++k) {
                const long kk = std::max(long(a), std::min(long(b), k));
                const double w = L((pos - double(k)) / stretch);
                sx += w * v[kk].x; sy += w * v[kk].y; sz += w * v[kk].z; sw += w;
            }
            Sample s = v[j];
            s.x = float(sx / sw); s.y = float(sy / sw); s.z = float(sz / sw);
            s.t = (uint32_t)std::llround(t); s.ot = s.t;
            if (!o.empty() && s.t <= o.back().t) continue;
            o.push_back(s);
        }
        a = b + 1;
    }
    return o;
}
// Zero-phase gravity (forward + backward EMA), then scale the dynamic part
// only; gain_all scales the whole vector (sensor gain error).
Rec amplitude(const Rec &v, float sc, bool gain_all, float tau = 300.f)
{
    Rec o = v;
    if (gain_all) { for (auto &p : o) { p.x *= sc; p.y *= sc; p.z *= sc; } return o; }
    std::vector<float> g(v.size() * 3);
    float c[3] = {v[0].x, v[0].y, v[0].z};
    for (size_t i = 0; i < v.size(); ++i) {
        const float dt = i ? float(v[i].t - v[i - 1].t) : 0.f;
        const float k = dt > 80 ? 1.f : dt / (tau + dt);
        const float a[3] = {v[i].x, v[i].y, v[i].z};
        for (int q = 0; q < 3; ++q) { c[q] += k * (a[q] - c[q]); g[i * 3 + q] = c[q]; }
    }
    float b[3] = {g[(v.size() - 1) * 3], g[(v.size() - 1) * 3 + 1], g[(v.size() - 1) * 3 + 2]};
    for (size_t i = v.size(); i-- > 0;) {
        const float dt = i + 1 < v.size() ? float(v[i + 1].t - v[i].t) : 0.f;
        const float k = dt > 80 ? 1.f : dt / (tau + dt);
        for (int q = 0; q < 3; ++q) { b[q] += k * (g[i * 3 + q] - b[q]); g[i * 3 + q] = b[q]; }
    }
    for (size_t i = 0; i < o.size(); ++i) {
        float *a[3] = {&o[i].x, &o[i].y, &o[i].z};
        for (int q = 0; q < 3; ++q) *a[q] = g[i * 3 + q] + sc * (*a[q] - g[i * 3 + q]);
    }
    return o;
}
Rec scale_range(const Rec &v, int64_t a, int64_t b, float sc)
{
    Rec full = amplitude(v, sc, false);
    Rec o = v;
    for (size_t i = 0; i < o.size(); ++i) if (o[i].ot >= a && o[i].ot <= b) o[i] = full[i];
    return o;
}
struct Mat { float m[3][3]; };
Rec rotate(const Rec &v, const Mat &R)
{
    Rec o = v;
    for (auto &p : o) {
        const float a[3] = {p.x, p.y, p.z};
        p.x = R.m[0][0] * a[0] + R.m[0][1] * a[1] + R.m[0][2] * a[2];
        p.y = R.m[1][0] * a[0] + R.m[1][1] * a[1] + R.m[1][2] * a[2];
        p.z = R.m[2][0] * a[0] + R.m[2][1] * a[1] + R.m[2][2] * a[2];
    }
    return o;
}
Mat mul(const Mat &A, const Mat &B)
{
    Mat C{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) C.m[i][j] += A.m[i][k] * B.m[k][j];
    return C;
}
Mat axis_angle(const double ax[3], double ang)
{
    double n = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    Mat R{};
    if (n < 1e-9) { R.m[0][0] = R.m[1][1] = R.m[2][2] = 1; return R; }
    const double x = ax[0] / n, y = ax[1] / n, z = ax[2] / n, c = std::cos(ang), s = std::sin(ang), C = 1 - c;
    const double r[3][3] = {{c + x * x * C, x * y * C - z * s, x * z * C + y * s},
                            {y * x * C + z * s, c + y * y * C, y * z * C - x * s},
                            {z * x * C - y * s, z * y * C + x * s, c + z * z * C}};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R.m[i][j] = float(r[i][j]);
    return R;
}
Mat a_to_b(const double a_in[3], const double b_in[3], double spin)
{
    double a[3], b[3];
    const double na = std::sqrt(a_in[0] * a_in[0] + a_in[1] * a_in[1] + a_in[2] * a_in[2]);
    const double nb = std::sqrt(b_in[0] * b_in[0] + b_in[1] * b_in[1] + b_in[2] * b_in[2]);
    for (int i = 0; i < 3; ++i) { a[i] = a_in[i] / na; b[i] = b_in[i] / nb; }
    double ax[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    double ang = std::acos(std::max(-1.0, std::min(1.0, d)));
    if (std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]) < 1e-9 && d < 0) { ax[0] = 1; ax[1] = ax[2] = 0; if (std::fabs(a[0]) > 0.9) { ax[0] = 0; ax[1] = 1; } }
    return mul(axis_angle(b, spin), axis_angle(ax, ang));
}
// Recordings back to back as one stream (no reset), with a smooth tilt bridge.
Rec concat(const std::vector<Rec> &pieces, uint32_t join_dt, uint32_t bridge_ms, int64_t t_start)
{
    Rec o;
    int64_t t = t_start;
    for (const auto &p : pieces) {
        if (p.empty()) continue;
        if (!o.empty() && bridge_ms) {
            const Sample a = o.back(), b = p.front();
            for (uint32_t k = join_dt; k < bridge_ms; k += join_dt) {
                const float f = float(k) / bridge_ms;
                const float s = f * f * (3 - 2 * f);
                Sample q{0, a.x + s * (b.x - a.x), a.y + s * (b.y - a.y), a.z + s * (b.z - a.z), -1, -1};
                t += join_dt; q.t = (uint32_t)t; o.push_back(q);
            }
        }
        int64_t base = o.empty() ? t : t + join_dt;
        const int64_t first = p.front().t;
        for (const auto &s : p) {
            Sample q = s;
            const int64_t nt = base + (int64_t(s.t) - first);
            q.t = (uint32_t)nt;
            o.push_back(q);
            t = nt;
        }
    }
    return o;
}

// ---------- families ----------
using Make = std::function<std::vector<Rec>(int run)>;
// objective: must give 0 FP; recall: counts towards the recall averages.
struct Family { std::string name; int runs; Make make; bool objective; bool recall = true; };
struct Agg {
    std::string name; bool objective = false, recall = true;
    int runs = 0, runs_bad = 0, pos = 0, ok = 0, pos_light = 0, ok_light = 0, missed = 0, wrong = 0, fp = 0;
    std::map<std::string, int> what;
    void add(const Score &s) {
        ++runs; if (s.fp() || s.missed || s.wrong) ++runs_bad;
        pos += s.pos; ok += s.ok; pos_light += s.pos_light; ok_light += s.ok_light;
        missed += s.missed; wrong += s.wrong; fp += s.fp();
        for (auto &b : s.bad) ++what[b];
    }
    double recall_normal() const { return pos - pos_light ? double(ok - ok_light) / (pos - pos_light) : 1; }
    double recall_light() const { return pos_light ? double(ok_light) / pos_light : 1; }
};

unsigned g_threads = 1;
Agg run_family(const Family &f, const player::TapGestureConfig &cfg)
{
    std::vector<Score> scores(f.runs);
    std::atomic<int> next{0};
    auto worker = [&] {
        for (int i; (i = next++) < f.runs;) scores[i] = score_streams(f.make(i), cfg);
    };
    std::vector<std::thread> pool;
    for (unsigned k = 1; k < std::min<unsigned>(g_threads, unsigned(f.runs)); ++k) pool.emplace_back(worker);
    worker();
    for (auto &t : pool) t.join();
    Agg a; a.name = f.name; a.objective = f.objective; a.recall = f.recall;
    for (auto &s : scores) a.add(s);
    return a;
}

std::vector<Rec> map_all(const std::function<Rec(const Rec &)> &fn)
{
    std::vector<Rec> st;
    for (auto &r : g_data) st.push_back(fn(r));
    return st;
}

std::vector<Family> families(const std::string &suite)
{
    std::vector<Family> f;
    f.push_back({"as recorded", 1, [](int) { return g_data; }, true});
    if (suite == "base") return f;
    const bool full = suite == "full";
    if (!g_ph0.empty()) f.push_back({"FIFO pairs box-averaged to 200 Hz, phase 0", 1, [](int) {
        std::vector<Rec> st = g_data;
        for (auto &[i, r] : g_ph0) st[i] = r;
        return st; }, true});
    if (!g_ph1.empty()) f.push_back({"FIFO pairs box-averaged to 200 Hz, phase 1", 1, [](int) {
        std::vector<Rec> st = g_data;
        for (auto &[i, r] : g_ph1) st[i] = r;
        return st; }, true});
    const int seeds = full ? 20 : 4, phases = full ? 8 : 2;
    f.push_back({"jitter +-2 ms", seeds, [](int k) { return map_all([k](const Rec &r) { return jitter(r, k * 7919u + 13u * unsigned(r[0].src) + 1, 2, true); }); }, true});
    for (double hz : {200.0, 150.0}) {
        const double per = 1000.0 / hz;
        const char *mn[] = {"linear", "nearest", "box"};
        for (int mode : {2, 0}) {
            char nm[64]; std::snprintf(nm, sizeof nm, "%.0f Hz %s x%d phases", hz, mn[mode], phases);
            f.push_back({nm, phases, [per, mode, phases](int k) { return map_all([=](const Rec &r) { return resample(r, per, per * k / phases, mode); }); }, true});
        }
        char nm[64]; std::snprintf(nm, sizeof nm, "%.0f Hz Lanczos x%d phases", hz, phases);
        f.push_back({nm, phases, [per, phases](int k) { return map_all([=](const Rec &r) { return lanczos(r, per, per * k / phases); }); }, true});
    }
    for (int n : {4, 5}) {
        char nm[64]; std::snprintf(nm, sizeof nm, "drop 1 in %d (all offsets)", n);
        f.push_back({nm, n, [n](int k) { return map_all([=](const Rec &r) { return drop_every(r, n, k); }); }, true});
    }
    for (double p : {0.01, 0.02, 0.05}) {
        char nm[64]; std::snprintf(nm, sizeof nm, "random loss %.0f%% x%d", p * 100, seeds);
        f.push_back({nm, seeds, [p](int k) { return map_all([=](const Rec &r) { return drop_random(r, p, k * 31u + 7u * unsigned(r[0].src) + 3); }); }, true});
    }
    for (float sc : {0.8f, 0.9f, 1.1f, 1.25f, 1.5f}) {
        char nm[64]; std::snprintf(nm, sizeof nm, "amplitude x%.2f (dynamic)", sc);
        f.push_back({nm, 1, [sc](int) { return map_all([=](const Rec &r) { return amplitude(r, sc, false); }); }, true});
    }
    for (float sc : {0.8f, 1.2f}) {
        char nm[64]; std::snprintf(nm, sizeof nm, "sensor gain x%.2f", sc);
        f.push_back({nm, 1, [sc](int) { return map_all([=](const Rec &r) { return amplitude(r, sc, true); }); }, true});
    }
    {
        // Same rotation for every recording: other poses and mounts.
        static const double g0[3] = {0.005, 0.146, -0.99};
        static const double poses[][3] = {{0.92, 0, 0.39}, {0.92, 0, -0.39}, {0.92, 0.39, 0}, {1, 0, 0}, {0, 0, -1}, {0, 0, 1},
                                          {0, 1, 0}, {0, -1, 0}, {0, 0.8, -0.6}, {-1, 0, 0}, {1, 1, 1}};
        const int spins = full ? 4 : 1;
        f.push_back({"rotations: 11 poses", 11 * spins, [spins](int k) {
            const Mat R = a_to_b(g0, poses[k / spins], (k % spins) * 3.14159265358979 / 2);
            return map_all([&](const Rec &r) { return rotate(r, R); }); }, true, false});
    }
    f.push_back({"all sessions concatenated, 2 s bridges", 1, [](int) { return std::vector<Rec>{concat(g_data, 5, 2000, 1000)}; }, true});
    if (full) {
        // Report only: where the detector degrades.
        for (double hz : {125.0, 100.0}) {
            const double per = 1000.0 / hz;
            char nm[64]; std::snprintf(nm, sizeof nm, "REPORT %.0f Hz box x4", hz);
            f.push_back({nm, 4, [per](int k) { return map_all([=](const Rec &r) { return resample(r, per, per * k / 4, 2); }); }, false});
        }
        f.push_back({"REPORT random loss 20% x10", 10, [](int k) { return map_all([=](const Rec &r) { return drop_random(r, 0.2, k * 31u + 7u * unsigned(r[0].src) + 3); }); }, false});
        f.push_back({"REPORT jitter +-3 ms, duplicates x10", 10, [](int k) { return map_all([k](const Rec &r) { return jitter(r, k * 7919u + 13u * unsigned(r[0].src) + 1, 3, false); }); }, false});
        f.push_back({"REPORT amplitude x0.6 (dynamic)", 1, [](int) { return map_all([](const Rec &r) { return amplitude(r, 0.6f, false); }); }, false});
        f.push_back({"REPORT amplitude x2.0 (dynamic)", 1, [](int) { return map_all([](const Rec &r) { return amplitude(r, 2.0f, false); }); }, false});
    }
    return f;
}

struct Summary { int fp = 0, wrong = 0; double recall_normal = 0, recall_light = 0, min_recall_normal = 1; int families = 0; };

Summary report(const std::vector<Agg> &aggs, bool verbose, FILE *json)
{
    Summary s;
    for (auto &a : aggs) {
        std::printf("  %-44s runs=%-3d normal %5.1f%%  light %5.1f%%  missed=%-4d wrong=%-3d FP=%-3d%s\n", a.name.c_str(), a.runs,
                    100 * a.recall_normal(), 100 * a.recall_light(), a.missed, a.wrong, a.fp, a.fp || a.wrong ? (a.objective ? "   <-- FP" : "   (report)") : "");
        if (verbose || a.fp || a.wrong)
            for (auto &[k, v] : a.what)
                if (verbose || k.find("exp none") != std::string::npos || k.find("UNLABELLED") == 0 || k.find("got none") == std::string::npos)
                    std::printf("        %4dx  %s\n", v, k.c_str());
        if (!a.objective) continue;
        s.fp += a.fp; s.wrong += a.wrong;
        if (!a.recall) continue;  // device-frame rules make rotated recall meaningless
        ++s.families;
        s.recall_normal += a.recall_normal(); s.recall_light += a.recall_light();
        s.min_recall_normal = std::min(s.min_recall_normal, a.recall_normal());
    }
    if (s.families) { s.recall_normal /= s.families; s.recall_light /= s.families; }
    std::printf("OBJECTIVE fp=%d wrong=%d recall_normal=%.4f min_recall_normal=%.4f recall_light=%.4f families=%d\n",
                s.fp, s.wrong, s.recall_normal, s.min_recall_normal, s.recall_light, s.families);
    if (json) {
        std::fprintf(json, "{\"fp\":%d,\"wrong\":%d,\"recall_normal\":%.4f,\"min_recall_normal\":%.4f,\"recall_light\":%.4f,\"families\":[", s.fp, s.wrong,
                     s.recall_normal, s.min_recall_normal, s.recall_light);
        for (size_t i = 0; i < aggs.size(); ++i)
            std::fprintf(json, "%s{\"name\":\"%s\",\"objective\":%s,\"runs\":%d,\"recall_normal\":%.4f,\"recall_light\":%.4f,\"missed\":%d,\"wrong\":%d,\"fp\":%d}",
                         i ? "," : "", aggs[i].name.c_str(), aggs[i].objective ? "true" : "false", aggs[i].runs, aggs[i].recall_normal(),
                         aggs[i].recall_light(), aggs[i].missed, aggs[i].wrong, aggs[i].fp);
        std::fprintf(json, "]}\n");
    }
    return s;
}

// Stress: scale single windows (table taps, 4-tap series, hardest handling).
void stress(const player::TapGestureConfig &cfg)
{
    std::printf("\n== STRESS: one label window's dynamic part scaled (FP = a gesture inside that negative window)\n");
    for (const auto &l : g_labels) {
        if (l.expected != 0) continue;
        const bool interesting = l.name.find("TABLE") != std::string::npos || l.name.find("table") != std::string::npos ||
                                 l.name.find("4 taps") != std::string::npos || l.name.find("4-tap") != std::string::npos ||
                                 l.name.find("single") != std::string::npos || l.name.find("1 tap") != std::string::npos;
        if (!interesting) continue;
        std::printf("  %-58s", l.name.substr(0, 58).c_str());
        for (float sc : {0.5f, 0.7f, 1.5f, 2.0f, 2.5f, 3.0f}) {
            std::vector<Rec> st = g_data;
            st[l.src] = scale_range(g_data[l.src], l.start, int64_t(l.end) + 200, sc);
            const Score s = score_streams(st, cfg);
            std::string res = "ok";
            for (auto &b : s.bad) if (b.compare(0, l.name.size(), l.name) == 0) res = b.substr(b.find("got ") + 4, 6);
            std::printf(" x%.1f:%-6s", sc, res.c_str());
        }
        std::printf("\n");
    }
}

// Per-impulse decisions inside [from, to] of every source whose name contains `which`.
void trace(const std::string &which, int64_t from, int64_t to, const player::TapGestureConfig &cfg)
{
    static const char *reasons[] = {"None", "Smooth", "TooFast", "TooSlow", "Vibration", "TooMany", "Sustained", "CompletionNotQuiet",
                                    "NotQuietBefore", "Tilt", "Axial", "Weak", "NoStrongTap", "Inconsistent", "Census", "Unsettled", "Slow", "Ringing"};
    for (size_t i = 0; i < g_data.size(); ++i) {
        if (g_sources[i].find(which) == std::string::npos) continue;
        player::TapGestureDetector d{cfg};
        auto prev = d.snapshot();
        for (const auto &x : g_data[i]) {
            const auto g = d.feed(x.t, x.x, x.y, x.z);
            const auto now = d.snapshot();
            if (x.ot >= from && x.ot <= to) {
                std::string what;
                if (now.impulses != prev.impulses) {
                    char b[200];
                    std::snprintf(b, sizeof b, " impulse rms=%.3f along=%.2f axial=%.2f smooth=%.3f lead=%.3f hz=%.0f pre=%.3f", now.last_rms_g, now.last_along, now.last_axial, now.last_smooth_g, now.last_lead_g, now.last_hz, now.last_pre_smooth_g);
                    what += b;
                }
                if (now.accepted_taps != prev.accepted_taps) what += " TAP#" + std::to_string(now.taps);
                for (unsigned k = 1; k < unsigned(player::TapRejectReason::Count); ++k)
                    if (now.rejected[k] != prev.rejected[k]) what += std::string(" reject:") + (k < sizeof reasons / sizeof *reasons ? reasons[k] : "?");
                if (now.seeds != prev.seeds) what += " SEED";
                if (now.blocked != prev.blocked) what += now.blocked ? " BLOCK" : " unblock";
                if (now.settling != prev.settling) what += now.settling ? " settling" : " settled";
                if (g != player::TapGesture::None) what += g == player::TapGesture::Triple ? " ==> TRIPLE" : " ==> DOUBLE";
                if (!what.empty()) std::printf("  %s %lld:%s\n", g_sources[i].c_str(), (long long)x.ot, what.c_str());
            }
            prev = now;
        }
    }
}

void ablate(const std::string &suite, const player::TapGestureConfig &base)
{
    std::printf("\n== ABLATION: each rule disabled alone (any FP means that rule alone stopped the event)\n");
    std::vector<Family> fams = families(suite);
    int single_points = 0;
    for (unsigned r = 0; r < g_rule_count; ++r) {
        player::TapGestureConfig cfg = base;
        cfg.rules &= ~(1u << r);
        std::map<std::string, int> what;
        int fp = 0;
        double recall = 0; int n = 0;
        for (auto &f : fams) {
            if (!f.objective) continue;
            const Agg a = run_family(f, cfg);
            fp += a.fp + a.wrong;
            recall += a.recall_normal(); ++n;
            for (auto &[k, v] : a.what)
                if (k.find("exp none") != std::string::npos || k.find("UNLABELLED") == 0 || k.find("got none") == std::string::npos) what[k] += v;
        }
        std::printf("  without %-22s FP+wrong=%-4d recall_normal=%.3f\n", g_rule_names[r], fp, n ? recall / n : 0);
        for (auto &[k, v] : what) std::printf("        %4dx  %s\n", v, k.c_str());
        if (fp) ++single_points;
    }
    std::printf("ABLATION single_point_rules=%d of %u\n", single_points, g_rule_count);
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: eval LABELS BASE_DIR [--suite quick|full|base] [--ablate] [--stress] [--json OUT] [--verbose] [--threads N] [key=value ...]\n"); return 2; }
    const std::string base = std::string(argv[2]) + "/";
    std::string suite = "quick", json_path;
    bool do_ablate = false, do_stress = false, verbose = false, list = false;
    std::string trace_src; int64_t trace_from = 0, trace_to = 0;
    g_threads = std::max(1u, std::thread::hardware_concurrency());
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--suite" && i + 1 < argc) suite = argv[++i];
        else if (a == "--json" && i + 1 < argc) json_path = argv[++i];
        else if (a == "--threads" && i + 1 < argc) g_threads = unsigned(std::atoi(argv[++i]));
        else if (a == "--ablate") do_ablate = true;
        else if (a == "--playing") g_playing = true;
        else if (a == "--trace" && i + 3 < argc) { trace_src = argv[++i]; trace_from = std::atoll(argv[++i]); trace_to = std::atoll(argv[++i]); }
        else if (a == "--stress") do_stress = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--list") list = true;
        else if (const auto eq = a.find('='); eq != std::string::npos) {
            if (!set_field(g_cfg, a.substr(0, eq), std::atof(a.c_str() + eq + 1))) { std::fprintf(stderr, "unknown field %s\n", a.c_str()); return 2; }
        } else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    {
        FILE *f = std::fopen(argv[1], "r");
        if (!f) { std::perror(argv[1]); return 2; }
        char line[512];
        if (!std::fgets(line, sizeof line, f)) return 2;
        while (std::fgets(line, sizeof line, f)) {
            char src[256] = {}, name[256] = {}; unsigned a, b; int e;
            if (std::sscanf(line, "%255[^,],%u,%u,%d,%255[^\r\n]", src, &a, &b, &e, name) >= 4) {
                int idx = -1;
                for (size_t i = 0; i < g_sources.size(); ++i) if (g_sources[i] == src) idx = int(i);
                if (idx < 0) {
                    g_sources.push_back(src); idx = int(g_sources.size()) - 1;
                    g_data.emplace_back();
                    if (!load(base + src, idx, g_data.back())) { std::perror((base + src).c_str()); return 2; }
                    // A raw 400 Hz FIFO source X-raw.csv: X.csv and X-ph1.csv
                    // hold its frame pairs averaged at the two pair phases.
                    std::string s = src;
                    if (s.size() > 8 && s.compare(s.size() - 8, 8, "-raw.csv") == 0) {
                        Rec r;
                        if (load(base + s.substr(0, s.size() - 8) + ".csv", idx, r)) g_ph0[idx] = std::move(r);
                        Rec q;
                        if (load(base + s.substr(0, s.size() - 8) + "-ph1.csv", idx, q)) g_ph1[idx] = std::move(q);
                    }
                }
                const std::string nm = name;
                g_labels.push_back({idx, a, b, e, nm, nm.find("light") != std::string::npos && e > 0});
            }
        }
        std::fclose(f);
    }
    std::printf("detector %zu bytes, config %zu bytes, rules:", sizeof(player::TapGestureDetector), sizeof(player::TapGestureConfig));
    for (unsigned r = 0; r < g_rule_count; ++r) std::printf(" %s%s", g_rule_names[r], g_cfg.rules >> r & 1 ? "" : "(off)");
    std::printf("\nconfig:");
    print_fields(g_cfg);
    if (list) {
        for (size_t i = 0; i < g_data.size(); ++i)
            std::printf("source %zu %s: %zu samples%s%s\n", i, g_sources[i].c_str(), g_data[i].size(), g_ph0.count(int(i)) ? " +ph0" : "", g_ph1.count(int(i)) ? " +ph1" : "");
    }
    // Hits as recorded, for inspection.
    {
        for (size_t i = 0; i < g_data.size(); ++i)
            for (auto &h : run(g_data[i], g_cfg))
                if (verbose) std::printf("  hit %s at %s %lld\n", kind(h.kind), g_sources[h.src].c_str(), (long long)h.ot);
    }
    if (!trace_src.empty()) { trace(trace_src, trace_from, trace_to, g_cfg); return 0; }
    std::printf("\n== %s suite\n", suite.c_str());
    std::vector<Agg> aggs;
    for (auto &f : families(suite)) aggs.push_back(run_family(f, g_cfg));
    FILE *json = json_path.empty() ? nullptr : std::fopen(json_path.c_str(), "w");
    const Summary s = report(aggs, verbose, json);
    if (json) std::fclose(json);
    if (do_stress) stress(g_cfg);
    if (do_ablate) ablate(suite, g_cfg);
    return s.fp || s.wrong ? 1 : 0;
}
