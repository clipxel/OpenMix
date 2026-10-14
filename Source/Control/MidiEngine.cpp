#include "MidiEngine.h"
#include "../Library/Library.h"
#include "../Common/Strings.h"

namespace om
{

//==============================================================================
static juce::String kindName (MidiBinding::Kind k)
{
    switch (k)
    {
        case MidiBinding::Kind::note:       return "note";
        case MidiBinding::Kind::cc:         return "cc";
        case MidiBinding::Kind::pitchWheel: return "pitchwheel";
    }
    return "note";
}

static juce::String encodingName (MidiBinding::Encoding e)
{
    switch (e)
    {
        case MidiBinding::Encoding::button:         return "button";
        case MidiBinding::Encoding::absolute7:      return "abs7";
        case MidiBinding::Encoding::absolute14:     return "abs14";
        case MidiBinding::Encoding::relativeOffset: return "rel_offset";
        case MidiBinding::Encoding::relativeTwos:   return "rel_twos";
        case MidiBinding::Encoding::relativeSign:   return "rel_sign";
    }
    return "button";
}

juce::var MidiBinding::toVar() const
{
    auto o = std::make_unique<juce::DynamicObject>();
    o->setProperty ("device", device);
    o->setProperty ("channel", channel);
    o->setProperty ("kind", kindName (kind));
    o->setProperty ("number", number);
    if (lsbNumber >= 0) o->setProperty ("lsb", lsbNumber);
    o->setProperty ("encoding", encodingName (encoding));
    o->setProperty ("action", action);
    o->setProperty ("deck", deck);
    o->setProperty ("shift", shift);
    if (std::abs (scale - 1.0f) > 1e-6f) o->setProperty ("scale", scale);
    if (invert) o->setProperty ("invert", true);
    if (! ledOut) o->setProperty ("led", false);
    if (outNumber >= 0 || outChannel > 0)
    {
        auto out = std::make_unique<juce::DynamicObject>();
        if (outChannel > 0) out->setProperty ("channel", outChannel);
        if (outNumber >= 0) { out->setProperty ("kind", kindName (outKind)); out->setProperty ("number", outNumber); }
        o->setProperty ("out", juce::var (out.release()));
    }
    if (onValue != 127) o->setProperty ("on", onValue);
    if (offValue != 0) o->setProperty ("off", offValue);
    if (dimValue >= 0) o->setProperty ("dim", dimValue);
    if (outputOnly) o->setProperty ("output_only", true);
    return juce::var (o.release());
}

MidiBinding MidiBinding::fromVar (const juce::var& v)
{
    MidiBinding b;
    b.device = v.getProperty ("device", "*").toString();
    b.channel = juce::jlimit (1, 16, (int) v.getProperty ("channel", 1));
    const auto k = v["kind"].toString();
    b.kind = k == "cc" ? Kind::cc : k == "pitchwheel" ? Kind::pitchWheel : Kind::note;
    b.number = (int) v["number"];
    b.lsbNumber = (int) v.getProperty ("lsb", -1);
    const auto e = v["encoding"].toString();
    b.encoding = e == "abs7" ? Encoding::absolute7 : e == "abs14" ? Encoding::absolute14
               : e == "rel_offset" ? Encoding::relativeOffset : e == "rel_twos" ? Encoding::relativeTwos
               : e == "rel_sign" ? Encoding::relativeSign : Encoding::button;
    b.action = v["action"].toString();
    b.deck = (int) v.getProperty ("deck", 0);
    b.shift = (bool) v.getProperty ("shift", false);
    b.scale = (float) (double) v.getProperty ("scale", 1.0);
    b.invert = (bool) v.getProperty ("invert", false);
    b.ledOut = (bool) v.getProperty ("led", true);
    if (auto out = v["out"]; out.isObject())
    {
        b.outChannel = juce::jlimit (0, 16, (int) out.getProperty ("channel", 0));
        if (out.hasProperty ("number"))
        {
            b.outKind = out["kind"].toString() == "cc" ? Kind::cc : Kind::note;
            b.outNumber = juce::jlimit (0, 127, (int) out["number"]);
        }
    }
    b.onValue = juce::jlimit (0, 127, (int) v.getProperty ("on", 127));
    b.offValue = juce::jlimit (0, 127, (int) v.getProperty ("off", 0));
    b.dimValue = juce::jlimit (-1, 127, (int) v.getProperty ("dim", -1));
    b.outputOnly = (bool) v.getProperty ("output_only", false) || Actions::isMeter (b.action);
    return b;
}

juce::String MidiBinding::describeControl() const
{
    juce::String s = "Ch" + juce::String (channel) + " ";
    switch (kind)
    {
        case Kind::note:       s << "Nota " << number; break;
        case Kind::cc:         s << "CC " << number; break;
        case Kind::pitchWheel: s << "Pitch wheel"; break;
    }
    if (shift) s << " + SHIFT";
    return s;
}

//==============================================================================
MidiEngine::MidiEngine (Actions& a) : actions (a)
{
    load();
    refreshDevices();
}

MidiEngine::~MidiEngine()
{
    allLightsOff();
    cancelPendingUpdate();
    for (auto& in : inputs) in->stop();
}

void MidiEngine::refreshDevices()
{
    for (auto& in : inputs) in->stop();
    inputs.clear();
    inputNames.clear();
    outputs.clear();

    for (auto& d : juce::MidiInput::getAvailableDevices())
        if (auto in = juce::MidiInput::openDevice (d.identifier, this))
        {
            inputNames.add (d.name);
            in->start();
            inputs.push_back (std::move (in));
        }

    for (auto& d : juce::MidiOutput::getAvailableDevices())
        if (auto out = juce::MidiOutput::openDevice (d.identifier))
            outputs.push_back (std::move (out));

    ledSent.clear();
}

juce::StringArray MidiEngine::connectedInputs() const { return inputNames; }

void MidiEngine::load()
{
    map.clear();
    const auto json = juce::JSON::parse (Library::dataFolder().getChildFile ("midi.json"));
    if (auto* arr = json["bindings"].getArray())
        for (auto& v : *arr)
            map.push_back (MidiBinding::fromVar (v));
}

void MidiEngine::save() const
{
    juce::Array<juce::var> arr;
    for (auto& b : map) arr.add (b.toVar());
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty ("version", 1);
    root->setProperty ("bindings", arr);
    Library::dataFolder().getChildFile ("midi.json").replaceWithText (juce::JSON::toString (juce::var (root.release())));
}

bool MidiEngine::hasBindingsFor (const juce::String& device) const
{
    return std::any_of (map.begin(), map.end(), [&device] (const MidiBinding& b) { return b.device == device; });
}

void MidiEngine::installMapping (const juce::String& device, const std::vector<MidiBinding>& mapping)
{
    map.erase (std::remove_if (map.begin(), map.end(), [&device] (const MidiBinding& b) { return b.device == device; }), map.end());
    for (auto b : mapping)
    {
        b.device = device;
        map.push_back (b);
    }
    takeover.clear();
    msbValues.clear();
    ledSent.clear();
    save();
}

//==============================================================================
void MidiEngine::handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m)
{
    if (m.isSysEx() || m.isActiveSense() || m.isMidiClock())
        return;

    int idx = -1;
    for (size_t i = 0; i < inputs.size(); ++i)
        if (inputs[i].get() == source) { idx = (int) i; break; }

    {
        const juce::ScopedLock sl (queueWriteLock);
        const auto scope = fifo.write (1);
        if (scope.blockSize1 > 0) queue[(size_t) scope.startIndex1] = { idx, m };
    }
    triggerAsyncUpdate();
}

