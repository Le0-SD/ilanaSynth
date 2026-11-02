#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

#include "FilterUnit.h"
#include "GranularOsc.h"
#include "KarplusStrong.h"
#include "LfoShape.h"
#include "WestCoast.h"
#include "MaterialBody.h"
#include "NoiseColours.h"
#include "Modulation.h"
#include "Mseg.h"
#include "OscillatorIds.h"
#include "ResonatorBank.h"
#include "SpectralCache.h"
#include "SamplePlayer.h"
#include "Tuning.h"
#include "Svf.h"
#include "TensionAdsr.h"
#include "Dx7Engine.h"
#include "OperatorEgParams.h"
#include "UnisonBank.h"
#include "VoiceRouting.h"
#include "WavetableOscillator.h"

namespace UnisonMode
{
enum
{
    Classic = 0,
    Hypersaw,
    Octaves,
    Fifths,
    // Appended: chord stacks, spread detune as Classic.
    CenterDrop,  // the middle voice an octave down
    Octaves2,    // 0, +12, +24, -12, -24
    PowerChord,  // root and fifth
    MajorChord,
    MinorChord,
    Harmonics,   // the harmonic series: 0, 12, 19, 24, 28, 31 ...
    OddHarmonics, // 0, 19, 28, 34 ...
    Count
};

inline juce::StringArray getNames()
{
    return { "Classic", "Hypersaw", "Octaves", "Fifths", "Center Drop", "2x Octaves", "Power Chord",
             "Major Chord", "Minor Chord", "Harmonics", "Odd Harmonics" };
}
} // namespace UnisonMode

// Where an oscillator enters the filter section. Default follows the
// serial/parallel switch (into Filter 1 when serial, both when parallel).
namespace FilterRoute
{
enum { Default = 0, Filter1, Filter2, Direct, Both, Count };

inline juce::StringArray getNames() { return { "Default", "Filter 1", "Filter 2", "No filter", "Both" }; }
} // namespace FilterRoute

// How an operator's self-feedback (the matrix diagonal) is taken.
namespace FmFeedback
{
// DX7 (appended): the plain average of the last two samples, the DX7's own
// feedback (Filtered smooths that average further).
enum { Plain = 0, Filtered, Cross, Dx7, Count };

// Shown names (the index is what's saved): Filtered is "Smooth" and Cross
// "Cross Pair" (UI review 6, I6-10: two of them both claimed the DX7).
inline juce::StringArray getNames() { return { "Plain", "Smooth", "Cross Pair", "DX7" }; }
// A short tag for the matrix's feedback cells.
inline juce::String getTag (int type)
{
    return type == Filtered ? "SMOOTH" : type == Cross ? "CROSS" : type == Dx7 ? "DX7" : "PLAIN";
}

// Cross feedback runs between the two oscillators of a pair: 1-2, 3-4, 5-6.
inline int partnerOf (int osc) { return osc ^ 1; }
} // namespace FmFeedback

// Operator tuning (M5).
namespace OscTuning
{
enum { Semitones = 0, Ratio, Fixed, Count };

inline juce::StringArray getModeNames() { return { "Semitones", "Ratio", "Fixed Hz" }; }
inline juce::StringArray getSnapNames() { return { "Free", "Harmonic", "Inharmonic", "Bell" }; }

// The ratio sets the SNAP knob picks from: whole-number harmonics, square
// roots of non-square numbers (classic inharmonic FM ratios), and the
// partials of a tuned church bell and a free bar.
inline const std::vector<double>& snapSet (int set)
{
    static const std::vector<double> harmonic = []
    {
        std::vector<double> ratios { 0.25, 0.5 };
        for (int n = 1; n <= 32; ++n)
            ratios.push_back ((double) n);
        return ratios;
    }();
    static const std::vector<double> inharmonic = []
    {
        std::vector<double> ratios { 0.5 * std::sqrt (2.0), 0.5 * std::sqrt (3.0) };
        for (int n = 2; n <= 128; ++n)
        {
            const auto root = std::sqrt ((double) n);
            if (std::abs (root - std::round (root)) > 1.0e-9)
                ratios.push_back (root);
        }
        return ratios;
    }();
    // Bell: hum, prime, tierce, quint, nominal, deciem, undeciem, duodeciem,
    // upper octave; bar: the free-bar modes 2.756, 5.404, 8.933, 13.34.
    static const std::vector<double> bell { 0.5, 1.0, 1.2, 1.5, 2.0, 2.5, 2.667, 2.756, 3.0, 4.0,
                                            5.0, 5.404, 6.0, 8.0, 8.933, 13.34 };
    static const std::vector<double> none;

    switch (set)
    {
        case 1:  return harmonic;
        case 2:  return inharmonic;
        case 3:  return bell;
        default: return none;
    }
}

// The nearest ratio of the set (in pitch), or the ratio itself when Free.
inline double snapRatio (double ratio, int set)
{
    const auto& ratios = snapSet (set);

    if (ratios.empty() || ratio <= 0.0)
        return ratio;

    auto best = ratios.front();
    auto bestDistance = 1.0e9;

    for (const auto candidate : ratios)
    {
        const auto distance = std::abs (std::log (candidate / ratio));

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = candidate;
        }
    }

    return best;
}
} // namespace OscTuning

struct VoiceParams
{
    static constexpr int numOscillators = OscillatorIds::count;
    static constexpr int maxUnison = 16;
    // String and sample unison each carry a long delay/sample buffer, so they
    // stop at 8 voices; wavetable unison goes to 16.
    static constexpr int maxBufferedUnison = 8;
    static constexpr int numLfos = Mod::numLfoSources;

    struct OscParams
    {
        const Wavetable* table = nullptr;
        float frame = 0.0f;
        float level = 1.0f;
        float pan = 0.0f;
        double semitones = 0.0;
        double cents = 0.0;
        int unison = 1;
        float detuneCents = 0.0f;
        float spread = 0.0f;
        int unisonMode = UnisonMode::Classic;
        float unisonBlend = 1.0f;
        // Per unison voice: the frame and warp amount drift across the stack (0 = every voice alike).
        float frameSpread = 0.0f;
        float warpSpread = 0.0f;
        // Spectral warp: what each voice needs to pick its own amount (see SpectralCache::getForVoice).
        int spectralMode = 0;
        int spectralTable = -1;
        float spectralAmount = 0.0f;
        const Wavetable* baseTable = nullptr;
        int frameBlend = 0; // FrameBlend: Crossfade, Spectral (settings.table is then the blended copy) or Stepped
        // Quantise this oscillator's note (plus SEMI) to a scale (Scales::getNames, 0 = off) and root (0-11).
        int scale = 0;
        int scaleRoot = 0;
        int warpMode = 0;
        float warpAmount = 0.0f;
        int route = 0; // FilterRoute
        int ampEnv = 0; // 0..15, ENV 1..16; 16 = the MSEG as a one-shot envelope; 17 = the Operator EG

