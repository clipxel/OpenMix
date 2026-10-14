#pragma once

#include "../Components/Controls.h"
#include "../../Engine/AudioEngine.h"

namespace om
{

/** Settings ▸ Audio: SALIDAS (what goes out), HARDWARE (which card / a "Y"
    splitter) and ENTRADAS, then one row per output with its card, channel pair and format. Every
    change applies at once. One card is used at a time (all rows share it). */
class AudioSettingsView : public juce::Component, private juce::ChangeListener
{
public:
    explicit AudioSettingsView (AudioEngine&);
    ~AudioSettingsView() override;

    void refresh();
    std::function<void()> onRoutingChanged;      // persist settings

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Role { master, phones, deck1, deck2 };
    struct Row
    {
        Role role;
        juce::ComboBox device, channels;
        Button info { {}, Icon::none, Button::Style::ghost };
        juce::Rectangle<int> label;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override  { refresh(); }
    void setMode (int mode);
    void selectDevice (const juce::String& name);
    void chooseHardware (bool builtIn);
    void showFormatMenu (Button&);
    static bool isBuiltIn (const juce::String& deviceName);
    juce::StringArray outputDevices() const;
    std::atomic<int>* channelFor (Role);
    void changed();

    AudioEngine& engine;
    Button outSpeakers { "SOLO ALTAVOCES", Icon::speaker }, outPhones { juce::String::fromUTF8 ("ALTAVOCES + AUDÍFONOS"), Icon::headphones },
           outDecks { "DECKS SEPARADOS", Icon::layout };
    Button hwBuiltIn { "AUDIO INTERCONSTRUIDO", Icon::cpu }, hwUsb { "TARJETA(S) DE AUDIO USB", Icon::controller },
           hwSplit { juce::String::fromUTF8 ("DIVISOR EN \"Y\" DE ESTÉREO A MONOAURAL"), Icon::headphones };
    Button inMic { juce::String::fromUTF8 ("MICRÓFONO") }, inTimecode { juce::String::fromUTF8 ("CÓDIGO DE TIEMPO") },
           inLine { juce::String::fromUTF8 ("ENTRADA DE LÍNEA") };
    std::array<std::unique_ptr<Row>, 3> rows;
    int visibleRows = 0;
    juce::Rectangle<int> outBox, hwBox, inBox, tableTitle, noteArea;
    juce::String note;
};

} // namespace om
