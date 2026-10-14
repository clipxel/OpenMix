#include "MainComponent.h"

using namespace om;

//==============================================================================
/** Horizontal drag bar between the performance area and the library. */
class MainComponent::Splitter : public juce::Component
{
public:
    std::function<void (int deltaY)> onDrag;
    std::function<void()> onDragEnd;

    Splitter() { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); }

    void paint (juce::Graphics& g) override
    {
        getLookAndFeel().drawStretchableLayoutResizerBar (g, getWidth(), getHeight(), false, isMouseOver(), isMouseButtonDown());
    }
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }
    void mouseDown (const juce::MouseEvent& e) override { lastY = e.getScreenY(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (onDrag) onDrag (e.getScreenY() - lastY);
        lastY = e.getScreenY();
    }
    void mouseUp (const juce::MouseEvent&) override { if (onDragEnd) onDragEnd(); repaint(); }

private:
    int lastY = 0;
};

//==============================================================================
MainComponent::MainComponent (bool snapshot) : snapshotMode (snapshot)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);

    current = Workspace::defaults();

    splitter = std::make_unique<Splitter>();
    waveSplitter = std::make_unique<Splitter>();

    for (auto* c : std::initializer_list<juce::Component*> { &topBar, &waveforms, &fxPanel, &deckA, &mixer, &deckB, &libraryView,
                                                             splitter.get(), waveSplitter.get() })
        addAndMakeVisible (c);
    addChildComponent (settingsPanel);

    // Wiring ------------------------------------------------------------------
    topBar.onOpenSettings = [this] (int s) { settingsPanel.setBounds (getLocalBounds()); settingsPanel.show (s); };
    topBar.onToggleRecord = [this] { toggleRecord(); };

    deckA.onLoadRequest = [this] (const juce::File& f) { loadFileToDeck (f, 0); };
    deckB.onLoadRequest = [this] (const juce::File& f) { loadFileToDeck (f, 1); };

    libraryView.onLoadToDeck = [this] (const TrackPtr& t, int d) { loadToDeck (t, d); };
    libraryView.onAddFolder = [this] { addFolder(); };

    splitter->onDrag = [this] (int dy)
    {
        const float avail = (float) juce::jmax (1, getHeight() - topBar.getHeight() - waveforms.getHeight()
                                                  - (fxPanel.isVisible() ? fxPanel.getHeight() : 0));
        current.libraryRatio = juce::jlimit (0.12f, 0.75f, current.libraryRatio - (float) dy / avail);
        resized();
    };

    // Drag the line under the waveforms to make them taller/shorter (both lanes).
    auto laneAccum = std::make_shared<float> (0.0f);
    waveSplitter->onDrag = [this, laneAccum] (int dy)
    {
        *laneAccum += (float) dy * 0.5f;
        const int step = (int) *laneAccum;
        if (step == 0) return;
        *laneAccum -= (float) step;
        current.laneHeight = juce::jlimit (Workspace::minLane, Workspace::maxLane, current.laneHeight + step);
        resized();
    };

    settingsPanel.currentWorkspace = [this]() -> Workspace& { return current; };
    settingsPanel.onWorkspaceEdited = [this] { applyLayout(); };
    settingsPanel.onResetSizes = [this] { current = Workspace::defaults(); applyLayout(); };
    settingsPanel.onClosed = [this] { grabKeyboardFocus(); };
    settingsPanel.onAudioRoutingChanged = [this] { if (settings != nullptr) saveSettings(); };
    libraryView.onSidebarResized = [this] { if (settings != nullptr) saveSettings(); };

    // Persist cue edits to the library so they survive reloads.
    for (int i = 0; i < 2; ++i)
    {
        engine.deck (i).onCuesChanged = [this, i]
        {
            auto& d = engine.deck (i);
            auto t = d.getTrack();
            if (t == nullptr) return;

            if (auto lt = library.find (t->file.getFullPathName()))
            {
                lt->cuePoint = d.getCuePoint();
                for (int k = 0; k < 8; ++k)
                {
                    lt->hotCues[(size_t) k] = d.getHotCue (k);
                    lt->savedLoops[(size_t) k] = d.getSavedLoop (k);
                }
                library.trackEdited (lt);
            }

            // Tracks on external drives: cues also travel with the drive (<drive>/.OpenMix/cues.json).
            if (VolumeCueStore::externalVolumeFor (t->file) != juce::File())
            {
                VolumeCueStore::Cues c = cueStore.get (t->file).value_or (VolumeCueStore::Cues {});
                c.cuePoint = d.getCuePoint();
                for (int k = 0; k < 8; ++k)
                {
                    c.hotCues[(size_t) k] = d.getHotCue (k);
                    c.savedLoops[(size_t) k] = d.getSavedLoop (k);
                }
                if (const auto err = cueStore.put (t->file, c); err.isNotEmpty())
                    (i == 0 ? deckA : deckB).showMessage (err, true);
            }
        };
    }

    // Beat-grid edits: local library (locked against re-analysis) or the drive's .OpenMix.
    for (int i = 0; i < 2; ++i)
    {
        engine.deck (i).onGridChanged = [this, i]
        {
            auto& d = engine.deck (i);
            auto t = d.getTrack();
            if (t == nullptr) return;
            const auto g = d.getGrid();
            if (auto lt = library.find (t->file.getFullPathName()))
            {
                lt->bpm = g.bpm;
                lt->firstBeat = g.firstBeatSeconds;
                lt->gridEdited = true;
                library.trackEdited (lt);
            }
            if (VolumeCueStore::externalVolumeFor (t->file) != juce::File())
            {
                auto c = cueStore.get (t->file).value_or (VolumeCueStore::Cues {});
                if (c.cuePoint < 0.0 && d.getCuePoint() > 0.0) c.cuePoint = d.getCuePoint();
                c.bpm = g.bpm;
                c.firstBeat = g.firstBeatSeconds;
                if (const auto err = cueStore.put (t->file, c); err.isNotEmpty())
                    (i == 0 ? deckA : deckB).showMessage (err, true);
            }
        };
    }

    engine.addChangeListener (this);
    Theme::get().addChangeListener (this);

    if (! snapshotMode)
    {
        midi = std::make_unique<MidiEngine> (actions);
        controllerLib = std::make_unique<ControllerLibrary> (*midi);
        keyboard.load (om::Library::dataFolder().getChildFile ("keyboard.json"));
        settingsPanel.setControllers (midi.get(), &actions, &keyboard);
        settingsPanel.setControllerLibrary (controllerLib.get());
        topBar.setControllerLibrary (controllerLib.get());
        settingsPanel.onControllerOnlineChanged = [this] { if (settings != nullptr) saveSettings(); };
        settingsPanel.onUpdateCheckChanged = [this] { if (settings != nullptr) saveSettings(); };
        updater.addChangeListener (this);

        juce::PropertiesFile::Options o;
        o.applicationName = "OpenMix";
        o.filenameSuffix = ".settings";
        o.folderName = Library::dataFolder().getFullPathName();
        o.osxLibrarySubFolder = "Application Support";
        settings = std::make_unique<juce::PropertiesFile> (Library::dataFolder().getChildFile ("settings.xml"), o);
        loadSettings();

        // Updates: a quiet check a few seconds after start (only asks if there is something new).
        settingsPanel.updateToggle.setToggleState (settings->getBoolValue ("updateCheck", true), juce::dontSendNotification);
        if (settingsPanel.updateToggle.getToggleState())
            juce::Timer::callAfterDelay (3000, [safe = juce::Component::SafePointer<MainComponent> (this)] { if (safe) safe->updater.checkForUpdates(); });

        auto audioState = settings->getXmlValue ("audioDevice");
        engine.initialiseDevice (audioState.get());

        for (int d = 0; d < AudioEngine::numDecks; ++d)
            for (int p = 0; p < RmxUnit::numDrums; ++p)
                if (const juce::File f (settings->getValue ("rmxSample" + juce::String (d) + juce::String (p))); f.existsAsFile())
                    engine.loadRmxSample (d, p, f, nullptr);
        for (int d = 0; d < AudioEngine::numDecks; ++d)
        {
            engine.rmx (d).setBaseRoll (settings->getDoubleValue ("rmxRoll" + juce::String (d), RmxUnit::defaultRoll));
            engine.rmx (d).drumLevel = (float) settings->getDoubleValue ("rmxVol" + juce::String (d), 0.8);
            if (const auto tone = (float) settings->getDoubleValue ("rmxTone" + juce::String (d), 0.0); std::abs (tone) > 0.01f)
                engine.setRmxToneSpeed (d, tone, 1.0f);
        }
    }
    else
    {
        applyLayout();
    }

    vblank = std::make_unique<juce::VBlankAttachment> (this, [this] { frame(); });
}

