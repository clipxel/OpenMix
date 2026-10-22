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
    std::function<void()> onToggleAutoMix;
    std::function<void()> onOpenFeedback;
    void setAutoMixState (bool panelOpen, bool running);
    /** Effects workspace: 0 = BEAT FX (club mixer, one effect section), 1 = 3 FX per deck. */
    std::function<void (int)> onFxModeChange;
    void setFxMode (int m)                         { fxMode.setSelected (m); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AudioEngine& engine;
    StatusPill audio, controllers, cpu;
    Button rec { "REC", Icon::record, Button::Style::standard };
    Button settings { {}, Icon::settings, Button::Style::ghost };
    Button automixBtn { "AUTOMIX" };
    Segmented fxMode { { "BEAT FX", "3 FX" } };
    Button helpBtn { {}, Icon::support, Button::Style::ghost };
    void showHelpMenu();
    juce::Rectangle<int> brandArea;
    const ControllerLibrary* controllerLib = nullptr;
    juce::String lastControllerText;
    int lastMidiCount = -1;
    juce::uint32 lastMidiPoll = 0;
};

} // namespace om
