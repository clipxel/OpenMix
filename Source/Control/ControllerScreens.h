#pragma once

#include "../Engine/AudioEngine.h"
#include "../Library/Library.h"
#include <juce_audio_devices/juce_audio_devices.h>

namespace om
{

/** The screens of controllers that have them, fed by OpenMix: track, BPM, pitch, key, time and the
    browser list. Today the Denon DJ MCX8000 (two screens, "MCX8000 Display Left/Right" MIDI ports):
    inMusic SysEx F0 47 <screen> 37 05 <command> <sizes> <data> F7, the data as bytes packed into
    7-bit MIDI bytes low bits first, text in UTF-16. Worked out from what DJ software sends to the unit. */
class ControllerScreens
{
public:
    struct Browser
    {
        std::function<const std::vector<TrackPtr>&()> rows;   // the list on screen
        std::function<int()> selected;                 // its selected row (−1: none)
    };

    ControllerScreens (AudioEngine&, Browser);

    /** Per UI frame (message thread). */
    void tick();

    /** The data of one MCX8000 SysEx message, packed and framed (tests). */
    static juce::MemoryBlock mcxMessage (int screenId, int command, const std::vector<juce::uint8>& data);
    static std::vector<juce::uint8> pack7 (const std::vector<juce::uint8>& bytes);

private:
    struct Screen
    {
        juce::String port;                       // MIDI output name
        int id;                                  // 0x10 left, 0x20 right
        int side;                                // 0 left decks (1 / 3), 1 right decks (2 / 4)
        std::unique_ptr<juce::MidiOutput> out;
        int shownDeck = -1;
        juce::String shownTrack, shownTempo, shownKey;
        juce::String shownBrowser;
    };

    void openPorts();
    void send (Screen&, int screenId, int command, const std::vector<juce::uint8>& data);
    void sendTrack (Screen&, int deck);
    void sendTempo (Screen&, int deck, bool force);
    void sendClock (Screen&, int deck);
    void sendBrowser (Screen&, bool force);

    AudioEngine& engine;
    Browser browser;
    std::array<Screen, 2> screens;
    juce::uint32 lastScan = 0;
    double startMs = juce::Time::getMillisecondCounterHiRes();
};

} // namespace om
