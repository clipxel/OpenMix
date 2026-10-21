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
    libraryView.onReanalyse = [this] (const std::vector<TrackPtr>& sel) { reanalyse (sel); };
    library.onAnalysed = [this] (const TrackPtr& t)
    {
        // A finished analysis reaches a deck that has this track loaded, right away.
        for (int d = 0; d < 2; ++d)
            if (auto lt = engine.deck (d).getTrack(); lt != nullptr && lt->file == t->file && t->bpm > 0.0)
                lt->setGrid ({ t->bpm, t->firstBeat });
    };
    libraryView.onAddToAutoMix = [this] (const std::vector<TrackPtr>& t) { automix.add (t); automixOpen = true; resized(); };
    topBar.onToggleAutoMix = [this] { automixOpen = ! automixOpen; resized(); };
    topBar.onFxModeChange = [this] (int m) { setFxMode (m); if (settings != nullptr) saveSettings(); };
    topBar.onOpenFeedback = [this] { showFeedback (FeedbackCard::Kind::report); };
    settingsPanel.onOpenFeedback = [this] { showFeedback (FeedbackCard::Kind::report); };
    addChildComponent (automixPanel);

    splitter->onDrag = [this] (int dy)
    {
        const float avail = (float) juce::jmax (1, getHeight() - topBar.getHeight() - waveforms.getHeight());
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

            // Tracks on external drives: cues also travel with the drive (<drive>/OpenMix/biblioteca.json).
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
        controllerLib->addChangeListener (this);
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

        resized();
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
    if (controllerLib != nullptr) controllerLib->removeChangeListener (this);
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
    // Development aid: with "devcapture" in the data folder, a "capture.request" file makes OpenMix
    // save an image of its own window (capture.png) — only this window, no screen permission needed.
    {
        static juce::uint32 lastCheck = 0;
        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastCheck > 400)
        {
            lastCheck = now;
            const auto dir = Library::dataFolder();
            const auto req = dir.getChildFile ("capture.request");
            if (req.existsAsFile() && dir.getChildFile ("devcapture").existsAsFile())
            {
                req.deleteFile();
                if (auto* top = getTopLevelComponent())
                {
                    const auto img = top->createComponentSnapshot (top->getLocalBounds(), true, 1.0f);
                    const auto out = dir.getChildFile ("capture.png");
                    out.deleteFile();
                    juce::FileOutputStream os (out);
                    if (os.openedOk()) juce::PNGImageFormat().writeImageToStream (img, os);
                }
            }
        }
    }

    actions.tick();
    keyboard.tick();
    if (midi != nullptr) midi->tick();
    if (controllerLib != nullptr) controllerLib->tick();
    automix.tick();
    topBar.setAutoMixState (automixOpen, automix.isRunning());
    if (automixPanel.isVisible()) automixPanel.refresh();

    waveforms.refresh();
    deckA.refresh();
    deckB.refresh();
    mixer.refresh();
    if (fxPanel.isVisible())
        fxPanel.refresh();
    libraryView.refresh();
    topBar.refresh();

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
    if (controllerLib != nullptr && source == controllerLib.get()) { controllersChanged(); return; }
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

/** A unit with its own mixer (all-in-one systems, club-style controllers) mixes the decks itself: OpenMix
    must send each deck to its own channel of the unit's sound card instead of a finished mix. Done once
    per unit, only when the current sound card has the outputs for it; the user can change it back. */
void MainComponent::controllersChanged()
{
    if (settings == nullptr) return;
    for (auto& d : controllerLib->devices())
    {
        if (d.status != ControllerLibrary::Status::mapped || ! ControllerLibrary::hasHardwareMixer (d.profileName))
            continue;
        const auto key = "hwMixer_" + ControllerLibrary::normalise (d.profileName);
        if (settings->getBoolValue (key) || engine.outputChannelCount() < 4)
            continue;
        settings->setValue (key, true);
        if (engine.outputMode.load() != AudioEngine::separateDecks)
        {
            engine.outputMode = AudioEngine::separateDecks;
            engine.splitMono = false;
            engine.deckChannel[0] = 0;
            engine.deckChannel[1] = 2;
            engine.phonesChannel = -1;
            saveSettings();
            deckA.showMessage (d.profileName + juce::String::fromUTF8 (" tiene mixer propio: cada deck sale por su canal"), false);
        }
    }
}