        // M5 operator settings. Tuning: 0 = semitones (as before), 1 = a
        // frequency ratio of the note (already snapped), 2 = a fixed pitch.
        int tuneMode = 0;
        double ratio = 1.0;
        double fixedHz = 440.0;
        float keyLevel = 0.0f;   // level key scaling: dB per octave from C3, x6
        int feedbackType = 0;    // FmFeedback: plain, filtered, cross, DX7

        // M6: the PD chain's second stage and the DCW-style warp envelope
        // (0 = off, 1..16 = ENV 1..16, 17 = MSEG).
        int warpMode2 = 0;
        float warpAmount2 = 0.0f;
        int pdEnv = 0;
        float pdEnvAmount = 1.0f;

        bool stringMode = false;
        int stringExcite = 0;
        float stringDecay = 0.75f;
        float stringDamping = 0.35f;
        float stringSustain = 0.0f;
        float stringStiffness = 0.0f;
        float stringPickup = 0.0f;
        float stringExcitationPosition = 0.0f;
        float stringPickHardness = 1.0f;
        float stringPickPosition = 0.0f;
        bool stringSlap = false;
        float bowPressure = 0.5f, bowSpeed = 0.5f;
        float bridgeBuzz = 0.0f, fretRattle = 0.0f;
        float stringSplit = 0.0f; // the second vibration direction (0 = off)
        float stringClick = 1.0f; // raw pluck snap heard by the pickup at the start (1 = as before)
        // M4 keys: felt hardness, strings sharing the bridge, dampers on
        // release, and how much the string changes from bass to treble.
        float hammerHardness = 0.5f, couple = 0.0f, damper = 0.0f, registerMap = 0.0f;
        float epDistance = 0.5f, epPosition = 0.5f; // M7.3 tine/reed pickup
        float fbGain = 0.5f, fbDistance = 0.5f;     // M8.5 feedback amp
        // Stereo routes into the string knobs (Mod::StereoString): the left
        // strings get + these and the right ones minus; one string plays as a
        // left / right twin while any such route is set.
        bool stringStereo = false;
        std::array<float, Mod::StereoString::numFields> stringStereoOffsets {};
        int chord = 0;

        bool sampleMode = false;
        const SampleData* sample = nullptr;
        bool sampleTuned = true;
        bool sampleLoop = false;
        bool sampleReverse = false;
        float sampleStart = 0.0f;
        float sampleRoot = 60.0f;   // step 29: a plain sample's recorded note (C3 = 60)
        bool sampleLoopOwn = false; // step 27: LOOP uses its own span
        float sampleLoopStart = 0.25f, sampleLoopEnd = 0.75f;
        float sampleLoopXfade = 0.0f; // step 28: crossfade at the loop wrap
        int samplePlayback = 0;       // step 33: Gate, One Shot, Sustain Loop (SamplePlayer::Playback)
        bool samplePingPong = false;  // step 34: the loop runs forward and back
        float sampleStartRandom = 0.0f; // step 35: random start amount
        bool sampleStartSafe = false;   // step 35: random starts land on attacks
        int sampleQuality = 0;          // step 39: SampleInterpolation::Quality
        bool sampleSilenceOutside = false; // step 36: keys outside every zone play nothing
        int sampleRoundRobin = 0;          // step 37: Off, Cycle, Random
        float sampleVelBlend = 0.0f;       // step 37: velocity layer crossfade width
        float sampleEnd = 1.0f;
        float sampleFadeIn = 0.0f;
        float sampleFadeOut = 0.0f;
        // Roadmap step 40: SYNC follows the host tempo (0 off, 1 the BPM
        // below, 2.. the region's length in beats, sampleSyncBeats); STRETCH
        // keeps the pitch apart from the speed.
        int sampleSync = 0;
        float sampleBpm = 120.0f;
        bool sampleStretch = false;
        // TIME's high-quality engines (0 Signalsmith, 1 Bungee, 2 PGHI,
        // 3 Paulstretch; -1 none): a copy rendered in the background
        // (StretchRenderer) plays; until it's there, the live stretch above.
        int sampleEngine = -1;

        bool granularMode = false; // also sets sampleMode: grains read the sample
        bool grainLive = false;    // M7.5: grains read the live input's history
        int liveWrite = 0;
        bool liveMode = false;     // M7.5: the Live mode plays the audio input
        float grainSizeMs = 80.0f;
        float grainDensity = 0.5f;
        float grainSpray = 0.15f;
        float grainPitch = 0.0f;
        float grainSpread = 0.6f;
        float grainPosition = 0.0f; // 0..1 through the sample (apart from sampleStart)
        float grainScan = 0.0f;     // -2..2: how fast the position moves (1 = the sample's own speed)
        bool grainFreeze = false;
    };

    struct FilterParams
    {
        int type = FilterType::LowPass;
        bool slope24 = false;
        float cutoffHz = 20000.0f;
        float resonance = 0.2f;
        float drive = 1.0f;
        float envAmount = 0.0f;
        float keyTrack = 0.0f;
        float morph = 0.0f;
    };

    // Per-voice LFO settings. When perVoice is off the voice reads the
    // synth-wide LFO buffer instead (free-running, shared by all voices).
    struct LfoParams
    {
        bool perVoice = false;
        bool keyTrack = false;         // rate follows the note: RATE 4 Hz = the note's pitch
        int shape = 0;
        double baseIncrement = 0.0;   // cycles per voice-rate sample, before modulation
        float startPhase = 0.0f;
        float physA = 0.5f, physB = 0.5f;
        bool kick = false;
        const float* steps = nullptr;  // 16 values
        const float* custom = nullptr; // lfoDrawSteps values
        int customSize = 0;
        // M8.1
        LfoSimSettings sim;            // the simulated shapes' settings
        float smooth = 0.0f;           // SMOOTH, a fraction of a cycle
        bool needsB = false;           // output B is routed
        unsigned triggerCount = 0;     // beats, Generative steps and FIRE, counted by the processor
    };

    std::array<OscParams, numOscillators> oscillators;
    std::array<bool, numOscillators> oscillatorEnabled { true, false, true };
    int subOctaveOffset = -12;

    float fmAmount = 0.0f;
    float fmFeedback = 0.0f;
    float fmMatrix[numOscillators][numOscillators] {}; // [source][target]
    int fmMode = 0;               // 0 phase, 1 through-zero, 2 exponential
    float fmNoise[numOscillators] {}; // M5: the noise operator into each oscillator
    float fmNoiseColour = 1.0f;   // 0 dark (about 200 Hz) .. 1 white
    int fmNoiseType = 0;          // 0: the colour-knob noise (older patches); 1..6: NoiseColours::Type + 1
    std::array<bool, numOscillators> oscOut { true, true, true };
    float ringMod = 0.0f;
    bool hardSync = false;
    float drift = 0.0f;
    float tuneLoose = 0.0f; // a random offset per note, up to this many cents
    bool tuneLooseBoth = true; // sharp or flat; off = flat only

