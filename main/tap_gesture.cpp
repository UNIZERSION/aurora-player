#include "tap_gesture.h"

#include <cmath>

namespace player {
namespace {
uint32_t elapsed(uint32_t now, uint32_t since) { return now - since; }
float coefficient(uint32_t dt, float time_ms) { return float(dt) / (time_ms + float(dt)); }
}

TapGestureDetector::TapGestureDetector(const TapGestureConfig &config) : config_(config) {}

void TapGestureDetector::reset()
{
    ++stats_.resets;
    reset_state();
}

void TapGestureDetector::reset_state()
{
    initialized_ = false;
    settling_ = true;
    blocked_ = false;
    in_window_ = measured_ = quiet_ = sustained_ = window_quiet_ = false;
    taps_ = 0;
    envelope_ = activity_ = envelope_low_ = 0;
}

void TapGestureDetector::seed(uint32_t now_ms, const float a[3])
{
    reset_state();
    ++stats_.seeds;
    for (unsigned k = 0; k < 3; ++k) {
        gravity_[k] = a[k];
        smooth_[k] = previous_d_[k] = 0;
    }
    previous_ms_ = armed_ms_ = quiet_ms_ = now_ms;
    // Arming counts as activity: the first tap needs a full isolation period.
    last_active_ms_ = run_start_ms_ = last_onset_ms_ = light_end_ms_ = now_ms;
    run_peak_ = 0;
    still_before_ms_ = 0;
    moving_ = false;
    initialized_ = true;
}

TapGestureSnapshot TapGestureDetector::snapshot() const
{
    auto result = stats_;
    result.taps = taps_;
    result.settling = !initialized_ || settling_;
    result.blocked = blocked_;
    result.in_window = in_window_;
    return result;
}

void TapGestureDetector::cancel(uint32_t now_ms, TapRejectReason reason)
{
    ++stats_.rejected[static_cast<unsigned>(reason)];
    stats_.last_reject = reason;
    taps_ = 0;
    blocked_ = true;
    if (quiet_) quiet_ms_ = now_ms;
}

// Classify the impulse whose features were integrated over feature_ms.
void TapGestureDetector::measure(uint32_t now_ms)
{
    Impulse &p = impulse_;
    const float rms = std::sqrt(p.energy / float(config_.feature_ms));
    const float along = p.energy > 0 ? p.along / p.energy : 0;
    float axis[3];
    for (unsigned k = 0; k < 3; ++k) axis[k] = p.energy > 0 ? p.axis[k] / p.energy : 0;
    const float axial = axis[0];
    ++stats_.impulses;
    stats_.last_rms_g = rms;
    stats_.last_along = along;
    stats_.last_axial = axial;
    stats_.last_smooth_g = p.smooth;
    stats_.last_lead_g = p.lead;
    stats_.last_pre_smooth_g = p.pre_smooth;
    // sqrt(rad/ms) -> Hz: 1000 / (2 pi)
    const float hz = p.energy > 0 ? std::sqrt(p.jerk / p.energy) * 159.155f : 0;
    stats_.last_hz = hz;
    if (rms > stats_.peak_rms_g) stats_.peak_rms_g = rms;

    TapRejectReason reason = TapRejectReason::None;
    const bool cross = along <= config_.cross_max_along;
    p.gravity_tap = !cross;
    if (rule(TapRuleSmooth) && (p.smooth > config_.max_smooth_g || p.smooth > config_.max_smooth_ratio * rms))
        reason = TapRejectReason::Smooth;
    else if (rule(TapRuleAxial) && axial >= config_.max_axial)
        reason = TapRejectReason::Axial;
    else if (rule(TapRuleSharp) && hz < config_.min_hz)
        reason = TapRejectReason::Slow;
    else if (rms < config_.light_rms_g)
        reason = TapRejectReason::Weak;
    else if (!cross && rms < config_.tap_rms_g && rule(TapRuleVibration))
        reason = TapRejectReason::Vibration;

    if (reason != TapRejectReason::None) {
        // Not a tap. During a series any extra impulse (a fourth light tap,
        // handling, a knock) cancels it; otherwise it only delays the next
        // first tap through the isolation rule.
        if (taps_ && rule(TapRuleTrailing)) {
            cancel(now_ms, reason);
        } else {
            ++stats_.rejected[static_cast<unsigned>(reason)];
            stats_.last_reject = reason;
        }
        return;
    }
    if (!taps_) {
        if (rule(TapRuleLeadQuiet) && p.lead > config_.lead_g) { cancel(now_ms, TapRejectReason::NotQuietBefore); return; }
        if (rule(TapRuleIsolation) && p.quiet_since_ms < config_.isolation_ms) { cancel(now_ms, TapRejectReason::NotQuietBefore); return; }
        if (rule(TapRuleCensus) && (p.previous_onset_ms < config_.isolation_ms || p.previous_light_ms < config_.isolation_ms)) {
            cancel(now_ms, TapRejectReason::Census);
            return;
        }
        series_onsets_ = 0;
        unexplained_ = trail_suspect_ = false;
        all_ringing_ = true;
        ++series_;
        for (unsigned k = 0; k < 3; ++k) {
            series_gravity_[k] = gravity_[k];
            series_axis_[k] = axis[k];
        }
        strongest_ = weakest_ = rms;
        all_cross_ = cross;
    } else {
        // A later tap starts from a settled device: the low-frequency motion
        // just before it is at most the rebound of the earlier taps. A hand
        // that lifts or drops the device between two impulses (a knock, then
        // the bump of the landing) leaves it moving when the second arrives.
        if (rule(TapRuleSettle) && p.pre_smooth > config_.settle_ratio * strongest_) { cancel(now_ms, TapRejectReason::Unsettled); return; }
        const uint32_t interval = elapsed(p.onset_ms, last_tap_ms_);
        stats_.last_tap_interval_ms = interval;
        if (rule(TapRuleInterval) && interval < config_.min_interval_ms) { cancel(now_ms, TapRejectReason::TooFast); return; }
        if (rule(TapRuleInterval) && interval > config_.max_interval_ms) { cancel(now_ms, TapRejectReason::TooSlow); return; }
        if (taps_ >= 3) { cancel(now_ms, TapRejectReason::TooMany); return; }
        // The same finger on the same spot: the energy lands on the same
        // axes, and no tap is many times stronger than another.
        float spread = 0;
        for (unsigned k = 0; k < 3; ++k) spread += std::fabs(axis[k] - series_axis_[k]);
        spread *= 0.5f;
        stats_.last_spread = spread;
        const float strongest = rms > strongest_ ? rms : strongest_, weakest = rms < weakest_ ? rms : weakest_;
        if (rule(TapRuleConsistency) && (spread > config_.max_spread || strongest > config_.max_rms_ratio * weakest)) {
            cancel(now_ms, TapRejectReason::Inconsistent);
            return;
        }
        strongest_ = strongest;
        weakest_ = weakest;
        all_cross_ = all_cross_ && cross;
    }
    ++taps_;
    ++stats_.accepted_taps;
    last_tap_ms_ = p.onset_ms;
    p.tap = taps_;
    p.series = series_;
}

// Decided ring_to_ms after the onset, long before the series completes. A
// finger on the enclosure damps the tap; a knock on the table reaches the
// device through its feet and keeps ringing (table and stand modes, about
// 85 Hz on the stand) after the impulse. A tap rings when it is along
// gravity and the energy of (ring_from_ms, ring_to_ms] exceeds ring_ratio
// of the energy of its first feature_ms: a ratio, so independent of how hard
// it was. A fresh impact in that window (a double hit, a rattle that starts
// a new rise) makes the measurement void, which counts as not ringing.
void TapGestureDetector::check_ringing()
{
    Impulse &p = impulse_;
    if (!p.ring_pending) return;
    p.ring_pending = false;
    // A window cut short (split or closed early) is scaled from the part of
    // the tail it covers; a void (strong re-impact) is unknown.
    const uint32_t span = config_.ring_to_ms - config_.ring_from_ms;
    const float ratio = p.energy > 0 && p.tail_ms ? p.tail / p.energy * float(span) / float(p.tail_ms < span ? p.tail_ms : span) : 0;
    if (!p.ring_void) stats_.last_ring = ratio;
    const bool ringing = p.gravity_tap && !p.ring_void && ratio > config_.ring_ratio;
    if (p.tap && p.series == series_ && p.tap <= taps_ && !ringing) all_ringing_ = false;
}

TapGesture TapGestureDetector::complete(uint32_t now_ms)
{
    if (!taps_ || in_window_ || elapsed(now_ms, last_tap_ms_) < config_.completion_ms) return TapGesture::None;
    if (taps_ == 1) { taps_ = 0; return TapGesture::None; }
    // Every onset since the first tap must have become one of its taps: an
    // impulse that was not counted (a light fourth tap, a knock, a tap whose
    // features failed) means the series is not what the user tapped.
    if (rule(TapRuleCensus) && (series_onsets_ + 1 != taps_ || unexplained_)) {
        cancel(now_ms, TapRejectReason::Census);
        return TapGesture::None;
    }
    // Every tap of the series rang: knocks on the table next to the device.
    // One clean tap is enough, so a rattle after one tap costs nothing.
    if (rule(TapRuleRinging) && all_ringing_) { cancel(now_ms, TapRejectReason::Ringing); return TapGesture::None; }
    float dot = 0, a = 0, b = 0;
    for (unsigned k = 0; k < 3; ++k) {
        dot += gravity_[k] * series_gravity_[k];
        a += gravity_[k] * gravity_[k];
        b += series_gravity_[k] * series_gravity_[k];
    }
    const float limit = std::cos(config_.max_tilt_deg * 0.01745329f);
    if (rule(TapRuleTilt) && !(dot >= limit * std::sqrt(a * b))) { cancel(now_ms, TapRejectReason::Tilt); return TapGesture::None; }
    // A series along gravity needs one strong tap. A series across gravity
    // may be light, except while music plays: then its strongest tap must at
    // least reach the clear-tap level (speaker vibration stays far below).
    const float needed = !all_cross_ ? config_.strong_rms_g : playing_ ? config_.tap_rms_g : 0.0f;
    if (rule(TapRuleStrongTap) && strongest_ < needed) {
        cancel(now_ms, TapRejectReason::NoStrongTap);
        return TapGesture::None;
    }
    // Motion still going on at completion is the same evidence as sustained
    // motion: the series was part of handling, not taps on a resting device.
    if (rule(TapRuleSustain) && !quiet_) { cancel(now_ms, TapRejectReason::CompletionNotQuiet); return TapGesture::None; }
    const auto result = taps_ == 2 ? TapGesture::Double : TapGesture::Triple;
    if (result == TapGesture::Double) ++stats_.double_events;
    else ++stats_.triple_events;
    taps_ = 0;
    return result;
}

TapGesture TapGestureDetector::feed(uint32_t now_ms, float x_g, float y_g, float z_g)
{
    ++stats_.samples;
    const float a[3] = {x_g, y_g, z_g};
    if (!std::isfinite(x_g) || !std::isfinite(y_g) || !std::isfinite(z_g) ||
        std::fabs(x_g) > 64 || std::fabs(y_g) > 64 || std::fabs(z_g) > 64) {
        ++stats_.invalid_samples;
        reset_state();
        return TapGesture::None;
    }
    if (!initialized_) { seed(now_ms, a); return TapGesture::None; }
    const uint32_t dt = elapsed(now_ms, previous_ms_);
    if (dt == 0) { ++stats_.duplicate_samples; return TapGesture::None; }
    if (dt > stats_.max_sample_gap_ms) stats_.max_sample_gap_ms = dt;
    if (dt > config_.gap_ms) {
        // A gap, or a timestamp that went backwards (unsigned wrap), is a
        // discontinuity: nothing pending survives it.
        ++stats_.stale_samples;
        seed(now_ms, a);
        return TapGesture::None;
    }
    previous_ms_ = now_ms;

    // Dynamic part against a slow gravity estimate, its fast energy envelope,
    // a slower activity level, and a 30 ms low-pass (pushes, tilting).
    float d[3], m2 = 0;
    const float kg = coefficient(dt, config_.gravity_ms), ks = coefficient(dt, config_.smooth_ms);
    const float smooth_before = std::sqrt(smooth_[0] * smooth_[0] + smooth_[1] * smooth_[1] + smooth_[2] * smooth_[2]);
    float smooth2 = 0;
    for (unsigned k = 0; k < 3; ++k) {
        d[k] = a[k] - gravity_[k];
        m2 += d[k] * d[k];
        gravity_[k] += kg * d[k];
        smooth_[k] += ks * (d[k] - smooth_[k]);
        smooth2 += smooth_[k] * smooth_[k];
    }
    const float lead = std::sqrt(activity_);
    envelope_ += coefficient(dt, config_.envelope_ms) * (m2 - envelope_);
    activity_ += coefficient(dt, config_.activity_ms) * (m2 - activity_);
    const float envelope = std::sqrt(envelope_), smooth = std::sqrt(smooth2);
    if (envelope < config_.quiet_g) {
        if (!quiet_) quiet_ms_ = now_ms;
        quiet_ = true;
    } else {
        quiet_ = false;
    }
    // An impulse starts where the envelope reaches onset_g while rising
    // sharply above its recent low. A decaying tail of rocking and ringing,
    // however long, never starts another one; a fresh impact during it does.
    const bool active = envelope >= config_.onset_g;
    const bool onset = active && envelope >= config_.rise_factor * envelope_low_;
    if (envelope < envelope_low_) envelope_low_ = envelope;
    else envelope_low_ += coefficient(dt, config_.rise_ms) * (envelope - envelope_low_);
    // Isolation counts any motion above the still level, not only impulses:
    // a first tap too light to open an impulse still delays a series. The
    // stillness is measured up to where this run of motion began, so the
    // leading edge of the tap itself does not count against it.
    if (envelope >= config_.quiet_g) {
        if (!moving_) {
            still_before_ms_ = elapsed(now_ms, last_active_ms_);
            run_start_ms_ = now_ms;
            run_peak_ = 0;
        }
        moving_ = true;
        last_active_ms_ = now_ms;
        if (envelope > run_peak_) run_peak_ = envelope;
    } else {
        // Census also counts motion runs too light to open an impulse.
        if (moving_ && run_peak_ >= config_.trail_g) light_end_ms_ = now_ms;
        moving_ = false;
    }
    const uint32_t quiet_for = elapsed(now_ms, run_start_ms_) <= 30 ? still_before_ms_ : 0;
    float previous[3];
    for (unsigned k = 0; k < 3; ++k) { previous[k] = previous_d_[k]; previous_d_[k] = d[k]; }
    // Energy of the derivative of d over this sample: |delta d|^2 / dt.
    float step2 = 0;
    for (unsigned k = 0; k < 3; ++k) step2 += (d[k] - previous[k]) * (d[k] - previous[k]);
    const float jerk = step2 / float(dt);

    if (settling_) {
        if (onset) last_onset_ms_ = now_ms;  // Census: activity right after arming
        if (elapsed(now_ms, armed_ms_) >= config_.settle_ms && quiet_ && elapsed(now_ms, quiet_ms_) >= 150) settling_ = false;
        return TapGesture::None;
    }

    // Sustained smooth displacement (a push, lifting, walking) cancels.
    if (smooth >= config_.sustain_g) {
        if (!sustained_) smooth_ms_ = now_ms;
        sustained_ = true;
        if (rule(TapRuleSustain) && elapsed(now_ms, smooth_ms_) >= config_.sustain_ms) {
            if (taps_ || !blocked_) cancel(now_ms, TapRejectReason::SustainedMotion);
        }
    } else {
        sustained_ = false;
    }

    // Impulse windows: an onset opens one; its first feature_ms are measured,
    // the rest (rebound on the stand, ringing) belongs to the same tap unless
    // the device went still for split_quiet_ms and a new onset follows.
    if (in_window_) {
        const uint32_t age = elapsed(now_ms, impulse_.onset_ms);
        // A re-onset as strong as the tap itself (or retap_g) is another tap,
        // so light finger drumming cannot hide every other beat in a window.
        const float retap = std::fmin(config_.retap_g, std::sqrt(impulse_.energy / float(config_.feature_ms)));
        if (impulse_.ring_pending) {
            // A light onset (a rattle on a knock) is part of the tail and
            // cannot make a knock look damped. A strong re-impact (half the
            // tap or more) ends the measurement: after rocking (the rebound
            // of a device landing back on its support) as unknown, which
            // counts as not ringing; otherwise the tail collected so far,
            // scaled to the window, decides (unknown before 20 ms of tail).
            if (measured_ && age > config_.feature_ms && onset &&
                envelope >= 0.5f * std::sqrt(impulse_.energy / float(config_.feature_ms))) {
                impulse_.ring_void = smooth_before >= config_.rebound_smooth_g || impulse_.tail_ms < 20;
                check_ringing();
            }
            else if (age > config_.ring_from_ms && age <= config_.ring_to_ms) {
                impulse_.tail += m2 * float(dt);
                impulse_.tail_ms += dt;
            }
            else if (age > config_.ring_to_ms && measured_) check_ringing();
        }
        if (!measured_ && age <= config_.feature_ms) {
            float along = 0, norm = 0;
            for (unsigned k = 0; k < 3; ++k) { along += d[k] * gravity_[k]; norm += gravity_[k] * gravity_[k]; }
            along = norm > 0 ? along * along / norm : 0;
            impulse_.energy += m2 * float(dt);
            impulse_.along += along * float(dt);
            impulse_.jerk += jerk;
            for (unsigned k = 0; k < 3; ++k) impulse_.axis[k] += d[k] * d[k] * float(dt);
            if (smooth > impulse_.smooth) impulse_.smooth = smooth;
        } else if (!measured_) {
            measured_ = true;
            if (!blocked_) measure(now_ms);
        }
        if (measured_) {
            if (envelope < config_.quiet_g) {
                if (!window_quiet_) window_quiet_ms_ = now_ms;
                window_quiet_ = true;
            } else if (onset && ((window_quiet_ && elapsed(now_ms, window_quiet_ms_) >= config_.split_quiet_ms) ||
                                 (age >= config_.rebound_min_ms && envelope >= retap &&
                                  smooth_before < config_.rebound_smooth_g))) {
                // A separate onset inside the window: a tap too fast to be
                // part of the series (finger drumming), or other activity.
                if (taps_ && rule(TapRuleSplit)) cancel(now_ms, TapRejectReason::TooFast);
                in_window_ = false;
            } else {
                window_quiet_ = false;
            }
        }
        if (in_window_ && age >= config_.window_ms) { in_window_ = false; check_ringing(); }
    }
    if (!in_window_ && onset) {
        if (impulse_.ring_pending) { impulse_.ring_void = true; check_ringing(); }
        // The light motion just before this onset was its leading edge.
        if (trail_suspect_ && trail_run_ms_ == run_start_ms_) trail_suspect_ = false;
        impulse_ = Impulse{};
        impulse_.onset_ms = now_ms;
        impulse_.lead = lead;
        impulse_.quiet_since_ms = quiet_for;
        impulse_.previous_onset_ms = elapsed(now_ms, last_onset_ms_);
        impulse_.previous_light_ms = elapsed(now_ms, light_end_ms_);
        last_onset_ms_ = now_ms;
        if (taps_) ++series_onsets_;
        // The sample before the onset carries the leading edge.
        float along = 0, norm = 0, p2 = 0;
        for (unsigned k = 0; k < 3; ++k) {
            along += previous[k] * gravity_[k];
            norm += gravity_[k] * gravity_[k];
            p2 += previous[k] * previous[k];
        }
        impulse_.energy = (p2 + m2) * float(dt);
        float a2 = 0;
        for (unsigned k = 0; k < 3; ++k) a2 += d[k] * gravity_[k];
        impulse_.along = norm > 0 ? (along * along + a2 * a2) / norm * float(dt) : 0;
        for (unsigned k = 0; k < 3; ++k) impulse_.axis[k] = (previous[k] * previous[k] + d[k] * d[k]) * float(dt);
        impulse_.smooth = smooth;
        impulse_.jerk = jerk;
        impulse_.pre_smooth = smooth_before;
        impulse_.ring_pending = true;
        in_window_ = true;
        measured_ = window_quiet_ = false;
    }

    if (taps_ && !in_window_) {
        if (envelope >= config_.census_g) unexplained_ = true;
        else if (!trail_suspect_ && envelope >= config_.trail_g && moving_ && still_before_ms_ >= 40 &&
                 int32_t(run_start_ms_ - last_tap_ms_) >= int32_t(config_.window_ms)) {
            trail_suspect_ = true;
            trail_run_ms_ = run_start_ms_;
        }
    }
    if (trail_suspect_ && elapsed(now_ms, trail_run_ms_) > 30) {
        trail_suspect_ = false;
        if (taps_) unexplained_ = true;
    }
    if (blocked_) {
        if (quiet_ && elapsed(now_ms, quiet_ms_) >= config_.blocked_quiet_ms) blocked_ = false;
        return TapGesture::None;
    }
    return complete(now_ms);
}

} // namespace player