/** Effects workspace: BEAT FX (club mixer: one effect section at the mixer's side) or 3 FX per deck
    (a row of three effects above each deck). Controllers keep their own way either way. */
void MainComponent::setFxMode (int m)
{
    fxMode = juce::jlimit (0, 1, m);
    topBar.setFxMode (fxMode);
    mixer.setShowBeatFx (fxMode == 0);
    resized();
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
        if (auto added = library.addTrack (t))           // keeps an analysis already done in the EXPLORADOR
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
        const auto rb = DeviceMonitor::analysisFor (*t);    // cues only: BPM / grid are always OpenMix's
        if (rb.cuePoint >= 0.0) meta.cuePoint = rb.cuePoint;
        meta.hotCues = rb.hotCues;
        meta.savedLoops = rb.hotLoops;
        meta.key = t->detectedKey;
    }

    // Cues saved with the drive by OpenMix take precedence over the exported ones.
    if (auto c = cueStore.get (t->file))
    {
        if (c->cuePoint >= 0.0) meta.cuePoint = c->cuePoint;
        meta.hotCues = c->hotCues;
        meta.savedLoops = c->savedLoops;
        if (c->bpm > 0.0) { meta.knownGrid = BeatGrid { c->bpm, c->firstBeat }; meta.alignGridToKicks = false; }   // the DJ's own grid
        else if (c->analysedBpm > 0.0) { meta.knownGrid = BeatGrid { c->analysedBpm, c->analysedFirst }; meta.alignGridToKicks = false; }
    }

    auto& view = deck == 1 ? deckB : deckA;
    engine.loadTrack (deck, t->file, meta, [this, &view, deck, t] (bool ok, const juce::String& err)
    {
        if (! ok) { view.showMessage (err, true); return; }
        const auto lt = engine.deck (deck).getTrack();
        const auto g = engine.deck (deck).getGrid();
        const auto key = lt != nullptr ? lt->meta.key : juce::String();
        // A track analysed on load (exported USB): the list shows it right away.
        if (t->isExternal() && g.isValid())
        {
            // The drive's list may have been re-read meanwhile: update the row it shows now too.
            for (auto& x : { t, devices.findByPath (t->path()) })
                if (x != nullptr)
                {
                    x->bpm = g.bpm;
                    x->firstBeat = g.firstBeatSeconds;
                    if (key.isNotEmpty()) x->detectedKey = key;
                    x->analysis = Track::Analysis::done;
                }
            libraryView.repaint();
        }
        // Drive database: the analysis of every track played from an external drive stays on it.
        if (VolumeCueStore::externalVolumeFor (t->file) != juce::File() && g.isValid())
            if (const auto e = cueStore.noteAnalysis (t->file, g.bpm, g.firstBeatSeconds, key); e.isNotEmpty())
                view.showMessage (e, true);
    });
}

void MainComponent::reanalyse (const std::vector<TrackPtr>& tracks)
{
    for (auto& t : tracks)
    {
        if (! t->isExternal())
        {
            library.requestAnalysis (t, true);
            continue;
        }
        // Exported USB track: forget the analysis kept on the drive; analysed again now if it is
        // loaded, otherwise on its next load.
        t->bpm = 0.0;
        t->analysis = Track::Analysis::pending;
        if (auto c = cueStore.get (t->file))
        {
            c->bpm = 0.0; c->analysedBpm = 0.0;
            cueStore.put (t->file, *c);
        }
        for (int d = 0; d < 2; ++d)
            if (auto lt = engine.deck (d).getTrack(); lt != nullptr && lt->file == t->file)
            {
                juce::Component::SafePointer<MainComponent> self (this);
                std::thread ([self, lt]
                {
                    const auto g = Analysis::detectBeatGrid (lt->audio, lt->sampleRate);
                    juce::MessageManager::callAsync ([self, lt, g]
                    {
                        if (self == nullptr || ! g.isValid()) return;
                        lt->setGrid (g);
                        if (VolumeCueStore::externalVolumeFor (lt->file) != juce::File())
                            self->cueStore.noteAnalysis (lt->file, g.bpm, g.firstBeatSeconds, {});
                    });
                }).detach();
            }
    }
}

