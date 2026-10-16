#pragma once

#include "../Components/Controls.h"
#include "../Contact.h"
#include "../../Common/Strings.h"
#include <BinaryData.h>

namespace om
{

/** "Ayudanos a mejorar" card: the user writes a message and sends it straight to us (feedback form),
    no account or chat app needed. Two uses:
      · goodbye  — now and then when OpenMix closes; any choice then closes the app
      · report   — from the support button / Settings; just closes the card */
class FeedbackCard : public juce::Component
{
public:
    enum class Kind { goodbye, report };

    explicit FeedbackCard (Kind k) : kind (k)
    {
        text.setMultiLine (true, true);
        text.setReturnKeyStartsNewLine (true);
        text.setFont (font (Type::body));
        text.setTextToShowWhenEmpty (kind == Kind::goodbye ? juce::String::fromUTF8 ("Qué te gustó, qué mejorarías, qué le falta…")
                                                           : juce::String::fromUTF8 ("Contanos qué pasó o qué te gustaría que tenga OpenMix…"),
                                     colour (Col::textTertiary));
        text.setColour (juce::TextEditor::backgroundColourId, colour (Col::bgField));
        text.setColour (juce::TextEditor::outlineColourId, colour (Col::lineStrong));
        text.setColour (juce::TextEditor::focusedOutlineColourId, accent());
        text.setColour (juce::TextEditor::textColourId, colour (Col::textPrimary));
        text.onTextChange = [this] { send.setEnabled (text.getText().trim().isNotEmpty() && ! sending); };
        addAndMakeVisible (text);

        send.setTextType (Type::body, Weight::bold);
        send.setLitColour (accent());
        send.setToggleState (true, juce::dontSendNotification);
        send.setEnabled (false);
        send.onClick = [this] { doSend(); };
        addAndMakeVisible (send);

        for (auto* b : { &community, &instagram })
        {
            b->setTextType (Type::label, Weight::semibold);
            addAndMakeVisible (*b);
        }
        community.onClick = [] { Contact::open (Contact::community()); };
        instagram.onClick = [] { Contact::open (Contact::instagramChat()); };

        close.setTextType (Type::label, Weight::semibold);
        close.onClick = [this] { finish(); };
        addAndMakeVisible (close);

        never.setButtonText (juce::String::fromUTF8 ("No volver a mostrar al cerrar"));
        never.setColour (juce::ToggleButton::textColourId, colour (Col::textTertiary));
        if (kind == Kind::goodbye) addAndMakeVisible (never);
        setWantsKeyboardFocus (true);
    }

    std::function<void (bool neverAgain)> onDone;

    void visibilityChanged() override { if (isVisible()) text.grabKeyboardFocus(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        auto c = card.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (c.translated (0, 6).expanded (3), Radius::lg + 4);
        g.setColour (colour (Col::bgPanel));
        g.fillRoundedRectangle (c, Radius::lg + 2);
        g.setColour (colour (Col::lineStrong));
        g.drawRoundedRectangle (c.reduced (0.5f), Radius::lg + 2, 1.0f);

        auto r = c.reduced (28.0f, 22.0f);
        static const auto logo = juce::ImageCache::getFromMemory (BinaryData::logomark_png, BinaryData::logomark_pngSize);
        if (logo.isValid())
        {
            const float h = 30.0f, w = h * (float) logo.getWidth() / (float) logo.getHeight();
            g.drawImage (logo, juce::Rectangle<float> (w, h).withCentre ({ r.getCentreX(), r.getY() + h * 0.5f }));
        }
        r.removeFromTop (42.0f);
        Text::draw (g, kind == Kind::goodbye ? juce::String::fromUTF8 ("¿Qué te pareció OpenMix?") : juce::String ("Ayudanos a mejorar"),
                    r.removeFromTop (26.0f), font (Type::deckTitle, Weight::bold), colour (Col::textPrimary), juce::Justification::centred);
        r.removeFromTop (4.0f);
        g.setFont (font (Type::body));
        g.setColour (colour (Col::textSecondary));
        g.drawFittedText (juce::String::fromUTF8 ("Tu mensaje nos llega directo y lo leemos todo.\nOpenMix mejora con lo que nos contás."),
                          r.removeFromTop (36.0f).toNearestInt(), juce::Justification::centredTop, 2, 1.0f);

        if (status.isNotEmpty())
            Text::draw (g, status, statusArea.toFloat(), font (Type::label, Weight::semibold),
                        statusOk ? colour (Col::play) : colour (Col::warning), juce::Justification::centredLeft);
        Text::draw (g, juce::String::fromUTF8 ("¿Preferís charlar? ") + Contact::handle() + juce::String::fromUTF8 (" en Instagram y WhatsApp"),
                    linksLabel.toFloat(), font (Type::label), colour (Col::textTertiary), juce::Justification::centred);
    }

    void resized() override
    {
        card = getLocalBounds().withSizeKeepingCentre (juce::jmin (480, getWidth() - 40), kind == Kind::goodbye ? 430 : 400);
        auto r = card.reduced (28, 22);
        r.removeFromTop (42 + 26 + 4 + 36 + 8);
        text.setBounds (r.removeFromTop (110));
        r.removeFromTop (Space::md);
        auto row = r.removeFromTop (Size::controlLg + 4);
        send.setBounds (row.removeFromRight (150));
        statusArea = row.withTrimmedRight (Space::md);
        r.removeFromTop (Space::lg);
        linksLabel = r.removeFromTop (18);
        r.removeFromTop (Space::xs);
        auto links = r.removeFromTop (Size::controlMd);
        const int lw = (links.getWidth() - Space::sm) / 2;
        community.setBounds (links.removeFromLeft (lw));
        links.removeFromLeft (Space::sm);
        instagram.setBounds (links);
        auto bottom = r.removeFromBottom (Size::controlMd);
        close.setBounds (bottom.removeFromRight (110));
        never.setBounds (bottom.removeFromLeft (260));
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { finish(); return true; }
        return false;
    }

private:
    void doSend()
    {
        const auto msg = text.getText().trim();
        if (msg.isEmpty() || sending) return;
        sending = true;
        send.setEnabled (false);
        status = "Enviando…"_u;
        statusOk = true;
        repaint();
        juce::Component::SafePointer<FeedbackCard> self (this);
        Contact::submitFeedback (msg, [self] (bool ok)
        {
            if (self == nullptr) return;
            self->sending = false;
            self->statusOk = ok;
            self->status = ok ? juce::String::fromUTF8 ("¡Gracias! Tu mensaje nos llegó.")
                              : juce::String::fromUTF8 ("No se pudo enviar (¿sin internet?). Probá de nuevo o escribinos por Instagram.");
            self->send.setEnabled (! ok);
            if (ok) self->text.setReadOnly (true);
            self->close.setButtonText (self->kind == Kind::goodbye ? "Salir" : "Listo");
            self->repaint();
            if (ok) juce::Timer::callAfterDelay (1400, [self] { if (self != nullptr) self->finish(); });
        });
    }

    void finish() { if (onDone) onDone (never.getToggleState()); }

    Kind kind;
    juce::TextEditor text;
    Button send { "Enviar" };
    Button community { "Comunidad (WhatsApp)" }, instagram { "Instagram" };
    Button close { kind == Kind::goodbye ? "Salir" : "Cerrar" };
    juce::ToggleButton never;
    juce::Rectangle<int> card, statusArea, linksLabel;
    juce::String status;
    bool statusOk = true, sending = false;
};

} // namespace om
