#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace om
{

/** Simple geometric icon set drawn on a 16×16 grid with 1.5pt strokes.
    Original drawings — no third-party glyphs. */
enum class Icon
{
    none,
    play, pause, playPause, stop,
    loop, chevronLeft, chevronRight, chevronDown, chevronUp,
    search, folder, folderOpen, playlist, history, collection, music,
    settings, record, speaker, controller, cpu,
    warning, error, unplugged, check, plus, minus, close,
    headphones, lock, eject, layout, jog, waveform, fx, dot
};

namespace Icons
{
    /** Returns the outline/fill geometry inside a 0..16 box. */
    juce::Path path (Icon);

    /** True if the icon is drawn filled, false for stroked. */
    bool isFilled (Icon);

    void draw (juce::Graphics&, Icon, juce::Rectangle<float> bounds, juce::Colour, float strokeWidth = 1.5f);
}

} // namespace om