void MidiEngine::handleAsyncUpdate()
{
    while (fifo.getNumReady() > 0)
    {
        Incoming in;
        {
            const auto scope = fifo.read (1);
            if (scope.blockSize1 <= 0) break;
            in = queue[(size_t) scope.startIndex1];
        }
        process (inputNames[in.deviceIndex], in.msg);
    }
}

void MidiEngine::inject (const juce::String& device, const juce::MidiMessage& m)
{
    process (device, m);
}

bool MidiEngine::matches (const MidiBinding& b, const juce::String& device, const juce::MidiMessage& m, bool lsb) const
{
    if (b.device != "*" && b.device != device) return false;
    if (b.outputOnly) return false;
    if (m.getChannel() != b.channel) return false;
    const int num = lsb ? b.lsbNumber : b.number;
    switch (b.kind)
    {
        case MidiBinding::Kind::note:       return (m.isNoteOn() || m.isNoteOff()) && m.getNoteNumber() == num;
        case MidiBinding::Kind::cc:         return m.isController() && m.getControllerNumber() == num;
        case MidiBinding::Kind::pitchWheel: return m.isPitchWheel() && ! lsb;
    }
    return false;
}

void MidiEngine::process (const juce::String& device, const juce::MidiMessage& m)
{
    lastText = device + "  ·  "_u + m.getDescription();
    lastTime = juce::Time::getMillisecondCounter();

    if (capture != nullptr)
    {
        capture (device, m);
        return;
    }

    if (learning.has_value())
    {
        const bool press = (m.isNoteOn() && m.getVelocity() > 0) || (m.isController() && m.getControllerValue() > 0) || m.isPitchWheel();
        if (! press) return;

        const auto* info = actions.find (learning->action);
        if (info == nullptr) { learning.reset(); return; }

        MidiBinding b;
        b.device = device;
        b.channel = m.getChannel();
        b.action = learning->action;
        b.deck = info->perDeck ? learning->deck : -1;
        b.shift = learning->shift;

        if (m.isPitchWheel())       { b.kind = MidiBinding::Kind::pitchWheel; b.encoding = MidiBinding::Encoding::absolute7; }
        else if (m.isNoteOn())      { b.kind = MidiBinding::Kind::note; b.number = m.getNoteNumber(); b.encoding = MidiBinding::Encoding::button; }
        else
        {
            b.kind = MidiBinding::Kind::cc;
            b.number = m.getControllerNumber();
            const int v = m.getControllerValue();
            if (info->type == ControlType::relative)
            {
                // 63/65 → offset-64 encoders; 1/127 → two's complement.
                b.encoding = (v > 32 && v < 96) ? MidiBinding::Encoding::relativeOffset : MidiBinding::Encoding::relativeTwos;
                if (info->id == "jog") b.scale = 1.0f / 512.0f;
            }
            else if (info->type == ControlType::absolute) b.encoding = MidiBinding::Encoding::absolute7;
            else                                          b.encoding = MidiBinding::Encoding::button;
        }

        // Replace whatever was on this control/layer before.
        map.erase (std::remove_if (map.begin(), map.end(), [&] (const MidiBinding& o)
        {
            return o.device == b.device && o.channel == b.channel && o.kind == b.kind && o.number == b.number && o.shift == b.shift;
        }), map.end());
        map.push_back (b);
        takeover.clear();
        save();

        auto done = std::move (learning->done);
        learning.reset();
        if (done) done (b);
        return;
    }

    // Shift buttons first.
    for (auto& b : map)
        if (b.action == "shift" && matches (b, device, m, false))
            shiftHeld[device] = m.isNoteOn() ? m.getVelocity() > 0 : (m.isController() && m.getControllerValue() >= 64);

    const bool shifted = shiftHeld[device];
    bool handled = false;

    for (size_t i = 0; i < map.size(); ++i)
    {
        const auto& b = map[i];
        if (b.action == "shift" || b.shift != shifted) continue;
        const bool isLsb = b.encoding == MidiBinding::Encoding::absolute14 && b.lsbNumber >= 0 && matches (b, device, m, true);
        if (isLsb || matches (b, device, m, false))
        {
            apply (b, i, m);
            handled = true;
        }
    }

    // Faders and jogs keep working while SHIFT is held unless a shift binding exists.
    if (! handled && shifted)
        for (size_t i = 0; i < map.size(); ++i)
        {
            const auto& b = map[i];
            if (b.shift || b.action == "shift" || b.encoding == MidiBinding::Encoding::button) continue;
            if (matches (b, device, m, false)) apply (b, i, m);
        }
}

