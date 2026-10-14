#pragma once

#include "MidiEngine.h"
#include <set>

namespace om
{

/** Guided mapping for a controller that is not in the library. One instruction at a time ("press PLAY
    on the left deck", "move the left jog wheel"…); each answer becomes a binding with the right
    encoding (buttons, 7/14-bit faders, relative encoders, jog). Deck 2 is not asked control by
    control: after mapping deck 1, the user presses PLAY on deck 2 once, the wizard learns how the
    two decks differ (MIDI channel and/or note offset) and mirrors every deck-1 control, then checks
    one more control to confirm. Pure logic, no UI: feed it MIDI messages, read prompt(). */
class MappingWizard
{
public:
    struct Step
    {
        juce::String action;
        int deck;                  // 0 / 1, or −1 for global controls
        juce::String prompt;
        bool optional = true;      // can be skipped (controllers differ)
    };

    enum class Phase { deckOne, deckTwoProbe, deckTwoCheck, deckTwoManual, globals, finished };

    MappingWizard (const Actions&, const juce::String& deviceName);

    Phase phase() const noexcept { return currentPhase; }
    const Step* current() const;
    int stepIndex() const noexcept { return index; }
    int stepCount() const noexcept;
    juce::String prompt() const;

    /** Feed every incoming message from the device. True when it completed the current step. */
    bool feed (const juce::MidiMessage&, double timeSeconds);
    /** Call periodically: finishes a fader/jog step once the control stopped moving. */
    void poll (double timeSeconds);
    void skip();

    /** The mapping so far (complete after phase() == finished). */
    const std::vector<MidiBinding>& bindings() const noexcept { return result; }
    /** Deck 2 relation found from the probe: channel and note/CC offsets. */
    int deckChannelOffset() const noexcept { return channelOffset; }
    int deckNumberOffset() const noexcept  { return numberOffset; }

private:
    void advance();
    void finishPending();
    void mirrorDeckTwo();
    bool alreadyMapped (const juce::MidiMessage&) const;
    const std::vector<Step>& activeSteps() const;

    const Actions& actions;
    juce::String device;
    std::vector<Step> deckSteps, globalSteps;
    Phase currentPhase = Phase::deckOne;
    int index = 0;
    std::vector<MidiBinding> result;

    // A continuous control (fader / knob / jog) is captured over a short gesture.
    struct Pending { MidiBinding binding; double lastTime = 0.0; std::set<int> ccSeen; std::vector<int> values; bool any = false; };
    Pending pending;

    int channelOffset = 0, numberOffset = 0;
    MidiBinding probe;                      // deck-1 binding used to find the deck-2 relation
    MidiBinding checkExpected;              // mirrored binding the user confirms in the check step
};

} // namespace om
