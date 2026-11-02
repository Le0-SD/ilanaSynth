#include "Voice.h"
#include "FastMath.h"
#include "Generative.h"
#include "StretchRenderer.h"

#include <array>
#include <cmath>

namespace
{
// The SUB card's COLOUR: 0 passes the wave as it is (every older patch); up
// to 1 drives it into a tanh, which adds harmonics to a sine and rounds the
// edges of a square or saw, scaled so the peak stays at the wave's own.
inline float colourSub (float wave, float colour)
{
    if (colour <= 0.0005f)
        return wave;
    const auto drive = 1.0f + 6.0f * colour;
    return std::tanh (wave * drive) / std::tanh (drive);
}

int chordInterval (int mode, int voiceIndex)
{
    switch (mode)
    {
        case 1:  { static const int table[] = { 0, 12 }; return table[voiceIndex % 2]; }
        case 2:  { static const int table[] = { 0, 7 }; return table[voiceIndex % 2]; }
        case 3:  { static const int table[] = { 0, 7, 12 }; return table[voiceIndex % 3]; }
        case 4:  { static const int table[] = { 0, 4, 7, 12 }; return table[voiceIndex % 4]; }
        case 5:  { static const int table[] = { 0, 3, 7, 12 }; return table[voiceIndex % 4]; }
        case 6:  { static const int table[] = { 0, 5, 7, 12 }; return table[voiceIndex % 4]; }
        default: return 0;
    }
}

// Unison voices are spread over -1..1; each mode maps that position to a
// pitch offset. Classic is linear; Hypersaw bunches voices near the centre
// like the JP-8000 supersaw; Octaves and Fifths stack intervals too.
double unisonDetunePosition (int mode, float position)
{
    if (mode == UnisonMode::Hypersaw)
        return (position >= 0.0f ? 1.0 : -1.0) * std::pow ((double) std::abs (position), 1.5);

    return (double) position;
}

int unisonStackInterval (int mode, int voiceIndex, int numVoices)
{
    if (numVoices < 2)
        return 0;

    if (mode == UnisonMode::Octaves)
    {
        static const int table[] = { 0, 12, -12 };
        return table[voiceIndex % 3];
    }

    if (mode == UnisonMode::Fifths)
    {
        static const int table[] = { 0, 7, 12 };
        return table[voiceIndex % 3];
    }

    switch (mode)
    {
        case UnisonMode::CenterDrop: return voiceIndex == numVoices / 2 ? -12 : 0;
        case UnisonMode::Octaves2:   { static const int t[] { 0, 12, 24, -12, -24 }; return t[voiceIndex % 5]; }
        case UnisonMode::PowerChord: { static const int t[] { 0, 7 }; return t[voiceIndex % 2]; }
        case UnisonMode::MajorChord: { static const int t[] { 0, 4, 7 }; return t[voiceIndex % 3]; }
        case UnisonMode::MinorChord: { static const int t[] { 0, 3, 7 }; return t[voiceIndex % 3]; }
        // Harmonic n sits 12 log2 (n) semitones above the root.
        case UnisonMode::Harmonics:     return (int) std::lround (12.0 * std::log2 ((double) (voiceIndex + 1)));
        case UnisonMode::OddHarmonics:  return (int) std::lround (12.0 * std::log2 ((double) (2 * voiceIndex + 1)));
        default: break;
    }

    return 0;
}

float lfoShapeAt (int shape, double phase, float hold, const float* steps, const float* custom, int customSize)
{
    switch (shape)
    {
        case 0: return (float) std::sin (juce::MathConstants<double>::twoPi * phase);
        case 1: return (float) (1.0 - 4.0 * std::abs (phase - 0.5));
        case 2: return (float) (2.0 * phase - 1.0);
        case 3: return (float) (1.0 - 2.0 * phase);
        case 4: return phase < 0.5 ? 1.0f : -1.0f;
        case 5: return hold;
        case 7: return steps != nullptr ? steps[juce::jlimit (0, 15, (int) (phase * 16.0))] : 0.0f;
        default:
        {
            // Draw (6) and any table-driven shape.
            if (custom == nullptr || customSize <= 0)
                return 0.0f;

            const auto position = phase * (double) customSize;
            const auto index = (int) position % customSize;
            const auto next = (index + 1) % customSize;
            const auto frac = (float) (position - std::floor (position));
            return custom[index] + (custom[next] - custom[index]) * frac;
        }
    }
}

// Scales for explicit destinations: how far a full-depth modulation moves
// the target, in the target's own units.
// (The rest are each knob's whole range: Mod::Range.)
constexpr float driveRange = 9.0f;       // drive multiplier

// How much of the cabled audio a material body takes in besides its own
// excitation, so audio patched into BODY rings the body.
constexpr float bodyAudioShare = 0.3f;
} // namespace

Voice::Voice()
{
    filterDestinationMarks();
    if (fixedRandomSeeds().load())
    {
        random.setSeed (12345);
        driftRandom.setSeed (54321);
    }
}

void Voice::setCurrentPlaybackSampleRate (double newRate)
{
    juce::SynthesiserVoice::setCurrentPlaybackSampleRate (newRate);

    if (newRate <= 0.0)
        return;

    sampleRate = newRate;

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        frameSmooth[osc].reset (newRate, 0.02);
        levelSmooth[osc].reset (newRate, 0.02);
        oscEnableSmooth[osc].reset (newRate, 0.02);
    }
    noiseSmooth.reset (newRate, 0.02);
    noiseEnableSmooth.reset (newRate, 0.02);
    subOscColourSmooth.reset (newRate, 0.02);
    noiseGenerator.prepare (newRate);
    fmNoiseGenerator.prepare (newRate);
    subOscLevelSmooth.reset (newRate, 0.02);
    subOscEnableSmooth.reset (newRate, 0.02);
    subOsc.setSampleRate (newRate);

    ampEnv.setSampleRate (newRate);
    filterEnv.setSampleRate (newRate);
    filter2Env.setSampleRate (newRate);
    modEnv.setSampleRate (newRate);
    env4.setSampleRate (newRate);
    for (auto& env : extraEnvs)
        env.setSampleRate (newRate);
    envMseg.prepare (newRate);

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        oscBank[osc].setSampleRate (newRate);

        for (int u = 0; u < VoiceParams::maxBufferedUnison; ++u)
        {
            stringFor (osc, u).prepare (newRate);
            sampleUnison[osc][u].prepare (newRate);
            sampleLayer[osc][u].prepare (newRate);
            grains[osc][u].prepare (newRate);
        }
    }

    resonatorL.prepare (newRate);
    resonatorR.prepare (newRate);
    materialBodyL.prepare (newRate);
    materialBodyR.prepare (newRate);
    westGateL.prepare (newRate);
    westGateR.prepare (newRate);
    westFolder.reset();

    for (auto* filter : { &filter1L, &filter1R, &filter2L, &filter2R,
                          &bothFilter1L, &bothFilter1R, &bothFilter2L, &bothFilter2R })
        filter->prepare (newRate);
    filter1Linked = filter2Linked = true;
}

void Voice::syncSamplePlayers()
{
    const auto setup = [] (const VoiceParams::OscParams& osc, const SampleZone* zone, const SampleData* copy, bool sameSound,
                           SamplePlayer* players, GranularOsc* clouds, float startMod, float endMod, float level)
    {
        const auto start = juce::jlimit (0.0f, 0.98f, osc.sampleStart + startMod);
        const auto end = juce::jlimit (start + 0.01f, 1.0f, osc.sampleEnd + endMod);
        // A multisample plays the region the note picked, with its loop; a
        // key outside every zone, with OUTSIDE ZONES on Silence, nothing.
        const auto* sample = zone != nullptr ? zone->data.get()
                             : osc.sample != nullptr && ! osc.sample->zones.empty() && (osc.sampleSilenceOutside || osc.sample->isSliced()) ? nullptr
                                                                                                                 : osc.sample;
        // A rendered copy is the whole sample stretched, so the region's
        // start and end stay where they were; a region loop scales with it.
        const auto loopScale = copy != nullptr && sample != nullptr && sample->getNumSamples() > 0
                                   ? (double) copy->getNumSamples() / (double) sample->getNumSamples()
                                   : 1.0;

        SamplePlayer::Params sampleParams;
        sampleParams.sample = copy != nullptr ? copy : sample;
        sampleParams.keepPosition = sameSound;
        if (zone != nullptr)
        {
            sampleParams.gain = zone->gain * level;
            if (zone->loop)
            {
                sampleParams.loopStart = (int) std::lround (zone->loopStart * loopScale);
                sampleParams.loopEnd = (int) std::lround (zone->loopEnd * loopScale);
            }
        }
        sampleParams.loop = osc.sampleLoop;
        sampleParams.ownLoop = osc.sampleLoopOwn;
        sampleParams.loopFrom = osc.sampleLoopStart;
        sampleParams.loopTo = osc.sampleLoopEnd;
        sampleParams.loopCrossfade = osc.sampleLoopXfade;
        sampleParams.playback = osc.samplePlayback;
        sampleParams.pingPong = osc.samplePingPong;
        sampleParams.startRandom = osc.sampleStartRandom;
        sampleParams.startSafe = osc.sampleStartSafe;
        sampleParams.quality = osc.sampleQuality;
        // Step 32: a slice (or zone) can be reversed on its own.
        sampleParams.reverse = osc.sampleReverse != (zone != nullptr && zone->reverse);
        sampleParams.start = start;
        sampleParams.end = end;
        sampleParams.fadeIn = osc.sampleFadeIn;
        sampleParams.fadeOut = osc.sampleFadeOut;
        // TIME's engines: the copy plays through the live stretch too, which
        // at the copy's own speed passes it through, and covers bends,
        // cents and tempo changes mid-note.
        sampleParams.stretch = osc.sampleStretch || osc.sampleEngine >= 0;

        for (int u = 0; u < VoiceParams::maxBufferedUnison; ++u)
            if (! players[u].setParamsIsNoOp (sampleParams))
                players[u].setParams (sampleParams);

        if (osc.granularMode && clouds != nullptr)
        {
            GranularOsc::Params grainParams;
            grainParams.sample = sample;
            grainParams.position = juce::jlimit (0.0f, 1.0f, osc.grainPosition + startMod);
            grainParams.scan = osc.grainScan;
            grainParams.freeze = osc.grainFreeze;
            grainParams.sizeMs = osc.grainSizeMs;
            grainParams.density = osc.grainDensity;
            grainParams.spray = osc.grainSpray;
            grainParams.pitchSpray = osc.grainPitch;
            grainParams.spread = osc.grainSpread;
            grainParams.reverse = osc.sampleReverse;
            grainParams.live = osc.grainLive;
            grainParams.liveWrite = osc.liveWrite;

            for (int u = 0; u < VoiceParams::maxBufferedUnison; ++u)
                clouds[u].setParams (grainParams);
        }
    };

    constexpr Mod::Destination startDestinations[] { Mod::Destination::Osc1SampleStart,
                                                      Mod::Destination::Osc2SampleStart,
                                                      Mod::Destination::SubSampleStart,
                                                      Mod::Destination::Osc4SampleStart, Mod::Destination::Osc5SampleStart,
                                                      Mod::Destination::Osc6SampleStart };
    constexpr Mod::Destination endDestinations[] { Mod::Destination::Osc1SampleEnd,
                                                    Mod::Destination::Osc2SampleEnd,
                                                    Mod::Destination::SubSampleEnd,
                                                    Mod::Destination::Osc4SampleEnd, Mod::Destination::Osc5SampleEnd,
                                                    Mod::Destination::Osc6SampleEnd };

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        if (osc < 3 || params.oscillatorEnabled[osc]
            || oscEnableSmooth[osc].getCurrentValue() > 0.0005f)
        {
            const auto& settings = params.oscillators[osc];
            const auto* sample = settings.sample;
            sampleZone[osc] = sample != nullptr && ! sample->zones.empty() ? sample->zoneFor (lastNote, lastVelocity, settings.sampleSilenceOutside || sample->isSliced(),
                                                                                                    settings.sampleRoundRobin, zonePick[osc])
                                                                                    : nullptr;
            // Step 37: near a velocity layer's edge, the layer across it too.
            layerKeep[osc] = 1.0f;
            layerGain[osc] = 0.0f;
            layerZone[osc] = sampleZone[osc] != nullptr && ! settings.granularMode
                                 ? sample->blendPartner (*sampleZone[osc], lastNote, lastVelocity, settings.sampleVelBlend,
                                                         settings.sampleRoundRobin, zonePick[osc], layerKeep[osc], layerGain[osc])
                                 : nullptr;
            const auto* origin = sampleZone[osc] != nullptr ? sampleZone[osc]->data.get() : sample;
            const auto* copy = settings.granularMode ? nullptr : stretchCopyFor (osc, origin);
            const auto sameSound = origin != nullptr && origin == stretchOrigin[osc];
            stretchOrigin[osc] = origin;
            stretchCopy[osc] = copy;
            setup (settings, sampleZone[osc], copy, sameSound, sampleUnison[osc], grains[osc],
                   blockMod (startDestinations[osc]), blockMod (endDestinations[osc]), layerKeep[osc]);
            const auto* layer = layerZone[osc] != nullptr ? layerZone[osc]->data.get() : nullptr;
            if (layer != nullptr)
                setup (settings, layerZone[osc], nullptr, layer == layerOrigin[osc], sampleLayer[osc], nullptr,
                       blockMod (startDestinations[osc]), blockMod (endDestinations[osc]), layerGain[osc]);
            layerOrigin[osc] = layer;
        }
    stretchNew = false;
}

// The copy of origin TIME's engine rendered for this note (asked for if it
// isn't there yet), or null to play the original.
const SampleData* Voice::stretchCopyFor (int osc, const SampleData* origin)
{
    const auto& settings = params.oscillators[osc];
    if (params.stretchRenderer == nullptr || settings.sampleEngine < 0 || origin == nullptr || origin->getNumSamples() < 2
        || ! settings.sampleMode)
        return nullptr;

    // The note's key: its length from the tempo and its semitones from the
    // note, picked once per note (and again if the sample or engine changes).
    if (stretchNew || origin != stretchOrigin[osc] || settings.sampleEngine != stretchEngine[osc])
    {
        const auto regionSeconds = origin->sampleRate > 0.0 ? (double) juce::jmax (0.0f, settings.sampleEnd - settings.sampleStart)
                                                                  * origin->getNumSamples() / origin->sampleRate
                                                            : 0.0;
        const auto tempo = SampleTempo::tempoRatio (settings.sampleSync, settings.sampleBpm, regionSeconds, params.hostBpm);
        stretchStep[osc] = StretchRenderer::lengthStep (1.0 / tempo);
        stretchEngine[osc] = settings.sampleEngine;
        stretchSemis[osc] = 0;
        if (settings.sampleTuned)
        {
            const auto* zone = sampleZone[osc];
            const auto frequency = settings.tuneMode == OscTuning::Semitones
                                       ? baseFrequency * std::exp2 ((settings.semitones + (osc == 2 ? params.subOctaveOffset : 0)
                                                                     + settings.cents / 100.0) / 12.0)
                                   : settings.tuneMode == OscTuning::Fixed
                                       ? oscFrequencyFactor (settings)
                                       : baseFrequency * settings.ratio * std::exp2 ((settings.semitones + settings.cents / 100.0) / 12.0);
            const auto root = zone != nullptr ? zone->rootHz() : plainRootHz (settings.sampleRoot);
            if (frequency > 0.0 && root > 0.0)
                stretchSemis[osc] = juce::jlimit (-48, 48, (int) std::lround (12.0 * std::log2 (frequency / root)));
        }
    }

    if (stretchStep[osc] == 0 && stretchSemis[osc] == 0)
        return nullptr; // the copy would be the original

    return params.stretchRenderer->get (osc, origin, settings.sampleEngine, stretchStep[osc], stretchSemis[osc]);
}

void Voice::startNote (int midiNoteNumber, float velocity, juce::SynthesiserSound*, int currentPitchWheelPosition)
{
    if (paramsSource != nullptr)
        params = *paramsSource;

    const auto mono = monoPending;
    const auto legato = mono && monoLegato;
    releaseSilentSamples = 0;
    releasePeak = 0.0f;
    heldAsleep = false;
    heldSilentSamples = 0;
    fadeTotal = fadeRemaining = 0;
    sounded = false;
    ampGainFade = 1.0f;
    const auto keepRunning = mono && monoKeepRunning;
    const auto glide = ! mono || monoGlide;
    monoPending = false;
    monoKeepRunning = false;

    baseFrequency = params.tuning != nullptr ? params.tuning->getFrequency (midiNoteNumber)
                                             : juce::MidiMessage::getMidiNoteInHertz (midiNoteNumber);

    // Stretch tuning (Railsback): bass a little flat, treble sharp, about
    // +/-35 cents at the ends of the keyboard when fully on.
    if (params.stretch > 0.0f)
    {
        const auto distance = (double) (midiNoteNumber - 60) / 40.0;
        const auto cents = (double) params.stretch * 35.0 * (distance < 0.0 ? -1.0 : 1.0) * distance * distance;
        baseFrequency *= std::exp2 (cents / 1200.0);
    }

    // A hand-tuned instrument is never quite in tune: each note lands up to
    // 40 cents off, a fresh draw every time (sharp or flat, or flat only).
    if (params.tuneLoose > 0.0f)
    {
        const auto draw = driftRandom.nextFloat();
        const auto shift = params.tuneLooseBoth ? draw * 2.0f - 1.0f : -draw;
        baseFrequency *= std::exp2 ((double) shift * (double) params.tuneLoose / 1200.0);
    }

    pitchNote = midiNoteNumber;
    keyTrackValue = juce::jlimit (-1.0f, 1.0f, (float) (midiNoteNumber - 60) / 48.0f);
    keyTrackOctaves = (float) (midiNoteNumber - 60) / 12.0f;

    // Poly: glide from the previous note played, not from this voice's own
    // last pitch (which depends on which voice the synth handed out). Off
    // until ilana decides: it changes how poly GLIDE presets sound.
    if (polyGlideFromLastNote && ! mono && params.glideTime > 0.001f && shared != nullptr && shared->lastNoteFrequency > 0.0)
    {
        currentFrequency = shared->lastNoteFrequency;
        hasPlayedNote = true;
    }
    else if (! hasPlayedNote || ! glide)
    {
        currentFrequency = baseFrequency;
        hasPlayedNote = true;
    }

    if (shared != nullptr)
    {
        shared->lastNoteFrequency = baseFrequency;

        // Mod wheel, expression and pressure as they are now, not as this
        // voice last heard them.
        const auto channel = playingChannel();
        modWheel = shared->modWheel[channel];
        expressionValue = shared->expression[channel];
        aftertouchValue = shared->pressure[channel];
        masterBend = params.mpe ? shared->masterBendSemitones : 0.0;
    }
    else
        masterBend = 0.0;

    pitchWheelMoved (currentPitchWheelPosition);

    // Legato: the note just changes pitch; envelopes, phases and filters
    // carry on from where they are.
    if (legato)
        return;

    velocityLevel = velocity;
    lastNote = midiNoteNumber;
    stretchNew = true;
    lastVelocity = juce::jlimit (1, 127, juce::roundToInt (velocity * 127.0f));
    noteHeld = true;

    // Per-voice LFOs restart with each articulated note.
    for (int lfo = 0; lfo < VoiceParams::numLfos; ++lfo)
    {
        lfoPhases[lfo] = (double) juce::jlimit (0.0f, 1.0f, params.lfos[lfo].startPhase);
        // LFO 5-16 use their own generator so the voice's random sequence
        // (pans, drift, noise) stays as it was before the LFO pool.
        auto& lfoRandom = lfo < 4 ? random : lfoPoolRandom;
        lfoHolds[lfo] = lfoRandom.nextFloat() * 2.0f - 1.0f;
        lfoChaos[lfo].reset (lfoRandom);
        lfoChaos[lfo].resetPhysics (params.lfos[lfo].shape, params.lfos[lfo].physA);
        if (params.lfos[lfo].shape == LfoShapes::Pendulum && params.lfos[lfo].kick)
            lfoChaos[lfo].kick (velocity);

        // M8.1: own seeds, so the voice's random sequence is untouched.
        const auto& sim = params.lfos[lfo].sim;
        lfoSmoothers[lfo].reset();
        lfoSeenTriggers[lfo] = params.lfos[lfo].triggerCount;
        if (LfoSimShapes::isSim (sim.shape))
        {
            lfoSims[lfo].reset (sim, ++lfoSimNotes * 7919u + (std::uint32_t) midiNoteNumber * 131u
                                         + (std::uint32_t) (velocity * 1000.0f) + (std::uint32_t) lfo * 104729u);
            if (sim.shape == LfoSimShapes::Pendulum && params.lfos[lfo].kick)
                lfoSims[lfo].trigger (sim, 0, true, velocity);
            float a = 0.0f, b = 0.0f;
            lfoSims[lfo].next (sim, 0.0, a, b);
            lfoValues[lfo] = a;
            lfoValuesB[lfo] = b;
        }
    }

    if (keepRunning)
    {
        // Mono retrigger: restart envelopes from their current level and keep
        // oscillator phases and filter state so there is no click.
        ampEnv.retrigger();
        filterEnv.retrigger();
        filter2Env.retrigger();
        modEnv.retrigger();
        env4.retrigger();
        for (auto& env : extraEnvs)
            env.retrigger();
        envMseg.reset();
        return;
    }

    randomValue = random.nextFloat() * 2.0f - 1.0f;
    voicePan = random.nextFloat() * 2.0f - 1.0f;

    for (int u = 0; u < VoiceParams::maxUnison; ++u)
    {
        // The first eight keep their original spacing (eighths of a cycle) so
        // existing patches start the same; voices 9-16 sit in between.
        const auto basePhase = u < 8 ? (double) u / 8.0 : (double) (u - 8) / 8.0 + 1.0 / 16.0;
        const auto jitter = (double) random.nextFloat() * (double) params.unisonRandom;

        auto phase = basePhase + jitter;
        phase -= std::floor (phase);

        for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
            oscBank[osc].resetPhase (u, phase);
    }

    for (auto& bank : oscBank)
        bank.switchTableHard();

    resonatorL.reset();
    resonatorR.reset();
    materialBodyL.reset();
    materialBodyR.reset();
    // M8.3: the gate is struck by the note (a vactrol keeps its state: a
    // new strike on a still-lit cell starts from where it is).
    westStrikeRemaining = params.west.on ? juce::jmax (1, (int) (WestCoastTuning::get().strikeSeconds * sampleRate)) : 0;
    bodyStrikePending = true;
    bodyTailSamplesRemaining = legacyMaterialBody()
                                   ? (int) (sampleRate * juce::jmin (10.0,
                                       1.5 * (0.08 + 7.92 * (double) params.resonatorDecay * (double) params.resonatorDecay)))
                                   : 0;

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        const auto& settings = params.oscillators[osc];

        if (! settings.stringMode || ! params.oscillatorEnabled[osc])
            continue;

        const auto pitch = settings.tuneMode == OscTuning::Semitones
                               ? currentFrequency
                                     * std::exp2 ((settings.semitones + (osc == 2 ? params.subOctaveOffset : 0)
                                                   + settings.cents / 100.0) / 12.0)
                               : oscFrequencyFactor (settings);

        // A stereo string route: the twin (or each unison string) starts on its own side of it.
        const auto unison = settings.stringExcite == (int) KarplusStrong::Excite::ModalPiano
                                ? 1 : juce::jlimit (1, VoiceParams::maxBufferedUnison, settings.unison);
        stringTwin[osc] = settings.stringStereo && unison == 1;
        twinAsleep[osc] = stringTwin[osc] && ! disableTwinSleep && stereoStringAtRest (osc);
        const auto count = stringTwin[osc] ? 2 : unison;
        if (stringTwin[osc])
            stringFor (osc, 1).matchNoise (stringFor (osc, 0));

        for (int u = 0; u < VoiceParams::maxBufferedUnison; ++u)
        {
            auto& string = stringFor (osc, u);
            string.setFrequency (pitch);
            configureString (string, settings, u < count ? stringSide (osc, u, count) : 0.0f);
            string.trigger (velocity);
        }
    }

    // Step 37: each oscillator's round-robin alternate, picked once per note.
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        const auto mode = params.oscillators[osc].sampleRoundRobin;
        zonePick[osc] = mode == 1 && params.roundRobin != nullptr ? params.roundRobin->next (osc, midiNoteNumber)
                        : mode == 2                                ? (unsigned) zoneRandom.nextInt (1 << 30)
                                                                   : 0u;
    }

    syncSamplePlayers();

    const auto bufferedCount = [] (int unison) { return juce::jlimit (1, VoiceParams::maxBufferedUnison, unison); };

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        const auto& settings = params.oscillators[osc];

        if (settings.sampleMode && params.oscillatorEnabled[osc] && settings.sample != nullptr)
            for (int u = 0; u < bufferedCount (settings.unison); ++u)
            {
                // Step 35: its own generator, drawn only while a random start
                // is set, so older patches' random sequences don't shift.
                const auto startUnit = settings.sampleStartRandom > 0.0f ? sampleStartRandom.nextFloat() : 0.0f;
                sampleUnison[osc][u].trigger (startUnit);
                sampleLayer[osc][u].trigger (startUnit); // step 37: the blended layer starts with it
                // Live grains (ilanaSynth FX) draw their seeds from their own
                // seeded generator, so an input renders the same every time;
                // sample grains stay free, as the plugin's analog randomness is.
                grains[osc][u].reset ((juce::uint32) (midiNoteNumber * 7919 + u * 104729
                                                      + (settings.grainLive ? liveGrainRandom.nextInt() : random.nextInt())));
            }

        lastStringMode[osc] = settings.stringMode;
        lastSampleMode[osc] = settings.sampleMode;
        previousOsc[osc] = 0.0f;
    }

    driftValue = driftRandom.nextFloat() * 2.0f - 1.0f;
    driftTarget = driftValue;

    for (auto* filter : { &filter1L, &filter1R, &filter2L, &filter2R,
                          &bothFilter1L, &bothFilter1R, &bothFilter2L, &bothFilter2R })
        filter->reset();
    filter1Linked = filter2Linked = true;
    filter1Open = filter2Open = false;
    filter1Fade = 0;
    filter1Ran = false;
    filter2Fade = 0;
    filter2Ran = false;
    openFilter1L.reset();
    openFilter1R.reset();
    openFilter2L.reset();
    openFilter2R.reset();

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        frameSmooth[osc].setCurrentAndTargetValue (params.oscillators[osc].frame);
        levelSmooth[osc].setCurrentAndTargetValue (params.oscillators[osc].level);
        oscEnableSmooth[osc].setCurrentAndTargetValue (params.oscillatorEnabled[osc] ? 1.0f : 0.0f);
    }
    noiseSmooth.setCurrentAndTargetValue (params.noiseLevel);
    noiseEnableSmooth.setCurrentAndTargetValue (params.noiseEnabled ? 1.0f : 0.0f);
    subOscColourSmooth.setCurrentAndTargetValue (params.subOscColour);
    noiseGenerator.reset();
    fmNoiseGenerator.reset();
    noiseLow = 0.0f;
    warpNoiseLow = 0.0f;
    subOscLevelSmooth.setCurrentAndTargetValue (params.subOscLevel);
    subOscEnableSmooth.setCurrentAndTargetValue (params.subOscEnabled ? 1.0f : 0.0f);

    ampEnv.noteOn();
    filterEnv.noteOn();
    filter2Env.noteOn();
    modEnv.noteOn();
    env4.noteOn();
    for (auto& env : extraEnvs)
        env.noteOn();

    envMseg.reset();
    msegEnvValue = 0.0f;
    fmNoiseState = 0.0f;

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        feedbackHistory[osc] = feedbackFiltered[osc] = 0.0f;

    // The Operator EG: gains for the oscillators whose ENVELOPE picks it.
    dx7Playing = params.operatorEgUsed;
    if (dx7Playing)
    {
        // Scaling follows the key moved by KEY OFFSET (a DX7 voice's
        // transpose; the oscillators get its pitch from their SEMI knobs).
        const auto& eg = params.operatorEg;
        const auto transposed = juce::jlimit (0, 127, midiNoteNumber + (int) eg[OperatorEg::keyOffsetByte] - 24);
        std::array<bool, 6> carriers {};
        for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
            carriers[(size_t) osc] = params.oscOut[osc] && params.oscillators[osc].ampEnv == OperatorEg::envelopeChoice;
        dx7Note.start (eg, transposed, lastVelocity, sampleRate, carriers);
        dx7Settings = eg;
        dx7Previous.fill (0.0f);
        dx7Current = dx7Note.getGains();
        dx7Count = 0;
    }
}

