#pragma once

#include "../Components/Controls.h"
#include "../../Engine/Deck.h"

namespace om
{

/** Shared waveform painting: 3-band envelope (lows/mids/highs) drawn as
    stacked symmetric layers so the structure of the track is readable. */
namespace WavePaint
{
    void drawBands (juce::Graphics&, const WaveformData&, juce::Rectangle<float> area,
                    double startSeconds, double secondsPerPixel, float alpha = 1.0f);
}

//==============================================================================
/** Zoomed, scrolling waveform with fixed centre playhead, beat grid,
    hot cues, cue point and loop region. Drag to scrub (paused) or nudge (playing). */
class ScrollingWaveform : public juce::Component
{
public:
    explicit ScrollingWaveform (Deck&);

    void setSecondsVisible (double s)             { secondsVisible = s; repaint(); }
    void refresh();                                // once per frame
    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    std::function<void (double factor)> onZoom;

private:
    Deck& deck;
    double secondsVisible = 8.0;
    double lastPos = -1.0;
    bool wasPlaying = false;
    int lastDragX = 0;
    const void* lastTrack = nullptr;
};

//==============================================================================
/** Stacked lanes for both decks with a shared zoom control, spanning the window
    width above the decks. */
class WaveformStrip : public juce::Component
{
public:
    WaveformStrip (Deck& a, Deck& b);

    void refresh();
    void setLaneHeight (int h)                    { laneHeight = h; }
    int  preferredHeight() const                  { return laneHeight * 2 + 1 + phaseBarHeight; }
    static constexpr int phaseBarHeight = 14;
    void resized() override;
    void paint (juce::Graphics&) override;

    void zoomBy (double factor);

private:
    Deck& deckA;
    Deck& deckB;
    juce::Rectangle<int> phaseArea;
    double lastDelta = 9.0;
    ScrollingWaveform laneA, laneB;
    Button zoomIn { {}, Icon::plus, Button::Style::ghost }, zoomOut { {}, Icon::minus, Button::Style::ghost };
    double secondsVisible = 8.0;
    int laneHeight = 76;
};

//==============================================================================
/** Whole-track overview inside each deck: played part dimmed, markers, click to seek. */
class OverviewWaveform : public juce::Component
{
public:
    explicit OverviewWaveform (Deck&);

    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override                       { cache = {}; }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    void rebuildCache();

    Deck& deck;
    juce::Image cache;
    const void* cachedTrack = nullptr;
    int lastPlayheadX = -1;
};

} // namespace om
