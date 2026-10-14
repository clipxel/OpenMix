#pragma once

#include "../Components/Controls.h"
#include "../../Engine/AudioEngine.h"

namespace om
{

/** Compact two-channel mixer. Every control is bound to an engine atomic and
    re-synchronised each frame, so positions always equal engine state. */
class MixerView : public juce::Component
{
public:
    explicit MixerView (AudioEngine&);

    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Strip
    {
        Strip (int deckIndex);
        int index;
        Knob trim { "GAIN", true }, hi { "HI", true }, mid { "MID", true }, low { "LOW", true }, filter { "FILTER", true };
        Button pfl { {}, Icon::headphones };
        Button fx1 { "FX1" }, fx2 { "FX2" }, fx3 { "FX3" };
        Fader fader { true };
        LevelMeter meter { true };
        Segmented assign { { "A", "THRU", "B" } };
    };

    void bindStrip (Strip&);

    AudioEngine& engine;
    std::array<std::unique_ptr<Strip>, 2> strips;

    Knob masterKnob { "MASTER", false };
    Button limBtn { "LIM" };
    float grDb = 0.0f;           // displayed gain reduction (with decay)
    juce::Rectangle<int> grArea;
    LevelMeter masterMeter { true };
    Knob cueMix { "CUE/MST", true }, phones { "PHONES", false };
    Fader crossfader { false, true };
    Segmented curve { { "SUAVE", "CORTE", "POTENCIA" } };

    juce::Rectangle<int> masterArea, clipLabelArea, phonesArea;
    bool masterClipShown = false;
};

} // namespace om