MainComponent::~MainComponent()
{
    vblank = nullptr;
    if (! snapshotMode)
        saveSettings();
    updater.removeChangeListener (this);
    Theme::get().removeChangeListener (this);
    engine.removeChangeListener (this);
    for (int i = 0; i < 2; ++i)
        engine.deck (i).onCuesChanged = nullptr;
    for (int i = 0; i < 2; ++i)
        engine.deck (i).onGridChanged = nullptr;
}

//==============================================================================
void MainComponent::frame()
{
    actions.tick();
    keyboard.tick();
    if (midi != nullptr) midi->tick();
    if (controllerLib != nullptr) controllerLib->tick();

    waveforms.refresh();
    deckA.refresh();
    deckB.refresh();
    mixer.refresh();
    libraryView.refresh();
    topBar.refresh();
    if (fxPanel.isVisible())
        fxPanel.refresh();

    // History: a track counts as played the first time it actually starts.
    for (int i = 0; i < 2; ++i)
    {
        auto& d = engine.deck (i);
        const auto t = d.getTrack();
        if (t.get() != trackSeen[(size_t) i])
        {
            trackSeen[(size_t) i] = t.get();
            playNoted[(size_t) i] = false;
        }
        if (t != nullptr && d.isPlaying() && ! playNoted[(size_t) i])
        {
            playNoted[(size_t) i] = true;
            library.notePlayed (t->file, i);
        }
    }
}

