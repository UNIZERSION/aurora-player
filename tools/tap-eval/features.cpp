// Per-impulse feature dump for tap detector design.
// usage: features LABELS BASE_DIR [onset_g=0.08] > features.csv
// Every impulse onset (energy envelope >= onset_g after the previous impulse
// window has closed) in every source is listed with the label window it falls
// in (or none), and phase-robust features measured over the first 40 ms:
//   rms40/rms15  sqrt(sum |d|^2 dt / T): equivalent RMS amplitude in g
//   pk           largest single-sample |d| (phase dependent, for reference)
//   along        fraction of the 40 ms energy along gravity
//   cross        RMS of the cross-gravity component over 40 ms
//   xfrac        fraction of the energy on the device X (long) axis
//   conc         energy share of the first 12 ms
//   smooth       peak |30 ms low-pass of d| (handling/push measure)
//   quiet        ms since the envelope last exceeded 0.05 g before this onset
//   lead         RMS activity (150 ms envelope) just before the onset
//   rebound      peak envelope in 40..230 ms after onset, and its delay
//   swings       sign changes of the dominant-axis component >= 30 % of peak
//   tilt         degrees between gravity now and 1 s before
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct S { double t; float a[3]; };
struct Label { std::string src; double start, end; int expected; std::string name; };

