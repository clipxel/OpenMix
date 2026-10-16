#include "WaveformViews.h"

namespace om
{

//==============================================================================
void WavePaint::drawBands (juce::Graphics& g, const WaveformData& w, juce::Rectangle<float> area,
                           double start, double spp, float alpha)
{
    const int cols = (int) area.getWidth();
    if (cols <= 0 || w.isEmpty())
        return;

    const double fps = WaveformData::framesPerSecond;
    const int nf = w.numFrames();
    const float cy = area.getCentreY();
    const float half = area.getHeight() * 0.5f;

    std::vector<float> lo ((size_t) cols, 0.0f), mi ((size_t) cols, 0.0f), hi ((size_t) cols, 0.0f);

    for (int x = 0; x < cols; ++x)
    {
        const double t0 = start + (double) x * spp;
        int f0 = (int) std::floor (t0 * fps);
        int f1 = juce::jmax (f0 + 1, (int) std::floor ((t0 + spp) * fps));
        if (f1 <= 0 || f0 >= nf) continue;
        f0 = juce::jmax (0, f0);
        f1 = juce::jmin (nf, f1);

        int l = 0, m = 0, h = 0;
        for (int f = f0; f < f1; ++f)
        {
            l = juce::jmax (l, (int) w.low[(size_t) f]);
            m = juce::jmax (m, (int) w.mid[(size_t) f]);
            h = juce::jmax (h, (int) w.high[(size_t) f]);
        }
        lo[(size_t) x] = (float) l / 255.0f;
        mi[(size_t) x] = (float) m / 255.0f;
        hi[(size_t) x] = (float) h / 255.0f;
    }

    auto layer = [&] (const std::vector<float>& v, float scale, juce::Colour c)
    {
        juce::Path p;
        p.startNewSubPath (area.getX(), cy);
        for (int x = 0; x < cols; ++x)
            p.lineTo (area.getX() + (float) x + 0.5f, cy - v[(size_t) x] * half * scale);
        for (int x = cols - 1; x >= 0; --x)
            p.lineTo (area.getX() + (float) x + 0.5f, cy + v[(size_t) x] * half * scale);
        p.closeSubPath();
        g.setColour (c.withMultipliedAlpha (alpha));
        g.fillPath (p);
    };

    layer (lo, 0.98f, colour (Col::waveLow));
    layer (mi, 0.80f, colour (Col::waveMid).withAlpha (0.92f));
    layer (hi, 0.55f, colour (Col::waveHigh).withAlpha (0.88f));
}

//==============================================================================
ScrollingWaveform::ScrollingWaveform (Deck& d) : deck (d)
{
    setOpaque (true);
    setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
}

void ScrollingWaveform::refresh()
{
    const auto t = deck.getTrack().get();
    const auto p = deck.getPosition();
    if (std::abs (p - lastPos) > 1.0e-9 || t != lastTrack)
    {
        lastPos = p;
        lastTrack = t;
        repaint();
    }
}

void ScrollingWaveform::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.fillAll (colour (Col::bgField));

    const auto deckCol = Theme::get().deckColour (deck.getIndex());
    const auto track = deck.getTrack();

    // Deck identity edge
    g.setColour (deckCol);
    g.fillRect (r.removeFromLeft (2.0f));

    if (track == nullptr)
    {
        Text::draw (g, juce::String ("Deck ") + (deck.getIndex() == 0 ? "1" : "2") + "  ·  arrastrá una pista aquí o hacé doble clic en la biblioteca"_u,
                    r.reduced (12, 0), font (Type::label), colour (Col::textTertiary), juce::Justification::centred);
        g.setColour (colour (Col::lineStrong));
        g.fillRect (juce::Rectangle<float> (1.0f, r.getHeight()).withCentre (r.getCentre()));
        return;
    }

    const double pos = deck.getPosition();
    const double visible = trackSecondsVisible();
    const double spp = visible / juce::jmax (1.0f, r.getWidth());
    const double start = pos - (double) (r.getWidth() * 0.5f) * spp;
    const double end = start + visible;
    auto xOf = [&] (double t) { return r.getX() + (float) ((t - start) / spp); };

