#include "SettingsPanel.h"
#include "../../Control/MappingImport.h"
#include "../../Engine/Recorder.h"

namespace om
{
/** Rows of the MIDI mapping table: control · action · deck · delete. */
class SettingsPanel::BindingList : public juce::ListBoxModel
{
public:
    BindingList (SettingsPanel& p) : panel (p) {}

    int getNumRows() override { return panel.midi != nullptr ? (int) panel.midi->bindings().size() : 0; }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (panel.midi == nullptr || ! juce::isPositiveAndBelow (row, getNumRows())) return;
        const auto& b = panel.midi->bindings()[(size_t) row];
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
        if (selected) { g.setColour (colour (Col::bgControl)); g.fillRect (r); }
        else if (row % 2) { g.setColour (colour (Col::bgRaised).withAlpha (0.5f)); g.fillRect (r); }
        r.reduce (8, 0);

        const auto* info = panel.actions != nullptr ? panel.actions->find (b.action) : nullptr;
        Text::draw (g, b.describeControl(), r.removeFromLeft (150), font (Type::label), colour (Col::textSecondary));
        Text::draw (g, info != nullptr ? info->label : b.action + " (desconocida)", r.removeFromLeft (210),
                    font (Type::body, Weight::medium), info != nullptr ? colour (Col::textPrimary) : colour (Col::error));
        Text::draw (g, b.deck >= 0 ? "Deck " + juce::String (b.deck + 1) : juce::String ("Global"), r.removeFromLeft (70),
                    font (Type::label), b.deck >= 0 ? Theme::get().deckColour (b.deck) : colour (Col::textTertiary));
        Text::draw (g, b.device == "*" ? juce::String ("Cualquier dispositivo") : b.device, r, font (Type::label), colour (Col::textTertiary));
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() || panel.midi == nullptr) return;
        juce::PopupMenu m;
        m.addItem ("Eliminar este mapeo", [this, row]
        {
            auto& v = panel.midi->bindings();
            if (juce::isPositiveAndBelow (row, (int) v.size()))
            {
                v.erase (v.begin() + row);
                panel.midi->save();
                panel.bindingBox.updateContent();
                panel.bindingBox.repaint();
            }
        });
        m.showMenuAsync ({});
    }

    void deleteKeyPressed (int row) override
    {
        if (panel.midi == nullptr) return;
        auto& v = panel.midi->bindings();
        if (juce::isPositiveAndBelow (row, (int) v.size()))
        {
            v.erase (v.begin() + row);
            panel.midi->save();
            panel.bindingBox.updateContent();
        }
    }

    SettingsPanel& panel;
};
}

