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
/** Small dot beside the pitch fader, at its 0 % mark: click = glide back to 0 %, double = jump. */
class PitchZeroDot : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void()> onClick;
    void setState (bool gliding, bool atZero, juce::Colour c)
    {
        if (gliding == lit && atZero == zero && c == col) return;
        lit = gliding; zero = atZero; col = c; repaint();
    }
    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().withSizeKeepingCentre (7.0f, 7.0f);
        if (lit || over) { g.setColour (col.withAlpha (lit ? 0.35f : 0.18f)); g.fillEllipse (r.expanded (3.0f)); }
        g.setColour (lit ? col : zero ? col.withAlpha (0.35f) : col.withAlpha (0.8f));
        g.fillEllipse (r);
    }
    void mouseDown (const juce::MouseEvent&) override  { if (onClick) onClick(); }
    void mouseEnter (const juce::MouseEvent&) override { over = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { over = false; repaint(); }

private:
    bool lit = false, zero = true, over = false;
    juce::Colour col;
};

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
    std::unique_ptr<juce::Component> makeGridEditor();   // the GRID callout's content (also for snapshots)
    void nextPadPage()                             { setPadMode ((PadMode) (((int) padMode + 1) % numPadModes)); }
    void showPadPage (int page)                    { if (juce::isPositiveAndBelow (page, numPadModes)) setPadMode ((PadMode) page); }
    /** Controller light of PAD 1–8 in the current page (Actions::Led: 0 off, 1 dim, 2 on, 3 blink). */
    int padLed (int pad) const                     { return juce::isPositiveAndBelow (pad, 8) ? padLeds[(size_t) pad] : 0; }

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
    enum class PadMode { hotCue = 0, roll, loop, savedLoop, jump };
    static constexpr int numPadModes = 5;

    void setPadMode (PadMode);
    void layoutLoopAndPads (juce::Rectangle<int> loopRow, int padRowH);
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

    Fader pitch { true, true };
    Button pitchRange { "±8%"_u, Icon::none, Button::Style::ghost };
    Button deckSwap { {}, Icon::none, Button::Style::standard };      // shows the other deck of this side (1 ⇄ 3, 2 ⇄ 4)
    Button bendDown { {}, Icon::minus }, bendUp { {}, Icon::plus };
    PitchZeroDot pitchZero;

    // Loop row
    Button loopHalf { "½"_u }, loopDouble { "2×"_u }, loopToggle { "LOOP", Icon::loop };
    Button loopIn { "IN" }, loopOut { "OUT" };
    Button jumpBack { {}, Icon::chevronLeft }, jumpFwd { {}, Icon::chevronRight };

    // Pads
    std::array<std::unique_ptr<Button>, numPadModes> padTabs;
    std::array<std::unique_ptr<Button>, 8> pads;
    PadMode padMode = PadMode::hotCue;
    std::array<int, 8> padLeds {};

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
