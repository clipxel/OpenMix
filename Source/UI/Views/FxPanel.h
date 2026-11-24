#pragma once

#include "../Components/Controls.h"
#include "../Workspace.h"
#include "../../Engine/AudioEngine.h"

namespace om
{

/** Three effect slots per deck: type (menu), ON, WET and a BPM-synced parameter. */
class FxPanel : public juce::Component
{
public:
    explicit FxPanel (AudioEngine&);
    void refresh();
    void setPresentation (ViewLayout, int centreWidth);
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
    void showSlotMenu (int side);
    bool focused() const { return presentation == ViewLayout::virtualDj || presentation == ViewLayout::rekordbox; }

    AudioEngine& engine;
    std::vector<std::unique_ptr<Slot>> slots;       // every deck's; the two sides on screen are shown
    std::pair<int, int> shown { -1, -1 };
    std::array<juce::Rectangle<int>, 2> deckAreas;
    std::array<Button, 2> slotButtons;
    std::array<int, AudioEngine::numDecks> selectedSlots {};
    ViewLayout presentation = ViewLayout::openMix;
    int mixerGap = 0;
    int lastState = -1;
};

} // namespace om
