#include "AutoMixPanel.h"

namespace om
{

AutoMixPanel::AutoMixPanel (AutoMix& a, AudioEngine& e, Hooks h) : automix (a), engine (e), hooks (std::move (h))
{
    closeBtn.setTooltip ("Cerrar AUTOMIX (sigue mezclando aunque esté cerrado)"_u);
    closeBtn.onClick = [this] { if (hooks.onClose) hooks.onClose(); };
    addAndMakeVisible (closeBtn);

    runBtn.setTextType (Type::label, Weight::bold);
    runBtn.setLitColour (colour (Col::play));
    runBtn.setTooltip ("Iniciar / detener la mezcla automática de la lista"_u);
    runBtn.onClick = [this] { automix.setRunning (! automix.isRunning()); };
    addAndMakeVisible (runBtn);

    mixNowBtn.setTextType (Type::caption, Weight::bold);
    mixNowBtn.setTooltip ("Pasar ya al próximo tema de la lista"_u);
    mixNowBtn.onClick = [this] { automix.mixNow(); };
    addAndMakeVisible (mixNowBtn);

    const char* modeTips[] = {
        "Mezcla en los últimos beats de cada tema",
        "Cada tema suena hasta la mitad y entra el siguiente, en una frase",
        "Mezcla larga y realista: entra sin graves, cambio de graves a la mitad y el que sale se filtra",
        "El tema que sale se llena de reverb, se corta en el compás y entra el nuevo",
        "El tema que sale se va con un eco al tempo, se corta en el compás y entra el nuevo",
        "Elige solo: mezcla larga si los tempos son parecidos; eco o reverb si son muy distintos" };
    for (int i = 0; i < (int) AutoMix::Mode::numModes; ++i)
    {
        auto& b = modeBtns[(size_t) i];
        b.setButtonText (AutoMix::modeName ((AutoMix::Mode) i));
        b.setStyle (Button::Style::segment);
        b.setTextType (Type::micro, Weight::bold);
        b.setTooltip (juce::String::fromUTF8 (modeTips[i]));
        b.onClick = [this, i] { automix.setMode ((AutoMix::Mode) i); };
        addAndMakeVisible (b);
    }
    for (int i = 0; i < 5; ++i)
    {
        auto& b = lengthBtns[(size_t) i];
        b.setStyle (Button::Style::segment);
        b.setTextType (Type::caption, Weight::bold);
        b.setTooltip ("Cada mezcla dura " + juce::String (lengths[i]) + " beats");
        b.onClick = [this, i] { automix.setMixBeats (lengths[i]); };
        addAndMakeVisible (b);
    }

    addSelBtn.setTooltip ("Agregar los temas seleccionados en la biblioteca"_u);
    addSelBtn.onClick = [this] { if (hooks.selected) automix.add (hooks.selected()); };
    addAllBtn.setTooltip ("Agregar todos los temas que muestra la biblioteca (carpeta, playlist o búsqueda)"_u);
    addAllBtn.onClick = [this] { if (hooks.visible) automix.add (hooks.visible()); };
    shuffleBtn.setTooltip ("Mezclar el orden de la lista"_u);
    shuffleBtn.onClick = [this] { automix.shuffle(); };
    clearBtn.setTooltip ("Vaciar la lista"_u);
    clearBtn.onClick = [this] { automix.clear(); };
    for (auto* b : { &addSelBtn, &addAllBtn, &shuffleBtn, &clearBtn })
    {
        b->setTextType (Type::caption, Weight::semibold);
        addAndMakeVisible (*b);
    }

    list.setRowHeight (34);
    list.setColour (juce::ListBox::backgroundColourId, colour (Col::bgField));
    list.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    list.setMultipleSelectionEnabled (true);
    addAndMakeVisible (list);

    automix.addChangeListener (this);
    updateButtons();
}

AutoMixPanel::~AutoMixPanel()
{
    automix.removeChangeListener (this);
}

void AutoMixPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    list.updateContent();
    list.repaint();
    updateButtons();
    repaint();
}

void AutoMixPanel::updateButtons()
{
    const bool on = automix.isRunning();
    runBtn.setButtonText (on ? "DETENER" : "INICIAR");
    runBtn.setIcon (on ? Icon::stop : Icon::play);
    runBtn.setToggleState (on, juce::dontSendNotification);
    mixNowBtn.setEnabled (on && ! automix.isMixing() && ! automix.queue().empty());
    for (int i = 0; i < 5; ++i)
        lengthBtns[(size_t) i].setToggleState (automix.mixBeats() == lengths[i], juce::dontSendNotification);
    for (int i = 0; i < (int) AutoMix::Mode::numModes; ++i)
        modeBtns[(size_t) i].setToggleState ((int) automix.mode() == i, juce::dontSendNotification);
}