void Voice::resetForNewPatch()
{
    for (auto* filter : { &filter1L, &filter1R, &filter2L, &filter2R,
                          &bothFilter1L, &bothFilter1R, &bothFilter2L, &bothFilter2R })
        filter->reset();
    filter1Linked = filter2Linked = true;
    filter1Open = filter2Open = false;
    filter1Fade = 0;
    filter1Ran = false;
    filter2Fade = 0;
    filter2Ran = false;
    openFilter1L.reset();
    openFilter1R.reset();
    openFilter2L.reset();
    openFilter2R.reset();
    westGateL.reset();
    westGateR.reset();
    westFolder.reset();
    westStrikeRemaining = 0;
    resonatorL.reset();
    resonatorR.reset();
    materialBodyL.reset();
    materialBodyR.reset();
    bodyStrikePending = false;
    bodyTailSamplesRemaining = 0;

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        // A voice that has not played since its last reset still has silent
        // strings and samples: skip clearing their buffers, so a patch load
        // does not clear every voice's delay lines in one audio block.
        if (hasPlayedNote)
            for (int u = 0; u < VoiceParams::maxBufferedUnison; ++u)
            {
                stringFor (osc, u).reset();
                sampleUnison[osc][u].reset();
                sampleLayer[osc][u].reset();
            }
        feedbackHistory[osc] = feedbackFiltered[osc] = previousOsc[osc] = 0.0f;
    }
    fmNoiseState = 0.0f;

    // As a new instance: no note to glide from, and the seeded generators
    // start their sequences again (the analog ones stay free in the plugin,
    // as the constructor leaves them).
    if (fixedRandomSeeds().load())
    {
        random.setSeed (12345);
        driftRandom.setSeed (54321);
    }
    fmNoiseRandom.setSeed (31337);
    lfoPoolRandom.setSeed (27183);
    liveGrainRandom.setSeed (0x1f3a);
    zoneRandom.setSeed (0x2b17);
    sampleStartRandom.setSeed (0x5a17);
    lfoSimNotes = 0;
    driftValue = driftTarget = 0.0f;
    hasPlayedNote = false;

    if (shared != nullptr)
        shared->lastNoteFrequency = 0.0;
}

void Voice::stopNote (float, bool allowTailOff)
{
    // The synth hard-stops a voice before reusing it; a mono note change
    // keeps the voice sounding instead.
    if (! allowTailOff && monoPending && monoKeepRunning)
        return;

    // Already released: the synth stops every voice on a note when that note
    // starts again, released ones too. Restarting the release here kept a
    // looped clip's old voices alive whenever the release outlasted the loop.
    if (allowTailOff && ! noteHeld)
        return;

    noteHeld = false;

    if (allowTailOff)
    {
        // Step 33: a Sustain Loop leaves its loop now; a One Shot still
        // playing holds the envelopes' release until it ends.
        for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        {
            for (auto& player : sampleUnison[osc])
                player.release();
            for (auto& player : sampleLayer[osc])
                player.release();
        }
        if (oneShotPlaying())
        {
            oneShotHold = true;
            return;
        }
        releaseEnvelopes();
    }
    else
    {
        oneShotHold = false;
        bodyTailSamplesRemaining = 0;
        ampEnv.reset();
        filterEnv.reset();
        filter2Env.reset();
        modEnv.reset();
        env4.reset();
        for (auto& env : extraEnvs)
            env.reset();
        extraEnvValues.fill (0.0f); // the ENV page monitors these
        dx7Playing = false;
        lastAmpValue = 0.0f;
        lastLifetimeValue = 0.0f;
        lastFilterValue = 0.0f;
        lastModValue = 0.0f;
        clearCurrentNote();
    }
}

bool Voice::oneShotPlaying() const
{
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        // Granular reads the sample through its cloud, never through these
        // players, so they would never reach the end and hold the release.
        if (params.oscillators[osc].sampleMode && ! params.oscillators[osc].granularMode
            && params.oscillators[osc].samplePlayback == SamplePlayer::oneShot
            && params.oscillatorEnabled[osc])
            for (const auto& player : sampleUnison[osc])
                if (player.holdsRelease())
                    return true;
    return false;
}

void Voice::releaseEnvelopes()
{
    oneShotHold = false;
    {
        if (legacyMaterialBody())
            bodyTailSamplesRemaining = (int) (sampleRate * juce::jmin (10.0,
                1.5 * (0.08 + 7.92 * (double) params.resonatorDecay * (double) params.resonatorDecay)));
        if (dx7Playing)
            dx7Note.keyUp();
        ampEnv.noteOff();
        filterEnv.noteOff();
        filter2Env.noteOff();
        modEnv.noteOff();
        env4.noteOff();
        for (auto& env : extraEnvs)
            env.noteOff();
    }
}

void Voice::pitchWheelMoved (int newValue)
{
    // MPE: channel 1 is the master channel, its bend reaches every voice
    // through setMasterBend; a note on it has no member bend of its own.
    memberBend = params.mpe && playingChannel() == 1
                     ? 0.0
                     : ((double) newValue - 8192.0) / 8192.0 * (double) params.pitchBendRange;
    bendSemitones = memberBend + masterBend;
}

void Voice::controllerMoved (int controllerNumber, int newValue)
{
    const auto value = (float) newValue / 127.0f;

    if (controllerNumber == 1)
        modWheel = value;
    else if (controllerNumber == 11)
        expressionValue = value;
}

void Voice::channelPressureChanged (int newValue)
{
    aftertouchValue = (float) newValue / 127.0f;
}

void Voice::aftertouchChanged (int newValue)
{
    aftertouchValue = (float) newValue / 127.0f;
}

float Voice::voiceLfoValue (int lfo) const
{
    const auto& lfoParams = params.lfos[lfo];

    if (LfoSimShapes::isSim (lfoParams.shape))
        return lfoValues[lfo];

    if (LfoShapes::isStateful (lfoParams.shape))
        return lfoChaos[lfo].value (lfoParams.shape, lfoPhases[lfo]);

    return lfoShapeAt (lfoParams.shape, lfoPhases[lfo], lfoHolds[lfo], lfoParams.steps,
                       lfoParams.custom, lfoParams.customSize);
}

// An LFO is read at the control rate when only slow routes hear it (see
// evaluateModsRated) and its value is a function of its phase (and hold):
// then its shape is worked out only at the samples those routes are
// evaluated, while its phase still moves every sample. Vital runs an LFO at
// a control rate unless an audio-rate route needs it.
void Voice::classifySlowLfos()
{
    for (int lfo = 0; lfo < VoiceParams::numLfos; ++lfo)
    {
        const auto& settings = params.lfos[lfo];
        const auto shape = settings.shape;
        lfoSlow[lfo] = modSlotsPrepared && settings.perVoice && ! settings.needsB
                       && ! LfoSimShapes::isSim (shape) && ! LfoShapes::isStateful (shape) && ! LfoShapes::isPhysics (shape)
                       && shape != LfoShapes::Chaos && ! (lfoSmoothCoefficients[lfo] < 1.0f);
    }
    for (int k = 0; k < numGroupSlots[1]; ++k)
    {
        const auto& slot = params.modSlots[groupSlots[1][(size_t) k]];
        for (const auto source : { slot.source, slot.aux })
        {
            if (const auto lfo = Mod::lfoIndexFor (source); lfo >= 0)
                lfoSlow[lfo] = false;
            if (const auto lfo = Mod::lfoBIndexFor (source); lfo >= 0)
                lfoSlow[lfo] = false;
        }
    }
}

// Like the slow LFOs: an envelope (other than ENV 1) whose value only
// reaches what is read at the control points (slow mod routes, the
// filters' sub-block update) is worked out only there and just moves on in
// between. Not one that is an oscillator's amp or warp envelope, the WEST
// gate's source, a per-sample route's source, or (ENV 2 and 3) the filters'
// envelope while filter FM updates them every sample.
void Voice::classifySlowEnvelopes()
{
    const auto envelopeOf = [] (Mod::Source source)
    {
        switch (source)
        {
            case Mod::Source::FilterEnv:  return 1;
            case Mod::Source::FilterEnv2: return 2;
            case Mod::Source::ModEnv:     return 3;
            case Mod::Source::Env4:       return 4;
            default: break;
        }
        if (source >= Mod::Source::Env6 && source <= Mod::Source::Env16)
            return 5 + ((int) source - (int) Mod::Source::Env6);
        return -1;
    };

    for (auto& slow : envSlow)
        slow = modSlotsPrepared && ! disableSlowEnvelopes;
    envSlow[0] = envSlow[16] = false;
    if (modFilterFm)
        envSlow[1] = envSlow[2] = false;
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        if (! params.oscillatorEnabled[osc] && oscEnableSmooth[osc].getCurrentValue() <= 0.0005f)
            continue;
        const auto& settings = params.oscillators[osc];
        envSlow[(size_t) juce::jlimit (0, 16, settings.ampEnv)] = false;
        if (settings.pdEnv > 0)
            envSlow[(size_t) juce::jlimit (0, 16, settings.pdEnv - 1)] = false;
    }
    if (params.west.on && params.west.source != 0)
        if (const auto env = envelopeOf ((Mod::Source) params.west.source); env >= 0)
            envSlow[(size_t) env] = false;
    for (int k = 0; k < numGroupSlots[1]; ++k)
    {
        const auto& slot = params.modSlots[groupSlots[1][(size_t) k]];
        for (const auto source : { slot.source, slot.aux })
            if (const auto env = envelopeOf (source); env >= 0)
                envSlow[(size_t) env] = false;
    }
}

void Voice::advanceVoiceLfos (int sampleIndex)
{
    for (int index = 0; index < numPerVoiceLfos; ++index)
    {
        const auto lfo = perVoiceLfos[index];
        const auto shape = params.lfos[lfo].shape;

        if (LfoSimShapes::isSim (shape))
        {
            float a = 0.0f, b = 0.0f;
            lfoSims[lfo].next (params.lfos[lfo].sim, lfoIncrements[lfo], a, b);
            if (lfoSmoothCoefficients[lfo] < 1.0f)
                lfoSmoothers[lfo].process (a, b, lfoSmoothCoefficients[lfo]);
            lfoValues[lfo] = a;
            lfoValuesB[lfo] = b;
            auto next = lfoPhases[lfo] + lfoIncrements[lfo];
            lfoPhases[lfo] = next - std::floor (next);
            continue;
        }

        if (shape == LfoShapes::Chaos)
            lfoChaos[lfo].advance (lfoIncrements[lfo]);
        else if (LfoShapes::isPhysics (shape))
            lfoChaos[lfo].advancePhysics (shape, lfoIncrements[lfo], params.lfos[lfo].physA, params.lfos[lfo].physB);

        if (! lfoSlow[lfo] || (sampleIndex & (modControlInterval - 1)) == 0)
            lfoValues[lfo] = voiceLfoValue (lfo);

        if (lfoSmoothCoefficients[lfo] < 1.0f || params.lfos[lfo].needsB)
        {
            auto a = lfoValues[lfo];
            auto b = a;
            if (params.lfos[lfo].needsB && ! LfoShapes::isStateful (shape) && shape != LfoShapes::SampleHold)
                b = lfoShapeAt (shape, lfoPhases[lfo] + 0.25 - std::floor (lfoPhases[lfo] + 0.25), lfoHolds[lfo],
                                params.lfos[lfo].steps, params.lfos[lfo].custom, params.lfos[lfo].customSize);
            if (lfoSmoothCoefficients[lfo] < 1.0f)
                lfoSmoothers[lfo].process (a, b, lfoSmoothCoefficients[lfo]);
            lfoValues[lfo] = a;
            lfoValuesB[lfo] = b;
        }

        auto next = lfoPhases[lfo] + lfoIncrements[lfo];

        if (next >= 1.0)
        {
            next -= std::floor (next);
            auto& lfoRandom = lfo < 4 ? random : lfoPoolRandom;
            lfoHolds[lfo] = lfoRandom.nextFloat() * 2.0f - 1.0f;

            if (LfoShapes::isStateful (shape) && ! LfoShapes::isPhysics (shape))
                lfoChaos[lfo].onCycle (shape, lfoRandom);
        }

        lfoPhases[lfo] = next;
    }
}

void Voice::reExcite (float level)
{
    if (! isVoiceActive() || ! isKeyDown())
        return;

    // A struck string sounds again: a sleeping held note wakes.
    heldAsleep = false;
    heldSilentSamples = 0;
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        if (params.oscillators[osc].stringMode && params.oscillatorEnabled[osc])
            for (int u = 0; u < stringsPlaying (osc); ++u)
                stringFor (osc, u).trigger (juce::jlimit (0.0f, 1.0f, level * velocityLevel * 1.25f));
}

void Voice::evaluateMods (float* mods, int sampleIndex, float ampValue, float filterValue,
                          float filter2Value, float modValue, float env4Value) const
{
    for (int d = 0; d < params.numActiveDestinations; ++d)
        mods[params.activeDestinations[d]] = 0.0f;
    mods[(int) Mod::Destination::Count] = mods[(int) Mod::Destination::Count + 1] = 0.0f;

    if (params.anyExtendedFmMods)
        fmCellMods.fill (0.0f);
    if (params.anySpectralMods)
        spectralMods.fill (0.0f);

    if (modSlotsPrepared)
    {
        for (int s = 0; s < params.numModSlots; ++s)
        {
            const auto targetIndex = slotTargets[(size_t) s];
            if (targetIndex == -1)
                continue;
            auto* target = targetIndex >= 0 ? &mods[targetIndex]
                           : targetIndex <= -1000 ? &spectralMods[(size_t) (-1000 - targetIndex)]
                                                  : &fmCellMods[(size_t) (-2 - targetIndex)];

            if (slotHeld[(size_t) s])
            {
                *target += slotAmounts[(size_t) s];
                continue;
            }

            const auto& slot = params.modSlots[s];
            auto value = Mod::shape (slot, sourceValue (slot.source, sampleIndex, ampValue, filterValue,
                                                        filter2Value, modValue, env4Value));

            if (slot.aux != Mod::Source::None)
                value *= Mod::auxScale (slot.aux, sourceValue (slot.aux, sampleIndex, ampValue, filterValue,
                                                                filter2Value, modValue, env4Value));

            *target += slot.depth * value;
        }

        return;
    }

    for (int s = 0; s < params.numModSlots; ++s)
    {
        const auto& slot = params.modSlots[s];
        auto* target = Mod::isExplicitDestination (slot.destination) ? &mods[slot.destination] : nullptr;

        if (const auto stereoTarget = Mod::stereoTargetFor (slot); stereoTarget >= 0)
            target = &mods[stereoTarget];

        if (target == nullptr && params.anyExtendedFmMods)
            if (const auto cell = Mod::extendedFmCellFor (slot.destination); cell >= 0)
                target = &fmCellMods[(size_t) cell];

        if (target == nullptr && params.anySpectralMods && Mod::isVoiceSpectralSlot (slot))
            target = &spectralMods[(size_t) Mod::spectralOscFor (slot.destination)];

        if (target == nullptr)
            continue;

        auto value = Mod::shape (slot, sourceValue (slot.source, sampleIndex, ampValue, filterValue,
                                                    filter2Value, modValue, env4Value));

        if (slot.aux != Mod::Source::None)
            value *= Mod::auxScale (slot.aux, sourceValue (slot.aux, sampleIndex, ampValue, filterValue,
                                                            filter2Value, modValue, env4Value));

        *target += slot.depth * value;
    }
}

// What the per-sample loops read every sample (the rest, pitch included, is
// read once a sub-block by updateSubBlock); with filter FM on,
// updateFilterCoefficients reads the filters' settings every sample too.
bool Voice::isPerSampleDestination (int destination, bool filterFm)
{
    using D = Mod::Destination;
    if (destination >= (int) D::Count) // the stereo cutoffs follow the cutoffs
        return filterFm;
    switch ((D) destination)
    {
        case D::Filter1Cutoff: case D::Filter1Env: case D::Filter1Reso: case D::Filter1Morph:
        case D::Filter2Cutoff: case D::Filter2Env: case D::Filter2Reso: case D::Filter2Morph:
            return filterFm;
        case D::FmAmount: case D::FmFeedback: case D::RingMod:
        case D::Fm1to2: case D::Fm1to3: case D::Fm2to3: case D::Fm3to1:
        case D::Fm3to2: case D::Fm2Feedback: case D::Fm3Feedback:
        case D::Filter1Fm: case D::Filter2Fm:
        case D::Osc1Frame: case D::Osc2Frame: case D::SubFrame:
        case D::Osc4Frame: case D::Osc5Frame: case D::Osc6Frame:
        case D::AmpLevel: case D::Osc1Level: case D::Osc2Level: case D::SubLevel:
        case D::Osc4Level: case D::Osc5Level: case D::Osc6Level: case D::NoiseLevel:
            return true;
        default:
            return false;
    }
}

// One group's slots (prepareModSlots), in slot order: evaluateMods' sums
// for those destinations, to the bit.
void Voice::evaluateModGroup (int group, float* mods, int sampleIndex, float ampValue, float filterValue,
                              float filter2Value, float modValue, float env4Value) const
{
    const auto& destinations = groupDestinations[(size_t) group];
    for (int d = 0; d < numGroupDestinations[(size_t) group]; ++d)
        mods[destinations[(size_t) d]] = 0.0f;

    if (group == 1 && params.anyExtendedFmMods)
        fmCellMods.fill (0.0f);

    const auto& slots = groupSlots[(size_t) group];
    for (int k = 0; k < numGroupSlots[(size_t) group]; ++k)
    {
        const auto s = (size_t) slots[(size_t) k];
        const auto targetIndex = slotTargets[s];
        auto* target = targetIndex >= 0 ? &mods[targetIndex] : &fmCellMods[(size_t) (-2 - targetIndex)];

        if (slotHeld[s])
        {
            *target += slotAmounts[s];
            continue;
        }

        const auto& slot = params.modSlots[s];
        auto value = Mod::shape (slot, sourceValue (slot.source, sampleIndex, ampValue, filterValue,
                                                    filter2Value, modValue, env4Value));

        if (slot.aux != Mod::Source::None)
            value *= Mod::auxScale (slot.aux, sourceValue (slot.aux, sampleIndex, ampValue, filterValue,
                                                            filter2Value, modValue, env4Value));

        *target += slot.depth * value;
    }
}

void Voice::evaluateModsRated (float* mods, int sampleIndex, float ampValue, float filterValue,
                               float filter2Value, float modValue, float env4Value)
{
    if (! modSlotsPrepared)
    {
        evaluateMods (mods, sampleIndex, ampValue, filterValue, filter2Value, modValue, env4Value);
        return;
    }

    // Group 0, the routes read once a sub-block (updateSubBlock, at the same
    // samples) or only held sources: worked out only there, holding in
    // between, so the same values where they are read. Group 1, moving
    // sources into what is read every sample: every sample.
    if ((sampleIndex & (modControlInterval - 1)) == 0)
        evaluateModGroup (0, mods, sampleIndex, ampValue, filterValue, filter2Value, modValue, env4Value);
    if (numGroupSlots[1] > 0)
        evaluateModGroup (1, mods, sampleIndex, ampValue, filterValue, filter2Value, modValue, env4Value);
}

