#include "AudioEngine.h"
#include "../Common/Strings.h"
#include "SincResampler.h"

namespace om
{

AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();

    for (int i = 0; i < numDecks; ++i)
        decks[(size_t) i] = std::make_unique<Deck> (i);

    for (auto& deckSlots : fxSlots)
    {
        for (auto [slot, t] : { std::pair { 0, BeatFx::Type::echo }, std::pair { 1, BeatFx::Type::reverb }, std::pair { 2, BeatFx::Type::filter } })
        {
            deckSlots[(size_t) slot].type = (int) t;
            deckSlots[(size_t) slot].param = BeatFx::defaultParam (t);
            deckSlots[(size_t) slot].wet = BeatFx::defaultLevel (t);
        }
    }

    deviceManager.addChangeListener (this);
    startTimerHz (5);
}

AudioEngine::~AudioEngine()
{
    *alive = false;
    stopTimer();
    rec.stop();
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
    loaderPool.removeAllJobs (true, 4000);
}

void AudioEngine::initialiseDevice (const juce::XmlElement* savedState)
{
    status = DeviceStatus::initialising;
    lastError = deviceManager.initialise (0, 8, savedState, true);     // every output pair available for routing
    enableAllOutputs();
    deviceManager.addAudioCallback (this);
    updateStatus();
}

void AudioEngine::enableAllOutputs()
{
    auto setup = deviceManager.getAudioDeviceSetup();
    if (auto* d = deviceManager.getCurrentAudioDevice())
    {
        const int n = d->getOutputChannelNames().size();
        if (setup.outputChannels.countNumberOfSetBits() < n)
        {
            setup.useDefaultOutputChannels = false;
            setup.outputChannels.clear();
            setup.outputChannels.setRange (0, n, true);
            deviceManager.setAudioDeviceSetup (setup, true);
        }
    }
}

double AudioEngine::getOutputLatencyMs() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice())
        return 1000.0 * (d->getOutputLatencyInSamples() + d->getCurrentBufferSizeSamples()) / juce::jmax (1.0, d->getCurrentSampleRate());
    return 0.0;
}

//==============================================================================
void AudioEngine::loadTrack (int deckIndex, const juce::File& file, TrackMeta meta, LoadCallback cb)
{
    auto& d = deck (deckIndex);
    if (d.isPlaying())
    {
        if (cb) cb (false, "El deck " + juce::String::charToString ((juce::juce_wchar) ('A' + deckIndex)) + " está reproduciendo"_u);
        return;
    }

    const int generation = ++loadGeneration[(size_t) deckIndex];
    loading[(size_t) deckIndex] = true;
    sendChangeMessage();

    auto aliveFlag = alive;
    loaderPool.addJob ([this, aliveFlag, deckIndex, generation, file, meta, cb]
    {
        auto result = std::make_shared<LoadedTrack>();
        juce::String error;

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr)
            error = "Formato no soportado o archivo ilegible";
        else if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0)
            error = "El archivo no contiene audio";
        else if ((double) reader->lengthInSamples / reader->sampleRate > 60.0 * 45.0)
            error = "Pista demasiado larga (máx. 45 min)"_u;
        else
        {
            const int len = (int) reader->lengthInSamples;
            const int lead = Analysis::mp3TrimmedLeadSamples (file);   // same timeline as club players
            result->file = file;
            result->meta = meta;
            result->sampleRate = reader->sampleRate;
            const int pad = juce::jmax (0, lead), skip = juce::jmin (len, juce::jmax (0, -lead));
            result->audio.setSize (2, len - skip + pad);
            result->audio.clear();
            reader->read (&result->audio, pad, len - skip, skip, true, reader->numChannels > 1);
            if (reader->numChannels == 1)
                result->audio.copyFrom (1, 0, result->audio, 0, 0, len - skip + pad);

            result->wave = Analysis::computeWaveform (result->audio, result->sampleRate);
            result->loudnessLufs = Analysis::integratedLoudness (result->audio, result->sampleRate);
            result->analysedGrid = ! meta.knownGrid.isValid() ? Analysis::detectBeatGrid (result->audio, result->sampleRate)
                                 : meta.alignGridToKicks ? Analysis::alignGridToKicks (result->audio, result->sampleRate, meta.knownGrid)
                                                         : meta.knownGrid;
            result->setGrid (result->analysedGrid);
        }

        juce::MessageManager::callAsync ([this, aliveFlag, deckIndex, generation, result, error, cb]
        {
            if (! *aliveFlag || generation != loadGeneration[(size_t) deckIndex])
                return;

            loading[(size_t) deckIndex] = false;

            if (error.isEmpty() && deck (deckIndex).isPlaying())
            {
                if (cb) cb (false, "El deck empezó a reproducir durante la carga"_u);
            }
            else if (error.isEmpty())
            {
                deck (deckIndex).setTrack (result);
                if (autoGain.load())
                    channel (deckIndex).trimDb = autoGainTrimFor (*result);   // the GAIN knob follows
                if (onTrackLoaded) onTrackLoaded (deckIndex, result->file);
                if (cb) cb (true, {});
            }
            else if (cb)
            {
                cb (false, error);
            }

            sendChangeMessage();
        });
    });
}

