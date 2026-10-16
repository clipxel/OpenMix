#pragma once

#include <juce_core/juce_core.h>
#include <array>

namespace om
{

/** Each external drive carries its own OpenMix database: "<drive>/OpenMix/biblioteca.json", keyed by
    the path relative to the drive root, with the analysis (BPM, grid, key) of every track of that
    drive played in OpenMix and the DJ's cues / grid edits, so they follow the drive between
    computers. Only that folder is ever written: never the rekordbox export (PIONEER/), never
    Contents/ or any audio file. Older drives with a hidden ".OpenMix/cues.json" are still read.
    Tracks on the computer's own disk use the local library (Documents/OpenMix). */
class VolumeCueStore
{
public:
    struct Cues
    {
        double cuePoint = -1.0;
        std::array<double, 8> hotCues { -1, -1, -1, -1, -1, -1, -1, -1 };
        std::array<std::pair<double, double>, 8> savedLoops {};
        double bpm = 0.0, firstBeat = 0.0;       // edited beat grid (0 = not edited)
        double analysedBpm = 0.0, analysedFirst = 0.0;   // grid as analysed / re-aligned when it was played
        juce::String key;
    };

    /** Root of the external volume holding the file, or {} for internal/system disks. */
    static juce::File externalVolumeFor (const juce::File&);
    /** Tests: treat this directory as an external volume too. */
    static void setExtraVolumeRoot (const juce::File&);

    std::optional<Cues> get (const juce::File& track);
    /** Returns an error message on failure (read-only medium, permissions…). */
    juce::String put (const juce::File& track, const Cues&);
    /** Records a played track's analysis on its drive (only when new or changed). */
    juce::String noteAnalysis (const juce::File& track, double bpm, double firstBeat, const juce::String& key);

    static juce::File storeFolder (const juce::File& volumeRoot) { return volumeRoot.getChildFile ("OpenMix"); }
    static juce::File storeFile (const juce::File& volumeRoot)   { return storeFolder (volumeRoot).getChildFile ("biblioteca.json"); }
    static juce::File legacyFile (const juce::File& volumeRoot)  { return volumeRoot.getChildFile (".OpenMix").getChildFile ("cues.json"); }

private:
    struct Volume
    {
        juce::var data;      // { "version":1, "tracks": { relPath: {...} } }
        juce::Time loadedMtime;
    };
    Volume& volume (const juce::File& root);
    juce::String write (const juce::File& root, Volume&);

    std::map<juce::String, Volume> volumes;
};

} // namespace om
