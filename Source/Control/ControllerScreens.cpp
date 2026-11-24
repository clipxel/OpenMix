#include "ControllerScreens.h"

namespace om
{

namespace
{
using Bytes = std::vector<juce::uint8>;

void put (Bytes& b, size_t at, const Bytes& v)                { for (size_t i = 0; i < v.size() && at + i < b.size(); ++i) b[at + i] = v[i]; }
void putU32 (Bytes& b, size_t at, juce::uint32 v)             { for (int i = 0; i < 4; ++i) b[at + (size_t) i] = (juce::uint8) (v >> (8 * i)); }
void putU16 (Bytes& b, size_t at, int v)                      { b[at] = (juce::uint8) (v & 0xFF); b[at + 1] = (juce::uint8) ((v >> 8) & 0xFF); }

/** Text in UTF-16 (little endian), at most `chars` characters, the rest zeros. */
void putText16 (Bytes& b, size_t at, const juce::String& s, int chars)
{
    int i = 0;
    for (auto c : s)
    {
        if (i >= chars - 1) break;
        const auto u = (juce::uint32) c > 0xFFFF ? (juce::uint32) '?' : (juce::uint32) c;
        b[at + (size_t) i * 2] = (juce::uint8) (u & 0xFF);
        b[at + (size_t) i * 2 + 1] = (juce::uint8) (u >> 8);
        ++i;
    }
}

void putAscii (Bytes& b, size_t at, const juce::String& s, int chars)
{
    const auto a = s.toRawUTF8();
    for (int i = 0; i < chars - 1 && a[i] != 0; ++i) b[at + (size_t) i] = (juce::uint8) a[i];
}

/** Camelot key colours, around the wheel as DJ software shows them. */
juce::Colour keyColour (const juce::String& camelot)
{
    const int n = camelot.getIntValue();
    if (n < 1 || n > 12) return juce::Colours::white;
    return juce::Colour::fromHSV ((float) ((n - 1) % 12) / 12.0f, camelot.endsWithIgnoreCase ("A") ? 0.75f : 0.55f, 1.0f, 1.0f);
}

Bytes rgba (juce::Colour c) { return { c.getRed(), c.getGreen(), c.getBlue(), 0xFF }; }

juce::String trackText (const juce::String& title, const juce::String& artist)
{
    return artist.isNotEmpty() ? artist + " - " + title : title;
}
} // namespace

ControllerScreens::ControllerScreens (AudioEngine& e, Browser b) : engine (e), browser (std::move (b))
{
    screens[0].port = "MCX8000 Display Left";  screens[0].id = 0x10; screens[0].side = 0;
    screens[1].port = "MCX8000 Display Right"; screens[1].id = 0x20; screens[1].side = 1;
}

std::vector<juce::uint8> ControllerScreens::pack7 (const std::vector<juce::uint8>& bytes)
{
    Bytes out;
    juce::uint32 acc = 0;
    int bits = 0;
    for (auto x : bytes)
    {
        acc |= (juce::uint32) x << bits;
        bits += 8;
        while (bits >= 7) { out.push_back ((juce::uint8) (acc & 0x7F)); acc >>= 7; bits -= 7; }
    }
    if (bits > 0) out.push_back ((juce::uint8) (acc & 0x7F));
    return out;
}

juce::MemoryBlock ControllerScreens::mcxMessage (int screenId, int command, const std::vector<juce::uint8>& data)
{
    const auto packed = pack7 (data);
    const int n = (int) packed.size();
    Bytes m { 0xF0, 0x47, (juce::uint8) screenId, 0x37, 0x05, (juce::uint8) command,
              (juce::uint8) (((n + 2) >> 7) & 0x7F), (juce::uint8) ((n + 2) & 0x7F),
              (juce::uint8) ((n >> 7) & 0x7F), (juce::uint8) (n & 0x7F) };
    m.insert (m.end(), packed.begin(), packed.end());
    m.push_back (0xF7);
    return { m.data(), m.size() };
}

void ControllerScreens::openPorts()
{
    const auto devices = juce::MidiOutput::getAvailableDevices();
    for (auto& s : screens)
    {
        const bool present = std::any_of (devices.begin(), devices.end(), [&] (const juce::MidiDeviceInfo& d) { return d.name == s.port; });
        if (! present) { s.out.reset(); continue; }
        if (s.out != nullptr) continue;
        for (auto& d : devices)
            if (d.name == s.port) { s.out = juce::MidiOutput::openDevice (d.identifier); break; }
        s.shownDeck = -1;                                   // a screen just plugged in gets everything again
        s.shownTrack = s.shownTempo = s.shownKey = s.shownBrowser = {};
    }
}

void ControllerScreens::send (Screen& s, int screenId, int command, const std::vector<juce::uint8>& data)
{
    if (s.out == nullptr) return;
    const auto m = mcxMessage (screenId, command, data);
    s.out->sendMessageNow (juce::MidiMessage (m.getData(), (int) m.getSize()));
}

void ControllerScreens::sendTrack (Screen& s, int deck)
{
    auto& k = engine.deck (deck);
    const auto t = k.getTrack();
    const auto title = t != nullptr ? trackText (t->meta.title.isNotEmpty() ? t->meta.title : t->file.getFileNameWithoutExtension(), t->meta.artist) : juce::String();
    const auto key = t != nullptr ? t->meta.key : juce::String();
    const auto bpm = k.getEffectiveBpm();
    const auto lengthMs = (juce::uint32) juce::jmax (0.0, k.getLength() * 1000.0);
    const auto sig = juce::String (deck) + "|" + (t != nullptr ? t->file.getFullPathName() : juce::String())
                   + "|" + title + "|" + key + "|" + juce::String (lengthMs);
    if (sig == s.shownTrack) return;
    s.shownTrack = sig;

    // 01: deck · title (96 characters) · BPM · key · colour · length in ms
    Bytes d (249, 0);
    d[0] = (juce::uint8) deck;
    putText16 (d, 1, title, 96);
    if (bpm > 0) putAscii (d, 193, juce::String (juce::roundToInt (bpm)), 6);
    putText16 (d, 199, key, 21);
    put (d, 241, rgba (keyColour (key)));
    putU32 (d, 245, lengthMs);
    send (s, s.id, 0x01, d);

    // 0C: deck · key · colour
    Bytes kd (16, 0);
    kd[0] = (juce::uint8) deck;
    putAscii (kd, 1, key, 4);
    put (kd, 12, rgba (keyColour (key)));
    send (s, s.id, 0x0C, kd);
    s.shownTempo = {};
}

void ControllerScreens::sendTempo (Screen& s, int deck, bool force)
{
    auto& k = engine.deck (deck);
    const auto bpm = k.getEffectiveBpm();
    const auto pitch = k.getPitch() * 100.0;
    const auto bpmText = bpm > 0 ? juce::String (bpm, 1) : juce::String ("0.0");
    const auto pitchText = (pitch >= 0 ? "+" : "") + juce::String (pitch, 1);
    const auto rangeText = juce::String (juce::roundToInt (k.getPitchRange() * 100.0));
    const auto sig = juce::String (deck) + bpmText + pitchText + rangeText;
    if (! force && sig == s.shownTempo) return;
    s.shownTempo = sig;

    // 04: deck · BPM · pitch % · pitch range %, as text
    Bytes d (19, 0);
    d[0] = (juce::uint8) deck;
    putAscii (d, 1, bpmText, 7);
    putAscii (d, 8, pitchText, 7);
    putAscii (d, 15, rangeText, 4);
    send (s, s.id, 0x04, d);
}

void ControllerScreens::sendClock (Screen& s, int deck)
{
    auto& k = engine.deck (deck);
    const double pos = k.hasTrack() ? k.getPosition() : 0.0;
    double phase = 0.0;
    if (const auto t = k.getTrack(); t != nullptr && t->grid().bpm > 0)
    {
        const double beats = (pos - t->grid().firstBeatSeconds) * t->grid().bpm / 60.0;
        phase = beats - std::floor (beats);
    }
    const double speed = k.isPlaying() ? 1.0 + k.getPitch() : 0.0;

    // 05: deck · time stamp (ms) · position (ms) · beat phase ×1000 · speed ×1000 (0 = stopped)
    Bytes d (15, 0);
    d[0] = (juce::uint8) deck;
    putU32 (d, 1, (juce::uint32) (juce::Time::getMillisecondCounterHiRes() - startMs));
    putU32 (d, 5, (juce::uint32) juce::jmax (0.0, pos * 1000.0));
    putU16 (d, 9, juce::roundToInt (phase * 1000.0));
    putU32 (d, 11, (juce::uint32) juce::roundToInt (juce::jmax (0.0, speed) * 1000.0));
    send (s, s.id, 0x05, d);
}

void ControllerScreens::sendBrowser (Screen& s, bool force)
{
    if (! browser.rows) return;
    const auto& rows = browser.rows();
    const int sel = browser.selected ? browser.selected() : -1;
    constexpr int shown = 7;                                   // rows on the screen
    const int first = juce::jlimit (0, juce::jmax (0, (int) rows.size() - shown), sel - shown / 2);
    juce::String sig (sel);
    for (int r = 0; r < shown && first + r < (int) rows.size(); ++r) sig << "|" << rows[(size_t) (first + r)]->path();
    if (! force && sig == s.shownBrowser) return;
    s.shownBrowser = sig;

    for (int r = 0; r < shown; ++r)
    {
        const int i = first + r;
        const auto t = i < (int) rows.size() ? rows[(size_t) i] : TrackPtr();

        // 24: row · title (128) · artist (40) · … · BPM · colour
        Bytes d (361, 0);
        d[0] = (juce::uint8) r;
        if (t != nullptr)
        {
            putText16 (d, 1, t->displayTitle(), 128);
            putText16 (d, 257, t->artist.isNotEmpty() ? t->artist : juce::String (" "), 40);
            if (t->bpm > 0) putAscii (d, 353, juce::String (juce::roundToInt (t->bpm)), 4);
        }
        put (d, 357, rgba (juce::Colours::white));
        send (s, s.id, 0x24, d);

        // 25: row · length (ms) · key · key colour
        Bytes x (41, 0);
        x[0] = (juce::uint8) r;
        if (t != nullptr)
        {
            putU32 (x, 1, (juce::uint32) juce::jmax (0.0, t->duration * 1000.0));
            putText16 (x, 5, t->displayKey(), 16);
            put (x, 37, rgba (keyColour (t->displayKey())));
        }
        send (s, s.id, 0x25, x);
    }

    // 23: selected row · rows on screen
    Bytes c (10, 0);
    c[0] = (juce::uint8) juce::jlimit (0, shown - 1, sel - first);
    c[6] = (juce::uint8) juce::jmin (shown, (int) rows.size());
    send (s, s.id, 0x23, c);
}

void ControllerScreens::tick()
{
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastScan > 2000) { lastScan = now; openPorts(); }

    for (auto& s : screens)
    {
        if (s.out == nullptr) continue;
        const int deck = engine.visibleDeck (s.side);
        if (deck != s.shownDeck) { s.shownDeck = deck; s.shownTrack = {}; }
        sendTrack (s, deck);
        sendTempo (s, deck, false);
        sendClock (s, deck);
        sendBrowser (s, false);
    }
}

} // namespace om
