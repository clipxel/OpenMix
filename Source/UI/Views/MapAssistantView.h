#pragma once

#include "../Components/Controls.h"
#include "../../Control/MapAssistant.h"
#include "../../Control/MidiEngine.h"
#include "../../Control/KeyboardControl.h"

namespace om
{

/** Settings ▸ Controllers ▸ "Mapear escribiendo": a chat. The DJ writes what a control should do in
    their own words; the assistant says what it understood (function, deck, SHIFT layer, with other
    readings to pick from); then the DJ presses the control on the controller, or a key of the
    computer (holding Shift = the SHIFT layer), and it is mapped. Works without internet. */
class MapAssistantView : public juce::Component, private juce::Timer
{
public:
    MapAssistantView (MidiEngine&, Actions&, KeyboardControl*);
    ~MapAssistantView() override;

    void start();
    std::function<void()> onClosed;
    std::function<void()> onMapped;                // a binding was added (refresh lists)

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void sendForSnapshot (const juce::String& t)  { input.setText (t, false); send(); }

private:
    struct Message { bool user; juce::String text; };

    void timerCallback() override;
    void send();
    void propose (const MapAssistant::Reading&, const std::vector<MapAssistant::Reading>& others);
    void confirm();
    void finished (const juce::String& what);
    void cancelWaiting();
    void close();
    void say (const juce::String& text)            { history.push_back ({ false, text }); repaint(); }
    juce::String describe (const MapAssistant::Reading&) const;
    void refreshButtons();

    MidiEngine& midi;
    Actions& actions;
    KeyboardControl* keyboard;
    MapAssistant assistant { actions };

    std::vector<Message> history;
    std::optional<MapAssistant::Reading> pending;   // waiting for "sí"
    std::vector<MapAssistant::Reading> alternatives;
    bool waiting = false;                           // waiting for the control / key
    juce::String lastAction, lastLabel;             // for "lo mismo para el deck 2"

    juce::TextEditor input;
    Button sendBtn { "Enviar" }, confirmBtn { juce::String::fromUTF8 ("Sí, mapearlo") }, cancelBtn { "Cerrar" };
    std::array<Button, 2> altBtns;
    Segmented deckSel { { "DECK 1 / IZQ", "DECK 2 / DER", "DECK 3", "DECK 4" } };
    Segmented layerSel { { "SIN SHIFT", "CON SHIFT" } };
    juce::Rectangle<int> chatArea, proposalArea;
};

} // namespace om