void MainComponent::updaterChanged()
{
    using S = Updater::State;
    const auto& info = updater.available();
    switch (updater.state())
    {
        case S::available:
        {
            juce::String msg = juce::String::fromUTF8 ("OpenMix ") + info.version + juce::String::fromUTF8 (" ya está disponible (tenés la ")
                             + Updater::currentVersion() + juce::String::fromUTF8 (").\n\nSe descarga, se verifica y se reemplaza sola: no hace falta reinstalar.");
            if (info.notes.isNotEmpty())
                msg << "\n\n" << info.notes.substring (0, 600) << (info.notes.length() > 600 ? juce::String::fromUTF8 ("…") : juce::String());
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::InfoIcon, juce::String::fromUTF8 ("Hay una nueva versión de OpenMix"), msg,
                                                "Actualizar ahora", juce::String::fromUTF8 ("Más tarde"), this,
                                                juce::ModalCallbackFunction::create ([this] (int r) { if (r == 1) updater.download(); }));
            break;
        }
        case S::downloading:
            if (updateProgress == nullptr)
            {
                updateProgress = std::make_unique<juce::AlertWindow> (juce::String::fromUTF8 ("Actualizando OpenMix"),
                                                                      juce::String::fromUTF8 ("Descargando y verificando la versión ") + info.version + juce::String::fromUTF8 ("…"),
                                                                      juce::MessageBoxIconType::NoIcon);
                updateProgress->addProgressBarComponent (updateProgressValue);
                updateProgress->enterModalState (false);
                juce::Timer::callAfterDelay (100, [safe = juce::Component::SafePointer<MainComponent> (this)]
                {
                    auto t = std::make_shared<std::function<void()>>();
                    *t = [safe, t]
                    {
                        if (safe == nullptr || safe->updater.state() != Updater::State::downloading) return;
                        safe->updateProgressValue = safe->updater.progress();
                        juce::Timer::callAfterDelay (100, *t);
                    };
                    (*t)();
                });
            }
            break;
        case S::ready:
            updateProgress = nullptr;
            if (settings != nullptr) saveSettings();
            updater.installAndRelaunch();                      // swaps the app as soon as OpenMix quits, then opens it
            break;
        case S::failed:
            updateProgress = nullptr;
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, juce::String::fromUTF8 ("No se pudo actualizar"),
                                                    updater.error() + juce::String::fromUTF8 ("\n\nPodés bajar la versión nueva desde la página de OpenMix."));
            break;
        case S::idle:
        case S::checking:
        case S::upToDate:
            break;
    }
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &updater) { updaterChanged(); return; }
    if (source == &Theme::get())
    {
        // Accent changed: everything repaints with the new token.
        repaint();
        for (auto* c : getChildren())
            c->repaint();
        return;
    }
    deckA.repaint();
    deckB.repaint();
}