    // Loop region (behind the waveform)
    if (deck.getLoopEnd() > deck.getLoopStart())
    {
        const float x0 = xOf (deck.getLoopStart()), x1 = xOf (deck.getLoopEnd());
        const auto c = deck.isLoopActive() ? accent() : colour (Col::textTertiary);
        g.setColour (c.withAlpha (deck.isLoopActive() ? 0.16f : 0.07f));
        g.fillRect (juce::Rectangle<float> (x0, r.getY(), x1 - x0, r.getHeight()).getIntersection (r));
        g.setColour (c.withAlpha (0.8f));
        g.fillRect (juce::Rectangle<float> (x0, r.getY(), 1.0f, r.getHeight()).getIntersection (r));
        g.fillRect (juce::Rectangle<float> (x1 - 1.0f, r.getY(), 1.0f, r.getHeight()).getIntersection (r));
    }

    // Beat grid
    const auto grid = track->grid();
    if (grid.isValid())
    {
        const double bl = grid.beatLengthSeconds();
        const bool dense = bl / spp < 6.0;
        for (double b = std::ceil (grid.beatAt (start)); grid.timeOfBeat (b) <= end; b += 1.0)
        {
            const bool down = std::fmod (std::abs (b), 4.0) < 0.01;
            if (dense && ! down) continue;
            const float x = std::round (xOf (grid.timeOfBeat (b)));
            g.setColour (colour (down ? Col::waveDownbeat : Col::waveBeat).withAlpha (down ? 0.55f : 0.45f));
            g.fillRect (juce::Rectangle<float> (x, r.getY(), 1.0f, r.getHeight()));
        }
    }

    WavePaint::drawBands (g, track->wave, r.reduced (0.0f, 3.0f), start, spp);

    // Downbeat ticks in front, so the grid stays readable over loud passages.
    if (grid.isValid())
    {
        for (double b = std::ceil (grid.beatAt (start)); grid.timeOfBeat (b) <= end; b += 1.0)
        {
            const bool down = std::fmod (std::abs (b), 4.0) < 0.01;
            const float x = std::round (xOf (grid.timeOfBeat (b)));
            g.setColour (colour (down ? Col::textPrimary : Col::textTertiary).withAlpha (down ? 0.75f : 0.5f));
            const float len = down ? 6.0f : 3.0f;
            g.fillRect (juce::Rectangle<float> (x, r.getY(), 1.0f, len));
            g.fillRect (juce::Rectangle<float> (x, r.getBottom() - len, 1.0f, len));
        }
    }

    // Cue point
    {
        const float x = xOf (deck.getCuePoint());
        if (x >= r.getX() - 6 && x <= r.getRight() + 6)
        {
            juce::Path tri;
            tri.addTriangle (x - 5.0f, r.getBottom(), x + 5.0f, r.getBottom(), x, r.getBottom() - 6.0f);
            g.setColour (colour (Col::cue));
            g.fillPath (tri);
        }
    }

    // Hot cues: line + lettered flag (stays legible when many cues are close)
    const auto flagFont = font (Type::micro, Weight::bold);
    for (int i = 0; i < 8; ++i)
    {
        const auto t = deck.getHotCue (i);
        if (t < 0.0) continue;
        const float x = xOf (t);
        if (x < r.getX() - 20 || x > r.getRight()) continue;
        const auto c = Theme::hotCueColour (i);
        g.setColour (c.withAlpha (0.9f));
        g.fillRect (juce::Rectangle<float> (std::round (x), r.getY(), 1.0f, r.getHeight()));
        auto flag = juce::Rectangle<float> (std::round (x), r.getY(), 13.0f, 12.0f);
        g.fillRoundedRectangle (flag, 2.0f);
        Text::draw (g, juce::String::charToString ((juce::juce_wchar) ('A' + i)), flag, flagFont, colour (Col::textOnAccent),
                    juce::Justification::centred, false);
    }

