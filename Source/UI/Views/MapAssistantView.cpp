#include "MapAssistantView.h"
#include "../../Common/Strings.h"

namespace om
{

MapAssistantView::MapAssistantView (MidiEngine& m, Actions& a, KeyboardControl* k) : midi (m), actions (a), keyboard (k)
{
    input.setFont (font (Type::body));
    input.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Ej.: «que este botón borre el hot cue 3 del deck izquierdo con shift»"), colour (Col::textTertiary));
    input.onReturnKey = [this] { send(); };
    addAndMakeVisible (input);

    for (auto* b : { &sendBtn, &confirmBtn, &cancelBtn, &altBtns[0], &altBtns[1] })
        addAndMakeVisible (b);
    sendBtn.onClick = [this] { send(); };
    confirmBtn.useAccentWhenLit();
    confirmBtn.setToggleState (true, juce::dontSendNotification);
    confirmBtn.onClick = [this] { confirm(); };
    cancelBtn.onClick = [this] { if (waiting) { cancelWaiting(); say (juce::String::fromUTF8 ("Cancelado. Escribí otra cosa cuando quieras.")); } else close(); };
    for (size_t i = 0; i < altBtns.size(); ++i)
        altBtns[i].onClick = [this, i]
        {
            if (i >= alternatives.size() || ! pending) return;
            auto r = alternatives[i];
            std::vector<MapAssistant::Reading> rest { *pending };
            for (size_t j = 0; j < alternatives.size(); ++j) if (j != i) rest.push_back (alternatives[j]);
            if (r.deck == -2) r.deck = pending->deck;
            r.shift = pending->shift;
            propose (r, rest);
        };

    addAndMakeVisible (deckSel);
    addAndMakeVisible (layerSel);
    deckSel.onChange = [this] (int d) { if (pending) { pending->deck = d; repaint(); } };
    layerSel.onChange = [this] (int s) { if (pending) { pending->shift = s == 1; repaint(); } };
    setWantsKeyboardFocus (true);
}

MapAssistantView::~MapAssistantView()
{
    if (waiting) midi.cancelLearn();
}

void MapAssistantView::start()
{
    history.clear();
    pending.reset();
    alternatives.clear();
    waiting = false;
    say (juce::String::fromUTF8 ("Contame con tus palabras qué querés que haga el control (un botón, una perilla, un fader, el jog o una tecla de la compu). "
                                 "Podés decir el deck, el lado y si es con SHIFT."));
    refreshButtons();
    startTimerHz (5);
    input.grabKeyboardFocus();
}

juce::String MapAssistantView::describe (const MapAssistant::Reading& r) const
{
    juce::String s = juce::String::fromUTF8 ("«") + r.label + juce::String::fromUTF8 ("»");
    if (r.deck >= 0) s << juce::String::fromUTF8 (" · deck ") << (r.deck + 1) << (r.deck == 0 ? " (izquierdo)" : r.deck == 1 ? " (derecho)" : "");
    if (r.shift) s << " · con SHIFT";
    return s;
}

void MapAssistantView::send()
{
    auto text = input.getText().trim();
    if (text.isEmpty()) return;
    input.clear();
    history.push_back ({ true, text });
    if (waiting) cancelWaiting();

    auto readings = assistant.interpret (text, 3);

    // "lo mismo para el deck 2", "y el derecho igual": the last function, on another deck / layer.
    const auto n = MapAssistant::normalise (text);
    const bool sameAgain = lastAction.isNotEmpty()
                        && (n.contains ("mismo") || n.contains ("misma") || n.contains ("igual") || n.contains ("tambien") || n.contains ("same"));
    if (sameAgain && (readings.empty() || readings.front().score < 0.6f))
    {
        auto again = assistant.interpret (lastLabel + " " + text, 3);
        if (! again.empty()) readings = again;
    }

    if (readings.empty() || readings.front().score < 0.3f)
    {
        say (juce::String::fromUTF8 ("No te entendí bien. ¿Lo podés decir de otra forma? Por ejemplo: «el play del deck 1», "
                                     "«la perilla de graves del canal 2», «con shift borrar el hot cue 3»."));
        pending.reset();
        alternatives = readings;
        refreshButtons();
        return;
    }
    auto best = readings.front();
    readings.erase (readings.begin());
    propose (best, readings);
}

void MapAssistantView::propose (const MapAssistant::Reading& r, const std::vector<MapAssistant::Reading>& others)
{
    pending = r;
    const auto* info = actions.find (r.action);
    const bool perDeck = info != nullptr && info->perDeck;
    if (perDeck && pending->deck < 0) pending->deck = 0;      // asked below; the left deck until chosen
    alternatives.clear();
    for (auto& o : others) if (o.score >= 0.25f && alternatives.size() < altBtns.size()) alternatives.push_back (o);

    juce::String msg = juce::String::fromUTF8 ("Entendí: ") + describe (*pending) + ".";
    if (perDeck && r.deck < 0) msg << juce::String::fromUTF8 (" ¿Para qué deck? Elegilo abajo.");
    if (! alternatives.empty()) msg << juce::String::fromUTF8 (" Si no era eso, elegí otra opción.");
    say (msg);

    deckSel.setSelected (juce::jmax (0, pending->deck));
    layerSel.setSelected (pending->shift ? 1 : 0);
    refreshButtons();
}

void MapAssistantView::confirm()
{
    if (! pending) return;
    const auto r = *pending;
    waiting = true;
    say (juce::String::fromUTF8 ("Ahora apretá o mové el control de tu controlador para ") + describe (r)
         + juce::String::fromUTF8 (", o una tecla de la computadora (con la tecla Shift apretada queda en la capa SHIFT)."));
    const auto* info = actions.find (r.action);
    const int deck = info != nullptr && info->perDeck ? r.deck : -1;
    midi.startLearn (r.action, deck, r.shift, [this, r] (const MidiBinding& b)
    {
        waiting = false;
        if (onMapped) onMapped();
        finished (describe (r) + juce::String::fromUTF8 (" → ") + b.describeControl());
    });
    refreshButtons();
    grabKeyboardFocus();
}

bool MapAssistantView::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { if (waiting) { cancelWaiting(); say ("Cancelado."); } else close(); return true; }
    if (! waiting || ! pending || keyboard == nullptr) return false;
    if (k.getModifiers().isCommandDown() || k.getModifiers().isCtrlDown()) return false;