//==============================================================================
void MainComponent::loadToDeck (const TrackPtr& t, int deck)
{
    if (t == nullptr)
        return;

    if (! t->file.existsAsFile())
    {
        (deck == 1 ? deckB : deckA).showMessage ("Archivo no encontrado", true);
        return;
    }

    // From the EXPLORADOR (a file of this computer not yet in the collection): add it, so its
    // analysis, cues and history are kept like any other track.
    if (! t->isExternal() && library.find (t->path()) == nullptr)
    {
        if (auto added = library.addFile (t->file))
        {
            loadToDeck (added, deck);
            return;
        }
    }

    if (deck < 0)
    {
        const bool aBusy = engine.deck (0).isPlaying(), bBusy = engine.deck (1).isPlaying();
        if (! aBusy && (bBusy || ! engine.deck (0).hasTrack())) deck = 0;
        else if (! bBusy) deck = 1;
        else
        {
            deckA.showMessage ("Ambos decks están reproduciendo"_u, false);
            deckB.showMessage ("Ambos decks están reproduciendo"_u, false);
            return;
        }
    }

    TrackMeta meta;
    meta.title = t->displayTitle();
    meta.artist = t->artist;
    meta.key = t->displayKey();
    meta.cuePoint = t->cuePoint;
    meta.hotCues = t->hotCues;
    meta.savedLoops = t->savedLoops;
    if (t->analysis == Track::Analysis::done && t->bpm > 0.0)
        meta.knownGrid = BeatGrid { t->bpm, t->firstBeat };

    if (t->isExternal())
    {
        // Exported USB: use its beat grid and cues as exported (read-only).
        const auto rb = DeviceMonitor::analysisFor (*t);
        if (rb.grid.isValid()) meta.knownGrid = rb.grid;
        if (rb.cuePoint >= 0.0) meta.cuePoint = rb.cuePoint;
        meta.hotCues = rb.hotCues;
        meta.savedLoops = rb.hotLoops;
        meta.key = t->displayKey();
    }

    // Cues saved with the drive by OpenMix take precedence over the exported ones.
    if (auto c = cueStore.get (t->file))
    {
        if (c->cuePoint >= 0.0) meta.cuePoint = c->cuePoint;
        meta.hotCues = c->hotCues;
        meta.savedLoops = c->savedLoops;
        if (c->bpm > 0.0) meta.knownGrid = BeatGrid { c->bpm, c->firstBeat };
    }

    auto& view = deck == 1 ? deckB : deckA;
    engine.loadTrack (deck, t->file, meta, [&view] (bool ok, const juce::String& err)
    {
        if (! ok) view.showMessage (err, true);
    });
}

void MainComponent::loadFileToDeck (const juce::File& f, int deck)
{
    // Dropped from Finder: add to the collection so tags, cues and history persist.
    if (auto t = library.addFile (f))
        loadToDeck (t, deck);
    else
        (deck == 1 ? deckB : deckA).showMessage ("Formato no soportado", true);
}

void MainComponent::addFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Agregar carpeta de música"_u,
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (dir.isDirectory())
            library.addFolder (dir);
    });
}

void MainComponent::toggleRecord()
{
    if (engine.recorder().isRecording())
    {
        const auto file = engine.recorder().getFile();
        engine.stopRecording();
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::NoIcon)
                                          .withTitle ("Grabación guardada"_u)
                                          .withMessage (file.getFullPathName())
                                          .withButton ("Mostrar en Finder")
                                          .withButton ("OK"),
                                      [file] (int r) { if (r == 1) file.revealToUser(); });
        return;
    }

    const auto err = engine.startRecording();
    if (err.isNotEmpty())
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("No se pudo grabar")
                                          .withMessage (err)
                                          .withButton ("OK"), nullptr);
}

//==============================================================================
void MainComponent::applyLayout()
{
    resized();
    repaint();
}

