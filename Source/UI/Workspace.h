#pragma once

#include <juce_core/juce_core.h>

namespace om
{

/** The single OpenMix arrangement: stacked waveforms · FX · [deck | mixer | deck] · library.
    Only the sizes are user-adjustable (dragged in the UI) and persisted. */
struct Workspace
{
    int  laneHeight = 60;          // per-deck waveform lane (points)
    float libraryRatio = 0.5375f;  // share of the area below waveforms + FX given to the library

    static constexpr int minLane = 36, maxLane = 200;

    static Workspace defaults() { return {}; }

    juce::var toVar() const
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("lane", laneHeight);
        o->setProperty ("ratio", libraryRatio);
        return juce::var (o.release());
    }

    static Workspace fromVar (const juce::var& v)
    {
        Workspace w;
        if (v.hasProperty ("lane"))  w.laneHeight = juce::jlimit (minLane, maxLane, (int) v["lane"]);
        if (v.hasProperty ("ratio")) w.libraryRatio = juce::jlimit (0.12f, 0.75f, (float) (double) v["ratio"]);
        return w;
    }
};

} // namespace om