void Voice::prepareModSlots()
{
    stereoRouted = {};

    // Set only from MIDI and the block's parameters, never inside a render.
    const auto held = [] (Mod::Source source)
    {
        switch (source)
        {
            case Mod::Source::None:
            case Mod::Source::Velocity:
            case Mod::Source::KeyTrack:
            case Mod::Source::Random:
            case Mod::Source::ModWheel:
            case Mod::Source::Aftertouch:
            case Mod::Source::Expression:
            case Mod::Source::Macro1:
            case Mod::Source::Macro2:
            case Mod::Source::Macro3:
            case Mod::Source::Macro4:
            case Mod::Source::Macro5:
            case Mod::Source::Macro6:
            case Mod::Source::Macro7:
            case Mod::Source::Macro8:
            case Mod::Source::VectorX:
            case Mod::Source::VectorY:
                return true;
            default:
                return false;
        }
    };

    for (int s = 0; s < params.numModSlots; ++s)
    {
        const auto& slot = params.modSlots[s];
        auto targetIndex = Mod::isExplicitDestination (slot.destination) ? slot.destination : -1;

        if (targetIndex == -1 && params.anyExtendedFmMods)
            if (const auto cell = Mod::extendedFmCellFor (slot.destination); cell >= 0)
                targetIndex = -2 - cell;

        if (targetIndex == -1 && params.anySpectralMods && Mod::isVoiceSpectralSlot (slot))
            targetIndex = -1000 - Mod::spectralOscFor (slot.destination);

        // A stereo route into a filter cutoff: summed apart from the cutoff itself.
        if (const auto stereoTarget = Mod::stereoTargetFor (slot); stereoTarget >= 0)
        {
            targetIndex = stereoTarget;
            stereoRouted[(size_t) (stereoTarget - (int) Mod::Destination::Count)] = true;
        }

        slotTargets[(size_t) s] = targetIndex;
        slotHeld[(size_t) s] = held (slot.source) && held (slot.aux);

        if (slotHeld[(size_t) s] && targetIndex != -1)
        {
            auto value = Mod::shape (slot, sourceValue (slot.source, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));

            if (slot.aux != Mod::Source::None)
                value *= Mod::auxScale (slot.aux, sourceValue (slot.aux, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));

            slotAmounts[(size_t) s] = slot.depth * value;
        }
    }

    // Each routed destination's group (see evaluateModsRated): per sample
    // when it is read every sample and a moving source feeds it; the OSC 4-6
    // FM cells always.
    std::array<bool, (size_t) Mod::Destination::Count + Mod::numStereoDestinations> moving {};
    auto filterFm = params.filter1Fm != 0.0f || params.filter2Fm != 0.0f;
    for (int s = 0; s < params.numModSlots; ++s)
    {
        const auto targetIndex = slotTargets[(size_t) s];
        if (targetIndex == (int) Mod::Destination::Filter1Fm || targetIndex == (int) Mod::Destination::Filter2Fm)
            filterFm = true;
        if (targetIndex >= 0 && ! slotHeld[(size_t) s])
            moving[(size_t) targetIndex] = true;
    }
    modFilterFm = filterFm;
    const auto groupOf = [&] (int targetIndex)
    {
        return (targetIndex < -1 && targetIndex > -1000) || (moving[(size_t) targetIndex] && isPerSampleDestination (targetIndex, filterFm)) ? 1 : 0;
    };

    numGroupSlots = {};
    for (int s = 0; s < params.numModSlots; ++s)
    {
        const auto targetIndex = slotTargets[(size_t) s];
        if (targetIndex == -1 || targetIndex <= -1000) // (spectral routes: read once a block, not in a group)
            continue;
        const auto group = (size_t) groupOf (targetIndex);
        groupSlots[group][(size_t) numGroupSlots[group]++] = s;
    }
    numGroupDestinations = {};
    for (int d = 0; d < params.numActiveDestinations; ++d)
    {
        const auto destination = params.activeDestinations[d];
        const auto group = (size_t) groupOf (destination);
        groupDestinations[group][(size_t) numGroupDestinations[group]++] = destination;
    }
    for (int f = 0; f < Mod::numStereoDestinations; ++f)
        if (stereoRouted[(size_t) f])
        {
            const auto destination = (int) Mod::Destination::Count + f;
            const auto group = (size_t) groupOf (destination);
            groupDestinations[group][(size_t) numGroupDestinations[group]++] = destination;
        }

    modSlotsPrepared = true;
}

void Voice::updateUnisonLayout()
{
    const auto layout = [] (const VoiceParams::OscParams& osc, int numUnison, int chord, float detuneCents,
                            float blend, double* offsets, double* ratios, float* gains)
    {
        auto power = 0.0f;
        const auto centreLow = (numUnison - 1) / 2;
        const auto centreHigh = numUnison / 2;

        for (int u = 0; u < numUnison; ++u)
        {
            const auto position = numUnison > 1
                                      ? juce::jlimit (-1.0f, 1.0f, (float) u / (float) (numUnison - 1) * 2.0f - 1.0f)
                                      : 0.0f;

            offsets[u] = (double) chordInterval (chord, u)
                         + (double) unisonStackInterval (osc.unisonMode, u, numUnison)
                         + unisonDetunePosition (osc.unisonMode, position) * (double) detuneCents / 100.0;

            const auto isCentre = u == centreLow || u == centreHigh;
            gains[u] = isCentre ? 1.0f : juce::jlimit (0.0f, 1.0f, blend);
            power += gains[u] * gains[u];
        }

        const auto norm = 1.0f / std::sqrt (juce::jmax (1.0e-6f, power));

        for (int u = 0; u < numUnison; ++u)
        {
            gains[u] *= norm;
            ratios[u] = std::exp2 (offsets[u] / 12.0);
        }
    };

    constexpr Mod::Destination detuneDestinations[] { Mod::Destination::Osc1Detune,
                                                       Mod::Destination::Osc2Detune,
                                                       Mod::Destination::SubDetune,
                                                       Mod::Destination::Osc4Detune, Mod::Destination::Osc5Detune,
                                                       Mod::Destination::Osc6Detune };
    constexpr Mod::Destination blendDestinations[] { Mod::Destination::Osc1Blend,
                                                      Mod::Destination::Osc2Blend,
                                                      Mod::Destination::SubBlend,
                                                      Mod::Destination::Osc4Blend, Mod::Destination::Osc5Blend,
                                                      Mod::Destination::Osc6Blend };

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        if (osc >= 3 && numOscUnison[osc] == 0)
            continue;
        const auto& settings = params.oscillators[osc];
        layout (settings, numOscUnison[osc], settings.chord,
                juce::jlimit (0.0f, 200.0f, settings.detuneCents + blockMod (detuneDestinations[osc]) * Mod::Range::detuneCents),
                settings.unisonBlend + blockMod (blendDestinations[osc]),
                unisonOffset[osc], unisonRatio[osc], unisonGains[osc]);
    }
}

