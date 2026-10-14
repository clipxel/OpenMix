#include "AudioSettingsView.h"

namespace om
{

AudioSettingsView::AudioSettingsView (AudioEngine& e) : engine (e)
{
    for (auto* b : { &outSpeakers, &outPhones, &outDecks, &hwBuiltIn, &hwUsb, &hwSplit, &inMic, &inTimecode, &inLine })
    {
        b->setTextType (Type::body, Weight::bold);
        b->useAccentWhenLit();
        addAndMakeVisible (*b);
    }
    outSpeakers.onClick = [this] { setMode (AudioEngine::speakersOnly); };
    outPhones.onClick   = [this] { setMode (AudioEngine::speakersPhones); };
    outDecks.onClick    = [this] { setMode (AudioEngine::separateDecks); };
    outSpeakers.setTooltip ("El master sale por una salida estéreo"_u);
    outPhones.setTooltip (juce::String::fromUTF8 ("Master por una salida y auriculares (pre-escucha) por otra"));
    outDecks.setTooltip (juce::String::fromUTF8 ("Cada deck por su propia salida, para un mixer externo"));

    hwBuiltIn.onClick = [this] { chooseHardware (true); };
    hwUsb.onClick     = [this] { chooseHardware (false); };
    hwSplit.onClick   = [this]
    {
        const bool on = ! engine.splitMono.load();
        engine.splitMono = on;
        if (on) engine.outputMode = AudioEngine::speakersPhones;
        changed();
    };
    hwSplit.setTooltip (juce::String::fromUTF8 ("Con un cable divisor en Y: izquierda = master (mono), derecha = auriculares (mono)"));

    for (auto* b : { &inMic, &inTimecode, &inLine })
        b->setNotImplemented (true, juce::String::fromUTF8 ("Entradas de audio: todavía no disponibles"));

    for (auto& row : rows)
    {
        row = std::make_unique<Row>();
        auto* raw = row.get();
        raw->device.onChange = [this, raw] { selectDevice (raw->device.getText()); };
        raw->channels.onChange = [this, raw]
        {
            const int id = raw->channels.getSelectedId();
            if (id <= 0) return;
            if (auto* ch = channelFor (raw->role)) *ch = id == 1000 ? -1 : id - 1;
            changed();
        };
        raw->info.setTextType (Type::label, Weight::medium);
        raw->info.setTooltip ("Frecuencia de muestreo y buffer");
        raw->info.onClick = [this, raw] { showFormatMenu (raw->info); };
        addChildComponent (raw->device);
        addChildComponent (raw->channels);
        addChildComponent (raw->info);
    }

    engine.getDeviceManager().addChangeListener (this);
    refresh();
}

AudioSettingsView::~AudioSettingsView()
{
    engine.getDeviceManager().removeChangeListener (this);
}

bool AudioSettingsView::isBuiltIn (const juce::String& n)
{
    for (auto* k : { "MacBook", "Built-in", "Internal", "Bocinas", "Altavoces", "Speakers", "Interno", "iMac", "Mac mini",
                     "Mac Studio", "Mac Pro", "Headphones", "Auriculares", "Audífonos" })
        if (n.containsIgnoreCase (juce::String::fromUTF8 (k))) return true;
    return false;
}

juce::StringArray AudioSettingsView::outputDevices() const
{
    if (auto* type = engine.getDeviceManager().getCurrentDeviceTypeObject())
        return type->getDeviceNames (false);
    return {};
}

std::atomic<int>* AudioSettingsView::channelFor (Role r)
{
    switch (r)
    {
        case Role::master: return &engine.masterChannel;
        case Role::phones: return &engine.phonesChannel;
        case Role::deck1:  return &engine.deckChannel[0];
        case Role::deck2:  return &engine.deckChannel[1];
    }
    return nullptr;
}

void AudioSettingsView::changed()
{
    refresh();
    if (onRoutingChanged) onRoutingChanged();
}

void AudioSettingsView::setMode (int mode)
{
    engine.outputMode = mode;
    if (mode != AudioEngine::speakersPhones) engine.splitMono = false;
    changed();
}

void AudioSettingsView::selectDevice (const juce::String& name)
{
    auto& dm = engine.getDeviceManager();
    auto setup = dm.getAudioDeviceSetup();
    if (name.isEmpty() || name == setup.outputDeviceName) return;
    setup.outputDeviceName = name;
    setup.inputDeviceName = {};
    setup.useDefaultOutputChannels = true;
    const auto err = dm.setAudioDeviceSetup (setup, true);
    engine.enableAllOutputs();
    note = err;

    // Sensible pairs for the new card: master on 1 & 2, headphones / deck 2 on 3 & 4 when it has them.
    const int outs = dm.getCurrentAudioDevice() != nullptr ? dm.getCurrentAudioDevice()->getOutputChannelNames().size() : 2;
    engine.masterChannel = 0;
    engine.phonesChannel = outs >= 4 ? 2 : -1;
    engine.deckChannel[0] = 0;
    engine.deckChannel[1] = outs >= 4 ? 2 : 0;
    changed();
}

void AudioSettingsView::chooseHardware (bool builtIn)
{
    for (auto& n : outputDevices())
        if (isBuiltIn (n) == builtIn)
        {
            engine.splitMono = false;
            selectDevice (n);
            changed();
            return;
        }
    note = builtIn ? juce::String::fromUTF8 ("No se encontró el audio interno")
                   : juce::String::fromUTF8 ("No hay ninguna tarjeta de audio USB conectada");
    repaint();
}

void AudioSettingsView::showFormatMenu (Button& target)
{
    auto& dm = engine.getDeviceManager();
    auto* dev = dm.getCurrentAudioDevice();
    if (dev == nullptr) return;
    juce::PopupMenu m;
    m.addSectionHeader ("Frecuencia de muestreo");
    for (auto rate : dev->getAvailableSampleRates())
        m.addItem (juce::String (rate / 1000.0, 1) + " kHz", true, std::abs (rate - dev->getCurrentSampleRate()) < 1.0, [this, rate]
        {
            auto s = engine.getDeviceManager().getAudioDeviceSetup();
            s.sampleRate = rate;
            engine.getDeviceManager().setAudioDeviceSetup (s, true);
            changed();
        });
    m.addSectionHeader ("Buffer (latencia)");
    for (auto size : dev->getAvailableBufferSizes())
    {
        if (size < 32 || size > 4096) continue;
        m.addItem (juce::String (size) + "  (" + juce::String (1000.0 * size / dev->getCurrentSampleRate(), 1) + " ms)", true,
                   size == dev->getCurrentBufferSizeSamples(), [this, size]
        {
            auto s = engine.getDeviceManager().getAudioDeviceSetup();
            s.bufferSize = size;
            engine.getDeviceManager().setAudioDeviceSetup (s, true);
            changed();
        });
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
}

void AudioSettingsView::refresh()
{
    const int mode = engine.outputMode.load();
    const bool split = engine.splitMono.load();
    auto& dm = engine.getDeviceManager();
    auto* dev = dm.getCurrentAudioDevice();
    const auto current = dm.getAudioDeviceSetup().outputDeviceName;
    const int outs = dev != nullptr ? dev->getOutputChannelNames().size() : 0;

    outSpeakers.setToggleState (mode == AudioEngine::speakersOnly, juce::dontSendNotification);
    outPhones.setToggleState (mode == AudioEngine::speakersPhones, juce::dontSendNotification);
    outDecks.setToggleState (mode == AudioEngine::separateDecks, juce::dontSendNotification);
    hwBuiltIn.setToggleState (dev != nullptr && isBuiltIn (current) && ! split, juce::dontSendNotification);
    hwUsb.setToggleState (dev != nullptr && ! isBuiltIn (current), juce::dontSendNotification);
    hwSplit.setToggleState (split, juce::dontSendNotification);

    std::vector<Role> roles;
    if (mode == AudioEngine::speakersOnly)        roles = { Role::master };
    else if (mode == AudioEngine::speakersPhones) roles = { Role::master, Role::phones };
    else                                          roles = { Role::deck1, Role::deck2, Role::phones };
    visibleRows = (int) roles.size();

    const auto devices = outputDevices();
    juce::String info = "-";
    if (dev != nullptr)
    {
        const double sr = dev->getCurrentSampleRate();
        info = juce::String (sr / 1000.0, sr == 44100.0 ? 1 : 0) + "k " + juce::String (1000.0 * dev->getCurrentBufferSizeSamples() / sr, 1)
             + "ms " + juce::String (outs) + "out";
    }

    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto& row = *rows[i];
        const bool vis = (int) i < visibleRows;
        row.device.setVisible (vis);
        row.channels.setVisible (vis);
        row.info.setVisible (vis);
        if (! vis) continue;
        row.role = roles[i];

        row.device.clear (juce::dontSendNotification);
        for (int k = 0; k < devices.size(); ++k)
            row.device.addItem (devices[k], k + 1);
        row.device.setSelectedId (devices.indexOf (current) + 1, juce::dontSendNotification);

        row.channels.clear (juce::dontSendNotification);
        if (split && (row.role == Role::master || row.role == Role::phones))
        {
            const int mc = engine.masterChannel.load();
            row.channels.addItem ("chan " + juce::String (mc + (row.role == Role::master ? 1 : 2)) + " (mono)", 1);
            row.channels.setSelectedId (1, juce::dontSendNotification);
            row.channels.setEnabled (false);
        }
        else
        {
            row.channels.setEnabled (true);
            for (int c = 0; c + 1 < juce::jmax (2, outs); c += 2)
                row.channels.addItem ("chan " + juce::String (c + 1) + " & " + juce::String (c + 2), c + 1);
            if (row.role == Role::phones) row.channels.addItem ("ninguna", 1000);
            const int ch = channelFor (row.role)->load();
            row.channels.setSelectedId (ch < 0 ? 1000 : ch + 1, juce::dontSendNotification);
        }
        row.info.setButtonText (info);
    }

    if (dev == nullptr)
        note = juce::String::fromUTF8 ("No hay tarjeta de audio activa: elegí una en la lista");
    else if (mode == AudioEngine::speakersPhones && ! split && (engine.phonesChannel.load() < 0 || engine.phonesChannel.load() + 1 >= outs))
        note = juce::String::fromUTF8 ("Esta tarjeta tiene ") + juce::String (outs)
             + juce::String::fromUTF8 (" salidas: para auriculares hace falta una tarjeta de 4 salidas o el divisor en \"Y\"");
    else if (mode == AudioEngine::separateDecks && engine.deckChannel[0].load() == engine.deckChannel[1].load())
        note = juce::String::fromUTF8 ("Los dos decks salen por el mismo par de canales: elegí pares distintos");
    else
        note = {};

    resized();
    repaint();
}

void AudioSettingsView::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (24 + 156);
    top.removeFromTop (24);
    const int gap = Space::lg;
    const int w = top.getWidth() - 2 * gap;
    outBox = top.removeFromLeft (w * 27 / 100);
    top.removeFromLeft (gap);
    inBox = top.removeFromRight (w * 27 / 100);
    top.removeFromRight (gap);
    hwBox = top;

