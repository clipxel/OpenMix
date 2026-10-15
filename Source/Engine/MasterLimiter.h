#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <deque>
#include <vector>

namespace om
{

/** Transparent brick-wall limiter for the master bus.

    • True-peak detection: inter-sample peaks estimated with a 4× polyphase
      interpolator, so the D/A output never exceeds the ceiling.
    • Look-ahead: gain reduction is in place *before* the peak arrives (sliding
      minimum + matched box filter ⇒ no overshoot, no clicks).
    • Soft knee in the last 1.5 dB under the ceiling: peaks are rounded into it instead of being
      flattened against a wall.
    • Program-dependent release (as in mastering limiters): an isolated transient recovers in tens
      of milliseconds, so a kick that touches the ceiling does not duck the music after it; while
      limiting is sustained the release slows down, so the bass does not pump or distort.
    • Stereo-linked. Below the knee it is a pure delay (gain 1, bit-exact). */
class MasterLimiter
{
public:
    std::atomic<bool>  enabled { true };
    std::atomic<float> ceilingDb { -0.5f };     // dBTP
    std::atomic<float> releaseMs { 40.0f };     // fastest release (isolated peaks)
    static constexpr float slowReleaseMs = 600.0f, kneeDb = 1.5f;

    void prepare (double sampleRate, int maxBlock);
    void process (float* l, float* r, int n);

    int latencySamples() const noexcept           { return delay; }
    /** Largest gain reduction (dB, ≥ 0) since last call — for the meter. */
    float drainReductionDb() noexcept             { return reductionDb.exchange (0.0f); }

private:

    double sr = 48000.0;
    int lookahead = 72, tpDelay = 8, delay = 80;

    // Audio delay lines
    std::vector<float> dl, dr;
    int writePos = 0;

    // True-peak FIR history per channel
    static constexpr int tpTaps = 16, tpPhases = 4;
    std::array<std::array<float, tpTaps>, tpPhases - 1> tpCoeffs {};
    std::vector<float> histL, histR;
    int histPos = 0;

    // Sliding minimum (monotonic deque of (index, value)) + box filter
    std::deque<std::pair<juce::int64, float>> minQ;
    std::vector<float> box;
    double boxSum = 0.0;
    int boxPos = 0;
    juce::int64 sampleIndex = 0;

    float gain = 1.0f;
    float sustained = 0.0f;                     // share of recent time spent limiting (0…1)
    std::atomic<float> reductionDb { 0.0f };
};

} // namespace om