void Voice::renderNextBlock (juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples)
{
    renderStart = startSample;
    if (! hasActiveAmpEnvelope())
    {
        lastAmpValue = 0.0f;
        lastLifetimeValue = 0.0f;
        clearCurrentNote();
        return;
    }

    // Held sleep: nothing this note plays can sound again until the key
    // (or pedal) comes up; then it carries on from where it stopped.
    if (heldAsleep)
    {
        if (noteHeld && ! monoPending && fadeTotal == 0 && heldSleepAllowed() && ! filterSettingsMoved()
            && enabledOscillators() == sleptOscillators)
            return;
        heldAsleep = false;
        heldSilentSamples = 0;
    }

    sounded = true;

    // Step 33: the One Shot that held the release has played out.
    if (oneShotHold && ! oneShotPlaying())
        releaseEnvelopes();

    // Block-rate modulation first: everything below can use it. The
    // per-sample values only clear destinations that are routed, so wipe
    // them all here too: a routing removed since the last block must not
    // leave its final value stuck on the target.
    std::fill (blockMods.begin(), blockMods.end(), 0.0f);
    std::fill (sampleMods.begin(), sampleMods.end(), 0.0f);

    // (MIDI splits a render, so the held sources are fixed within this one.)
    prepareModSlots();

    if (params.numModSlots > 0)
    {
        for (int lfo = 0; lfo < VoiceParams::numLfos; ++lfo)
            if (params.lfos[lfo].perVoice)
                lfoValues[lfo] = voiceLfoValue (lfo);

        evaluateMods (blockMods.data(), 0, lastAmpValue, lastFilterValue, lastFilter2Value, lastModValue, lastEnv4Value);
    }

    // Per-note routes into the spectral amounts: each voice's own value for the block.
    spectralBlock = params.anySpectralMods && params.numModSlots > 0 ? spectralMods : std::array<float, 6> {};

    const auto scaleTime = [] (float seconds, float mod, float octaves)
    {
        const auto clamp = Mod::Range::timeClampOctaves;
        return mod != 0.0f ? seconds * std::exp2 (juce::jlimit (-clamp, clamp, mod * octaves)) : seconds;
    };

    const auto modulatedEnvelope = [&scaleTime] (TensionAdsr::Parameters env, float a, float d, float s, float r)
    {
        env.attack = scaleTime (env.attack, a, Mod::Range::attackOctaves);
        env.decay = scaleTime (env.decay, d, Mod::Range::decayOctaves);
        env.sustain = juce::jlimit (0.0f, 1.0f, env.sustain + s);
        env.release = scaleTime (env.release, r, Mod::Range::releaseOctaves);
        return env;
    };

    // Rate key scaling (M5): every stage time shrinks up the keyboard, as a
    // struck or plucked note dies faster the higher it is.
    const auto keyScaled = [this] (TensionAdsr::Parameters env, int index)
    {
        const auto rate = params.envKeyRate[(size_t) index];

        if (rate == 0.0f)
            return env;

        const auto factor = std::exp2 (-rate * keyTrackOctaves);
        env.attack *= factor;
        env.decay *= factor;
        env.release *= factor;
        env.delay *= factor;
        env.hold *= factor;
        return env;
    };

    using D = Mod::Destination;
    ampEnv.setParameters (keyScaled (modulatedEnvelope (params.ampEnv, blockMod (D::AmpAttack), blockMod (D::AmpDecay),
                                                        blockMod (D::AmpSustain), blockMod (D::AmpRelease)), 0));
    filterEnv.setParameters (keyScaled (modulatedEnvelope (params.filterEnv, blockMod (D::FeAttack), blockMod (D::FeDecay),
                                                           blockMod (D::FeSustain), blockMod (D::FeRelease)), 1));
    filter2Env.setParameters (keyScaled (modulatedEnvelope (params.filter2Env, blockMod (D::F2eAttack), blockMod (D::F2eDecay),
                                                            blockMod (D::F2eSustain), blockMod (D::F2eRelease)), 2));
    modEnv.setParameters (keyScaled (modulatedEnvelope (params.modEnv, blockMod (D::MeAttack), blockMod (D::MeDecay),
                                                        blockMod (D::MeSustain), blockMod (D::MeRelease)), 3));
    env4.setParameters (keyScaled (modulatedEnvelope (params.env4, blockMod (D::E4Attack), blockMod (D::E4Decay),
                                                      blockMod (D::E4Sustain), blockMod (D::E4Release)), 4));
    numNeededExtraEnvs = 0;
    for (int env = 0; env < (int) extraEnvs.size(); ++env)
        if (params.extraEnvNeeded[(size_t) env])
        {
            extraEnvs[(size_t) env].setParameters (keyScaled (params.extraEnvs[(size_t) env], env + 5));
            neededExtraEnvs[numNeededExtraEnvs++] = env;
        }

    numPerVoiceLfos = 0;
    for (int lfo = 0; lfo < VoiceParams::numLfos; ++lfo)
        if (params.lfos[lfo].perVoice)
            perVoiceLfos[numPerVoiceLfos++] = lfo;

    if (params.msegEnvNeeded)
        envMseg.setParams (params.msegShape.levels, params.msegShape.times,
                           params.msegShape.rateHz, params.msegShape.loop);

    for (int lfo = 0; lfo < VoiceParams::numLfos; ++lfo)
    {
        lfoIncrements[lfo] = params.lfos[lfo].baseIncrement
                             * std::exp2 ((double) blockMod (Mod::lfoRateDestinationFor (lfo)) * (double) Mod::Range::lfoRateOctaves);

        // Key tracked: RATE 4 Hz runs at the note's own pitch, 8 Hz an
        // octave above, 2 Hz an octave below.
        if (params.lfos[lfo].keyTrack)
            lfoIncrements[lfo] = juce::jmin (0.45, lfoIncrements[lfo] * currentFrequency / 4.0);

        // M8.1: SMOOTH, and the triggers counted by the processor.
        const auto& lfoSettings = params.lfos[lfo];
        lfoSmoothCoefficients[lfo] = LfoSmoother::coefficientFor (lfoSettings.smooth, lfoIncrements[lfo] * sampleRate, sampleRate);
        if (lfoSettings.triggerCount != lfoSeenTriggers[lfo])
        {
            lfoSeenTriggers[lfo] = lfoSettings.triggerCount;
            if (lfoSettings.perVoice && LfoSimShapes::isSim (lfoSettings.shape))
                lfoSims[lfo].trigger (lfoSettings.sim, ++lfoSimNotes * 7919u + (std::uint32_t) lfo);
        }
        lfoSims[lfo].sampleRate = sampleRate;
    }

    // Level key scaling: KEY LVL 1 is +6 dB per octave above C3 (and -6 dB
    // per octave below); negative tilts the other way. Modulating operators
    // use it to keep FM brightness even across the keyboard. Per block, so
    // turning or modulating it reaches held notes.
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        const auto keyLevel = params.oscillators[osc].keyLevel;
        keyLevelGain[osc] = keyLevel != 0.0f
                                ? juce::jlimit (0.0f, 4.0f, juce::Decibels::decibelsToGain (keyLevel * 6.0f * keyTrackOctaves, -120.0f))
                                : 1.0f;
    }

    if (params.west.on)
    {
        westFolder.setParams (params.west.fold, params.west.symmetry, params.west.stages);
    }

    glideCoeff = params.glideTime > 0.001f
                     ? 1.0f - std::exp (-1.0f / (float) (params.glideTime * sampleRate))
                     : 1.0f;

    const auto unisonLimit = [this] (const VoiceParams::OscParams& osc)
    {
        const auto normalLimit = (osc.stringMode || osc.sampleMode) ? VoiceParams::maxBufferedUnison
                                                                    : VoiceParams::maxUnison;
        return params.quality == 0 ? juce::jmin (4, normalLimit) : normalLimit;
    };

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        const auto& settings = params.oscillators[osc];
        numOscUnison[osc] = osc >= 3 && ! params.oscillatorEnabled[osc]
                                  && oscEnableSmooth[osc].getCurrentValue() <= 0.0005f
                                ? 0 : juce::jlimit (1, unisonLimit (settings), settings.unison);

        // A grand's lowest notes have one string each, the low bass two,
        // and three start around the tenor: with the register map on, a
        // hammered note uses no more strings than that (detuned unison
        // strings in the bass beat audibly, which a real one cannot).
        if (settings.stringMode && settings.registerMap > 0.0f
            && (settings.stringExcite == (int) KarplusStrong::Excite::Hammer || settings.stringExcite == (int) KarplusStrong::Excite::Piano)
            && numOscUnison[osc] > 1)
        {
            const auto note = getCurrentlyPlayingNote();
            numOscUnison[osc] = juce::jmin (numOscUnison[osc], note < 35 ? 1 : (note < 47 ? 2 : 3));
        }
        // The Modal Piano's recording already has all of a note's strings.
        if (settings.stringMode && settings.stringExcite == (int) KarplusStrong::Excite::ModalPiano)
            numOscUnison[osc] = juce::jmin (numOscUnison[osc], 1);

        // A stereo string route on one string: a second plays its right side.
        const auto twin = settings.stringMode && settings.stringStereo && numOscUnison[osc] == 1;
        if (twin && ! stringTwin[osc] && settings.stringMode == lastStringMode[osc])
        {
            // (A route added mid-note: the twin picks up where the string is.)
            stringFor (osc, 1) = stringFor (osc, 0);
        }
        if (! twin)
            twinAsleep[osc] = false;
        stringTwin[osc] = twin;
    }

    updateUnisonLayout();

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        if (params.oscillatorEnabled[osc] || oscEnableSmooth[osc].getCurrentValue() > 0.0005f)
        {
            const auto& settings = params.oscillators[osc];
            const auto* table = settings.table;

            // A per-note route into SPEC AMT: this voice's own amount picks the table.
            if (params.spectralCache != nullptr && spectralBlock[(size_t) osc] != 0.0f && settings.spectralMode > 0)
                table = params.spectralCache->getForVoice (settings.spectralTable, settings.spectralMode,
                                                           juce::jlimit (0.0f, 1.0f, settings.spectralAmount + spectralBlock[(size_t) osc]),
                                                           settings.baseTable, table);

            // A warped copy fades from the last one (UnisonBank::setWavetable).
            oscBank[osc].setWavetable (table, table != settings.baseTable);
        }
        else if (oscBank[osc].getWavetable() != nullptr)
        {
            // Not rendering: a spectral copy this bank holds (or fades from)
            // can be freed before it plays again (SpectralCache keeps a
            // replaced copy 3 s), and its pitch updates still read the table.
            // It lets go, and takes its next table as it is, without a fade.
            oscBank[osc].setWavetable (nullptr);
        }

    const auto configureStrings = [this] (int osc)
    {
        const auto& settings = params.oscillators[osc];

        const auto count = stringsPlaying (osc);
        if (twinAsleep[osc] && stringTwin[osc] && ! stereoStringAtRest (osc))
        {
            // The route moves: the twin wakes where string 0 is.
            stringFor (osc, 1) = stringFor (osc, 0);
            twinAsleep[osc] = false;
        }
        for (int u = 0; u < (stringTwin[osc] && twinAsleep[osc] ? 1 : count); ++u)
        {
            configureString (stringFor (osc, u), settings, stringSide (osc, u, count));
        }
    };

    // Keep OSC 3's original transition order until the behaviour-preserving
    // checkpoint; its string transition is recorded before sample sync.
    configureStrings (2);

    if (params.oscillators[2].stringMode && ! lastStringMode[2] && params.oscillatorEnabled[2])
        for (int u = 0; u < stringsPlaying (2); ++u)
            stringFor (2, u).trigger (velocityLevel);

    lastStringMode[2] = params.oscillators[2].stringMode;

    const auto panMod = juce::jlimit (-1.0f, 1.0f, blockMod (D::Pan));

    const auto computePans = [this, panMod] (const VoiceParams::OscParams& osc, int numUnison, float oscPanMod,
                                             float spreadMod, float* gainsL, float* gainsR)
    {
        const auto spread = juce::jlimit (0.0f, 1.0f, osc.spread + spreadMod);

        for (int u = 0; u < numUnison; ++u)
        {
            const auto spreadOffset = numUnison > 1
                                          ? spread * ((float) u / (float) (numUnison - 1) * 2.0f - 1.0f)
                                          : 0.0f;
            const auto pan = juce::jlimit (-1.0f, 1.0f, osc.pan + oscPanMod + spreadOffset + panMod
                                                            + params.voiceSpread * voicePan);
            const auto angle = (pan * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi;
            gainsL[u] = std::cos (angle);
            gainsR[u] = std::sin (angle);
        }
    };

    constexpr D panDestinations[] { D::Osc1Pan, D::Osc2Pan, D::SubPan,
                                    D::Osc4Pan, D::Osc5Pan, D::Osc6Pan };
    constexpr D spreadDestinations[] { D::Osc1Spread, D::Osc2Spread, D::SubSpread,
                                       D::Osc4Spread, D::Osc5Spread, D::Osc6Spread };

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        if (osc < 3 || numOscUnison[osc] > 0)
            computePans (params.oscillators[osc], numOscUnison[osc],
                         blockMod (panDestinations[osc]) * Mod::Range::oscPan, blockMod (spreadDestinations[osc]),
                         panGainL[osc], panGainR[osc]);

    computePans (VoiceParams::OscParams {}, 1, 0.0f, 0.0f, &panGainSubOscL, &panGainSubOscR);

    // Each wavetable unison voice's blend gain and pan, folded per block.
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        oscBank[osc].setWeights (unisonGains[osc], panGainL[osc], panGainR[osc], numOscUnison[osc]);

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        frameSmooth[osc].setTargetValue (params.oscillators[osc].frame);
        levelSmooth[osc].setTargetValue (params.oscillators[osc].level);
        oscEnableSmooth[osc].setTargetValue (params.oscillatorEnabled[osc] ? 1.0f : 0.0f);
    }

    noiseSmooth.setTargetValue (params.noiseLevel);
    noiseEnableSmooth.setTargetValue (params.noiseEnabled ? 1.0f : 0.0f);
    subOscColourSmooth.setTargetValue (params.subOscColour);
    subOscLevelSmooth.setTargetValue (params.subOscLevel);
    subOscEnableSmooth.setTargetValue (params.subOscEnabled ? 1.0f : 0.0f);
    subOsc.setWavetable (params.subOscTable);

    bool modeChanged[VoiceParams::numOscillators] {};
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        modeChanged[osc] = params.oscillators[osc].stringMode != lastStringMode[osc]
                           || params.oscillators[osc].sampleMode != lastSampleMode[osc];

    syncSamplePlayers();

    const auto retriggerNewPlayers = [] (SamplePlayer* players, bool wasSampling, bool wasEnabled,
                                         int previousCount, int count)
    {
        count = juce::jmin (count, VoiceParams::maxBufferedUnison);

        for (int u = (! wasSampling || ! wasEnabled) ? 0 : previousCount; u < count; ++u)
            players[u].trigger();
    };

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        const auto& settings = params.oscillators[osc];
        const auto sampling = settings.sampleMode && params.oscillatorEnabled[osc] && settings.sample != nullptr;

        if (sampling)
        {
            retriggerNewPlayers (sampleUnison[osc], lastSampleMode[osc], lastEnabled[osc],
                                 lastUnison[osc], numOscUnison[osc]);
            retriggerNewPlayers (sampleLayer[osc], lastSampleMode[osc], lastEnabled[osc],
                                 lastUnison[osc], numOscUnison[osc]);
        }

        lastSampleMode[osc] = settings.sampleMode;
        lastEnabled[osc] = params.oscillatorEnabled[osc];
        lastUnison[osc] = numOscUnison[osc];
    }

    for (int osc = 0; osc < 2; ++osc)
        configureStrings (osc);

    if (params.oscillators[0].stringMode && ! lastStringMode[0])
        for (int u = 0; u < stringsPlaying (0); ++u)
            stringFor (0, u).trigger (velocityLevel);

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        if (modeChanged[osc])
        {
            oscEnableSmooth[osc].setCurrentAndTargetValue (0.0f);
            oscEnableSmooth[osc].setTargetValue (params.oscillatorEnabled[osc] ? 1.0f : 0.0f);
        }

    if (params.oscillators[1].stringMode && ! lastStringMode[1])
        for (int u = 0; u < stringsPlaying (1); ++u)
            stringFor (1, u).trigger (velocityLevel);

    for (int osc = 0; osc < 2; ++osc)
        lastStringMode[osc] = params.oscillators[osc].stringMode;

    for (int osc = 3; osc < VoiceParams::numOscillators; ++osc)
    {
        if (! params.oscillatorEnabled[osc])
            continue;

        configureStrings (osc);
        if (params.oscillators[osc].stringMode && ! lastStringMode[osc])
            for (int u = 0; u < stringsPlaying (osc); ++u)
                stringFor (osc, u).trigger (velocityLevel);
        lastStringMode[osc] = params.oscillators[osc].stringMode;
    }

    const auto resonatorAmount = juce::jlimit (0.0f, 1.0f, params.resonatorAmount + blockMod (D::ResAmount));
    const auto resonatorDecay = juce::jlimit (0.0f, 1.0f, params.resonatorDecay + blockMod (D::ResDecay));

    resonatorL.setParams (params.resonatorOn ? resonatorAmount : 0.0f, resonatorDecay, 0.35f);
    resonatorR.setParams (params.resonatorOn ? resonatorAmount : 0.0f, resonatorDecay, 0.35f);

    const auto driveAmount1 = juce::jlimit (1.0f, 10.0f, params.filter1.drive + blockMod (D::Filter1Drive) * driveRange);
    const auto driveAmount2 = juce::jlimit (1.0f, 10.0f, params.filter2.drive + blockMod (D::Filter2Drive) * driveRange);
    // 303 Acid and Moog Drive take the drive inside (into their feedback
    // loop's input); every other model is driven by a tanh in front.
    const auto inside1 = FilterType::drivesInside (params.filter1.type);
    const auto inside2 = FilterType::drivesInside (params.filter2.type);
    // A stereo route into a drive: the left side gets more, the right less (2026-10-30).
    const auto stereoDrive = [this] (float amount, int index)
    {
        const auto shift = stereoRouted[(size_t) index] ? blockMod ((Mod::Destination) ((int) Mod::Destination::Count + index)) * driveRange : 0.0f;
        return DriveAmounts { juce::jlimit (1.0f, 10.0f, amount + shift), juce::jlimit (1.0f, 10.0f, amount - shift) };
    };
    const auto drives1 = stereoDrive (driveAmount1, Mod::StereoIndex::drive1);
    const auto drives2 = stereoDrive (driveAmount2, Mod::StereoIndex::drive2);
    for (auto* filter : { &filter1L, &bothFilter1L })
        filter->setDrive (inside1 ? drives1.l : 1.0f);
    for (auto* filter : { &filter1R, &bothFilter1R })
        filter->setDrive (inside1 ? drives1.r : 1.0f);
    for (auto* filter : { &filter2L, &bothFilter2L })
        filter->setDrive (inside2 ? drives2.l : 1.0f);
    for (auto* filter : { &filter2R, &bothFilter2R })
        filter->setDrive (inside2 ? drives2.r : 1.0f);
    const auto drive1 = inside1 ? DriveAmounts {} : drives1;
    const auto drive2 = inside2 ? DriveAmounts {} : drives2;

    auto* left = outputBuffer.getWritePointer (0);
    auto* right = outputBuffer.getNumChannels() > 1 ? outputBuffer.getWritePointer (1) : nullptr;

    const auto ampVelScale = 1.0f - params.ampVelocity + params.ampVelocity * velocityLevel;
    // The Operator EG supplies its operators' gains, so the amp envelope steps aside.
    bool alternateAmpRouting = dx7Playing;
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        alternateAmpRouting = alternateAmpRouting
                              || (params.oscillatorEnabled[osc] && params.oscillators[osc].ampEnv != 0);

    // Only the modal body's tail left (the amp envelope is over, nothing
    // else rings it): render the body alone. Not with per-oscillator
    // envelopes or the Operator EG (their sources may still drive it) or a
    // live input ringing it.
    if (legacyMaterialBody() && bodyTailSamplesRemaining > 0
        && ! alternateAmpRouting && ! operatorEgOwnsVoice() && ! envelopesActive()
        && (params.liveInput == nullptr || params.inputToBody == 0.0f))
    {
        if (resonatorAmount > 0.001f)
            renderBodyTail (left, right, startSample, numSamples,
                            resonatorAmount * ampVelScale * juce::jlimit (0.0f, 2.0f, 1.0f + blockMod (D::AmpLevel)));
        lastAmpValue = 0.0f;
        lastLifetimeValue = 0.0f;
        bodyTailSamplesRemaining = juce::jmax (0, bodyTailSamplesRemaining - numSamples);
        if (fadeTotal > 0 && fadeRemaining == 0)
            stopNote (0.0f, false);
        if (! hasActiveAmpEnvelope())
            clearCurrentNote();
        return;
    }

    auto* mods = sampleMods.data();

    constexpr D frameDestinations[] { D::Osc1Frame, D::Osc2Frame, D::SubFrame,
                                      D::Osc4Frame, D::Osc5Frame, D::Osc6Frame };
    constexpr D levelDestinations[] { D::Osc1Level, D::Osc2Level, D::SubLevel,
                                      D::Osc4Level, D::Osc5Level, D::Osc6Level };
    constexpr D warpDestinations[] { D::Osc1Warp, D::Osc2Warp, D::SubWarp,
                                     D::Osc4Warp, D::Osc5Warp, D::Osc6Warp };
    // The bus each source adds into: its _route (the fixed chain's buses),
    // or with the ROUTING cables on the bus of its cables (one per distinct
    // set of destinations, VoiceRouting.h).
    const auto& routing = params.routing;
    const auto bodyInChain = bodyIsInChain();
    const auto numBuses = routing.on ? routing.numBuses : (int) FilterRoute::Count;
    int routes[VoiceParams::numOscillators] {};
    bool active[VoiceParams::numOscillators] {};

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        const auto& settings = params.oscillators[osc];
        routes[osc] = routing.on ? routing.busOf[(size_t) osc] : juce::jlimit (0, FilterRoute::Count - 1, settings.route);
        active[osc] = params.oscillatorEnabled[osc]
                      && (settings.stringMode || settings.sampleMode || settings.table != nullptr);
    }

    const auto routeSubOsc = routing.on ? routing.busOf[(size_t) VoiceRouting::subNoiseSource]
                                        : juce::jlimit (0, FilterRoute::Count - 1, params.subOscRoute);
    bool bothActive = ! routing.on && routeSubOsc == FilterRoute::Both;
    for (int osc = 0; osc < VoiceParams::numOscillators && ! routing.on; ++osc)
        bothActive = bothActive || (active[osc] && routes[osc] == FilterRoute::Both);
    bothRouteActive = bothActive;
    const auto subOscActive = params.subOscEnabled && params.subOscTable != nullptr;
    const auto subOscFrames = WavetableOscillator::frameReadFor (params.subOscTable, 0.0f);

    // M5 operator extras. Each is skipped while unused, so older patches
    // render exactly as before.
    auto anyAltFeedback = false;
    auto anyNoiseOperator = false;
    auto anyWarpNoise = false; // an oscillator's WARP is FM Noise
    // The FM cells that involve OSC 4-6. A zero cell adds nothing, so unless
    // the extended cells are modulated only the non-zero ones are summed
    // (in the same order as the full matrix).
    int extendedFmCells[27][2];
    auto numExtendedFmCells = 0;

    for (int target = 0; target < VoiceParams::numOscillators; ++target)
        for (int source = target < 3 ? 3 : 0; source < VoiceParams::numOscillators; ++source)
            if (params.anyExtendedFmMods || params.fmMatrix[source][target] != 0.0f)
            {
                extendedFmCells[numExtendedFmCells][0] = source;
                extendedFmCells[numExtendedFmCells][1] = target;
                ++numExtendedFmCells;
            }

    // The oscillators whose feedback history the per-sample loop keeps
    // (DX7 and filtered feedback types), and those whose string drives a
    // body (the others' drive is zero).
    int historyOscs[VoiceParams::numOscillators] {}, numHistoryOscs = 0;
    int drivingOscs[VoiceParams::numOscillators] {}, numDrivingOscs = 0;
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        const auto type = params.oscillators[osc].feedbackType;
        if (type == FmFeedback::Dx7 || type == FmFeedback::Filtered)
            historyOscs[numHistoryOscs++] = osc;
        if (params.oscillators[osc].stringMode && ! params.oscillators[osc].sampleMode)
            drivingOscs[numDrivingOscs++] = osc;
    }

    // OSC 1-3 that are off with nothing still fading: the per-sample loop
    // skips them (their smoothers would return the same values).
    bool idleOsc[3] {};
    for (int osc = 0; osc < 3; ++osc)
        idleOsc[osc] = ! active[osc] && oscEnableSmooth[osc].getCurrentValue() <= 0.0005f
                       && ! oscEnableSmooth[osc].isSmoothing() && ! frameSmooth[osc].isSmoothing()
                       && ! levelSmooth[osc].isSmoothing();

    // The per-sample loop's oscillators: OSC 1-3 that are idle and OSC 4-6
    // that are off and silent at the block's start are left out (nothing in
    // the loop would move them; their outputs are set after it).
    int visitOscs[VoiceParams::numOscillators] {}, numVisitOscs = 0;
    int skippedOscs[VoiceParams::numOscillators] {}, numSkippedOscs = 0;
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        const auto skip = osc < 3 ? idleOsc[osc]
                                  : ! active[osc] && oscEnableSmooth[osc].getCurrentValue() <= 0.0005f;
        if (skip)
            skippedOscs[numSkippedOscs++] = osc;
        else
            visitOscs[numVisitOscs++] = osc;
    }
    const auto skipsOsc1 = idleOsc[1];
    // An oscillator skipped all block whose feedback state is at rest (zero)
    // stays there: its history update would leave it unchanged.
    {
        auto kept = 0;
        for (int k = 0; k < numHistoryOscs; ++k)
        {
            const auto osc = historyOscs[k];
            auto resting = feedbackHistory[osc] == 0.0f && ! std::signbit (feedbackHistory[osc])
                           && feedbackFiltered[osc] == 0.0f && ! std::signbit (feedbackFiltered[osc]);
            auto skippedAll = false;
            for (int v = 0; v < numSkippedOscs; ++v)
                skippedAll = skippedAll || skippedOscs[v] == osc;
            if (! (resting && skippedAll))
                historyOscs[kept++] = osc;
        }
        numHistoryOscs = kept;
    }

    // An operator whose FM input cells are all zero and unmodulated this
    // block gets nothing from its feedback type (every term is zero), so its
    // per-sample sum is skipped; its feedback filter still tracks.
    bool altFeedbackInput[VoiceParams::numOscillators] {};
    int altSources[VoiceParams::numOscillators][VoiceParams::numOscillators];
    int numAltSources[VoiceParams::numOscillators] {};
    auto anyAltFeedbackInput = false;
    const auto cellIsSilent = [this, mods] (int source, int target)
    {
        // (OSC 1 hears its own feedback and OSC 2 through the legacy
        // amounts, which the processor mirrors into the matrix.)
        const auto amount = target == 0 && source == 0 ? params.fmFeedback
                          : target == 0 && source == 1 ? params.fmAmount
                                                       : params.fmMatrix[source][target];
        if (amount != 0.0f || params.fmMatrix[source][target] != 0.0f)
            return false;

        if (source >= 3 || target >= 3)
            return ! params.anyExtendedFmMods;

        static constexpr D legacy[3][3] { { D::FmFeedback, D::Fm1to2, D::Fm1to3 },
                                          { D::FmAmount, D::Fm2Feedback, D::Fm2to3 },
                                          { D::Fm3to1, D::Fm3to2, D::Fm3Feedback } };
        const auto destination = (int) legacy[source][target];
        for (int d = 0; d < params.numActiveDestinations; ++d)
            if (params.activeDestinations[d] == destination)
                return false;
        return mods[destination] == 0.0f;
    };

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        anyAltFeedback = anyAltFeedback || (active[osc] && params.oscillators[osc].feedbackType != FmFeedback::Plain);
        anyNoiseOperator = anyNoiseOperator || (active[osc] && params.fmNoise[osc] > 0.0f);
        anyWarpNoise = anyWarpNoise || (active[osc] && params.oscillators[osc].warpMode == Warp::NoiseFm);

        // The other operators that feed it: a silent cell adds an exact zero,
        // so leaving it out of the sum changes nothing.
        if (active[osc] && params.oscillators[osc].feedbackType != FmFeedback::Plain)
            for (int source = 0; source < VoiceParams::numOscillators; ++source)
                if (! cellIsSilent (source, osc))
                {
                    altFeedbackInput[osc] = true;
                    if (source != osc)
                        altSources[osc][numAltSources[osc]++] = source;
                }
        anyAltFeedbackInput = anyAltFeedbackInput || altFeedbackInput[osc];
    }
    // The heard noise's colour, on the FM noise's scale (about 200 Hz to
    // white); 1 leaves it white.
    auto heardNoiseCoeff = 1.0f, heardNoiseGain = 1.0f;
    if (params.noiseColour < 0.999f)
    {
        const auto cutoff = juce::jmin (0.45 * sampleRate, 200.0 * std::pow (100.0, (double) juce::jlimit (0.0f, 1.0f, params.noiseColour)));
        heardNoiseCoeff = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * cutoff / sampleRate));
        heardNoiseGain = std::pow (heardNoiseCoeff / (2.0f - heardNoiseCoeff), -0.25f);
    }
    const auto noiseCutoff = juce::jmin (0.45 * sampleRate,
                                         200.0 * std::pow (100.0, (double) juce::jlimit (0.0f, 1.0f, params.fmNoiseColour)));
    const auto noiseCoeff = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * noiseCutoff / sampleRate));

    // The matrix amount from source to target, with the legacy cells' own
    // modulation destinations.
    const auto fmAmountAt = [this] (const float* sampleMods, int source, int target)
    {
        const auto amount = params.fmMatrix[source][target];

        if (source >= 3 || target >= 3)
            return params.anyExtendedFmMods ? juce::jlimit (0.0f, 1.0f, amount + fmCellMods[(size_t) (source * 6 + target)])
                                            : amount;

        static constexpr D legacy[3][3] { { D::FmFeedback, D::Fm1to2, D::Fm1to3 },
                                          { D::FmAmount, D::Fm2Feedback, D::Fm2to3 },
                                          { D::Fm3to1, D::Fm3to2, D::Fm3Feedback } };
        return amount + sampleMods[(int) legacy[source][target]];
    };

    // Each 16-sample chunk (the sub-block the coefficients follow) runs in
    // two passes: the sources (envelopes, modulation, oscillators, FM,
    // strings) for every sample, then the post chain (drive, filters, west
    // coast, body, amp) for every sample. Nothing in the post chain feeds the
    // sources within a chunk, so the result is the same as one pass per
    // sample, and the post chain runs as tight loops (Vital works in blocks
    // the same way). Two cases do feed back, and run one sample per chunk:
    // the body ringing the strings (coupling mode 2) and audio-rate filter FM
    // (the coefficients move every sample).
    constexpr int maxChunk = 16;
    const auto sampleFeedback = (params.resonatorOn && params.bodyType != 0 && params.bodyCouplingMode == 2
                                 && params.bodyCoupling > 0.0f)
                                || params.filter1Fm != 0.0f || params.filter2Fm != 0.0f
                                || [this]
                                   {
                                       for (int s = 0; s < params.numModSlots; ++s)
                                           if (params.modSlots[s].isActive()
                                               && (params.modSlots[s].destination == (int) D::Filter1Fm
                                                   || params.modSlots[s].destination == (int) D::Filter2Fm))
                                               return true;
                                       return false;
                                   }();
    float chunkBusL[maxChunk][VoiceRouting::maxBuses], chunkBusR[maxChunk][VoiceRouting::maxBuses];
    float chunkBodyL[maxChunk], chunkBodyR[maxChunk], chunkLive[maxChunk], chunkAmp[maxChunk];
    float chunkAmpLevel[maxChunk], chunkWest[maxChunk];

    // The post chain for one chunk, stage by stage: every stage only reads
    // the stage before it at the same sample, so running each over the chunk
    // in turn gives the per-sample result, and the filters run as blocks.
    const auto drive = [] (float value, float amount) { return amount > 1.0f ? FastMath::tanh (value * amount) : value; };

    // A mono source feeds both sides alike: drive it once (the same result).
    const auto drivePair = [&drive] (float l, float r, DriveAmounts amount, float& outL, float& outR)
    {
        outL = drive (l, amount.l);
        outR = r == l && amount.r == amount.l ? outL : drive (r, amount.r);
    };

    // M8.3: the west-coast voice's control (the LED's drive): a strike on
    // each note, or any mod source, on top of OPEN.
    const auto westProcess = [&] (float& l, float& r, float westSource)
    {
        const auto& w = params.west;
        auto control = w.open;
        if (w.source == 0)
        {
            if (westStrikeRemaining > 0)
            {
                control += 2.0f * w.strike * velocityLevel; // the LED overdriven
                --westStrikeRemaining;
            }
        }
        else
        {
            control += 2.0f * w.strike * westSource;
        }
        const auto gateMode = (LowPassGate::Mode) juce::jlimit (0, 2, w.mode);
        westGateL.setParams (gateMode, w.decay, w.resonance);
        westGateR.setParams (gateMode, w.decay, w.resonance);
        westFolder.process (l, r);
        westGateL.processPair (l, r, control, westGateR);
    };
    const auto westReplacesFilter2 = params.west.on && params.west.position == 1;

    // Roadmap step 16: the ROUTING cables. Each used stage runs in the
    // compiled order (VoiceRouting.h); its input is the sum of the buses and
    // stages cabled into it (Filter 1 and 2 behind their DRIVE, as in the
    // fixed chain), and its output goes to each stage or OUT it is cabled to.
    // WEST switched off passes its input on. Two filters into one place mix
    // as the fixed chain's PARALLEL does (BALANCE, 0.7071).
    const auto runCables = [&] (int n, float* outL, float* outR)
    {
        constexpr int numNodes = VoiceRouting::numStages + 1; // the stages, then OUT
        float nodeL[numNodes][maxChunk], nodeR[numNodes][maxChunk];
        for (int node = 0; node < numNodes; ++node)
        {
            std::fill (nodeL[node], nodeL[node] + n, 0.0f);
            std::fill (nodeR[node], nodeR[node] + n, 0.0f);
        }

        for (int bus = 0; bus < routing.numBuses; ++bus)
            for (int node = 0; node < numNodes; ++node)
                if ((routing.busMask[(size_t) bus] & VoiceRouting::bitOf (node)) != 0)
                    for (int s = 0; s < n; ++s)
                    {
                        nodeL[node][s] += chunkBusL[s][bus];
                        nodeR[node][s] += chunkBusR[s][bus];
                    }

        const auto gain1 = juce::jmin (1.0f, 1.0f - params.filterBalance) * 0.7071f;
        const auto gain2 = juce::jmin (1.0f, 1.0f + params.filterBalance) * 0.7071f;

        for (int k = 0; k < routing.numOrdered; ++k)
        {
            const auto stage = routing.order[(size_t) k];
            float inL[maxChunk], inR[maxChunk], stageL[maxChunk], stageR[maxChunk];

            if (stage == VoiceRouting::f1)
            {
                for (int s = 0; s < n; ++s)
                    drivePair (nodeL[stage][s], nodeR[stage][s], drive1, inL[s], inR[s]);
                if (filter1Open)
                    processOpenPairBlock (openFilter1L, openFilter1R, filter1Linked, inL, inR, stageL, stageR, n);
                else
                    processFilterPairBlock (filter1L, filter1R, filter1Linked, inL, inR, stageL, stageR, n);
                filter1Ran = true;

                if (filter1Fade > 0)
                {
                    float oldL[maxChunk], oldR[maxChunk];
                    if (filter1Open)
                        processFilterPairBlock (filter1L, filter1R, filter1FadeLinked, inL, inR, oldL, oldR, n);
                    else
                        processOpenPairBlock (openFilter1L, openFilter1R, filter1FadeLinked, inL, inR, oldL, oldR, n);
                    for (int s = 0; s < n && filter1Fade > 0; ++s, --filter1Fade)
                    {
                        const auto g = (float) filter1Fade / (float) filterFadeLength;
                        stageL[s] += (oldL[s] - stageL[s]) * g;
                        stageR[s] += (oldR[s] - stageR[s]) * g;
                    }
                }
            }
            else if (stage == VoiceRouting::f2)
            {
                for (int s = 0; s < n; ++s)
                    drivePair (nodeL[stage][s], nodeR[stage][s], drive2, inL[s], inR[s]);
                processFilter2Block (inL, inR, stageL, stageR, n);
            }
            else if (stage == VoiceRouting::west)
            {
                for (int s = 0; s < n; ++s)
                {
                    stageL[s] = nodeL[stage][s];
                    stageR[s] = nodeR[stage][s];
                    if (params.west.on)
                        westProcess (stageL[s], stageR[s], chunkWest[s]);
                }
            }
            else
            {
                // BODY: the Classic resonator rings on what comes in; a material
                // body is struck as ever and also rung by it (a share of the
                // audio), then mixed with the dry as BODY AMOUNT says. Off, it
                // passes the signal on.
                for (int s = 0; s < n; ++s)
                {
                    auto l = nodeL[stage][s];
                    auto r = nodeR[stage][s];
                    if (params.resonatorOn && resonatorAmount > 0.001f)
                    {
                        if (params.bodyType == 0)
                        {
                            const auto excite = chunkLive[s] * params.inputToBody;
                            l = resonatorL.process (l, excite);
                            r = resonatorR.process (r, excite);
                        }
                        else
                        {
                            const auto exciteEnvelope = alternateAmpRouting ? 1.0f : chunkAmp[s];
                            const auto wetL = materialBodyL.process (chunkBodyL[s] * exciteEnvelope + l * bodyAudioShare);
                            const auto wetR = materialBodyR.process (chunkBodyR[s] * exciteEnvelope + r * bodyAudioShare);
                            if (params.bodyCouplingMode == 2 && params.bodyCoupling > 0.0f)
                                for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
                                    if (params.oscillatorEnabled[osc] && params.oscillators[osc].stringMode)
                                        for (int u = 0; u < stringsPlaying (osc); ++u)
                                            stringFor (osc, u).addBridgeInput (0.01f * params.bodyCoupling * (wetL + wetR));
                            l = l * (1.0f - resonatorAmount) + wetL * resonatorAmount;
                            r = r * (1.0f - resonatorAmount) + wetR * resonatorAmount;
                        }
                    }
                    stageL[s] = l;
                    stageR[s] = r;
                }
            }

            const auto destinations = routing.stageOut[(size_t) stage];
            const auto paired = stage == VoiceRouting::f1 || stage == VoiceRouting::f2 ? routing.filterPairInto : 0;
            for (int node = 0; node < numNodes; ++node)
            {
                if ((destinations & VoiceRouting::bitOf (node)) == 0)
                    continue;
                const auto gain = (paired & VoiceRouting::bitOf (node)) != 0 ? (stage == VoiceRouting::f1 ? gain1 : gain2) : 1.0f;
                for (int s = 0; s < n; ++s)
                {
                    nodeL[node][s] += stageL[s] * gain;
                    nodeR[node][s] += stageR[s] * gain;
                }
            }
        }

        std::copy (nodeL[VoiceRouting::out], nodeL[VoiceRouting::out] + n, outL);
        std::copy (nodeR[VoiceRouting::out], nodeR[VoiceRouting::out] + n, outR);
    };

    const auto postChunk = [&] (int chunkStart, int n)
    {
        float defaultL[maxChunk], defaultR[maxChunk], inL[maxChunk], inR[maxChunk];
        float f1L[maxChunk], f1R[maxChunk], in2L[maxChunk], in2R[maxChunk], outL[maxChunk], outR[maxChunk];

        if (routing.on)
        {
            runCables (n, outL, outR);
        }
        else
        {
            // Filter 1 hears the Default and Filter-1 buses.
            for (int s = 0; s < n; ++s)
            {
                const auto* busL = chunkBusL[s];
                const auto* busR = chunkBusR[s];
                drivePair (busL[FilterRoute::Default], busR[FilterRoute::Default], drive1, defaultL[s], defaultR[s]);
                inL[s] = defaultL[s];
                inR[s] = defaultR[s];
                if (busL[FilterRoute::Filter1] != 0.0f || busR[FilterRoute::Filter1] != 0.0f)
                    drivePair (busL[FilterRoute::Default] + busL[FilterRoute::Filter1],
                               busR[FilterRoute::Default] + busR[FilterRoute::Filter1], drive1, inL[s], inR[s]);
            }

            if (filter1Open)
                processOpenPairBlock (openFilter1L, openFilter1R, filter1Linked, inL, inR, f1L, f1R, n);
            else
                processFilterPairBlock (filter1L, filter1R, filter1Linked, inL, inR, f1L, f1R, n);
            filter1Ran = true;

            if (filter1Fade > 0)
            {
                float oldL[maxChunk], oldR[maxChunk];
                if (filter1Open)
                    processFilterPairBlock (filter1L, filter1R, filter1FadeLinked, inL, inR, oldL, oldR, n);
                else
                    processOpenPairBlock (openFilter1L, openFilter1R, filter1FadeLinked, inL, inR, oldL, oldR, n);
                for (int s = 0; s < n && filter1Fade > 0; ++s, --filter1Fade)
                {
                    const auto g = (float) filter1Fade / (float) filterFadeLength;
                    f1L[s] += (oldL[s] - f1L[s]) * g;
                    f1R[s] += (oldR[s] - f1R[s]) * g;
                }
            }

            if (params.filtersParallel)
            {
                // Filter 2 hears the (Filter-1-driven) Default bus plus its own.
                for (int s = 0; s < n; ++s)
                    drivePair (defaultL[s] + chunkBusL[s][FilterRoute::Filter2], defaultR[s] + chunkBusR[s][FilterRoute::Filter2],
                               drive2, in2L[s], in2R[s]);

                float f2L[maxChunk], f2R[maxChunk];
                if (westReplacesFilter2)
                {
                    for (int s = 0; s < n; ++s)
                    {
                        f2L[s] = in2L[s];
                        f2R[s] = in2R[s];
                        westProcess (f2L[s], f2R[s], chunkWest[s]);
                    }
                }
                else
                {
                    processFilter2Block (in2L, in2R, f2L, f2R, n);
                }

                // Balance fades one filter out; at the centre both are at full.
                const auto gain1 = juce::jmin (1.0f, 1.0f - params.filterBalance);
                const auto gain2 = juce::jmin (1.0f, 1.0f + params.filterBalance);
                for (int s = 0; s < n; ++s)
                {
                    outL[s] = (f1L[s] * gain1 + f2L[s] * gain2) * 0.7071f;
                    outR[s] = (f1R[s] * gain1 + f2R[s] * gain2) * 0.7071f;
                }
            }
            else
            {
                for (int s = 0; s < n; ++s)
                    drivePair (f1L[s] + chunkBusL[s][FilterRoute::Filter2], f1R[s] + chunkBusR[s][FilterRoute::Filter2],
                               drive2, in2L[s], in2R[s]);

                if (westReplacesFilter2)
                {
                    for (int s = 0; s < n; ++s)
                    {
                        outL[s] = in2L[s];
                        outR[s] = in2R[s];
                        westProcess (outL[s], outR[s], chunkWest[s]);
                    }
                }
                else
                {
                    processFilter2Block (in2L, in2R, outL, outR, n);
                }
            }
        }

        for (int s = 0; s < n; ++s)
        {
            const auto i = chunkStart + s;
            const auto* busL = chunkBusL[s];
            const auto* busR = chunkBusR[s];
            auto sampleL = outL[s];
            auto sampleR = outR[s];

            // (With the cables the bypass and WEST are cables, run above.)
            if (! routing.on)
            {
                sampleL += busL[FilterRoute::Direct];
                sampleR += busR[FilterRoute::Direct];
                if (params.west.on && params.west.position == 0)
                    westProcess (sampleL, sampleR, chunkWest[s]);
            }

            // A separate parallel pair keeps the existing serial/parallel paths
            // bit-identical whenever no oscillator selects Both.
            if (bothActive)
            {
                const auto both1L = bothFilter1L.process (drive (busL[FilterRoute::Both], drive1.l));
                const auto both1R = bothFilter1R.process (drive (busR[FilterRoute::Both], drive1.r));
                const auto both2L = bothFilter2L.process (drive (busL[FilterRoute::Both], drive2.l));
                const auto both2R = bothFilter2R.process (drive (busR[FilterRoute::Both], drive2.r));
                sampleL += (both1L + both2L) * 0.7071f;
                sampleR += (both1R + both2R) * 0.7071f;
            }

            float bodyWetL = 0.0f, bodyWetR = 0.0f;
            if (bodyInChain)
            {
                // (The BODY stage ran above, among the cables.)
            }
            else if (params.resonatorOn && resonatorAmount > 0.001f && params.bodyType == 0)
            {
                // M7.5: the live input rings the Classic body too.
                const auto excite = chunkLive[s] * params.inputToBody;
                sampleL = resonatorL.process (sampleL, excite);
                sampleR = resonatorR.process (sampleR, excite);
            }
            else if (params.resonatorOn && resonatorAmount > 0.001f)
            {
                const auto exciteEnvelope = alternateAmpRouting ? 1.0f : chunkAmp[s];
                bodyWetL = materialBodyL.process (chunkBodyL[s] * exciteEnvelope);
                bodyWetR = materialBodyR.process (chunkBodyR[s] * exciteEnvelope);
                sampleL *= 1.0f - resonatorAmount;
                sampleR *= 1.0f - resonatorAmount;
                if (params.bodyCouplingMode == 2 && params.bodyCoupling > 0.0f)
                    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
                        if (params.oscillatorEnabled[osc] && params.oscillators[osc].stringMode)
                            for (int u = 0; u < stringsPlaying (osc); ++u)
                                stringFor (osc, u).addBridgeInput (0.01f * params.bodyCoupling * (bodyWetL + bodyWetR));
            }

            const auto ampLevelMod = chunkAmpLevel[s];
            auto ampGain = (alternateAmpRouting ? 1.0f : chunkAmp[s]) * ampVelScale
                           * juce::jlimit (0.0f, 2.0f, 1.0f + ampLevelMod);

            // SUSTAIN VOICES: a voice over the cap fades out (startFadeOut).
            if (fadeTotal > 0)
            {
                ampGainFade = fadeRemaining > 0 ? (float) --fadeRemaining / (float) fadeTotal : 0.0f;
                ampGain *= ampGainFade;
            }

            if (params.resonatorOn && params.bodyType != 0 && resonatorAmount > 0.001f && ! bodyInChain)
            {
                const auto wetGain = resonatorAmount * ampVelScale
                                     * juce::jlimit (0.0f, 2.0f, 1.0f + ampLevelMod) * ampGainFade;
                left[startSample + i] += sampleL * ampGain + bodyWetL * wetGain;
                if (right != nullptr)
                    right[startSample + i] += sampleR * ampGain + bodyWetR * wetGain;
                else
                    left[startSample + i] += sampleR * ampGain + bodyWetR * wetGain;
            }
            else
            {
                left[startSample + i] += sampleL * ampGain;
                if (right != nullptr)
                    right[startSample + i] += sampleR * ampGain;
                else
                    left[startSample + i] += sampleR * ampGain;
                if (watchReleaseSilence || watchHeldSilence)
                    releasePeak = juce::jmax (releasePeak, std::abs (sampleL * ampGain), std::abs (sampleR * ampGain));
            }
        }
    };

    // Block-wise sources: within a chunk, every operator and string reads
    // the others only through their previous sample, so each one can run
    // over the whole chunk in turn (the ones it hears first), with the
    // per-sample work (envelopes, modulation, LFOs) done first and the buses
    // summed afterwards in the old order. Every value is computed as in the
    // per-sample loop, so the samples are the same. Cases where two
    // oscillators hear each other within the chunk (cross feedback, a
    // feedback loop in the matrix, the strings ringing each other) or that
    // read another oscillator's current sample (Warp FM / Ring, hard sync,
    // samples, grains, live input) keep the per-sample loop.
    int blockOrder[VoiceParams::numOscillators] {};
    const auto useBlockSources = [&]
    {
        if (! blockSourcesEnabled() || params.hardSync || params.anyExtendedFmMods
            || (params.bodyCouplingMode == 3 && params.bodyCoupling > 0.0f))
            return false;

        // It pays only with two or more wavetable / FM operators; strings and
        // single-oscillator patches measure faster on the per-sample loop.
        bool edge[VoiceParams::numOscillators][VoiceParams::numOscillators] {};
        auto numRendering = 0;
        for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        {
            const auto& settings = params.oscillators[osc];
            const auto mayRender = active[osc] || oscEnableSmooth[osc].getCurrentValue() > 0.0005f
                                   || oscEnableSmooth[osc].isSmoothing();
            if (mayRender && (settings.sampleMode || settings.granularMode || settings.liveMode
                              || settings.stringMode
                              || settings.warpMode == Warp::Fm || settings.warpMode == Warp::Ring
                              || settings.warpMode == Warp::NoiseFm))
                return false;
            numRendering += mayRender ? 1 : 0;
            if (altFeedbackInput[osc] && settings.feedbackType == FmFeedback::Cross)
                return false;
            for (int k = 0; k < numAltSources[osc]; ++k)
                edge[altSources[osc][k]][osc] = true;
        }
        if (numRendering < 2)
            return false;
        for (int target = 0; target < 3; ++target)
            for (int source = 0; source < 3; ++source)
                if (! cellIsSilent (source, target))
                    edge[source][target] = true;
        for (int cell = 0; cell < numExtendedFmCells; ++cell)
            edge[extendedFmCells[cell][0]][extendedFmCells[cell][1]] = true;

        // Each oscillator after the ones it hears (a loop through two or
        // more of them can't be ordered).
        bool placed[VoiceParams::numOscillators] {};
        for (int slot = 0; slot < VoiceParams::numOscillators; ++slot)
        {
            auto next = -1;
            for (int osc = 0; osc < VoiceParams::numOscillators && next < 0; ++osc)
            {
                if (placed[osc])
                    continue;
                auto ready = true;
                for (int source = 0; source < VoiceParams::numOscillators; ++source)
                    ready = ready && (source == osc || placed[source] || ! edge[source][osc]);
                if (ready)
                    next = osc;
            }
            if (next < 0)
                return false;
            placed[next] = true;
            blockOrder[slot] = next;
        }
        return true;
    }();

    // The block-wise sources for one chunk (see useBlockSources).
    const auto renderSourcesBlock = [&] (int chunkStart, int chunkEnd)
    {
        constexpr int numOsc = VoiceParams::numOscillators;
        constexpr int maxU = VoiceParams::maxBufferedUnison;
        const auto n = chunkEnd - chunkStart;

        // Per sample, before the oscillators: envelopes, the Operator EG,
        // LFOs, modulation, and what the oscillators read of them.
        float selectedEnvs[maxChunk][numOsc], levelMods[maxChunk][numOsc], frameMods[maxChunk][numOsc];
        float legacyMods[maxChunk][9], dx7Gains[maxChunk][numOsc], fmNoise[maxChunk], ringMods[maxChunk];
        double dx7Rates[maxChunk];
        float subL[maxChunk], subR[maxChunk], noiseValue[maxChunk];
        bool subOn[maxChunk], noiseOn[maxChunk];
        static constexpr D legacyDestinations[9] { D::FmFeedback, D::Fm1to2, D::Fm1to3,
                                                   D::FmAmount, D::Fm2Feedback, D::Fm2to3,
                                                   D::Fm3to1, D::Fm3to2, D::Fm3Feedback };

        for (int i = chunkStart; i < chunkEnd; ++i)
        {
            const auto slot = i - chunkStart;
            const auto liveSample = params.liveInput != nullptr ? params.liveInput[startSample + i] : 0.0f;
            const auto ampValue = ampEnv.getNextSample();
            const auto control = (i & (modControlInterval - 1)) == 0;
            const auto filterValue = stepEnvelope (filterEnv, 1, control);
            const auto filter2Value = stepEnvelope (filter2Env, 2, control);
            const auto modValue = stepEnvelope (modEnv, 3, control);
            const auto env4Value = stepEnvelope (env4, 4, control);
            for (int index = 0; index < numNeededExtraEnvs; ++index)
            {
                const auto env = (size_t) neededExtraEnvs[index];
                extraEnvValues[env] = stepEnvelope (extraEnvs[env], (int) env + 5, control);
            }
            if (params.msegEnvNeeded)
                msegEnvValue = juce::jlimit (0.0f, 1.0f, envMseg.getNextValue());
            const float envelopeValues[17] { ampValue, filterValue, filter2Value, modValue, env4Value,
                                             extraEnvValues[0], extraEnvValues[1], extraEnvValues[2],
                                             extraEnvValues[3], extraEnvValues[4], extraEnvValues[5],
                                             extraEnvValues[6], extraEnvValues[7], extraEnvValues[8],
                                             extraEnvValues[9], extraEnvValues[10], msegEnvValue };

            float dx7Gain[6] {};
            auto dx7PitchRate = 1.0;
            if (dx7Playing)
            {
                if (dx7Count == 0)
                {
                    if (params.operatorEg != dx7Settings)
                    {
                        dx7Settings = params.operatorEg;
                        dx7Note.update (dx7Settings);
                    }
                    dx7Previous = dx7Current;
                    dx7Note.step();
                    dx7Current = dx7Note.getGains();
                }
                const auto t = (float) (dx7Count + 1) / (float) Dx7::block;
                for (int op = 0; op < 6; ++op)
                    dx7Gain[op] = dx7Previous[(size_t) op] + (dx7Current[(size_t) op] - dx7Previous[(size_t) op]) * t;
                dx7Count = (dx7Count + 1) % Dx7::block;
                if (const auto octaves = dx7Note.getPitchOctaves(); octaves != 0.0f)
                    dx7PitchRate = std::exp2 ((double) octaves);
            }

            advanceVoiceLfos (i);
            evaluateModsRated (mods, i, ampValue, filterValue, filter2Value, modValue, env4Value);

            if ((i & 15) == 0)
                updateSubBlock (mods, filterValue, filter2Value, envelopeValues);
            else if (params.filter1Fm != 0.0f || params.filter2Fm != 0.0f
                     || mods[(int) D::Filter1Fm] != 0.0f || mods[(int) D::Filter2Fm] != 0.0f)
                updateFilterCoefficients (mods, filterValue, filter2Value);

            for (int osc = 0; osc < numOsc; ++osc)
            {
                const auto ampEnvIndex = params.oscillators[osc].ampEnv;
                selectedEnvs[slot][osc] = envelopeValues[ampEnvIndex <= 16 ? ampEnvIndex : 0];
                levelMods[slot][osc] = mods[(int) levelDestinations[osc]];
                frameMods[slot][osc] = mods[(int) frameDestinations[osc]];
                dx7Gains[slot][osc] = dx7Gain[osc];
            }
            for (int cell = 0; cell < 9; ++cell)
                legacyMods[slot][cell] = mods[(int) legacyDestinations[cell]];
            dx7Rates[slot] = dx7PitchRate;
            ringMods[slot] = params.ringMod + mods[(int) D::RingMod];

            if (anyNoiseOperator)
                stepFmNoise (noiseCoeff);
            fmNoise[slot] = fmNoiseState;

            // The sub and the noise (their own smoothers and generator; the
            // noise's draws keep their place between the LFOs').
            const auto subOscLevel = subOscLevelSmooth.getNextValue();
            const auto enableSubOsc = subOscEnableSmooth.getNextValue();
            subOn[slot] = subOscLevel > 0.0f && params.subOscTable != nullptr && (subOscActive || enableSubOsc > 0.0005f);
            if (subOn[slot])
            {
                subOsc.setFramePosition (0.0f);
                const auto raw = colourSub (subOsc.getNextSample (0.0, subOscFrames), subOscColourSmooth.getNextValue());
                const auto gain = subOscLevel * enableSubOsc;
                subL[slot] = raw * gain * panGainSubOscL * (alternateAmpRouting ? ampValue : 1.0f);
                subR[slot] = raw * gain * panGainSubOscR * (alternateAmpRouting ? ampValue : 1.0f);
            }

            const auto noiseLevel = juce::jlimit (0.0f, 1.0f, noiseSmooth.getNextValue() + mods[(int) D::NoiseLevel])
                                    * noiseEnableSmooth.getNextValue();
            noiseOn[slot] = noiseLevel > 0.0f;
            if (noiseOn[slot])
            {
                auto value = noiseGenerator.process (params.noiseType, random.nextFloat() * 2.0f - 1.0f) * noiseLevel * 0.5f
                             * (alternateAmpRouting ? ampValue : 1.0f);
                if (heardNoiseCoeff < 1.0f)
                {
                    noiseLow += heardNoiseCoeff * (value - noiseLow);
                    value = noiseLow * heardNoiseGain;
                }
                noiseValue[slot] = value;
            }

            chunkLive[slot] = liveSample;
            chunkAmp[slot] = ampValue;
            chunkAmpLevel[slot] = mods[(int) D::AmpLevel];
            chunkWest[slot] = params.west.on && params.west.source != 0
                                  ? juce::jlimit (0.0f, 1.0f, sourceValue ((Mod::Source) params.west.source, i, ampValue, filterValue,
                                                                           filter2Value, modValue, env4Value))
                                  : 0.0f;

            lastAmpValue = ampValue;
            lastLifetimeValue = ampValue;
            if (operatorEgOwnsVoice())
                lastLifetimeValue = dx7Note.isActive() ? 1.0f : 0.0f;
            else if (alternateAmpRouting)
                for (int osc = 0; osc < numOsc; ++osc)
                    if (params.oscillatorEnabled[osc])
                        lastLifetimeValue = juce::jmax (lastLifetimeValue,
                                                        usesOperatorEg (osc) ? (dx7Note.isActive() ? 1.0f : 0.0f)
                                                        : envelopeValues[params.oscillators[osc].ampEnv <= 16 ? params.oscillators[osc].ampEnv : 0]);
            lastFilterValue = filterValue;
            lastFilter2Value = filter2Value;
            lastModValue = modValue;
            lastEnv4Value = env4Value;
        }

        // Each oscillator over the chunk, after the ones it hears. "prev"
        // holds each one's previousOsc after every sample (slot 0: before
        // the chunk); one not run yet is only read through a silent cell.
        float prev[numOsc][maxChunk + 1];
        float monoOut[numOsc][maxChunk] {};
        float driveOut[numOsc][maxChunk] {};
        float heardL[numOsc][maxChunk][maxU], heardR[numOsc][maxChunk][maxU];
        int heardCount[numOsc][maxChunk] {};
        for (int osc = 0; osc < numOsc; ++osc)
        {
            prev[osc][0] = previousOsc[osc];
            for (int slot = 1; slot <= n; ++slot)
                prev[osc][slot] = 0.0f;
        }

        const auto fmPhase = [this] (double input) { return params.fmMode == 0 ? input : 0.0; };
        const auto fmRate = [this] (double input)
        {
            if (params.fmMode == 1)
                return 1.0 + 4.0 * input;

            if (params.fmMode == 2)
                return std::exp2 (juce::jlimit (-4.0, 4.0, 2.0 * input));

            return 1.0;
        };

        const auto modulated = [this] (D destination)
        {
            for (int d = 0; d < params.numActiveDestinations; ++d)
                if (params.activeDestinations[d] == (int) destination)
                    return true;
            return false;
        };

        for (int order = 0; order < numOsc; ++order)
        {
            const auto osc = blockOrder[order];
            const auto& settings = params.oscillators[osc];

            // The extended cells this one hears, in the matrix's order.
            int opCells[27];
            auto numOpCells = 0;
            for (int cell = 0; cell < numExtendedFmCells; ++cell)
                if (extendedFmCells[cell][1] == osc)
                    opCells[numOpCells++] = extendedFmCells[cell][0];

            // A table oscillator whose smoothers rest and whose level and
            // frame aren't modulated: the same enable, level and frame on
            // every sample of the chunk (a resting smoother's next value is
            // its target, with nothing to advance).
            const auto steady = ! oscEnableSmooth[osc].isSmoothing()
                                && ! levelSmooth[osc].isSmoothing() && ! frameSmooth[osc].isSmoothing()
                                && (osc != 2 || ! modulated (levelDestinations[osc])) && ! modulated (frameDestinations[osc])
                                && params.fmMode == 0 && ! anyNoiseOperator;
            // A modulated level (OSC 1-2, 4-6) is read per sample.
            const auto levelPerSample = modulated (levelDestinations[osc]);
            if (steady)
            {
                const auto skipped = (osc >= 3 && ! active[osc] && oscEnableSmooth[osc].getCurrentValue() <= 0.0005f)
                                     || (osc < 3 && idleOsc[osc]);
                auto render = false;
                auto renderLevel = 0.0f, enable = 0.0f, levelTarget = 0.0f;
                WavetableOscillator::FrameRead frames {};
                if (! skipped)
                {
                    enable = oscEnableSmooth[osc].getNextValue();
                    const auto level = osc == 2 ? juce::jlimit (0.0f, 1.0f, levelSmooth[osc].getNextValue() + levelMods[0][osc]) : 0.0f;
                    render = (active[osc] || enable > 0.0005f) && (osc != 2 || level > 0.0f);
                    if (render)
                    {
                        const auto frame = juce::jlimit (0.0f, 1.0f, frameSmooth[osc].getNextValue() + frameMods[0][osc]);
                        levelTarget = levelSmooth[osc].getNextValue();
                        renderLevel = osc == 2 ? level : juce::jlimit (0.0f, 1.0f, levelTarget + levelMods[0][osc]);
                        frames = WavetableOscillator::frameReadFor (oscBank[osc].getWavetable(), frame, settings.frameBlend == FrameBlend::stepped);
                    }
                }
                const auto operatorEg = usesOperatorEg (osc);
                const auto egRate = operatorEg && settings.tuneMode != OscTuning::Fixed;
                const auto levelEnable = renderLevel * enable;
                const auto oversample = params.quality == 2;
                const auto out = params.oscOut[osc];
                const auto keyGain = keyLevelGain[osc];
                const auto feedbackType = settings.feedbackType;
                auto& bank = oscBank[osc];
                // Nothing to render or hear: its FM input isn't needed.
                if (skipped || ! render)
                {
                    for (int slot = 0; slot < n; ++slot)
                    {
                        monoOut[osc][slot] = 0.0f;
                        prev[osc][slot + 1] = 0.0f;
                        if (anyAltFeedback)
                        {
                            if (feedbackType == FmFeedback::Dx7)
                                feedbackHistory[osc] = 0.0f;
                            else if (feedbackType == FmFeedback::Filtered)
                            {
                                const auto average = 0.5f * (0.0f + feedbackHistory[osc]);
                                feedbackFiltered[osc] += feedbackCoeff[osc] * (average - feedbackFiltered[osc]);
                                feedbackHistory[osc] = 0.0f;
                            }
                        }
                    }
                    continue;
                }
                const auto single = render && bank.canRenderSingle (oversample);
                const auto singleRows = single ? bank.singleRead (frames) : UnisonBank::SingleRead {};

                // An operator that doesn't hear itself: its FM input for the
                // whole chunk first (every source it hears is done), then a
                // lean render loop: the same sums in the same order. (Its own
                // row reads as zero there: a silent cell's term is a zero
                // either way, and the input's + 0.0 drops the zero's sign.)
                // (With DX7 or filtered feedback the input is the sum over
                // its sources, its own term a zero while its cell is.)
                const auto altInput = anyAltFeedbackInput && altFeedbackInput[osc];
                auto selfFed = ! cellIsSilent (osc, osc);
                for (int k = 0; k < numOpCells; ++k)
                    selfFed = selfFed || opCells[k] == osc;
                for (int k = 0; k < numAltSources[osc]; ++k)
                    selfFed = selfFed || altSources[osc][k] == osc;
                if (render && ! selfFed)
                {
                    static constexpr float zeroRow[maxChunk + 1] {};
                    const float* rows[numOsc];
                    for (int source = 0; source < numOsc; ++source)
                        rows[source] = source == osc ? zeroRow : prev[source];

                    double fmIn[maxChunk];
                    if (altInput)
                    {
                        for (int slot = 0; slot < n; ++slot)
                            fmIn[slot] = 0.0;
                        for (int k = 0; k < numAltSources[osc]; ++k)
                        {
                            const auto source = altSources[osc][k];
                            const auto* row = rows[source];
                            if (source >= 3 || osc >= 3)
                            {
                                const auto amount = (double) params.fmMatrix[source][osc];
                                for (int slot = 0; slot < n; ++slot)
                                    fmIn[slot] += amount * (double) row[slot];
                            }
                            else
                                for (int slot = 0; slot < n; ++slot)
                                    fmIn[slot] += (double) (params.fmMatrix[source][osc] + legacyMods[slot][source * 3 + osc]) * (double) row[slot];
                        }
                    }
                    else if (osc == 0)
                        for (int slot = 0; slot < n; ++slot)
                        {
                            const auto* m = legacyMods[slot];
                            const auto fmAmount = params.fmAmount + m[3];
                            const auto fmFeedback = params.fmFeedback + m[0];
                            const auto fm3to1 = params.fmMatrix[2][0] + m[6];
                            fmIn[slot] = (double) (fmAmount * rows[1][slot] + fmFeedback * rows[0][slot]) + (double) (fm3to1 * rows[2][slot]);
                        }
                    else if (osc < 3)
                        for (int slot = 0; slot < n; ++slot)
                        {
                            const auto* m = legacyMods[slot];
                            fmIn[slot] = (double) ((params.fmMatrix[0][osc] + m[osc]) * rows[0][slot]
                                                   + (params.fmMatrix[1][osc] + m[3 + osc]) * rows[1][slot]
                                                   + (params.fmMatrix[2][osc] + m[6 + osc]) * rows[2][slot]);
                        }
                    else
                        for (int slot = 0; slot < n; ++slot)
                            fmIn[slot] = 0.0;
                    for (int k = 0; k < (altInput ? 0 : numOpCells); ++k)
                    {
                        const auto amount = (double) params.fmMatrix[opCells[k]][osc];
                        const auto* row = rows[opCells[k]];
                        for (int slot = 0; slot < n; ++slot)
                            fmIn[slot] += amount * (double) row[slot];
                    }

                    const auto constantGain = ! levelPerSample && ! alternateAmpRouting;
                    const auto fixedGain = levelEnable * 1.0f * keyGain * 1.0f;
                    const auto carrierScale = operatorEg ? dx7CarrierScale : 1.0f;
                    for (int slot = 0; slot < n; ++slot)
                    {
                        const auto rate = egRate ? dx7Rates[slot] : 1.0;
                        auto gain = fixedGain;
                        if (! constantGain)
                        {
                            const auto selectedEnv = operatorEg ? dx7Gains[slot][osc] * dx7LevelScale : selectedEnvs[slot][osc];
                            const auto slotLevelEnable = levelPerSample ? juce::jlimit (0.0f, 1.0f, levelTarget + levelMods[slot][osc]) * enable
                                                                        : levelEnable;
                            gain = slotLevelEnable * (alternateAmpRouting ? selectedEnv : 1.0f) * keyGain * 1.0f;
                        }
                        const auto sums = single ? bank.renderSingle (singleRows, fmIn[slot] + 0.0, rate)
                                                 : bank.render (fmIn[slot] + 0.0, frames, rate, oversample);
                        const auto oscMono = 0.0f + sums.mono * gain;
                        if (out)
                        {
                            const auto heard = operatorEg ? gain * carrierScale : gain;
                            heardL[osc][slot][0] = sums.left * heard;
                            heardR[osc][slot][0] = sums.right * heard;
                            heardCount[osc][slot] = 1;
                        }
                        monoOut[osc][slot] = oscMono;
                        const auto clamped = juce::jlimit (-2.0f, 2.0f, oscMono);
                        prev[osc][slot + 1] = clamped;
                        if (anyAltFeedback)
                        {
                            if (feedbackType == FmFeedback::Dx7)
                                feedbackHistory[osc] = clamped;
                            else if (feedbackType == FmFeedback::Filtered)
                            {
                                const auto average = 0.5f * (clamped + feedbackHistory[osc]);
                                feedbackFiltered[osc] += feedbackCoeff[osc] * (average - feedbackFiltered[osc]);
                                feedbackHistory[osc] = clamped;
                            }
                        }
                    }
                    continue;
                }

                for (int slot = 0; slot < n; ++slot)
                {
                    const auto* m = legacyMods[slot];
                    const auto* p = prev;
                    double fmInput = 0.0;
                    if (osc == 0)
                    {
                        const auto fmAmount = params.fmAmount + m[3];
                        const auto fmFeedback = params.fmFeedback + m[0];
                        const auto fm3to1 = params.fmMatrix[2][0] + m[6];
                        fmInput = (double) (fmAmount * p[1][slot] + fmFeedback * p[0][slot]) + (double) (fm3to1 * p[2][slot]);
                    }
                    else if (osc == 1)
                        fmInput = (double) ((params.fmMatrix[0][1] + m[1]) * p[0][slot]
                                            + (params.fmMatrix[1][1] + m[4]) * p[1][slot]
                                            + (params.fmMatrix[2][1] + m[7]) * p[2][slot]);
                    else if (osc == 2)
                        fmInput = (double) ((params.fmMatrix[0][2] + m[2]) * p[0][slot]
                                            + (params.fmMatrix[1][2] + m[5]) * p[1][slot]
                                            + (params.fmMatrix[2][2] + m[8]) * p[2][slot]);

                    for (int k = 0; k < numOpCells; ++k)
                        fmInput += (double) params.fmMatrix[opCells[k]][osc] * (double) p[opCells[k]][slot];

                    if (anyAltFeedbackInput)
                    {
                        if (altFeedbackInput[osc])
                        {
                            auto sum = 0.0;
                            for (int k = 0; k < numAltSources[osc]; ++k)
                            {
                                const auto source = altSources[osc][k];
                                const auto amount = source >= 3 || osc >= 3 ? params.fmMatrix[source][osc]
                                                                            : params.fmMatrix[source][osc] + m[source * 3 + osc];
                                sum += (double) amount * (double) p[source][slot];
                            }
                            const auto self = (double) (osc >= 3 ? params.fmMatrix[osc][osc] : params.fmMatrix[osc][osc] + m[osc * 4]);
                            if (feedbackType == FmFeedback::Filtered)
                                sum += self * (double) feedbackFiltered[osc];
                            else if (feedbackType == FmFeedback::Dx7)
                                sum += self * (0.5 * ((double) p[osc][slot] + (double) feedbackHistory[osc]));
                            fmInput = sum;
                        }
                        fmInput += 0.0;
                    }

                    auto oscMono = 0.0f;
                    if (render)
                    {
                        const auto rate = 1.0 * (egRate ? dx7Rates[slot] : 1.0);
                        const auto selectedEnv = operatorEg ? dx7Gains[slot][osc] * dx7LevelScale : selectedEnvs[slot][osc];
                        const auto slotLevelEnable = levelPerSample ? juce::jlimit (0.0f, 1.0f, levelTarget + levelMods[slot][osc]) * enable
                                                                    : levelEnable;
                        const auto gain = slotLevelEnable * (alternateAmpRouting ? selectedEnv : 1.0f) * keyGain * 1.0f;
                        const auto sums = single ? bank.renderSingle (singleRows, fmInput + 0.0, rate)
                                                 : bank.render (fmInput + 0.0, frames, rate, oversample);
                        oscMono += sums.mono * gain;
                        if (out)
                        {
                            const auto heard = operatorEg ? gain * dx7CarrierScale : gain;
                            heardL[osc][slot][0] = sums.left * heard;
                            heardR[osc][slot][0] = sums.right * heard;
                            heardCount[osc][slot] = 1;
                        }
                    }

                    monoOut[osc][slot] = oscMono;
                    const auto clamped = skipped ? 0.0f : juce::jlimit (-2.0f, 2.0f, oscMono);
                    prev[osc][slot + 1] = clamped;
                    if (anyAltFeedback)
                    {
                        if (feedbackType == FmFeedback::Dx7)
                            feedbackHistory[osc] = clamped;
                        else if (feedbackType == FmFeedback::Filtered)
                        {
                            const auto average = 0.5f * (clamped + feedbackHistory[osc]);
                            feedbackFiltered[osc] += feedbackCoeff[osc] * (average - feedbackFiltered[osc]);
                            feedbackHistory[osc] = clamped;
                        }
                    }
                }
                continue;
            }

            for (int slot = 0; slot < n; ++slot)
            {
                const auto* p = &prev[0][0];
                const auto previous = [&prev, slot] (int source) { return prev[source][slot]; };
                juce::ignoreUnused (p);
                const auto* m = legacyMods[slot];
                const auto amountAt = [this, m] (int source, int target)
                {
                    const auto amount = params.fmMatrix[source][target];
                    if (source >= 3 || target >= 3)
                        return amount;
                    return amount + m[source * 3 + target];
                };

                // The FM input, as the per-sample loop sums it.
                double fmInput = 0.0;
                if (osc == 0)
                {
                    const auto fmAmount = params.fmAmount + m[3];
                    const auto fmFeedback = params.fmFeedback + m[0];
                    const auto fm3to1 = params.fmMatrix[2][0] + m[6];
                    fmInput = (double) (fmAmount * previous (1) + fmFeedback * previous (0)) + (double) (fm3to1 * previous (2));
                }
                else if (osc == 1)
                    fmInput = (double) ((params.fmMatrix[0][1] + m[1]) * previous (0)
                                        + (params.fmMatrix[1][1] + m[4]) * previous (1)
                                        + (params.fmMatrix[2][1] + m[7]) * previous (2));
                else if (osc == 2)
                    fmInput = (double) ((params.fmMatrix[0][2] + m[2]) * previous (0)
                                        + (params.fmMatrix[1][2] + m[5]) * previous (1)
                                        + (params.fmMatrix[2][2] + m[8]) * previous (2));

                for (int cell = 0; cell < numExtendedFmCells; ++cell)
                    if (extendedFmCells[cell][1] == osc)
                        fmInput += (double) amountAt (extendedFmCells[cell][0], osc) * (double) previous (extendedFmCells[cell][0]);

                if (anyAltFeedbackInput)
                {
                    if (altFeedbackInput[osc])
                    {
                        auto sum = 0.0;
                        for (int k = 0; k < numAltSources[osc]; ++k)
                        {
                            const auto source = altSources[osc][k];
                            sum += (double) amountAt (source, osc) * (double) previous (source);
                        }
                        const auto self = (double) amountAt (osc, osc);
                        if (settings.feedbackType == FmFeedback::Filtered)
                            sum += self * (double) feedbackFiltered[osc];
                        else if (settings.feedbackType == FmFeedback::Dx7)
                            sum += self * (0.5 * ((double) previous (osc) + (double) feedbackHistory[osc]));
                        fmInput = sum;
                    }
                    fmInput += 0.0; // the (empty) cross term
                }

                if (anyNoiseOperator)
                    fmInput += (double) (params.fmNoise[osc] * fmNoise[slot]);

                auto oscMono = 0.0f;
                auto skipped = false;
                if (osc >= 3 && ! active[osc] && oscEnableSmooth[osc].getCurrentValue() <= 0.0005f)
                    skipped = true;
                else if (osc < 3 && idleOsc[osc])
                    skipped = true;

                if (! skipped)
                {
                    const auto enable = oscEnableSmooth[osc].getNextValue();
                    const auto level = osc == 2 ? juce::jlimit (0.0f, 1.0f, levelSmooth[osc].getNextValue() + levelMods[slot][osc])
                                                : 0.0f;
                    const auto shouldRender = (active[osc] || enable > 0.0005f) && (osc != 2 || level > 0.0f);

                    if (shouldRender)
                    {
                        const auto frame = juce::jlimit (0.0f, 1.0f, frameSmooth[osc].getNextValue() + frameMods[slot][osc]);
                        const auto renderLevel = osc == 2 ? level : juce::jlimit (0.0f, 1.0f,
                                                                                   levelSmooth[osc].getNextValue() + levelMods[slot][osc]);
                        const auto phase = fmPhase (fmInput) + 0.0;
                        const auto operatorEg = usesOperatorEg (osc);
                        const auto rate = fmRate (fmInput)
                                          * (operatorEg && settings.tuneMode != OscTuning::Fixed ? dx7Rates[slot] : 1.0);
                        const auto frames = WavetableOscillator::frameReadFor (oscBank[osc].getWavetable(), frame, settings.frameBlend == FrameBlend::stepped);
                        const auto selectedEnv = operatorEg ? dx7Gains[slot][osc] * dx7LevelScale : selectedEnvs[slot][osc];
                        const auto oscGain = renderLevel * enable * (alternateAmpRouting ? selectedEnv : 1.0f) * keyLevelGain[osc];

                        const auto sums = oscBank[osc].render (phase, frames, rate, params.quality == 2);
                        const auto gain = oscGain * 1.0f;
                        oscMono += sums.mono * gain;
                        if (params.oscOut[osc])
                        {
                            const auto heard = operatorEg ? gain * dx7CarrierScale : gain;
                            heardL[osc][slot][0] = sums.left * heard;
                            heardR[osc][slot][0] = sums.right * heard;
                            heardCount[osc][slot] = 1;
                        }
                    }
                    else
                    {
                        frameSmooth[osc].getNextValue();
                        if (osc != 2)
                            levelSmooth[osc].getNextValue();
                    }
                }

                monoOut[osc][slot] = oscMono;
                prev[osc][slot + 1] = juce::jlimit (-2.0f, 2.0f, oscMono);
                if (skipped)
                    prev[osc][slot + 1] = 0.0f;

                if (anyAltFeedback)
                {
                    if (settings.feedbackType == FmFeedback::Dx7)
                        feedbackHistory[osc] = prev[osc][slot + 1];
                    else if (settings.feedbackType == FmFeedback::Filtered)
                    {
                        const auto average = 0.5f * (prev[osc][slot + 1] + feedbackHistory[osc]);
                        feedbackFiltered[osc] += feedbackCoeff[osc] * (average - feedbackFiltered[osc]);
                        feedbackHistory[osc] = prev[osc][slot + 1];
                    }
                }
            }
        }

        for (int osc = 0; osc < numOsc; ++osc)
            previousOsc[osc] = prev[osc][n];

        // The buses, summed in the per-sample loop's order.
        for (int slot = 0; slot < n; ++slot)
        {
            float busL[VoiceRouting::maxBuses] {};
            float busR[VoiceRouting::maxBuses] {};

            for (int osc = 0; osc < numOsc; ++osc)
            {
                for (int u = 0; u < heardCount[osc][slot]; ++u)
                {
                    busL[routes[osc]] += heardL[osc][slot][u];
                    busR[routes[osc]] += heardR[osc][slot][u];
                }

                if (osc == 1)
                {
                    const auto ringMod = ringMods[slot];
                    if (ringMod > 0.0f && active[0] && active[1])
                        for (int bus = 0; bus < numBuses; ++bus)
                        {
                            busL[bus] += (busL[bus] * monoOut[1][slot] - busL[bus]) * ringMod;
                            busR[bus] += (busR[bus] * monoOut[1][slot] - busR[bus]) * ringMod;
                        }
                }
            }

            if (subOn[slot])
            {
                busL[routeSubOsc] += subL[slot];
                busR[routeSubOsc] += subR[slot];
            }
            if (noiseOn[slot])
            {
                busL[routeSubOsc] += noiseValue[slot];
                busR[routeSubOsc] += noiseValue[slot];
            }

            float bodyExciteL = 0.0f, bodyExciteR = 0.0f;
            if (params.resonatorOn && params.bodyType != 0 && resonatorAmount > 0.001f)
            {
                if (bodyStrikePending && params.bodyCouplingMode == 1 && params.bodyCoupling > 0.0f)
                {
                    bool hasString = false;
                    for (int osc = 0; osc < numOsc; ++osc)
                        hasString = hasString || (params.oscillatorEnabled[osc] && params.oscillators[osc].stringMode);
                    if (hasString)
                    {
                        materialBodyL.strike (params.bodyCoupling * velocityLevel);
                        materialBodyR.strike (params.bodyCoupling * velocityLevel);
                    }
                }
                bodyStrikePending = false;
                for (int bus = 0; bus < numBuses; ++bus)
                {
                    bodyExciteL += busL[bus];
                    bodyExciteR += busR[bus];
                }
                bodyExciteL += chunkLive[slot] * params.inputToBody;
                bodyExciteR += chunkLive[slot] * params.inputToBody;
                if (params.bodyCouplingMode == 1)
                {
                    bodyExciteL *= 0.2f;
                    bodyExciteR *= 0.2f;
                    for (int osc = 0; osc < numOsc; ++osc)
                    {
                        const auto drive = driveOut[osc][slot];
                        bodyExciteL += drive * params.bodyCoupling * 0.2f;
                        bodyExciteR += drive * params.bodyCoupling * 0.2f;
                    }
                }
            }

            std::copy (busL, busL + numBuses, chunkBusL[slot]);
            std::copy (busR, busR + numBuses, chunkBusR[slot]);
            chunkBodyL[slot] = bodyExciteL;
            chunkBodyR[slot] = bodyExciteR;
        }
    };

    classifySlowLfos();
    classifySlowEnvelopes();

    // Watch a released note for silence (not one the modal body, the
    // Operator EG or a live input can still sound through).
    watchReleaseSilence = ! disableReleaseSilence && ! noteHeld && ! monoPending && ! dx7Playing && params.liveInput == nullptr
                          && ! legacyMaterialBody();
    if (! watchReleaseSilence)
        releaseSilentSamples = 0;
    watchHeldSilence = ! disableHeldSilence && noteHeld && ! monoPending && ! dx7Playing && params.liveInput == nullptr
                       && ! legacyMaterialBody() && heldSleepAllowed();
    if (! watchHeldSilence)
        heldSilentSamples = 0;
    heldSourcePeak = 0.0f;

    for (int chunkStart = 0; chunkStart < numSamples;)
    {
        const auto chunkEnd = sampleFeedback ? chunkStart + 1 : juce::jmin (numSamples, (chunkStart | (maxChunk - 1)) + 1);

    if (useBlockSources)
        renderSourcesBlock (chunkStart, chunkEnd);
    else
    for (int i = chunkStart; i < chunkEnd; ++i)
    {
        const auto liveSample = params.liveInput != nullptr ? params.liveInput[startSample + i] : 0.0f;
        const auto ampValue = ampEnv.getNextSample();
        const auto control = (i & (modControlInterval - 1)) == 0;
        const auto filterValue = stepEnvelope (filterEnv, 1, control);
        const auto filter2Value = stepEnvelope (filter2Env, 2, control);
        const auto modValue = stepEnvelope (modEnv, 3, control);
        const auto env4Value = stepEnvelope (env4, 4, control);
        for (int index = 0; index < numNeededExtraEnvs; ++index)
        {
            const auto env = (size_t) neededExtraEnvs[index];
            extraEnvValues[env] = stepEnvelope (extraEnvs[env], (int) env + 5, control);
        }
        if (params.msegEnvNeeded)
            msegEnvValue = juce::jlimit (0.0f, 1.0f, envMseg.getNextValue());
        const float envelopeValues[17] { ampValue, filterValue, filter2Value, modValue, env4Value,
                                         extraEnvValues[0], extraEnvValues[1], extraEnvValues[2],
                                         extraEnvValues[3], extraEnvValues[4], extraEnvValues[5],
                                         extraEnvValues[6], extraEnvValues[7], extraEnvValues[8],
                                         extraEnvValues[9], extraEnvValues[10], msegEnvValue };

        // The Operator EG: step the operator gains every 64 samples, ramp between.
        float dx7Gain[6] {};
        auto dx7PitchRate = 1.0;
        if (dx7Playing)
        {
            if (dx7Count == 0)
            {
                // Settings turned (or modulated) since the note began.
                if (params.operatorEg != dx7Settings)
                {
                    dx7Settings = params.operatorEg;
                    dx7Note.update (dx7Settings);
                }
                dx7Previous = dx7Current;
                dx7Note.step();
                dx7Current = dx7Note.getGains();
            }
            const auto t = (float) (dx7Count + 1) / (float) Dx7::block;
            for (int op = 0; op < 6; ++op)
                dx7Gain[op] = dx7Previous[(size_t) op] + (dx7Current[(size_t) op] - dx7Previous[(size_t) op]) * t;
            dx7Count = (dx7Count + 1) % Dx7::block;
            if (const auto octaves = dx7Note.getPitchOctaves(); octaves != 0.0f)
                dx7PitchRate = std::exp2 ((double) octaves);
        }

        advanceVoiceLfos (i);
        evaluateModsRated (mods, i, ampValue, filterValue, filter2Value, modValue, env4Value);

        if ((i & 15) == 0)
            updateSubBlock (mods, filterValue, filter2Value, envelopeValues);
        else if (params.filter1Fm != 0.0f || params.filter2Fm != 0.0f
                 || mods[(int) D::Filter1Fm] != 0.0f || mods[(int) D::Filter2Fm] != 0.0f)
            updateFilterCoefficients (mods, filterValue, filter2Value);

        // Keep the old operator order: 1, sync, 2, ring, then 3.
        float busL[VoiceRouting::maxBuses] {};
        float busR[VoiceRouting::maxBuses] {};
        float oscMono[VoiceParams::numOscillators] {};
        float stringDrive[VoiceParams::numOscillators] {};

        const auto fmAmount = params.fmAmount + mods[(int) D::FmAmount];
        const auto fmFeedback = params.fmFeedback + mods[(int) D::FmFeedback];
        const auto fm3to1 = params.fmMatrix[2][0] + mods[(int) D::Fm3to1];
        double fmInput[VoiceParams::numOscillators] {
            (double) (fmAmount * previousOsc[1] + fmFeedback * previousOsc[0]) + (double) (fm3to1 * previousOsc[2]),
            (double) ((params.fmMatrix[0][1] + mods[(int) D::Fm1to2]) * previousOsc[0]
                      + (params.fmMatrix[1][1] + mods[(int) D::Fm2Feedback]) * previousOsc[1]
                      + (params.fmMatrix[2][1] + mods[(int) D::Fm3to2]) * previousOsc[2]),
            (double) ((params.fmMatrix[0][2] + mods[(int) D::Fm1to3]) * previousOsc[0]
                      + (params.fmMatrix[1][2] + mods[(int) D::Fm2to3]) * previousOsc[1]
                      + (params.fmMatrix[2][2] + mods[(int) D::Fm3Feedback]) * previousOsc[2])
        };

        for (int cell = 0; cell < numExtendedFmCells; ++cell)
        {
            const auto source = extendedFmCells[cell][0];
            const auto target = extendedFmCells[cell][1];
            fmInput[target] += (double) fmAmountAt (mods, source, target) * (double) previousOsc[source];
        }

        // Filtered and cross feedback replace an operator's plain self term.
        if (anyAltFeedbackInput)
        {
            double cross[VoiceParams::numOscillators] {};

            for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
            {
                const auto type = params.oscillators[osc].feedbackType;

                if (! altFeedbackInput[osc])
                    continue;

                auto sum = 0.0;
                for (int k = 0; k < numAltSources[osc]; ++k)
                {
                    const auto source = altSources[osc][k];
                    sum += (double) fmAmountAt (mods, source, osc) * (double) previousOsc[source];
                }

                const auto self = (double) fmAmountAt (mods, osc, osc);

                if (type == FmFeedback::Filtered)
                    sum += self * (double) feedbackFiltered[osc];
                else if (type == FmFeedback::Dx7)
                {
                    // The DX7's own feedback: the plain average of the last
                    // two samples.
                    sum += self * (0.5 * ((double) previousOsc[osc] + (double) feedbackHistory[osc]));
                }
                else
                {
                    const auto partner = FmFeedback::partnerOf (osc);
                    sum += self * (double) previousOsc[partner];
                    cross[partner] += self * (double) previousOsc[osc];
                }

                fmInput[osc] = sum;
            }

            for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
                fmInput[osc] += cross[osc];
        }

        // FM Noise: the NOISE card's generator (its type and colour, not its level
        // or switch). One draw a sample, which the heard noise below reuses.
        auto warpNoiseDraw = 0.0f, warpNoiseHeard = 0.0f;
        if (anyWarpNoise)
        {
            warpNoiseDraw = noiseGenerator.process (params.noiseType, random.nextFloat() * 2.0f - 1.0f);
            warpNoiseHeard = warpNoiseDraw;

            if (heardNoiseCoeff < 1.0f)
            {
                warpNoiseLow += heardNoiseCoeff * (warpNoiseDraw - warpNoiseLow);
                warpNoiseHeard = warpNoiseLow * heardNoiseGain;
            }
        }

        // The noise operator: coloured noise into any oscillator's FM input.
        if (anyNoiseOperator)
        {
            stepFmNoise (noiseCoeff);

            for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
                fmInput[osc] += (double) (params.fmNoise[osc] * fmNoiseState);
        }

        const auto fmPhase = [this] (double input) { return params.fmMode == 0 ? input : 0.0; };
        const auto fmRate = [this] (double input)
        {
            if (params.fmMode == 1)
                return 1.0 + 4.0 * input;

            if (params.fmMode == 2)
                return std::exp2 (juce::jlimit (-4.0, 4.0, 2.0 * input));

            return 1.0;
        };

        for (int v = 0; v < numVisitOscs; ++v)
        {
            const auto osc = visitOscs[v];
            if (osc >= 3 && ! active[osc] && oscEnableSmooth[osc].getCurrentValue() <= 0.0005f)
            {
                previousOsc[osc] = 0.0f;
                continue;
            }
            const auto& settings = params.oscillators[osc];
            const auto warpAmount = settings.pdEnv > 0
                                        ? juce::jlimit (0.0f, 1.0f, settings.warpAmount + mods[(int) warpDestinations[osc]]
                                                                        + settings.pdEnvAmount * envelopeValues[juce::jlimit (0, 16, settings.pdEnv - 1)])
                                        : juce::jlimit (0.0f, 1.0f, settings.warpAmount + mods[(int) warpDestinations[osc]]);
            const auto enable = oscEnableSmooth[osc].getNextValue();
            const auto level = osc == 2 ? juce::jlimit (0.0f, 1.0f,
                                                        levelSmooth[osc].getNextValue() + mods[(int) levelDestinations[osc]])
                                        : 0.0f;
            const auto shouldRender = (active[osc] || enable > 0.0005f) && (osc != 2 || level > 0.0f);

            if (shouldRender)
            {
                const auto frame = juce::jlimit (0.0f, 1.0f,
                                                 frameSmooth[osc].getNextValue() + mods[(int) frameDestinations[osc]]);
                const auto renderLevel = osc == 2 ? level : juce::jlimit (0.0f, 1.0f,
                                                   levelSmooth[osc].getNextValue() + mods[(int) levelDestinations[osc]]);
                const auto warpSource = osc == 0 ? previousOsc[1] : oscMono[0];
                const auto phase = fmPhase (fmInput[osc])
                                   + (settings.warpMode == Warp::Fm ? (double) (warpAmount * warpSource) : 0.0)
                                   + (settings.warpMode == Warp::NoiseFm ? (double) (warpAmount * warpNoiseHeard) : 0.0);
                // The Operator EG's pitch envelope and LFO move every ratio
                // operator it plays (a fixed-frequency one stays put, as on
                // the DX7).
                const auto operatorEg = usesOperatorEg (osc);
                const auto rate = fmRate (fmInput[osc])
                                  * (operatorEg && settings.tuneMode != OscTuning::Fixed ? dx7PitchRate : 1.0);
                const auto ring = settings.warpMode == Warp::Ring
                                      ? 1.0f + (warpSource - 1.0f) * warpAmount : 1.0f;
                const auto frames = WavetableOscillator::frameReadFor (oscBank[osc].getWavetable(), frame, settings.frameBlend == FrameBlend::stepped);

                auto stringSum = 0.0f;
                // The Operator EG's gain in place of an envelope (ENV 1 if the
                // note started before an oscillator picked it).
                const auto selectedEnv = operatorEg ? dx7Gain[osc] * dx7LevelScale
                                                    : envelopeValues[settings.ampEnv <= 16 ? settings.ampEnv : 0];
                const auto oscGain = renderLevel * enable * (alternateAmpRouting ? selectedEnv : 1.0f) * keyLevelGain[osc];
                const auto wavetable = ! settings.granularMode && ! settings.sampleMode && ! settings.stringMode
                                       && ! settings.liveMode;

                if (wavetable)
                {
                    const auto sums = oscBank[osc].render (phase, frames, rate, params.quality == 2);
                    const auto gain = oscGain * ring;
                    oscMono[osc] += sums.mono * gain;

                    if (params.oscOut[osc])
                    {
                        // Operator EG carriers are heard as Dexed scales them;
                        // the FM path above keeps the gain in cycles.
                        const auto heard = operatorEg ? gain * dx7CarrierScale : gain;
                        busL[routes[osc]] += sums.left * heard;
                        busR[routes[osc]] += sums.right * heard;
                    }
                }

                // Strings alone: the loop below without the other modes'
                // branches (the same sums in the same order).
                const auto stringsOnly = settings.stringMode && ! settings.liveMode && ! settings.granularMode
                                         && ! settings.sampleMode;
                if (stringsOnly)
                {
                    const auto trim = exciterTrimFor (settings.stringExcite, params.exciterLevels);
                    const auto match = trim != 1.0f;
                    const auto liveIn = params.inputToStrings > 0.0f;
                    const auto liveAmount = liveSample * params.inputToStrings;
                    const auto envFactor = alternateAmpRouting ? selectedEnv : 1.0f;
                    const auto keyGain = keyLevelGain[osc];
                    const auto fm = (float) fmInput[osc];
                    const auto out = params.oscOut[osc];
                    const auto route = routes[osc];
                    auto mono = oscMono[osc];
                    auto bl = busL[route], br = busR[route];
                    if (stringTwin[osc])
                    {
                        // A stereo route on one string: string 0 is the left
                        // side and its twin the right, each at the voice's
                        // own pan (identical while the route sits at 0).
                        auto& left = stringFor (osc, 0);
                        auto& right = stringFor (osc, 1);
                        if (liveIn)
                        {
                            left.addLiveInput (liveAmount);
                            right.addLiveInput (liveAmount);
                        }
                        // Both sides in one call (the same as two process() calls).
                        // A resting route: both sides are string 0.
                        float rawL, rawR;
                        if (twinAsleep[osc])
                            rawL = rawR = left.process (aftertouchValue, noteHeld, fm);
                        else
                            left.processPair (right, aftertouchValue, noteHeld, fm, rawL, rawR);
                        auto sideL = left.side(), sideR = twinAsleep[osc] ? sideL : right.side();
                        if (match)
                        {
                            rawL *= trim;
                            rawR *= trim;
                            sideL *= trim;
                            sideR *= trim;
                        }
                        if (watchHeldSilence)
                            heldSourcePeak = juce::jmax (heldSourcePeak, std::abs (rawL) + std::abs (sideL), std::abs (rawR) + std::abs (sideR));
                        const auto raw = 0.5f * (rawL + rawR);
                        stringSum += raw;

                        const auto gain = unisonGains[osc][0] * renderLevel * enable * envFactor * keyGain;
                        mono += raw * gain;
                        if (out)
                        {
                            const auto heard = operatorEg ? gain * dx7CarrierScale : gain;
                            bl += (rawL + sideL) * heard * panGainL[osc][0];
                            br += (rawR - sideR) * heard * panGainR[osc][0];
                        }
                    }
                    for (int u = 0; u < (stringTwin[osc] ? 0 : numOscUnison[osc]); ++u)
                    {
                        auto& string = stringFor (osc, u);
                        if (liveIn)
                            string.addLiveInput (liveAmount);
                        auto raw = string.process (aftertouchValue, noteHeld, fm);
                        auto side = string.side(); // (stereo exciters)
                        if (match)
                        {
                            raw *= trim;
                            side *= trim;
                        }
                        if (watchHeldSilence)
                            heldSourcePeak = juce::jmax (heldSourcePeak, std::abs (raw) + std::abs (side));
                        stringSum += raw;

                        const auto gain = unisonGains[osc][u] * renderLevel * enable * envFactor * keyGain;
                        mono += raw * gain;
                        if (out)
                        {
                            const auto heard = operatorEg ? gain * dx7CarrierScale : gain;
                            bl += (raw + side) * heard * panGainL[osc][u];
                            br += (raw - side) * heard * panGainR[osc][u];
                        }
                    }
                    oscMono[osc] = mono;
                    busL[route] = bl;
                    busR[route] = br;
                }

                for (int u = 0; u < (wavetable || stringsOnly ? 0 : numOscUnison[osc]); ++u)
                {
                    float raw = 0.0f;
                    float sampleL = 0.0f;
                    float sampleR = 0.0f;
                    float side = 0.0f; // (stereo exciters)

                    if (settings.liveMode)
                    {
                        raw = liveSample;
                    }
                    else if (settings.granularMode)
                    {
                        grains[osc][u].process (sampleL, sampleR);
                        raw = 0.5f * (sampleL + sampleR);
                    }
                    else if (settings.sampleMode)
                    {
                        sampleUnison[osc][u].process (sampleL, sampleR);
                        if (layerZone[osc] != nullptr)
                        {
                            float layerL = 0.0f, layerR = 0.0f;
                            sampleLayer[osc][u].process (layerL, layerR);
                            sampleL += layerL;
                            sampleR += layerR;
                        }
                        raw = 0.5f * (sampleL + sampleR);
                    }
                    else
                    {
                        if (params.inputToStrings > 0.0f)
                            stringFor (osc, u).addLiveInput (liveSample * params.inputToStrings);
                        auto& string = stringFor (osc, u);
                        raw = string.process (aftertouchValue, noteHeld, (float) fmInput[osc]);
                        side = string.side();
                        if (params.exciterLevels > 0)
                        {
                            const auto trim = exciterTrimFor (settings.stringExcite, params.exciterLevels);
                            raw *= trim;
                            side *= trim;
                        }
                        stringSum += raw;
                    }

                    const auto gain = unisonGains[osc][u] * renderLevel * enable
                                      * (alternateAmpRouting ? selectedEnv : 1.0f) * keyLevelGain[osc];
                    oscMono[osc] += raw * gain;

                    if (params.oscOut[osc])
                    {
                        // Heard at the Operator Env's carrier scale too, so
                        // picking it changes every mode's level alike.
                        const auto heard = operatorEg ? gain * dx7CarrierScale : gain;
                        busL[routes[osc]] += (settings.sampleMode ? sampleL : raw + side) * heard * panGainL[osc][u];
                        busR[routes[osc]] += (settings.sampleMode ? sampleR : raw - side) * heard * panGainR[osc][u];
                    }
                }

                // Coupled strings: the strings of one note share the bridge,
                // which soaks up their in-phase motion. The in-phase part of
                // the note dies fast (the prompt sound); as the detuned
                // strings drift apart, the rest rings on (the aftersound).
                if (settings.stringMode && ! settings.sampleMode && settings.couple > 0.0f && numOscUnison[osc] > 1)
                {
                    const auto count = juce::jmin (numOscUnison[osc], VoiceParams::maxBufferedUnison);
                    const auto bridge = stringSum / (float) juce::jmax (1, count);

                    for (int u = 0; u < count; ++u)
                        stringFor (osc, u).addBridgeInput (-settings.couple * settings.couple * 30.0f * PianoTuning::get().coupling * bridge);
                }
                if (settings.stringMode && ! settings.sampleMode)
                    stringDrive[osc] = stringSum / (float) juce::jmax (1, numOscUnison[osc]);
            }
            else
            {
                frameSmooth[osc].getNextValue();

                if (osc != 2)
                    levelSmooth[osc].getNextValue();
            }

            if (osc == 0 && params.hardSync && active[1] && active[0]
                && ! params.oscillators[0].stringMode && ! params.oscillators[1].stringMode
                && ! params.oscillators[0].sampleMode && ! params.oscillators[1].sampleMode
                && oscBank[0].lane0Wrapped())
                for (int u = 0; u < numOscUnison[1]; ++u)
                    oscBank[1].resetPhase (u, 0.0);

            if (osc == 1)
            {
                const auto ringMod = params.ringMod + mods[(int) D::RingMod];

                if (ringMod > 0.0f && active[0] && active[1])
                    for (int bus = 0; bus < numBuses; ++bus)
                    {
                        busL[bus] += (busL[bus] * oscMono[1] - busL[bus]) * ringMod;
                        busR[bus] += (busR[bus] * oscMono[1] - busR[bus]) * ringMod;
                    }

                previousOsc[0] = juce::jlimit (-2.0f, 2.0f, oscMono[0]);
                previousOsc[1] = juce::jlimit (-2.0f, 2.0f, oscMono[1]);
            }
            else if (osc >= 2)
            {
                previousOsc[osc] = juce::jlimit (-2.0f, 2.0f, oscMono[osc]);
            }
        }
        // The skipped oscillators' outputs, as the loop above would have
        // left them (nothing in it reads them).
        for (int v = 0; v < numSkippedOscs; ++v)
        {
            const auto osc = skippedOscs[v];
            if (osc == 1)
                previousOsc[0] = juce::jlimit (-2.0f, 2.0f, oscMono[0]);
            if (osc != 0 || skipsOsc1)
                previousOsc[osc] = 0.0f;
        }

        if (anyAltFeedback)
            for (int k = 0; k < numHistoryOscs; ++k)
                if (const auto osc = historyOscs[k]; params.oscillators[osc].feedbackType == FmFeedback::Dx7)
                    feedbackHistory[osc] = previousOsc[osc];
                else
                {
                    // The average of two samples (the DX7's feedback filter)
                    // through a gentle one-pole: calm, saw-like feedback.
                    const auto average = 0.5f * (previousOsc[osc] + feedbackHistory[osc]);
                    feedbackFiltered[osc] += feedbackCoeff[osc] * (average - feedbackFiltered[osc]);
                    feedbackHistory[osc] = previousOsc[osc];
                }

        // Dedicated sub: plain table at frame 0, centre pan.
        const auto subOscLevel = subOscLevelSmooth.getNextValue();
        const auto enableSubOsc = subOscEnableSmooth.getNextValue();

        if (subOscLevel > 0.0f && params.subOscTable != nullptr && (subOscActive || enableSubOsc > 0.0005f))
        {
            subOsc.setFramePosition (0.0f);
            const auto raw = colourSub (subOsc.getNextSample (0.0, subOscFrames), subOscColourSmooth.getNextValue());
            const auto gain = subOscLevel * enableSubOsc;
            busL[routeSubOsc] += raw * gain * panGainSubOscL * (alternateAmpRouting ? ampValue : 1.0f);
            busR[routeSubOsc] += raw * gain * panGainSubOscR * (alternateAmpRouting ? ampValue : 1.0f);
        }

        const auto noiseLevel = juce::jlimit (0.0f, 1.0f, noiseSmooth.getNextValue() + mods[(int) D::NoiseLevel])
                                * noiseEnableSmooth.getNextValue();

        if (noiseLevel > 0.0f)
        {
            auto value = (anyWarpNoise ? warpNoiseDraw : noiseGenerator.process (params.noiseType, random.nextFloat() * 2.0f - 1.0f)) * noiseLevel * 0.5f
                         * (alternateAmpRouting ? ampValue : 1.0f);
            // NOISE COLOUR under white: a one-pole low-pass, part of the
            // lost level made up (white skips it, so older patches are
            // unchanged).
            if (heardNoiseCoeff < 1.0f)
            {
                noiseLow += heardNoiseCoeff * (value - noiseLow);
                value = noiseLow * heardNoiseGain;
            }
            busL[routeSubOsc] += value;
            busR[routeSubOsc] += value;
        }

        float bodyExciteL = 0.0f, bodyExciteR = 0.0f;
        if (params.resonatorOn && params.bodyType != 0 && resonatorAmount > 0.001f)
        {
            if (bodyStrikePending && params.bodyCouplingMode == 1 && params.bodyCoupling > 0.0f)
            {
                bool hasString = false;
                for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
                    hasString = hasString || (params.oscillatorEnabled[osc] && params.oscillators[osc].stringMode);
                if (hasString)
                {
                    materialBodyL.strike (params.bodyCoupling * velocityLevel);
                    materialBodyR.strike (params.bodyCoupling * velocityLevel);
                }
            }
            bodyStrikePending = false;
            for (int bus = 0; bus < numBuses; ++bus)
            {
                bodyExciteL += busL[bus];
                bodyExciteR += busR[bus];
            }
            // M7.5: the live input rings the body too.
            bodyExciteL += liveSample * params.inputToBody;
            bodyExciteR += liveSample * params.inputToBody;
            if (params.bodyCouplingMode == 1)
            {
                bodyExciteL *= 0.2f;
                bodyExciteR *= 0.2f;
                for (int k = 0; k < numDrivingOscs; ++k)
                {
                    const auto drive = stringDrive[drivingOscs[k]];
                    bodyExciteL += drive * params.bodyCoupling * 0.2f;
                    bodyExciteR += drive * params.bodyCoupling * 0.2f;
                }
            }
        }

        if (params.bodyCouplingMode == 3 && params.bodyCoupling > 0.0f)
        {
            for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
            {
                if (! params.oscillatorEnabled[osc] || ! params.oscillators[osc].stringMode)
                    continue;
                auto otherDrive = 0.0f;
                auto otherCount = 0;
                for (int other = 0; other < VoiceParams::numOscillators; ++other)
                    if (other != osc && params.oscillatorEnabled[other] && params.oscillators[other].stringMode)
                    {
                        otherDrive += stringDrive[other];
                        ++otherCount;
                    }
                if (otherCount == 0)
                    continue;
                const auto count = stringsPlaying (osc);
                for (int u = 0; u < count; ++u)
                    stringFor (osc, u).addBridgeInput (0.015f * params.bodyCoupling * otherDrive / (float) otherCount);
            }
        }

        const auto slot = i - chunkStart;
        std::copy (busL, busL + numBuses, chunkBusL[slot]);
        std::copy (busR, busR + numBuses, chunkBusR[slot]);
        chunkBodyL[slot] = bodyExciteL;
        chunkBodyR[slot] = bodyExciteR;
        chunkLive[slot] = liveSample;
        chunkAmp[slot] = ampValue;
        chunkAmpLevel[slot] = mods[(int) D::AmpLevel];
        chunkWest[slot] = params.west.on && params.west.source != 0
                              ? juce::jlimit (0.0f, 1.0f, sourceValue ((Mod::Source) params.west.source, i, ampValue, filterValue,
                                                                       filter2Value, modValue, env4Value))
                              : 0.0f;

        lastAmpValue = ampValue;
        lastLifetimeValue = ampValue;
        if (operatorEgOwnsVoice())
            lastLifetimeValue = dx7Note.isActive() ? 1.0f : 0.0f;
        else if (alternateAmpRouting)
            for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
                if (params.oscillatorEnabled[osc])
                    lastLifetimeValue = juce::jmax (lastLifetimeValue,
                                                    usesOperatorEg (osc) ? (dx7Note.isActive() ? 1.0f : 0.0f)
                                                    : envelopeValues[params.oscillators[osc].ampEnv <= 16 ? params.oscillators[osc].ampEnv : 0]);
        lastFilterValue = filterValue;
        lastFilter2Value = filter2Value;
        lastModValue = modValue;
        lastEnv4Value = env4Value;
    }

    postChunk (chunkStart, chunkEnd - chunkStart);

        chunkStart = chunkEnd;
    }

    const auto samplePositionOf = [] (const SamplePlayer& player, const SampleData* data)
    {
        if (data == nullptr || data->getNumSamples() <= 0 || ! player.isActive())
            return -1.0f;

        return (float) juce::jlimit (0.0, 1.0, player.getSourcePosition() / (double) data->getNumSamples());
    };

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        lastSamplePosition[osc] = params.oscillators[osc].sampleMode
                                      ? samplePositionOf (sampleUnison[osc][0], sampleZone[osc] != nullptr ? sampleZone[osc]->data.get()
                                                                                                           : params.oscillators[osc].sample)
                                      : -1.0f;

    if (bodyTailSamplesRemaining > 0)
        bodyTailSamplesRemaining = juce::jmax (0, bodyTailSamplesRemaining - numSamples);

    if (fadeTotal > 0 && fadeRemaining == 0)
        stopNote (0.0f, false);

    // A released note whose output has stayed under -110 dB for about 90 ms
    // is over, even if its release time is not: free the voice now rather
    // than render silence (decayed strings, long releases).
    if (watchReleaseSilence)
    {
        releaseSilentSamples = releasePeak < releaseSilence ? releaseSilentSamples + numSamples : 0;
        releasePeak = 0.0f;
        if (releaseSilentSamples >= (int) (sampleRate * 0.09))
            stopNote (0.0f, false);
    }
    else if (watchHeldSilence)
    {
        // Silent at the output and at the strings themselves: an output
        // held at zero by the amp (its DELAY, an MSEG, a route into a
        // level) while the strings still ring is not a note that has died.
        const auto silent = releasePeak < releaseSilence && heldSourcePeak < releaseSilence;
        heldSilentSamples = silent ? heldSilentSamples + numSamples : 0;
        releasePeak = 0.0f;
        heldSourcePeak = 0.0f;
        if (heldSilentSamples >= (int) (sampleRate * 0.09))
        {
            heldAsleep = true;
            rememberFilterSettings();
        }
    }

    if (! hasActiveAmpEnvelope())
    {
        lastAmpValue = 0.0f;
        lastLifetimeValue = 0.0f;
        clearCurrentNote();
    }
}