void MidiEngine::apply (const MidiBinding& b, size_t index, const juce::MidiMessage& m)
{
    const auto* info = actions.find (b.action);
    if (info == nullptr)
        return;

    const int raw = m.isController() ? m.getControllerValue()
                  : m.isNoteOn() ? m.getVelocity()
                  : m.isPitchWheel() ? m.getPitchWheelValue() : 0;
    float value = 0.0f;

    switch (b.encoding)
    {
        case MidiBinding::Encoding::button:
            value = m.isNoteOff() ? 0.0f : (m.isNoteOn() ? (m.getVelocity() > 0 ? 1.0f : 0.0f) : (raw >= 64 ? 1.0f : 0.0f));
            break;
        case MidiBinding::Encoding::absolute7:
            value = m.isPitchWheel() ? (float) raw / 16383.0f : (float) raw / 127.0f;
            break;
        case MidiBinding::Encoding::absolute14:
            if (m.isController() && m.getControllerNumber() == b.lsbNumber)
                value = (float) (msbValues[index] * 128 + raw) / 16383.0f;
            else
            {
                msbValues[index] = raw;
                value = (float) (raw * 128) / 16383.0f;
            }
            break;
        case MidiBinding::Encoding::relativeOffset: value = (float) (raw - 64); break;
        case MidiBinding::Encoding::relativeTwos:   value = (float) (raw < 64 ? raw : raw - 128); break;
        case MidiBinding::Encoding::relativeSign:   value = (float) ((raw & 64) ? -(raw & 63) : (raw & 63)); break;
    }

    if (info->type == ControlType::relative) value *= b.scale;
    if (b.invert) value = info->type == ControlType::absolute ? 1.0f - value : -value;

    const int deck = info->perDeck ? b.deck : -1;

    // Soft takeover: a fader only grabs the parameter once it reaches its current value,
    // so the engine (and the on-screen control) never jumps.
    if (info->type == ControlType::absolute)
    {
        const float cur = actions.currentValue (b.action, deck);
        if (cur >= 0.0f)
        {
            auto& tk = takeover[index];
            if (tk.lastSent >= 0.0f && std::abs (cur - tk.lastSent) > 0.02f)
                tk.caught = false;                       // moved elsewhere (mouse, other control)
            if (! tk.caught)
            {
                if (std::abs (value - cur) > 0.04f)
                    return;
                tk.caught = true;
            }
            tk.lastSent = value;
        }
    }

    if (onDispatch) onDispatch (b, deck, value);
    actions.trigger (b.action, deck, value);
}

