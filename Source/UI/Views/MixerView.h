#pragma once

#include "../Components/Controls.h"
#include "../../Engine/AudioEngine.h"

namespace om
{

/** Two-channel club mixer laid out like the flagship four-channel ones, reduced to two channels:

      MASTER · HEADPHONES  |  CH 1  ·  CH 2  |  BEAT FX
                           |  SOUND COLOR FX |
                           |   crossfader    |

    Every control is bound to an engine atomic and re-synchronised each frame, so positions always
    equal engine state. */
/** X-PAD: touch a position and the effect plays with that BEAT while you hold it (switching itself on
    if it was off); let go and it goes back to how it was. */
class XPad : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit XPad (AudioEngine& e) : engine (e) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void refresh()                                 { repaint(); }

private:
    int segments() const;
    int indexAt (int segment) const;
    int segmentAt (int x) const;
    void touch (int x);
    AudioEngine& engine;
    int held = -1;
    float savedParam = 0.0f, savedTime = 0.0f;
    bool savedOn = false;
};

class MixerView : public juce::Component
{
public:
    explicit MixerView (AudioEngine&);

    void refresh();
    /** BEAT FX workspace shows the effect section at the side; the 3 FX workspace hides it. */
    void setShowBeatFx (bool);
    bool showsBeatFx() const noexcept               { return showBeatFx; }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Strip
    {
        Strip (int deckIndex);
        int index;
        Knob trim { "TRIM", true }, hi { "HI", true }, mid { "MID", true }, low { "LOW", true }, color { "COLOR", true };
        Button cue { "CUE", Icon::headphones };
        Fader fader { true };
        LevelMeter meter { true };
        Segmented assign { { "A", "THRU", "B" } };
    };

    void bindStrip (Strip&);
    void selectColor (int c);                 // SOUND COLOR FX button: lights it, or switches it off
    void showFxMenu();
    void showTargetMenu();

    AudioEngine& engine;
    std::array<std::unique_ptr<Strip>, 2> strips;

    // Master / headphones
    Knob masterKnob { "MASTER", false };
    Button limBtn { "LIM" };
    float grDb = 0.0f;           // displayed gain reduction (with decay)
    juce::Rectangle<int> grArea;
    LevelMeter masterMeter { true };
    Knob cueMix { "MIXING", true }, phones { "LEVEL", false };
    Fader crossfader { false, true };
    Segmented curve { { "SUAVE", "CORTE", "POTENCIA" } };

    // SOUND COLOR FX
    std::array<std::unique_ptr<Button>, (size_t) ChannelStrip::Color::numColors> colorBtns;
    Knob colorParam { "PARAMETER", false };
    int colorSelected = (int) ChannelStrip::Color::filter;       // −1: none lit

    // BEAT FX
    Button beatDown { {}, Icon::chevronLeft }, beatUp { {}, Icon::chevronRight };
    Button fxSelect { "ECHO" }, chSelect { "MASTER" };
    std::array<std::unique_ptr<Button>, 3> bandBtns;              // FX FREQUENCY: LOW · MID · HI
    Knob fxLevel { "LEVEL/DEPTH", false }, fxTime { "TIME", false };
    Button tapBtn { "TAP" }, autoBtn { "AUTO" };
    XPad xpad { engine };
    Button fxOn { "ON/OFF" };
    int lastFxType = -1;
    bool showBeatFx = true;

    juce::Rectangle<int> masterArea, clipLabelArea, phonesArea, colorArea, fxArea, fxDisplay, logoArea;
    bool masterClipShown = false;
    juce::String shownFx, shownBeat, shownTarget;
};

} // namespace om
