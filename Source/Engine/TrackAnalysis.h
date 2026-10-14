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

    /** Estimates the musical key (chromagram + Krumhansl–Kessler profiles).
        Returns Camelot notation ("8A", "11B"…) or empty when the signal is too weak. */
    juce::String detectKey (const juce::AudioBuffer<float>& audio, double sampleRate,
                            const std::atomic<bool>* shouldAbort = nullptr);

    /** Camelot ↔ musical notation helpers. */
    /** Samples macOS trims from the start of a gapless (LAME-tagged) MP3: encoder delay +
        529 decoder delay. Club players keep them, so OpenMix prepends them to share
        the same timeline (grids and cues line up with exported USB drives). Read-only peek. */
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
