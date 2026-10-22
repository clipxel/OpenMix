#pragma once

#include "../Control/MappingImport.h"

#include <juce_core/juce_core.h>

namespace om
{
    /** Headless functional test of the engine and controller layer (no audio device).
        Returns the process exit code: 0 when everything passed. */
    /** Read-only inspection of a real exported USB (prints a summary, verifies nothing changed). */
    int inspectUsbExport (const juce::File& volumeRoot);

    /** Plays through the real default audio device (low volume) and reports dropouts,
        DSP load and clicks in the recorded output. */
    int runAudioDeviceTest (const juce::File& usbRoot, const juce::File& scratchDir);

    /** Compares OpenMix's analyser against the analysis stored on a real exported USB (read-only). */
    int runAnalyzerBenchmark (const juce::File& usbRoot, int count, const juce::File& reportFile);
    int gridDebug (const juce::File& usbRoot, const juce::String& nameContains);
    /** Measures every club-mixer BEAT FX against its manual; with a track, also renders a demo WAV. */
    int fxAudit (const juce::File& demoIn, const juce::File& demoOut);

    /** Simulates every control of these controller mappings (see SelfTest.cpp). */
    int simulateControllers (const std::vector<MappingImport::Source>&, const juce::String& only);

    /** Builds Resources/mappings/library.json from a folder of mapping files (see SelfTest.cpp). */
    int buildMappingLibrary (const juce::File& sourceFolder, const juce::File& outFile);

    int runSelfTest (const juce::File& trackA, const juce::File& trackB, const juce::File& usbRoot = {});
}