    auto stack = [] (juce::Rectangle<int> box, std::initializer_list<Button*> bs)
    {
        auto b = box.reduced (Space::md);
        const int h = (b.getHeight() - 2 * Space::sm) / 3;
        for (auto* x : bs)
        {
            x->setBounds (b.removeFromTop (h));
            b.removeFromTop (Space::sm);
        }
    };
    stack (outBox, { &outSpeakers, &outPhones, &outDecks });
    stack (inBox, { &inMic, &inTimecode, &inLine });
    {
        auto b = hwBox.reduced (Space::md);
        const int h = (b.getHeight() - 2 * Space::sm) / 3;
        auto row1 = b.removeFromTop (h);
        b.removeFromTop (Space::sm);
        auto row2 = b.removeFromTop (h);
        const int half = (row1.getWidth() - Space::sm) / 2;
        hwBuiltIn.setBounds (row1.removeFromLeft (half));
        row1.removeFromLeft (Space::sm);
        hwUsb.setBounds (row1);
        hwSplit.setBounds (row2);
    }

    r.removeFromTop (Space::xl + 8);
    tableTitle = r.removeFromTop (22);
    r.removeFromTop (Space::sm);
    for (int i = 0; i < visibleRows; ++i)
    {
        auto& row = *rows[(size_t) i];
        auto line = r.removeFromTop (28);
        r.removeFromTop (2);
        row.label = line.removeFromLeft (130);
        row.info.setBounds (line.removeFromRight (150));
        line.removeFromRight (2);
        row.channels.setBounds (line.removeFromRight (190));
        line.removeFromRight (2);
        row.device.setBounds (line);
    }
    r.removeFromTop (Space::md);
    noteArea = r.removeFromTop (22);
}

