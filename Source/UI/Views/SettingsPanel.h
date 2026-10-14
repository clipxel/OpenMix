#pragma once

#include "../Components/Controls.h"
#include "../Workspace.h"
#include "../../Engine/AudioEngine.h"
#include "../../Library/Library.h"
#include "../../Control/MidiEngine.h"
#include "../../Control/ControllerLibrary.h"
#include "../../Control/KeyboardControl.h"
#include "KeyboardMapView.h"
#include "AudioSettingsView.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace om
{

/** In-window settings sheet (no extra OS windows). Sections share the panel language. */
class SettingsPanel : public juce::Component, private juce::Timer
{
public:
    enum Section { audio = 0, appearance, layout, controllers, diagnostics, numSections };

    SettingsPanel (AudioEngine&, Library&);
    ~SettingsPanel() override;

    void show (int section);
    void showControllerTab (int tab)               { ctrlTab.setSelected (tab); setSection (controllers); }
    void setControllers (MidiEngine*, Actions*, KeyboardControl*);
    void setControllerLibrary (ControllerLibrary*);
    std::function<void()> onControllerOnlineChanged;
    void close();

    // Layout section talks to the main window through these.
    std::function<Workspace&()> currentWorkspace;
    std::function<void()> onWorkspaceEdited;
    std::function<void()> onResetSizes;
    std::function<void()> onClosed;
    std::function<void()> onAudioRoutingChanged;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;
    void setSection (int);
    void syncLayoutControls();

    AudioEngine& engine;
    Library& library;
    int section = audio;

    juce::Rectangle<int> sheet, nav, content;
    std::array<std::unique_ptr<Button>, numSections> navButtons;
    Button closeBtn { {}, Icon::close, Button::Style::ghost };

    AudioSettingsView audioView { engine };

    // Appearance
    std::vector<std::unique_ptr<Button>> swatches;
    juce::TextEditor hexField;

    // Layout
    juce::Slider laneSlider;
    Button resetSizes { juce::String::fromUTF8 ("Restablecer tamaños originales") };
    Segmented xfCurve { { "SUAVE", "CORTE", "POTENCIA" } };
    Segmented limCeiling { { "-0.1 dBTP", "-0.5 dBTP", "-1.0 dBTP" } };
    juce::ToggleButton autoGainToggle { "Auto-gain: igualar sonoridad al cargar (objetivo -14 LUFS)" };

    // Controllers
    class BindingList;
    void startLearnFlow();
    void layoutControllers (juce::Rectangle<int>);
    MidiEngine* midi = nullptr;
    ControllerLibrary* controllerLib = nullptr;
    Button driverBtn { juce::String(), Icon::none };
    juce::String driverPage;
    juce::ToggleButton onlineToggle { "Al conectar un controlador, buscar su mapeo en internet (solo se descarga la lista pública de mapeos)" };
    Actions* actions = nullptr;
    KeyboardControl* keyboard = nullptr;
    Segmented ctrlTab { { "MIDI", "TECLADO" } };
    juce::ToggleButton vinylToggle { "Modo vinilo en ambos decks (cada deck tiene su botón VINYL)" }, learnShift { "Capa SHIFT" };
    Button learnBtn { "Aprender control", Icon::plus }, cancelLearnBtn { "Cancelar" };
    std::unique_ptr<BindingList> bindingList;
    std::unique_ptr<KeyboardMapView> keyMap;
    juce::ListBox bindingBox;
    juce::Rectangle<int> monitorArea, ctrlListArea;
    juce::String learnStatus;
};

} // namespace om
