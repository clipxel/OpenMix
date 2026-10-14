#include "Theme.h"

namespace om
{

static juce::Colour hex (juce::uint32 rgb) { return juce::Colour (0xff000000 | rgb); }

Theme& Theme::get()
{
    static Theme instance;
    return instance;
}

Theme::Theme()
{
    auto set = [this] (Col c, juce::uint32 rgb) { colours[(size_t) c] = hex (rgb); };

    // Surfaces — charcoal, never pure black; ~4–6% luminance steps between tiers.
    set (Col::bgApp,          0x111214);
    set (Col::bgPanel,        0x17181B);
    set (Col::bgRaised,       0x1D1E22);
    set (Col::bgControl,      0x25272C);
    set (Col::bgControlHover, 0x2D3036);
    set (Col::bgField,        0x0D0E10);
    set (Col::bgMenu,         0x1E1F23);

    set (Col::line,           0x26282D);
    set (Col::lineStrong,     0x363940);

    set (Col::textPrimary,    0xE8E9EB);
    set (Col::textSecondary,  0xA0A4AB);
    set (Col::textTertiary,   0x6A6E77);
    set (Col::textDisabled,   0x464950);
    set (Col::textOnAccent,   0x140C0A);

    // Semantic states (each paired with an icon/label wherever it's critical)
    set (Col::play,           0x3CCB8B);
    set (Col::cue,            0xEFC24A);
    set (Col::sync,           0x6F8FF2);
    set (Col::record,         0xF03E3E);
    set (Col::warning,        0xEFC34A);
    set (Col::error,          0xF0474D);
    set (Col::clip,           0xFF3434);
    set (Col::disconnected,   0x7B8089);
    set (Col::success,        0x3CCB8B);

    set (Col::deckA,          0x2EC6E8);
    set (Col::deckB,          0xF29A3A);

    // Waveform: blue lows, amber mids, warm-white highs. Muted enough that
    // cue / loop markers keep the strongest contrast.
    set (Col::waveLow,        0x2C5FD4);
    set (Col::waveMid,        0xD9893A);
    set (Col::waveHigh,       0xEDE7DC);
    set (Col::waveBeat,       0x3A3D44);
    set (Col::waveDownbeat,   0x6A6E77);

    set (Col::meterLow,       0x3CCB8B);
    set (Col::meterMid,       0xEFC34A);
    set (Col::meterHigh,      0xFF3434);

    accentColour = accentPresets()[0];

    // Resolve the UI family: prefer the macOS system face (SF), fall back gracefully.
    for (auto candidate : { "SF Pro Text", ".AppleSystemUIFont", "Helvetica Neue" })
    {
        juce::Font f (juce::FontOptions (candidate, 12.0f, juce::Font::plain));
        if (auto tf = f.getTypefacePtr(); tf != nullptr && tf->getName().isNotEmpty()
                                           && ! tf->getName().equalsIgnoreCase ("Lucida Grande"))
        {
            family = candidate;
            break;
        }
    }

    if (family.isEmpty())
        family = juce::Font::getDefaultSansSerifFontName();
}

juce::Array<juce::Colour> Theme::accentPresets()
{
    return { hex (0xF25E46),   // OpenMix coral (default)
             hex (0xF2834A),   // tangerine
             hex (0xE84F7A),   // raspberry
             hex (0x4FA8F2),   // signal blue
             hex (0x3CCBA6),   // jade
             hex (0xC9A7F5) }; // lilac
}

juce::Colour Theme::hotCueColour (int slot)
{
    static const juce::uint32 cols[] = { 0xEE5A52, 0x7FD457, 0x3FC7E6, 0x5C7CF0,
                                         0xE064C4, 0xF0B347, 0x2ED1B0, 0xA474F0 };
    return hex (cols[(size_t) juce::jlimit (0, 7, slot)]);
}

void Theme::setAccent (juce::Colour newAccent)
{
    if (newAccent == accentColour)
        return;

    accentColour = newAccent;
    sendChangeMessage();
}

float Theme::pointSize (Type t)
{
    switch (t)
    {
        case Type::micro:     return 9.0f;
        case Type::caption:   return 10.0f;
        case Type::label:     return 11.0f;
        case Type::body:      return 12.0f;
        case Type::title:     return 13.0f;
        case Type::deckTitle: return 15.0f;
        case Type::timer:     return 17.0f;
        case Type::display:   return 24.0f;
    }
    return 12.0f;
}

juce::Font Theme::font (Type t, Weight w) const
{
    const char* style = "Regular";
    switch (w)
    {
        case Weight::regular:  style = "Regular";  break;
        case Weight::medium:   style = "Medium";   break;
        case Weight::semibold: style = "Semibold"; break;
        case Weight::bold:     style = "Bold";     break;
    }

    return juce::Font (juce::FontOptions (family, style, 1.0f).withPointHeight (pointSize (t)));
}

//==============================================================================
namespace Text
{
    void draw (juce::Graphics& g, const juce::String& s, juce::Rectangle<float> r, const juce::Font& f,
               juce::Colour c, juce::Justification j, bool ellipsis)
    {
        g.setFont (f);
        g.setColour (c);
        g.drawText (s, r, j, ellipsis);
    }