//==============================================================================
void MainComponent::loadSettings()
{
    if (controllerLib != nullptr)
        controllerLib->setOnlineLookup (settings->getBoolValue ("controllerOnlineLookup", true));

    if (auto accentHex = settings->getValue ("accent"); accentHex.isNotEmpty())
        Theme::get().setAccent (juce::Colour::fromString (accentHex));

    // Older versions stored a named workspace; only its sizes matter now.
    auto saved = juce::JSON::parse (settings->getValue ("layout"));
    if (! saved.isObject())
        saved = juce::JSON::parse (settings->getValue ("currentWorkspace"));
    current = saved.isObject() ? Workspace::fromVar (saved) : Workspace::defaults();
    applyLayout();

    libraryView.setColumnState (settings->getValue ("columns"));
    libraryView.setSidebarWidth (settings->getIntValue ("librarySidebar", om::Size::sidebarWidth));
    engine.crossfaderCurve = settings->getIntValue ("xfCurve", 0);
    for (int d = 0; d < AudioEngine::numDecks; ++d)
    {
        engine.deck (d).setVinylMode (settings->getBoolValue ("vinylMode" + juce::String (d + 1), settings->getBoolValue ("vinylMode", true)));
        engine.deck (d).setQuantize (settings->getBoolValue ("quantize" + juce::String (d + 1), true));
        engine.deck (d).setKeyLock (settings->getBoolValue ("keyLock" + juce::String (d + 1), true));
        engine.deck (d).setQuantizeBeats (settings->getDoubleValue ("quantizeBeats" + juce::String (d + 1), 1.0));
        engine.channel (d).colorType = juce::jlimit (0, (int) ChannelStrip::Color::numColors - 1, settings->getIntValue ("colorFx" + juce::String (d + 1), 0));
    }
    engine.limiter().enabled = settings->getBoolValue ("limiterOn", true);
    engine.outputMode = settings->getIntValue ("outputMode", om::AudioEngine::speakersPhones);
    engine.masterChannel = settings->getIntValue ("masterChannel", 0);
    engine.phonesChannel = settings->getIntValue ("phonesChannel", 2);
    engine.deckChannel[0] = settings->getIntValue ("deck1Channel", 0);
    engine.deckChannel[1] = settings->getIntValue ("deck2Channel", 2);
    engine.splitMono = settings->getBoolValue ("splitMono", false);
    engine.limiter().ceilingDb = (float) settings->getDoubleValue ("limiterCeiling", -0.5);
    engine.autoGain = settings->getBoolValue ("autoGain", true);
}

void MainComponent::saveSettings()
{
    if (settings == nullptr)
        return;

    settings->setValue ("accent", accent().toString());
    if (controllerLib != nullptr)
        settings->setValue ("controllerOnlineLookup", controllerLib->onlineLookup());
    settings->setValue ("updateCheck", settingsPanel.updateToggle.getToggleState());
    settings->setValue ("layout", juce::JSON::toString (current.toVar(), true));
    for (auto* oldKey : { "workspaces", "currentWorkspace", "currentIsCustom" })
        settings->removeValue (oldKey);
    settings->setValue ("columns", libraryView.getColumnState());
    settings->setValue ("librarySidebar", libraryView.getSidebarWidth());
    settings->setValue ("xfCurve", engine.crossfaderCurve.load());
    for (int d = 0; d < AudioEngine::numDecks; ++d)
    {
        settings->setValue ("vinylMode" + juce::String (d + 1), engine.deck (d).isVinylMode());
        settings->setValue ("quantize" + juce::String (d + 1), engine.deck (d).isQuantizeOn());
        settings->setValue ("keyLock" + juce::String (d + 1), engine.deck (d).isKeyLock());
        settings->setValue ("quantizeBeats" + juce::String (d + 1), engine.deck (d).getQuantizeBeats());
        settings->setValue ("colorFx" + juce::String (d + 1), engine.channel (d).colorType.load());
        for (int p = 0; p < RmxUnit::numDrums; ++p)
            settings->setValue ("rmxSample" + juce::String (d) + juce::String (p), engine.rmx (d).sampleName (p));
        settings->setValue ("rmxTone" + juce::String (d), engine.rmx (d).getTone());
        settings->setValue ("rmxRoll" + juce::String (d), engine.rmx (d).getBaseRoll());
        settings->setValue ("rmxVol" + juce::String (d), engine.rmx (d).drumLevel.load());
    }
    settings->setValue ("limiterOn", engine.limiter().enabled.load());
    settings->setValue ("outputMode", engine.outputMode.load());
    settings->setValue ("masterChannel", engine.masterChannel.load());
    settings->setValue ("phonesChannel", engine.phonesChannel.load());
    settings->setValue ("deck1Channel", engine.deckChannel[0].load());
    settings->setValue ("deck2Channel", engine.deckChannel[1].load());
    settings->setValue ("splitMono", engine.splitMono.load());
    settings->setValue ("limiterCeiling", engine.limiter().ceilingDb.load());
    settings->setValue ("autoGain", engine.autoGain.load());
    if (auto xml = engine.getDeviceManager().createStateXml())
        settings->setValue ("audioDevice", xml.get());
    settings->saveIfNeeded();
}

