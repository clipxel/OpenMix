#pragma once

#include <juce_core/juce_core.h>
#include <array>

namespace om
{

enum class ViewLayout { openMix, virtualDj, serato, rekordbox, traktor };

inline constexpr std::array<const char*, 5> viewLayoutNames {
    "OpenMix", "VirtualDJ Pro", "Serato", "rekordbox", "Traktor"
};

/** Arrangement and user-adjustable panel sizes. */
struct Workspace
{
    ViewLayout layout = ViewLayout::openMix;
    int  laneHeight = 60;          // per-deck waveform lane (points)
    float libraryRatio = 0.5375f;  // share of the area below waveforms + FX given to the library

    static constexpr int minLane = 36, maxLane = 200;

    static Workspace defaults() { return {}; }

    juce::var toVar() const
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("view", (int) layout);
        o->setProperty ("lane", laneHeight);
        o->setProperty ("ratio", libraryRatio);
        return juce::var (o.release());
    }

    static Workspace fromVar (const juce::var& v)
    {
        Workspace w;
        if (v.hasProperty ("view")) w.layout = (ViewLayout) juce::jlimit (0, (int) viewLayoutNames.size() - 1, (int) v["view"]);
        if (v.hasProperty ("lane"))  w.laneHeight = juce::jlimit (minLane, maxLane, (int) v["lane"]);
        if (v.hasProperty ("ratio")) w.libraryRatio = juce::jlimit (0.12f, 0.75f, (float) (double) v["ratio"]);
        return w;
    }
};

} // namespace om