    // Slip ghost: where the track will continue when the scratch/roll/reverse ends.
    if (const auto ghost = deck.getSlipPosition(); ghost >= 0.0)
    {
        const float gx = xOf (ghost);
        if (gx >= r.getX() && gx <= r.getRight())
        {
            for (float y = r.getY(); y < r.getBottom(); y += 6.0f)
            {
                g.setColour (accent());
                g.fillRect (juce::Rectangle<float> (std::round (gx) - 0.75f, y, 1.5f, 3.0f));
            }
        }
    }

    // Playhead
    const float px = r.getCentreX();
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRect (juce::Rectangle<float> (px - 1.5f, r.getY(), 3.0f, r.getHeight()));
    g.setColour (colour (Col::textPrimary));
    g.fillRect (juce::Rectangle<float> (px - 0.75f, r.getY(), 1.5f, r.getHeight()));
    juce::Path head;
    head.addTriangle (px - 4.0f, r.getY(), px + 4.0f, r.getY(), px, r.getY() + 5.0f);
    head.addTriangle (px - 4.0f, r.getBottom(), px + 4.0f, r.getBottom(), px, r.getBottom() - 5.0f);
    g.fillPath (head);

    // Lane label
    auto tag = juce::Rectangle<float> (r.getX() + 6.0f, r.getY() + 5.0f, 16.0f, 14.0f);
    g.setColour (deckCol.withAlpha (0.2f));
    g.fillRoundedRectangle (tag, 2.0f);
    Text::draw (g, deck.getIndex() == 0 ? "1" : "2", tag, font (Type::caption, Weight::bold), deckCol, juce::Justification::centred, false);
}

double ScrollingWaveform::trackSecondsVisible() const
{
    // The lanes show the same number of BEATS (not seconds): two synced tracks scroll together, beat
    // under beat, whatever their tempos. The zoom (secondsVisible) is expressed at 120 BPM.
    const auto g = deck.getGrid();
    return g.isValid() ? secondsVisible * 2.0 * g.beatLengthSeconds() : secondsVisible;
}

void ScrollingWaveform::mouseDown (const juce::MouseEvent& e)
{
    lastDragX = e.x;
    wasPlaying = deck.isPlaying();
}

void ScrollingWaveform::mouseDrag (const juce::MouseEvent& e)
{
    const int dx = e.x - lastDragX;
    lastDragX = e.x;
    const double spp = trackSecondsVisible() / juce::jmax (1, getWidth());

    if (wasPlaying)
        deck.jogBend (juce::jlimit (-1.0, 1.0, -dx * 0.15));
    else
        deck.jogScrub (-dx * spp);
}

void ScrollingWaveform::mouseUp (const juce::MouseEvent&)
{
    deck.jogBend (0.0);
}

void ScrollingWaveform::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (onZoom && std::abs (w.deltaY) > 0.0f)
        onZoom (w.deltaY > 0 ? 0.85 : 1.0 / 0.85);
}

//==============================================================================
WaveformStrip::WaveformStrip (Deck& a, Deck& b) : deckA (a), deckB (b), laneA (a), laneB (b)
{
    for (auto* l : { &laneA, &laneB })
    {
        addAndMakeVisible (l);
        l->onZoom = [this] (double f) { zoomBy (f); };
    }
    addAndMakeVisible (zoomIn);
    addAndMakeVisible (zoomOut);
    zoomIn.setTooltip ("Acercar (rueda del mouse sobre la waveform)");
    zoomOut.setTooltip ("Alejar");
    zoomIn.onClick  = [this] { zoomBy (0.7); };
    zoomOut.onClick = [this] { zoomBy (1.0 / 0.7); };
}

void WaveformStrip::zoomBy (double f)
{
    secondsVisible = juce::jlimit (2.0, 32.0, secondsVisible * f);
    laneA.setSecondsVisible (secondsVisible);
    laneB.setSecondsVisible (secondsVisible);
}