    int code = k.getKeyCode();
    if (code >= 'A' && code <= 'Z') code += 'a' - 'A';
    const bool shift = k.getModifiers().isShiftDown() || pending->shift;
    const auto r = *pending;
    const auto* info = actions.find (r.action);
    keyboard->assign (code, shift, r.action, info != nullptr && info->perDeck ? r.deck : -1);
    keyboard->save();
    midi.cancelLearn();
    waiting = false;
    if (onMapped) onMapped();
    finished (describe (r) + juce::String::fromUTF8 (" → tecla ") + (shift ? "SHIFT + " : "") + k.getTextDescription().toUpperCase());
    return true;
}

void MapAssistantView::finished (const juce::String& what)
{
    if (pending) { lastAction = pending->action; lastLabel = pending->label; }
    pending.reset();
    alternatives.clear();
    say (juce::String::fromUTF8 ("Listo: ") + what + juce::String::fromUTF8 (". ¿Qué más mapeamos?"));
    refreshButtons();
    input.grabKeyboardFocus();
}

void MapAssistantView::cancelWaiting()
{
    if (! waiting) return;
    midi.cancelLearn();
    waiting = false;
    refreshButtons();
}

void MapAssistantView::close()
{
    cancelWaiting();
    stopTimer();
    if (onClosed) onClosed();
}

void MapAssistantView::timerCallback()
{
    const bool hidden = ! isVisible() || (getParentComponent() != nullptr && ! getParentComponent()->isVisible());
    if (hidden && waiting) cancelWaiting();
}