//==============================================================================
bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress ('f', juce::ModifierKeys::commandModifier, 0))
    {
        libraryView.focusSearch();
        return true;
    }
    if (k == juce::KeyPress (',', juce::ModifierKeys::commandModifier, 0))
    {
        settingsPanel.setBounds (getLocalBounds());
        settingsPanel.show (SettingsPanel::audio);
        return true;
    }
    return keyboard.keyPressed (k);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (colour (Col::bgApp));
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    settingsPanel.setBounds (r);

    topBar.setBounds (r.removeFromTop (Size::topBar));

    // Waveforms are the visual protagonists, but shrink before decks become unusable.
    const int fxH = 70;
    const int minLibH = 150;
    const int mixerW = juce::jlimit (268, 340, juce::roundToInt ((float) r.getWidth() * 0.22f));
    const int deckW = (r.getWidth() - mixerW) / 2;
    const bool wantsJog = deckW >= 440;      // small screens: compact decks, waveforms keep priority
    const int minDeckH = 330;
    const int spare = r.getHeight() - minDeckH - minLibH - fxH - 7;
    const int lane = juce::jlimit (34, juce::jmax (34, current.laneHeight), (spare - 1) / 2);
    waveforms.setLaneHeight (lane);
    waveforms.setBounds (r.removeFromTop (waveforms.preferredHeight()));
    waveSplitter->setBounds (r.removeFromTop (5));

    fxPanel.setBounds (r.removeFromTop (fxH));

    {
        int libH = juce::jlimit (minLibH, juce::jmax (minLibH, r.getHeight() - minDeckH),
                                 juce::roundToInt ((float) r.getHeight() * current.libraryRatio));
        if (! wantsJog)
            libH = juce::jmax (libH, r.getHeight() - 7 - minDeckH - 40);
        libraryView.setBounds (r.removeFromBottom (libH));
        splitter->setBounds (r.removeFromBottom (7));
    }

    // Decks + mixer
    deckA.setBounds (r.removeFromLeft (deckW));
    deckB.setBounds (r.removeFromRight (deckW));
    mixer.setBounds (r);

    // Jogs only when the deck can host one without crushing pads and info.
    const bool jogFits = wantsJog && deckA.getHeight() >= 330;
    for (auto* d : { &deckA, &deckB })
    {
        d->setDense (d->getHeight() < 360);
        d->setShowJog (jogFits);
    }
}

//==============================================================================
void MainComponent::prepareSnapshot (const juce::File& a, const juce::File& b, std::function<void()> done,
                                     const juce::String& devicePlaylist)
{
    if (devicePlaylist.isNotEmpty())
    {
        // Wait for the USB export to be parsed, then open the requested playlist.
        auto attempts = std::make_shared<int> (0);
        auto self = juce::Component::SafePointer<MainComponent> (this);
        std::function<void()> poll;
        auto pollPtr = std::make_shared<std::function<void()>>();
        *pollPtr = [self, attempts, devicePlaylist, a, b, done, pollPtr]
        {
            if (self == nullptr) return;
            if (self->devices.devices().empty() && ++(*attempts) < 50)
            {
                juce::Timer::callAfterDelay (100, *pollPtr);
                return;
            }
            if (! self->devices.devices().empty())
            {
                const auto& ex = *self->devices.devices()[0]->ex;
                int pid = -1;
                for (auto& p : ex.playlists)
                    if (p.name.containsIgnoreCase (devicePlaylist)) { pid = p.id; break; }
                self->libraryView.selectSource ({ LibraryView::Source::Kind::device, 0, {}, pid });
            }
            self->prepareSnapshot (a, b, done);
        };
        (*pollPtr)();
        return;
    }


    auto pending = std::make_shared<int> (0);
    auto finish = [this, pending, done]
    {
        if (--(*pending) > 0) return;
        for (int i = 0; i < 2; ++i)
            if (engine.deck (i).hasTrack())
                engine.deck (i).seek (juce::jmin (64.0, engine.deck (i).getLength() * 0.3));
        resized();
        frame();
        juce::Timer::callAfterDelay (250, [this, done] { frame(); done(); });
    };

    const juce::File files[2] = { a, b };
    for (int i = 0; i < 2; ++i)
        if (files[i].existsAsFile())
            ++(*pending);

    if (*pending == 0)
    {
        ++(*pending);
        finish();
        return;
    }

    for (int i = 0; i < 2; ++i)
        if (files[i].existsAsFile())
        {
            auto t = library.addFile (files[i]);
            TrackMeta meta;
            if (t != nullptr) { meta.title = t->displayTitle(); meta.artist = t->artist; meta.key = t->key; }
            engine.loadTrack (i, files[i], meta, [finish] (bool, const juce::String&) { finish(); });
        }
}