float AudioEngine::autoGainTrimFor (const LoadedTrack& t) const
{
    if (t.loudnessLufs <= -70.0)
        return 0.0f;
    return (float) juce::jlimit (-12.0, 6.0, (double) autoGainTargetLufs.load() - t.loudnessLufs);   // gentle boosts only
}

void AudioEngine::loadRmxSample (int deckIndex, int pad, const juce::File& file, LoadCallback cb)
{
    if (file == juce::File())
    {
        rmx (deckIndex).setSample (pad, nullptr, {});
        if (cb) cb (true, {});
        return;
    }
    const double outRate = currentRate.load() > 0.0 ? currentRate.load() : 48000.0;
    auto aliveFlag = alive;
    loaderPool.addJob ([this, aliveFlag, deckIndex, pad, file, outRate, cb]
    {
        std::shared_ptr<juce::AudioBuffer<float>> buf;
        juce::String error;
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr) error = "Formato no soportado";
        else
        {
            const int len = (int) std::min (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 10.0));  // ≤ 10 s
            juce::AudioBuffer<float> src (2, len);
            src.clear();
            reader->read (&src, 0, len, 0, true, reader->numChannels > 1);
            if (reader->numChannels == 1) src.copyFrom (1, 0, src, 0, 0, len);
            const double ratio = reader->sampleRate / outRate;
            const int outLen = (int) std::floor ((len - 1) / ratio);
            buf = std::make_shared<juce::AudioBuffer<float>> (2, juce::jmax (1, outLen));
            const auto& rs = SincResampler::get();
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < outLen; ++i)
                    buf->setSample (c, i, rs.interpolate (src.getReadPointer (c), len, i * ratio, ratio));
        }
        juce::MessageManager::callAsync ([this, aliveFlag, deckIndex, pad, buf, error, file, cb]
        {
            if (! *aliveFlag) return;
            if (error.isEmpty())
            {
                rmx (deckIndex).setSample (pad, buf, file.getFullPathName());
                if (rmx (deckIndex).needsProcessing())       // re-render with the current TONE / SPEED
                    setRmxToneSpeed (deckIndex, rmx (deckIndex).getTone(), rmx (deckIndex).getSpeed());
            }
            if (cb) cb (error.isEmpty(), error);
        });
    });
}

void AudioEngine::setRmxToneSpeed (int deckIndex, float semitones, float speed)
{
    auto& u = rmx (deckIndex);
    u.setToneAndSpeed (juce::jlimit (-12.0f, 12.0f, semitones), juce::jlimit (0.5f, 2.0f, speed));
    const int gen = ++rmxRenderGen[(size_t) deckIndex];
    if (! u.needsProcessing())
    {
        u.setProcessed ({});
        return;
    }
    std::array<std::shared_ptr<const juce::AudioBuffer<float>>, RmxUnit::numDrums> sources;
    for (int p = 0; p < RmxUnit::numDrums; ++p) sources[(size_t) p] = u.sourceFor (p);
    const double rate = u.sampleRate();
    const float t = u.getTone(), s = u.getSpeed();
    auto aliveFlag = alive;
    loaderPool.addJob ([this, aliveFlag, deckIndex, gen, sources, rate, t, s]
    {
        juce::Thread::sleep (60);                                   // knob still moving: render only the last value
        std::array<std::shared_ptr<juce::AudioBuffer<float>>, RmxUnit::numDrums> out;
        for (int p = 0; p < RmxUnit::numDrums; ++p)
        {
            if (rmxRenderGen[(size_t) deckIndex].load() != gen) return;
            if (sources[(size_t) p] != nullptr) out[(size_t) p] = RmxUnit::render (*sources[(size_t) p], rate, t, s);
        }
        juce::MessageManager::callAsync ([this, aliveFlag, deckIndex, gen, out]
        {
            if (! *aliveFlag || rmxRenderGen[(size_t) deckIndex].load() != gen) return;
            rmx (deckIndex).setProcessed (out);
        });
    });
}

