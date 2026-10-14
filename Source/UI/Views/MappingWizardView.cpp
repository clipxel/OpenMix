#include "MappingWizardView.h"
#include "../../Common/Strings.h"

namespace om
{

static constexpr const char* communityUrl = "https://github.com/clipxel/OpenMix/discussions/new/choose";

MappingWizardView::MappingWizardView (MidiEngine& m, const Actions& a) : midi (m), actions (a)
{
    for (auto* b : { &skipBtn, &cancelBtn, &saveBtn, &shareBtn })
        addAndMakeVisible (b);
    saveBtn.useAccentWhenLit();
    saveBtn.setToggleState (true, juce::dontSendNotification);
    skipBtn.onClick = [this] { if (wizard) { wizard->skip(); repaint(); refreshButtons(); } };
    cancelBtn.onClick = [this] { close(); };
    saveBtn.onClick = [this] { save(); };
    shareBtn.onClick = [this] { share(); };
}

MappingWizardView::~MappingWizardView()
{
    midi.setCapture (nullptr);
}

void MappingWizardView::start()
{
    wizard = std::make_unique<MappingWizard> (actions, juce::String());
    device = {};
    flash = {};
    saved = false;
    midi.setCapture ([this] (const juce::String& dev, const juce::MidiMessage& msg) { handle (dev, msg); });
    startTimerHz (10);
    refreshButtons();
    repaint();
}

void MappingWizardView::handle (const juce::String& dev, const juce::MidiMessage& msg)
{
    if (wizard == nullptr || wizard->phase() == MappingWizard::Phase::finished)
        return;
    if (device.isEmpty())
    {
        device = dev;                              // the controller being mapped: the first one that answers
        wizard = std::make_unique<MappingWizard> (actions, device);
    }
    if (dev != device)
        return;

    const auto before = wizard->prompt();
    if (wizard->feed (msg, juce::Time::getMillisecondCounterHiRes() / 1000.0))
    {
        flash = juce::String::fromUTF8 ("✓ ") + before;
        flashAt = juce::Time::getMillisecondCounter();
    }
    refreshButtons();
    repaint();
}

void MappingWizardView::timerCallback()
{
    if (wizard == nullptr) return;
    const bool hidden = ! isVisible() || (getParentComponent() != nullptr && ! getParentComponent()->isVisible());
    if (hidden && ! saved)                     // settings closed or another section opened: give the controller back
    {
        close();
        return;
    }
    const auto before = wizard->prompt();
    wizard->poll (juce::Time::getMillisecondCounterHiRes() / 1000.0);   // ends fader / jog gestures
    if (wizard->prompt() != before)
    {
        flash = juce::String::fromUTF8 ("✓ ") + before;
        flashAt = juce::Time::getMillisecondCounter();
        refreshButtons();
    }
    repaint();
}

void MappingWizardView::refreshButtons()
{
    const bool finished = wizard != nullptr && wizard->phase() == MappingWizard::Phase::finished;
    skipBtn.setVisible (! finished);
    saveBtn.setVisible (finished && ! saved);
    shareBtn.setVisible (finished && saved);
    cancelBtn.setButtonText (finished && saved ? "Cerrar" : "Cancelar");
    resized();
}

void MappingWizardView::save()
{
    if (wizard == nullptr || device.isEmpty()) return;
    midi.installMapping (device, wizard->bindings());
    saved = true;
    flash = juce::String::fromUTF8 ("Mapeo guardado: el controlador ya funciona");
    flashAt = juce::Time::getMillisecondCounter();
    midi.setCapture (nullptr);
    refreshButtons();
    repaint();
}

void MappingWizardView::share()
{
    if (wizard == nullptr) return;
    // A file to attach + the community page: sharing is the user's choice, nothing is sent by itself.
    juce::Array<juce::var> arr;
    for (auto b : wizard->bindings()) { b.device = "*"; arr.add (b.toVar()); }
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty ("version", 1);
    root->setProperty ("controller", device);
    root->setProperty ("bindings", arr);
    const auto file = juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                        .getNonexistentChildFile ("OpenMix - " + juce::File::createLegalFileName (device), ".midi.json");
    file.replaceWithText (juce::JSON::toString (juce::var (root.release())));
    file.revealToUser();
    juce::URL (communityUrl).launchInDefaultBrowser();
    flash = juce::String::fromUTF8 ("Guardado en el Escritorio: adjuntalo en la página que se abrió");
    flashAt = juce::Time::getMillisecondCounter();
    repaint();
}

void MappingWizardView::close()
{
    stopTimer();
    midi.setCapture (nullptr);
    wizard.reset();
    if (onClosed) onClosed();
}

void MappingWizardView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (colour (Col::bgRaised));
    g.fillRoundedRectangle (r, Radius::lg);
    g.setColour (colour (Col::lineStrong));
    g.drawRoundedRectangle (r.reduced (0.5f), Radius::lg, Stroke::hair);
    if (wizard == nullptr) return;