void Voice::configureString (KarplusStrong& string, const VoiceParams::OscParams& settings, float side) const
{
    auto stiffness = settings.stringStiffness;
    auto damping = settings.stringDamping;
    auto decay = settings.stringDecay;
    auto excitePosition = settings.stringExcitationPosition;
    auto hammer = settings.hammerHardness;
    auto bowPressure = settings.bowPressure;

    // Stereo string routes: this string's side of them (every knob is 0..1).
    if (side != 0.0f)
    {
        namespace S = Mod::StereoString;
        const auto& offsets = settings.stringStereoOffsets;
        const auto moved = [side] (float value, float offset) { return juce::jlimit (0.0f, 1.0f, value + side * offset); };
        decay = moved (decay, offsets[S::decay]);
        damping = moved (damping, offsets[S::damping]);
        stiffness = moved (stiffness, offsets[S::stiffness]);
        excitePosition = moved (excitePosition, offsets[S::excitePosition]);
        hammer = moved (hammer, offsets[S::hammer]);
        bowPressure = moved (bowPressure, offsets[S::bowPressure]);
    }

    // Register map: short, stiff, bright treble strings; long, looser bass
    // strings that ring longer.
    if (settings.registerMap > 0.0f)
    {
        // The knob's whole travel: bass (-1, C2 and below) to treble (+1, C6
        // and above), twice as steep as before so the range is audible.
        const auto amount = settings.registerMap * 2.0f;
        const auto t = juce::jlimit (-1.0f, 1.0f, keyTrackValue * 2.0f);
        // Wound bass strings are less stiff than the plain treble ones.
        stiffness = juce::jlimit (0.0f, 1.0f, stiffness + amount * (0.16f + 0.35f * juce::jmax (0.0f, t)
                                                                         - 0.1f * juce::jmax (0.0f, -t)));
        damping = juce::jlimit (0.0f, 1.0f, damping - amount * 0.25f * t);
        decay = juce::jlimit (0.0f, 1.0f, decay - amount * 0.05f * t);
    }

    string.setParams (static_cast<KarplusStrong::Excite> (juce::jlimit (0, KarplusStrong::numExcites - 1, settings.stringExcite)),
                      settings.stringSustain, damping, decay);
    string.setPhysicalParams (stiffness, settings.stringPickup,
                              excitePosition, settings.stringPickHardness,
                              settings.stringPickPosition, settings.stringSlap);
    string.setBowAndBuzz (bowPressure, settings.bowSpeed, settings.bridgeBuzz, settings.fretRattle);
    string.setSplit (settings.stringSplit);
    string.setClick (settings.stringClick);
    string.setKeysParams (hammer, settings.damper);
    string.setEco (params.quality == 0);
    string.setFeedbackParams (settings.fbGain, settings.fbDistance);
    if (string.isElectric())
        string.setElectricParams (settings.epDistance, settings.epPosition);
}