    float voiceSpread = 0.0f;
    float stretch = 0.0f; // M4: piano stretch tuning, 0 = equal temperament
    double hostBpm = 120.0; // the host's tempo, for samples that follow it (step 40)
    class StretchRenderer* stretchRenderer = nullptr; // rendered copies for TIME's engines
    struct SampleRoundRobin* roundRobin = nullptr;    // step 37: the plugin's round-robin counters
    const Tuning* tuning = nullptr; // Scala tuning; nullptr = 12-TET (the old path)
    float unisonRandom = 0.0f;

    float filter1Fm = 0.0f;
    float filter2Fm = 0.0f;

    bool resonatorOn = false;
    float resonatorAmount = 0.0f;
    float resonatorDecay = 0.7f;
    float resonatorOffset = 0.0f;
    float resonatorKeytrack = 1.0f;
    int bodyType = 0; // Classic, Bar, Plate, Bell, Shell; Classic preserves res_*
    float bodyMaterial = 0.0f, bodySize = 0.5f;
    int bodyCouplingMode = 0; // Off, String to body, Body to string, Strings
    float bodyCoupling = 0.0f;

    // The dedicated sub oscillator (sine/square/saw one or two octaves down).
    // It and the noise share one route.
    bool subOscEnabled = false;
    float subOscLevel = 0.5f;
    int subOscOctave = -12;
    int subOscRoute = 0;
    const Wavetable* subOscTable = nullptr;
    float noiseLevel = 0.0f;
    float noiseColour = 1.0f;     // review 8 (V8-15): the heard noise's colour, 0 dark .. 1 white (as before)
    bool noiseEnabled = true;     // the NOISE card's own switch (on, as before: the level alone decided)
    int noiseType = 0;            // NoiseColours::Type; White is the old noise
    float subOscColour = 0.0f;    // the SUB card's COLOUR: 0 the plain wave (as before), 1 driven into harmonics

    FilterParams filter1;
    FilterParams filter2;
    bool filtersParallel = false;
    float filterBalance = 0.0f; // parallel only: -1 all Filter 1 .. +1 all Filter 2
    // Roadmap step 16: the ROUTING cables, compiled. Off (as in every older
    // patch): the fixed chain above (routes, filtersParallel, west.position).
    VoiceRouting::Table routing;

    TensionAdsr::Parameters ampEnv { 0.005f, 0.3f, 0.8f, 0.25f, 0.0f };
    TensionAdsr::Parameters filterEnv { 0.01f, 0.4f, 0.4f, 0.3f, 0.0f };
    TensionAdsr::Parameters filter2Env { 0.01f, 0.4f, 0.4f, 0.3f, 0.0f };
    TensionAdsr::Parameters modEnv { 0.05f, 0.4f, 0.5f, 0.3f, 0.0f };
    TensionAdsr::Parameters env4 { 0.05f, 0.4f, 0.5f, 0.3f, 0.0f };
    std::array<TensionAdsr::Parameters, 11> extraEnvs {};
    std::array<float, 11> extraEnvVelocity {};
    std::array<bool, 11> extraEnvNeeded {};
    // M5: envelope times shrink up the keyboard (0 = off, 1 = halve per octave),
    // ENV 1..16.
    std::array<float, 16> envKeyRate {};

    // The MSEG's shape, run per voice as a one-shot envelope when an
    // oscillator picks it as its amp or warp envelope.
    struct MsegShape
    {
        float levels[Mseg::numPoints] { 0.0f, 1.0f, 0.0f, -1.0f };
        float times[Mseg::numPoints] { 0.25f, 0.25f, 0.25f, 0.25f };
        double rateHz = 0.5;
        bool loop = true;
    };
    MsegShape msegShape;
    bool msegEnvNeeded = false;
    int quality = 1;
    float ampVelocity = 0.5f;
    float filterVelocity = 0.5f;
    // ENV 3-5's own velocity (0 = off), like ENV 6-16's.
    float filter2EnvVelocity = 0.0f, modEnvVelocity = 0.0f, env4Velocity = 0.0f;

    float glideTime = 0.0f;
    float pitchBendRange = 2.0f;
    // MPE: member channels bend by pitchBendRange (48), the master channel
    // (1) bends every voice by masterBendRange (the BEND knob).
    bool mpe = false;
    float masterBendRange = 2.0f;

    float macros[Mod::numMacros] {};
    float vectorX = 0.5f, vectorY = 0.5f; // M8.5

    const float* lfoBuffers[numLfos] {}; // free-running LFOs, shared by every voice
    const float* lfoBuffersB[numLfos] {}; // their outputs B (M8.1)
    const float* clockSh = nullptr;
    const float* mseg = nullptr;
    // M7.5 live input (ilanaSynth FX), at the voice rate for this block, and
    // its envelope follower; null when there is no input.
    const float* liveInput = nullptr;
    const float* inputEnv = nullptr;
    float inputToBody = 0.0f, inputToStrings = 0.0f;
    // Physical exciters at matched loudness (2026-10-01): Bow and Tine came
    // out ~8 dB over a plucked string. Off for patches saved before then that
    // use those exciters, so they keep their sound. Which trims apply: the
    // exciter's own generation (exciterTrimLevel) up to this one (0 none;
    // 1 Bow, Hammer, Tine, Reed, Piano; 2 also the Modal Piano, 2026-10-30).
    int exciterLevels = 0;
    // The Operator EG (2026-10-02): its settings as a DX7 voice (only the
    // envelope, scaling, pitch EG and LFO bytes are read), filled when an
    // enabled oscillator's ENVELOPE is the Operator EG (operatorEgUsed).
    Dx7::Voice operatorEg {};
    bool operatorEgUsed = false;

    // M8.3: the west-coast voice (wavefolder into a low-pass gate).
    struct WestParams
    {
        bool on = false;
        int position = 0;       // 0 after the filters, 1 in place of Filter 2
        float fold = 0.3f, symmetry = 0.0f;
        int stages = 2;
        int mode = 0;           // LowPassGate::Mode
        float decay = 1.0f, resonance = 0.2f, strike = 1.0f, open = 0.0f;
        int source = 0;         // 0: a strike on each note; else Mod::Source (source - 1 + 1)
    } west;
    LfoParams lfos[numLfos];

    // Only the slots that are switched on, packed at the front, plus the
    // explicit destinations they touch (so the voice only clears those).
    Mod::Slot modSlots[Mod::maxSlots] {};
    int numModSlots = 0;
    int activeDestinations[Mod::maxSlots] {};
    int numActiveDestinations = 0;
    bool anyExtendedFmMods = false;   // a slot targets an FM cell to or from OSC 4-6
    bool anySpectralMods = false;     // a per-note source feeds a spectral amount: each voice picks its own table
    SpectralCache* spectralCache = nullptr;