void WaveformStrip::refresh()
{
    laneA.refresh();
    laneB.refresh();

    // Phase meter: deck 2's beat phase relative to deck 1 (−½ … +½ beat).
    double delta = 9.0;
    const auto ga = deckA.getGrid(), gb = deckB.getGrid();
    if (ga.isValid() && gb.isValid() && deckA.isPlaying() && deckB.isPlaying())
    {
        const double fa = ga.beatAt (deckA.getPosition()), fb = gb.beatAt (deckB.getPosition());
        delta = (fb - std::floor (fb)) - (fa - std::floor (fa));
        if (delta > 0.5) delta -= 1.0;
        if (delta < -0.5) delta += 1.0;
    }
    if (std::abs (delta - lastDelta) > 0.002)
    {
        lastDelta = delta;
        repaint (phaseArea);
    }
}

void WaveformStrip::resized()
{
    auto r = getLocalBounds();
    phaseArea = r.removeFromBottom (phaseBarHeight).withTrimmedRight (26);
    auto controls = r.removeFromRight (26);
    const int lane = (r.getHeight() - 1) / 2;
    laneA.setBounds (r.removeFromTop (lane));
    r.removeFromTop (1);
    laneB.setBounds (r);

    controls.reduce (3, 6);
    zoomIn.setBounds (controls.removeFromTop (20));
    controls.removeFromTop (4);
    zoomOut.setBounds (controls.removeFromTop (20));
}

void WaveformStrip::paint (juce::Graphics& g)
{
    g.fillAll (colour (Col::bgPanel));

    // Phase meter
    {
        auto bar = phaseArea.toFloat().reduced (2.0f, 3.0f);
        g.setColour (colour (Col::bgField));
        g.fillRoundedRectangle (bar, 2.0f);
        const float cx = bar.getCentreX();
        g.setColour (colour (Col::lineStrong));
        for (float q : { -0.25f, 0.25f })
            g.fillRect (juce::Rectangle<float> (cx + q * bar.getWidth(), bar.getY() + 2.0f, 1.0f, bar.getHeight() - 4.0f));
        g.setColour (Theme::get().deckColour (0));
        g.fillRect (juce::Rectangle<float> (cx - 1.0f, bar.getY(), 2.0f, bar.getHeight()));

        if (lastDelta < 1.0)
        {
            const double beatMs = deckA.getGrid().isValid() ? 60000.0 / (deckA.getGrid().bpm * deckA.getTempoRatio()) : 500.0;
            const double ms = lastDelta * beatMs;
            const bool locked = std::abs (ms) < 10.0;
            const float x = cx + (float) lastDelta * bar.getWidth();
            g.setColour (locked ? colour (Col::play) : Theme::get().deckColour (1));
            g.fillRoundedRectangle (juce::Rectangle<float> (6.0f, bar.getHeight()).withCentre ({ x, bar.getCentreY() }), 1.5f);
            Text::drawTabular (g, "FASE 2: " + juce::String (ms >= 0 ? "+" : "") + juce::String ((int) std::round (ms)) + " ms",
                               bar.withTrimmedLeft (8.0f), font (Type::micro, Weight::semibold),
                               locked ? colour (Col::play) : colour (Col::textSecondary), juce::Justification::centredLeft);
        }
        else
        {
            Text::draw (g, juce::String::fromUTF8 ("FASE — los dos decks deben estar sonando"), bar.withTrimmedLeft (8.0f),
                        font (Type::micro, Weight::semibold), colour (Col::textDisabled), juce::Justification::centredLeft);
        }
    }
    auto r = getLocalBounds().removeFromRight (26).toFloat();
    Text::drawTabular (g, juce::String (secondsVisible, 0) + "s", r.removeFromBottom (18.0f), font (Type::micro, Weight::medium),
                       colour (Col::textTertiary), juce::Justification::centred);
}

//==============================================================================
OverviewWaveform::OverviewWaveform (Deck& d) : deck (d)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void OverviewWaveform::rebuildCache()
{
    const auto track = deck.getTrack();
    cachedTrack = track.get();
    const int w = getWidth(), h = getHeight();
    if (w <= 0 || h <= 0)
        return;

    const float scale = 2.0f; // retina-safe
    cache = juce::Image (juce::Image::ARGB, juce::roundToInt ((float) w * scale), juce::roundToInt ((float) h * scale), true);
    juce::Graphics g (cache);
    g.addTransform (juce::AffineTransform::scale (scale));
    if (track != nullptr)
        WavePaint::drawBands (g, track->wave, getLocalBounds().toFloat().reduced (0, 1), 0.0, track->lengthSeconds() / w);
}

