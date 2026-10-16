#pragma once

#include "TrackAnalysis.h"
#include <juce_core/juce_core.h>
#include <memory>

namespace om
{

/** Descriptive data shown on a deck. Comes from the library (tags) — never invented. */
struct TrackMeta
{
    juce::String title, artist, key;
    BeatGrid knownGrid;                 // reuse a previous analysis when available
    bool alignGridToKicks = false;      // knownGrid comes from another program: put its beats on this audio's kicks
    double cuePoint = 0.0;
    std::array<double, 8> hotCues { -1, -1, -1, -1, -1, -1, -1, -1 };
    std::array<std::pair<double, double>, 8> savedLoops {};
};

/** Fully decoded track resident in memory: enables instant seeking, loops and jog. */
struct LoadedTrack
{
    juce::File file;
    TrackMeta meta;
    juce::AudioBuffer<float> audio;     // always 2 channels
    double sampleRate = 44100.0;
    WaveformData wave;
    /** Beat grid — editable while the track plays (grid editor), so it is stored as
        atomics: the UI writes, the audio thread reads, no tearing. */
    BeatGrid grid() const noexcept                { return { gridBpm.load(), gridFirst.load() }; }
    void setGrid (BeatGrid g) const noexcept      { gridBpm = g.bpm; gridFirst = g.firstBeatSeconds; }
    BeatGrid analysedGrid;                        // as detected / imported, for "reset"
    double loudnessLufs = -100.0;       // integrated loudness, for auto-gain

    mutable std::atomic<double> gridBpm { 0.0 }, gridFirst { 0.0 };

    int numSamples() const noexcept        { return audio.getNumSamples(); }
    double lengthSeconds() const noexcept  { return sampleRate > 0 ? (double) numSamples() / sampleRate : 0.0; }
};

using LoadedTrackPtr = std::shared_ptr<const LoadedTrack>;

} // namespace om
