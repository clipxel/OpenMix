#pragma once

#include "WaveformViews.h"
#include "../../Engine/AudioEngine.h"

namespace om
{

/** Flat virtual platter: deck-coloured ring shows track progress, a marker
    rotates at 33⅓ rpm with the real playhead; centre shows elapsed/remaining. */
class JogWheel : public juce::Component
{
public:
    explicit JogWheel (Deck&);
    void refresh();
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    float angleAt (juce::Point<float>) const;
    Deck& deck;
    float lastAngle = 0.0f;
    bool dragging = false, wasPlaying = false;
    double lastPos = -1.0;
};

//==============================================================================
class DeckView : public juce::Component,
                 public juce::FileDragAndDropTarget,
                 public juce::DragAndDropTarget
{
public:
    DeckView (AudioEngine&, int deckIndex);

    void setShowJog (bool);
    bool isShowingJog() const noexcept { return showJog; }
    void setDense (bool d)             { dense = d; resized(); }

    /** Pull engine state into controls. Called every frame. */
    void refresh();

    void showMessage (const juce::String& text, bool isError);

    std::function<void (const juce::File&)> onLoadRequest;

    /** Keyboard / MIDI: PAD 1–8 of the current pad page, and the next page. */
    void externalPad (int pad, bool down)          { if (down) padDown (pad, false); else padUp (pad); }
    void nextPadPage()                             { setPadMode ((PadMode) (((int) padMode + 1) % numPadModes)); }

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override  { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override              { dragOver = false; repaint(); }
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override                 { dragOver = true; repaint(); }
    void itemDragExit (const SourceDetails&) override                  { dragOver = false; repaint(); }

private:
    enum class PadMode { hotCue = 0, roll, loop, savedLoop, jump, sampler };
    static constexpr int numPadModes = 6;

    void setPadMode (PadMode);
    void padDown (int index, bool shift);
    void padUp (int index);
    void padRightClick (int index);
    void updatePads();
    void cyclePitchRange();

    AudioEngine& engine;
    Deck& deck;
    const int index;

    OverviewWaveform overview;
    JogWheel jog;

    Button sync { "SYNC", Icon::none, Button::Style::standard };
    Button cue  { "CUE", Icon::none, Button::Style::transport };
    Button play { {}, Icon::playPause, Button::Style::transport };
    Button quantize { "Q" }, keyLock { {}, Icon::lock }, slip { "SLIP" };
    Button reverseBtn { "REV" }, brakeBtn { "BRAKE" }, censorBtn { "CENSOR" };
    Button vinylBtn { "VINYL" };
    Button gridBtn { "GRID" };
    void openGridEditor();
    std::unique_ptr<juce::FileChooser> sampleChooser;

    Fader pitch { true, true };
    Button pitchRange { "±8%"_u, Icon::none, Button::Style::ghost };
    Button bendDown { {}, Icon::minus }, bendUp { {}, Icon::plus };

    // Loop row
    Button loopHalf { "½"_u }, loopDouble { "2×"_u }, loopToggle { "LOOP", Icon::loop };
    Button loopIn { "IN" }, loopOut { "OUT" };
    Button jumpBack { {}, Icon::chevronLeft }, jumpFwd { {}, Icon::chevronRight };

    // Pads
    std::array<std::unique_ptr<Button>, numPadModes> padTabs;
    std::array<std::unique_ptr<Button>, 8> pads;
    PadMode padMode = PadMode::hotCue;
    // SAMPLES/RMX: X-PAD tone and roll speed, set before launching (the held-pad gesture adds to them)
    Knob padTone { "TONO", true }, padRoll { "VEL.", false }, padVol { "VOL", false };

    bool showJog = true, dense = false, dragOver = false;
    juce::String message;
    bool messageIsError = false;
    juce::uint32 messageUntil = 0;

    // Painted header regions
    juce::Rectangle<int> keyArea;
    juce::Rectangle<int> headerArea, infoArea, loopLabelArea, pitchValueArea, padArea, bodyArea;

    // Repaint gating for header text
    juce::String lastHeaderKey;
};

} // namespace om