//==============================================================================
void MidiEngine::startLearn (const juce::String& action, int deck, bool shift, std::function<void (const MidiBinding&)> done)
{
    learning = Learn { action, deck, shift, std::move (done) };
}

void MidiEngine::cancelLearn() { learning.reset(); }

juce::MidiOutput* MidiEngine::outputFor (const juce::String& device)
{
    for (auto& o : outputs)
        if (o->getName() == device)
            return o.get();

    // Windows names the two halves of one controller "MIDIIN2 (X)" / "MIDIOUT2 (X)", and some
    // drivers add " In" / " Out": compare what is left.
    auto core = [] (juce::String n)
    {
        n = n.trim();
        for (auto* w : { "MIDIIN", "MIDIOUT" })
            if (n.startsWithIgnoreCase (w) && n.containsChar ('('))
                n = n.fromFirstOccurrenceOf ("(", false, false).upToLastOccurrenceOf (")", false, false);
        for (auto* w : { " In", " Out", " Input", " Output" })
            if (n.endsWithIgnoreCase (w))
                n = n.dropLastCharacters ((int) std::strlen (w));
        return n.trim().toLowerCase();
    };
    const auto want = core (device);
    for (auto& o : outputs)
        if (core (o->getName()) == want)
            return o.get();
    return nullptr;
}

