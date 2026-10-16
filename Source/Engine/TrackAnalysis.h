#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace om
{

/** 3-band peak envelope used by every waveform view.
    One frame per (1 / framesPerSecond) seconds; values 0..255. */
struct WaveformData
{
    static constexpr double framesPerSecond = 150.0;

    std::vector<juce::uint8> low, mid, high;

    int numFrames() const noexcept { return (int) low.size(); }
    bool isEmpty() const noexcept  { return low.empty(); }
};

/** Constant-tempo beat grid. Invalid when bpm == 0 (analysis failed or not run). */
struct BeatGrid
{
    double bpm = 0.0;
    double firstBeatSeconds = 0.0;

    bool isValid() const noexcept                 { return bpm > 1.0; }
    double beatLengthSeconds() const noexcept     { return isValid() ? 60.0 / bpm : 0.0; }

    /** Fractional beat index at a time (0 at firstBeat). */
    double beatAt (double seconds) const noexcept { return (seconds - firstBeatSeconds) / beatLengthSeconds(); }
    double timeOfBeat (double beat) const noexcept { return firstBeatSeconds + beat * beatLengthSeconds(); }
    double nearestBeatTime (double seconds) const noexcept { return timeOfBeat (std::round (beatAt (seconds))); }
};

namespace Analysis
{
    /** Builds the 3-band envelope (lows < 200 Hz, mids, highs > 2.5 kHz). */
    WaveformData computeWaveform (const juce::AudioBuffer<float>& audio, double sampleRate,
                                  const std::atomic<bool>* shouldAbort = nullptr);

    /** Estimates a constant tempo (70–180 BPM window) and the phase of beat 0.
        Returns an invalid grid if no confident periodicity is found. */
    BeatGrid detectBeatGrid (const juce::AudioBuffer<float>& audio, double sampleRate,
                             const std::atomic<bool>* shouldAbort = nullptr);

    /** Grid corrector: starting from a tempo the DJ confirmed (e.g. after ÷2), finds the exact BPM
        (±0.6 %) and phase on the kicks of the whole track, keeping the bar position closest to
        `approx`. Invalid input → returned unchanged. */
    BeatGrid refineGrid (const juce::AudioBuffer<float>& audio, double sampleRate, BeatGrid approx);

        /** A grid from another program (exported USB): same BPM and bar, beats moved onto the kicks of
        the whole track as decoded here (≤ 50 ms). Unchanged when the track has no clear kicks. */
    BeatGrid alignGridToKicks (const juce::AudioBuffer<float>& audio, double sampleRate, BeatGrid grid);

    /** Diagnostics: median position of the kick attacks relative to a grid's beats (ms, + = late). */
    double attackOffsetMs (const juce::AudioBuffer<float>& audio, double sampleRate, BeatGrid grid);

    /** Diagnostics: OpenMix's grid vs a reference grid, and where the attacks fall on the beat. */
    juce::String debugGrid (const juce::AudioBuffer<float>& audio, double sampleRate, BeatGrid reference);

    /** Estimates the musical key (chromagram + Krumhansl–Kessler profiles).
        Returns Camelot notation ("8A", "11B"…) or empty when the signal is too weak. */
    juce::String detectKey (const juce::AudioBuffer<float>& audio, double sampleRate,
                            const std::atomic<bool>* shouldAbort = nullptr);

    /** Camelot ↔ musical notation helpers. */
    /** Timeline correction for MP3s so grids and cues line up with club players and exported USB
        drives: > 0 = silence to prepend (samples the decoder trimmed), < 0 = samples to drop from
        the start (decoder delay this decoder keeps). Read-only peek at the header. */
    int mp3TrimmedLeadSamples (const juce::File&);

    /** Integrated loudness in LUFS (ITU-R BS.1770-4 / EBU R128: K-weighting,
        400 ms blocks, −70 LUFS absolute and −10 LU relative gates). Silence → −100. */
    double integratedLoudness (const juce::AudioBuffer<float>& audio, double sampleRate);

    juce::String camelotToMusical (const juce::String& camelot);
    bool camelotCompatible (const juce::String& a, const juce::String& b);
    /** Shifts a Camelot key by semitones (+1 semitone = +7 on the wheel). */
    juce::String transposeCamelot (const juce::String& camelot, int semitones);
    /** Smallest semitone shift (−6…+6) that makes `key` equal to `target` (or its relative). */
    int semitonesToMatch (const juce::String& key, const juce::String& target);
}

} // namespace om
