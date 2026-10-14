#pragma once

#include "../Components/Controls.h"
#include "../../Control/KeyboardControl.h"

namespace om
{

/** Settings ▸ Controllers ▸ Keyboard: the computer keyboard drawn key by key, each cap showing the
    function mapped to it with a deck-coloured stripe. Clicking a key reassigns it. */
class KeyboardMapView : public juce::Component
{
public:
    KeyboardMapView (KeyboardControl&, Actions&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override   { hover = -1; repaint(); }

private:
    struct Key { int code; juce::String name; float width; juce::Rectangle<float> bounds; };

    void buildKeys();
    int keyAt (juce::Point<float>) const;
    void showAssignMenu (int keyIndex);

    KeyboardControl& keyboard;
    Actions& actions;
    std::vector<Key> keys;
    int hover = -1;
    Segmented layer { { "NORMAL", "SHIFT" } };
    Button factory { "de fábrica" };
    juce::Rectangle<float> board, note;
};

} // namespace om