    static float digitAdvance (const juce::Font& f)
    {
        float w = 0.0f;
        for (char d = '0'; d <= '9'; ++d)
            w = juce::jmax (w, juce::GlyphArrangement::getStringWidth (f, juce::String::charToString (d)));
        return w;
    }

    float tabularWidth (const juce::String& s, const juce::Font& f)
    {
        const auto adv = digitAdvance (f);
        float w = 0.0f;
        for (auto ch : s)
            w += juce::CharacterFunctions::isDigit (ch) ? adv
                                                       : juce::GlyphArrangement::getStringWidth (f, juce::String::charToString (ch));
        return w;
    }

    void drawTabular (juce::Graphics& g, const juce::String& s, juce::Rectangle<float> r, const juce::Font& f,
                      juce::Colour c, juce::Justification j)
    {
        const auto adv   = digitAdvance (f);
        const auto total = tabularWidth (s, f);

        float x = r.getX();
        if (j.testFlags (juce::Justification::horizontallyCentred)) x = r.getCentreX() - total * 0.5f;
        else if (j.testFlags (juce::Justification::right))          x = r.getRight() - total;

        g.setFont (f);
        g.setColour (c);

        float baseline = r.getCentreY() + (f.getAscent() - f.getDescent()) * 0.5f;
        if (j.testFlags (juce::Justification::top))         baseline = r.getY() + f.getAscent();
        else if (j.testFlags (juce::Justification::bottom)) baseline = r.getBottom() - f.getDescent();

        juce::GlyphArrangement ga;
        for (auto ch : s)
        {
            const auto str  = juce::String::charToString (ch);
            const auto natW = juce::GlyphArrangement::getStringWidth (f, str);
            const auto cell = juce::CharacterFunctions::isDigit (ch) ? adv : natW;
            ga.addLineOfText (f, str, x + (cell - natW) * 0.5f, baseline);
            x += cell;
        }
        ga.draw (g);
    }

    void caption (juce::Graphics& g, const juce::String& s, juce::Rectangle<float> r, juce::Colour c,
                  juce::Justification j)
    {
        auto f = Theme::get().font (Type::caption, Weight::medium);
        f.setExtraKerningFactor (0.06f);

        // towupper is locale-dependent; map Spanish accented letters explicitly.
        auto up = s.toUpperCase();
        const juce::String lower = juce::String::fromUTF8 ("áéíóúñü"), upper = juce::String::fromUTF8 ("ÁÉÍÓÚÑÜ");
        for (int i = 0; i < lower.length(); ++i)
            up = up.replaceCharacter (lower[i], upper[i]);

        draw (g, up, r, f, c, j, false);
    }
}

} // namespace om
