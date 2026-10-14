#pragma once

#include "../Components/Controls.h"
#include "../../Control/MappingWizard.h"

namespace om
{

/** Settings ▸ Controllers ▸ "Mapear con asistente": one big instruction at a time, the controller's
    own controls answer it. While open, the controller does not trigger anything in the decks. */
class MappingWizardView : public juce::Component, private juce::Timer
{
public:
    MappingWizardView (MidiEngine&, const Actions&);
    ~MappingWizardView() override;

    void start();
    std::function<void()> onClosed;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void handle (const juce::String& device, const juce::MidiMessage&);
    void save();
    void share();
    void close();
    void refreshButtons();

    MidiEngine& midi;
    const Actions& actions;
    std::unique_ptr<MappingWizard> wizard;
    juce::String device;                          // the first controller that answers
    juce::String flash;                           // short confirmation under the prompt
    juce::uint32 flashAt = 0;
    bool saved = false;

    Button skipBtn { juce::String::fromUTF8 ("Saltear (no lo tiene)") }, cancelBtn { "Cancelar" },
           saveBtn { "Guardar mapeo" }, shareBtn { "Compartir con la comunidad" };
};

} // namespace om
