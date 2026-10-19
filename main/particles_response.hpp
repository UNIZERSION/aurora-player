#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

// PARTICLES-only response to the existing normalized, pre-volume FFT bands.
// No allocation, random motion, tempo clock, audio mutation or peak normalization.
namespace aurora_particles {
constexpr float pi = 3.14159265358979323846f;
inline float unit(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
}
inline float drive(float value) {
    value = unit(value);
    // Modest contrast lift, preserving exact silence and full-scale endpoints.
    return value * (1.25f - .25f * value);
}
struct Point {
    float x, y, radius, alpha, glow_alpha, core_alpha;
};

template<std::size_t Bands> struct Response {
    static_assert(Bands >= 2, "Particle interpolation needs at least two bands");
    float bands[Bands]{};
    float bass = 0, bed = 0, pulse = 0, phase = 0;

    void advance(const float *input, std::size_t count, float dt, bool playing) {
        if (!std::isfinite(dt) || dt <= 0) return;
        dt = std::min(dt, .050f); // Same long-frame bound as the existing UI.
        const float attack = 1 - std::exp(-dt / .055f);
        const float release = 1 - std::exp(-dt / .180f);
        constexpr std::size_t low_count = Bands < 12 ? Bands : 12;
        float target = 0;
        for (std::size_t i = 0; i < Bands; ++i) {
            const float value = drive(input && count ? input[i * count / Bands] : 0);
            bands[i] += (value - bands[i]) * (value > bands[i] ? attack : release);
            if (i < low_count) target += value / low_count;
        }
        bass += (target - bass) * (1 - std::exp(-dt / (target > bass ? .075f : .220f)));
        // The difference from a slower level follows real bass onsets. It falls
        // to zero for a steady note and never fabricates beats during silence.
        bed += (target - bed) * (1 - std::exp(-dt / .400f));
        const float onset = std::clamp((bass - bed) * 1.6f, 0.f, .25f);
        pulse += (onset - pulse) * (onset > pulse ? attack : release);
        if (playing) phase = std::fmod(phase + dt * (.75f + .65f * bass), 4 * pi);
    }

    Point point(unsigned lane, unsigned index) const {
        const float u = std::min(index, 43u) / 43.f;
        const float position = u * (Bands - 1);
        const auto band = std::min(Bands - 2, std::size_t(position));
        const float mix = position - band;
        const float energy = bands[band] * (1 - mix) + bands[band + 1] * mix;
        const float angle = u * 2 * pi + phase + (lane % 3) * (2 * pi / 3);
        const float depth = (std::cos(angle) + 1) * .5f;
        const float envelope = std::sin(u * pi);
        const float alpha = std::min(255.f, 40 + depth * 55 + energy * 160 + pulse * 80);
        return {
            15 + u * 422 + std::cos(angle) * 3 * envelope,
            55 + std::sin(angle) * (9 + 25 * bass + 10 * energy + 3 * pulse) * envelope
               + std::sin(angle * 2 - phase * .5f) * 3.5f * energy,
            1.f + depth * .7f + energy * 1.65f + pulse * .75f,
            alpha,
            alpha * (.14f + .10f * pulse),
            std::clamp((energy - .55f) * 120 + pulse * 75, 0.f, 100.f),
        };
    }
};
} // namespace aurora_particles