void MapAssistantView::refreshButtons()
{
    const bool asking = pending.has_value() && ! waiting;
    const auto* info = pending ? actions.find (pending->action) : nullptr;
    confirmBtn.setVisible (asking);
    deckSel.setVisible (asking && info != nullptr && info->perDeck);
    layerSel.setVisible (asking);
    for (size_t i = 0; i < altBtns.size(); ++i)
    {
        const bool show = (asking || ! pending) && ! waiting && i < alternatives.size();
        altBtns[i].setVisible (show);
        if (show) altBtns[i].setButtonText (juce::String::fromUTF8 ("¿O era: ") + alternatives[i].label + "?");
    }
    cancelBtn.setButtonText (waiting ? "Cancelar" : "Cerrar");
    resized();
    repaint();
}

void MapAssistantView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (colour (Col::bgRaised));
    g.fillRoundedRectangle (r, Radius::lg);
    g.setColour (colour (Col::lineStrong));
    g.drawRoundedRectangle (r.reduced (0.5f), Radius::lg, Stroke::hair);

    auto top = r.reduced (Space::xl, Space::lg).removeFromTop (16.0f);
    Text::draw (g, juce::String::fromUTF8 ("MAPEAR ESCRIBIENDO · funciona sin internet"), top, font (Type::caption, Weight::semibold), colour (Col::textTertiary));

    // Chat: the newest messages, from the bottom up.
    auto c = chatArea.toFloat();
    g.saveState();
    g.reduceClipRegion (chatArea);
    float y = c.getBottom();
    const auto f = font (Type::body);
    for (auto it = history.rbegin(); it != history.rend() && y > c.getY(); ++it)
    {
        juce::AttributedString as;
        as.append (it->text, f, it->user ? colour (Col::textPrimary) : colour (Col::textSecondary));
        as.setWordWrap (juce::AttributedString::byWord);
        const float maxW = c.getWidth() * 0.78f;
        juce::TextLayout layout;
        layout.createLayout (as, maxW - 20.0f);
        const float h = layout.getHeight() + 12.0f, w = juce::jmin (maxW, layout.getWidth() + 20.0f);
        y -= h;
        const juce::Rectangle<float> bubble (it->user ? c.getRight() - w : c.getX(), y, w, h);
        g.setColour (it->user ? accent().withAlpha (0.22f) : colour (Col::bgControl));
        g.fillRoundedRectangle (bubble, Radius::md);
        layout.draw (g, bubble.reduced (10.0f, 6.0f));
        y -= Space::sm;
    }
    g.restoreState();

    if (waiting)
        Text::draw (g, juce::String::fromUTF8 ("Esperando el control o la tecla…"), proposalArea.toFloat().removeFromTop (18.0f),
                    font (Type::label, Weight::medium), accent());
}

void MapAssistantView::resized()
{
    auto r = getLocalBounds().reduced (Space::xl, Space::lg);
    r.removeFromTop (16 + Space::sm);

    auto inputRow = r.removeFromBottom (Size::controlMd);
    cancelBtn.setBounds (inputRow.removeFromRight (96));
    inputRow.removeFromRight (Space::sm);
    sendBtn.setBounds (inputRow.removeFromRight (90));
    inputRow.removeFromRight (Space::sm);
    input.setBounds (inputRow);
    r.removeFromBottom (Space::sm);

    // What was understood: "sí", deck, layer; other readings on the row above.
    proposalArea = r.removeFromBottom (Size::controlMd);
    auto p = proposalArea;
    if (confirmBtn.isVisible()) { confirmBtn.setBounds (p.removeFromLeft (140)); p.removeFromLeft (Space::md); }
    if (deckSel.isVisible()) { deckSel.setBounds (p.removeFromLeft (juce::jmin (380, p.getWidth() - 220))); p.removeFromLeft (Space::md); }
    if (layerSel.isVisible()) layerSel.setBounds (p.removeFromLeft (juce::jmin (210, p.getWidth())));
    if (altBtns[0].isVisible())
    {
        r.removeFromBottom (Space::sm);
        auto alt = r.removeFromBottom (Size::controlMd);
        for (auto& b : altBtns)
            if (b.isVisible()) { b.setBounds (alt.removeFromLeft (juce::jmin (320, alt.getWidth() / 2 - Space::sm))); alt.removeFromLeft (Space::sm); }
    }
    r.removeFromBottom (Space::sm);
    chatArea = r;
}

} // namespace om