namespace om
{

static const char* sectionNames[] = { "Audio", "Apariencia", "Interfaz", "Controladores", "Diagnóstico" };
static const Icon sectionIcons[] = { Icon::speaker, Icon::dot, Icon::layout, Icon::controller, Icon::cpu };

SettingsPanel::SettingsPanel (AudioEngine& e, Library& l) : engine (e), library (l)
{
    setWantsKeyboardFocus (true);

    for (int i = 0; i < numSections; ++i)
    {
        auto b = std::make_unique<Button> (juce::String::fromUTF8 (sectionNames[i]), sectionIcons[i], Button::Style::ghost);
        b->setTextType (Type::body, Weight::medium);
        b->onClick = [this, i] { setSection (i); };
        addAndMakeVisible (*b);
        navButtons[(size_t) i] = std::move (b);
    }

    addAndMakeVisible (closeBtn);
    closeBtn.setTooltip ("Cerrar (Esc)");
    closeBtn.onClick = [this] { close(); };

    audioView.onRoutingChanged = [this] { if (onAudioRoutingChanged) onAudioRoutingChanged(); };
    addChildComponent (audioView);

    // Appearance
    for (auto c : Theme::accentPresets())
    {
        auto b = std::make_unique<Button> (juce::String(), Icon::none, Button::Style::pad);
        b->setPadColour (c);
        b->onClick = [c] { Theme::get().setAccent (c); };
        b->setTooltip ("#" + c.toDisplayString (false));
        addChildComponent (*b);
        swatches.push_back (std::move (b));
    }
    hexField.setFont (font (Type::body));
    hexField.setInputRestrictions (7, "#0123456789abcdefABCDEF");
    hexField.setTextToShowWhenEmpty ("#RRGGBB", colour (Col::textTertiary));
    hexField.onReturnKey = [this]
    {
        auto t = hexField.getText().trimCharactersAtStart ("#");
        if (t.length() == 6)
            Theme::get().setAccent (juce::Colour ((juce::uint32) (0xff000000u | (juce::uint32) t.getHexValue32())));
    };
    addChildComponent (hexField);

    // Layout
    laneSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    laneSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    laneSlider.setRange (Workspace::minLane, Workspace::maxLane, 1);
    laneSlider.onValueChange = [this]
    {
        if (! currentWorkspace) return;
        currentWorkspace().laneHeight = (int) laneSlider.getValue();
        if (onWorkspaceEdited) onWorkspaceEdited();
    };
    addChildComponent (laneSlider);
    limCeiling.setCompact (true);
    limCeiling.onChange = [this] (int i) { engine.limiter().ceilingDb = i == 0 ? -0.1f : i == 1 ? -0.5f : -1.0f; };
    addChildComponent (limCeiling);
    autoGainToggle.onClick = [this] { engine.autoGain = autoGainToggle.getToggleState(); };
    addChildComponent (autoGainToggle);
    autoCueToggle.onClick = [this] { for (int d = 0; d < 2; ++d) engine.deck (d).setAutoCue (autoCueToggle.getToggleState()); };
    addChildComponent (autoCueToggle);

    resetSizes.onClick = [this]
    {
        if (onResetSizes) onResetSizes();
        syncLayoutControls();
    };
    addChildComponent (resetSizes);
    xfCurve.onChange = [this] (int c) { engine.crossfaderCurve = c; };
    addChildComponent (xfCurve);
}

SettingsPanel::~SettingsPanel()
{
    bindingBox.setModel (nullptr);
}

void SettingsPanel::setControllerLibrary (ControllerLibrary* lib)
{
    addChildComponent (updateToggle);
    updateToggle.onClick = [this] { if (onUpdateCheckChanged) onUpdateCheckChanged(); };
    controllerLib = lib;
    addChildComponent (onlineToggle);
    addChildComponent (driverBtn);
    driverBtn.onClick = [this] { if (driverPage.isNotEmpty()) juce::URL (driverPage).launchInDefaultBrowser(); };
    onlineToggle.onClick = [this]
    {
        if (controllerLib != nullptr) controllerLib->setOnlineLookup (onlineToggle.getToggleState());
        if (onControllerOnlineChanged) onControllerOnlineChanged();
    };
}

void SettingsPanel::importMapping (const juce::File& file, const juce::String& controllerHint)
{
    if (midi == nullptr) return;
    MappingImport::Result r;
    if (file != juce::File())
        r = MappingImport::fromFile (file);
    else
        for (auto& s : MappingImport::findInstalled())
            if (s.controller == controllerHint) { r = MappingImport::convert (s); break; }
    if (r.controller.isEmpty()) r.controller = controllerHint;

    if (r.bindings.empty())
    {
        learnStatus = juce::String::fromUTF8 ("No se pudo leer ningún control de ese archivo");
        repaint();
        return;
    }

    // Which connected controller gets it: the one with that model name, the only one connected,
    // or any (it will apply to whatever controller sends those messages).
    juce::String target = "*";
    const auto connected = midi->connectedInputs();
    for (auto& d : connected)
        if (ControllerLibrary::nameMatches (d, r.controller)) target = d;
    if (target == "*" && connected.size() == 1) target = connected[0];
    midi->installMapping (target, r.bindings);
    learnStatus = juce::String::fromUTF8 ("Importado: ") + juce::String ((int) r.bindings.size())
                + juce::String::fromUTF8 (" asignaciones") + (r.untranslated.isEmpty() ? juce::String()
                : juce::String::fromUTF8 (" · ") + juce::String (r.untranslated.size()) + juce::String::fromUTF8 (" funciones sin equivalente en OpenMix"));
    bindingBox.updateContent();
    repaint();
}

void SettingsPanel::setControllers (MidiEngine* m, Actions* a, KeyboardControl* k)
{
    midi = m;
    actions = a;
    keyboard = k;

    bindingList = std::make_unique<BindingList> (*this);
    bindingBox.setModel (bindingList.get());
    bindingBox.setRowHeight (24);
    bindingBox.setColour (juce::ListBox::backgroundColourId, colour (Col::bgField));
    addChildComponent (bindingBox);

    for (auto* c : std::initializer_list<juce::Component*> { &ctrlTab, &vinylToggle, &learnShift, &learnBtn, &cancelLearnBtn, &wizardBtn, &importBtn })
        addChildComponent (c);

    importBtn.onClick = [this]
    {
        // Mappings that DJ software on this computer already has, plus "choose a file".
        const auto found = MappingImport::findInstalled();
        juce::PopupMenu menu;
        menu.addSectionHeader (found.empty() ? juce::String::fromUTF8 ("No se encontraron mapeos de otros programas en esta computadora")
                                             : juce::String::fromUTF8 ("Encontrados en esta computadora"));
        for (size_t i = 0; i < found.size(); ++i)
            menu.addItem ((int) i + 1, found[i].controller);
        menu.addSeparator();
        menu.addItem (100000, juce::String::fromUTF8 ("Elegir archivo (.csv o .xml)…"));
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (importBtn), [this, found] (int r)
        {
            if (r == 100000)
            {
                importChooser = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Mapeo de otro programa"), juce::File(), "*.csv;*.xml");
                importChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                            [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) importMapping (fc.getResult(), {}); });
            }
            else if (juce::isPositiveAndBelow (r - 1, (int) found.size()))
            {
                const auto& s = found[(size_t) (r - 1)];
                auto res = MappingImport::convert (s);
                importMapping ({}, s.controller);
                juce::ignoreUnused (res);
            }
        });
    };

    if (midi != nullptr && actions != nullptr)
    {
        wizardView = std::make_unique<MappingWizardView> (*midi, *actions);
        addChildComponent (*wizardView);
        wizardView->onClosed = [this] { wizardView->setVisible (false); resized(); repaint(); };
        wizardBtn.useAccentWhenLit();
        wizardBtn.setToggleState (true, juce::dontSendNotification);
        wizardBtn.onClick = [this]
        {
            if (midi) midi->cancelLearn();
            wizardView->start();
            wizardView->setVisible (true);
            wizardView->toFront (false);
            resized();
        };
    }

    if (keyboard != nullptr && actions != nullptr)
    {
        keyMap = std::make_unique<KeyboardMapView> (*keyboard, *actions);
        addChildComponent (*keyMap);
    }
    ctrlTab.setCompact (true);
    ctrlTab.onChange = [this] (int) { setSection (controllers); };
    vinylToggle.onClick = [this]
    {
        for (int d = 0; d < AudioEngine::numDecks; ++d) engine.deck (d).setVinylMode (vinylToggle.getToggleState());
    };
    learnBtn.useAccentWhenLit();
    learnBtn.onClick = [this] { startLearnFlow(); };
    cancelLearnBtn.onClick = [this]
    {
        if (midi) midi->cancelLearn();
        learnStatus = {};
        learnBtn.setToggleState (false, juce::dontSendNotification);
        repaint();
    };
}

