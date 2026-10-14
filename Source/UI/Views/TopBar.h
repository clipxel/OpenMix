#pragma once

#include "../Components/Controls.h"
#include "../../Engine/AudioEngine.h"

namespace om
{

/** Compact application bar: brand, system status, record, settings. */
class TopBar : public juce::Component
{
public:
    explicit TopBar (AudioEngine&);

    void refresh();

    std::function<void (int section)> onOpenSettings;   // 0 audio, 1 appearance, 2 layout, 3 controllers, 4 diagnostics
    std::function<void()> onToggleRecord;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AudioEngine& engine;
    StatusPill audio, controllers, cpu;
    Button rec { "REC", Icon::record, Button::Style::standard };
    Button settings { {}, Icon::settings, Button::Style::ghost };
    juce::Rectangle<int> brandArea;
    int lastMidiCount = -1;
    juce::uint32 lastMidiPoll = 0;
};

} // namespace om
