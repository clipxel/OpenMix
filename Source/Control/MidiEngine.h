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

    /** MIDI Learn: the next control moved on any device is bound to this action. */
    void startLearn (const juce::String& action, int deck, bool shift, std::function<void (const MidiBinding&)> done);
    void cancelLearn();
    bool isLearning() const noexcept { return learning.has_value(); }

    /** Per-frame: LED feedback. */
    void tick();

    juce::String lastMessageText() const { return lastText; }
    juce::uint32 lastMessageTime() const { return lastTime; }

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
    std::map<juce::String, bool> ledSent;                  // device|ch|num → last state

    struct Learn { juce::String action; int deck; bool shift; std::function<void (const MidiBinding&)> done; };
    std::optional<Learn> learning;

    juce::String lastText;
    juce::uint32 lastTime = 0;
};

} // namespace om