void SettingsPanel::startLearnFlow()
{
    if (midi == nullptr || actions == nullptr) return;

    // Pick an action (grouped by category), then a deck if it is per-deck.
    juce::PopupMenu menu;
    std::map<juce::String, juce::PopupMenu> cats;
    juce::StringArray order;
    for (auto& info : actions->list())
    {
        if (! order.contains (info.category)) order.add (info.category);
        auto& sub = cats[info.category];
        if (info.perDeck)
        {
            juce::PopupMenu decks;
            for (int d = 0; d < AudioEngine::numDecks; ++d)
                decks.addItem ("Deck " + juce::String (d + 1), [this, id = info.id, d, label = info.label]
                {
                    learnStatus = juce::String::fromUTF8 ("Mové o apretá el control para «") + label + " · Deck "_u + juce::String (d + 1) + juce::String::fromUTF8 ("»…");
                    learnBtn.setToggleState (true, juce::dontSendNotification);
                    midi->startLearn (id, d, learnShift.getToggleState(), [this] (const MidiBinding& b)
                    {
                        learnStatus = juce::String::fromUTF8 ("Asignado: ") + b.describeControl();
                        learnBtn.setToggleState (false, juce::dontSendNotification);
                        bindingBox.updateContent();
                        repaint();
                    });
                    repaint();
                });
            sub.addSubMenu (info.label, decks);
        }
        else
        {
            sub.addItem (info.label, [this, id = info.id, label = info.label]
            {
                learnStatus = juce::String::fromUTF8 ("Mové o apretá el control para «") + label + juce::String::fromUTF8 ("»…");
                learnBtn.setToggleState (true, juce::dontSendNotification);
                midi->startLearn (id, -1, learnShift.getToggleState(), [this] (const MidiBinding& b)
                {
                    learnStatus = juce::String::fromUTF8 ("Asignado: ") + b.describeControl();
                    learnBtn.setToggleState (false, juce::dontSendNotification);
                    bindingBox.updateContent();
                    repaint();
                });
                repaint();
            });
        }
    }
    for (auto& c : order)
        menu.addSubMenu (c, cats[c]);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&learnBtn));
}

