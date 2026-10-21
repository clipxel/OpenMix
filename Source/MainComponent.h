#pragma once

#include "Engine/AudioEngine.h"
#include "Library/Library.h"
#include "UI/Workspace.h"
#include "UI/Views/TopBar.h"
#include "UI/Views/WaveformViews.h"
#include "UI/Views/DeckView.h"
#include "UI/Views/MixerView.h"
#include "UI/Views/FxPanel.h"
#include "UI/Views/LibraryView.h"
#include "UI/Views/SettingsPanel.h"
#include "Control/Actions.h"
#include "Control/MidiEngine.h"
#include "Control/ControllerLibrary.h"
#include "Common/Updater.h"
#include "Control/KeyboardControl.h"
#include "Control/AutoMix.h"
#include "UI/Views/AutoMixPanel.h"
#include "UI/Views/FeedbackCard.h"
#include "Library/VolumeCueStore.h"

/** Main screen: top bar · stacked waveforms · [deck 1 | mixer | deck 2] · library.
    One fixed arrangement; only its sizes are adjustable (and persisted). Adapts to the window size. */
class MainComponent : public juce::Component,
                      public juce::DragAndDropContainer,
                      private juce::ChangeListener
{
public:
    explicit MainComponent (bool snapshotMode = false);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    /** Verification helper (--snapshot): load files, then call back. */
    /** Snapshot mode: advance both decks' pad pages . */
    /** Goodbye card now and then (2nd close, then every 8th) unless turned off. True if shown: `quit`
        runs when the user is done. */
    bool offerFeedbackBeforeQuit (std::function<void()> quit);
    void showFeedback (om::FeedbackCard::Kind, std::function<void()> after = {});
    void showFeedbackForSnapshot (bool goodbye)    { showFeedback (goodbye ? om::FeedbackCard::Kind::goodbye : om::FeedbackCard::Kind::report); }
    std::unique_ptr<juce::Component> gridEditorForSnapshot() { return deckA.makeGridEditor(); }
    void showAutoMixForSnapshot()                 { automixOpen = true; automix.add (libraryView.visibleTracks()); resized(); frame(); }
    void setFxModeForSnapshot (int m)             { setFxMode (m); }
    void showPadPageForSnapshot (int pages)       { for (int i = 0; i < pages; ++i) { deckA.nextPadPage(); deckB.nextPadPage(); } }
    /** Snapshot mode: open a settings section (and controllers tab) for the picture. */
    void showSettingsForSnapshot (int section, int tab)
    {
        if (section == om::SettingsPanel::controllers && tab == 0 && midi == nullptr)
            midi = std::make_unique<om::MidiEngine> (actions);       // the MIDI page needs the engine (no devices touched)
        settingsPanel.setControllers (midi.get(), &actions, &keyboard);
        settingsPanel.setBounds (getLocalBounds());
        settingsPanel.show (section);
        if (section == om::SettingsPanel::controllers) settingsPanel.showControllerTab (tab);
    }
    void openMappingWizardForSnapshot()          { settingsPanel.openMappingWizard(); }
    void prepareSnapshot (const juce::File& a, const juce::File& b, std::function<void()> done,
                          const juce::String& devicePlaylist = {});

private:
    class Splitter;

    void frame();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void controllersChanged();

    void loadToDeck (const om::TrackPtr&, int deck);
    void reanalyse (const std::vector<om::TrackPtr>&);
    void loadFileToDeck (const juce::File&, int deck);
    void addFolder();
    void toggleRecord();

    void applyLayout();
    void loadSettings();
    void saveSettings();

    const bool snapshotMode;
    std::unique_ptr<juce::PropertiesFile> settings;

    om::AudioEngine engine;
    om::Library library { engine.getFormatManager() };
    om::DeviceMonitor devices;
    om::VolumeCueStore cueStore;      // cues that travel with external drives (<drive>/.OpenMix)

    om::TopBar topBar { engine };
    om::WaveformStrip waveforms { engine };
    om::DeckView deckA { engine, 0 }, deckB { engine, 1 }, deckC { engine, 2 }, deckD { engine, 3 };
    om::DeckView& viewFor (int d)                 { return d == 1 ? deckB : d == 2 ? deckC : d == 3 ? deckD : deckA; }
    om::DeckView& sideView (int side)             { return viewFor (engine.visibleDeck (side)); }
    std::pair<int, int> shownDecks { 0, 1 };      // decks laid out on screen last time
    juce::StringArray noteShown;                  // one-time notices this session
    om::MixerView mixer { engine };
    om::FxPanel fxPanel { engine };          // 3 FX workspace: three effects per deck above the decks
    int fxMode = 0;                          // 0 BEAT FX · 1 3 FX
    void setFxMode (int);
    om::LibraryView libraryView { library, engine, devices };
    std::unique_ptr<Splitter> splitter, waveSplitter;
    om::Actions actions { engine, { [this] (int d) { loadToDeck (libraryView.selectedTrack(), d); },
                                    [this] (int delta) { libraryView.moveSelection (delta); },
                                    [this] { toggleRecord(); },
                                    [this] (int d, int pad, bool down) { viewFor (d).externalPad (pad, down); },
                                    [this] (int d) { viewFor (d).nextPadPage(); },
                                    [this] (int d, int pad) { return viewFor (d).padLed (pad); },
                                    [this] { automix.setRunning (! automix.isRunning()); },
                                    [this] { libraryView.browseFocusToggle(); },
                                    [this] (int d, int page, const juce::String& note)
                                    {
                                        if (page >= 0) viewFor (d).showPadPage (page);
                                        if (note.isNotEmpty()) viewFor (d).showMessage (note, false);
                                    } } };
    om::KeyboardControl keyboard { actions };
    om::AutoMix automix { engine, { [this] (const om::TrackPtr& t, int d) { loadToDeck (t, d); } } };
    om::AutoMixPanel automixPanel { automix, engine,
        { [this] { return libraryView.selectedTracks(); },
          [this] { return libraryView.visibleTracks(); },
          [this] (const juce::String& path) { auto t = library.find (path); return t != nullptr ? t : library.addFile (juce::File (path)); },
          [this] { automixOpen = false; resized(); } } };
    bool automixOpen = false;
    std::unique_ptr<om::FeedbackCard> feedback;
    std::unique_ptr<om::MidiEngine> midi;     // not created in snapshot mode (no device access)
    std::unique_ptr<om::ControllerLibrary> controllerLib;   // plug and play: finds and installs mappings
    om::Updater updater;
    void updaterChanged();
    std::unique_ptr<juce::AlertWindow> updateProgress;
    double updateProgressValue = 0.0;
    om::SettingsPanel settingsPanel { engine, library };
    juce::TooltipWindow tooltips { this, 600 };

    om::Workspace current;   // sizes of the single layout

    std::array<bool, om::AudioEngine::numDecks> playNoted {};
    std::array<const void*, om::AudioEngine::numDecks> trackSeen {};
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::VBlankAttachment> vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