void MidiEngine::tick()
{
    // Re-scan devices every 2 s so hot-plugged controllers just work.
    static juce::uint32 lastScan = 0;
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastScan > 2000)
    {
        lastScan = now;
        juce::StringArray names;
        for (auto& d : juce::MidiInput::getAvailableDevices()) names.add (d.name);
        if (names != inputNames)
            refreshDevices();
    }

    // One value per light per frame: the first binding that drives a light wins (a pad can be
    // bound to several actions; without this its light would flicker between them).
    std::map<juce::String, Light> frame;
    std::array<float, 3> meters { -1.0f, -1.0f, -1.0f };     // deck 1, deck 2, master: read once

    for (const auto& b : map)
    {
        if (! b.ledOut || b.action == "shift" || b.kind == MidiBinding::Kind::pitchWheel && b.outNumber < 0)
            continue;

        int value;
        if (Actions::isMeter (b.action))
        {
            const auto slot = (size_t) (b.action == "vu_master" ? 2 : juce::jlimit (0, 1, b.deck));
            if (meters[slot] < 0.0f)
                meters[slot] = actions.meterLevel (slot == 2 ? -1 : (int) slot);
            value = b.offValue + juce::roundToInt (meters[slot] * (float) (b.onValue - b.offValue));
        }
        else
        {
            if (b.encoding != MidiBinding::Encoding::button || b.shift != shiftHeld[b.device])
                continue;
            using L = Actions::Led;
            switch (actions.ledLevel (b.action, b.deck))
            {
                case L::on:    value = b.onValue; break;
                case L::dim:   value = b.dimValue >= 0 ? b.dimValue : b.offValue; break;
                case L::blink: value = actions.blinkPhase (b.deck) ? b.onValue : b.offValue; break;
                case L::off:
                default:       value = b.offValue; break;
            }
        }

        const auto key = b.device + "|" + juce::String (b.lightChannel()) + "|" + juce::String ((int) b.lightKind()) + "|" + juce::String (b.lightNumber());
        frame.emplace (key, Light { b.device, b.lightChannel(), b.lightKind(), b.lightNumber(), value, b.offValue });
    }

    for (auto& [key, light] : frame)
    {
        auto it = ledSent.find (key);
        if (it != ledSent.end() && it->second.value == light.value)
            continue;
        ledSent[key] = light;
        const auto v = (juce::uint8) juce::jlimit (0, 127, light.value);
        send (light.device, light.kind == MidiBinding::Kind::cc ? juce::MidiMessage::controllerEvent (light.channel, light.number, v)
                                                                : juce::MidiMessage::noteOn (light.channel, light.number, v));
    }
}

void MidiEngine::allLightsOff()
{
    for (auto& [key, light] : ledSent)
        if (light.value != light.off)
        {
            const auto v = (juce::uint8) juce::jlimit (0, 127, light.off);
            send (light.device, light.kind == MidiBinding::Kind::cc ? juce::MidiMessage::controllerEvent (light.channel, light.number, v)
                                                                    : juce::MidiMessage::noteOn (light.channel, light.number, v));
            light.value = light.off;
        }
}

void MidiEngine::send (const juce::String& device, const juce::MidiMessage& msg)
{
    if (onOutput)
        onOutput (device, msg);
    if (device == "*")
        for (auto& o : outputs) o->sendMessageNow (msg);
    else if (auto* o = outputFor (device))
        o->sendMessageNow (msg);
}

} // namespace om
