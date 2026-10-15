#pragma once

#include "../Components/Controls.h"
#include "../../Engine/AudioEngine.h"
#include "../../Control/ControllerLibrary.h"

namespace om
{

/** Compact application bar: brand, system status, record, settings. */
class TopBar : public juce::Component
{
public:
    explicit TopBar (AudioEngine&);

    void refresh();
    void setControllerLibrary (const ControllerLibrary* lib) { controllerLib = lib; }

    std::function<void (int section)> onOpenSettings;   // 0 audio, 1 appearance, 2 layout, 3 controllers, 4 diagnostics
    std::function<void()> onToggleRecord;
    std::function<void()> onToggleFx;
    /** Small screens: the FX panel is folded away and this button shows / hides it. */
    void setFxButton (bool visible, bool panelShown);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AudioEngine& engine;
    StatusPill audio, controllers, cpu;
    Button rec { "REC", Icon::record, Button::Style::standard };
    Button settings { {}, Icon::settings, Button::Style::ghost };
    Button fx { "FX" };
    juce::Rectangle<int> brandArea;
    const ControllerLibrary* controllerLib = nullptr;
    juce::String lastControllerText;
    int lastMidiCount = -1;
    juce::uint32 lastMidiPoll = 0;
};

} // namespace om