void MainComponent::showFeedback (FeedbackCard::Kind kind, std::function<void()> after)
{
    feedback = std::make_unique<FeedbackCard> (kind);
    feedback->onDone = [this, after, kind] (bool neverAgain)
    {
        if (kind == FeedbackCard::Kind::goodbye && settings != nullptr)
        {
            if (neverAgain) settings->setValue ("feedbackOnQuit", false);
            settings->saveIfNeeded();
        }
        juce::MessageManager::callAsync ([this, after]
        {
            feedback = nullptr;
            if (after) after();
        });
    };
    addAndMakeVisible (*feedback);
    feedback->setBounds (getLocalBounds());
    feedback->toFront (true);
}

bool MainComponent::offerFeedbackBeforeQuit (std::function<void()> quit)
{
    if (settings == nullptr || snapshotMode || feedback != nullptr) return false;
    const int closes = settings->getIntValue ("closeCount", 0) + 1;
    settings->setValue ("closeCount", closes);
    settings->saveIfNeeded();
    if (! settings->getBoolValue ("feedbackOnQuit", true) || ! (closes == 2 || (closes > 2 && closes % 8 == 0)))
        return false;
    showFeedback (FeedbackCard::Kind::goodbye, std::move (quit));
    return true;
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
    {
        // SOUND COLOR FX lit button (−1: none) and its PARAMETER; BEAT FX section.
        const int color = juce::jlimit (-1, (int) ChannelStrip::Color::numColors - 1, settings->getIntValue ("colorFxSelected", (int) ChannelStrip::Color::filter));
        for (int d = 0; d < AudioEngine::numDecks; ++d)
        {
            if (color >= 0) engine.channel (d).colorType = color;
            engine.channel (d).colorOn = color >= 0;
            engine.channel (d).colorParam = (float) settings->getDoubleValue ("colorFxParam", 0.5);
        }
        auto& bx = engine.beatFx();
        bx.select ((BeatFx::Type) juce::jlimit (0, (int) BeatFx::Type::numTypes - 1, settings->getIntValue ("beatFxType", (int) BeatFx::Type::echo)));
        bx.param = (float) settings->getDoubleValue ("beatFxBeat", bx.param.load());
        bx.level = (float) settings->getDoubleValue ("beatFxLevel", bx.level.load());
        bx.target = juce::jlimit (0, (int) AudioEngine::BeatFxSection::numTargets - 1, settings->getIntValue ("beatFxChannel", AudioEngine::BeatFxSection::master));
        for (int k = 0; k < 3; ++k) bx.bands[(size_t) k] = settings->getBoolValue ("beatFxBand" + juce::String (k), true);
    }
    setFxMode (settings->getIntValue ("fxMode", 0));
    engine.limiter().enabled = settings->getBoolValue ("limiterOn", true);
    engine.outputMode = settings->getIntValue ("outputMode", om::AudioEngine::speakersPhones);
    engine.masterChannel = settings->getIntValue ("masterChannel", 0);
    engine.phonesChannel = settings->getIntValue ("phonesChannel", 2);
    engine.deckChannel[0] = settings->getIntValue ("deck1Channel", 0);
    engine.deckChannel[1] = settings->getIntValue ("deck2Channel", 2);
    engine.splitMono = settings->getBoolValue ("splitMono", false);
    engine.limiter().ceilingDb = (float) settings->getDoubleValue ("limiterCeiling", -0.5);
    engine.autoGain = settings->getBoolValue ("autoGain", true);
    for (int d = 0; d < 2; ++d) engine.deck (d).setAutoCue (settings->getBoolValue ("autoCue", true));
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
    }
    settings->setValue ("colorFxSelected", engine.channel (0).colorOn.load() ? engine.channel (0).colorType.load() : -1);
    settings->setValue ("colorFxParam", engine.channel (0).colorParam.load());
    {
        const auto& bx = engine.beatFx();
        settings->setValue ("beatFxType", bx.type.load());
        settings->setValue ("beatFxBeat", bx.param.load());
        settings->setValue ("beatFxLevel", bx.level.load());
        settings->setValue ("beatFxChannel", bx.target.load());
        for (int k = 0; k < 3; ++k) settings->setValue ("beatFxBand" + juce::String (k), bx.bands[(size_t) k].load());
    }
    settings->setValue ("fxMode", fxMode);
    settings->setValue ("limiterOn", engine.limiter().enabled.load());
    settings->setValue ("outputMode", engine.outputMode.load());
    settings->setValue ("masterChannel", engine.masterChannel.load());
    settings->setValue ("phonesChannel", engine.phonesChannel.load());
    settings->setValue ("deck1Channel", engine.deckChannel[0].load());
    settings->setValue ("deck2Channel", engine.deckChannel[1].load());
    settings->setValue ("splitMono", engine.splitMono.load());
    settings->setValue ("limiterCeiling", engine.limiter().ceilingDb.load());
    settings->setValue ("autoGain", engine.autoGain.load());
    settings->setValue ("autoCue", engine.deck (0).isAutoCue());
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
    if (feedback != nullptr) feedback->setBounds (r);

    // Small screens (notebooks of 11–13", 1366×768, 1280×800): the library gives up some rows,
    // so decks and mixer keep every control usable.
    const bool compact = getHeight() < 780;

    topBar.setBounds (r.removeFromTop (Size::topBar));

    // Waveforms are the visual protagonists, but shrink before decks become unusable.
    const bool deckFxRow = fxMode == 1;
    fxPanel.setVisible (deckFxRow);
    const int fxH = deckFxRow ? (compact ? 60 : 70) : 0;
    const int minLibH = compact ? 110 : 150;
    const int mixerW = deckFxRow ? juce::jlimit (268, 340, juce::roundToInt ((float) r.getWidth() * 0.22f))
                                 : juce::jlimit (330, 420, juce::roundToInt ((float) r.getWidth() * 0.27f));   // club mixer: BEAT FX at its side
    const int deckW = (r.getWidth() - mixerW) / 2;
    const bool wantsJog = deckW >= 440;      // small screens: compact decks, waveforms keep priority
    const int minDeckH = wantsJog ? 400 : 330;    // with jogs: decks keep room for the platter before the library grows
    const int spare = r.getHeight() - minDeckH - minLibH - fxH - 7;
    const int lane = juce::jlimit (34, juce::jmax (34, current.laneHeight), (spare - 1) / 2);
    waveforms.setLaneHeight (lane);
    waveforms.setBounds (r.removeFromTop (waveforms.preferredHeight()));
    waveSplitter->setBounds (r.removeFromTop (5));
    if (deckFxRow)
        fxPanel.setBounds (r.removeFromTop (fxH));

    {
        int libH = juce::jlimit (minLibH, juce::jmax (minLibH, r.getHeight() - minDeckH),
                                 juce::roundToInt ((float) r.getHeight() * current.libraryRatio));
        if (! wantsJog)
            libH = juce::jmax (libH, r.getHeight() - 7 - minDeckH - 40);
        auto libArea = r.removeFromBottom (libH);
        automixPanel.setVisible (automixOpen);
        if (automixOpen)
            automixPanel.setBounds (libArea.removeFromRight (juce::jlimit (300, 400, libArea.getWidth() / 4)));
        libraryView.setBounds (libArea);
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