bool Voice::operatorEgOwnsVoice() const
{
    if (! dx7Playing)
        return false;
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        if (params.oscillatorEnabled[osc] && params.oscillators[osc].ampEnv != OperatorEg::envelopeChoice)
            return false;
    return true;
}

bool Voice::hasActiveAmpEnvelope() const
{
    if (operatorEgOwnsVoice())
        return dx7Note.isActive();
    // The modal body rings on after the envelopes (up to 1.5 x its decay,
    // 10 s at most), but only while it can be heard: once its level is under
    // -110 dB (its wet gain is at most 2: under -104 dB out per voice) the
    // tail is over, and the voice ends with the envelopes. A body that never
    // rang (BODY AMOUNT at 0) holds no voice at all.
    constexpr auto bodySilence = 3.0e-6f;
    if (legacyMaterialBody() && bodyTailSamplesRemaining > 0
        && (materialBodyL.level() > bodySilence || materialBodyR.level() > bodySilence))
        return true;
    return envelopesActive();
}

bool Voice::envelopesActive() const
{
    bool anyOscillator = false;
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        if (! params.oscillatorEnabled[osc])
            continue;
        anyOscillator = true;
        if (usesOperatorEg (osc))
        {
            if (dx7Note.isActive())
                return true;
            continue;
        }
        const auto selected = juce::jlimit (0, 16, params.oscillators[osc].ampEnv);
        // The MSEG envelope lives as long as ENV 1 (it has no release).
        const auto active = selected == 0 || selected == 16 ? ampEnv.isActive()
                            : selected == 1 ? filterEnv.isActive()
                            : selected == 2 ? filter2Env.isActive()
                            : selected == 3 ? modEnv.isActive()
                            : selected == 4 ? env4.isActive()
                                            : extraEnvs[(size_t) (selected - 5)].isActive();
        if (active)
            return true;
    }
    return (params.subOscEnabled || (params.noiseEnabled && params.noiseLevel > 0.0f) || ! anyOscillator) && ampEnv.isActive();
}