void SettingsPanel::layoutControllers (juce::Rectangle<int> c)
{
    const bool midiTab = ctrlTab.getSelected() == 0;
    ctrlTab.setBounds (c.removeFromTop (24).withWidth (200));
    c.removeFromTop (Space::md);

    vinylToggle.setVisible (midiTab);
    onlineToggle.setVisible (midiTab && controllerLib != nullptr);
    {
        // A plugged-in controller whose driver is missing: one button to its official download page.
        driverPage = {};
        juce::String who;
        if (controllerLib != nullptr)
            for (auto& d : controllerLib->devices())
                if (d.driverPage.isNotEmpty()) { driverPage = d.driverPage; who = d.profileName.isNotEmpty() ? d.profileName : d.name; break; }
        driverBtn.setButtonText (juce::String::fromUTF8 ("Descargar el driver de ") + who + juce::String::fromUTF8 (" (página oficial)"));
        driverBtn.setVisible (midiTab && driverPage.isNotEmpty());
    }
    learnShift.setVisible (midiTab);
    learnBtn.setVisible (midiTab);
    wizardBtn.setVisible (midiTab && wizardView != nullptr);
    importBtn.setVisible (midiTab && midi != nullptr);
    if (wizardView != nullptr && wizardView->isVisible() && ! (midiTab && section == controllers))
        wizardView->setVisible (false);
    cancelLearnBtn.setVisible (midiTab && midi != nullptr && midi->isLearning());
    bindingBox.setVisible (midiTab);
    if (keyMap != nullptr) keyMap->setVisible (! midiTab && section == controllers);

    if (! midiTab)
    {
        ctrlListArea = c;
        if (keyMap != nullptr) keyMap->setBounds (c);
        return;
    }

    monitorArea = c.removeFromTop (44);
    vinylToggle.setBounds (c.removeFromTop (24).withWidth (360));
    if (controllerLib != nullptr)
        onlineToggle.setBounds (c.removeFromTop (24).withWidth (620));
    if (driverBtn.isVisible())
    {
        c.removeFromTop (Space::sm);
        driverBtn.setBounds (c.removeFromTop (Size::controlMd).withWidth (420));
    }
    c.removeFromTop (Space::sm);
    auto row = c.removeFromTop (Size::controlMd);
    learnBtn.setBounds (row.removeFromLeft (170));
    row.removeFromLeft (Space::md);
    if (wizardView != nullptr)
    {
        wizardBtn.setBounds (row.removeFromRight (190));
        row.removeFromRight (Space::md);
        importBtn.setBounds (row.removeFromRight (260));
        row.removeFromRight (Space::md);
    }
    learnShift.setBounds (row.removeFromLeft (120));
    row.removeFromLeft (Space::md);
    cancelLearnBtn.setBounds (row.removeFromLeft (90));
    c.removeFromTop (Space::xl + 2);
    ctrlListArea = c;
    bindingBox.setBounds (c);
    if (wizardView != nullptr && wizardView->isVisible())
        wizardView->setBounds (c.removeFromTop (juce::jmin (c.getHeight(), 230)));
}

void SettingsPanel::show (int s)
{
    setVisible (true);
    toFront (true);
    grabKeyboardFocus();
    setSection (s);
    startTimerHz (4);
}

