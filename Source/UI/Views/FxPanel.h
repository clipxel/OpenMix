#pragma once

#include "../Components/Controls.h"
#include "../../Engine/AudioEngine.h"

namespace om
{

/** Three effect slots per deck: type (menu), ON, WET and a BPM-synced parameter. */
class FxPanel : public juce::Component
{
public:
    explicit FxPanel (AudioEngine&);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Slot
    {
        int deck = 0, index = 0;
        Button type { "ECHO", Icon::chevronDown, Button::Style::standard };
        Button on { "ON" };
        Knob wet { "LEVEL", false }, param { "BEAT", false };
        juce::Rectangle<int> area;
        int lastType = -1;
    };

    void showTypeMenu (Slot&);

    AudioEngine& engine;
    std::vector<std::unique_ptr<Slot>> slots;
    std::array<juce::Rectangle<int>, 2> deckAreas;
    int lastState = -1;
};

} // namespace om