//==============================================================================
juce::String AudioEngine::startRecording()
{
    const auto sr = currentRate.load();
    if (sr <= 0.0)
        return "No hay dispositivo de audio activo";

    auto name = "OpenMix " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S") + ".wav";
    auto err = rec.start (Recorder::defaultFolder().getChildFile (name), sr);
    sendChangeMessage();
    return err;
}

void AudioEngine::stopRecording()
{
    rec.stop();
    sendChangeMessage();
}

//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    prepareForTest (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(),
                    device->getActiveOutputChannels().countNumberOfSetBits());
    // The built-in X-PAD sounds were re-synthesised at the new rate: re-render TONE / SPEED too.
    auto aliveFlag = alive;
    juce::MessageManager::callAsync ([this, aliveFlag]
    {
        if (! *aliveFlag) return;
        for (int d = 0; d < numDecks; ++d)
            if (rmx (d).needsProcessing()) setRmxToneSpeed (d, rmx (d).getTone(), rmx (d).getSpeed());
    });
}

void AudioEngine::prepareForTest (double sr, int block, int outputs)
{
    deckBuf.setSize (2, juce::jmax (block, 4096));
    cueBuf.setSize (2, juce::jmax (block, 4096));
    masterBuf.setSize (2, juce::jmax (block, 4096));
    sepBuf.setSize (2 * numDecks, juce::jmax (block, 4096));

    for (auto& d : decks) d->prepare (sr);
    for (auto& c : channels) c.prepare (sr, juce::jmax (block, 4096));
    for (auto& deckSlots : fxSlots)
        for (auto& f : deckSlots) f.prepare (sr, juce::jmax (block, 4096));
    for (auto& u : rmxUnits) u.prepare (sr, juce::jmax (block, 4096));

    masterLimiter.prepare (sr, juce::jmax (block, 4096));
    masterGain.reset (sr, 0.02);
    masterGain.setCurrentAndTargetValue (masterDbToGain (masterDb.load()));

    currentRate = sr;
    currentBlock = block;
    numOutputs = outputs;
}

void AudioEngine::audioDeviceStopped()
{
    currentRate = 0.0;
    numOutputs = 0;
}

void AudioEngine::audioDeviceError (const juce::String& message)
{
    lastError = message;
}