void AutoMixPanel::refresh()
{
    if (list.getListBoxModel() != nullptr && shownRows != (int) automix.queue().size())
    {
        shownRows = (int) automix.queue().size();
        list.updateContent();
        list.repaint();
    }
    juce::String s;
    if (! automix.isRunning())
        s = automix.queue().empty() ? "Arrastrá temas acá desde la biblioteca"_u
                                    : juce::String (automix.queue().size()) + " temas en la lista";
    else if (automix.status().isNotEmpty())
        s = automix.status();
    else if (automix.isMixing())
        s = "Mezclando…"_u;
    else if (const double t = automix.secondsToMix(); t >= 0.0)
        s = AutoMix::modeName (automix.mode()) + juce::String::fromUTF8 (" · próxima mezcla en ") + juce::String ((int) t / 60) + ":" + juce::String ((int) t % 60).paddedLeft ('0', 2);
    if (s != lastStatus)
    {
        lastStatus = s;
        repaint (statusArea);
    }
    mixNowBtn.setEnabled (automix.isRunning() && ! automix.isMixing() && ! automix.queue().empty());
}

//==============================================================================
int AutoMixPanel::getNumRows() { return (int) automix.queue().size(); }

void AutoMixPanel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    const auto& q = automix.queue();
    if (! juce::isPositiveAndBelow (row, (int) q.size())) return;
    const auto& t = *q[(size_t) row];

    if (selected)
        g.fillAll (colour (Col::bgControlHover));
    g.setColour (colour (Col::line));
    g.fillRect (0, h - 1, w, 1);

    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (Space::md, 0);
    auto num = r.removeFromLeft (22);
    Text::drawTabular (g, juce::String (row + 1), num, font (Type::label, Weight::semibold),
                       row == 0 && automix.isRunning() ? colour (Col::play) : colour (Col::textTertiary));

    auto right = r.removeFromRight (64);
    const auto bpm = t.bpm > 0.0 ? juce::String (t.bpm, 1) : juce::String ("—"_u);
    const int secs = (int) std::round (t.duration);
    Text::drawTabular (g, bpm, right.removeFromTop (right.getHeight() / 2).withTrimmedTop (3), font (Type::label, Weight::semibold),
                       colour (Col::textSecondary), juce::Justification::bottomRight);
    Text::drawTabular (g, juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2), right.withTrimmedBottom (3),
                       font (Type::micro), colour (Col::textTertiary), juce::Justification::topRight);

    r.removeFromRight (Space::sm);
    Text::draw (g, t.displayTitle(), r.removeFromTop (r.getHeight() / 2).withTrimmedTop (3), font (Type::body, Weight::medium),
                colour (Col::textPrimary), juce::Justification::bottomLeft);
    Text::draw (g, row == 0 && automix.isRunning() ? "PRÓXIMO · "_u + t.artist : t.artist, r.withTrimmedBottom (3),
                font (Type::label), colour (Col::textTertiary), juce::Justification::topLeft);
}

juce::var AutoMixPanel::getDragSourceDescription (const juce::SparseSet<int>& rows)
{
    return rows.isEmpty() ? juce::var() : juce::var ("automix:" + juce::String (rows[0]));
}

void AutoMixPanel::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu()) return;
    juce::PopupMenu m;
    m.addItem ("Quitar de la lista", [this, row] { automix.remove (row); });
    m.addItem ("Pasar a próximo"_u, [this, row] { automix.move (row, 0); });
    m.addItem ("Mandar al final", [this, row] { automix.move (row, (int) automix.queue().size()); });
    m.showMenuAsync ({});
}

void AutoMixPanel::deleteKeyPressed (int)
{
    for (int i = list.getNumSelectedRows(); --i >= 0;)
        automix.remove (list.getSelectedRow (i));
    list.deselectAllRows();
}

//==============================================================================
bool AutoMixPanel::isInterestedInDragSource (const SourceDetails& d)
{
    const auto s = d.description.toString();
    return s.startsWith ("track:") || s.startsWith ("automix:");
}

int AutoMixPanel::insertIndexAt (juce::Point<int> p) const
{
    const auto local = list.getLocalPoint (this, p);
    const int row = list.getRowContainingPosition (local.x, local.y + list.getRowHeight() / 2);
    return row < 0 ? (int) automix.queue().size() : row;
}

void AutoMixPanel::itemDragMove (const SourceDetails& d)
{
    const int i = insertIndexAt (d.localPosition);
    if (i != dropIndex) { dropIndex = i; repaint(); }
}