void AudioSettingsView::paint (juce::Graphics& g)
{
    auto box = [&g] (juce::Rectangle<int> b, const juce::String& title)
    {
        auto tab = juce::Rectangle<float> ((float) b.getX() + 20.0f, (float) b.getY() - 24.0f, 150.0f, 24.0f);
        g.setColour (colour (Col::bgRaised));
        g.fillRoundedRectangle (tab.withHeight (30.0f), Radius::md);
        g.fillRoundedRectangle (b.toFloat(), Radius::lg);
        Text::draw (g, title, tab, font (Type::title, Weight::bold), colour (Col::textPrimary), juce::Justification::centred);
    };
    box (outBox, "SALIDAS");
    box (hwBox, "HARDWARE");
    box (inBox, "ENTRADAS");

    Text::draw (g, "Salidas", tableTitle.toFloat(), font (Type::title, Weight::semibold), colour (Col::textPrimary));
    for (int i = 0; i < visibleRows; ++i)
    {
        const auto& row = *rows[(size_t) i];
        const juce::String name = row.role == Role::master ? "Altavoces"
                                : row.role == Role::phones ? juce::String::fromUTF8 ("Audífonos")
                                : row.role == Role::deck1  ? "Deck 1" : "Deck 2";
        auto lr = row.label.toFloat();
        g.setColour (colour (Col::bgField));
        g.fillRoundedRectangle (lr.reduced (0, 1), Radius::sm);
        Text::draw (g, name, lr.reduced (8, 0), font (Type::body, Weight::medium),
                    row.role == Role::deck1 ? Theme::get().deckColour (0) : row.role == Role::deck2 ? Theme::get().deckColour (1)
                                            : colour (Col::textPrimary));
    }
    if (note.isNotEmpty())
        Text::draw (g, note, noteArea.toFloat(), font (Type::label, Weight::medium), colour (Col::warning));
}

} // namespace om