void AudioEngine::applySync()
{
    // Who has been on air the longest? That deck is never retimed by SYNC.
    ++syncClock;

    // Seeks and cue / hot-cue jumps happen first, so a synced deck is aligned at its new place before
    // anything of it is heard (otherwise the jump would undo the alignment and phase would settle late).
    std::array<bool, numDecks> moved {};
    for (int i = 0; i < numDecks; ++i)
        decks[(size_t) i]->withAudioLock ([&] { moved[(size_t) i] = decks[(size_t) i]->audioResolveMoves(); });
    std::array<bool, numDecks> audibleNow {};
    for (int i = 0; i < numDecks; ++i)
    {
        audibleNow[(size_t) i] = decks[(size_t) i]->isAudiblyPlaying();
        if (! audibleNow[(size_t) i]) playingSince[(size_t) i] = 0;
        else if (playingSince[(size_t) i] == 0) playingSince[(size_t) i] = syncClock;
    }

    for (int i = 0; i < numDecks; ++i)
    {
        auto& d = *decks[(size_t) i];
        auto& o = *decks[(size_t) (1 - i)];
        const bool dPlays = audibleNow[(size_t) i], oPlays = audibleNow[(size_t) (1 - i)];
        // PLAY was just pressed and nothing has sounded yet: align before the first sample.
        const bool starting = d.isPlaying() && ! dPlays && ! wasAudible[(size_t) i];
        const bool justStarted = dPlays && ! wasAudible[(size_t) i];
        wasAudible[(size_t) i] = dPlays;

        if (! d.isSyncOn())
        {
            d.audioSetPhaseCorrection (0.0);
            continue;
        }

        // Leader: a manually chosen master; otherwise the deck that is playing (the one playing
        // longest when both are). The track on air keeps its tempo — the incoming one adapts.
        const int masterIdx = masterDeck.load();
        bool dLeads;
        if (masterIdx >= 0)          dLeads = masterIdx == i;
        else if (dPlays && ! oPlays) dLeads = true;
        else if (oPlays && ! dPlays) dLeads = false;
        else if (dPlays && oPlays)   dLeads = playingSince[(size_t) i] < playingSince[(size_t) (1 - i)];
        else                         dLeads = o.isSyncOn() && o.getSyncStamp() > d.getSyncStamp();   // both stopped

        if (dLeads)
        {
            if (d.consumeJustLoaded())
                d.audioSetPitch (0.0);              // nothing to follow: the new track plays at its own tempo
            d.consumeSyncEngaged();
            d.audioSetPhaseCorrection (0.0);
            continue;
        }

        bool otherHasGrid = false, ran = false;
        d.withAudioLock ([&]
        {
            ran = true;
            o.withAudioLock ([&]
            {
                otherHasGrid = true;
                const auto dg = d.audioGrid(), og = o.audioGrid();
                if (! dg.isValid() || ! og.isValid())
                {
                    otherHasGrid = og.isValid();
                    return;
                }

                // Tempo: follow the leader (half/double time aware).
                double ratio = og.bpm * (1.0 + o.getPitch()) / dg.bpm;
                double beatScale = 1.0;
                while (ratio > 1.5)  { ratio *= 0.5; beatScale *= 2.0; }
                while (ratio < 0.75) { ratio *= 2.0; beatScale *= 0.5; }
                d.audioSetPitch (ratio - 1.0);

                d.consumeJustLoaded();
                // A jump of the leader realigns this deck only while it is still silent; once it is
                // heard, moving its playhead because the other track jumped would cut it.
                const bool leaderJumped = moved[(size_t) (1 - i)];
                const bool engaged = d.consumeSyncEngaged() || moved[(size_t) i] || (leaderJumped && ! dPlays);
                if (! oPlays || ! d.isPlaying() || ! (dPlays || starting))   // a stopped deck is never moved
                {
                    d.audioSetPhaseCorrection (0.0);
                    return;
                }

                // Phase: keep beats locked continuously (not only when SYNC is pressed).
                const double oBeat = og.beatAt (o.audioPositionSeconds()) / beatScale;
                const double dBeat = dg.beatAt (d.audioPositionSeconds());
                double delta = (oBeat - std::floor (oBeat)) - (dBeat - std::floor (dBeat));
                if (delta > 0.5)  delta -= 1.0;
                if (delta < -0.5) delta += 1.0;

                // Manual corrections are respected: when the DJ nudges (jog / pitch bend), the
                // resulting offset becomes the new target and sync holds it instead of undoing it.
                const bool userNudging = d.isScratching() || std::abs (d.getBend()) > 1.0e-6;
                if (engaged || starting || justStarted)
                    phaseOffset[(size_t) i] = 0.0;

                // Loops, rolls and other performance moves on either deck break the beat-for-beat
                // relation on purpose (a ½ beat loop jumps back every half beat). While one is going on,
                // sync only keeps the tempo; afterwards it holds whatever phase the DJ left.
                auto offGrid = [] (Deck& x)
                {
                    return x.isLoopActive() || x.isRolling() || x.isReverse() || x.isCensor()
                        || x.isBraking() || x.isBackspinning();
                };
                const bool performing = offGrid (d) || offGrid (o) || o.isScratching();

                // CUE held on a paused deck: the stab starts exactly on the cue point, never shifted.
                // If PLAY follows while it sounds, sync keeps the phase the DJ played it in.
                if (d.isCuePreviewing())
                {
                    wasNudging[(size_t) i] = true;
                    d.audioSetPhaseCorrection (0.0);
                    return;
                }

                if (userNudging || (performing && ! (starting || engaged)))
                {
                    wasNudging[(size_t) i] = true;
                    d.audioSetPhaseCorrection (0.0);
                    return;
                }
                if (starting || engaged)
                    wasNudging[(size_t) i] = false;               // PLAY / SYNC / a jump: align for real
                if (wasNudging[(size_t) i] || (leaderJumped && dPlays && ! engaged))
                {
                    wasNudging[(size_t) i] = false;
                    phaseOffset[(size_t) i] = delta;          // keep the phase the DJ left
                }

                double err = delta - phaseOffset[(size_t) i];
                if (err > 0.5)  err -= 1.0;
                if (err < -0.5) err += 1.0;

                // The playhead is only moved before the deck is heard or when the DJ asks for it
                // (SYNC pressed, or a jump on this deck). A deck on air is never cut by sync.
                if (starting || engaged || justStarted)
                {
                    d.audioShiftSeconds (err * dg.beatLengthSeconds());     // aligned at once (silent if starting)
                    d.audioSetPhaseCorrection (0.0);
                }
                else
                {
                    // Hold the target phase against tiny drift (≤ 1 %, inaudible).
                    const double errSeconds = err * dg.beatLengthSeconds() / ratio;
                    d.audioSetPhaseCorrection (juce::jlimit (-0.01, 0.01, errSeconds / 0.4));
                }
            });
        });
        if (ran && ! otherHasGrid && d.consumeJustLoaded())
            d.audioSetPitch (0.0);                            // nothing on the other deck to follow
    }
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputChannels,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    ++callbackCount;

    for (int c = 0; c < numOutputChannels; ++c)
        if (outputs[c] != nullptr)
            juce::FloatVectorOperations::clear (outputs[c], numSamples);

    if (numOutputChannels < 2 || outputs[0] == nullptr || outputs[1] == nullptr || numSamples > deckBuf.getNumSamples())
        return;

    applySync();

    masterBuf.clear (0, numSamples);
    float* mL = masterBuf.getWritePointer (0);
    float* mR = masterBuf.getWritePointer (1);
    cueBuf.clear (0, numSamples);
    const int mode = outputMode.load();

    if (const float rate = autoXfPerSecond.load(); rate > 0.0f && currentRate.load() > 0.0)
    {
        const float step = rate * (float) (numSamples / currentRate.load());
        const float cur = crossfader.load(), tgt = autoXfTarget.load();
        const float next = std::abs (tgt - cur) <= step ? tgt : cur + (tgt > cur ? step : -step);
        crossfader = next;
        if (std::abs (next - tgt) < 1.0e-6f) autoXfPerSecond = 0.0f;
    }
    const float xf = crossfader.load();
    const int curve = crossfaderCurve.load();

    for (int i = 0; i < numDecks; ++i)
    {
        float* l = deckBuf.getWritePointer (0);
        float* r = deckBuf.getWritePointer (1);

        decks[(size_t) i]->render (l, r, numSamples);
        auto& ch = channels[(size_t) i];
        ch.processPreFader (l, r, numSamples, decks[(size_t) i]->getRenderedBpm());

        {
            auto& dk = *decks[(size_t) i];
            rmxUnits[(size_t) i].process (l, r, numSamples, dk.audioBlockBeat(), dk.audioBeatsPerSample(), dk.getRenderedBpm(),
                                          dk.getQuantizeBeats());
        }

        ch.meter.push (l, r, numSamples);              // channel meter: pre-fader, as on a club mixer

        if (mode != speakersOnly && ch.pfl.load())
        {
            cueBuf.addFrom (0, 0, l, numSamples);
            cueBuf.addFrom (1, 0, r, numSamples);
        }

        // Linear ramps from last block's gains avoid zipper noise on fader moves.
        auto ramp = [numSamples] (float* x, float* y, float g0, float g1)
        {
            const float step = (g1 - g0) / (float) numSamples;
            for (int s = 0; s < numSamples; ++s)
            {
                const float gs = g0 + step * (float) (s + 1);
                x[s] *= gs;
                y[s] *= gs;
            }
        };

        // Channel fader first, then the beat FX (post-fader, as on a club mixer): pulling the fader down
        // stops feeding the echo / reverb but its tail keeps ringing out at full level.
        const float fg = ch.faderGain();
        ramp (l, r, lastFaderGain[(size_t) i], fg);
        lastFaderGain[(size_t) i] = fg;

        {
            auto& dk = *decks[(size_t) i];
            for (auto& f : fxSlots[(size_t) i])
                f.process (l, r, numSamples, dk.getRenderedBpm(), dk.audioBlockBeat(), dk.audioBeatsPerSample());
        }

        if (mode == separateDecks)
        {
            // External mixer: each deck leaves on its own pair, after the channel and its FX.
            sepBuf.copyFrom (2 * i,     0, l, numSamples);
            sepBuf.copyFrom (2 * i + 1, 0, r, numSamples);
        }

        const float xg = ch.crossfaderGain (xf, curve);
        ramp (l, r, lastXfGain[(size_t) i], xg);
        lastXfGain[(size_t) i] = xg;
        for (int s = 0; s < numSamples; ++s)
        {
            mL[s] += l[s];
            mR[s] += r[s];
        }
    }

    masterGain.setTargetValue (masterDbToGain (masterDb.load()));
    for (int s = 0; s < numSamples; ++s)
    {
        const float g = masterGain.getNextValue();
        mL[s] *= g;
        mR[s] *= g;
    }

    masterLimiter.process (mL, mR, numSamples);
    master.push (mL, mR, numSamples);
    rec.write (mL, mR, numSamples);

    // ── Routing to the card ──
    auto out = [&] (int c) -> float* { return juce::isPositiveAndBelow (c, numOutputChannels) ? outputs[c] : nullptr; };
    auto putStereo = [&] (int first, const float* a, const float* b)
    {
        if (auto* o = out (first))     juce::FloatVectorOperations::add (o, a, numSamples);
        if (auto* o = out (first + 1)) juce::FloatVectorOperations::add (o, b, numSamples);
    };
    const float mix = cueMix.load();
    const float pg = juce::Decibels::decibelsToGain (phonesDb.load());
    float* pL = cueBuf.getWritePointer (0);
    float* pR = cueBuf.getWritePointer (1);
    for (int s = 0; s < numSamples; ++s)                      // headphones: cue / master blend
    {
        pL[s] = (pL[s] * (1.0f - mix) + mL[s] * mix) * pg;
        pR[s] = (pR[s] * (1.0f - mix) + mR[s] * mix) * pg;
    }

    const int mc = masterChannel.load(), pc = phonesChannel.load();
    if (mode == separateDecks)
    {
        for (int i = 0; i < numDecks; ++i)
            putStereo (deckChannel[(size_t) i].load(), sepBuf.getReadPointer (2 * i), sepBuf.getReadPointer (2 * i + 1));
        if (pc >= 0) putStereo (pc, pL, pR);
    }
    else if (mode == speakersPhones && splitMono.load())
    {
        // "Y" splitter on one stereo output: left = master (mono), right = headphones (mono).
        float* oL = out (mc);
        float* oR = out (mc + 1);
        for (int s = 0; s < numSamples; ++s)
        {
            if (oL != nullptr) oL[s] += 0.5f * (mL[s] + mR[s]);
            if (oR != nullptr) oR[s] += 0.5f * (pL[s] + pR[s]);
        }
    }
    else
    {
        putStereo (mc, mL, mR);
        if (mode == speakersPhones && pc >= 0 && pc != mc)
            putStereo (pc, pL, pR);
    }
}

