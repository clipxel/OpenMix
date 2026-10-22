#pragma once

#include "../Theme/Theme.h"
#include "../Theme/Icons.h"
#include "../../Engine/Meter.h"

namespace om
{

//==============================================================================
/** Every interactive control in OpenMix shares this button.
    Resting state is quiet; the "lit" (toggle-on) state uses a semantic colour
    (accent by default) so active states are obvious at a glance. */
class Button : public juce::Button
{
public:
    enum class Style
    {
        standard,   // raised control face
        ghost,      // text/icon only, for low-priority actions
        transport,  // play / cue: largest, always outlined in its semantic colour
        pad,        // hot cue pads
        segment     // inside a segmented group
    };

    Button (const juce::String& text = {}, Icon icon = Icon::none, Style = Style::standard);

    void setStyle (Style s)                       { style = s; repaint(); }
    void setIcon (Icon i)                         { icon = i; repaint(); }
    void setLitColour (juce::Colour c)            { litColour = c; useAccent = false; repaint(); }
    void useAccentWhenLit()                       { useAccent = true; repaint(); }
    void setPadColour (std::optional<juce::Colour> c) { padColour = c; repaint(); }
    void setSubText (const juce::String& s)       { if (s != subText) { subText = s; repaint(); } }
    void setTextType (Type t, Weight w)           { textType = t; textWeight = w; repaint(); }

    /** Development marker: the control is visible so the layout can be judged,
        but it is disabled, drawn with a dashed outline and tagged "N/I". */
    void setNotImplemented (bool ni, const juce::String& what = {});
    bool isNotImplemented() const noexcept        { return notImplemented; }

    std::function<void (const juce::MouseEvent&)> onRightClick;
    std::function<void()> onPress, onRelease;      // momentary actions (cue)
    std::function<void (const juce::MouseEvent&)> onDrag;   // gestures while held

    void paintButton (juce::Graphics&, bool over, bool down) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    juce::Colour lit() const { return useAccent ? accent() : litColour; }

    Style style;
    Icon icon;
    juce::Colour litColour;
    bool useAccent = true;
    bool notImplemented = false;
    std::optional<juce::Colour> padColour;
    juce::String subText;
    Type textType = Type::label;
    Weight textWeight = Weight::semibold;
};

//==============================================================================
/** Engine binding shared by knobs and faders: the control writes through `set`
    and re-reads `get` every frame, so its position always equals engine state. */
struct Binding
{
    std::function<double()> get;
    std::function<void (double)> set;
};

class BoundSlider : public juce::Slider
{
public:
    using juce::Slider::Slider;

    void bind (Binding b);
    void syncFromEngine();
    void setDefault (double v)                    { setDoubleClickReturnValue (true, v); }
    void valueChanged() override;

    std::function<juce::String (double)> formatter;

protected:
    juce::String formatted() const { return formatter ? formatter (getValue()) : juce::String (getValue(), 2); }
    bool showValue() const { return isMouseButtonDown() || isMouseOverOrDragging(); }

    Binding binding;
    bool syncing = false;
};

//==============================================================================
class Knob : public BoundSlider
{
public:
    Knob (const juce::String& label, bool bipolar);

    void setLabel (const juce::String& s)         { label = s; repaint(); }
    void setArcColour (std::optional<juce::Colour> c) { arcColour = c; repaint(); }
    /** Draws a small "kill" marker when the value sits at minimum (EQ). */
    void setShowsKill (bool k)                    { showsKill = k; }
    /** Caption to the left of the dial instead of below — denser vertical stacks (mixer). */
    void setLabelOnSide (bool s)                  { labelOnSide = s; repaint(); }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    std::function<void()> onRightClick;

private:
    juce::String label;
    bool bipolar;
    bool showsKill = false, labelOnSide = false;
    std::optional<juce::Colour> arcColour;
};

//==============================================================================
class Fader : public BoundSlider
{
public:
    explicit Fader (bool vertical, bool centreDetent = false);

    void setCapColour (std::optional<juce::Colour> c) { capColour = c; repaint(); }
    void setTicks (int n)                          { ticks = n; repaint(); }
    void paint (juce::Graphics&) override;

private:
    bool vertical, centreDetent;
    int ticks = 0;
    std::optional<juce::Colour> capColour;
};

//==============================================================================
/** Peak meter with ballistics and a latched clip indicator (colour + "CLIP" text). */
class LevelMeter : public juce::Component
{
public:
    explicit LevelMeter (bool stereo = true);

    void setSource (MeterState* m)                { source = m; }
    void setShowScale (bool s)                    { showScale = s; }
    void update();                                 // call once per frame
    bool isClipLit() const noexcept               { return clipLit; }
    void paint (juce::Graphics&) override;

    static float dbToNorm (float db) noexcept;

private:
    MeterState* source = nullptr;
    bool stereo;
    bool showScale = false;
    float levelL = 0, levelR = 0, holdL = 0, holdR = 0;
    double holdUntilL = 0, holdUntilR = 0, clipUntil = 0;
    bool clipLit = false;
};

//==============================================================================
/** Small icon + text status indicator (never colour-only). */
class StatusPill : public juce::Component, public juce::SettableTooltipClient
{
public:
    void set (Icon, const juce::String& text, juce::Colour, bool pulse = false);
    void paint (juce::Graphics&) override;
    int idealWidth() const;
    std::function<void()> onClick;
    void mouseUp (const juce::MouseEvent&) override { if (onClick) onClick(); }

private:
    Icon icon = Icon::dot;
    juce::String text;
    juce::Colour col;
    bool pulse = false;
};

//==============================================================================
/** Exclusive choice between a few options (workspaces, X-fader assign, curves). */
class Segmented : public juce::Component
{
public:
    explicit Segmented (juce::StringArray options);

    void setSelected (int index, bool notify = false);
    int  getSelected() const noexcept              { return selected; }
    void setLitColour (juce::Colour c)             { litColour = c; useAccent = false; repaint(); }
    void setCompact (bool c)                       { compact = c; repaint(); }
    void setOptionText (int i, const juce::String& s) { if (i < options.size()) { options.set (i, s); repaint(); } }
    std::function<void (int)> onChange;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::StringArray options;
    int selected = 0;
    bool useAccent = true, compact = false;
    juce::Colour litColour;
};

//==============================================================================
/** Caption + hairline, used to title sections inside panels. */
void drawSectionTitle (juce::Graphics&, juce::Rectangle<float>, const juce::String&, juce::Colour = {});

/** Dashed outline + "N/I" tag used on any surface that is not implemented yet. */
void drawNotImplemented (juce::Graphics&, juce::Rectangle<float>, const juce::String& tag = "N/I");

} // namespace om