    // Appends a routing (for code that drives a voice directly, e.g. tests).
    void addModSlot (Mod::Source source, Mod::Destination destination, float depth)
    {
        if (numModSlots >= Mod::maxSlots)
            return;

        Mod::Slot slot;
        slot.source = source;
        slot.destination = (int) destination;
        slot.depth = depth;
        modSlots[numModSlots++] = slot;

        for (int d = 0; d < numActiveDestinations; ++d)
            if (activeDestinations[d] == (int) destination)
                return;

        activeDestinations[numActiveDestinations++] = (int) destination;
    }
};

// What the synth knows that a note starting later must still see: the last
// mod wheel, expression and channel pressure per MIDI channel (a voice only
// hears the controllers that arrive while it plays), the MPE master-channel
// bend and the pitch of the last note started (poly glide).
struct VoiceSharedState
{
    float modWheel[17] {};
    float expression[17] { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    float pressure[17] {};
    double masterBendSemitones = 0.0;
    double lastNoteFrequency = 0.0; // 0: none yet
};

// The voices' "analog" generators (random, drift) and the arpeggiator's
// random run free in the plugin: every note differs a little. The fingerprint
// build seeds them so presets render the same every time; tests that compare
// renders or measure pitch turn the same seeding on here, before they make
// a processor.
inline std::atomic<bool>& fixedRandomSeeds()
{
#if ILANA_FINGERPRINT_BUILD
    static std::atomic<bool> fixed { true };
#else
    static std::atomic<bool> fixed { false };
#endif
    return fixed;
}

class Voice : public juce::SynthesiserVoice
{
public:
    Voice();

    // Poly glide starts from the last note played, not the reused voice's own pitch. Off: it changes old presets.
    static inline bool polyGlideFromLastNote = false;

    void setParams (const VoiceParams& newParams) { params = newParams; }

    // Where an idle voice finds this block's parameters when a note starts on
    // it (the processor copies them into the voices that are sounding only).
    void setParamsSource (const VoiceParams* source) noexcept { paramsSource = source; }
    // Fade out over the given samples, then end (a voice over SUSTAIN
    // VOICES); a new note on the voice cancels it.
    void startFadeOut (int samples) noexcept
    {
        if (fadeTotal == 0)
            fadeTotal = fadeRemaining = juce::jmax (1, samples);
    }
    bool isFadingOut() const noexcept { return fadeTotal > 0; }
    // False until the note has been rendered once (a note stolen in the sample it started has made no sound to cut).
    bool hasSounded() const noexcept { return sounded; }

    float getLastAmpValue() const { return lastAmpValue; }
    // Gain bringing each exciter's first-second RMS within ~2 dB of a plucked
    // string (Burst) at C3-C5 on Init (`ilanaSnapshot --exciters`).
    static float exciterTrim (int excite) noexcept
    {
        switch (excite)
        {
            case 4:  return 0.376f; // Bow, -8.5 dB
            case 5:  return 0.708f; // Hammer (classic), -3 dB
            case 7:  return 0.398f; // Tine, -8 dB
            case 8:  return 0.708f; // Reed, -3 dB
            case 9:  return 2.0f;   // Piano, +6 dB (heard 2-6 dB under Hammer; the drive takes some back)
            case 11: return 10.0f;  // Modal Piano, +20 dB (the table is in Piano V's dBFS; level with Piano and a wavetable osc at C4)
            default: return 1.0f;
        }
    }
    // The generation of the state's "exciterLevels" that brought each trim in.
    static constexpr int currentExciterLevels = 2;
    static int exciterTrimLevel (int excite) noexcept { return excite == 11 ? 2 : 1; }
    // The trim a patch at this generation plays (1 when it predates the exciter's trim).
    static float exciterTrimFor (int excite, int levels) noexcept
    {
        return levels >= exciterTrimLevel (excite) ? exciterTrim (excite) : 1.0f;
    }
    float getWestGateLevel() const { return params.west.on && isVoiceActive() ? westGateL.getConductance() : 0.0f; }
    float getLastLifetimeValue() const { return lastLifetimeValue; }
    float getLastExtraEnvValue (int index) const { return extraEnvValues[(size_t) juce::jlimit (0, 10, index)]; }
    float getLastSamplePosition (int oscIndex) const
    {
        return lastSamplePosition[(size_t) juce::jlimit (0, VoiceParams::numOscillators - 1, oscIndex)];
    }

    float getLastWavetablePhase (int oscIndex) const
    {
        return oscBank[juce::jlimit (0, VoiceParams::numOscillators - 1, oscIndex)].getPhase (0);
    }
    float getLastFilterValue() const { return lastFilterValue; }
    float getLastFilter2Value() const { return lastFilter2Value; }
    float getLastModValue() const { return lastModValue; }
    float getLastEnv4Value() const { return lastEnv4Value; }
    // ENV 1-16's stage and progress (TensionAdsr::getDisplayPosition), for
    // the envelope graphs' playhead.
    float getEnvelopePosition (int env) const
    {
        switch (env)
        {
            case 0: return ampEnv.getDisplayPosition();
            case 1: return filterEnv.getDisplayPosition();
            case 2: return filter2Env.getDisplayPosition();
            case 3: return modEnv.getDisplayPosition();
            case 4: return env4.getDisplayPosition();
            default: return extraEnvs[(size_t) juce::jlimit (0, 10, env - 5)].getDisplayPosition();
        }
    }
    float getEnvelopeStageStart (int env) const
    {
        switch (env)
        {
            case 0: return ampEnv.getDisplayStageStart();
            case 1: return filterEnv.getDisplayStageStart();
            case 2: return filter2Env.getDisplayStageStart();
            case 3: return modEnv.getDisplayStageStart();
            case 4: return env4.getDisplayStageStart();
            default: return extraEnvs[(size_t) juce::jlimit (0, 10, env - 5)].getDisplayStageStart();
        }
    }
    std::array<float, 5> getEnvelopeSeconds (int env) const
    {
        switch (env)
        {
            case 0: return ampEnv.getDisplaySeconds();
            case 1: return filterEnv.getDisplaySeconds();
            case 2: return filter2Env.getDisplaySeconds();
            case 3: return modEnv.getDisplaySeconds();
            case 4: return env4.getDisplaySeconds();
            default: return extraEnvs[(size_t) juce::jlimit (0, 10, env - 5)].getDisplaySeconds();
        }
    }
    float getVelocity() const { return velocityLevel; }
    float getKeyTrack() const { return keyTrackValue; }
    float getRandomValue() const { return randomValue; }
    // The Operator Env's LFO and pitch envelope (sources Op LFO and Op Pitch
    // Env), 0 while the note doesn't play the Operator Env.
    float getOpLfoValue() const { return dx7Playing ? dx7Note.getLfoOutput() : 0.0f; }
    float getOpPitchValue() const { return dx7Playing ? dx7Note.getPitchShape() : 0.0f; }
    // Seconds since the note began and since its key was let go (-1: held;
    // both -1 while the note doesn't play the Operator Env).
    float getOpEnvSeconds() const { return dx7Playing ? dx7Note.getSecondsPlayed() : -1.0f; }
    float getOpEnvReleaseSeconds() const { return dx7Playing ? dx7Note.getSecondsReleased() : -1.0f; }
    float getLfoPhase (int lfo) const { return (float) lfoPhases[(size_t) juce::jlimit (0, 3, lfo)]; }

