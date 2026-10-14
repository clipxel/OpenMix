#pragma once

#include <juce_core/juce_core.h>
#include <array>

namespace om
{

/** Cue points that travel with an external drive.

    For tracks on an external drive cues created in OpenMix are
    stored in "<volume>/.OpenMix/cues.json", keyed by the path relative to the volume
    root, so they follow the drive between computers. The folder is only created on
    the first cue edit; nothing else on the drive is ever written (never the export
    folder, never audio files). Tracks on the internal disk keep using the local library. */
class VolumeCueStore
{
public:
    struct Cues
    {
        double cuePoint = -1.0;
        std::array<double, 8> hotCues { -1, -1, -1, -1, -1, -1, -1, -1 };
        std::array<std::pair<double, double>, 8> savedLoops {};
        double bpm = 0.0, firstBeat = 0.0;       // edited beat grid (0 = not edited)
    };

    /** Root of the external volume holding the file, or {} for internal/system disks. */
    static juce::File externalVolumeFor (const juce::File&);
    /** Tests: treat this directory as an external volume too. */
    static void setExtraVolumeRoot (const juce::File&);

    std::optional<Cues> get (const juce::File& track);
    /** Returns an error message on failure (read-only medium, permissions…). */
    juce::String put (const juce::File& track, const Cues&);

    static juce::File storeFile (const juce::File& volumeRoot) { return volumeRoot.getChildFile (".OpenMix").getChildFile ("cues.json"); }

private:
    struct Volume
    {
        juce::var data;      // { "version":1, "tracks": { relPath: {...} } }
        juce::Time loadedMtime;
    };
    Volume& volume (const juce::File& root);

    std::map<juce::String, Volume> volumes;
};

} // namespace om