static std::vector<S> load(const std::string &path)
{
    std::vector<S> v;
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) { std::perror(path.c_str()); std::exit(2); }
    char line[128];
    if (!std::fgets(line, sizeof line, f)) std::exit(2);
    unsigned t; int x, y, z;
    while (std::fscanf(f, "%u,%d,%d,%d", &t, &x, &y, &z) == 4) v.push_back({double(t), {x / 1000.f, y / 1000.f, z / 1000.f}});
    std::fclose(f);
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const std::string base = std::string(argv[2]) + "/";
    const double onset_g = argc > 3 ? std::atof(argv[3]) : 0.08;
    std::vector<Label> labels;
    std::vector<std::string> sources;
    {
        FILE *f = std::fopen(argv[1], "r");
        char line[512];
        if (!f || !std::fgets(line, sizeof line, f)) return 2;
        while (std::fgets(line, sizeof line, f)) {
            char src[256] = {}, name[256] = {}; unsigned a, b; int e;
            if (std::sscanf(line, "%255[^,],%u,%u,%d,%255[^\r\n]", src, &a, &b, &e, name) >= 4) {
                labels.push_back({src, double(a), double(b), e, name});
                if (std::find(sources.begin(), sources.end(), src) == sources.end()) sources.push_back(src);
            }
        }
        std::fclose(f);
    }
    std::printf("source,t0,expected,label,rms40,rms15,pk,along,cross,xfrac,conc,smooth,quiet,lead,rebound,rebound_ms,swings,tilt,dt_med,first_sign,peak_sign,sum10,hf\n");
    for (const auto &src : sources) {
        const auto v = load(base + src);
        const size_t n = v.size();
        double g[3] = {v[0].a[0], v[0].a[1], v[0].a[2]};
        double e2 = 0, act = 0, lp[3] = {0, 0, 0};
        double last_active = v[0].t, window_end = -1e18;
        std::vector<double> gx(n), gy(n), gz(n), env(n), actv(n), smv(n);
        std::vector<std::array<float, 3>> d(n);
        for (size_t i = 0; i < n; ++i) {
            const double dt = i ? v[i].t - v[i - 1].t : 5;
            if (dt > 80) { // recording gap: reseed
                for (int k = 0; k < 3; ++k) { g[k] = v[i].a[k]; lp[k] = 0; }
                e2 = act = 0;
            }
            const bool frozen = v[i].t < window_end;
            double m2 = 0, dd[3];
            for (int k = 0; k < 3; ++k) { dd[k] = v[i].a[k] - g[k]; m2 += dd[k] * dd[k]; }
            if (!frozen) for (int k = 0; k < 3; ++k) g[k] += std::min(1.0, dt / (500 + dt)) * dd[k];
            e2 += std::min(1.0, dt / (10 + dt)) * (m2 - e2);
            act += std::min(1.0, dt / (150 + dt)) * (m2 - act);
            for (int k = 0; k < 3; ++k) lp[k] += std::min(1.0, dt / (30 + dt)) * (dd[k] - lp[k]);
            gx[i] = g[0]; gy[i] = g[1]; gz[i] = g[2];
            env[i] = std::sqrt(e2); actv[i] = std::sqrt(act); smv[i] = std::sqrt(lp[0] * lp[0] + lp[1] * lp[1] + lp[2] * lp[2]);
            for (int k = 0; k < 3; ++k) d[i][k] = float(dd[k]);
            if (env[i] >= onset_g && v[i].t >= window_end) window_end = v[i].t + 230;
        }
        // Second pass: onsets and features.
        window_end = -1e18;
        last_active = v[0].t;
        for (size_t i = 1; i < n; ++i) {
            if (v[i].t - v[i - 1].t > 80) { window_end = -1e18; last_active = v[i].t; continue; }
            if (!(env[i] >= onset_g && v[i].t >= window_end)) {
                if (env[i] >= 0.05) last_active = v[i].t;
                continue;
            }
            const double t0 = v[i].t;
            window_end = t0 + 230;
            const size_t s0 = i - 1;
            double gv[3] = {gx[s0], gy[s0], gz[s0]};
            const double gn = std::sqrt(gv[0] * gv[0] + gv[1] * gv[1] + gv[2] * gv[2]);
            for (auto &q : gv) q /= gn;
            double J = 0, J15 = 0, J12 = 0, Ja = 0, Jx = 0, pk = 0, sm = 0, axe[3] = {0, 0, 0};
            size_t j = s0;
            for (; j < n && v[j].t <= t0 + 40; ++j) {
                const double dt = j ? std::min(10.0, v[j].t - v[j - 1].t) : 5;
                double m2 = 0, al = 0;
                for (int k = 0; k < 3; ++k) { m2 += d[j][k] * d[j][k]; al += d[j][k] * gv[k]; axe[k] += d[j][k] * d[j][k] * dt; }
                J += m2 * dt; Ja += al * al * dt; Jx += d[j][0] * d[j][0] * dt;
                if (v[j].t <= t0 + 15) J15 += m2 * dt;
                if (v[j].t <= t0 + 12) J12 += m2 * dt;
                pk = std::max(pk, std::sqrt(m2));
                sm = std::max(sm, smv[j]);
            }
            const int dom = int(std::max_element(axe, axe + 3) - axe);
            double dpk = 0;
            for (size_t q = s0; q < j; ++q) dpk = std::max(dpk, double(std::fabs(d[q][dom])));
            int swings = 0, sign = 0;
            for (size_t q = s0; q < j; ++q) {
                const double val = d[q][dom];
                if (std::fabs(val) < 0.3 * dpk) continue;
                const int sg = val > 0 ? 1 : -1;
                if (sign && sg != sign) ++swings;
                sign = sg;
            }
            // Signed along-gravity motion: first clear swing, largest swing,
            // and the 10 ms integral (g*ms), positive = along gravity (down).
            double amax = 0;
            for (size_t q = s0; q < j; ++q) {
                const double al = d[q][0] * gv[0] + d[q][1] * gv[1] + d[q][2] * gv[2];
                amax = std::max(amax, std::fabs(al));
            }
            int first_sign = 0, peak_sign = 0;
            double sum10 = 0, best = 0;
            for (size_t q = s0; q < j; ++q) {
                const double al = d[q][0] * gv[0] + d[q][1] * gv[1] + d[q][2] * gv[2];
                if (!first_sign && std::fabs(al) >= 0.5 * amax) first_sign = al > 0 ? 1 : -1;
                if (std::fabs(al) > best) { best = std::fabs(al); peak_sign = al > 0 ? 1 : -1; }
                if (v[q].t <= t0 + 10) sum10 += al * (q ? std::min(10.0, v[q].t - v[q - 1].t) : 5);
            }
            // High-frequency share: first-difference energy over energy
            // (2 for an isolated spike or 50 Hz, up to 4 at the Nyquist rate).
            double dif = 0, en = 0;
            for (size_t q = s0 + 1; q < j; ++q)
                for (int k = 0; k < 3; ++k) { const double df = d[q][k] - d[q - 1][k]; dif += df * df; en += d[q][k] * d[q][k]; }
            const double hf = en > 0 ? dif / en : 0;
            double reb = 0, reb_t = 0;
            for (size_t q = j; q < n && v[q].t <= t0 + 230; ++q) if (env[q] > reb) { reb = env[q]; reb_t = v[q].t - t0; }
            // tilt vs 1 s before
            size_t b = i;
            while (b > 0 && v[b].t > t0 - 1000) --b;
            const double g1[3] = {gx[b], gy[b], gz[b]};
            const double n1 = std::sqrt(g1[0] * g1[0] + g1[1] * g1[1] + g1[2] * g1[2]);
            const double cosang = std::max(-1.0, std::min(1.0, (g1[0] * gv[0] + g1[1] * gv[1] + g1[2] * gv[2]) / n1));
            std::vector<double> dts;
            for (size_t q = s0 + 1; q < j; ++q) dts.push_back(v[q].t - v[q - 1].t);
            std::sort(dts.begin(), dts.end());
            std::string lab = "";
            int exp = -9;
            for (auto &l : labels)
                if (l.src == src && t0 >= l.start - 300 && t0 <= l.end) { lab = l.name; exp = l.expected; }
            for (auto &c : lab) if (c == ',') c = ';';
            const double T = 40;
            std::printf("%s,%.0f,%d,%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.0f,%.3f,%.3f,%.0f,%d,%.1f,%.1f,%d,%d,%.2f,%.2f\n", src.c_str(), t0, exp, lab.c_str(),
                        std::sqrt(J / T), std::sqrt(J15 / 15), pk, J > 0 ? Ja / J : 0, std::sqrt(std::max(0.0, J - Ja) / T), J > 0 ? Jx / J : 0,
                        J > 0 ? J12 / J : 0, sm, t0 - last_active, actv[s0], reb, reb_t, swings, std::acos(cosang) * 57.2958,
                        dts.empty() ? 0 : dts[dts.size() / 2], first_sign, peak_sign, sum10, hf);
            last_active = t0;
        }
    }
    return 0;
}
