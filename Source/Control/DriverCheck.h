#pragma once

#include <juce_core/juce_core.h>

namespace om::DriverCheck
{

/** USB devices plugged in right now, as "vvvv:pppp" (lowercase hex). Lets OpenMix notice a
    controller even before its driver is installed (it may not show up as MIDI yet). Windows only;
    an empty list elsewhere. Read-only: nothing is downloaded or installed. */
juce::StringArray presentUsbIds();

/** "%ProgramFiles%\\Vendor\\x.dll" or "/Library/Audio/…" → the real path (environment variables
    expanded on Windows). Used to tell whether a driver is already installed. */
juce::File expandPath (const juce::String& path);

} // namespace om::DriverCheck