    bool canPlaySound (juce::SynthesiserSound*) override { return true; }

    // Mono / legato: the synth calls this right before handing the voice a
    // new note. keepRunning stops the hard reset between notes; legato also
    // skips retriggering the envelopes; glide says whether to slide pitch
    // from the previous note or jump.
    void prepareMonoNote (bool legato, bool keepRunning, bool glide)
    {
        monoLegato = legato;
        monoKeepRunning = keepRunning;
        monoGlide = glide;
        monoPending = true;
    }

    void setCurrentPlaybackSampleRate (double newRate) override;
    void startNote (int midiNoteNumber, float velocity, juce::SynthesiserSound* sound, int currentPitchWheelPosition) override;

    // Re-strikes this voice's Physical strings while its key is held
    // (Euclid's Exciter target). Other oscillator modes are left alone.
    void reExcite (float level);
    void stopNote (float velocity, bool allowTailOff) override;
    // A new patch: clear what the last one left in the voice (filter and
    // body memory, the vactrol, strings, glide origin, random sequences),
    // so its first notes don't carry the old patch's ringing.
    void resetForNewPatch();
    void pitchWheelMoved (int newValue) override;
    void controllerMoved (int controllerNumber, int newValue) override;
    void channelPressureChanged (int newValue) override;

    // The MIDI channel this voice plays on (JUCE keeps its own copy private).
    int playingChannel() const
    {
        for (int channel = 1; channel <= 16; ++channel)
            if (isPlayingChannel (channel))
                return channel;

        return 1;
    }
    void aftertouchChanged (int newValue) override;
    void setSharedState (VoiceSharedState* state) noexcept { shared = state; }
    // MPE master-channel bend, in semitones (added to this voice's own bend).
    void setMasterBend (double semitones) noexcept { masterBend = semitones; bendSemitones = memberBend + masterBend; }
    bool isMpe() const noexcept { return params.mpe; }
    // What the note started with (the regression tests read these).
    float getModWheel() const noexcept { return modWheel; }
    float getExpression() const noexcept { return expressionValue; }
    float getAftertouch() const noexcept { return aftertouchValue; }
    double getBendSemitones() const noexcept { return bendSemitones; }
    double getCurrentFrequency() const noexcept { return currentFrequency; }
    float getMasterBendRange() const noexcept { return params.masterBendRange; }
    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples) override;

private:
    void syncSamplePlayers();
    const SampleData* stretchCopyFor (int osc, const SampleData* origin);
    void updateSubBlock (const float* mods, float filterEnvValue, float filter2EnvValue, const float* envelopeValues);
    double advanceGlideAndDrift (float driftMod);
    void tuneBody (double driftedFrequency);
    /** True when the voice cable graph routes audio through the physical body. */
    bool bodyIsInChain() const;
    /** True when a material body runs in the legacy way: excited by the physical bus, not cabled. */
    bool legacyMaterialBody() const;
    void renderBodyTail (float* left, float* right, int startSample, int numSamples, float wetGain);
    bool envelopesActive() const;
    double oscFrequencyFactor (const VoiceParams::OscParams& settings) const;
    void updateFilterCoefficients (const float* mods, float filterEnvValue, float filter2EnvValue);
    void updateUnisonLayout();
    // 1 at amount 0 exactly, so an envelope without velocity is untouched.
    float velocityScaleFor (float amount) const { return 1.0f - amount + amount * velocityLevel; }
    float sourceValue (Mod::Source source, int sampleIndex, float ampValue, float filterValue,
                       float filter2Value, float modValue, float env4Value) const;
    void evaluateMods (float* mods, int sampleIndex, float ampValue, float filterValue,
                       float filter2Value, float modValue, float env4Value) const;
    void evaluateModGroup (int group, float* mods, int sampleIndex, float ampValue, float filterValue,
                           float filter2Value, float modValue, float env4Value) const;
    // evaluateMods in the per-sample loops: moving routes into what is read
    // every sample (FM, frames, levels) every sample, the rest every
    // modControlInterval samples, where they are read (Vital runs its
    // modulation at a control rate).
    void evaluateModsRated (float* mods, int sampleIndex, float ampValue, float filterValue,
                            float filter2Value, float modValue, float env4Value);
    static bool isPerSampleDestination (int destination, bool filterFm);
    void prepareModSlots();
    void advanceVoiceLfos (int sampleIndex);
    void classifySlowLfos();
    void classifySlowEnvelopes();

    // Per block: the per-voice LFOs and the extra envelopes in use, so the
    // sample loop walks short lists instead of testing every slot.
    int perVoiceLfos[VoiceParams::numLfos] {};
    int numPerVoiceLfos = 0;
    int neededExtraEnvs[11] {};
    int numNeededExtraEnvs = 0;
    bool hasActiveAmpEnvelope() const;
    // side: +1 the left strings, -1 the right ones (stereo string routes), 0 both.
    void configureString (KarplusStrong& string, const VoiceParams::OscParams& settings, float side = 0.0f) const;
    // A one-voice physical oscillator with a stereo route plays a second string as its right side.
    bool stringTwin[VoiceParams::numOscillators] {};
    // While the route sits at 0 the twin would play what string 0 plays, so
    // it sleeps and string 0's output stands for both (CPU round 2026-10-31).
    // The first block the route moves, the twin wakes as a copy of string 0.
    bool twinAsleep[VoiceParams::numOscillators] {};
    bool stereoStringAtRest (int osc) const
    {
        for (auto offset : params.oscillators[osc].stringStereoOffsets)
            if (offset != 0.0f)
                return false;
        return true;
    }
    int stringsPlaying (int osc) const { return stringTwin[osc] ? 2 : juce::jmin (numOscUnison[osc], VoiceParams::maxBufferedUnison); }
    // Where unison string u of n sits: +1 the leftmost, -1 the rightmost (the twin: 0 left, 1 right).
    float stringSide (int osc, int u, int count) const
    {
        if (! params.oscillators[osc].stringStereo)
            return 0.0f;
        if (stringTwin[osc])
            return u == 0 ? 1.0f : -1.0f;
        return count > 1 ? 1.0f - 2.0f * (float) u / (float) (count - 1) : 0.0f;
    }
    float voiceLfoValue (int lfo) const;
    float blockMod (Mod::Destination destination) const { return blockMods[(size_t) destination]; }

