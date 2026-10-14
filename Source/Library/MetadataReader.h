#pragma once

#include <juce_core/juce_core.h>

namespace om
{

/** Tags read from the file itself (ID3 / MP4 / AIFF / RIFF info), via the OS. */
struct FileTags
{
    juce::String title, artist, album, genre, comment, key;
    double bpm = 0.0;
    double durationSeconds = 0.0;
};

namespace Metadata
{
    FileTags read (const juce::File&);
}

} // namespace om
