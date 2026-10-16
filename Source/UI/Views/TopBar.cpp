#include "TopBar.h"
#include <BinaryData.h>
#include "../Contact.h"

namespace om
{

TopBar::TopBar (AudioEngine& e) : engine (e)
{
    addAndMakeVisible (audio);
    addAndMakeVisible (controllers);
    addAndMakeVisible (cpu);
    addAndMakeVisible (rec);
    addAndMakeVisible (settings);

    audio.onClick       = [this] { if (onOpenSettings) onOpenSettings (0); };
    controllers.onClick = [this] { if (onOpenSettings) onOpenSettings (3); };
    cpu.onClick         = [this] { if (onOpenSettings) onOpenSettings (4); };

    rec.setLitColour (colour (Col::record));
    rec.setTextType (Type::label, Weight::bold);
    rec.setTooltip ("Grabar la salida master en WAV (~/Música/OpenMix Recordings)"_u);
    rec.onClick = [this] { if (onToggleRecord) onToggleRecord(); };

    settings.setTooltip ("Configuración"_u);
    settings.onClick = [this] { if (onOpenSettings) onOpenSettings (0); };
    fx.setTextType (Type::label, Weight::bold);
    fx.setTooltip ("Mostrar u ocultar los efectos (pantallas chicas)"_u);
    fx.onClick = [this] { if (onToggleFx) onToggleFx(); };
    addChildComponent (fx);
    automixBtn.setTextType (Type::label, Weight::bold);
    automixBtn.setTooltip ("Mezcla automática: abrir / cerrar el panel"_u);
    automixBtn.onClick = [this] { if (onToggleAutoMix) onToggleAutoMix(); };
    addAndMakeVisible (automixBtn);
    helpBtn.setTooltip ("Ayuda, contacto y reportes: escribinos y te respondemos"_u);
    helpBtn.onClick = [this] { showHelpMenu(); };
    addAndMakeVisible (helpBtn);
}

void TopBar::refresh()
{
    const auto widthsBefore = audio.idealWidth() + controllers.idealWidth() + cpu.idealWidth();
    using S = AudioEngine::DeviceStatus;
    switch (engine.getStatus())
    {
        case S::running:      audio.set (Icon::speaker,   engine.getStatusDetail(), colour (Col::success)); break;
        case S::initialising: audio.set (Icon::speaker,   juce::String::fromUTF8 ("Iniciando audio…"), colour (Col::warning)); break;
        case S::error:        audio.set (Icon::error,     "Error de audio: " + engine.getStatusDetail(), colour (Col::error)); break;
        case S::disconnected: audio.set (Icon::unplugged, "Audio desconectado", colour (Col::disconnected)); break;
        case S::unavailable:  audio.set (Icon::warning,   "Sin salida de audio", colour (Col::warning)); break;
    }
    audio.setTooltip ("Dispositivo de audio — clic para configurar"_u);

    // Controllers: what the plug-and-play library found for the connected devices.
    const auto now = juce::Time::getMillisecondCounter();
    if (controllerLib != nullptr)
    {
        const auto text = controllerLib->summary();
        if (text != lastControllerText || lastMidiCount < 0)
        {
            lastControllerText = text;
            lastMidiCount = 0;
            using CS = ControllerLibrary::Status;
            const auto& devs = controllerLib->devices();
            const auto status = devs.empty() ? CS::noMapping : devs.front().status;
            const bool driverMissing = ! devs.empty() && devs.front().driverPage.isNotEmpty();
            const auto col = devs.empty() ? colour (Col::textTertiary)
                           : driverMissing ? colour (Col::warning)
                           : (status == CS::mapped || status == CS::mappedByUser) ? colour (Col::play)
                           : status == CS::searching ? colour (Col::sync) : colour (Col::warning);
            controllers.set (Icon::controller, devs.empty() ? juce::String ("Sin controlador") : text, col);
            controllers.setTooltip (devs.empty() ? juce::String::fromUTF8 ("Enchufá un controlador: OpenMix lo reconoce y lo mapea solo")
                                   : driverMissing ? "Este controlador necesita su driver. Clic para ver dónde descargarlo (página oficial del fabricante)"_u
                                   : status == CS::noMapping ? "No hay mapeo para este controlador todavía: mapealo en Configuración › Controladores"_u
                                   : status == CS::offline ? "No se pudo descargar el mapeo (sin conexión). Se reintenta al volver a conectarlo."_u
                                   : juce::String::fromUTF8 ("Controladores — clic para configurar"));
            resized();
        }
    }
    // MIDI device list is polled at 1 Hz (enumeration is not free).
    else if (now - lastMidiPoll > 1000 || lastMidiCount < 0)
    {
        lastMidiPoll = now;
        const int n = juce::MidiInput::getAvailableDevices().size();
        if (n != lastMidiCount)
        {
            lastMidiCount = n;
            if (n == 0) controllers.set (Icon::controller, "Sin controlador", colour (Col::textTertiary));
            else        controllers.set (Icon::controller, juce::String (n) + " MIDI", colour (Col::sync));
            controllers.setTooltip (n == 0 ? "No hay dispositivos MIDI conectados" : "Dispositivos MIDI detectados");
            resized();
        }
    }

    const auto load = engine.getCpuUsage();
    const int pct = (int) std::round (load * 100.0);
    cpu.set (Icon::cpu, "DSP " + juce::String (pct) + "%",
             load > 0.8 ? colour (Col::error) : load > 0.6 ? colour (Col::warning) : colour (Col::textTertiary));
    cpu.setTooltip ("Carga del callback de audio. Dropouts: " + juce::String (juce::jmax (0, engine.getXRunCount())));

    const bool recording = engine.recorder().isRecording();
    rec.setToggleState (recording, juce::dontSendNotification);
    if (recording)
    {
        const int s = (int) engine.recorder().getSecondsRecorded();
        rec.setButtonText (juce::String::formatted ("%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60));
    }
    else
    {
        rec.setButtonText ("REC");
    }
    rec.setEnabled (recording || engine.getStatus() == S::running);

    if (audio.idealWidth() + controllers.idealWidth() + cpu.idealWidth() != widthsBefore)
        resized();
}

void TopBar::resized()
{
    auto r = getLocalBounds().reduced (Space::md, 6);
    brandArea = r.removeFromLeft (124);
    r.removeFromLeft (Space::lg);

    settings.setBounds (r.removeFromRight (28));
    r.removeFromRight (Space::xs);
    helpBtn.setBounds (r.removeFromRight (28));
    r.removeFromRight (Space::sm);
    rec.setBounds (r.removeFromRight (84));
    r.removeFromRight (Space::sm);
    automixBtn.setBounds (r.removeFromRight (84));
    r.removeFromRight (Space::sm);
    if (fx.isVisible())
    {
        fx.setBounds (r.removeFromRight (44));
        r.removeFromRight (Space::sm);
    }
    r.removeFromRight (Space::md - Space::sm);
    cpu.setBounds (r.removeFromRight (cpu.idealWidth() + 4));
    controllers.setBounds (r.removeFromRight (controllers.idealWidth()));
    r.removeFromRight (Space::xs);
    audio.setBounds (r.removeFromRight (juce::jmin (audio.idealWidth(), juce::jmax (120, r.getWidth()))));
}

void TopBar::showHelpMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("Ayuda y contacto");
    m.addItem ("Ayudanos a mejorar: escribinos acá", [this] { if (onOpenFeedback) onOpenFeedback(); });
    m.addSeparator();
    m.addItem ("Unite a la comunidad de OpenMix (WhatsApp)", [] { Contact::open (Contact::community()); });
    m.addItem ("Escribinos por Instagram: " + juce::String (Contact::handle()), [] { Contact::open (Contact::instagramChat()); });
    m.addItem ("WhatsApp: buscanos como " + juce::String (Contact::handle()), false, false, nullptr);
    m.addSeparator();
    m.addItem ("Página de OpenMix", [] { Contact::open (Contact::site()); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&helpBtn));
}

void TopBar::setAutoMixState (bool panelOpen, bool running)
{
    automixBtn.setToggleState (panelOpen || running, juce::dontSendNotification);
    automixBtn.setLitColour (running ? colour (Col::play) : colour (Col::textSecondary));
}

void TopBar::setFxButton (bool visible, bool panelShown)
{
    fx.setToggleState (panelShown, juce::dontSendNotification);
    if (fx.isVisible() != visible)
    {
        fx.setVisible (visible);
        resized();
    }
}

void TopBar::paint (juce::Graphics& g)
{
    g.fillAll (colour (Col::bgApp));
    g.setColour (colour (Col::line));
    g.fillRect (getLocalBounds().removeFromBottom (1));

    // Brand mark (OpenMix logo).
    static const auto logo = juce::ImageCache::getFromMemory (BinaryData::logomark_png, BinaryData::logomark_pngSize);
    auto b = brandArea.toFloat();
    auto markArea = b.removeFromLeft (26.0f);
    if (logo.isValid())
    {
        const float h = 22.0f, w = h * (float) logo.getWidth() / (float) logo.getHeight();
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (logo, juce::Rectangle<float> (w, h).withCentre (markArea.getCentre()));
    }
    b.removeFromLeft (7.0f);
    auto f = font (Type::title, Weight::bold);
    f.setExtraKerningFactor (0.12f);
    Text::draw (g, "OPENMIX", b, f, colour (Col::textPrimary), juce::Justification::centredLeft, false);
}

} // namespace om