    VoiceParams params;
    const VoiceParams* paramsSource = nullptr;

    // Wavetable unison: each oscillator's voices render together (SIMD).
    UnisonBank oscBank[VoiceParams::numOscillators];
    static_assert (VoiceParams::maxUnison <= UnisonBank::maxLanes);
    // Keep the first three banks' default-construction seed sequence exactly
    // as before M3b. Extra banks use explicit seeds and do not advance the
    // shared KarplusStrong counter used by existing presets.
    int renderStart = 0; // where this render call starts in the block (M7.5)
    KarplusStrong stringUnison[3][VoiceParams::maxBufferedUnison];
    struct ExtraStringBank
    {
        explicit ExtraStringBank (int osc) : voices {
            KarplusStrong (osc * 100003 + 0), KarplusStrong (osc * 100003 + 1),
            KarplusStrong (osc * 100003 + 2), KarplusStrong (osc * 100003 + 3),
            KarplusStrong (osc * 100003 + 4), KarplusStrong (osc * 100003 + 5),
            KarplusStrong (osc * 100003 + 6), KarplusStrong (osc * 100003 + 7) } {}
        KarplusStrong voices[VoiceParams::maxBufferedUnison];
    };
    ExtraStringBank extraStringUnison[3] { ExtraStringBank (3), ExtraStringBank (4), ExtraStringBank (5) };
    KarplusStrong& stringFor (int osc, int unison)
    {
        return osc < 3 ? stringUnison[osc][unison] : extraStringUnison[osc - 3].voices[unison];
    }
    SamplePlayer sampleUnison[VoiceParams::numOscillators][VoiceParams::maxBufferedUnison];
    // Step 37: the velocity layer a note blends with, played beside it.
    SamplePlayer sampleLayer[VoiceParams::numOscillators][VoiceParams::maxBufferedUnison];
    const SampleZone* layerZone[VoiceParams::numOscillators] {};
    const SampleData* layerOrigin[VoiceParams::numOscillators] {};
    float layerKeep[VoiceParams::numOscillators] {}, layerGain[VoiceParams::numOscillators] {};
    unsigned zonePick[VoiceParams::numOscillators] {}; // step 37: the round-robin alternate, per note
    GranularOsc grains[VoiceParams::numOscillators][VoiceParams::maxBufferedUnison];
    double sampleRatio[VoiceParams::numOscillators][VoiceParams::maxUnison] {};
    ResonatorBank resonatorL, resonatorR;
    MaterialBody materialBodyL, materialBodyR;
    // M8.3
    StereoWavefolder westFolder;
    LowPassGate westGateL, westGateR;
    int westStrikeRemaining = 0;
    bool bodyStrikePending = false;
    int bodyTailSamplesRemaining = 0;

    FilterUnit filter1L, filter1R, filter2L, filter2R;

    // A pair fed the same signal from the same (reset) state gives the same
    // output, so while linked only the left filter runs (the right one's
    // state is stale). The first different input copies the left state over
    // and the pair runs apart until the next reset.
    bool filter1Linked = true, filter2Linked = true;
    // Filter 2 while wide open (updateFilterCoefficients): a linear copy.
    bool filter1Open = false, filter2Open = false, bothRouteActive = false;
    static constexpr double openFilterHz = 19000.0;
    Airwindows::OpenLowPass openFilter1L, openFilter1R, openFilter2L, openFilter2R;
    double openCutoff1 = -1.0, openCutoff2 = -1.0;
    float openReso1 = -1.0f, openReso2 = -1.0f;
    // Filter 1 is often swept by a macro: moving between its two models while
    // it plays fades the old one out over filterFadeLength samples.
    static constexpr int filterFadeLength = 32;
    int filter1Fade = 0;
    bool filter1FadeLinked = true, filter1Ran = false;
    // Filter 2 the same (bug hunt 2026-10-31: crossing its open line clicked).
    int filter2Fade = 0;
    bool filter2FadeLinked = true, filter2Ran = false;
    // Filter 2 over a block: the model in use, and while a switch fades, the old one faded out.
    void processFilter2Block (const float* inL, const float* inR, float* outL, float* outR, int n)
    {
        if (filter2Open)
            processOpenPairBlock (openFilter2L, openFilter2R, filter2Linked, inL, inR, outL, outR, n);
        else
            processFilterPairBlock (filter2L, filter2R, filter2Linked, inL, inR, outL, outR, n);
        filter2Ran = true;

        if (filter2Fade > 0)
        {
            float oldL[16], oldR[16]; // (renderNextBlock's chunks are at most 16)
            jassert (n <= 16);
            if (filter2Open)
                processFilterPairBlock (filter2L, filter2R, filter2FadeLinked, inL, inR, oldL, oldR, n);
            else
                processOpenPairBlock (openFilter2L, openFilter2R, filter2FadeLinked, inL, inR, oldL, oldR, n);
            for (int s = 0; s < n && filter2Fade > 0; ++s, --filter2Fade)
            {
                const auto g = (float) filter2Fade / (float) filterFadeLength;
                outL[s] += (oldL[s] - outL[s]) * g;
                outR[s] += (oldR[s] - outR[s]) * g;
            }
        }
    }

public:
    // Tests: run Filter 2 even when it is wide open.
    inline static bool disableOpenFilterBypass = false;
    // Tests: keep released voices to the end of their release (no silence end).
    inline static bool disableReleaseSilence = false;
    // Tests: keep rendering a held note whose strings have died away (no held sleep).
    inline static bool disableHeldSilence = false;
    // Tests: always play a stereo route's twin string, even while the route rests.
    inline static bool disableTwinSleep = false;
    // Tests: every envelope computed every sample (no control-rate envelopes).
    inline static bool disableSlowEnvelopes = false;
    // The block-wise source path (renderNextBlock); ILANA_NO_BLOCK_SOURCES
    // keeps the per-sample loop everywhere, for comparing the two.
    static bool blockSourcesEnabled()
    {
        static const bool enabled = juce::SystemStats::getEnvironmentVariable ("ILANA_NO_BLOCK_SOURCES", "").isEmpty();
        return enabled;
    }
private:

    static void processFilterPair (FilterUnit& left, FilterUnit& right, bool& linked,
                                   float inLeft, float inRight, float& outLeft, float& outRight)
    {
        if (linked)
        {
            if (inLeft == inRight)
            {
                outLeft = outRight = left.process (inLeft);
                return;
            }

            right = left;
            linked = false;
        }

        outLeft = left.process (inLeft);
        outRight = right.process (inRight);
    }
    // processFilterPair over a block: the left filter alone while the sides
    // are equal, the right one copied from it where they first differ.
    static void processFilterPairBlock (FilterUnit& left, FilterUnit& right, bool& linked,
                                        const float* inLeft, const float* inRight, float* outLeft, float* outRight, int n)
    {
        auto start = 0;
        if (linked)
        {
            while (start < n && inLeft[start] == inRight[start])
                ++start;
            left.processBlock (inLeft, outLeft, start);
            std::copy (outLeft, outLeft + start, outRight);
            if (start == n)
                return;
            right = left;
            linked = false;
        }

        FilterUnit::processStereoBlock (left, right, inLeft + start, inRight + start, outLeft + start, outRight + start, n - start);
    }

