#include "Controls.h"

namespace om
{

//==============================================================================
Button::Button (const juce::String& text, Icon i, Style s)
    : juce::Button (text), style (s), icon (i)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setWantsKeyboardFocus (false);
}

void Button::setNotImplemented (bool ni, const juce::String& what)
{
    notImplemented = ni;
    setEnabled (! ni);
    setTooltip (ni ? "No implementado todavía"_u + (what.isNotEmpty() ? juce::String (": ") + what : juce::String()) : juce::String());
    repaint();
}

void Button::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        if (onRightClick) onRightClick (e);
        return;
    }
    juce::Button::mouseDown (e);
    if (onPress && isEnabled()) onPress();
}

void Button::mouseDrag (const juce::MouseEvent& e)
{
    juce::Button::mouseDrag (e);
    if (onDrag && isEnabled() && ! e.mods.isPopupMenu()) onDrag (e);
}

void Button::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    juce::Button::mouseUp (e);
    if (onRelease && isEnabled()) onRelease();
}

void Button::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool on = getToggleState();
    const bool enabled = isEnabled();
    const auto litC = lit();
    const float radius = style == Style::transport ? Radius::lg : Radius::md;

    juce::Colour face, outline = juce::Colours::transparentBlack, content;

    switch (style)
    {
        case Style::ghost:
            face = over && enabled ? colour (Col::bgControl) : juce::Colours::transparentBlack;
            content = on ? litC : colour (over ? Col::textPrimary : Col::textSecondary);
            break;

        case Style::transport:
            face = on ? litC : colour (over ? Col::bgControlHover : Col::bgControl);
            outline = on ? litC : litC.withAlpha (0.45f);
            content = on ? colour (Col::textOnAccent) : litC;
            break;

        case Style::pad:
            if (padColour.has_value())
            {
                face = padColour->withAlpha (over ? 0.95f : 0.82f);
                content = colour (Col::textOnAccent);
            }
            else
            {
                face = colour (over ? Col::bgControlHover : Col::bgControl);
                content = colour (Col::textTertiary);
            }
            break;

        case Style::standard:
        case Style::segment:
            if (on)
            {
                face = litC.withAlpha (0.18f);
                outline = litC.withAlpha (0.85f);
                content = litC.brighter (0.15f);
            }
            else
            {
                face = colour (over && enabled ? Col::bgControlHover : Col::bgControl);
                content = colour (Col::textPrimary);
            }
            break;
    }

    if (down && enabled)
        face = face.brighter (0.08f);

    if (! enabled)
    {
        face = style == Style::ghost ? juce::Colours::transparentBlack : colour (Col::bgRaised);
        outline = juce::Colours::transparentBlack;
        content = colour (Col::textDisabled);
    }

    g.setColour (face);
    g.fillRoundedRectangle (r, radius);

    if (! outline.isTransparent())
    {
        g.setColour (outline);
        g.drawRoundedRectangle (r.reduced (0.5f), radius, Stroke::control);
    }

    // Content: icon and/or text, centred. Optional sub-text under the main text.
    auto content_r = r.reduced (4.0f, 2.0f);
    const auto text = getButtonText();
    const auto f = font (textType, textWeight);

    if (subText.isNotEmpty())
    {
        auto sub = content_r.removeFromBottom (content_r.getHeight() * 0.38f);
        Text::draw (g, subText, sub, font (Type::micro), content.withMultipliedAlpha (0.75f), juce::Justification::centredTop);
    }

    if (icon != Icon::none && text.isEmpty())
    {
        const float s = juce::jmin (content_r.getHeight(), style == Style::transport ? 18.0f : 14.0f);
        Icons::draw (g, icon, content_r.withSizeKeepingCentre (s, s), content);
    }
    else if (icon != Icon::none)
    {
        const float s = juce::jmin (content_r.getHeight(), 12.0f);
        const float tw = juce::GlyphArrangement::getStringWidth (f, text);
        const float total = s + 5.0f + tw;
        auto row = content_r.withSizeKeepingCentre (juce::jmin (total, content_r.getWidth()), content_r.getHeight());
        Icons::draw (g, icon, row.removeFromLeft (s), content);
        row.removeFromLeft (5.0f);
        Text::draw (g, text, row, f, content, juce::Justification::centredLeft);
    }
    else
    {
        Text::draw (g, text, content_r, f, content, juce::Justification::centred);
    }

    if (notImplemented)
        drawNotImplemented (g, r, r.getWidth() < 56.0f ? juce::String() : juce::String ("N/I"));
}

