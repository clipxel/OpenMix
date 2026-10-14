#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Common/Strings.h"

/*  OpenMix design system — single source of truth for every visual token.

    Components never hard-code colours, sizes or fonts: they ask the Theme.
    Changing a token here (or the accent at runtime) updates the whole UI.
*/
namespace om
{

//==============================================================================
/** Surface depth scale (darkest → lightest) and functional colours. */
enum class Col
{
    // Surfaces
    bgApp,          // window background (deepest)
    bgPanel,        // primary panels (decks, mixer, library)
    bgRaised,       // elevated surfaces inside panels (sections, headers)
    bgControl,      // resting button / knob face
    bgControlHover, // hover
    bgField,        // text fields, search, waveform wells (inset)
    bgMenu,         // popup menus

    // Lines
    line,           // hairline separators
    lineStrong,     // control outlines, focused separators

    // Text
    textPrimary,
    textSecondary,
    textTertiary,
    textDisabled,
    textOnAccent,

    // Semantic states
    play,
    cue,
    sync,
    record,
    warning,
    error,
    clip,
    disconnected,
    success,

    // Deck identity
    deckA,
    deckB,

    // Waveform spectrum (3-band)
    waveLow,
    waveMid,
    waveHigh,
    waveBeat,
    waveDownbeat,

    // Meter ladder
    meterLow,
    meterMid,
    meterHigh,

    numColours
};

//==============================================================================
namespace Space   { constexpr int xxs = 2, xs = 4, sm = 6, md = 8, lg = 12, xl = 16, xxl = 24; }
namespace Radius  { constexpr float sm = 3.0f, md = 4.0f, lg = 6.0f, pill = 100.0f; }
namespace Stroke  { constexpr float hair = 1.0f, control = 1.0f, focus = 1.5f, knobArc = 2.5f; }

/** Control heights (points). Transport is the largest tier; secondary controls shrink. */
namespace Size
{
    constexpr int topBar        = 38;
    constexpr int controlXs     = 18;
    constexpr int controlSm     = 22;
    constexpr int controlMd     = 26;
    constexpr int controlLg     = 34;
    constexpr int transport     = 40;
    constexpr int knob          = 34;
    constexpr int knobSmall     = 28;
    constexpr int faderWidth    = 28;
    constexpr int meterWidth    = 4;
    constexpr int tableRow      = 22;
    constexpr int panelHeader   = 26;
    constexpr int sidebarWidth  = 210;
}

/** Type ramp. Values are in points (JUCE height ≈ point size × 1.2). */
enum class Type
{
    micro,      // 9  — meter scales, tiny tags
    caption,    // 10 — control labels (uppercase, tracked)
    label,      // 11 — secondary info, table cells
    body,       // 12 — primary list text
    title,      // 13 — panel titles
    deckTitle,  // 15 — track title on deck
    timer,      // 17 — elapsed / remaining
    display     // 24 — BPM
};

enum class Weight { regular, medium, semibold, bold };

//==============================================================================
class Theme : public juce::ChangeBroadcaster
{
public:
    static Theme& get();

    juce::Colour colour (Col c) const noexcept       { return colours[(size_t) c]; }
    juce::Colour accent() const noexcept             { return accentColour; }
    juce::Colour accentDim() const noexcept          { return accentColour.withMultipliedSaturation (0.75f).withAlpha (0.22f); }
    juce::Colour deckColour (int deckIndex) const     { return colour (deckIndex == 0 ? Col::deckA : Col::deckB); }
    static juce::Colour hotCueColour (int slot);

    /** Accent is user-configurable; every component repaints on change. */
    void setAccent (juce::Colour newAccent);

    static juce::Array<juce::Colour> accentPresets();

    juce::Font font (Type t, Weight w = Weight::regular) const;
    static float pointSize (Type t);

    /** Name of the resolved UI family (for diagnostics). */
    juce::String fontFamily() const { return family; }

private:
    Theme();

    std::array<juce::Colour, (size_t) Col::numColours> colours;
    juce::Colour accentColour;
    juce::String family;
};

inline juce::Colour colour (Col c)       { return Theme::get().colour (c); }
inline juce::Colour accent()             { return Theme::get().accent(); }
inline juce::Font   font (Type t, Weight w = Weight::regular) { return Theme::get().font (t, w); }

//==============================================================================
/** Text drawing helpers. drawTabular() gives every digit the same advance so
    BPM, pitch and timers never jitter while updating (the system UI font has
    proportional figures by default and JUCE exposes no OpenType feature API). */
namespace Text
{
    void draw (juce::Graphics&, const juce::String&, juce::Rectangle<float>, const juce::Font&,
               juce::Colour, juce::Justification = juce::Justification::centredLeft, bool ellipsis = true);

    float tabularWidth (const juce::String&, const juce::Font&);

    void drawTabular (juce::Graphics&, const juce::String&, juce::Rectangle<float>, const juce::Font&,
                      juce::Colour, juce::Justification = juce::Justification::centredLeft);

    /** Uppercase, tracked label used for control captions. */
    void caption (juce::Graphics&, const juce::String&, juce::Rectangle<float>, juce::Colour,
                  juce::Justification = juce::Justification::centred);
}

} // namespace om