    // The open Y Low Pass pair over a block: one filter while both sides
    // are equal (linear filters from equal states stay equal), the right one
    // copied from it where the sides first differ.
    static void processOpenPairBlock (Airwindows::OpenLowPass& l, Airwindows::OpenLowPass& r, bool& linked,
                                      const float* inLeft, const float* inRight, float* outLeft, float* outRight, int n)
    {
        auto s = 0;
        if (linked)
        {
            s = l.processLinkedRun (inLeft, inRight, outLeft, outRight, n);
            if (s == n)
                return;
            r = l;
            linked = false;
        }
        if (s < n)
            Airwindows::OpenLowPass::processPairBlock (l, r, inLeft + s, inRight + s, outLeft + s, outRight + s, n - s);
    }

    FilterUnit bothFilter1L, bothFilter1R, bothFilter2L, bothFilter2R;

    TensionAdsr ampEnv, filterEnv, filter2Env, modEnv, env4;
    std::array<TensionAdsr, 11> extraEnvs;
    std::array<float, 11> extraEnvValues {};
    Mseg envMseg;
    float msegEnvValue = 0.0f;

    // M5 operator state: the noise operator (its own generator, so existing
    // random sequences don't shift), filtered feedback history, and each
    // oscillator's key-scaled level.
    juce::Random fmNoiseRandom { 31337 };
    float fmNoiseState = 0.0f;