void SettingsPanel::close()
{
    stopTimer();
    setVisible (false);
    if (onClosed) onClosed();
}

void SettingsPanel::setSection (int s)
{
    section = s;
    for (int i = 0; i < numSections; ++i)
        navButtons[(size_t) i]->setToggleState (i == s, juce::dontSendNotification);

    audioView.setVisible (s == audio);
    if (s == audio) audioView.refresh();
    for (auto& b : swatches) b->setVisible (s == appearance);
    hexField.setVisible (s == appearance);
    for (auto* c : std::initializer_list<juce::Component*> { &laneSlider, &resetSizes, &xfCurve, &limCeiling, &autoGainToggle, &autoCueToggle })
        c->setVisible (s == layout);
    for (auto* c : std::initializer_list<juce::Component*> { &ctrlTab, &vinylToggle, &learnShift, &learnBtn, &cancelLearnBtn, &bindingBox })
        c->setVisible (s == controllers);
    onlineToggle.setVisible (s == controllers && ctrlTab.getSelected() == 0 && controllerLib != nullptr);
    wizardBtn.setVisible (s == controllers && ctrlTab.getSelected() == 0 && wizardView != nullptr);
    importBtn.setVisible (s == controllers && ctrlTab.getSelected() == 0 && midi != nullptr);
    if (wizardView != nullptr && s != controllers && wizardView->isVisible()) { wizardView->setVisible (false); midi->setCapture (nullptr); }
    if (controllerLib != nullptr) onlineToggle.setToggleState (controllerLib->onlineLookup(), juce::dontSendNotification);
    if (keyMap != nullptr) keyMap->setVisible (s == controllers && ctrlTab.getSelected() == 1);
    vinylToggle.setToggleState (engine.deck (0).isVinylMode() && engine.deck (1).isVinylMode(), juce::dontSendNotification);
    bindingBox.updateContent();

    syncLayoutControls();
    resized();
    repaint();
}

void SettingsPanel::syncLayoutControls()
{
    if (! currentWorkspace) return;
    const auto& w = currentWorkspace();
    laneSlider.setValue (w.laneHeight, juce::dontSendNotification);
    xfCurve.setSelected (engine.crossfaderCurve.load());
    {
        const float ceil = engine.limiter().ceilingDb.load();
        limCeiling.setSelected (ceil > -0.3f ? 0 : ceil > -0.75f ? 1 : 2);
    }
    autoGainToggle.setToggleState (engine.autoGain.load(), juce::dontSendNotification);
    autoCueToggle.setToggleState (engine.deck (0).isAutoCue(), juce::dontSendNotification);
    hexField.setText ("#" + accent().toDisplayString (false), false);
}

void SettingsPanel::timerCallback()
{
    if (section == diagnostics || section == controllers)
        repaint (content);
    if (section == controllers)
    {
        cancelLearnBtn.setVisible (ctrlTab.getSelected() == 0 && midi != nullptr && midi->isLearning());
        // Controllers come and go (and may need a driver): re-layout when that changes.
        juce::String page;
        if (controllerLib != nullptr)
            for (auto& d : controllerLib->devices())
                if (d.driverPage.isNotEmpty()) { page = d.driverPage; break; }
        if (page != driverPage)
            resized();
    }
}

bool SettingsPanel::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { close(); return true; }
    return false;
}

void SettingsPanel::mouseDown (const juce::MouseEvent& e)
{
    if (! sheet.contains (e.getPosition()))
        close();
}

