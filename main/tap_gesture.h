#pragma once

#include <cstdint>

namespace player {

enum class TapGesture : uint8_t { None, Double, Triple };

// Why an impulse was not counted, or why a pending series was cancelled.
// Indices 0-7 keep their 1.3.18 meaning in the worker's REJECT log where the
// mechanism still exists; the rest are appended.
enum class TapRejectReason : uint8_t {
    None, Smooth, TooFast, TooSlow, Vibration, TooMany,
    SustainedMotion, CompletionNotQuiet, NotQuietBefore, Tilt, Axial,
    Weak, NoStrongTap, Inconsistent, Census, Unsettled, Slow, Ringing, Count
};

// Independent rejection rules, one bit each in TapGestureConfig::rules. The
// evaluation harness (tools/tap-eval) disables them one at a time to check
// that every recorded negative is stopped by at least two of them in every
// evaluated family (amplitude up to 1.5x); harder table knocks, 2-3x, are
// stopped by Ringing.
#define TAP_GESTURE_RULES(X) \
    X(Smooth)      /* impulse is a push/handling: smooth part too large */ \
    X(Vibration)   /* weak impulse along gravity: table vibration */ \
    X(Axial)       /* impulse on the device's long axis: edge buttons */ \
    X(LeadQuiet)   /* first tap needs a still device before it */ \
    X(Isolation)   /* no other impulse shortly before the first tap */ \
    X(Interval)    /* onset-to-onset cadence */ \
    X(Split)       /* a separate onset inside a tap window: too fast */ \
    X(Trailing)    /* any impulse after the last tap cancels */ \
    X(Tilt)        /* orientation must not change during a series */ \
    X(Sustain)     /* sustained motion cancels */ \
    X(StrongTap)   /* a series needs one clear tap unless all are cross */ \
    X(Consistency) /* every tap of a series excites the same axes */ \
    X(Census)      /* impulses counted from isolation_ms before the series = taps */ \
    X(Settle)      /* a later tap starts from a device settled after the earlier ones */ \
    X(Sharp)       /* an impact is brief and broadband; a press or push is slow */ \
    X(Ringing)     /* every tap of the series kept ringing: knocks on the table */

enum TapRule : uint32_t {
#define TAP_GESTURE_RULE_ENUM(name) TapRule##name,
    TAP_GESTURE_RULES(TAP_GESTURE_RULE_ENUM)
#undef TAP_GESTURE_RULE_ENUM
    TapRuleCount
};
constexpr uint32_t TapAllRules = (1u << TapRuleCount) - 1u;

// Telemetry read from the task that feeds the detector. Counters survive
// reset so a screen transition cannot erase the evidence of an attempt.
struct TapGestureSnapshot {
    uint32_t samples = 0;
    uint32_t resets = 0;
    uint32_t seeds = 0;
    uint32_t invalid_samples = 0;
    uint32_t stale_samples = 0;
    uint32_t duplicate_samples = 0;
    uint32_t impulses = 0;          // onsets measured
    uint32_t accepted_taps = 0;     // impulses counted as taps
    uint32_t double_events = 0;
    uint32_t triple_events = 0;
    uint32_t rejected[static_cast<unsigned>(TapRejectReason::Count)] = {};
    uint32_t max_sample_gap_ms = 0;
    uint32_t last_tap_interval_ms = 0;
    float peak_rms_g = 0.0f;        // strongest impulse RMS since boot
    float last_rms_g = 0.0f;        // features of the last measured impulse
    float last_along = 0.0f;
    float last_axial = 0.0f;
    float last_spread = 0.0f;
    float last_pre_smooth_g = 0.0f; // |30 ms low-pass| just before the onset
    float last_hz = 0.0f;           // RMS frequency of the impulse
    float last_ring = 0.0f;         // ringing ratio of the last checked impulse
    float last_smooth_g = 0.0f;
    float last_lead_g = 0.0f;
    TapRejectReason last_reject = TapRejectReason::None;
    uint8_t taps = 0;
    bool settling = true;
    bool blocked = false;
    bool in_window = false;
};

// Every field that the evaluation harness may override (key=value).
#define TAP_GESTURE_TUNABLES(X) \
    X(float, onset_g) X(float, rise_factor) X(float, quiet_g) X(float, tap_rms_g) X(float, light_rms_g) X(float, strong_rms_g) \
    X(float, cross_max_along) X(float, max_axial) X(float, max_spread) X(float, max_rms_ratio) X(float, max_smooth_ratio) X(float, max_smooth_g) \
    X(float, lead_g) X(float, settle_ratio) X(float, min_hz) X(float, sustain_g) X(float, max_tilt_deg) X(float, gravity_ms) X(float, envelope_ms) \
    X(float, activity_ms) X(float, smooth_ms) X(float, rise_ms) \
    X(float, rebound_smooth_g) X(float, retap_g) X(float, census_g) X(float, trail_g) X(uint32_t, rebound_min_ms) \
    X(uint32_t, feature_ms) X(uint32_t, window_ms) X(uint32_t, split_quiet_ms) X(uint32_t, isolation_ms) \
    X(uint32_t, min_interval_ms) X(uint32_t, max_interval_ms) X(uint32_t, completion_ms) X(uint32_t, sustain_ms) \
    X(uint32_t, settle_ms) X(uint32_t, blocked_quiet_ms) X(uint32_t, gap_ms) \
    X(float, ring_ratio) X(uint32_t, ring_from_ms) X(uint32_t, ring_to_ms)

// Acceleration in g, timestamps in ms. The firmware feeds every 400 Hz
// BMI270 FIFO frame, stamped in whole ms (2/3 ms steps, sleep_tap_input.cpp);
// every window and
// filter is defined in ms and every amplitude feature integrates energy over
// time. At 150-200 Hz, with jitter or lost samples no false gesture was
// found in the evaluation, but fewer gestures are recognised (about 50-75 %
// of the 400 Hz recall at 150-200 Hz).
struct TapGestureConfig {
    float onset_g = 0.08f;          // energy envelope that opens an impulse,
    float rise_factor = 2.5f;       // when it also rose this much above its recent low
    float quiet_g = 0.04f;          // envelope below which the device is still
    // Impulse RMS over feature_ms. A tap across gravity (on a side) counts
    // from light_rms_g. Along gravity, a knock on the table under the device
    // has the same direction as a tap on its top, so such taps must reach
    // tap_rms_g, and their series needs one tap of strong_rms_g. The Ringing
    // rule tells them apart by their decay; 0.33 is the lowest tap_rms_g at
    // which amplitude alone still stops every recorded table series in every
    // evaluated family (at 0.325 a 1.5x table triple passes), so each table
    // series stays stopped by two independent rules.
    float tap_rms_g = 0.33f;
    float light_rms_g = 0.10f;
    float strong_rms_g = 0.60f;
    float cross_max_along = 0.50f;  // energy fraction along gravity of a cross tap
    float max_axial = 0.85f;        // energy fraction on the device X (long) axis
    float max_spread = 0.50f;       // axis-energy distance from the first tap
    float max_rms_ratio = 8.0f;     // strongest / weakest tap of a series
    float max_smooth_ratio = 0.65f; // |30 ms low-pass| peak / impulse RMS
    float max_smooth_g = 1.50f;
    float lead_g = 0.08f;           // activity RMS before the first tap
    // Low-frequency motion (|30 ms low-pass|) at the onset of a later tap, at
    // most this times the strongest earlier tap. A tap's own rebound stays
    // below it (up to 0.57 seen: hard fast taps on a device bouncing on a
    // table); a device lifted or dropped by a hand between a knock and the
    // landing bump does not (1.6 and more: partial free fall).
    float settle_ratio = 1.0f;
    // RMS frequency of the impulse, sqrt(energy of d/dt / energy of d) over
    // feature_ms. A finger or nail hitting the shell is a contact of a few
    // ms: taps measure >= 57 Hz in the 400 Hz FIFO frames (>= 12 Hz in old
    // recordings resampled to 150 Hz). Pressing a volume button or pushing
    // the device lasts tens of ms (7-10 Hz).
    float min_hz = 10.0f;
    float sustain_g = 0.20f;        // smooth displacement held for sustain_ms
    float max_tilt_deg = 8.0f;
    float gravity_ms = 400.0f;
    float envelope_ms = 8.0f;
    float activity_ms = 120.0f;
    float smooth_ms = 30.0f;
    float rise_ms = 40.0f;          // how fast the recent low follows a rising envelope
    uint32_t feature_ms = 40;       // impulse features: first 40 ms after onset
    uint32_t window_ms = 230;       // a tap's own rebound and ringing
    uint32_t split_quiet_ms = 80;   // stillness inside a window that splits it
    // A sharp new onset inside a window, at least rebound_min_ms after the
    // tap, is a separate (too fast) tap unless the device was rocking before
    // it (smooth displacement >= rebound_smooth_g): that is the rebound of a
    // device that rocked on its stand and landed again.
    uint32_t rebound_min_ms = 100;
    float rebound_smooth_g = 0.15f;
    float retap_g = 0.50f;          // envelope of such an onset, or the tap's own RMS if lower
    // After the last tap's window, a new motion run that starts from at least
    // 40 ms of stillness, reaches trail_g and does not open an impulse within
    // 30 ms (it is not just the leading edge of the next tap) is an impulse
    // too light to open one (a soft fourth tap): the series cannot be counted
    // (Census). The recorded taps' own late blips stay below 0.05 g.
    float trail_g = 0.06f;
    // During a series, motion this strong outside every tap window that did
    // not start an impulse (e.g. a tap whose onset was lost in ringing) is
    // unexplained: the series cannot be counted (Census).
    float census_g = 0.60f;
    uint32_t isolation_ms = 650;    // no impulse this long before the first tap
    uint32_t min_interval_ms = 235; // recorded series: 235-541 ms onset to onset
    uint32_t max_interval_ms = 600;
    uint32_t completion_ms = 650;   // after the last onset, then emit
    uint32_t sustain_ms = 150;
    uint32_t settle_ms = 500;
    uint32_t blocked_quiet_ms = 400;
    uint32_t gap_ms = 40;           // a longer sample gap is a discontinuity
    // Ringing: energy in (ring_from_ms, ring_to_ms] after an along-gravity
    // onset, relative to the energy of its first feature_ms. A tap on the
    // enclosure is damped by the finger (accepted taps at most 0.006); a knock on
    // the table next to the device reaches it through its feet and rings on
    // at about 85 Hz (at least 0.016). A series is rejected only when every
    // one of its taps rang, so one clean tap is enough.
    float ring_ratio = 0.011f;
    uint32_t ring_from_ms = 50;
    uint32_t ring_to_ms = 150;
    uint32_t rules = TapAllRules;
};

// This detector does not authorize transport commands. The caller must feed
// it only while the display is off AND PIN locking is enabled, and reset it
// whenever either condition changes or the sensor stream is discontinuous.
// Pending taps never survive reset, settling, gaps or invalid samples.
class TapGestureDetector {
public:
    explicit TapGestureDetector(const TapGestureConfig &config = {});
    void reset();
    TapGesture feed(uint32_t now_ms, float x_g, float y_g, float z_g);
    TapGestureSnapshot snapshot() const;
    // While music plays, the player's own speaker can shake the enclosure
    // (felt across gravity when the device stands upright), so a series
    // across gravity then needs one tap of at least tap_rms_g; light
    // cross-gravity series count only while nothing plays. Takes effect at
    // the next completion.
    void set_playing(bool playing) { playing_ = playing; }

private:
    struct Impulse {
        uint32_t onset_ms = 0;
        float energy = 0, along = 0, axis[3] = {}, smooth = 0, lead = 0;
        uint32_t quiet_since_ms = 0;
        uint32_t previous_onset_ms = 0;  // since the onset before this one
        uint32_t previous_light_ms = 0;  // since a motion run that reached trail_g ended
        float pre_smooth = 0;            // |low-pass| on the sample before the onset
        float jerk = 0;                  // sum |delta d|^2 / dt: energy of the derivative
        float tail = 0;                  // energy in the ringing window
        uint32_t tail_ms = 0;            // how much of that window it covers
        uint8_t tap = 0;                 // series position if counted as a tap
        uint8_t series = 0;              // which series it was counted in
        bool gravity_tap = false;        // measured as an along-gravity impulse
        bool ring_pending = false;
        bool ring_void = false;          // a strong re-impact inside the window
        bool split = false;
    };
    void reset_state();
    void seed(uint32_t now_ms, const float a[3]);
    bool rule(TapRule r) const { return (config_.rules >> r) & 1u; }
    void cancel(uint32_t now_ms, TapRejectReason reason);
    void measure(uint32_t now_ms);
    void check_ringing();
    TapGesture complete(uint32_t now_ms);

