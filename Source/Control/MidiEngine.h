#pragma once

#include "Actions.h"
#include <juce_audio_devices/juce_audio_devices.h>

namespace om
{

/** One physical control → one action. Matching is by device name (or "*"), channel,
    message kind and number; the shift layer selects between two bindings on the same control. */
struct MidiBinding
{
    enum class Kind { note, cc, pitchWheel };
    enum class Encoding
    {
        button,          // note on/off or CC ≥ 64 = pressed
        absolute7,       // 0…127
        absolute14,      // MSB on `number`, LSB on `lsbNumber`
        relativeOffset,  // 64 = no motion, 65… = +, 63… = −
        relativeTwos,    // 1… = +, 127… = −
        relativeSign     // bit 6 = sign
    };

    juce::String device = "*";
    int channel = 1;               // 1…16
    Kind kind = Kind::note;
    int number = 0;
    int lsbNumber = -1;
    Encoding encoding = Encoding::button;
    juce::String action;
    int deck = 0;                  // −1 for global actions
    bool shift = false;
    float scale = 1.0f;            // relative: units per tick (jog: revolutions per tick)
    bool invert = false;
    bool ledOut = true;            // echo LED state back to the device

    // Light / output message. By default the light answers on the same message as the control;
    // some controllers use another note or channel (or a light without a control, like VU meters).
    int outChannel = 0;            // 0 = same as `channel`
    Kind outKind = Kind::note;     // used when outNumber ≥ 0
    int outNumber = -1;            // −1 = same message as the control
    int onValue = 127, offValue = 0;
    int dimValue = -1;             // −1 = this light has no dim level (dim shows as off)
    bool outputOnly = false;       // a light with no control (VU meter, "track loaded", end warning)

    /** RGB pads: how the light's value encodes a colour. With a colour format, an action that has a
        colour (hot cues, loops…) is shown in that colour; dim = the same colour, darker. */
    enum class Rgb
    {
        none,
        hercules,      // 7-bit 0bRRGGGBB (Hercules DJControl Inpulse, …)
    };
    Rgb rgb = Rgb::none;
    static int encodeColour (Rgb, juce::Colour, float brightness);

    int lightChannel() const noexcept  { return outChannel > 0 ? outChannel : channel; }
    Kind lightKind() const noexcept    { return outNumber >= 0 ? outKind : kind; }
    int lightNumber() const noexcept   { return outNumber >= 0 ? outNumber : number; }

    juce::var toVar() const;
    static MidiBinding fromVar (const juce::var&);
    juce::String describeControl() const;
};

/** MIDI input/output for controllers: decoding (7/14-bit, relative encoders, jog),
    shift layers, soft takeover, LED feedback and MIDI Learn. Works with any MIDI
    controller; ready-made mappings are just binding lists. */
class MidiEngine : private juce::MidiInputCallback,
                   private juce::AsyncUpdater
{
public:
    explicit MidiEngine (Actions&);
    ~MidiEngine() override;

    void refreshDevices();
    juce::StringArray connectedInputs() const;

    std::vector<MidiBinding>& bindings() noexcept { return map; }
    void setBindings (std::vector<MidiBinding> b)   { map = std::move (b); takeover.clear(); }
    void load();
    void save() const;

    /** True if some binding targets this device by name (a mapping the user or the library made). */
    bool hasBindingsFor (const juce::String& device) const;
    /** Replaces every binding of `device` with `mapping` (its device field is rewritten) and saves. */
    void installMapping (const juce::String& device, const std::vector<MidiBinding>& mapping);

    /** Capture: every incoming message goes to this handler instead of triggering actions (mapping
        wizard). Pass nullptr to go back to normal. Message thread. */
    void setCapture (std::function<void (const juce::String& device, const juce::MidiMessage&)> handler) { capture = std::move (handler); }
    bool isCapturing() const noexcept { return capture != nullptr; }

    /** MIDI Learn: the next control moved on any device is bound to this action. */
    void startLearn (const juce::String& action, int deck, bool shift, std::function<void (const MidiBinding&)> done);
    void cancelLearn();
    bool isLearning() const noexcept { return learning.has_value(); }

    /** Per-frame: lights (on / dim / blinking in time / VU meters). The full state is sent again
        when a controller is connected, and every light is switched off when OpenMix closes. */
    void tick();
    void allLightsOff();

    /** Tests: every message sent to a controller (device name, message). */
    std::function<void (const juce::String&, const juce::MidiMessage&)> onOutput;

    juce::String lastMessageText() const { return lastText; }
    juce::uint32 lastMessageTime() const { return lastTime; }

    /** Tests and the controller simulator: called with every action a message triggers. */
    std::function<void (const MidiBinding&, int deck, float value)> onDispatch;

    /** Inject a message as if it came from `device` (tests, virtual controllers). */
    void inject (const juce::String& device, const juce::MidiMessage&);

private:
    struct Incoming { int deviceIndex; juce::MidiMessage msg; };

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void handleAsyncUpdate() override;
    void process (const juce::String& device, const juce::MidiMessage&);
    void apply (const MidiBinding&, size_t index, const juce::MidiMessage&);
    bool matches (const MidiBinding&, const juce::String& device, const juce::MidiMessage&, bool lsb) const;
    juce::MidiOutput* outputFor (const juce::String& device);
    void send (const juce::String& device, const juce::MidiMessage&);

    Actions& actions;
    std::vector<MidiBinding> map;

    std::vector<std::unique_ptr<juce::MidiInput>> inputs;
    std::vector<std::unique_ptr<juce::MidiOutput>> outputs;
    juce::StringArray inputNames;

    juce::AbstractFifo fifo { 1024 };
    std::array<Incoming, 1024> queue;
    juce::CriticalSection queueWriteLock;   // several inputs may call back concurrently

    std::map<juce::String, bool> shiftHeld;               // per device
    std::map<size_t, int> msbValues;                       // 14-bit: binding index → MSB
    struct Takeover { bool caught = false; float lastSent = -1.0f; };
    std::map<size_t, Takeover> takeover;
    struct Light { juce::String device; int channel; MidiBinding::Kind kind; int number; int value; int off; };
    std::map<juce::String, Light> ledSent;                 // device|ch|kind|num → last value sent

    std::function<void (const juce::String&, const juce::MidiMessage&)> capture;
    struct Learn { juce::String action; int deck; bool shift; std::function<void (const MidiBinding&)> done; };
    std::optional<Learn> learning;

    juce::String lastText;
    juce::uint32 lastTime = 0;
};

} // namespace om