//==============================================================================
void BoundSlider::bind (Binding b)
{
    binding = std::move (b);
    syncFromEngine();
}

void BoundSlider::syncFromEngine()
{
    if (! binding.get || isMouseButtonDown())
        return;

    const auto v = binding.get();
    if (std::abs (v - getValue()) > 1.0e-6)
    {
        const juce::ScopedValueSetter<bool> svs (syncing, true);
        setValue (v, juce::dontSendNotification);
        repaint();
    }
}

void BoundSlider::valueChanged()
{
    if (! syncing && binding.set)
        binding.set (getValue());
    repaint();
}

//==============================================================================
Knob::Knob (const juce::String& l, bool bp)
    : label (l), bipolar (bp)
{
    setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
    setMouseDragSensitivity (180);
    setVelocityBasedMode (false);
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
}

void Knob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && onRightClick)
    {
        onRightClick();
        return;
    }
    BoundSlider::mouseDown (e);
}

void Knob::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const bool hasLabel = label.isNotEmpty();
    auto labelArea = ! hasLabel ? juce::Rectangle<float>()
                   : labelOnSide ? r.removeFromLeft (juce::jmax (30.0f, r.getWidth() - r.getHeight()))
                                 : r.removeFromBottom (13.0f);
    const float d = juce::jmin (r.getWidth(), r.getHeight()) - 2.0f;
    auto dial = juce::Rectangle<float> (d, d).withCentre (r.getCentre());

    const auto& rp = getRotaryParameters();
    const float norm = (float) valueToProportionOfLength (getValue());
    const float angle = rp.startAngleRadians + norm * (rp.endAngleRadians - rp.startAngleRadians);
    const float centreAngle = (rp.startAngleRadians + rp.endAngleRadians) * 0.5f;
    const bool enabled = isEnabled();
    const bool killed = showsKill && norm <= 0.005f;

    // Track ring
    const float ringR = d * 0.5f - 1.5f;
    juce::Path track;
    track.addCentredArc (dial.getCentreX(), dial.getCentreY(), ringR, ringR, 0.0f, rp.startAngleRadians, rp.endAngleRadians, true);
    g.setColour (colour (Col::bgField));
    g.strokePath (track, juce::PathStrokeType (Stroke::knobArc, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc (from centre when bipolar)
    const float from = bipolar ? centreAngle : rp.startAngleRadians;
    if (std::abs (angle - from) > 0.01f)
    {
        juce::Path arc;
        arc.addCentredArc (dial.getCentreX(), dial.getCentreY(), ringR, ringR, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (! enabled ? colour (Col::textDisabled) : arcColour.value_or (colour (Col::textSecondary)));
        g.strokePath (arc, juce::PathStrokeType (Stroke::knobArc, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Face
    auto face = dial.reduced (d * 0.17f);
    g.setColour (colour (isMouseOverOrDragging() && enabled ? Col::bgControlHover : Col::bgControl));
    g.fillEllipse (face);
    g.setColour (colour (Col::lineStrong));
    g.drawEllipse (face, 1.0f);

    // Pointer
    const auto c = face.getCentre();
    const float pr = face.getWidth() * 0.5f;
    const juce::Point<float> p1 (c.x + std::sin (angle) * pr * 0.25f, c.y - std::cos (angle) * pr * 0.25f);
    const juce::Point<float> p2 (c.x + std::sin (angle) * (pr - 1.5f), c.y - std::cos (angle) * (pr - 1.5f));
    g.setColour (killed ? colour (Col::error) : colour (enabled ? Col::textPrimary : Col::textDisabled));
    g.drawLine ({ p1, p2 }, 2.0f);

    // Centre detent tick for bipolar knobs
    if (bipolar)
    {
        g.setColour (colour (Col::textTertiary));
        g.fillRect (juce::Rectangle<float> (1.0f, 2.5f).withCentre ({ dial.getCentreX(), dial.getY() + 0.5f }));
    }

    if (hasLabel)
    {
        const auto just = labelOnSide ? juce::Justification::centredLeft : juce::Justification::centred;
        if (showValue() && formatter)
            Text::drawTabular (g, formatted(), labelArea, font (Type::micro, Weight::medium), colour (Col::textPrimary), just);
        else if (killed)
            Text::caption (g, "KILL", labelArea, colour (Col::error), just);
        else
            Text::caption (g, label, labelArea, colour (enabled ? Col::textTertiary : Col::textDisabled), just);
    }
}

//==============================================================================
Fader::Fader (bool v, bool detent) : vertical (v), centreDetent (detent)
{
    setSliderStyle (v ? juce::Slider::LinearVertical : juce::Slider::LinearHorizontal);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setMouseCursor (v ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::LeftRightResizeCursor);
    setSliderSnapsToMousePosition (false);
}

void Fader::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const float capLong = 12.0f;
    const float norm = (float) valueToProportionOfLength (getValue());

    auto travel = vertical ? r.reduced (0.0f, capLong * 0.5f) : r.reduced (capLong * 0.5f, 0.0f);
    const float capPos = vertical ? travel.getBottom() - norm * travel.getHeight()
                                  : travel.getX() + norm * travel.getWidth();

    // Ticks
    if (ticks > 1)
    {
        g.setColour (colour (Col::line));
        for (int i = 0; i < ticks; ++i)
        {
            const float t = (float) i / (float) (ticks - 1);
            if (vertical)
            {
                const float y = travel.getBottom() - t * travel.getHeight();
                g.fillRect (juce::Rectangle<float> (r.getX(), y - 0.5f, 4.0f, 1.0f));
                g.fillRect (juce::Rectangle<float> (r.getRight() - 4.0f, y - 0.5f, 4.0f, 1.0f));
            }
            else
            {
                const float x = travel.getX() + t * travel.getWidth();
                g.fillRect (juce::Rectangle<float> (x - 0.5f, r.getY(), 1.0f, 3.0f));
                g.fillRect (juce::Rectangle<float> (x - 0.5f, r.getBottom() - 3.0f, 1.0f, 3.0f));
            }
        }
    }

    // Slot
    auto slot = vertical ? travel.withSizeKeepingCentre (4.0f, travel.getHeight() + 4.0f)
                         : travel.withSizeKeepingCentre (travel.getWidth() + 4.0f, 4.0f);
    g.setColour (colour (Col::bgField));
    g.fillRoundedRectangle (slot, 2.0f);

    if (centreDetent)
    {
        const float mid = vertical ? travel.getCentreY() : travel.getCentreX();
        g.setColour (colour (Col::textTertiary));
        if (vertical) g.fillRect (juce::Rectangle<float> (r.getX() + 2.0f, mid - 0.5f, r.getWidth() - 4.0f, 1.0f));
        else          g.fillRect (juce::Rectangle<float> (mid - 0.5f, r.getY() + 2.0f, 1.0f, r.getHeight() - 4.0f));

        // Offset from centre — visible deviation (pitch) without colouring the whole control.
        g.setColour (capColour.value_or (colour (Col::textSecondary)).withAlpha (0.8f));
        if (vertical) g.fillRect (slot.withY (juce::jmin (mid, capPos)).withHeight (std::abs (capPos - mid)).reduced (0.5f, 0.0f));
        else          g.fillRect (slot.withX (juce::jmin (mid, capPos)).withWidth (std::abs (capPos - mid)).reduced (0.0f, 0.5f));
    }

    // Cap
    auto cap = vertical ? juce::Rectangle<float> (r.getWidth() - 2.0f, capLong).withCentre ({ r.getCentreX(), capPos })
                        : juce::Rectangle<float> (capLong, r.getHeight() - 2.0f).withCentre ({ capPos, r.getCentreY() });
    g.setColour (colour (isMouseOverOrDragging() ? Col::bgControlHover : Col::bgControl).brighter (0.12f));
    g.fillRoundedRectangle (cap, Radius::sm);
    g.setColour (colour (Col::lineStrong).brighter (0.2f));
    g.drawRoundedRectangle (cap.reduced (0.5f), Radius::sm, 1.0f);
    g.setColour (capColour.value_or (colour (Col::textPrimary)));
    if (vertical) g.fillRect (cap.withSizeKeepingCentre (cap.getWidth() - 6.0f, 1.5f));
    else          g.fillRect (cap.withSizeKeepingCentre (1.5f, cap.getHeight() - 6.0f));
}

//==============================================================================
LevelMeter::LevelMeter (bool s) : stereo (s)
{
    setInterceptsMouseClicks (false, false);
}

float LevelMeter::dbToNorm (float db) noexcept
{
    // −48 … 0 dBFS, with more resolution near the top where mixing happens.
    if (db <= -48.0f) return 0.0f;
    if (db >= 0.0f)   return 1.0f;
    if (db > -18.0f)  return juce::jmap (db, -18.0f, 0.0f, 0.45f, 1.0f);
    return juce::jmap (db, -48.0f, -18.0f, 0.0f, 0.45f);
}

void LevelMeter::update()
{
    if (source == nullptr)
        return;

    const auto now = juce::Time::getMillisecondCounterHiRes();
    auto [pl, pr] = source->drain();
    if (! stereo) pl = pr = juce::jmax (pl, pr);

    auto step = [now] (float peak, float& level, float& hold, double& holdUntil)
    {
        const float db = juce::Decibels::gainToDecibels (peak, -60.0f);
        const float n = dbToNorm (db);
        level = juce::jmax (n, level - 0.022f);           // ~1.3 / s fall
        if (n >= hold || now > holdUntil) { hold = n; holdUntil = now + 900.0; }
    };

    const float oldL = levelL, oldR = levelR, oldHL = holdL, oldHR = holdR;
    step (pl, levelL, holdL, holdUntilL);
    step (pr, levelR, holdR, holdUntilR);

    if (source->drainClip())
        clipUntil = now + 1500.0;

    const bool clip = now < clipUntil;
    auto moved = [] (float a, float b) { return std::abs (a - b) > 1.0e-4f; };
    if (clip != clipLit || moved (oldL, levelL) || moved (oldR, levelR) || moved (oldHL, holdL) || moved (oldHR, holdR))
    {
        clipLit = clip;
        repaint();
    }
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto clipArea = r.removeFromTop (5.0f);
    r.removeFromTop (2.0f);

    g.setColour (clipLit ? colour (Col::clip) : colour (Col::bgField));
    g.fillRoundedRectangle (clipArea, 1.0f);

    const int bars = stereo ? 2 : 1;
    const float gap = 2.0f;
    const float w = (r.getWidth() - gap * (float) (bars - 1)) / (float) bars;

    for (int b = 0; b < bars; ++b)
    {
        auto bar = juce::Rectangle<float> (r.getX() + (float) b * (w + gap), r.getY(), w, r.getHeight());
        g.setColour (colour (Col::bgField));
        g.fillRect (bar);

        const float level = b == 0 ? levelL : levelR;
        const float hold  = b == 0 ? holdL : holdR;
        const float yNeg9 = bar.getBottom() - dbToNorm (-9.0f) * bar.getHeight();
        const float yNeg3 = bar.getBottom() - dbToNorm (-3.0f) * bar.getHeight();
        const float top   = bar.getBottom() - level * bar.getHeight();

        auto fillSeg = [&] (float from, float to, juce::Colour c)
        {
            const float y0 = juce::jmax (top, to);
            if (y0 < from) { g.setColour (c); g.fillRect (bar.withY (y0).withBottom (from)); }
        };
        fillSeg (bar.getBottom(), yNeg9, colour (Col::meterLow));
        fillSeg (yNeg9, yNeg3, colour (Col::meterMid));
        fillSeg (yNeg3, bar.getY(), colour (Col::meterHigh));

        // Segment gaps — a quiet ladder that reads as "meter" at a glance.
        g.setColour (colour (Col::bgPanel).withAlpha (0.7f));
        for (float y = bar.getBottom() - 3.0f; y > bar.getY(); y -= 3.0f)
            g.fillRect (bar.withY (y).withHeight (1.0f));

        if (hold > 0.01f)
        {
            const float hy = bar.getBottom() - hold * bar.getHeight();
            g.setColour (hold > 0.85f ? colour (Col::meterHigh) : colour (Col::textSecondary));
            g.fillRect (bar.withY (hy).withHeight (1.0f));
        }
    }
}

//==============================================================================
void StatusPill::set (Icon i, const juce::String& t, juce::Colour c, bool p)
{
    if (i == icon && t == text && c == col && p == pulse)
        return;
    icon = i; text = t; col = c; pulse = p;
    repaint();
}

int StatusPill::idealWidth() const
{
    return (int) std::ceil (juce::GlyphArrangement::getStringWidth (font (Type::label, Weight::medium), text)) + 32;
}

void StatusPill::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    if (onClick && isMouseOver())
    {
        g.setColour (colour (Col::bgControl));
        g.fillRoundedRectangle (r, Radius::md);
    }
    auto content = r.reduced (6.0f, 0.0f);
    Icons::draw (g, icon, content.removeFromLeft (12.0f).withSizeKeepingCentre (12, 12), col);
    content.removeFromLeft (6.0f);
    Text::draw (g, text, content, font (Type::label, Weight::medium), colour (Col::textSecondary));
}

//==============================================================================
Segmented::Segmented (juce::StringArray o) : options (std::move (o))
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void Segmented::setSelected (int i, bool notify)
{
    i = juce::jlimit (-1, options.size() - 1, i);
    if (i == selected) return;
    selected = i;
    repaint();
    if (notify && onChange) onChange (i);
}

void Segmented::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (colour (Col::bgField));
    g.fillRoundedRectangle (r, Radius::md);

    const float w = r.getWidth() / (float) juce::jmax (1, options.size());
    const auto lc = useAccent ? accent() : litColour;

    for (int i = 0; i < options.size(); ++i)
    {
        auto cell = juce::Rectangle<float> (r.getX() + (float) i * w, r.getY(), w, r.getHeight());
        if (i == selected)
        {
            g.setColour (colour (Col::bgControlHover));
            g.fillRoundedRectangle (cell.reduced (2.0f), Radius::sm);
            g.setColour (lc);
            g.fillRoundedRectangle (cell.reduced (2.0f).removeFromBottom (2.0f).reduced (w * 0.25f, 0.0f), 1.0f);
        }
        Text::draw (g, options[i], cell.reduced (1.0f, 0.0f), font (compact ? Type::micro : Type::label, Weight::semibold),
                    colour (i == selected ? Col::textPrimary : Col::textTertiary), juce::Justification::centred);
    }
}

void Segmented::mouseDown (const juce::MouseEvent& e)
{
    const int i = (int) ((float) e.x / ((float) getWidth() / (float) juce::jmax (1, options.size())));
    setSelected (i, true);
}

//==============================================================================
void drawSectionTitle (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title, juce::Colour c)
{
    Text::caption (g, title, r, c.isTransparent() ? colour (Col::textTertiary) : c, juce::Justification::centredLeft);
}

void drawNotImplemented (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& tag)
{
    juce::Path outline;
    outline.addRoundedRectangle (r.reduced (0.5f), Radius::md);
    juce::Path dashed;
    const float dashes[] = { 3.0f, 3.0f };
    juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
    g.setColour (colour (Col::warning).withAlpha (0.55f));
    g.fillPath (dashed);
    if (tag.isEmpty())
        return; // narrow controls: dashed outline + tooltip only, so the label stays readable

    const auto f = font (Type::micro, Weight::bold);
    const float tw = juce::GlyphArrangement::getStringWidth (f, tag) + 6.0f;
    auto badge = juce::Rectangle<float> (tw, 11.0f).withPosition (r.getRight() - tw - 2.0f, r.getY() + 2.0f);
    g.setColour (colour (Col::warning).withAlpha (0.18f));
    g.fillRoundedRectangle (badge, 2.0f);
    Text::draw (g, tag, badge, f, colour (Col::warning), juce::Justification::centred, false);
}

} // namespace om