    TapGestureConfig config_;
    TapGestureSnapshot stats_;
    float gravity_[3] = {};
    float series_gravity_[3] = {};
    float smooth_[3] = {};
    float previous_d_[3] = {};
    float envelope_ = 0, activity_ = 0, envelope_low_ = 0;
    uint32_t previous_ms_ = 0, armed_ms_ = 0;
    uint32_t last_onset_ms_ = 0;        // latest impulse onset of any kind
    uint32_t series_onsets_ = 0;        // impulse onsets since the first tap
    uint32_t last_active_ms_ = 0;       // latest sample above the still level
    uint32_t run_start_ms_ = 0;         // start of the current run above it
    uint32_t still_before_ms_ = 0;      // stillness that preceded that run
    uint32_t light_end_ms_ = 0;         // end of the latest run that reached trail_g
    float run_peak_ = 0;                // strongest envelope of the current run
    uint32_t last_tap_ms_ = 0;          // onset of the latest counted tap
    uint32_t quiet_ms_ = 0;             // start of the current still period
    uint32_t smooth_ms_ = 0;            // start of the current sustained period
    uint32_t window_quiet_ms_ = 0;
    Impulse impulse_;
    float strongest_ = 0, weakest_ = 0;
    float series_axis_[3] = {};
    uint8_t taps_ = 0;
    bool initialized_ = false, settling_ = true, blocked_ = false;
    bool in_window_ = false, measured_ = false, quiet_ = false, sustained_ = false;
    bool window_quiet_ = false, all_cross_ = true, moving_ = false, unexplained_ = false, playing_ = false;
    bool all_ringing_ = false;          // every tap of the series rang so far
    bool trail_suspect_ = false;        // light motion after the last tap window...
    uint32_t trail_run_ms_ = 0;         // ...in the motion run that started here
    uint8_t series_ = 0;                // ties a late ringing check to its series
};

} // namespace player