// A ratio or fixed-pitch operator's frequency before pitch modulation. Semi
// and fine still apply on top; a fixed pitch ignores the note, bend and glide.
double Voice::oscFrequencyFactor (const VoiceParams::OscParams& settings) const
{
    const auto offset = std::exp2 ((settings.semitones + settings.cents / 100.0) / 12.0);

    if (settings.tuneMode == OscTuning::Fixed)
        return juce::jlimit (0.5, 20000.0, settings.fixedHz) * offset;

    return currentFrequency * settings.ratio * offset;
}

void Voice::updateSubBlock (const float* mods, float filterEnvValue, float filter2EnvValue, const float* envelopeValues)
{
    using D = Mod::Destination;

    const auto driftedFrequency = advanceGlideAndDrift (mods[(int) D::Drift]);

    // Warp amounts move at sub-block rate: the table-level choice behind them
    // is too costly to redo every sample.
    constexpr D warpDestinations[] { D::Osc1Warp, D::Osc2Warp, D::SubWarp,
                                     D::Osc4Warp, D::Osc5Warp, D::Osc6Warp };
    constexpr D pitchDestinations[] { D::Osc1Pitch, D::Osc2Pitch, D::SubPitch,
                                      D::Osc4Pitch, D::Osc5Pitch, D::Osc6Pitch };

    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        const auto& settings = params.oscillators[osc];

        if (osc >= 3 && ! params.oscillatorEnabled[osc]
            && oscEnableSmooth[osc].getCurrentValue() <= 0.0005f)
            continue;

        if (osc == 2 && settings.level <= 0.0f)
            continue;

        // The warp envelope (the CZ's DCW) opens both stages of the PD chain.
        const auto envelopeWarp = settings.pdEnv > 0
                                      ? settings.pdEnvAmount * envelopeValues[juce::jlimit (0, 16, settings.pdEnv - 1)]
                                      : 0.0f;
        const auto warp = settings.pdEnv > 0
                              ? juce::jlimit (0.0f, 1.0f, settings.warpAmount + mods[(int) warpDestinations[osc]] + envelopeWarp)
                              : juce::jlimit (0.0f, 1.0f, settings.warpAmount + mods[(int) warpDestinations[osc]]);
        const auto warp2 = juce::jlimit (0.0f, 1.0f, settings.warpAmount2 + envelopeWarp);

        double baseFreq;

        if (settings.tuneMode == OscTuning::Semitones)
        {
            // Scale quantise: the note plus SEMI snaps to the nearest scale note.
            auto scaleShift = 0.0;
            if (settings.scale > 0)
            {
                const auto target = pitchNote + (int) std::lround (settings.semitones
                                                                 + (osc == 2 ? (double) params.subOctaveOffset : 0.0));
                scaleShift = (double) (Scales::quantize (target, settings.scale, settings.scaleRoot) - target);
            }

            const auto pitch = std::exp2 ((settings.semitones + scaleShift + (osc == 2 ? (double) params.subOctaveOffset : 0.0)
                                           + settings.cents / 100.0 + bendSemitones
                                           + (double) mods[(int) pitchDestinations[osc]] * 48.0) / 12.0);
            baseFreq = driftedFrequency * pitch;
        }
        else
        {
            // Ratio operators follow the note (with bend, glide and drift);
            // fixed ones follow only their own pitch modulation.
            const auto modulation = std::exp2 ((double) mods[(int) pitchDestinations[osc]] * 4.0);
            baseFreq = settings.tuneMode == OscTuning::Fixed
                           ? oscFrequencyFactor (settings) * modulation
                           : settings.ratio * std::exp2 ((settings.semitones + settings.cents / 100.0 + bendSemitones) / 12.0)
                                 * driftedFrequency * modulation;
        }

        if (settings.feedbackType == FmFeedback::Filtered)
            feedbackCoeff[osc] = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi
                                                           * juce::jlimit (500.0, 0.45 * sampleRate, 8.0 * baseFreq)
                                                           / sampleRate));

        oscBank[osc].setWarp (settings.warpMode, warp);
        oscBank[osc].setWarp2 (settings.warpMode2, warp2);
        oscBank[osc].setSpreads (settings.frameSpread, settings.warpSpread);
        {
            // Stereo FRAME / WARP routes (2026-10-30): the bank plays its voices apart, one voice as a left / right twin.
            const auto frameIndex = Mod::StereoIndex::frame + osc, warpIndex = Mod::StereoIndex::warp + osc;
            const auto stereoFrame = stereoRouted[(size_t) frameIndex], stereoWarp = stereoRouted[(size_t) warpIndex];
            oscBank[osc].setStereo (stereoFrame || stereoWarp,
                                    stereoFrame ? mods[(int) Mod::Destination::Count + frameIndex] : 0.0f,
                                    stereoWarp ? mods[(int) Mod::Destination::Count + warpIndex] : 0.0f);
        }

        oscBank[osc].setFrequencies (baseFreq, unisonRatio[osc], numOscUnison[osc]);

        for (int u = 0; u < numOscUnison[osc]; ++u)
        {
            const auto frequencyU = baseFreq * unisonRatio[osc][u];

            if (u < VoiceParams::maxBufferedUnison)
            {
                stringFor (osc, u).setFrequency (frequencyU);
                if (stringTwin[osc])
                    stringFor (osc, 1).setFrequency (frequencyU);
                const auto* zone = sampleZone[osc];
                const auto* played = zone != nullptr ? zone->data.get() : settings.sample;
                sampleRatio[osc][u] = settings.sampleTuned ? frequencyU / (zone != nullptr ? zone->rootHz() : plainRootHz (settings.sampleRoot)) : 1.0;
                grains[osc][u].setPlaybackRatio (sampleRatio[osc][u]);
                const auto rateRatio = played != nullptr ? played->sampleRate / sampleRate : 1.0;
                // Step 40: SYNC speeds the sample to the host's tempo; with
                // STRETCH the note sets only the pitch. Neither: as before.
                if (settings.sampleSync == SampleTempo::off && ! settings.sampleStretch && settings.sampleEngine < 0)
                {
                    sampleUnison[osc][u].setPlaybackRatio (rateRatio * sampleRatio[osc][u]);
                }
                else
                {
                    const auto regionSeconds = played != nullptr && played->sampleRate > 0.0
                                                   ? (double) juce::jmax (0.0f, settings.sampleEnd - settings.sampleStart)
                                                         * played->getNumSamples() / played->sampleRate
                                                   : 0.0;
                    const auto tempo = SampleTempo::tempoRatio (settings.sampleSync, settings.sampleBpm, regionSeconds, params.hostBpm);
                    if (stretchCopy[osc] != nullptr)
                        // The copy is already the note's length and pitch: the
                        // live stretch adds only what changed since (bend,
                        // cents, tempo) and the length's rounding.
                        sampleUnison[osc][u].setRates (rateRatio * tempo * StretchRenderer::lengthForStep (stretchStep[osc]),
                                                       rateRatio * sampleRatio[osc][u] / std::exp2 (stretchSemis[osc] / 12.0));
                    else if (settings.sampleStretch || settings.sampleEngine >= 0)
                        sampleUnison[osc][u].setRates (rateRatio * tempo, rateRatio * sampleRatio[osc][u]);
                    else
                        sampleUnison[osc][u].setPlaybackRatio (rateRatio * sampleRatio[osc][u] * tempo);
                }

                // Step 37: the blended velocity layer, at its own root and rate.
                if (const auto* layer = layerZone[osc]; layer != nullptr && layer->data->sampleRate > 0.0)
                {
                    const auto layerRate = layer->data->sampleRate / sampleRate;
                    const auto layerPitch = settings.sampleTuned ? frequencyU / layer->rootHz() : 1.0;
                    const auto layerSeconds = (double) juce::jmax (0.0f, settings.sampleEnd - settings.sampleStart)
                                              * layer->data->getNumSamples() / layer->data->sampleRate;
                    const auto layerTempo = settings.sampleSync == SampleTempo::off
                                                ? 1.0
                                                : SampleTempo::tempoRatio (settings.sampleSync, settings.sampleBpm, layerSeconds, params.hostBpm);
                    if (settings.sampleStretch || settings.sampleEngine >= 0)
                        sampleLayer[osc][u].setRates (layerRate * layerTempo, layerRate * layerPitch);
                    else
                        sampleLayer[osc][u].setPlaybackRatio (layerRate * layerPitch * layerTempo);
                }
            }
        }
    }

    if (params.subOscLevel > 0.0f)
        subOsc.setFrequency (driftedFrequency * std::exp2 (((double) params.subOscOctave + bendSemitones) / 12.0));

    tuneBody (driftedFrequency);

    updateFilterCoefficients (mods, filterEnvValue, filter2EnvValue);
}