void AutoMixPanel::itemDragExit (const SourceDetails&)
{
    dropIndex = -1;
    repaint();
}

void AutoMixPanel::itemDropped (const SourceDetails& d)
{
    const auto s = d.description.toString();
    const int at = insertIndexAt (d.localPosition);
    dropIndex = -1;
    if (s.startsWith ("automix:"))
        automix.move (s.fromFirstOccurrenceOf (":", false, false).getIntValue(), at);
    else if (hooks.trackFor)
    {
        std::vector<TrackPtr> tracks;
        for (auto& path : juce::StringArray::fromLines (s.fromFirstOccurrenceOf ("track:", false, false)))
            if (auto t = hooks.trackFor (path)) tracks.push_back (t);
        automix.insertAt (at, tracks);
    }
    repaint();
}

//==============================================================================
void AutoMixPanel::resized()
{
    auto r = getLocalBounds().reduced (Space::md);
    r.removeFromLeft (1);   // separator line
    headerArea = r.removeFromTop (24);
    closeBtn.setBounds (headerArea.removeFromRight (24));
    r.removeFromTop (Space::sm);

    auto row = r.removeFromTop (Size::controlLg);
    runBtn.setBounds (row.removeFromLeft (row.getWidth() / 2 - Space::xs / 2));
    row.removeFromLeft (Space::xs);
    mixNowBtn.setBounds (row);
    r.removeFromTop (Space::sm);

    {
        modeLabelArea = r.removeFromTop (14);
        const int n = (int) AutoMix::Mode::numModes, perRow = 3;
        for (int rw = 0; rw < 2; ++rw)
        {
            auto line = r.removeFromTop (Size::controlSm);
            r.removeFromTop (Space::xxs);
            const int mw = line.getWidth() / perRow;
            for (int k = 0; k < perRow && rw * perRow + k < n; ++k)
                modeBtns[(size_t) (rw * perRow + k)].setBounds (line.removeFromLeft (mw));
        }
        r.removeFromTop (Space::xs);
    }
    auto len = r.removeFromTop (Size::controlSm);
    lengthLabelArea = len.removeFromLeft (64);
    const int lw = len.getWidth() / 5;
    for (auto& b : lengthBtns) b.setBounds (len.removeFromLeft (lw));
    r.removeFromTop (Space::xs);
    statusArea = r.removeFromTop (20);
    r.removeFromTop (Space::xs);

    auto bottom = r.removeFromBottom (Size::controlSm);
    r.removeFromBottom (Space::sm);
    const int bw = (bottom.getWidth() - 3 * Space::xs) / 4;
    for (auto* b : { &addSelBtn, &addAllBtn, &shuffleBtn, &clearBtn })
    {
        b->setBounds (bottom.removeFromLeft (bw));
        bottom.removeFromLeft (Space::xs);
    }
    list.setBounds (r);
}

void AutoMixPanel::paint (juce::Graphics& g)
{
    g.fillAll (colour (Col::bgPanel));
    g.setColour (colour (Col::line));
    g.fillRect (0, 0, 1, getHeight());

    auto h = headerArea.toFloat();
    Text::draw (g, "AUTOMIX", h, font (Type::title, Weight::bold), colour (Col::textPrimary));
    Text::caption (g, "MODO", modeLabelArea.toFloat(), colour (Col::textTertiary), juce::Justification::centredLeft);
    Text::caption (g, "BEATS", lengthLabelArea.toFloat(), colour (Col::textTertiary), juce::Justification::centredLeft);
    Text::draw (g, lastStatus, statusArea.toFloat(), font (Type::label),
                automix.isRunning() ? colour (Col::textSecondary) : colour (Col::textTertiary));
    if (automix.queue().empty())
        Text::draw (g, "Arrastrá temas desde la biblioteca\no usá los botones de abajo"_u,
                    list.getBounds().toFloat(), font (Type::label), colour (Col::textDisabled), juce::Justification::centred, false);
}

void AutoMixPanel::paintOverChildren (juce::Graphics& g)
{
    if (dropIndex < 0) return;
    // Insertion line where the dragged tracks will go.
    const int rows = (int) automix.queue().size();
    const auto rowArea = dropIndex < rows ? list.getRowPosition (dropIndex, true) : list.getRowPosition (juce::jmax (0, rows - 1), true);
    const int y = list.getY() + (dropIndex < rows || rows == 0 ? rowArea.getY() : rowArea.getBottom());
    g.setColour (colour (Col::play));
    g.fillRect (list.getX(), juce::jlimit (list.getY(), list.getBottom() - 2, y - 1), list.getWidth(), 2);
}

} // namespace om