void OverviewWaveform::refresh()
{
    if (deck.getTrack().get() != cachedTrack)
    {
        cache = {};
        repaint();
        return;
    }
    const auto len = deck.getLength();
    const int x = len > 0 ? (int) (deck.getPosition() / len * getWidth() * 2.0) : -1;
    if (x != lastPlayheadX)
    {
        lastPlayheadX = x;
        repaint();
    }
}

void OverviewWaveform::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (colour (Col::bgField));
    g.fillRoundedRectangle (r, Radius::sm);

    const auto track = deck.getTrack();
    if (track == nullptr)
    {
        Text::draw (g, "Sin pista", r, font (Type::label), colour (Col::textDisabled), juce::Justification::centred);
        return;
    }

    if (! cache.isValid() || cachedTrack != track.get())
        rebuildCache();

    g.drawImage (cache, r);

    const double len = track->lengthSeconds();
    auto xOf = [&] (double t) { return r.getX() + (float) (t / len) * r.getWidth(); };
    const float px = xOf (deck.getPosition());

    // Played part dimmed
    g.setColour (colour (Col::bgField).withAlpha (0.6f));
    g.fillRect (r.withRight (px));

    // Minute ticks
    g.setColour (colour (Col::textTertiary).withAlpha (0.5f));
    for (double m = 60.0; m < len; m += 60.0)
        g.fillRect (juce::Rectangle<float> (std::round (xOf (m)), r.getBottom() - 3.0f, 1.0f, 3.0f));

    if (deck.getLoopEnd() > deck.getLoopStart())
    {
        const float x0 = xOf (deck.getLoopStart()), x1 = xOf (deck.getLoopEnd());
        g.setColour ((deck.isLoopActive() ? accent() : colour (Col::textTertiary)).withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (x0, r.getY(), juce::jmax (1.5f, x1 - x0), r.getHeight()));
    }

    for (int i = 0; i < 8; ++i)
        if (const auto t = deck.getHotCue (i); t >= 0.0)
        {
            const float x = xOf (t);
            juce::Path tri;
            tri.addTriangle (x - 3.5f, r.getY(), x + 3.5f, r.getY(), x, r.getY() + 5.0f);
            g.setColour (Theme::hotCueColour (i));
            g.fillPath (tri);
        }

    {
        const float x = xOf (deck.getCuePoint());
        juce::Path tri;
        tri.addTriangle (x - 3.5f, r.getBottom(), x + 3.5f, r.getBottom(), x, r.getBottom() - 5.0f);
        g.setColour (colour (Col::cue));
        g.fillPath (tri);
    }

    if (const auto ghost = deck.getSlipPosition(); ghost >= 0.0)
    {
        g.setColour (accent());
        g.fillRect (juce::Rectangle<float> (xOf (ghost) - 0.75f, r.getY(), 1.5f, r.getHeight()));
    }

    g.setColour (colour (Col::textPrimary));
    g.fillRect (juce::Rectangle<float> (px - 0.75f, r.getY(), 1.5f, r.getHeight()));

    // End-of-track warning: last 30 s while playing → warning outline.
    const double remaining = len - deck.getPosition();
    if (deck.isPlaying() && remaining < 30.0)
    {
        const bool on = (juce::Time::getMillisecondCounter() / 500) % 2 == 0;
        g.setColour (colour (Col::warning).withAlpha (on ? 0.9f : 0.3f));
        g.drawRoundedRectangle (r.reduced (0.5f), Radius::sm, 1.0f);
    }
}

void OverviewWaveform::mouseDown (const juce::MouseEvent& e)
{
    if (deck.hasTrack() && getWidth() > 0)
        deck.seek ((double) e.x / getWidth() * deck.getLength());
}

void OverviewWaveform::mouseDrag (const juce::MouseEvent& e) { mouseDown (e); }

} // namespace om