    // One sample of the noise operator: the colour-knob noise (type 0, as
    // every older patch) or one of the NOISE card's colours.
    void stepFmNoise (float coefficient)
    {
        const auto white = fmNoiseRandom.nextFloat() * 2.0f - 1.0f;
        if (params.fmNoiseType <= 0)
            fmNoiseState += coefficient * (white - fmNoiseState);
        else
            fmNoiseState = fmNoiseGenerator.process (params.fmNoiseType - 1, white);
    }
    float feedbackHistory[VoiceParams::numOscillators] {};
    float feedbackFiltered[VoiceParams::numOscillators] {};
    float feedbackCoeff[VoiceParams::numOscillators] {};
    float keyLevelGain[VoiceParams::numOscillators] { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    juce::Random random;
    juce::Random lfoPoolRandom { 27183 };
    juce::Random liveGrainRandom { 0x1f3a }; // seeds for live grains only

    juce::SmoothedValue<float> frameSmooth[VoiceParams::numOscillators];
    juce::SmoothedValue<float> levelSmooth[VoiceParams::numOscillators];
    juce::SmoothedValue<float> noiseSmooth;
    float warpNoiseLow = 0.0f; // FM Noise's own copy of that low-pass
    float noiseLow = 0.0f; // the heard noise's low-pass, while NOISE COLOUR is under white
    juce::SmoothedValue<float> noiseEnableSmooth, subOscColourSmooth;
    NoiseColours::Generator noiseGenerator, fmNoiseGenerator;
    juce::SmoothedValue<float> oscEnableSmooth[VoiceParams::numOscillators];
    WavetableOscillator subOsc;
    juce::SmoothedValue<float> subOscLevelSmooth, subOscEnableSmooth;

    // Modulation evaluated at the start of each block, for everything that
    // doesn't need to move within a block (envelope times, pans, detune...).
    std::array<float, (size_t) Mod::Destination::Count + Mod::numStereoDestinations> blockMods {};
    mutable std::array<float, 36> fmCellMods {};   // per-voice mods of the OSC 4-6 FM cells
    // Per-voice spectral amount mods: slot targets -1000 - osc; worked out once a block.
    mutable std::array<float, 6> spectralMods {};
    std::array<float, 6> spectralBlock {};
    std::array<bool, Mod::numStereoDestinations> stereoRouted {}; // a stereo route feeds Mod::stereoDestinations[i]
    // Whether Filter 1 (0) or 2 (1) has a stereo route (cutoff, resonance, morph or drive): its sides run apart.
    bool filterStereo (int filter) const
    {
        using namespace Mod::StereoIndex;
        return stereoRouted[(size_t) (cutoff1 + filter)] || stereoRouted[(size_t) (reso1 + filter)]
               || stereoRouted[(size_t) (morph1 + filter)] || stereoRouted[(size_t) (drive1 + filter)];
    }
    // A filter's drive on each side (1 = clean).
    struct DriveAmounts
    {
        float l = 1.0f, r = 1.0f;
    };

    // Per render (prepareModSlots): each slot's target (a mods index, or
    // -2 - cell for an OSC 4-6 FM cell, or -1 for none) and, for sources that
    // hold still through a render (velocity, key, wheels, macros, ...), its
    // amount, so the per-sample pass only evaluates the moving sources.
    std::array<int, Mod::maxSlots> slotTargets {};
    std::array<float, Mod::maxSlots> slotAmounts {};
    std::array<bool, Mod::maxSlots> slotHeld {};
    bool modSlotsPrepared = false;

    // Control-rate modulation (evaluateModsRated): each slot's group and the
    // routed destinations of each. The interval is updateSubBlock's.
    static constexpr int modControlInterval = 16;
    std::array<std::array<int, Mod::maxSlots>, 2> groupSlots {}, groupDestinations {}; // [0] slow, [1] per sample
    std::array<int, 2> numGroupSlots {}, numGroupDestinations {};
    std::array<float, (size_t) Mod::Destination::Count + Mod::numStereoDestinations> sampleMods {};

    double lfoPhases[VoiceParams::numLfos] {};
    double lfoIncrements[VoiceParams::numLfos] {};
    float lfoHolds[VoiceParams::numLfos] {};
    LfoChaos lfoChaos[VoiceParams::numLfos];
    float lfoValues[VoiceParams::numLfos] {};
    bool lfoSlow[VoiceParams::numLfos] {};
    // Envelopes read only at the control points (classifySlowEnvelopes),
    // by envelopeValues index: 0 ENV 1 (never), 1-4, 5-15 the extra ones.
    bool envSlow[17] {};
    // SUSTAIN VOICES fade (startFadeOut): samples left of fadeTotal.
    int fadeTotal = 0, fadeRemaining = 0;
    bool sounded = false;
    float ampGainFade = 1.0f;
    bool modFilterFm = false;
    float stepEnvelope (TensionAdsr& env, int index, bool control)
    {
        if (control || ! envSlow[index])
            return env.getNextSample();
        env.skip();
        return env.getCurrentValue();
    }
    // Release silence: a released voice ends once its output stays under
    // releaseSilence (-110 dB) for about 90 ms.
    static constexpr float releaseSilence = 3.0e-6f;
    bool watchReleaseSilence = false;
    float releasePeak = 0.0f;
    int releaseSilentSamples = 0;
    // Held sleep (CPU round 2026-10-31): a held note (key or pedal) whose
    // sources can only die away (struck and plucked strings, Modal Piano)
    // stops rendering once its output has stayed under releaseSilence for
    // about 90 ms, until the key comes up or the patch changes.
    bool watchHeldSilence = false, heldAsleep = false;
    int heldSilentSamples = 0;
    bool heldSleepAllowed() const;
    static const std::vector<bool>& filterDestinationMarks();
    // A filter (or the WEST gate) can ring on its own from the strings'
    // last -110 dB residue once pushed into self-oscillation, so a sleeping
    // note wakes the block any filter or WEST setting moves, and never
    // sleeps while a route modulates one (ilana 2026-10-31: "fix this").
    VoiceParams::FilterParams sleptFilter1, sleptFilter2;
    VoiceParams::WestParams sleptWest;
    float sleptFilterFm[2] {};
    void rememberFilterSettings();
    bool filterSettingsMoved() const;
    int sleptOscillators = 0;
    int enabledOscillators() const;
    // The strings' own output over the block (before the levels and the
    // amp): a note sleeps only once both it and the voice's output are silent.
    float heldSourcePeak = 0.0f;
    // M8.1: simulated shapes, SMOOTH and output B for the per-voice LFOs.
    LfoSim lfoSims[VoiceParams::numLfos];
    LfoSmoother lfoSmoothers[VoiceParams::numLfos];
    float lfoValuesB[VoiceParams::numLfos] {};
    float lfoSmoothCoefficients[VoiceParams::numLfos] {};
    unsigned lfoSeenTriggers[VoiceParams::numLfos] {};
    std::uint32_t lfoSimNotes = 0;

    double sampleRate = 44100.0;
    double baseFrequency = 440.0;
    double currentFrequency = 440.0;
    double bendSemitones = 0.0; // memberBend + masterBend
    double memberBend = 0.0, masterBend = 0.0;
    VoiceSharedState* shared = nullptr;
    float velocityLevel = 1.0f;
    // The note and velocity (1-127) a multisample picks its region by, and the
    // region each oscillator plays (null for a plain sample).
    int lastNote = 60, lastVelocity = 100;
    int pitchNote = 60; // the sounding note, legato included (scale quantise reads it)
    const SampleZone* sampleZone[VoiceParams::numOscillators] {};
    // TIME's engines: the rendered copy each oscillator plays (null: the
    // original), what it's a copy of, and the note's copy key, picked when
    // the note starts (stretchNew) so bends and tempo changes mid-note go
    // through the live stretch rather than another render.
    const SampleData* stretchCopy[VoiceParams::numOscillators] {};
    const SampleData* stretchOrigin[VoiceParams::numOscillators] {};
    int stretchStep[VoiceParams::numOscillators] {};
    int stretchSemis[VoiceParams::numOscillators] {};
    int stretchEngine[VoiceParams::numOscillators] {};
    bool stretchNew = true;
    bool noteHeld = false;
    // Step 33: a One Shot sample still plays after the key's release; the
    // envelopes' release waits for it (releaseEnvelopes).
    bool oneShotHold = false;
    void releaseEnvelopes();
    bool oneShotPlaying() const;

    // The Operator EG: the note's control side and its operator gains,
    // stepped every 64 samples and ramped in between (as msfa ramps them).
    Dx7::Note dx7Note;
    Dx7::Voice dx7Settings {}; // what the note last read (edits follow live)
    std::array<float, 6> dx7Previous {}, dx7Current {};
    int dx7Count = 0;
    bool dx7Playing = false; // the note started with the Operator EG in use
    // Dexed's output is the carriers' sum / 16 (msfa's >> 4 then >> 9).
    static constexpr float dx7CarrierScale = 1.0f / 16.0f;
    // An operator's level knob at 0.5 gives the DX7's own gain (the
    // importer sets 0.5), leaving room to push a modulator deeper.
    static constexpr float dx7LevelScale = 2.0f;
    // True while this oscillator plays the Operator EG.
    bool usesOperatorEg (int osc) const
    {
        return dx7Playing && params.oscillators[osc].ampEnv == OperatorEg::envelopeChoice;
    }
    // Every enabled oscillator plays the Operator EG, so its carriers decide
    // how long the voice lasts (as on the DX7).
    bool operatorEgOwnsVoice() const;
    float keyTrackValue = 0.0f;
    float keyTrackOctaves = 0.0f;
    float randomValue = 0.0f;
    float modWheel = 0.0f;
    float aftertouchValue = 0.0f;
    float expressionValue = 1.0f;

    float lastAmpValue = 0.0f;
    float lastLifetimeValue = 0.0f;
    float lastSamplePosition[VoiceParams::numOscillators] { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
    float lastFilterValue = 0.0f;
    float lastFilter2Value = 0.0f;
    float lastModValue = 0.0f;
    float lastEnv4Value = 0.0f;

    int numOscUnison[VoiceParams::numOscillators] { 1, 1, 1 };
    float panGainL[VoiceParams::numOscillators][VoiceParams::maxUnison] {};
    float panGainR[VoiceParams::numOscillators][VoiceParams::maxUnison] {};
    float panGainSubOscL = 0.7071f, panGainSubOscR = 0.7071f;
    float previousOsc[VoiceParams::numOscillators] {};
    // Per-unison-voice pitch offsets (semitones) and gains from the unison
    // mode, detune and blend.
    double unisonOffset[VoiceParams::numOscillators][VoiceParams::maxUnison] {};
    // exp2 (offset / 12), worked out per block rather than every sub-block.
    double unisonRatio[VoiceParams::numOscillators][VoiceParams::maxUnison] {};
    float unisonGains[VoiceParams::numOscillators][VoiceParams::maxUnison] {};
    float glideCoeff = 1.0f;
    bool hasPlayedNote = false;

    float driftValue = 0.0f;
    float driftTarget = 0.0f;
    juce::Random driftRandom;
    juce::Random sampleStartRandom { 0x5a17 }; // step 35: random sample starts, apart from the others
    juce::Random zoneRandom { 0x2b17 };        // step 37: Random round robin, apart from the others
    bool lastStringMode[VoiceParams::numOscillators] {};
    bool lastSampleMode[VoiceParams::numOscillators] {};
    bool lastEnabled[VoiceParams::numOscillators] { true, false, true };
    int lastUnison[VoiceParams::numOscillators] { 1, 1, 1 };
    float voicePan = 0.0f;

    bool monoPending = false;
    bool monoLegato = false;
    bool monoKeepRunning = false;
    bool monoGlide = true;
};