// One sub-block's glide and drift (the drift's random walk steps here, so it
// stays in step whether the voice renders in full or only its body's tail).
double Voice::advanceGlideAndDrift (float driftMod)
{
    currentFrequency += (baseFrequency - currentFrequency) * (double) glideCoeff;

    auto driftFactor = 1.0;
    const auto drift = juce::jlimit (0.0f, 1.0f, params.drift + driftMod);

    if (drift > 0.0f)
    {
        if (driftRandom.nextFloat() > 0.995f)
            driftTarget = driftRandom.nextFloat() * 2.0f - 1.0f;

        driftValue += (driftTarget - driftValue) * 0.002f;
        driftFactor = std::exp2 ((double) driftValue * (double) drift * 0.25 / 12.0);
    }

    return currentFrequency * driftFactor;
}

// The physical body as a stage among the routing cables (VoiceRouting::body).
bool Voice::bodyIsInChain() const
{
    return params.routing.on && params.routing.used[(size_t) VoiceRouting::body];
}

// A material body outside the chain: struck by the strings, ringing beside
// the filters and after the amp, as in every older patch.
// Held sleep is only for notes whose every sound dies away by itself once
// struck: physical strings with a one-off excitation (no bow, external
// drive, feedback amp or SUSTAIN), the Modal Piano, nothing else sounding
// (wavetable, sample, granular or live oscillators, noise) and no live
// input into the strings.
bool Voice::heldSleepAllowed() const
{
    using E = KarplusStrong::Excite;
    if (params.noiseEnabled && params.noiseLevel > 0.0f)
        return false;
    if (params.inputToStrings > 0.0f)
        return false;
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
    {
        if (params.fmNoise[osc] > 0.0f)
            return false;
        if (! params.oscillatorEnabled[osc])
            continue;
        const auto& settings = params.oscillators[osc];
        if (! settings.stringMode || settings.liveMode || settings.granularMode || settings.sampleMode)
            return false;
        const auto excite = (E) settings.stringExcite;
        if (excite == E::Bow || excite == E::External || excite == E::Feedback || settings.stringSustain > 0.0f)
            return false;
    }
    // A route into a filter or the WEST gate could push it into
    // self-oscillation while the note sleeps (its source doesn't run then).
    const auto& filterDestinations = filterDestinationMarks();
    for (int slot = 0; slot < params.numModSlots; ++slot)
    {
        const auto d = params.modSlots[slot].destination;
        if (! params.modSlots[slot].bypass && d >= 0 && d < (int) filterDestinations.size() && filterDestinations[(size_t) d])
            return false;
    }
    return true;
}

// The destinations that move a filter or the WEST gate (held sleep). Built
// once, by the first Voice (on the message thread), not on the audio thread.
const std::vector<bool>& Voice::filterDestinationMarks()
{
    static const auto marks = []
    {
        using D = Mod::Destination;
        std::vector<bool> table ((size_t) Mod::getNumDestinations(), false);
        for (const auto d : { D::Filter1Cutoff, D::Filter1Reso, D::Filter2Cutoff, D::Filter2Reso, D::Filter1Drive, D::Filter2Drive,
                              D::Filter1Env, D::Filter2Env, D::Filter1Fm, D::Filter2Fm, D::Filter1Morph, D::Filter2Morph })
            table[(size_t) d] = true;
        const auto& list = Mod::getParamDestinations();
        for (int i = 0; i < (int) list.size(); ++i)
        {
            const juce::String id (list[(size_t) i].id);
            const auto d = Mod::paramDestinationFor (i);
            if (d >= 0 && d < (int) table.size() && (id.startsWith ("f1_") || id.startsWith ("f2_") || id.startsWith ("west_") || id.startsWith ("filter")))
                table[(size_t) d] = true;
        }
        return table;
    }();
    return marks;
}

void Voice::rememberFilterSettings()
{
    sleptFilter1 = params.filter1;
    sleptFilter2 = params.filter2;
    sleptWest = params.west;
    sleptFilterFm[0] = params.filter1Fm;
    sleptFilterFm[1] = params.filter2Fm;
    sleptOscillators = enabledOscillators();
}

// Which oscillators are on, one bit each: one switched on (or back on) wakes
// a sleeping note, as its string may still hold what it last played.
int Voice::enabledOscillators() const
{
    auto mask = 0;
    for (int osc = 0; osc < VoiceParams::numOscillators; ++osc)
        if (params.oscillatorEnabled[osc])
            mask |= 1 << osc;
    return mask;
}

bool Voice::filterSettingsMoved() const
{
    const auto filterMoved = [] (const VoiceParams::FilterParams& a, const VoiceParams::FilterParams& b)
    {
        return a.type != b.type || a.slope24 != b.slope24 || a.cutoffHz != b.cutoffHz || a.resonance != b.resonance
               || a.drive != b.drive || a.envAmount != b.envAmount || a.keyTrack != b.keyTrack || a.morph != b.morph;
    };
    const auto& w = params.west;
    const auto& v = sleptWest;
    return filterMoved (params.filter1, sleptFilter1) || filterMoved (params.filter2, sleptFilter2)
           || params.filter1Fm != sleptFilterFm[0] || params.filter2Fm != sleptFilterFm[1]
           || w.on != v.on || w.position != v.position || w.fold != v.fold || w.symmetry != v.symmetry || w.stages != v.stages
           || w.mode != v.mode || w.decay != v.decay || w.resonance != v.resonance || w.strike != v.strike || w.open != v.open
           || w.source != v.source;
}

bool Voice::legacyMaterialBody() const
{
    return params.resonatorOn && params.bodyType != 0 && ! bodyIsInChain();
}

void Voice::tuneBody (double driftedFrequency)
{
    using D = Mod::Destination;

    if (params.resonatorOn && params.resonatorAmount + blockMod (D::ResAmount) > 0.001f)
    {
        const auto offset = juce::jlimit (-24.0f, 24.0f, params.resonatorOffset + blockMod (D::ResOffset) * Mod::Range::resOffsetSemitones);
        if (params.bodyType == 0)
        {
            resonatorL.setTuning (driftedFrequency, offset, params.resonatorKeytrack);
            resonatorR.setTuning (driftedFrequency, offset, params.resonatorKeytrack);
        }
        else
        {
            const auto noteHz = (driftedFrequency * (double) params.resonatorKeytrack
                                 + 220.0 * (1.0 - (double) params.resonatorKeytrack))
                                * std::exp2 ((double) offset / 12.0);
            const auto decay = juce::jlimit (0.0f, 1.0f, params.resonatorDecay + blockMod (D::ResDecay));
            materialBodyL.configure (params.bodyType - 1, params.bodyMaterial, params.bodySize,
                                     decay, noteHz, params.quality, -1.0f);
            materialBodyR.configure (params.bodyType - 1, params.bodyMaterial, params.bodySize,
                                     decay, noteHz, params.quality, 1.0f);
        }
    }
}

// The body's tail alone: every envelope is over, so the sources and filters
// add exact zeros (their output and the body's drive are both scaled by the
// finished amp envelope) and only the body rings. The same sums as the full
// render, without computing the zeros. AMP LEVEL modulation is read once per
// block here (per sample in the full render).
void Voice::renderBodyTail (float* left, float* right, int startSample, int numSamples, float wetGain)
{
    using D = Mod::Destination;
    for (int i = 0; i < numSamples; ++i)
    {
        if ((i & 15) == 0)
            tuneBody (advanceGlideAndDrift (blockMod (D::Drift)));
        if (fadeTotal > 0)
            ampGainFade = fadeRemaining > 0 ? (float) --fadeRemaining / (float) fadeTotal : 0.0f;
        const auto wetL = materialBodyL.process (0.0f) * wetGain * ampGainFade;
        const auto wetR = materialBodyR.process (0.0f) * wetGain * ampGainFade;
        if (right != nullptr)
        {
            left[startSample + i] += wetL;
            right[startSample + i] += wetR;
        }
        else
        {
            left[startSample + i] += wetL;
            left[startSample + i] += wetR;
        }
    }
}

void Voice::updateFilterCoefficients (const float* mods, float filterEnvValue, float filter2EnvValue)
{
    using D = Mod::Destination;

    const auto velocityEnvScale = 1.0f - params.filterVelocity + params.filterVelocity * velocityLevel;

    const auto fmOctaves1 = (double) (params.filter1Fm + mods[(int) D::Filter1Fm] * Mod::Range::filterFm) * (double) previousOsc[1] * 4.0;
    const auto fmOctaves2 = (double) (params.filter2Fm + mods[(int) D::Filter2Fm] * Mod::Range::filterFm) * (double) previousOsc[1] * 4.0;

    const auto keyOctaves1 = (double) params.filter1.keyTrack * (double) keyTrackOctaves;
    const auto envOctaves1 = (double) (params.filter1.envAmount + mods[(int) D::Filter1Env] * Mod::Range::filterEnvOctaves)
                             * (double) filterEnvValue * (double) velocityEnvScale;
    const auto cutoff1 = juce::jlimit (20.0, sampleRate * 0.45,
                                       (double) params.filter1.cutoffHz
                                           * std::exp2 (keyOctaves1 + envOctaves1 + fmOctaves1
                                                        + (double) mods[(int) D::Filter1Cutoff] * (double) Mod::Range::cutoffOctaves));
    const auto reso1 = juce::jlimit (0.0f, 1.0f, params.filter1.resonance + mods[(int) D::Filter1Reso]);

    // Filter 1 left wide open (the piano presets) gets the same bypass as
    // Filter 2 below, but only while nothing moves it: a swept Filter 1
    // crossing 19 kHz would restart from rest on each crossing.
    // Macros, the wheel and per-note sources (velocity, key) are fine: they
    // cross rarely, and a crossing fades between the two.
    const auto filter1Static = [this]
    {
        if (params.filter1.envAmount != 0.0f)
            return false;
        for (int i = 0; i < params.numModSlots; ++i)
        {
            const auto& slot = params.modSlots[i];
            const auto destination = slot.destination;
            if (destination != (int) D::Filter1Cutoff && destination != (int) D::Filter1Env
                && destination != (int) D::Filter1Reso && destination != (int) D::Filter1Fm)
                continue;
            using S = Mod::Source;
            const auto source = slot.source;
            const auto userOrNote = source == S::Velocity || source == S::KeyTrack || source == S::ModWheel
                                    || source == S::Aftertouch || source == S::Expression
                                    || (source >= S::Macro1 && source <= S::Macro4)
                                    || (source >= S::Macro5 && source <= S::Macro8);
            if (! userOrNote || destination == (int) D::Filter1Fm)
                return false;
        }
        return true;
    };
    const auto open1 = ! disableOpenFilterBypass && params.filter1.type == FilterType::LowPass
                       && ! params.filter1.slope24 && cutoff1 >= openFilterHz && reso1 <= 0.3f && fmOctaves1 == 0.0
                       && ! filterStereo (0) && filter1Static();

    if (open1 != filter1Open)
    {
        // The new model starts from rest; once Filter 1 has played, the old
        // one keeps its state and fades out (postChunk).
        filter1Fade = filter1Ran ? filterFadeLength : 0;
        filter1FadeLinked = filter1Linked;
        filter1Open = open1;
        if (open1)
        {
            openFilter1L.reset();
            openFilter1R.reset();
            openCutoff1 = -1.0;
        }
        else
        {
            filter1L.reset();
            filter1R.reset();
        }
        filter1Linked = true;
    }

    if (open1 && (cutoff1 != openCutoff1 || reso1 != openReso1))
    {
        openCutoff1 = cutoff1;
        openReso1 = reso1;
        openFilter1L.set (sampleRate, cutoff1, reso1);
        openFilter1R = openFilter1L;
    }

    if (! open1 || bothRouteActive)
    {
        for (auto* filter : { &filter1L, &filter1R, &bothFilter1L, &bothFilter1R })
            filter->setType (params.filter1.type, params.filter1.slope24);

        const auto morph1 = juce::jlimit (0.0f, 1.0f, params.filter1.morph + mods[(int) D::Filter1Morph]);
        const auto coefficients1 = FilterUnit::makeCoefficients (params.filter1.type, sampleRate, cutoff1, reso1, morph1);

        if (filterStereo (0))
        {
            // A stereo route: the left cutoff (resonance, morph) moves up as the right moves down.
            const auto shift = (double) mods[(int) D::Count + Mod::StereoIndex::cutoff1] * (double) Mod::Range::cutoffOctaves;
            const auto resoShift = mods[(int) D::Count + Mod::StereoIndex::reso1];
            const auto morphShift = mods[(int) D::Count + Mod::StereoIndex::morph1];
            const auto left = FilterUnit::makeCoefficients (params.filter1.type, sampleRate,
                                                            juce::jlimit (20.0, sampleRate * 0.45, cutoff1 * std::exp2 (shift)),
                                                            juce::jlimit (0.0f, 1.0f, reso1 + resoShift), juce::jlimit (0.0f, 1.0f, morph1 + morphShift));
            const auto right = FilterUnit::makeCoefficients (params.filter1.type, sampleRate,
                                                             juce::jlimit (20.0, sampleRate * 0.45, cutoff1 * std::exp2 (-shift)),
                                                             juce::jlimit (0.0f, 1.0f, reso1 - resoShift), juce::jlimit (0.0f, 1.0f, morph1 - morphShift));

            if (filter1Linked)
            {
                filter1R = filter1L;
                filter1Linked = false;
            }

            filter1L.setCoefficients (left);
            filter1R.setCoefficients (right);
            bothFilter1L.setCoefficients (left);
            bothFilter1R.setCoefficients (right);
        }
        else
        {
            filter1L.setCoefficients (coefficients1);
            filter1R.setCoefficients (coefficients1);
            bothFilter1L.setCoefficients (coefficients1);
            bothFilter1R.setCoefficients (coefficients1);
        }
    }

    const auto keyOctaves2 = (double) params.filter2.keyTrack * (double) keyTrackOctaves;
    const auto envOctaves2 = (double) (params.filter2.envAmount + mods[(int) D::Filter2Env] * Mod::Range::filterEnvOctaves)
                             * (double) filter2EnvValue * (double) velocityEnvScale
                             * (double) velocityScaleFor (params.filter2EnvVelocity);
    const auto cutoff2 = juce::jlimit (20.0, sampleRate * 0.45,
                                       (double) params.filter2.cutoffHz
                                           * std::exp2 (keyOctaves2 + envOctaves2 + fmOctaves2
                                                        + (double) mods[(int) D::Filter2Cutoff] * (double) Mod::Range::cutoffOctaves));
    const auto reso2 = juce::jlimit (0.0f, 1.0f, params.filter2.resonance + mods[(int) D::Filter2Reso]);

    // Filter 2 is a Low Pass left wide open on 361 of the 371 factory
    // presets. While it stays there (above 19 kHz, resonance up to 0.3, no
    // audio-rate FM) a linear copy of it runs instead (OpenLowPass: the same
    // response, a third of the cost; Vital likewise skips work a stage can't
    // be heard doing). Moving between the two starts the new one from rest.
    const auto open2 = ! disableOpenFilterBypass && params.filter2.type == FilterType::LowPass
                       && ! params.filter2.slope24 && cutoff2 >= openFilterHz && reso2 <= 0.3f && fmOctaves2 == 0.0
                       && ! filterStereo (1);

    if (open2 != filter2Open)
    {
        // As Filter 1: the new model starts from rest; once Filter 2 has
        // played, the old one keeps its state and fades out over
        // filterFadeLength (processFilter2Block). It used to restart both,
        // which clicked on every crossing (bug hunt 2026-10-31).
        filter2Fade = filter2Ran ? filterFadeLength : 0;
        filter2FadeLinked = filter2Linked;
        filter2Open = open2;
        if (open2)
        {
            openFilter2L.reset();
            openFilter2R.reset();
            openCutoff2 = -1.0;
        }
        else
        {
            filter2L.reset();
            filter2R.reset();
        }
        filter2Linked = true;
    }

    if (open2)
    {
        if (cutoff2 != openCutoff2 || reso2 != openReso2)
        {
            openCutoff2 = cutoff2;
            openReso2 = reso2;
            openFilter2L.set (sampleRate, cutoff2, reso2);
            openFilter2R = openFilter2L;
        }

        if (! bothRouteActive)
            return;
    }

    for (auto* filter : { &filter2L, &filter2R, &bothFilter2L, &bothFilter2R })
        filter->setType (params.filter2.type, params.filter2.slope24);

    const auto morph2 = juce::jlimit (0.0f, 1.0f, params.filter2.morph + mods[(int) D::Filter2Morph]);
    const auto coefficients2 = FilterUnit::makeCoefficients (params.filter2.type, sampleRate, cutoff2, reso2, morph2);

    if (filterStereo (1))
    {
        const auto shift = (double) mods[(int) D::Count + Mod::StereoIndex::cutoff2] * (double) Mod::Range::cutoffOctaves;
        const auto resoShift = mods[(int) D::Count + Mod::StereoIndex::reso2];
        const auto morphShift = mods[(int) D::Count + Mod::StereoIndex::morph2];
        const auto left = FilterUnit::makeCoefficients (params.filter2.type, sampleRate,
                                                        juce::jlimit (20.0, sampleRate * 0.45, cutoff2 * std::exp2 (shift)),
                                                        juce::jlimit (0.0f, 1.0f, reso2 + resoShift), juce::jlimit (0.0f, 1.0f, morph2 + morphShift));
        const auto right = FilterUnit::makeCoefficients (params.filter2.type, sampleRate,
                                                         juce::jlimit (20.0, sampleRate * 0.45, cutoff2 * std::exp2 (-shift)),
                                                         juce::jlimit (0.0f, 1.0f, reso2 - resoShift), juce::jlimit (0.0f, 1.0f, morph2 - morphShift));

        if (filter2Linked)
        {
            filter2R = filter2L;
            filter2Linked = false;
        }

        filter2L.setCoefficients (left);
        filter2R.setCoefficients (right);
        bothFilter2L.setCoefficients (left);
        bothFilter2R.setCoefficients (right);
        return;
    }

    filter2L.setCoefficients (coefficients2);
    filter2R.setCoefficients (coefficients2);
    bothFilter2L.setCoefficients (coefficients2);
    bothFilter2R.setCoefficients (coefficients2);
}

float Voice::sourceValue (Mod::Source source, int sampleIndex, float ampValue, float filterValue,
                          float filter2Value, float modValue, float env4Value) const
{
    if (source >= Mod::Source::Env6 && source <= Mod::Source::Env16)
    {
        const auto index = (size_t) ((int) source - (int) Mod::Source::Env6);
        const auto velocityScale = 1.0f - params.extraEnvVelocity[index]
                                   + params.extraEnvVelocity[index] * velocityLevel;
        return extraEnvValues[index] * velocityScale;
    }
    // The shared buffers cover the whole block; a render split by a MIDI
    // event starts part-way in (renderStart).
    const auto lfoSource = [this, sampleIndex] (int lfo, const float* shared)
    {
        if (params.lfos[lfo].perVoice)
            return lfoValues[lfo];

        return shared != nullptr ? shared[renderStart + sampleIndex] : 0.0f;
    };

    if (const auto lfo = Mod::lfoIndexFor (source); lfo >= 0)
        return lfoSource (lfo, params.lfoBuffers[lfo]);

    if (const auto lfo = Mod::lfoBIndexFor (source); lfo >= 0)
    {
        if (params.lfos[lfo].perVoice)
            return lfoValuesB[lfo];
        return params.lfoBuffersB[lfo] != nullptr ? params.lfoBuffersB[lfo][renderStart + sampleIndex] : 0.0f;
    }

    switch (source)
    {
        case Mod::Source::ModEnv:     return modValue * velocityScaleFor (params.modEnvVelocity);
        case Mod::Source::FilterEnv:  return filterValue * velocityScaleFor (params.filterVelocity);
        case Mod::Source::AmpEnv:     return ampValue;
        case Mod::Source::Velocity:   return velocityLevel;
        case Mod::Source::KeyTrack:   return keyTrackValue;
        case Mod::Source::Random:     return randomValue;
        case Mod::Source::ModWheel:   return modWheel;
        case Mod::Source::Aftertouch: return aftertouchValue;
        case Mod::Source::Expression: return expressionValue;
        case Mod::Source::Macro1:     return params.macros[0];
        case Mod::Source::Macro2:     return params.macros[1];
        case Mod::Source::Macro3:     return params.macros[2];
        case Mod::Source::Macro4:     return params.macros[3];
        case Mod::Source::Macro5:     return params.macros[4];
        case Mod::Source::Macro6:     return params.macros[5];
        case Mod::Source::Macro7:     return params.macros[6];
        case Mod::Source::Macro8:     return params.macros[7];
        case Mod::Source::ClockSh:    return params.clockSh != nullptr ? params.clockSh[renderStart + sampleIndex] : 0.0f;
        case Mod::Source::Mseg:       return params.mseg != nullptr ? params.mseg[renderStart + sampleIndex] : 0.0f;
        case Mod::Source::Env4:       return env4Value * velocityScaleFor (params.env4Velocity);
        case Mod::Source::FilterEnv2: return filter2Value * velocityScaleFor (params.filter2EnvVelocity);
        case Mod::Source::InputEnv:   return params.inputEnv != nullptr ? params.inputEnv[renderStart + sampleIndex] : 0.0f;
        case Mod::Source::VectorX:    return params.vectorX;
        case Mod::Source::VectorY:    return params.vectorY;
        case Mod::Source::OpLfo:      return dx7Playing ? dx7Note.getLfoOutput() : 0.0f;
        case Mod::Source::OpPitchEnv: return dx7Playing ? dx7Note.getPitchShape() : 0.0f;
        case Mod::Source::None:
        case Mod::Source::Count:
        default:                      return 0.0f;
    }
}
