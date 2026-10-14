#include "Icons.h"

namespace om::Icons
{

bool isFilled (Icon i)
{
    return i == Icon::play || i == Icon::pause || i == Icon::stop || i == Icon::record
        || i == Icon::dot  || i == Icon::eject;
}

juce::Path path (Icon i)
{
    juce::Path p;

    switch (i) // NOLINT
    {
        case Icon::none: break;

        case Icon::play:
            p.addTriangle (4.5f, 2.5f, 4.5f, 13.5f, 13.5f, 8.0f);
            break;

        case Icon::pause:
            p.addRoundedRectangle (4.0f, 3.0f, 2.8f, 10.0f, 0.6f);
            p.addRoundedRectangle (9.2f, 3.0f, 2.8f, 10.0f, 0.6f);
            break;

        case Icon::playPause:
            p.addTriangle (2.0f, 3.5f, 2.0f, 12.5f, 8.5f, 8.0f);
            p.startNewSubPath (11.0f, 4.0f); p.lineTo (11.0f, 12.0f);
            p.startNewSubPath (14.0f, 4.0f); p.lineTo (14.0f, 12.0f);
            break;

        case Icon::stop:
            p.addRoundedRectangle (3.5f, 3.5f, 9.0f, 9.0f, 1.0f);
            break;

        case Icon::loop:
            p.startNewSubPath (3.0f, 9.0f);
            p.lineTo (3.0f, 6.5f);
            p.quadraticTo (3.0f, 4.5f, 5.0f, 4.5f);
            p.lineTo (12.0f, 4.5f);
            p.startNewSubPath (10.0f, 2.5f); p.lineTo (12.0f, 4.5f); p.lineTo (10.0f, 6.5f);
            p.startNewSubPath (13.0f, 7.0f);
            p.lineTo (13.0f, 9.5f);
            p.quadraticTo (13.0f, 11.5f, 11.0f, 11.5f);
            p.lineTo (4.0f, 11.5f);
            p.startNewSubPath (6.0f, 9.5f); p.lineTo (4.0f, 11.5f); p.lineTo (6.0f, 13.5f);
            break;

        case Icon::chevronLeft:  p.startNewSubPath (10.0f, 3.5f); p.lineTo (5.5f, 8.0f); p.lineTo (10.0f, 12.5f); break;
        case Icon::chevronRight: p.startNewSubPath (6.0f, 3.5f);  p.lineTo (10.5f, 8.0f); p.lineTo (6.0f, 12.5f); break;
        case Icon::chevronDown:  p.startNewSubPath (3.5f, 6.0f);  p.lineTo (8.0f, 10.5f); p.lineTo (12.5f, 6.0f); break;
        case Icon::chevronUp:    p.startNewSubPath (3.5f, 10.0f); p.lineTo (8.0f, 5.5f);  p.lineTo (12.5f, 10.0f); break;

        case Icon::search:
            p.addEllipse (2.5f, 2.5f, 8.0f, 8.0f);
            p.startNewSubPath (9.5f, 9.5f); p.lineTo (13.5f, 13.5f);
            break;

        case Icon::folder:
            p.startNewSubPath (2.0f, 4.0f);
            p.lineTo (6.0f, 4.0f); p.lineTo (7.5f, 5.5f); p.lineTo (14.0f, 5.5f);
            p.lineTo (14.0f, 12.5f); p.lineTo (2.0f, 12.5f); p.closeSubPath();
            break;

        case Icon::folderOpen:
            p.startNewSubPath (2.0f, 12.5f);
            p.lineTo (2.0f, 4.0f); p.lineTo (6.0f, 4.0f); p.lineTo (7.5f, 5.5f); p.lineTo (12.5f, 5.5f); p.lineTo (12.5f, 7.5f);
            p.startNewSubPath (2.0f, 12.5f); p.lineTo (4.0f, 7.5f); p.lineTo (15.0f, 7.5f); p.lineTo (13.0f, 12.5f); p.closeSubPath();
            break;

        case Icon::playlist:
            p.startNewSubPath (2.0f, 4.0f);  p.lineTo (10.0f, 4.0f);
            p.startNewSubPath (2.0f, 8.0f);  p.lineTo (10.0f, 8.0f);
            p.startNewSubPath (2.0f, 12.0f); p.lineTo (7.0f, 12.0f);
            p.startNewSubPath (12.5f, 6.0f); p.lineTo (12.5f, 12.5f);
            p.addEllipse (9.5f, 11.0f, 3.0f, 3.0f);
            break;

        case Icon::history:
            p.addEllipse (2.5f, 2.5f, 11.0f, 11.0f);
            p.startNewSubPath (8.0f, 5.0f); p.lineTo (8.0f, 8.0f); p.lineTo (10.5f, 9.5f);
            break;

        case Icon::collection:
            p.addRoundedRectangle (2.5f, 5.5f, 11.0f, 8.0f, 1.5f);
            p.startNewSubPath (4.0f, 3.5f); p.lineTo (12.0f, 3.5f);
            p.startNewSubPath (5.5f, 1.8f); p.lineTo (10.5f, 1.8f);
            break;

        case Icon::music:
            p.startNewSubPath (6.0f, 12.0f); p.lineTo (6.0f, 3.0f); p.lineTo (13.0f, 2.0f); p.lineTo (13.0f, 11.0f);
            p.addEllipse (2.5f, 10.5f, 3.5f, 3.0f);
            p.addEllipse (9.5f, 9.5f, 3.5f, 3.0f);
            break;

        case Icon::settings:
            // Three sliders — reads as "configure" without a skeuomorphic gear.
            p.startNewSubPath (2.0f, 4.0f);  p.lineTo (14.0f, 4.0f);
            p.startNewSubPath (2.0f, 8.0f);  p.lineTo (14.0f, 8.0f);
            p.startNewSubPath (2.0f, 12.0f); p.lineTo (14.0f, 12.0f);
            p.addEllipse (4.0f, 2.5f, 3.0f, 3.0f);
            p.addEllipse (9.5f, 6.5f, 3.0f, 3.0f);
            p.addEllipse (5.5f, 10.5f, 3.0f, 3.0f);
            break;

        case Icon::record:
            p.addEllipse (3.5f, 3.5f, 9.0f, 9.0f);
            break;

        case Icon::speaker:
            p.startNewSubPath (2.5f, 6.0f); p.lineTo (5.0f, 6.0f); p.lineTo (8.5f, 3.0f); p.lineTo (8.5f, 13.0f);
            p.lineTo (5.0f, 10.0f); p.lineTo (2.5f, 10.0f); p.closeSubPath();
            p.startNewSubPath (11.0f, 5.5f); p.quadraticTo (12.5f, 8.0f, 11.0f, 10.5f);
            p.startNewSubPath (13.0f, 3.5f); p.quadraticTo (15.5f, 8.0f, 13.0f, 12.5f);
            break;

        case Icon::controller:
            p.addRoundedRectangle (1.5f, 4.0f, 13.0f, 8.0f, 1.5f);
            p.addEllipse (3.5f, 6.0f, 4.0f, 4.0f);
            p.startNewSubPath (10.0f, 6.0f); p.lineTo (10.0f, 10.0f);
            p.startNewSubPath (12.5f, 6.0f); p.lineTo (12.5f, 10.0f);
            break;

        case Icon::cpu:
            p.addRoundedRectangle (4.0f, 4.0f, 8.0f, 8.0f, 1.0f);
            for (float t : { 6.0f, 10.0f })
            {
                p.startNewSubPath (t, 1.5f);  p.lineTo (t, 4.0f);
                p.startNewSubPath (t, 12.0f); p.lineTo (t, 14.5f);
                p.startNewSubPath (1.5f, t);  p.lineTo (4.0f, t);
                p.startNewSubPath (12.0f, t); p.lineTo (14.5f, t);
            }
            break;

        case Icon::warning:
            p.startNewSubPath (8.0f, 2.0f); p.lineTo (14.5f, 13.5f); p.lineTo (1.5f, 13.5f); p.closeSubPath();
            p.startNewSubPath (8.0f, 6.0f); p.lineTo (8.0f, 9.5f);
            p.startNewSubPath (8.0f, 11.3f); p.lineTo (8.0f, 11.6f);
            break;

        case Icon::error:
            p.addEllipse (2.0f, 2.0f, 12.0f, 12.0f);
            p.startNewSubPath (5.5f, 5.5f); p.lineTo (10.5f, 10.5f);
            p.startNewSubPath (10.5f, 5.5f); p.lineTo (5.5f, 10.5f);
            break;

        case Icon::unplugged:
            p.startNewSubPath (2.0f, 14.0f); p.lineTo (14.0f, 2.0f);
            p.addRoundedRectangle (6.0f, 3.0f, 6.0f, 5.0f, 1.0f);
            p.startNewSubPath (7.5f, 1.0f); p.lineTo (7.5f, 3.0f);
            p.startNewSubPath (10.5f, 1.0f); p.lineTo (10.5f, 3.0f);
            p.startNewSubPath (9.0f, 8.0f); p.lineTo (9.0f, 10.0f); p.quadraticTo (9.0f, 13.0f, 5.0f, 13.0f);
            break;

        case Icon::check:
            p.startNewSubPath (3.0f, 8.5f); p.lineTo (6.5f, 12.0f); p.lineTo (13.0f, 4.5f);
            break;

        case Icon::plus:
            p.startNewSubPath (8.0f, 3.0f); p.lineTo (8.0f, 13.0f);
            p.startNewSubPath (3.0f, 8.0f); p.lineTo (13.0f, 8.0f);
            break;

        case Icon::minus:
            p.startNewSubPath (3.0f, 8.0f); p.lineTo (13.0f, 8.0f);
            break;

        case Icon::close:
            p.startNewSubPath (4.0f, 4.0f); p.lineTo (12.0f, 12.0f);
            p.startNewSubPath (12.0f, 4.0f); p.lineTo (4.0f, 12.0f);
            break;

        case Icon::headphones:
            p.startNewSubPath (2.5f, 10.0f); p.lineTo (2.5f, 8.0f);
            p.cubicTo (2.5f, 1.5f, 13.5f, 1.5f, 13.5f, 8.0f); p.lineTo (13.5f, 10.0f);
            p.addRoundedRectangle (2.0f, 9.0f, 3.0f, 5.0f, 1.0f);
            p.addRoundedRectangle (11.0f, 9.0f, 3.0f, 5.0f, 1.0f);
            break;

        case Icon::lock:
            p.addRoundedRectangle (3.0f, 7.0f, 10.0f, 7.0f, 1.2f);
            p.startNewSubPath (5.0f, 7.0f); p.lineTo (5.0f, 5.0f);
            p.cubicTo (5.0f, 1.5f, 11.0f, 1.5f, 11.0f, 5.0f); p.lineTo (11.0f, 7.0f);
            break;

        case Icon::eject:
            p.addTriangle (8.0f, 3.0f, 13.5f, 9.0f, 2.5f, 9.0f);
            p.addRectangle (2.5f, 11.0f, 11.0f, 2.0f);
            break;

        case Icon::layout:
            p.addRoundedRectangle (2.0f, 2.5f, 12.0f, 11.0f, 1.5f);
            p.startNewSubPath (2.0f, 6.5f); p.lineTo (14.0f, 6.5f);
            p.startNewSubPath (8.0f, 6.5f); p.lineTo (8.0f, 13.5f);
            break;

        case Icon::jog:
            p.addEllipse (2.0f, 2.0f, 12.0f, 12.0f);
            p.addEllipse (6.0f, 6.0f, 4.0f, 4.0f);
            p.startNewSubPath (8.0f, 2.0f); p.lineTo (8.0f, 4.5f);
            break;

        case Icon::waveform:
            {
                const float hs[] = { 2.0f, 5.0f, 9.0f, 4.0f, 11.0f, 6.0f, 3.0f };
                for (int k = 0; k < 7; ++k)
                {
                    const float x = 2.0f + (float) k * 2.0f;
                    p.startNewSubPath (x, 8.0f - hs[k] * 0.5f); p.lineTo (x, 8.0f + hs[k] * 0.5f);
                }
            }
            break;

        case Icon::fx:
            p.startNewSubPath (2.0f, 11.0f);
            p.cubicTo (5.0f, 11.0f, 5.0f, 5.0f, 8.0f, 5.0f);
            p.cubicTo (11.0f, 5.0f, 11.0f, 11.0f, 14.0f, 11.0f);
            break;

        case Icon::dot:
            p.addEllipse (5.0f, 5.0f, 6.0f, 6.0f);
            break;
    }

    return p;
}

void draw (juce::Graphics& g, Icon i, juce::Rectangle<float> bounds, juce::Colour c, float strokeWidth)
{
    if (i == Icon::none)
        return;

    auto p = path (i);
    const auto side = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto box  = juce::Rectangle<float> (side, side).withCentre (bounds.getCentre());
    const auto scale = side / 16.0f;

    p.applyTransform (juce::AffineTransform::scale (scale).translated (box.getX(), box.getY()));
    g.setColour (c);

    if (isFilled (i))
        g.fillPath (p);
    else
        g.strokePath (p, juce::PathStrokeType (strokeWidth * juce::jmax (0.75f, scale), juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
}

} // namespace om::Icons