void SettingsPanel::resized()
{
    auto r = getLocalBounds();
    sheet = r.withSizeKeepingCentre (juce::jmin (1180, r.getWidth() - 40), juce::jmin (720, r.getHeight() - 40));

    auto s = sheet;
    auto header = s.removeFromTop (44);
    closeBtn.setBounds (header.removeFromRight (44).reduced (10));
    nav = s.removeFromLeft (180);
    auto n = nav.reduced (Space::md);
    for (auto& b : navButtons)
    {
        b->setBounds (n.removeFromTop (30));
        n.removeFromTop (2);
    }

    content = s.reduced (Space::xl, Space::md);
    auto c = content;

    if (section == audio)
        audioView.setBounds (c);

    if (section == appearance)
    {
        c.removeFromTop (28);
        auto row = c.removeFromTop (36);
        for (auto& b : swatches)
        {
            b->setBounds (row.removeFromLeft (36));
            row.removeFromLeft (Space::md);
        }
        c.removeFromTop (Space::xl + 20);
        hexField.setBounds (c.removeFromTop (Size::controlMd).withWidth (120));
    }

    if (section == controllers)
        layoutControllers (c);

    updateToggle.setVisible (section == diagnostics);
    if (section == diagnostics)
        updateToggle.setBounds (content.withTop (content.getBottom() - 30).withWidth (520));

    if (section == layout)
    {
        c.removeFromTop (28);
        laneSlider.setBounds (c.removeFromTop (24).withWidth (300));
        c.removeFromTop (Space::lg + 16);
        xfCurve.setBounds (c.removeFromTop (24).withWidth (300));
        c.removeFromTop (Space::lg + 16);
        limCeiling.setBounds (c.removeFromTop (24).withWidth (300));
        c.removeFromTop (Space::md);
        autoGainToggle.setBounds (c.removeFromTop (24).withWidth (460));
        autoCueToggle.setBounds (c.removeFromTop (24).withWidth (460));
        c.removeFromTop (Space::xl + 4);
        resetSizes.setBounds (c.removeFromTop (Size::controlMd + 2).withWidth (240));
    }
}

void SettingsPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.55f));

    auto s = sheet.toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillRoundedRectangle (s.translated (0, 4).expanded (2), Radius::lg + 2);
    g.setColour (colour (Col::bgPanel));
    g.fillRoundedRectangle (s, Radius::lg);
    g.setColour (colour (Col::lineStrong));
    g.drawRoundedRectangle (s.reduced (0.5f), Radius::lg, 1.0f);

    auto header = s.removeFromTop (44.0f);
    Text::draw (g, juce::String::fromUTF8 ("Configuración"), header.reduced (Space::xl, 0), font (Type::title, Weight::semibold),
                colour (Col::textPrimary));
    g.setColour (colour (Col::line));
    g.fillRect (header.removeFromBottom (1.0f));

    g.setColour (colour (Col::bgRaised));
    g.fillRect (nav.toFloat().withTrimmedBottom (Radius::lg));
    g.setColour (colour (Col::line));
    g.fillRect (nav.getRight(), nav.getY(), 1, nav.getHeight());

    auto c = content.toFloat();
    auto title = [&] (const juce::String& t) { drawSectionTitle (g, c.removeFromTop (20.0f), t); c.removeFromTop (8.0f); };
    auto line = [&] (const juce::String& k, const juce::String& v, juce::Colour vc = {})
    {
        auto row = c.removeFromTop (22.0f);
        Text::draw (g, k, row.removeFromLeft (200.0f), font (Type::body), colour (Col::textSecondary));
        Text::drawTabular (g, v, row, font (Type::body, Weight::medium), vc.isTransparent() ? colour (Col::textPrimary) : vc);
    };

    switch (section)
    {
        case appearance:
            title ("Color de acento");
            c.removeFromTop (44.0f);
            title ("Personalizado (hex, Enter para aplicar)");
            break;

        case layout:
            title ("Altura de las waveforms (también se arrastra la línea bajo las waveforms)");
            c.removeFromTop (24.0f + Space::lg);
            title ("Curva del crossfader");
            c.removeFromTop (24.0f + Space::lg);
            title ("Limitador master (true peak): techo");
            break;

        case controllers:
        {
            if (midi == nullptr && ctrlTab.getSelected() == 0)
            {
                Text::draw (g, "Control MIDI no disponible en este modo.", c.removeFromTop (22.0f), font (Type::body), colour (Col::textTertiary));
                break;
            }
            c.removeFromTop (24.0f + Space::md);

            if (ctrlTab.getSelected() == 1)
                break;                                   // the keyboard map draws itself

            // Monitor: devices + last message
            auto mon = monitorArea.toFloat();
            const auto devices = midi->connectedInputs();
            Icons::draw (g, Icon::controller, mon.removeFromLeft (16.0f).withHeight (20.0f).withSizeKeepingCentre (14, 14),
                         devices.isEmpty() ? colour (Col::textTertiary) : colour (Col::sync));
            mon.removeFromLeft (8.0f);
            juce::StringArray shown;
            if (controllerLib != nullptr)
            {
                using S = ControllerLibrary::Status;
                for (auto& d : controllerLib->devices())
                    shown.add (d.name + juce::String::fromUTF8 (" — ")
                               + (d.status == S::mapped       ? juce::String::fromUTF8 ("mapeo de la biblioteca (") + d.profileName + ")"
                                : d.status == S::mappedByUser ? juce::String ("mapeo propio")
                                : d.status == S::searching    ? juce::String::fromUTF8 ("buscando mapeo…")
                                : d.status == S::noMapping    ? juce::String::fromUTF8 ("sin mapeo en la biblioteca: usá Aprender control")
                                : d.status == S::needsDriver  ? juce::String::fromUTF8 ("conectado, falta su driver")
                                                              : juce::String::fromUTF8 ("no se pudo descargar el mapeo"))
                               + (d.driverPage.isNotEmpty() && d.status != S::needsDriver ? juce::String::fromUTF8 (" · falta su driver") : juce::String()));
            }
            else shown = devices;
            Text::draw (g, shown.isEmpty() ? juce::String ("No hay dispositivos MIDI conectados")
                                           : shown.joinIntoString ("  ·  "_u),
                        mon.removeFromTop (20.0f), font (Type::body, Weight::medium), colour (Col::textPrimary));
            const bool recent = juce::Time::getMillisecondCounter() - midi->lastMessageTime() < 1500;
            Text::draw (g, midi->lastMessageText().isNotEmpty() ? juce::String::fromUTF8 ("Último mensaje: ") + midi->lastMessageText()
                                                                : juce::String::fromUTF8 ("Esperando mensajes MIDI…"),
                        mon.removeFromTop (18.0f), font (Type::label), recent ? colour (Col::play) : colour (Col::textTertiary));

            if (learnStatus.isNotEmpty())
                Text::draw (g, learnStatus, learnBtn.getBounds().toFloat().withX ((float) cancelLearnBtn.getRight() + 12.0f)
                                                .withRight ((float) content.getRight()),
                            font (Type::label, Weight::medium), midi->isLearning() ? colour (Col::warning) : colour (Col::play));

            Text::draw (g, juce::String ((int) midi->bindings().size()) + juce::String::fromUTF8 (" mapeos · clic derecho o ⌫ para borrar"),
                        ctrlListArea.toFloat().withY ((float) ctrlListArea.getY() - 18.0f).withHeight (16.0f),
                        font (Type::label), colour (Col::textTertiary));
            break;
        }

        case diagnostics:
        {
            title ("OpenMix");
            line (juce::String::fromUTF8 ("Versión"), JUCE_APPLICATION_VERSION_STRING);
            c.removeFromTop (Space::lg);
            title ("Motor de audio");
            line ("Estado", engine.getStatusDetail());
            line ("Frecuencia de muestreo", juce::String (engine.getSampleRate(), 0) + " Hz");
            line ("Buffer", juce::String (engine.getBufferSize()) + " muestras");
            line ("Latencia de salida", juce::String (engine.getOutputLatencyMs(), 1) + " ms");
            line ("Carga DSP", juce::String ((int) std::round (engine.getCpuUsage() * 100)) + " %",
                  engine.getCpuUsage() > 0.7 ? colour (Col::warning) : juce::Colour());
            line ("Dropouts (xruns)", juce::String (juce::jmax (0, engine.getXRunCount())),
                  engine.getXRunCount() > 0 ? colour (Col::warning) : juce::Colour());
            line ("Salida de auriculares", engine.hasHeadphoneOutput() ? "Disponible (salidas 3/4)" : "No disponible (requiere 4 salidas)");
            c.removeFromTop (Space::lg);
            title ("Biblioteca");
            line ("Pistas", juce::String ((int) library.tracks().size()));
            line (juce::String::fromUTF8 ("Análisis en cola"), juce::String (library.pendingAnalysisCount()));
            line ("Base de datos", Library::dataFolder().getChildFile ("library.json").getFullPathName());
            line ("Grabaciones", Recorder::defaultFolder().getFullPathName());
            line (juce::String::fromUTF8 ("Tipografía UI"), Theme::get().fontFamily());
            break;
        }

        case audio:
        default:
            break;
    }
}

} // namespace om
