#pragma once

#include "Actions.h"

namespace om
{

/** The mapping assistant: the DJ says in their own words what a control should do ("que este botón
    borre el hot cue 3 del deck izquierdo con shift", "the knob for the bass of deck 2") and it finds
    the function, the deck and the SHIFT layer. Works on the computer, without internet: a vocabulary
    of how DJs name every function (Spanish and English, with the usual slang and typos), numbers in
    words, sides, decks and loop sizes, scored against every function OpenMix has. */
class MapAssistant
{
public:
    explicit MapAssistant (const Actions& a) : actions (a) {}

    struct Reading
    {
        juce::String action;             // function id ("" = nothing understood)
        juce::String label;              // the function as the screen names it
        int deck = -2;                   // 0…3, −1 global, −2 not said (the assistant asks)
        bool shift = false;              // the user asked for the SHIFT layer
        float score = 0.0f;              // 0…1
        juce::String control;            // the kind of control the phrase names: "button", "knob", "fader", "jog", "encoder", ""
    };

    /** The best readings of the phrase, most likely first (at most `count`). */
    std::vector<Reading> interpret (const juce::String& phrase, int count = 3) const;

    /** A phrase reduced to plain words: lower case, no accents or punctuation, numbers as digits. */
    static juce::String normalise (const juce::String&);

private:
    const Actions& actions;
};

} // namespace om