    auto c = r.reduced (Space::xxl, Space::xl);
    Text::draw (g, juce::String::fromUTF8 ("ASISTENTE DE MAPEO") + (device.isNotEmpty() ? juce::String::fromUTF8 (" · ") + device : juce::String()),
                c.removeFromTop (16.0f), font (Type::caption, Weight::semibold), colour (Col::textTertiary));
    c.removeFromTop (Space::lg);

    const bool finished = wizard->phase() == MappingWizard::Phase::finished;
    const auto prompt = device.isEmpty() ? juce::String::fromUTF8 ("Apretá PLAY del deck izquierdo de tu controlador") : wizard->prompt();
    g.setColour (finished ? colour (Col::play) : colour (Col::textPrimary));
    g.setFont (font (Type::timer, Weight::semibold));
    g.drawFittedText (prompt, c.removeFromTop (48.0f).toNearestInt(), juce::Justification::centredLeft, 2);

    const bool recent = juce::Time::getMillisecondCounter() - flashAt < 1500;
    Text::draw (g, recent || finished ? flash : juce::String(), c.removeFromTop (20.0f), font (Type::label, Weight::medium), colour (Col::play));
    c.removeFromTop (Space::md);

    // Progress
    const int deck1 = (int) std::count_if (wizard->bindings().begin(), wizard->bindings().end(), [] (const MidiBinding& b) { return b.deck == 0; });
    const int deck2 = (int) std::count_if (wizard->bindings().begin(), wizard->bindings().end(), [] (const MidiBinding& b) { return b.deck == 1; });
    const int glob  = (int) wizard->bindings().size() - deck1 - deck2;
    juce::String progress;
    progress << deck1 << juce::String::fromUTF8 (" controles del deck izquierdo · ") << deck2
             << juce::String::fromUTF8 (" del derecho") << (deck2 > 0 && (wizard->deckChannelOffset() != 0 || wizard->deckNumberOffset() != 0) ? juce::String::fromUTF8 (" (deducidos)") : juce::String())
             << juce::String::fromUTF8 (" · ") << glob << " generales";
    Text::draw (g, progress, c.removeFromTop (18.0f), font (Type::label), colour (Col::textSecondary));
    if (! finished)
        Text::draw (g, juce::String::fromUTF8 ("Mientras el asistente está abierto, el controlador no dispara nada en los decks."),
                    c.removeFromTop (18.0f), font (Type::label), colour (Col::textTertiary));
}

void MappingWizardView::resized()
{
    auto r = getLocalBounds().reduced (Space::xxl, Space::xl);
    auto row = r.removeFromBottom (Size::controlMd);
    cancelBtn.setBounds (row.removeFromRight (110));
    row.removeFromRight (Space::md);
    if (skipBtn.isVisible()) skipBtn.setBounds (row.removeFromRight (180));
    if (saveBtn.isVisible()) saveBtn.setBounds (row.removeFromRight (160));
    if (shareBtn.isVisible()) shareBtn.setBounds (row.removeFromRight (220));
}

} // namespace om