//==============================================================================
void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateStatus();
}

void AudioEngine::timerCallback()
{
    updateStatus();
}

void AudioEngine::updateStatus()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    auto newStatus = status;
    juce::String detail;

    if (device == nullptr)
    {
        auto* type = deviceManager.getCurrentDeviceTypeObject();
        const bool anyDevice = type != nullptr && ! type->getDeviceNames (false).isEmpty();

        if (everHadDevice)       { newStatus = DeviceStatus::disconnected; detail = "Dispositivo desconectado"; }
        else if (! anyDevice)    { newStatus = DeviceStatus::unavailable;  detail = "No hay salidas de audio"; }
        else                     { newStatus = DeviceStatus::error;        detail = lastError.isNotEmpty() ? lastError : "Sin dispositivo seleccionado"; }
    }
    else
    {
        everHadDevice = true;
        const auto count = callbackCount.load();
        detail = device->getName() + " · "_u + juce::String (device->getCurrentSampleRate() / 1000.0, 1) + " kHz · "_u
               + juce::String (device->getCurrentBufferSizeSamples()) + " smp";

        if (count != lastSeenCount)       newStatus = DeviceStatus::running;
        else if (! device->isPlaying())   { newStatus = DeviceStatus::error; detail = lastError.isNotEmpty() ? lastError : "El dispositivo no está procesando"_u; }
        else                              newStatus = status == DeviceStatus::running ? DeviceStatus::error : DeviceStatus::initialising;

        lastSeenCount = count;
    }

    if (newStatus != status || detail != statusDetail)
    {
        status = newStatus;
        statusDetail = detail;
        sendChangeMessage();
    }
}

} // namespace om
