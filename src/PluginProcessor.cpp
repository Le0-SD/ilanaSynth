#include "PluginProcessor.h"
#include "dsp/BuiltInTables.h"
#include "dsp/MultiSample.h"
#include "gui/SampleBrowserModel.h"
#include "dsp/PitchDetect.h"

#ifdef ILANA_TEST_BUILD
#include "dsp/Modulation.h"
juce::AudioProcessorEditor* IlanaSynthAudioProcessor::createEditor() { return nullptr; }
#else
#include "PluginEditor.h"
juce::AudioProcessorEditor* IlanaSynthAudioProcessor::createEditor()
{
    return new IlanaSynthAudioProcessorEditor (*this);
}
#endif

#include "processor/ProcessorInternal.h"

#if defined(_M_X64) || defined(__x86_64__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
 #include <emmintrin.h>
#endif

namespace
{
class WavetableSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};
} // namespace

#if JUCE_LINUX || JUCE_BSD
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wzero-as-null-pointer-constant", "-Wshadow", "-Wextra-semi",
                                     "-Wnon-virtual-dtor", "-Wsign-conversion")
 #include <pluginterfaces/base/funknown.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

// A host built with JUCE (pluginval, AudioPluginHost, Waveform...) gives
// every plugin instance a host context, and all of them share one run loop
// (RunLoop::Impl, a SharedResourcePointer) that holds the FDs our JUCE
// registers through IRunLoop. When the last instance is deleted, the
// context release frees that run loop from inside LinuxEventLoop's
// dispatchPendingEvents(), which has already copied the callbacks of every
// FD that was ready in that poll. If one of ours was ready (a timer
// message, which APVTS posts several times a second), the copied callback
// then runs on the freed run loop and writes into its freed map:
// valgrind shows it on the first "Open plugin (cold)" teardown, and the
// corrupted heap made pluginval segfault after printing SUCCESS now and
// then. The plugin can't reach into the host's loop, so it keeps the first
// host context it is given for the rest of the process (one reference,
// never released: releasing it later would only move the same free into
// another dispatch pass, and releasing at exit would call into a host
// that is shutting down). Linux only: the shared run loop is the Linux
// IRunLoop.
void IlanaSynthAudioProcessor::HostRunLoopKeepAlive::setIHostApplication (Steinberg::FUnknown* host)
{
    static std::atomic<Steinberg::FUnknown*> kept { nullptr };
    Steinberg::FUnknown* none = nullptr;

    if (host != nullptr && kept.compare_exchange_strong (none, host))
        host->addRef();
}
#endif

// Built once per process, on all cores (the 120 tables take a few
// seconds on one).
FactoryTables::FactoryTables()
{
    const auto names = TableFactory::getFactoryTableNames();
    const auto count = TableFactory::getNumFactoryTables();
    tables.resize ((size_t) count);
    std::atomic<int> next { 0 };
    const auto work = [&]
    {
        for (auto i = next++; i < count; i = next++)
        {
            auto table = std::make_unique<Wavetable>();
            table->setName (names[i]);
            table->buildFromFrames (TableFactory::generate (i));
            tables[(size_t) i] = std::move (table);
        }
    };
    std::vector<std::thread> threads;
    const auto helpers = juce::jlimit (0, 7, (int) std::thread::hardware_concurrency() - 1);
    for (int t = 0; t < helpers; ++t)
        threads.emplace_back (work);
    work();
    for (auto& thread : threads)
        thread.join();
}

const FactoryTables& FactoryTables::get()
{
    static FactoryTables instance;
    return instance;
}


IlanaSynthAudioProcessor::IlanaSynthAudioProcessor()
#if ILANA_FX
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
#else
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
#endif
      apvts (*this, &undoManager, "PARAMS", createParameterLayout())
{
    fxMain = std::make_unique<FxEngine> (*this);
    fxRackRunner = std::make_unique<PatchGraph::Runner>();
    fxRackRunner->setSettings ({ false, false }); // the slots time themselves; the rack sleeps as a whole
    fxPatchRunner = std::make_unique<PatchGraph::Runner>();
    fxPatchRunner->setSettings ({ false, true });
    fxPatchRunner->setTailFunction (fxTailSamples);

    if (fixedRandomSeeds().load())
        arpRandom.setSeed (31415);
    // The seed a patch load gives (cutPatchTails), so a new instance's random
    // LFOs and the FX gate's Random pattern are the same every time.
    lfoRandom.setSeed (27182);

    for (int step = 0; step < 16; ++step)
    {
        const auto n = juce::String (step + 1);
        pseqChanceIds[(size_t) step] = "pseq_chance" + n;
        pseqRangeIds[(size_t) step] = "pseq_range" + n;
        pseqRatchetIds[(size_t) step] = "pseq_ratchet" + n;
        arpVelocityIds[(size_t) step] = "arp_vel" + n;
        arpLengthIds[(size_t) step] = "arp_len" + n;
        arpPitchIds[(size_t) step] = "arp_pitch" + n;
    }

    for (auto* parameter : getParameters())
        parameter->addListener (&paramEpochListener);
    for (int slot = 1; slot <= numFxSlots; ++slot)
        if (auto* parameter = apvts.getParameter ("fx_slot" + juce::String (slot)))
            parameter->addListener (&fxSlotTypeListener);

    arpHeldNotes.ensureStorageAllocated (128);
    clipHeld.ensureStorageAllocated (128);
    clipActive.reserve (1024);
    arpChordActive.ensureStorageAllocated (128);
    arpChordNotes.ensureStorageAllocated (128);
    stretchRenderer = std::make_unique<StretchRenderer> ([this] (int osc) { return getSharedSampleForOsc (osc); });
    spectralCache = std::make_unique<SpectralCache> (
        [this] (int choice) -> std::shared_ptr<const Wavetable>
        {
            const auto& factory = FactoryTables::get().tables;
            const auto factoryCount = (int) factory.size();

            // (The factory tables live for the whole session: a non-owning pointer.)
            if (choice < factoryCount)
                return std::shared_ptr<const Wavetable> (std::shared_ptr<const Wavetable> {}, factory[(size_t) juce::jmax (0, choice)].get());

            if (choice >= factoryCount + numUserSlots)
            {
                const auto* built = BuiltInTables::build (choice - factoryCount - numUserSlots); // (the spectral worker, not the audio thread)
                return std::shared_ptr<const Wavetable> (std::shared_ptr<const Wavetable> {}, built != nullptr ? built : factory[0].get());
            }

            const juce::SpinLock::ScopedLockType lock (tableLock);
            return userTables[(size_t) juce::jlimit (0, numUserSlots - 1, choice - factoryCount)];
        },
        TableFactory::getNumFactoryTables() + numUserSlots + BuiltInTables::count());

    for (auto& value : modDisplayValues)
        value.store (0.0f);

    for (auto& value : displaySamplePositions)
        value.store (-1.0f);

    for (int i = 0; i < Mod::numMacros; ++i)
        macroCc[i].store (20 + i);

    for (size_t cc = 0; cc < ccParameter.size(); ++cc)
    {
        ccParameter[cc].store (-1);
        pendingCcValues[cc].store (0.0f);
        ccValuePending[cc].store (false);
    }

    scopeLeft.assign ((size_t) scopeSize, 0.0f);
    scopeRight.assign ((size_t) scopeSize, 0.0f);

    for (int lfo = 0; lfo < numLfos; ++lfo)
        for (int i = 0; i < lfoDrawSteps; ++i)
            lfoCustom[(size_t) lfo][(size_t) i] = (float) std::sin (juce::MathConstants<double>::twoPi * (double) i / (double) lfoDrawSteps);

    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        lfoCurves[(size_t) lfo] = LfoCurve::preset (0);
        lfoCurves[(size_t) lfo].renderTable (lfoCurveTables[(size_t) lfo].data());
    }

    FactoryTables::get();

    userTables.resize ((size_t) numUserSlots);
    sampleSlots.resize ((size_t) numSampleOscs);

    for (int i = 0; i < SampleFactory::getNumFactorySamples(); ++i)
        factorySamples[(size_t) i] = SampleFactory::generate (i);

    {
        for (int i = 0; i < OscillatorIds::count; ++i)
        {
            const juce::String prefix (OscillatorIds::prefixes[(size_t) i]);
            oscCoreIds[(size_t) i] = { prefix + "_on", prefix + "_table", prefix + "_frame", prefix + "_level",
                                       prefix + "_pan", prefix + "_semi", prefix + "_fine", prefix + "_unison",
                                       prefix + "_detune", prefix + "_spread", prefix + "_spectral",
                                       prefix + "_spectral_amt", prefix + "_chord", prefix + "_out",
                                       prefix + "_frame_blend" };

            stringParamIds[(size_t) i] = { prefix + "_mode", prefix + "_excite", prefix + "_string_decay",
                                           prefix + "_string_damp", prefix + "_string_sustain",
                                           prefix + "_string_stiffness", prefix + "_string_pickup", prefix + "_string_excite_pos",
                                           prefix + "_string_pick_hardness", prefix + "_string_pick_pos", prefix + "_string_slap" };
            bowBuzzIds[(size_t) i] = { prefix + "_bow_pressure", prefix + "_bow_speed",
                                        prefix + "_bridge_buzz", prefix + "_fret_rattle", prefix + "_string_split",
                                        prefix + "_string_click" };
            keysParamIds[(size_t) i] = { prefix + "_hammer_hard", prefix + "_couple",
                                          prefix + "_damper", prefix + "_register" };
            electricParamIds[(size_t) i] = { prefix + "_ep_distance", prefix + "_ep_position" };
            feedbackParamIds[(size_t) i] = { prefix + "_fb_gain", prefix + "_fb_distance" };
            grainLiveIds[(size_t) i] = prefix + "_grain_live";
            sampleParamIds[(size_t) i] = { prefix + "_sample_tuned", prefix + "_sample_loop",
                                           prefix + "_sample_reverse", prefix + "_sample_start",
                                           prefix + "_sample_end", prefix + "_sample_fade_in",
                                           prefix + "_sample_fade_out" };
            sampleRootIds[(size_t) i] = prefix + "_sample_root";
            sampleLoopIds[(size_t) i] = { prefix + "_sample_loop_own", prefix + "_sample_loop_start", prefix + "_sample_loop_end", prefix + "_sample_loop_xfade" };
            sampleTempoIds[(size_t) i] = { prefix + "_sample_sync", prefix + "_sample_bpm", prefix + "_sample_stretch" };
            samplePlayIds[(size_t) i] = { prefix + "_sample_playback", prefix + "_sample_pingpong", prefix + "_sample_start_rand",
                                          prefix + "_sample_start_safe", prefix + "_sample_quality", prefix + "_sample_unmapped",
                                          prefix + "_sample_round_robin", prefix + "_sample_vel_blend" };
            grainParamIds[(size_t) i] = { prefix + "_grain_size", prefix + "_grain_density",
                                         prefix + "_grain_spray", prefix + "_grain_pitch",
                                         prefix + "_grain_spread", prefix + "_grain_pos",
                                         prefix + "_grain_scan", prefix + "_grain_freeze" };
        }
    }

    for (int i = 0; i < Mod::maxSlots; ++i)
    {
        const auto prefix = "mod" + juce::String (i + 1);
        modSlotIds[(size_t) i] = { prefix + "_src", prefix + "_dst", prefix + "_amt", prefix + "_curve",
                                   prefix + "_pol", prefix + "_aux", prefix + "_byp", prefix + "_stereo" };

        const auto& ids = modSlotIds[(size_t) i];
        modSlotRaw[(size_t) i] = { apvts.getRawParameterValue (ids.src), apvts.getRawParameterValue (ids.dst),
                                   apvts.getRawParameterValue (ids.amt), apvts.getRawParameterValue (ids.curve),
                                   apvts.getRawParameterValue (ids.polarity), apvts.getRawParameterValue (ids.aux),
                                   apvts.getRawParameterValue (ids.bypass), apvts.getRawParameterValue (ids.stereo) };
    }

    for (const auto& destination : Mod::getParamDestinations())
    {
        ParamDestination entry;
        entry.raw = apvts.getRawParameterValue (destination.id);
        entry.parameter = apvts.getParameter (destination.id);

        if (entry.raw != nullptr)
            rawToParamDestination[entry.raw] = (int) paramDestinations.size();

        paramDestinations.push_back (entry);
    }

    for (int env = 6; env <= 16; ++env)
    {
        const auto prefix = "env" + juce::String (env);
        extraEnvIds[(size_t) (env - 6)] = { prefix + "_attack", prefix + "_decay", prefix + "_sustain",
                                            prefix + "_release", prefix + "_curve", prefix + "_velocity" };
    }

    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
        oscAmpEnvIds[(size_t) osc] = prefix + "_amp_env";
        for (size_t field = 0; field < OperatorEg::operatorFields().size(); ++field)
            operatorEgIds[(size_t) osc][field] = prefix + OperatorEg::operatorFields()[field].suffix;
        sampleFactoryIds[(size_t) osc] = prefix + "_sample_factory";

        for (int target = 0; target < OscillatorIds::count; ++target)
            fmMatrixIds[(size_t) osc][(size_t) target] = fmRouteId (osc, target);
    }
    for (size_t field = 0; field < OperatorEg::voiceFields().size(); ++field)
        operatorEgVoiceIds[field] = OperatorEg::voiceFields()[field].suffix;

    buildParamCache();

    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        const auto prefix = "lfo" + juce::String (lfo + 1);
        auto& ids = lfoIds[(size_t) lfo];
        ids.shape = prefix + "_shape";
        ids.rate = prefix + "_rate";
        ids.sync = prefix + "_sync";
        ids.div = prefix + "_div";
        ids.retrig = prefix + "_retrig";
        ids.phase = prefix + "_phase";
        ids.key = prefix + "_key";
        ids.physA = prefix + "_phys_a";
        ids.physB = prefix + "_phys_b";
        ids.kick = prefix + "_kick";
        for (int param = 0; param < LfoSimInfo::numParams; ++param)
            ids.sim[(size_t) param] = prefix + "_p" + juce::String (param + 1);
        ids.smooth = prefix + "_smooth";
        ids.axis = prefix + "_axis";
        ids.trigger = prefix + "_trigger";
        ids.loop = prefix + "_loop";
        ids.seed = prefix + "_seed";
        ids.stereo = prefix + "_stereo";
        ids.fire = prefix + "_fire";

        for (int step = 0; step < 16; ++step)
            ids.steps[(size_t) step] = prefix + "_step" + juce::String (step + 1);
    }

    for (int m = 0; m < Mod::numMacros; ++m)
    {
        macroIds[(size_t) m] = "macro" + juce::String (m + 1);
        evolveIds[(size_t) m] = { "macro" + juce::String (m + 1) + "_evolve", "macro" + juce::String (m + 1) + "_evolve_rate" };
    }
    for (int point = 0; point < numVectorPoints; ++point)
        vectorPathIds[(size_t) point] = { "vec_px" + juce::String (point + 1), "vec_py" + juce::String (point + 1) };
    evolve.reset (4242u);
    vectorDrift.reset (9191u);

    for (int slot = 0; slot < numFxSlots; ++slot)
    {
        const auto prefix = "fx_slot" + juce::String (slot + 1);
        fxSlotIds[(size_t) slot] = { prefix, prefix + "_bypass", prefix + "_solo", prefix + "_mix", prefix + "_band" };
    }

    {
        for (int osc = 0; osc < OscillatorIds::count; ++osc)
        {
            const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
            oscShapeIds[(size_t) osc] = { prefix + "_warp", prefix + "_warp_amt", prefix + "_uni_mode", prefix + "_uni_blend",
                                          prefix + "_route", prefix + "_uni_frame", prefix + "_uni_warp",
                                          prefix + "_scale", prefix + "_scale_root" };
            operatorIds[(size_t) osc] = { prefix + "_tune", prefix + "_ratio", prefix + "_ratio_snap", prefix + "_fixed_hz",
                                          prefix + "_key_level", prefix + "_fb_type", prefix + "_warp2", prefix + "_warp2_amt",
                                          prefix + "_pd_env", prefix + "_pd_env_amt" };
            fmNoiseIds[(size_t) osc] = "fm_noise" + juce::String (osc + 1);
        }

        for (int env = 0; env < 16; ++env)
        {
            const auto prefix = envelopePrefix (env);
            envelopeExtraIds[(size_t) env] = { prefix + "_delay", prefix + "_hold", prefix + "_keyrate",
                                               prefix + "_acurve", prefix + "_dcurve", prefix + "_rcurve" };
        }

        for (int point = 0; point < Mseg::numPoints; ++point)
        {
            msegLevelIds[(size_t) point] = "mseg_level" + juce::String (point + 1);
            msegTimeIds[(size_t) point] = "mseg_time" + juce::String (point + 1);
        }
    }

    for (int step = 0; step < 16; ++step)
    {
        tapStepIds[(size_t) step] = "fx_taps_step" + juce::String (step + 1);
        gateStepIds[(size_t) step] = "fx_gate_step" + juce::String (step + 1);
    }

    for (int i = 0; i < numUserSlots; ++i)
        resetUserTableToDefault (i);

    synth.addSound (new WavetableSound());

    // The poly limit counts numVoices of them; the spares let a stolen voice fade out.
    for (int i = 0; i < numVoices + numSpareVoices; ++i)
        synth.addVoice (new Voice());
    synth.setSpareVoices (numSpareVoices);

    synth.setNoteStealingEnabled (true);

    // LFO 1-4 draw from lfoRandom exactly as before the pool; LFO 5-16 have
    // their own generator so they never shift anyone else's random sequence.
    for (int lfo = 0; lfo < numLfos; ++lfo)
        lfoSampleHolds[(size_t) lfo].store (randomForLfo (lfo).nextFloat() * 2.0f - 1.0f);

    for (int lfo = 0; lfo < numLfos; ++lfo)
        lfoChaos[(size_t) lfo].reset (randomForLfo (lfo));

    // M7.5: ilanaSynth FX opens on a patch that plays its input.
    if (isEffectBuild)
        if (const auto index = getFactoryPresetNames().indexOf ("Live Body"); index >= 0)
            loadFactoryPreset (index);
}

namespace
{
std::uint32_t hashParamId (const char* id) noexcept
{
    // FNV-1a.
    std::uint32_t hash = 2166136261u;

    for (; *id != 0; ++id)
        hash = (hash ^ (std::uint8_t) *id) * 16777619u;

    return hash;
}
} // namespace

void IlanaSynthAudioProcessor::buildParamCache()
{
    const auto& parameters = getParameters();
    std::uint32_t size = 1;

    while (size < (std::uint32_t) parameters.size() * 4)
        size <<= 1;

    paramCache.assign (size, {});
    paramCacheMask = size - 1;
    paramCacheIds.clear();
    paramCacheIds.reserve ((size_t) parameters.size());

    for (auto* parameter : parameters)
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
            paramCacheIds.push_back (withId->paramID);

    for (const auto& id : paramCacheIds)
    {
        ParamCacheEntry entry;
        entry.index = (int) (&id - paramCacheIds.data());
        entry.id = id.toRawUTF8();
        entry.hash = hashParamId (entry.id);
        entry.value = apvts.getRawParameterValue (id);

        if (entry.value == nullptr)
            continue;

        // A stored choice or step isn't always an exact integer (index 7 of
        // 136 comes back as 6.9999995), and (int) casts would truncate it.
        {
            auto* parameter = apvts.getParameter (id);
            entry.discrete = dynamic_cast<juce::AudioParameterChoice*> (parameter) != nullptr
                             || dynamic_cast<juce::AudioParameterInt*> (parameter) != nullptr
                             || dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr;
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                entry.fallback = ranged->convertFrom0to1 (ranged->getDefaultValue());
        }

        if (const auto found = rawToParamDestination.find (entry.value); found != rawToParamDestination.end())
            entry.destination = found->second;

        for (auto slot = entry.hash & paramCacheMask;; slot = (slot + 1) & paramCacheMask)
        {
            if (paramCache[slot].id == nullptr)
            {
                paramCache[slot] = entry;
                break;
            }
        }
    }
}

const IlanaSynthAudioProcessor::ParamCacheEntry* IlanaSynthAudioProcessor::findParam (const char* id) const
{
    if (paramCache.empty())
        return nullptr;

    const auto hash = hashParamId (id);

    for (auto slot = hash & paramCacheMask;; slot = (slot + 1) & paramCacheMask)
    {
        const auto& candidate = paramCache[slot];

        if (candidate.id == nullptr)
            return nullptr; // not a parameter

        if (candidate.hash == hash && std::strcmp (candidate.id, id) == 0)
            return &candidate;
    }
}

// Set while this thread runs processChunk. Modulation offsets are the audio
// thread's: another thread (the editor, a preset load) reads the plain
// value, rather than racing on offsets the audio thread rewrites each block
// (a preset load chose its macros' directions from them).
//
// fxValues (roadmap step 17): while an FX slot that is its own instance runs,
// its effect's parameters read the instance's own values (a table indexed by
// ParamCacheEntry::index, NaN where the parameter isn't the effect's). Per
// thread, so the editor reading the same parameter meanwhile sees the
// parameter itself.
namespace
{
struct AudioThreadParams
{
    bool reading = false;
    const float* fxValues = nullptr;
    const IlanaSynthAudioProcessor::FxLfoSlotMods* fxLfoMods = nullptr;
};
thread_local AudioThreadParams audioThreadParams;
} // namespace

const float* IlanaSynthAudioProcessor::swapFxParamValues (const float* values) noexcept
{
    return std::exchange (audioThreadParams.fxValues, values);
}

const IlanaSynthAudioProcessor::FxLfoSlotMods* IlanaSynthAudioProcessor::swapFxLfoMods (const FxLfoSlotMods* mods) noexcept
{
    return std::exchange (audioThreadParams.fxLfoMods, mods);
}

float IlanaSynthAudioProcessor::readParam (const ParamCacheEntry& entry) const
{
    auto& thread = audioThreadParams;
    auto result = entry.value->load();
    if (thread.fxValues != nullptr && entry.index >= 0)
        if (const auto own = thread.fxValues[entry.index]; own == own)
            result = own;
    // A host (or a damaged state) can set NaN, which parameters don't clamp.
    if (! std::isfinite (result))
        result = entry.fallback;
    // The FX patcher's LFO cables into the slot that is running (FxLfo.h):
    // an offset in the knob's own range, clamped to it.
    if (const auto* mods = thread.fxLfoMods; mods != nullptr && entry.index >= 0)
        for (int k = 0; k < mods->count; ++k)
            if (mods->index[(size_t) k] == entry.index)
            {
                const auto* parameter = mods->parameter[(size_t) k];
                const auto moved = parameter->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, parameter->convertTo0to1 (result) + mods->offset[(size_t) k]));
                if (std::isfinite (moved))
                    result = moved;
                break;
            }
    if (entry.discrete)
        result = std::round (result);

    if (thread.reading && anyParamModulation && entry.destination >= 0)
    {
        const auto offset = paramDestinationOffsets[(size_t) entry.destination];

        if (offset != 0.0f)
            if (auto* parameter = paramDestinations[(size_t) entry.destination].parameter)
                result = parameter->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, parameter->convertTo0to1 (result) + offset));
        if (entry.discrete)
            result = std::round (result);
    }

    return result;
}

float IlanaSynthAudioProcessor::getParam (const char* id) const
{
    const auto* entry = findParam (id);
    return entry != nullptr ? readParam (*entry) : 0.0f;
}

float IlanaSynthAudioProcessor::getRawParam (const ParamRef& ref) const
{
    auto* entry = ref.entry.load (std::memory_order_relaxed);

    if (entry == nullptr)
    {
        entry = findParam (ref.toRawUTF8());

        if (entry == nullptr)
            return 0.0f;

        ref.entry.store (entry, std::memory_order_relaxed);
    }

    auto value = entry->value->load();
    if (const auto* own = audioThreadParams.fxValues; own != nullptr && entry->index >= 0 && own[entry->index] == own[entry->index])
        value = own[entry->index];
    if (! std::isfinite (value))
        value = entry->fallback;
    return entry->discrete ? std::round (value) : value;
}

float IlanaSynthAudioProcessor::getParam (const ParamRef& ref) const
{
    auto* entry = ref.entry.load (std::memory_order_relaxed);

    if (entry == nullptr)
    {
        entry = findParam (ref.toRawUTF8());

        if (entry == nullptr)
            return 0.0f;

        ref.entry.store (entry, std::memory_order_relaxed);
    }

    return readParam (*entry);
}

void IlanaSynthAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    noteSpray.reset();
    arpHeldNotes.clearQuick();   // what's sounding is released by its gate
    generatedMidi.ensureSize (4096);
    midiForSynth.ensureSize (4096);
    clipScratch.ensureSize (8192);
    clipActive.clear();
    clipHeld.clearQuick();
    clipRunning = clipUsedHost = false;
    clipLastIndex = clipLastMode = -1;
    // A new stream: nothing to ease from.
    for (int channel = 0; channel < 2; ++channel)
        lastOutput[channel] = declick[channel] = 0.0f;
    resetBlockRamps();

    currentSampleRate = sampleRate;
    sympatheticStrings.prepare (sampleRate);
    soundboard.prepare (sampleRate);
    denseSoundboard.prepare (sampleRate);
    pedalResonance.prepare (sampleRate);
    mechanicalNoise.prepare (sampleRate);
    keysPedalDown = false;
    pedalHeldNotes.fill (false);
    displaySampleRate.store (sampleRate);
    baseSampleRate = sampleRate;
    expectedBlockSize = juce::jmax (1, samplesPerBlock);

    oversamplingFactor.store (wantedOversamplingFactor());

    for (auto* oversampler : { &oversampler2x, &oversampler4x })
    {
        oversampler->initProcessing ((size_t) expectedBlockSize);
        oversampler->reset();
    }

    const auto voiceRate = baseSampleRate * (double) oversamplingFactor.load();

    synth.setCurrentPlaybackSampleRate (voiceRate);
    // A new stream (a buffer-size or rate change in the host): notes still
    // held from the old one stop here, as the pedal and the arp's held keys
    // above do. Voices that carried on across the change buzzed on the PC
    // until their keys were let go and pressed again (ilana, 2026-10-10;
    // not reproduced on Linux, where every output matched).
    synth.allNotesOff (0, false);
    numHeldKeys = 0;
    numResumeKeys.store (0, std::memory_order_release);
    msegKeyDown.fill (false);
    msegKeysDown = 0;
    restoreSustainPedal();
    // MULTI-CORE: up to three more threads (the host runs its own too), with
    // buffers for a chunk at the highest oversampling.
    synth.prepareVoiceThreads (juce::jlimit (0, 3, juce::SystemStats::getNumCpus() - 1), expectedBlockSize * 4);

    scaledMidiBuffer.ensureSize (4096); // (a dense MPE block; sized like chunkMidi)

    lfoBuffers.setSize (numLfoChannels, expectedBlockSize * oversamplingFactor.load(), false, false, true);

    // PARALLEL FX routing's buffers, so the audio thread need not size them.
    fxParallelIn.setSize (2, expectedBlockSize, false, true, true);
    fxBranch.setSize (2, expectedBlockSize, false, true, true);
    fxScratch.setSize (2, expectedBlockSize, false, true, true);
    fxScratchOld.setSize (2, expectedBlockSize, false, true, true);
    fxBand.setSize (2, expectedBlockSize, false, true, true);

    // The first-use tables of the modulation lookups, built here and not on
    // the first audio block.
    (void) Mod::extendedFmCellFor (0);
    (void) Mod::spectralOscFor (0);

    // M7.5: room for the input at up to 4x oversampling, and 3 s of history.
    liveDry.setSize (2, expectedBlockSize, false, true, false);
    dryDelayRing.setSize (2, 1024, false, true, false);
    dryDelayRing.clear();
    dryDelayWrite = 0;
    liveVoice.assign ((size_t) expectedBlockSize * 4, 0.0f);
    liveEnvVoice.assign ((size_t) expectedBlockSize * 4, 0.0f);
    if (isEffectBuild)
    {
        liveHistory.buffer.setSize (2, (int) (sampleRate * 3.0), false, true, false);
        liveHistory.buffer.clear();
        liveHistory.sampleRate = sampleRate;
        liveHistory.name = "Live input";
    }
    liveHistoryWrite = 0;
    liveLast = liveEnvState = 0.0f;
    liveGateOpen = false;
    liveGateNote = -1;
    liveInputSamples = 0;
    for (auto& stage : dcBlock)
        for (auto& channel : stage)
            channel = { 0.0f, 0.0f };
    fxAsleep = false;
    fxSilentSamples = 0;

    mseg.prepare (voiceRate);
    mseg.reset();
    clockShPhase = 0.0;
    clockShValue = 0.0f;

    // The effects' memory: the shared engine and every slot's own instance.
    prepareFxEngines (sampleRate, samplesPerBlock);
    {
        const juce::ScopedLock edit (fxEditLock);
        fxRackRunner->prepare (sampleRate, expectedBlockSize, 2);
        fxPatchRunner->prepare (sampleRate, expectedBlockSize, 2);
        compileFxPlansNow();
        freeRetiredFx (true);
    }
    chunkMidi.ensureSize (4096);
    updateLatency();
}

// A reset forgets the sustain pedal (the synth's allNotesOff clears it), but
// the pedal is still physically down: tell the synth again.
void IlanaSynthAudioProcessor::restoreSustainPedal()
{
    for (int channel = 0; channel < 16; ++channel)
        if (pedalChannelDown[(size_t) channel])
            synth.handleController (channel + 1, 64, 127);
}

// The keys the synth was told are down, in the order they went down.
void IlanaSynthAudioProcessor::trackHeldKeys (const juce::MidiBuffer& midi) noexcept
{
    const auto remove = [this] (int channel, int note)
    {
        for (int i = 0; i < numHeldKeys; ++i)
            if (heldKeys[(size_t) i].channel == channel && heldKeys[(size_t) i].note == note)
            {
                for (int j = i + 1; j < numHeldKeys; ++j)
                    heldKeys[(size_t) (j - 1)] = heldKeys[(size_t) j];
                --numHeldKeys;
                return;
            }
    };

    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (message.isNoteOn())
        {
            remove (message.getChannel(), message.getNoteNumber());
            if (numHeldKeys == maxHeldKeys) // (the oldest goes)
            {
                for (int j = 1; j < numHeldKeys; ++j)
                    heldKeys[(size_t) (j - 1)] = heldKeys[(size_t) j];
                --numHeldKeys;
            }
            heldKeys[(size_t) numHeldKeys++] = { (juce::uint8) message.getChannel(), (juce::uint8) message.getNoteNumber(),
                                                 (juce::uint8) message.getVelocity() };
        }
        else if (message.isNoteOff())
        {
            remove (message.getChannel(), message.getNoteNumber());
        }
        else if (message.isAllNotesOff() || message.isAllSoundOff())
        {
            numHeldKeys = 0;
        }
    }
}

// The keys that were held when the voices were re-prepared (setOversampling)
// go to the synth again, at the start of this block. The output eases from
// where it was if no block went missing in between; if the host heard
// silence meanwhile it starts from silence.
void IlanaSynthAudioProcessor::playResumedKeys (juce::MidiBuffer& midi, int blockSamples) noexcept
{
    const auto count = numResumeKeys.exchange (0, std::memory_order_acq_rel);
    if (count <= 0)
        return;

    // (Only the keys still down: this block's own events have been tracked.)
    for (int i = 0; i < juce::jmin (count, maxHeldKeys); ++i)
    {
        const auto key = resumeKeys[(size_t) i];
        for (int j = 0; j < numHeldKeys; ++j)
            if (heldKeys[(size_t) j].channel == key.channel && heldKeys[(size_t) j].note == key.note)
            {
                midi.addEvent (juce::MidiMessage::noteOn (key.channel, key.note, key.velocity), 0);
                break;
            }
    }

    const auto blockMs = 1000.0 * (double) blockSamples / juce::jmax (1.0, baseSampleRate);
    if ((double) (juce::Time::getMillisecondCounter() - fxLastRackMs.load (std::memory_order_relaxed)) < 2.0 * blockMs + 2.0)
    {
        easeNextBlock();
    }
    else
    {
        for (int channel = 0; channel < 2; ++channel)
            lastOutput[channel] = declick[channel] = 0.0f;
    }
}

// A new patch starts from silence: the old voices stop and the effects'
// memory (reverb, delays, freeze, the piano body) is cleared, on the audio
// thread and without allocating.
void IlanaSynthAudioProcessor::cutPatchTails()
{
    synth.allNotesOff (0, false);
    numHeldKeys = 0; // (a new patch does not take the old one's held keys back)
    numResumeKeys.store (0, std::memory_order_release);
    restoreSustainPedal();
    for (int i = 0; i < synth.getNumVoices(); ++i)
        if (auto* voice = dynamic_cast<Voice*> (synth.getVoice (i)))
            voice->resetForNewPatch();
    noteSpray.reset();

    for (auto& stage : dcBlock)
        for (auto& channel : stage)
            channel = { 0.0f, 0.0f };
    fxAsleep = false;
    fxSilentSamples = 0;
    resetBlockRamps();

    for (int channel = 0; channel < 2; ++channel)
        declick[channel] = lastOutput[channel];

    resetFxEngines();
    sympatheticStrings.reset();
    soundboard.reset();
    denseSoundboard.reset();
    pedalResonance.reset();
    mechanicalNoise.reset();
    // (The band splitters' filters; the effects' own memory is in
    // resetFxEngines above.)
    for (auto& split : fxSplit)
    {
        for (auto& stage : split.lowA)
            for (auto& filter : stage)
                filter.reset();
        for (auto& stage : split.lowB)
            for (auto& filter : stage)
                filter.reset();
        for (auto& filter : split.allLow)
            filter.reset();
        for (auto& filter : split.allHigh)
            filter.reset();
    }

    // The modulators start over too, as in a new instance, so a patch sounds
    // the same whatever played before it (a slow free-running LFO otherwise
    // starts wherever the last patch left it).
    lfoRandom.setSeed (27182);
    lfoPoolRandom.setSeed (31415);
    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        lfoPhases[(size_t) lfo] = 0.0;
        lfoSampleHolds[(size_t) lfo].store (randomForLfo (lfo).nextFloat() * 2.0f - 1.0f);
        lfoFireWas[(size_t) lfo] = false;
        lfoBeatLast[(size_t) lfo] = -1;
        lfoSims[(size_t) lfo].restart();
        lfoSmoothers[(size_t) lfo].reset();
        lfoPreviousShapes[(size_t) lfo] = -1;
    }
    lfoSimSeedCounter = 1;
    for (int lfo = 0; lfo < numLfos; ++lfo)
        lfoChaos[(size_t) lfo].reset (randomForLfo (lfo));
    mseg.reset();
    clockShPhase = 0.0;
    clockShValue = 0.0f;
    vectorPathPhase = 0.0;
    evolve.reset (4242u);
    vectorDrift.reset (9191u);
    for (auto& drift : macroDrift)
        drift.store (0.0f);
}

// The oversamplers' filters delay the output a little; the host compensates.
void IlanaSynthAudioProcessor::updateLatency()
{
    const auto factor = oversamplingFactor.load();
    const auto latency = factor > 1 ? juce::roundToInt (activeOversampler().getLatencyInSamples()) : 0;
    setLatencySamples (latency);

    const auto delay = juce::jlimit (0, juce::jmax (0, dryDelayRing.getNumSamples() - 1), latency);
    if (delay != dryDelaySamples)
    {
        dryDelaySamples = delay;
        dryDelayRing.clear();
        dryDelayWrite = 0;
    }
}

void IlanaSynthAudioProcessor::delayLiveDry (int numSamples)
{
    const auto size = dryDelayRing.getNumSamples();
    if (dryDelaySamples <= 0 || size == 0)
        return;

    auto write = dryDelayWrite;
    for (int channel = 0; channel < juce::jmin (2, liveDry.getNumChannels()); ++channel)
    {
        auto* data = liveDry.getWritePointer (channel);
        auto* ring = dryDelayRing.getWritePointer (channel);
        write = dryDelayWrite;
        for (int i = 0; i < numSamples; ++i)
        {
            const auto read = (write - dryDelaySamples + size) % size;
            ring[write] = data[i];
            data[i] = ring[read];
            write = (write + 1) % size;
        }
    }
    dryDelayWrite = write;
}

int IlanaSynthAudioProcessor::wantedOversamplingFactor() const
{
    if (getParam ("oversampling") < 0.5f)
        return 1;

    // At 88.2 kHz and up the voices already run fast enough: the factor drops one
    // step there and two from 176.4 kHz, so the internal rate stays near
    // 100-200 kHz (Vital halves its factor the same way). At 44.1 and 48 kHz
    // the choice is used as set.
    const auto factor = (int) getParam ("os_factor") == 1 ? 4 : 2;
    const auto steps = baseSampleRate >= 176400.0 ? 2 : (baseSampleRate >= 88200.0 ? 1 : 0);
    return juce::jmax (1, factor >> steps);
}

void IlanaSynthAudioProcessor::setOversampling (int factor)
{
    factor = factor >= 4 ? 4 : (factor >= 2 ? 2 : 1);

    if (factor == oversamplingFactor.load())
        return;

    suspendProcessing (true);

    // The keys held down now (E-12): the voices are re-prepared below, which
    // cuts every note, so the next block plays these again.
    {
        const auto count = numHeldKeys;
        for (int i = 0; i < count; ++i)
            resumeKeys[(size_t) i] = heldKeys[(size_t) i];
        numResumeKeys.store (count, std::memory_order_release);
    }

    oversamplingFactor.store (factor);

    for (auto* oversampler : { &oversampler2x, &oversampler4x })
    {
        oversampler->reset();
        oversampler->initProcessing ((size_t) expectedBlockSize);
    }

    const auto voiceRate = baseSampleRate * (double) factor;

    synth.setCurrentPlaybackSampleRate (voiceRate);
    restoreSustainPedal();
    mseg.prepare (voiceRate);
    mseg.reset();
    lfoBuffers.setSize (numLfoChannels, expectedBlockSize * factor, false, false, true);
    scaledMidiBuffer.ensureSize (4096); // (a dense MPE block; sized like chunkMidi)
    updateLatency();

    suspendProcessing (false);
}

void IlanaSynthAudioProcessor::releaseResources()
{
}

bool IlanaSynthAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
   #if ILANA_FX
    if (input != juce::AudioChannelSet::disabled() && input != juce::AudioChannelSet::mono()
        && input != juce::AudioChannelSet::stereo())
        return false;
   #else
    if (input != juce::AudioChannelSet::disabled())
        return false;
   #endif

    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::stereo() || output == juce::AudioChannelSet::mono();
}

void IlanaSynthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Flush-to-zero and denormals-are-zero from the first instruction of the
    // callback: hosts reset MXCSR between calls (see docs/ROUTING-CPU.md).
    juce::ScopedNoDenormals noDenormals;

    const auto total = buffer.getNumSamples();

    if (total <= expectedBlockSize)
    {
        processChunk (buffer, midiMessages);
        mixSamplePreview (buffer);
        return;
    }

    // A host may hand over a longer block than it announced (some offline
    // renders do); the oversamplers are sized for the announced one.
    for (int start = 0; start < total; start += expectedBlockSize)
    {
        const auto length = juce::jmin (expectedBlockSize, total - start);
        juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), start, length);
        chunkMidi.clear();
        chunkMidi.addEvents (midiMessages, start, length, -start);
        processChunk (chunk, chunkMidi);
    }

    mixSamplePreview (buffer);
}

void IlanaSynthAudioProcessor::mixSamplePreview (juce::AudioBuffer<float>& buffer) noexcept
{
    // A new clip (or a stop) from the message thread; if it holds the lock
    // right now, the next block picks it up.
    const auto serial = previewSerial.load (std::memory_order_acquire);
    if (serial != previewSeenSerial)
    {
        const juce::SpinLock::ScopedTryLockType lock (previewLock);
        if (lock.isLocked())
        {
            previewSeenSerial = serial;
            if (previewPending != nullptr)
            {
                previewActive = previewPending;
                previewPosition = 0.0;
                previewFadeIn = 128;
                previewFadeOut = 0;
                previewPlaying.store (true);
            }
            else if (previewActive != nullptr && previewPlaying.load())
            {
                previewFadeOut = 512;
            }
        }
    }

    if (previewActive == nullptr || ! previewPlaying.load())
        return;

    const auto& clip = *previewActive;
    const auto length = clip.buffer.getNumSamples();
    const auto step = clip.sampleRate / juce::jmax (1.0, baseSampleRate);
    const auto clipChannels = clip.buffer.getNumChannels();
    const auto outChannels = buffer.getNumChannels();
    const auto fadeOutLength = 512;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const auto index = (int) previewPosition;
        if (index >= length - 1)
        {
            previewPlaying.store (false);
            return;
        }

        const auto fraction = (float) (previewPosition - (double) index);
        auto gain = clip.gain;
        if (previewFadeIn > 0)
            gain *= 1.0f - (float) --previewFadeIn / 128.0f;
        if (previewFadeOut > 0)
        {
            gain *= (float) previewFadeOut / (float) fadeOutLength;
            if (--previewFadeOut == 0)
            {
                previewPlaying.store (false);
                return;
            }
        }

        for (int c = 0; c < outChannels && c < 2; ++c)
        {
            const auto* data = clip.buffer.getReadPointer (juce::jmin (c, clipChannels - 1));
            buffer.getWritePointer (c)[i] += (data[index] + (data[index + 1] - data[index]) * fraction) * gain;
        }

        previewPosition += step;
    }
}

bool IlanaSynthAudioProcessor::startSamplePreview (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples <= 1 || reader->sampleRate <= 0.0)
        return false;

    const auto numSamples = (int) juce::jmin (reader->lengthInSamples,
                                              (juce::int64) (reader->sampleRate * SampleBrowserModel::previewSeconds));
    auto clip = std::make_shared<PreviewClip>();
    clip->sampleRate = reader->sampleRate;
    clip->buffer.setSize (juce::jlimit (1, 2, (int) reader->numChannels), numSamples);

    if (! reader->read (&clip->buffer, 0, numSamples, 0, true, true))
        return false;

    scrubAudio (clip->buffer);
    const auto level = SampleBrowserModel::measureLevel (clip->buffer);
    clip->gain = SampleBrowserModel::previewGain (level.rms, level.peak);
    previewGain.store (clip->gain);

    {
        const juce::SpinLock::ScopedLockType lock (previewLock);
        previewRetired[(size_t) previewRetiredIndex] = std::move (previewPending);
        previewRetiredIndex = (previewRetiredIndex + 1) % (int) previewRetired.size();
        previewPending = std::move (clip);
        previewSerial.fetch_add (1, std::memory_order_release);
    }

    return true;
}

void IlanaSynthAudioProcessor::stopSamplePreview()
{
    const juce::SpinLock::ScopedLockType lock (previewLock);
    if (previewPending == nullptr)
        return;

    previewRetired[(size_t) previewRetiredIndex] = std::move (previewPending);
    previewRetiredIndex = (previewRetiredIndex + 1) % (int) previewRetired.size();
    previewPending = nullptr;
    previewGain.store (1.0f);
    previewSerial.fetch_add (1, std::memory_order_release);
}

void IlanaSynthAudioProcessor::captureLiveInput (juce::AudioBuffer<float>& buffer)
{
    const auto numSamples = juce::jmin (buffer.getNumSamples(), liveDry.getNumSamples());
    const auto inputs = juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels());
    for (int channel = 0; channel < 2; ++channel)
        liveDry.copyFrom (channel, 0, buffer, juce::jmin (channel, inputs - 1), 0, numSamples);
    liveInputSamples = numSamples;
    buffer.clear();
}

// Once per block: INPUT GAIN, the envelope follower, the history for live
// grains, the input at the voice rate, and GATE / DRONE notes.
void IlanaSynthAudioProcessor::prepareLiveInput (int numSamples, int factor, juce::MidiBuffer& midi)
{
    factor = juce::jlimit (1, 4, factor);
    const auto valid = juce::jmin (numSamples, liveInputSamples);
    const auto gain = juce::Decibels::decibelsToGain (getParam (inGainRef));
    const auto rate = (float) juce::jmax (1.0, baseSampleRate);
    const auto attack = std::exp (-1.0f / (getParam (inAttackRef) * 0.001f * rate));
    const auto release = std::exp (-1.0f / (getParam (inReleaseRef) * 0.001f * rate));
    const auto trigger = (int) getParam (inTriggerRef);
    const auto threshold = juce::Decibels::decibelsToGain (getParam (inThresholdRef));
    const auto note = juce::jlimit (0, 127, (int) getParam (inNoteRef));
    const auto* left = liveDry.getReadPointer (0);
    const auto* right = liveDry.getReadPointer (1);
    const auto historyLength = liveHistory.buffer.getNumSamples();
    auto* historyL = historyLength > 0 ? liveHistory.buffer.getWritePointer (0) : nullptr;
    auto* historyR = historyLength > 0 ? liveHistory.buffer.getWritePointer (1) : nullptr;
    auto peak = 0.0f;
    auto crossing = -1, falling = -1;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto l = i < valid ? left[i] * gain : 0.0f;
        const auto r = i < valid ? right[i] * gain : 0.0f;
        const auto x = 0.5f * (l + r);
        const auto magnitude = std::abs (x);
        liveEnvState = magnitude > liveEnvState ? attack * liveEnvState + (1.0f - attack) * magnitude
                                                : release * liveEnvState + (1.0f - release) * magnitude;
        // A full-scale sine reads about 0.64 before the scaling.
        const auto envelope = juce::jmin (1.0f, liveEnvState * 1.5f);
        peak = juce::jmax (peak, magnitude);

        if (trigger == 1)
        {
            if (! liveGateOpen && crossing < 0 && envelope > threshold)
                crossing = i;
            else if (liveGateOpen && falling < 0 && envelope < threshold * 0.5f)
                falling = i;
        }

        for (int step = 0; step < factor; ++step)
        {
            const auto t = (float) (step + 1) / (float) factor;
            liveVoice[(size_t) (i * factor + step)] = liveLast + (x - liveLast) * t;
            liveEnvVoice[(size_t) (i * factor + step)] = envelope;
        }
        liveLast = x;

        if (historyL != nullptr)
        {
            historyL[liveHistoryWrite] = l;
            historyR[liveHistoryWrite] = r;
            liveHistoryWrite = (liveHistoryWrite + 1) % historyLength;
        }
    }

    inputLevelDisplay.store (juce::jmax (peak, inputLevelDisplay.load() * 0.9f));
    inputEnvDisplay.store (juce::jmin (1.0f, liveEnvState * 1.5f));

    // GATE plays IN NOTE while the input is over the threshold (it lets go
    // 6 dB under it); DRONE holds it down; either way it lets go when turned off.
    const auto noteOff = [this, &midi] (int position)
    {
        if (liveGateOpen && liveGateNote >= 0)
            midi.addEvent (juce::MidiMessage::noteOff (1, liveGateNote), position);
        liveGateOpen = false;
    };
    if (trigger == 1)
    {
        if (liveGateOpen && (falling >= 0 || note != liveGateNote))
            noteOff (juce::jmax (0, falling));
        if (! liveGateOpen && crossing >= 0)
        {
            const auto velocity = (juce::uint8) juce::jlimit (30, 127, 30 + (int) (97.0f * juce::jmin (1.0f, peak * 1.5f)));
            midi.addEvent (juce::MidiMessage::noteOn (1, note, velocity), crossing);
            liveGateOpen = true;
            liveGateNote = note;
        }
    }
    else if (trigger == 2)
    {
        // A new patch starts the drone again (loading one stops the voices).
        if (liveGateOpen && (note != liveGateNote || liveRetrigger.exchange (false)))
            noteOff (0);
        if (! liveGateOpen)
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
            liveGateOpen = true;
            liveGateNote = note;
        }
    }
    else if (liveGateOpen)
    {
        noteOff (0);
    }
}

void IlanaSynthAudioProcessor::processChunk (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    const auto startTicks = juce::Time::getHighResolutionTicks();
    struct AudioThreadReads
    {
        bool previous = audioThreadParams.reading;
        AudioThreadReads() { audioThreadParams.reading = true; }
        ~AudioThreadReads() { audioThreadParams.reading = previous; }
    } audioThreadReads;

    juce::ScopedNoDenormals noDenormals;
    spectralCache->setSynchronous (isNonRealtime());
    stretchRenderer->setSynchronous (isNonRealtime());

    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto channel = totalNumInputChannels; channel < totalNumOutputChannels; ++channel)
        buffer.clear (channel, 0, buffer.getNumSamples());

    // M7.5: the input goes aside (the engine plays it, DRY adds it back at
    // the end) and the buffer starts silent, as it does for the instrument.
    liveInputSamples = 0;
    if (totalNumInputChannels > 0)
        captureLiveInput (buffer);

    if (patchCut.exchange (false))
        cutPatchTails();

    {
        const juce::SpinLock::ScopedLockType lock (lfoShapeLock);
        activeLfoCustom = lfoCustom;
        activeLfoCurveTables = lfoCurveTables;
    }

    if (const auto epoch = remapEpoch.load(); epoch != activeRemapEpoch)
    {
        const juce::SpinLock::ScopedLockType lock (remapLock);
        activeModRemapTables = modRemapTables;
        for (size_t i = 0; i < activeModRemapOn.size(); ++i)
            activeModRemapOn[i] = modRemapOn[i].load();
        activeRemapEpoch = epoch;
    }

    {
        const auto scope = previewFifo.read (previewFifo.getNumReady());

        const auto addPreview = [&midiMessages, this] (int start, int size)
        {
            for (int i = start; i < start + size; ++i)
            {
                const auto& event = previewEvents[(size_t) i];
                midiMessages.addEvent (event.isOn ? juce::MidiMessage::noteOn (1, event.note, event.velocity)
                                                  : juce::MidiMessage::noteOff (1, event.note),
                                       0);
            }
        };

        addPreview (scope.startIndex1, scope.blockSize1);
        addPreview (scope.startIndex2, scope.blockSize2);
    }

    {
        if (wantedOversamplingFactor() != oversamplingFactor.load() || BuiltInTables::anyWanted())
            triggerAsyncUpdate();
    }

    if (isEffectBuild)
        prepareLiveInput (buffer.getNumSamples(), oversamplingFactor.load(), midiMessages);

    // Read every mod slot once; switched-on slots are packed for the voices
    // and evaluated synth-wide for the effects and the knob rings.
    Mod::Slot activeSlots[Mod::maxSlots];
    auto numActiveSlots = 0;

    for (int i = 0; i < Mod::maxSlots; ++i)
    {
        auto slot = readModSlot (i);
        slot.remap = activeModRemapOn[(size_t) i] ? activeModRemapTables[(size_t) i].data() : nullptr;

        // A spectral amount fed by a retriggered LFO is worked out per voice, like an envelope.
        if (Mod::spectralOscFor (slot.destination) >= 0)
            for (const auto source : { slot.source, slot.aux })
            {
                const auto lfo = juce::jmax (Mod::lfoIndexFor (source), Mod::lfoBIndexFor (source));

                if (lfo >= 0 && lfo < numLfos)
                    slot.perVoiceLfo = slot.perVoiceLfo || getParam (lfoIds[(size_t) lfo].retrig) > 0.5f
                                       || getParam (lfoIds[(size_t) lfo].key) > 0.5f;
            }

        if (slot.isActive())
            activeSlots[numActiveSlots++] = slot;
    }

    lfoRouted.fill (false);
    lfoRoutedB.fill (false);
    for (int i = 0; i < numActiveSlots; ++i)
        for (const auto source : { activeSlots[i].source, activeSlots[i].aux })
        {
            if (const auto lfo = Mod::lfoIndexFor (source); lfo >= 0)
                lfoRouted[(size_t) lfo] = true;
            if (const auto lfo = Mod::lfoBIndexFor (source); lfo >= 0)
                lfoRoutedB[(size_t) lfo] = true;
        }

    // M8.3: the WEST gate's strike source is read by the voices directly.
    if (getParam ("west_on") > 0.5f)
    {
        const auto westSource = (Mod::Source) (int) getParam ("west_src");
        if (const auto lfo = Mod::lfoIndexFor (westSource); lfo >= 0)
            lfoRouted[(size_t) lfo] = true;
        if (const auto lfo = Mod::lfoBIndexFor (westSource); lfo >= 0)
            lfoRoutedB[(size_t) lfo] = true;
    }

    const auto factor = oversamplingFactor.load();

    if (factor > 1)
    {
        const auto savedRate = currentSampleRate;
        currentSampleRate = baseSampleRate * (double) factor;
        renderLfos (buffer.getNumSamples() * factor, midiMessages);
        currentSampleRate = savedRate;
    }
    else
    {
        renderLfos (buffer.getNumSamples(), midiMessages);
    }

    for (const auto metadata : midiMessages)
    {
        if (metadata.getMessage().isNoteOn())
        {
            displayFrequency.store ((float) juce::MidiMessage::getMidiNoteInHertz (metadata.getMessage().getNoteNumber()));
            displayPhase.store (0.0f);
            ++noteOnCount;
        }
    }

    if (displayFrequency.load() > 0.0f)
    {
        auto phase = displayPhase.load() + displayFrequency.load() / (float) currentSampleRate * (float) buffer.getNumSamples();

        while (phase >= 1.0f)
            phase -= 1.0f;

        displayPhase.store (phase);
    }

    for (int lfo = 0; lfo < numLfos; ++lfo)
        lfoPhaseDisplays[(size_t) lfo].store ((float) lfoPhases[(size_t) lfo]);

    updateEvolveAndVector (buffer.getNumSamples());
    evaluateGlobalModulation (activeSlots, numActiveSlots);

    // Built in place: idle voices take it when a note starts on them (Voice::startNote)
    // instead of every voice copying the whole struct every block.
    auto& p = voiceParamsShared;
    p = VoiceParams {};

    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const auto& ids = oscCoreIds[(size_t) osc];
        auto& settings = p.oscillators[(size_t) osc];
        const auto choice = (int) getParam (ids.table);
        p.oscillatorEnabled[(size_t) osc] = getParam (ids.on) > 0.5f;
        settings.frameBlend = juce::jlimit (0, FrameBlend::count - 1, (int) getParam (ids.frameBlend));
        settings.table = p.oscillatorEnabled[(size_t) osc]
                             ? spectralCache->get (osc, choice, getTableForChoice (choice),
                                                   (int) getParam (ids.spectral),
                                                   getParam (ids.spectralAmount),
                                                   settings.frameBlend == FrameBlend::spectral,
                                                   (double) buffer.getNumSamples() / currentSampleRate)
                             : getTableForChoice (choice);
        settings.spectralMode = (int) getParam (ids.spectral);
        settings.spectralTable = choice;
        settings.spectralAmount = getParam (ids.spectralAmount);
        settings.baseTable = getTableForChoice (choice);
        settings.frame = getParam (ids.frame);
        settings.level = getParam (ids.level) * vectorGains[(size_t) osc];
        settings.pan = getParam (ids.pan);
        settings.semitones = (double) getParam (ids.semi);
        settings.cents = (double) getParam (ids.fine);
        settings.unison = (int) getParam (ids.unison);
        settings.detuneCents = getParam (ids.detune);
        settings.spread = getParam (ids.spread);
        settings.chord = (int) getParam (ids.chord);
    }

    p.subOctaveOffset = 0;

    // Dedicated sub: the Sine, PWM (square) or Analog (saw) table at frame 0.
    {
        const auto shape = (int) getParam ("sub_shape");
        p.subOscEnabled = getParam ("subosc_on") > 0.5f;
        p.subOscLevel = getParam ("subosc_level");
        p.subOscOctave = (int) getParam ("sub_octave") == 0 ? -12 : -24;
        p.subOscRoute = (int) getParam ("subosc_route");
        p.subOscTable = getTableForChoice (shape == 0 ? 8 : (shape == 1 ? 6 : 10));
    }

    p.noiseLevel = getParam ("noise_level");
    p.noiseEnabled = getParam ("noise_on") > 0.5f;
    p.noiseType = juce::jlimit (0, NoiseColours::count - 1, (int) getParam ("noise_type"));
    p.subOscColour = getParam ("sub_colour");
    p.noiseColour = getParam ("noise_color");

    p.filter1.type = juce::jlimit (0, FilterType::Count - 1, (int) getParam ("f1_type"));
    p.filter1.slope24 = getParam ("f1_slope") > 0.5f;
    p.filter1.cutoffHz = getParam ("f1_cutoff");
    p.filter1.resonance = getParam ("f1_reso");
    p.filter1.drive = getParam ("f1_drive");
    p.filter1.envAmount = getParam ("f1_env");
    p.filter1.keyTrack = getParam ("f1_keytrack");
    p.filter1.morph = getParam ("f1_morph");

    p.filter2.type = juce::jlimit (0, FilterType::Count - 1, (int) getParam ("f2_type"));
    p.filter2.slope24 = getParam ("f2_slope") > 0.5f;
    p.filter2.cutoffHz = getParam ("f2_cutoff");
    p.filter2.resonance = getParam ("f2_reso");
    p.filter2.drive = getParam ("f2_drive");
    p.filter2.envAmount = getParam ("f2_env");
    p.filter2.keyTrack = getParam ("f2_keytrack");
    p.filter2.morph = getParam ("f2_morph");

    p.filtersParallel = getParam ("filters_parallel") > 0.5f;
    p.filterBalance = getParam ("filter_balance");
    p.routing = currentRoutingTable();

    p.ampEnv = { getParam ("amp_attack"), getParam ("amp_decay"), getParam ("amp_sustain"),
                 getParam ("amp_release"), getParam ("amp_curve") };
    p.filterEnv = { getParam ("fe_attack"), getParam ("fe_decay"), getParam ("fe_sustain"),
                    getParam ("fe_release"), getParam ("fe_curve") };
    p.filter2Env = { getParam ("f2e_attack"), getParam ("f2e_decay"), getParam ("f2e_sustain"),
                     getParam ("f2e_release"), getParam ("f2e_curve") };
    p.modEnv = { getParam ("me_attack"), getParam ("me_decay"), getParam ("me_sustain"),
                 getParam ("me_release"), getParam ("me_curve") };
    p.env4 = { getParam ("e4_attack"), getParam ("e4_decay"), getParam ("e4_sustain"),
               getParam ("e4_release"), getParam ("e4_curve") };
    for (int env = 6; env <= 16; ++env)
    {
        const auto& ids = extraEnvIds[(size_t) (env - 6)];
        p.extraEnvs[(size_t) (env - 6)] = { getParam (ids.attack), getParam (ids.decay), getParam (ids.sustain),
                                            getParam (ids.release), getParam (ids.curve) };
        p.extraEnvVelocity[(size_t) (env - 6)] = getParam (ids.velocity);
    }
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
        p.oscillators[(size_t) osc].ampEnv = juce::jlimit (0, OperatorEg::envelopeChoice, (int) getParam (oscAmpEnvIds[(size_t) osc]));

    // The Operator EG's settings as the DX7 voice its engine reads, only
    // while an enabled oscillator plays it.
    p.operatorEgUsed = false;
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
        p.operatorEgUsed = p.operatorEgUsed
                           || (p.oscillatorEnabled[(size_t) osc] && p.oscillators[(size_t) osc].ampEnv == OperatorEg::envelopeChoice);
    if (p.operatorEgUsed)
    {
        auto& voice = p.operatorEg;
        for (int osc = 0; osc < OscillatorIds::count; ++osc)
            for (size_t field = 0; field < OperatorEg::operatorFields().size(); ++field)
            {
                const auto& info = OperatorEg::operatorFields()[field];
                // Operator k (1-6) lives at (6 - k) * 21 in the voice.
                voice[(size_t) ((5 - osc) * 21 + info.offset)]
                    = (std::uint8_t) juce::jlimit (0, info.maximum, juce::roundToInt (getParam (operatorEgIds[(size_t) osc][field])));
            }
        for (size_t field = 0; field < OperatorEg::voiceFields().size(); ++field)
        {
            const auto& info = OperatorEg::voiceFields()[field];
            voice[(size_t) info.offset] = (std::uint8_t) juce::jlimit (0, info.maximum, juce::roundToInt (getParam (operatorEgVoiceIds[field])));
        }
        voice[(size_t) OperatorEg::keyOffsetByte]
            = (std::uint8_t) (24 + juce::jlimit (-24, 24, juce::roundToInt (getParam (operatorEgKeyOffsetId))));
    }
    p.quality = juce::jlimit (0, 2, (int) getParam ("quality"));

    p.ampVelocity = getParam ("amp_velocity");
    p.filterVelocity = getParam ("filter_velocity");
    p.filter2EnvVelocity = getParam ("f2e_velocity");
    p.modEnvVelocity = getParam ("me_velocity");
    p.env4Velocity = getParam ("e4_velocity");
    p.glideTime = getParam ("glide");
    p.pitchBendRange = getParam ("bend_range");

    p.fmAmount = getParam ("fm_amount");
    p.fmFeedback = getParam ("fm_feedback");
    p.ringMod = getParam ("ring_mod");
    p.hardSync = getParam ("hard_sync") > 0.5f;
    p.drift = getParam ("drift");
    p.tuneLoose = getParam ("tune_loose");
    p.tuneLooseBoth = getParam ("tune_loose_both") > 0.5f;

    // FM matrix [source][target]; OSC 2 > 1 and OSC 1 feedback are the
    // original FM Amount and FM Feedback.
    p.fmMatrix[1][0] = p.fmAmount;
    p.fmMatrix[0][0] = p.fmFeedback;
    p.fmMatrix[0][1] = getParam ("fm_1to2");
    p.fmMatrix[0][2] = getParam ("fm_1to3");
    p.fmMatrix[1][2] = getParam ("fm_2to3");
    p.fmMatrix[2][0] = getParam ("fm_3to1");
    p.fmMatrix[2][1] = getParam ("fm_3to2");
    p.fmMatrix[1][1] = getParam ("fm_fb2");
    p.fmMatrix[2][2] = getParam ("fm_fb3");
    for (int source = 0; source < OscillatorIds::count; ++source)
        for (int target = 0; target < OscillatorIds::count; ++target)
        {
            if (source < 3 && target < 3)
                continue;

            p.fmMatrix[source][target] = getParam (fmMatrixIds[(size_t) source][(size_t) target]);
        }
    p.fmMode = (int) getParam ("fm_mode");
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
        p.oscOut[(size_t) osc] = getParam (oscCoreIds[(size_t) osc].out) > 0.5f;

    const auto fillStringParams = [this] (int oscIndex, VoiceParams::OscParams& osc)
    {
        const auto& ids = stringParamIds[(size_t) oscIndex];
        const auto mode = (int) getParam (ids[0]);
        osc.stringMode = mode == 1;
        osc.liveMode = mode == 4;
        osc.sampleMode = mode == 2 || mode == 3; // granular reads the sample too
        osc.granularMode = mode == 3;
        osc.stringExcite = (int) getParam (ids[1]);
        osc.stringDecay = getParam (ids[2]);
        osc.stringDamping = getParam (ids[3]);
        osc.stringSustain = getParam (ids[4]);
        osc.stringStiffness = getParam (ids[5]);
        osc.stringPickup = getParam (ids[6]);
        osc.stringExcitationPosition = getParam (ids[7]);
        osc.stringPickHardness = getParam (ids[8]);
        osc.stringPickPosition = getParam (ids[9]);
        osc.stringSlap = getParam (ids[10]) > 0.5f;
        const auto& extras = bowBuzzIds[(size_t) oscIndex];
        osc.bowPressure = getParam (extras[0]);
        osc.bowSpeed = getParam (extras[1]);
        osc.bridgeBuzz = getParam (extras[2]);
        osc.fretRattle = getParam (extras[3]);
        osc.stringSplit = getParam (extras[4]);
        osc.stringClick = getParam (extras[5]);
        const auto& keys = keysParamIds[(size_t) oscIndex];
        osc.hammerHardness = getParam (keys[0]);
        osc.couple = getParam (keys[1]);
        osc.damper = getParam (keys[2]);
        osc.registerMap = getParam (keys[3]);
        const auto& electric = electricParamIds[(size_t) oscIndex];
        osc.epDistance = getParam (electric[0]);
        osc.epPosition = getParam (electric[1]);
        osc.stringStereo = stringStereoRouted[(size_t) oscIndex];
        osc.stringStereoOffsets = stringStereoOffsets[(size_t) oscIndex];
        if (osc.stringExcite == 10)
        {
            osc.fbGain = getParam (feedbackParamIds[(size_t) oscIndex][0]);
            osc.fbDistance = getParam (feedbackParamIds[(size_t) oscIndex][1]);
        }
    };

    const auto fillSampleParams = [this] (int oscIndex, VoiceParams::OscParams& osc)
    {
        const auto& ids = sampleParamIds[(size_t) oscIndex];
        osc.sample = getSampleForOsc (oscIndex);
        osc.sampleTuned = getParam (ids[0]) > 0.5f;
        osc.sampleLoop = getParam (ids[1]) > 0.5f;
        osc.sampleReverse = getParam (ids[2]) > 0.5f;
        osc.sampleStart = getParam (ids[3]);
        osc.sampleEnd = getParam (ids[4]);
        osc.sampleFadeIn = getParam (ids[5]);
        osc.sampleFadeOut = getParam (ids[6]);
        const auto& loopIds = sampleLoopIds[(size_t) oscIndex];
        osc.sampleRoot = getParam (sampleRootIds[(size_t) oscIndex]);
        osc.sampleLoopOwn = getParam (loopIds[0]) > 0.5f;
        osc.sampleLoopStart = getParam (loopIds[1]);
        osc.sampleLoopEnd = getParam (loopIds[2]);
        osc.sampleLoopXfade = getParam (loopIds[3]);
        const auto& play = samplePlayIds[(size_t) oscIndex];
        osc.samplePlayback = juce::roundToInt (getParam (play[0]));
        osc.samplePingPong = getParam (play[1]) > 0.5f;
        osc.sampleStartRandom = getParam (play[2]);
        osc.sampleStartSafe = getParam (play[3]) > 0.5f;
        osc.sampleQuality = juce::roundToInt (getParam (play[4]));
        osc.sampleSilenceOutside = getParam (play[5]) > 0.5f;
        osc.sampleRoundRobin = juce::roundToInt (getParam (play[6]));
        osc.sampleVelBlend = getParam (play[7]);
        const auto& tempo = sampleTempoIds[(size_t) oscIndex];
        osc.sampleSync = juce::roundToInt (getParam (tempo[0]));
        osc.sampleBpm = getParam (tempo[1]);
        const auto time = juce::roundToInt (getParam (tempo[2]));
        osc.sampleStretch = time >= 1;
        osc.sampleEngine = time >= 2 ? juce::jmin (3, time - 2) : -1;

        const auto& grain = grainParamIds[(size_t) oscIndex];
        osc.grainSizeMs = getParam (grain[0]);
        osc.grainDensity = getParam (grain[1]);
        osc.grainSpray = getParam (grain[2]);
        osc.grainPitch = getParam (grain[3]);
        osc.grainSpread = getParam (grain[4]);
        osc.grainPosition = getParam (grain[5]);
        osc.grainScan = getParam (grain[6]);
        osc.grainFreeze = getParam (grain[7]) > 0.5f;
    };

    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        fillStringParams (osc, p.oscillators[(size_t) osc]);
        fillSampleParams (osc, p.oscillators[(size_t) osc]);
    }

    p.voiceSpread = getParam ("voice_spread");
    p.stretch = getParam ("stretch");
    p.hostBpm = juce::jlimit (20.0, 999.0, currentBpm.load());
    p.unisonRandom = getParam ("unison_random");
    p.filter1Fm = getParam ("f1_fm");
    p.filter2Fm = getParam ("f2_fm");

    p.resonatorOn = getParam ("res_on") > 0.5f;
    p.resonatorAmount = getParam ("res_amount");
    p.resonatorDecay = getParam ("res_decay");
    p.resonatorOffset = getParam ("res_offset");
    p.resonatorKeytrack = getParam ("res_keytrack");
    p.bodyType = (int) getParam ("body_type");
    p.bodyMaterial = getParam ("body_material");
    p.bodySize = getParam ("body_size");
    p.west.on = getParam ("west_on") > 0.5f;
    if (p.west.on)
    {
        p.west.position = (int) getParam ("west_pos");
        p.west.fold = getParam ("west_fold");
        p.west.symmetry = getParam ("west_sym");
        p.west.stages = (int) getParam ("west_stages");
        p.west.mode = (int) getParam ("west_mode");
        p.west.decay = getParam ("west_decay");
        p.west.resonance = getParam ("west_res");
        p.west.strike = getParam ("west_strike");
        p.west.open = getParam ("west_open");
        p.west.source = (int) getParam ("west_src"); // 0 strike, else the Mod::Source index
    }
    p.bodyCouplingMode = (int) getParam ("body_coupling_mode");
    p.bodyCoupling = getParam ("body_coupling");

    if (getParam ("mpe_mode") > 0.5f)
    {
        p.mpe = true;
        p.masterBendRange = p.pitchBendRange; // the BEND knob is the master channel's range
        p.pitchBendRange = 48.0f;
    }

    p.clockSh = lfoBuffers.getReadPointer (4);
    p.mseg = lfoBuffers.getReadPointer (5);

    p.macros[0] = macroValue (0);
    p.macros[1] = macroValue (1);
    p.macros[2] = macroValue (2);
    p.macros[3] = macroValue (3);
    for (int m = 4; m < Mod::numMacros; ++m)
        p.macros[m] = macroValue (m);
    p.vectorX = vectorX.load();
    p.vectorY = vectorY.load();

    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        p.lfoBuffers[lfo] = lfoBuffers.getReadPointer (lfoChannel (lfo));
        p.lfoBuffersB[lfo] = lfoBuffers.getReadPointer (lfoChannelB (lfo));
    }

    p.numModSlots = numActiveSlots;
    p.numActiveDestinations = 0;
    p.anyExtendedFmMods = false;
    p.anySpectralMods = false;
    p.spectralCache = spectralCache.get();
    p.stretchRenderer = stretchRenderer.get();
    p.roundRobin = &sampleRoundRobin;

    for (int i = 0; i < numActiveSlots; ++i)
    {
        p.modSlots[i] = activeSlots[i];
        const auto destination = activeSlots[i].destination;

        if (! Mod::isExplicitDestination (destination))
        {
            p.anySpectralMods = p.anySpectralMods || Mod::isVoiceSpectralSlot (activeSlots[i]);
            p.anyExtendedFmMods = p.anyExtendedFmMods || Mod::extendedFmCellFor (destination) >= 0;
            continue;
        }

        auto seen = false;

        for (int d = 0; d < p.numActiveDestinations && ! seen; ++d)
            seen = p.activeDestinations[d] == destination;

        if (! seen)
            p.activeDestinations[p.numActiveDestinations++] = destination;
    }

    for (int osc = 0; osc < OscillatorIds::count; ++osc)
        if (p.oscillatorEnabled[(size_t) osc] && p.oscillators[(size_t) osc].ampEnv >= 5)
            p.extraEnvNeeded[(size_t) (p.oscillators[(size_t) osc].ampEnv - 5)] = true;
    for (int i = 0; i < numActiveSlots; ++i)
        for (const auto source : { activeSlots[i].source, activeSlots[i].aux })
            if (source >= Mod::Source::Env6 && source <= Mod::Source::Env16)
                p.extraEnvNeeded[(size_t) ((int) source - (int) Mod::Source::Env6)] = true;

    // LFOs: retriggered ones run per voice (each note gets its own phase);
    // free-running ones are shared through the buffers rendered above.
    {
        const auto voiceRate = baseSampleRate * (double) oversamplingFactor.load();

        for (int lfo = 0; lfo < numLfos; ++lfo)
        {
            const auto& ids = lfoIds[(size_t) lfo];
            auto& lfoParams = p.lfos[lfo];

            auto rate = (double) getParam (ids.rate);

            if (getParam (ids.sync) > 0.5f)
                rate = (currentBpm.load() / 60.0) / getSyncDivisionBeats ((int) getParam (ids.div));

            for (int step = 0; step < 16; ++step)
                lfoStepValues[lfo][step] = getParam (ids.steps[(size_t) step]);

            lfoParams.keyTrack = getParam (ids.key) > 0.5f;
            lfoParams.perVoice = getParam (ids.retrig) > 0.5f || lfoParams.keyTrack;
            lfoParams.shape = (int) getParam (ids.shape);
            lfoParams.baseIncrement = juce::jlimit (0.001, 200.0, rate) / voiceRate;
            lfoParams.startPhase = getParam (ids.phase);
            lfoParams.physA = getParam (ids.physA);
            lfoParams.physB = getParam (ids.physB);
            lfoParams.kick = getParam (ids.kick) > 0.5f;
            lfoParams.steps = lfoStepValues[lfo];
            const auto isCurve = lfoParams.shape == curveShape;
            lfoParams.custom = isCurve ? activeLfoCurveTables[(size_t) lfo].data() : activeLfoCustom[(size_t) lfo].data();
            lfoParams.customSize = isCurve ? LfoCurve::tableSize : lfoDrawSteps;
            lfoParams.sim = readLfoSimSettings (lfo);
            lfoParams.smooth = getParam (ids.smooth);
            lfoParams.needsB = lfoRoutedB[(size_t) lfo];
            lfoParams.triggerCount = lfoTriggerCounts[(size_t) lfo];
        }
    }

    const auto fillWarpAndUnison = [this] (int oscIndex, VoiceParams::OscParams& osc)
    {
        const auto& ids = oscShapeIds[(size_t) oscIndex];
        osc.warpMode = (int) getParam (ids.warp);
        osc.warpAmount = getParam (ids.warpAmount);
        osc.unisonMode = (int) getParam (ids.unisonMode);
        osc.unisonBlend = getParam (ids.unisonBlend);
        osc.frameSpread = getParam (ids.frameSpread);
        osc.warpSpread = getParam (ids.warpSpread);
        osc.scale = (int) getParam (ids.scale);
        osc.scaleRoot = (int) getParam (ids.scaleRoot);
        osc.route = (int) getParam (ids.route);
    };

    for (int osc = 0; osc < OscillatorIds::count; ++osc)
        fillWarpAndUnison (osc, p.oscillators[(size_t) osc]);

    // M5 operators and M6 phase distortion.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const auto& ids = operatorIds[(size_t) osc];
        auto& settings = p.oscillators[(size_t) osc];
        settings.tuneMode = juce::jlimit (0, OscTuning::Count - 1, (int) getParam (ids.tune));
        settings.ratio = OscTuning::snapRatio ((double) getParam (ids.ratio), (int) getParam (ids.snap));
        settings.fixedHz = (double) getParam (ids.fixedHz);
        settings.keyLevel = getParam (ids.keyLevel);
        settings.feedbackType = juce::jlimit (0, FmFeedback::Count - 1, (int) getParam (ids.feedbackType));
        settings.warpMode2 = Warp::modeForStageTwoChoice ((int) getParam (ids.warp2));
        settings.warpAmount2 = getParam (ids.warp2Amount);
        settings.pdEnv = juce::jlimit (0, 17, (int) getParam (ids.pdEnv));
        settings.pdEnvAmount = getParam (ids.pdEnvAmount);
        p.fmNoise[osc] = getParam (fmNoiseIds[(size_t) osc]);
    }
    p.fmNoiseColour = getParam ("fm_noise_color");
    p.fmNoiseType = juce::jlimit (0, (int) NoiseColours::count, (int) getParam ("fm_noise_type"));

    for (int env = 0; env < 16; ++env)
    {
        const auto& ids = envelopeExtraIds[(size_t) env];
        const auto delay = getParam (ids.delay);
        const auto hold = getParam (ids.hold);
        auto& target = env == 0 ? p.ampEnv : env == 1 ? p.filterEnv : env == 2 ? p.filter2Env
                     : env == 3 ? p.modEnv : env == 4 ? p.env4 : p.extraEnvs[(size_t) (env - 5)];
        target.delay = delay;
        target.hold = hold;
        target.attackCurve = getParam (ids.attackCurve);
        target.decayCurve = getParam (ids.decayCurve);
        target.releaseCurve = getParam (ids.releaseCurve);
        p.envKeyRate[(size_t) env] = getParam (ids.keyRate);
    }

    // Envelopes an oscillator uses for its warp (ENV 6-16 only run when
    // needed), and the MSEG as a per-voice envelope.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        if (! p.oscillatorEnabled[(size_t) osc])
            continue;

        const auto& settings = p.oscillators[(size_t) osc];
        if (settings.pdEnv >= 6 && settings.pdEnv <= 16)
            p.extraEnvNeeded[(size_t) (settings.pdEnv - 6)] = true;
        p.msegEnvNeeded = p.msegEnvNeeded || settings.pdEnv == 17 || settings.ampEnv == 16;
    }

    if (p.msegEnvNeeded)
    {
        for (int point = 0; point < Mseg::numPoints; ++point)
        {
            p.msegShape.levels[point] = getParam (msegLevelIds[(size_t) point]);
            p.msegShape.times[point] = getParam (msegTimeIds[(size_t) point]);
        }

        p.msegShape.rateHz = (double) getParam ("mseg_rate");
        p.msegShape.loop = getParam ("mseg_loop") > 0.5f;
    }

    synth.setVoiceMode ((IlanaSynth::Mode) juce::jlimit (0, 2, (int) getParam ("voice_mode")),
                        (int) getParam ("poly_voices"), getParam ("glide_legato") > 0.5f);

    // Generative stage (scale snap, note spray) feeds the arpeggiator.
    {
        NoteSpray::Settings spray;
        spray.scale = (int) getParam ("gen_scale");
        spray.root = (int) getParam ("gen_root");
        spray.snapInput = getParam ("gen_snap") > 0.5f;
        spray.sprayOn = getParam ("spray_on") > 0.5f;
        spray.count = (int) getParam ("spray_count");
        spray.range = (int) getParam ("spray_range");
        spray.direction = (int) getParam ("spray_direction");
        spray.spreadSamples = (int) (getParam ("spray_spread") * 0.001 * currentSampleRate);
        spray.chance = getParam ("spray_chance");
        spray.velocityRandom = getParam ("spray_velocity");
        spray.strum = juce::jlimit (0, 2, (int) getParam ("spray_strum"));
        spray.strumSamples = (int) (getParam ("spray_strum_time") * 0.001 * currentSampleRate);
        noteSpray.process (midiMessages, generatedMidi, buffer.getNumSamples(), spray);
    }

    processArpeggiator (generatedMidi, buffer.getNumSamples(), midiForSynth);
    addEuclidExciterHits (midiForSynth, buffer.getNumSamples());
    processClip (midiForSynth, buffer.getNumSamples());

    for (const auto metadata : midiForSynth)
    {
        const auto message = metadata.getMessage();
        if (message.isNoteOn())
        {
            noteSerial.fetch_add (1);
            uiNoteVelocity[(size_t) message.getNoteNumber()].store ((juce::uint8) juce::jmax (1, (int) message.getVelocity()), std::memory_order_relaxed);
            uiNoteEpoch.fetch_add (1, std::memory_order_relaxed);
        }
        else if (message.isNoteOff())
        {
            uiNoteVelocity[(size_t) message.getNoteNumber()].store (0, std::memory_order_relaxed);
            uiNoteEpoch.fetch_add (1, std::memory_order_relaxed);
        }
        else if (message.isAllNotesOff() || message.isAllSoundOff())
        {
            for (auto& velocity : uiNoteVelocity)
                velocity.store (0, std::memory_order_relaxed);
            uiNoteEpoch.fetch_add (1, std::memory_order_relaxed);
        }
    }

    trackHeldKeys (midiForSynth);
    playResumedKeys (midiForSynth, buffer.getNumSamples());

    p.exciterLevels = exciterLevels.load();

    // M7.5: the live input and its envelope for the voices; live grains
    // read the input's history instead of the sample.
    if (isEffectBuild)
    {
        p.liveInput = liveVoice.data();
        p.inputEnv = liveEnvVoice.data();
        p.inputToBody = getParam (inBodyRef);
        p.inputToStrings = getParam (inStringsRef);
        for (int osc = 0; osc < OscillatorIds::count; ++osc)
        {
            auto& settings = p.oscillators[(size_t) osc];
            if (settings.granularMode && getParam (grainLiveIds[(size_t) osc]) > 0.5f)
            {
                settings.grainLive = true;
                settings.sample = &liveHistory;
                settings.liveWrite = liveHistoryWrite;
            }
        }
    }

    // Scala tuning: nullptr (off, or nothing loaded) keeps the 12-TET path.
    p.tuning = getParam (tuningOnRef) > 0.5f ? tuningState.acquireForAudio() : nullptr;
    // An MTS-ESP master in the session takes over (it retunes every client
    // live; new notes pick up its current table).
    if (mtsEsp.hasMaster())
    {
        for (int note = 0; note < 128; ++note)
            mtsTuning.setNote (note, mtsEsp.noteToFrequency (note), ! mtsEsp.shouldFilterNote (note));
        p.tuning = &mtsTuning;
    }
    synth.setTuning (p.tuning);
    synth.setVoiceThreadsEnabled (forceVoiceThreads >= 0 ? forceVoiceThreads > 0 : getParam (multiCoreRef) > 0.5f);
    {
        constexpr int caps[] { 0, 4, 6, 8, 12, 16 };
        synth.setSustainVoiceCap (caps[juce::jlimit (0, 5, (int) getParam (sustainVoicesRef))]);
    }

    for (int i = 0; i < synth.getNumVoices(); ++i)
        if (auto* voice = dynamic_cast<Voice*> (synth.getVoice (i)))
        {
            voice->setParamsSource (&voiceParamsShared);

            if (voice->isVoiceActive())
                voice->setParams (p);
        }

    if (factor > 1)
    {
        auto& oversampler = activeOversampler();
        juce::dsp::AudioBlock<float> baseBlock (buffer);
        auto upBlock = oversampler.processSamplesUp (baseBlock);
        float* upChannels[2] { upBlock.getChannelPointer (0),
                               upBlock.getNumChannels() > 1 ? upBlock.getChannelPointer (1)
                                                            : upBlock.getChannelPointer (0) };
        juce::AudioBuffer<float> upBuffer (upChannels, (int) upBlock.getNumChannels(), (int) upBlock.getNumSamples());

        scaledMidiBuffer.clear();

        for (const auto metadata : midiForSynth)
            scaledMidiBuffer.addEvent (metadata.getMessage(), metadata.samplePosition * factor);

        synth.renderNextBlock (upBuffer, scaledMidiBuffer, 0, upBuffer.getNumSamples());
        oversampler.processSamplesDown (baseBlock);
    }
    else
    {
        synth.renderNextBlock (buffer, midiForSynth, 0, buffer.getNumSamples());
    }

    {
        auto bestAmp = 0.0f;
        auto bestActivity = 0.0f;
        std::array<float, 11> extraEnvValues {};
        std::array<float, 16> envPositions;
        envPositions.fill (-1.0f);
        std::array<float, 16> envLevels {};
        std::array<float, 80> envSeconds {};
        Voice* newestVoice = nullptr;
        auto filterValue = 0.0f;
        auto filter2Value = 0.0f;
        auto modValue = 0.0f;
        auto env4Value = 0.0f;
        auto activeVoices = 0;
        std::array<float, OscillatorIds::count> samplePositions;
        samplePositions.fill (-1.0f);
        std::array<float, OscillatorIds::count> phases {};

        auto westLevel = 0.0f;
        for (int i = 0; i < synth.getNumVoices(); ++i)
        {
            if (auto* voice = dynamic_cast<Voice*> (synth.getVoice (i)))
            {
                westLevel = juce::jmax (westLevel, voice->getWestGateLevel());

                // The envelope graphs' playhead follows the newest sounding
                // note, not the loudest voice (a note still ringing out would
                // hold the dot while the new one began).
                if (voice->getLastLifetimeValue() > 0.001f && (newestVoice == nullptr || newestVoice->wasStartedBefore (*voice)))
                    newestVoice = voice;
                const auto amp = voice->getLastAmpValue();
                const auto activity = voice->getLastLifetimeValue();

                if (activity > 0.001f)
                    ++activeVoices;

                if (activity > bestActivity)
                {
                    bestActivity = activity;
                    bestAmp = amp;
                    filterValue = voice->getLastFilterValue();
                    filter2Value = voice->getLastFilter2Value();
                    modValue = voice->getLastModValue();
                    env4Value = voice->getLastEnv4Value();
                    for (int osc = 0; osc < OscillatorIds::count; ++osc)
                    {
                        samplePositions[(size_t) osc] = voice->getLastSamplePosition (osc);
                        phases[(size_t) osc] = voice->getLastWavetablePhase (osc);
                    }
                    for (int env = 0; env < 11; ++env)
                        extraEnvValues[(size_t) env] = voice->getLastExtraEnvValue (env);
                    monitorVelocity.store (voice->getVelocity());
                    monitorKeyTrack.store (voice->getKeyTrack());
                    monitorRandom.store (voice->getRandomValue());
                    monitorOpLfo.store (voice->getOpLfoValue());
                    monitorOpPitch.store (voice->getOpPitchValue());
                    monitorOpEnvSeconds.store (voice->getOpEnvSeconds());
                    monitorOpEnvRelease.store (voice->getOpEnvReleaseSeconds());
                }
            }
        }
        westGateDisplay.store (westLevel);


        activeVoiceCount.store (activeVoices);
        if (activeVoices == 0)
        {
            monitorOpLfo.store (0.0f);
            monitorOpPitch.store (0.0f);
            monitorOpEnvSeconds.store (-1.0f);
            monitorOpEnvRelease.store (-1.0f);
        }
        if (newestVoice != nullptr)
        {
            for (int env = 0; env < 16; ++env)
            {
                envPositions[(size_t) env] = newestVoice->getEnvelopePosition (env);
                const auto seconds = newestVoice->getEnvelopeSeconds (env);
                for (size_t i = 0; i < 5; ++i)
                    envSeconds[(size_t) env * 5 + i] = seconds[i];
                envLevels[(size_t) env] = newestVoice->getEnvelopeStageStart (env);
            }
        }

        envMonitorAmp.store (bestAmp);
        envMonitorFilter.store (filterValue);
        envMonitorFilter2.store (filter2Value);
        envMonitorMod.store (modValue);
        envMonitorEnv4.store (env4Value);
        for (int osc = 0; osc < OscillatorIds::count; ++osc)
        {
            displaySamplePositions[(size_t) osc].store (samplePositions[(size_t) osc]);
            oscDisplayPhases[(size_t) osc].store (phases[(size_t) osc]);
        }
        for (int env = 0; env < 11; ++env)
            envMonitorExtra[(size_t) env].store (extraEnvValues[(size_t) env]);
        for (int env = 0; env < 16; ++env)
        {
            envMonitorPositions[(size_t) env].store (envPositions[(size_t) env]);
            envMonitorStageStarts[(size_t) env].store (envLevels[(size_t) env]);
            for (size_t i = 0; i < 5; ++i)
                envMonitorSeconds[(size_t) env * 5 + i].store (envSeconds[(size_t) env * 5 + i]);
        }
    }

    processAcousticKeys (buffer, midiForSynth);

    const auto symOn = getParam ("sym_on") > 0.5f && getParam ("sym_amount") > 0.0f;
    if (! symOn && sympatheticWasOn)
        sympatheticStrings.reset();   // no stale ringing when switched back on
    sympatheticWasOn = symOn;

    if (symOn)
    {
        const char* const noteIds[] { "sym_note1", "sym_note2", "sym_note3", "sym_note4", "sym_note5", "sym_note6" };
        std::array<int, SympatheticStrings::maxStrings> notes {};
        for (int i = 0; i < SympatheticStrings::maxStrings; ++i)
            notes[(size_t) i] = (int) getParam (noteIds[i]);
        sympatheticStrings.setTuning ((int) getParam ("gen_scale"), (int) getParam ("gen_root"),
                                      getParam ("sym_manual") > 0.5f, notes);
        const auto count = (int) getParam ("sym_count");
        const auto amount = getParam ("sym_amount");
        const auto decay = getParam ("sym_decay");
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto input = right != nullptr ? 0.5f * (left[i] + right[i]) : left[i];
            const auto wet = sympatheticStrings.process (input, count, amount, decay);
            left[i] += wet;
            if (right != nullptr) right[i] += wet;
        }
    }

    // DC blockers (5 Hz one-pole high-passes) before and after the effects:
    // some warps, phase distortion and drives leave an offset (up to 0.4 of
    // full scale) that ate headroom, pushed the clipper and pumped the
    // dynamics, and asymmetric effects add their own.
    const auto blockDc = [this, &buffer] (int stage)
    {
        const auto pole = (float) std::exp (-juce::MathConstants<double>::twoPi * 5.0 / juce::jmax (1.0, baseSampleRate));
       #if defined(_M_X64) || defined(__x86_64__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
        // Both sides at once, one SSE lane each: the same arithmetic as
        // the loop below, bit for bit, with the two one-sample recursions
        // side by side instead of one after the other.
        if (buffer.getNumChannels() >= 2)
        {
            auto* left = buffer.getWritePointer (0);
            auto* right = buffer.getWritePointer (1);
            auto& stateL = dcBlock[stage][0];
            auto& stateR = dcBlock[stage][1];
            auto x1 = _mm_set_ps (0.0f, 0.0f, stateR[0], stateL[0]);
            auto y1 = _mm_set_ps (0.0f, 0.0f, stateR[1], stateL[1]);
            const auto poles = _mm_set1_ps (pole);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const auto x = _mm_set_ps (0.0f, 0.0f, right[i], left[i]);
                y1 = _mm_add_ps (_mm_sub_ps (x, x1), _mm_mul_ps (poles, y1));
                x1 = x;
                left[i] = _mm_cvtss_f32 (y1);
                right[i] = _mm_cvtss_f32 (_mm_shuffle_ps (y1, y1, 1));
            }
            alignas (16) float xs[4], ys[4];
            _mm_store_ps (xs, x1);
            _mm_store_ps (ys, y1);
            for (int channel = 0; channel < 2; ++channel)
            {
                auto& state = dcBlock[stage][(size_t) channel];
                state[0] = std::isfinite (xs[channel]) ? xs[channel] : 0.0f;
                state[1] = std::isfinite (ys[channel]) && std::abs (ys[channel]) > 1.0e-20f ? ys[channel] : 0.0f;
            }
            return;
        }
       #endif
        for (int channel = 0; channel < juce::jmin (2, buffer.getNumChannels()); ++channel)
        {
            auto* data = buffer.getWritePointer (channel);
            auto& state = dcBlock[stage][channel];
            auto x1 = state[0], y1 = state[1];
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const auto x = data[i];
                y1 = x - x1 + pole * y1;
                x1 = x;
                data[i] = y1;
            }
            state[0] = std::isfinite (x1) ? x1 : 0.0f;
            state[1] = std::isfinite (y1) && std::abs (y1) > 1.0e-20f ? y1 : 0.0f;
        }
    };

    blockDc (0);
    // FX-14: a staged slot swap goes in between two blocks, before the rack reads any slot.
    beginFxBlock();
    processEffectsUnlessAsleep (buffer);
    blockDc (1);

    // MASTER plus the preset's own level (output_trim, 0 unless a factory
    // preset set it), summed in dB.
    {
        const auto masterGain = juce::Decibels::decibelsToGain (getParam (masterRef) + getRawParam (outputTrimRef));
        masterRamp.begin (masterGain, buffer.getNumSamples());

        if (masterRamp.moving())
            buffer.applyGainRamp (0, buffer.getNumSamples(), masterRamp.from, masterGain);
        else
            buffer.applyGain (masterGain);
    }

    if (getParam ("master_clip") > 0.5f)
    {
        const auto clipGain = juce::Decibels::decibelsToGain (getParam ("master_clip_gain"));

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* data = buffer.getWritePointer (channel);

            for (int i = 0; i < buffer.getNumSamples(); ++i)
                data[i] = std::tanh (data[i] * clipGain);
        }
    }

    // Last line of defence: a sample that is not finite becomes 0 and a
    // denormal is flushed (one pass; finite audio is unchanged bit for bit). If
    // one got this far, the effects' memory may hold it too, so it starts empty.
    {
        auto bad = false;
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* data = buffer.getWritePointer (channel);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const auto value = data[i];
                if (! std::isfinite (value))
                {
                    bad = true;
                    data[i] = 0.0f;
                }
                else if (value != 0.0f && std::abs (value) < std::numeric_limits<float>::min())
                {
                    data[i] = 0.0f;
                }
            }
        }
        if (bad)
            resetFxEngines();
    }

    // After a patch change: ease from where the old patch's output stopped
    // (a 2 ms decay) instead of stepping from it to the new patch.
    const auto cancelStep = easeStep.exchange (false) && buffer.getNumSamples() > 0;
    for (int channel = 0; channel < juce::jmin (2, buffer.getNumChannels()); ++channel)
    {
        auto* data = buffer.getWritePointer (channel);
        const auto numSamples = buffer.getNumSamples();
        if (cancelStep)
            declick[channel] = lastOutput[channel] - data[0];
        if (declick[channel] != 0.0f)
        {
            const auto decay = std::exp (-1.0f / (0.002f * (float) juce::jmax (1.0, baseSampleRate)));
            for (int i = 0; i < numSamples; ++i)
            {
                data[i] += declick[channel];
                declick[channel] *= decay;
            }
            if (std::abs (declick[channel]) < 1.0e-6f)
                declick[channel] = 0.0f;
        }
        lastOutput[channel] = numSamples > 0 ? data[numSamples - 1] : lastOutput[channel];
    }

    // M7.5 DRY: the untouched input back in, delayed by the oversamplers'
    // latency so it lines up with the wet signal.
    if (liveInputSamples > 0)
        delayLiveDry (juce::jmin (liveInputSamples, buffer.getNumSamples()));

    if (liveInputSamples > 0)
        if (const auto dry = getParam (inDryRef); dry > 0.0f)
            for (int channel = 0; channel < juce::jmin (2, buffer.getNumChannels()); ++channel)
                buffer.addFrom (channel, 0, liveDry, juce::jmin (channel, liveDry.getNumChannels() - 1), 0,
                                juce::jmin (liveInputSamples, buffer.getNumSamples()), dry);

    {
        const juce::SpinLock::ScopedLockType lock (scopeLock);
        auto writePosition = scopeWritePos.load();

        const auto* leftData = buffer.getReadPointer (0);
        const auto* rightData = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : leftData;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            scopeLeft[(size_t) writePosition] = leftData[i];
            scopeRight[(size_t) writePosition] = rightData[i];
            writePosition = (writePosition + 1) % scopeSize;
        }

        scopeWritePos.store (writePosition);
        scopeBlockSamples.store (buffer.getNumSamples());
        scopeBlockMs.store (juce::Time::getMillisecondCounterHiRes());
    }

    // Output peaks for the meter; the editor takes them when it reads.
    for (int channel = 0; channel < juce::jmin (2, buffer.getNumChannels()); ++channel)
    {
        const auto peak = buffer.getMagnitude (channel, 0, buffer.getNumSamples());
        auto& held = outputPeaks[(size_t) channel];

        if (peak > held.load())
            held.store (peak);
    }
    outputLevelDisplay.store (buffer.getMagnitude (0, buffer.getNumSamples()));

    if (activeVoiceCount.load() > 0 || outputLevelDisplay.load() > 1.0e-5f || inputLevelDisplay.load() > 1.0e-5f)
        ++liveEpoch;

    const auto elapsedSeconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - startTicks);
    const auto availableSeconds = (double) buffer.getNumSamples() / juce::jmax (1.0, currentSampleRate);
    const auto usage = (float) juce::jlimit (0.0, 1.0, elapsedSeconds / juce::jmax (1.0e-6, availableSeconds));

    cpuUsage.store (cpuUsage.load() * 0.92f + usage * 0.08f);
}

void IlanaSynthAudioProcessor::copyScopeData (float* left, float* right, int numSamples) const
{
    numSamples = juce::jlimit (0, scopeSize, numSamples);

    const juce::SpinLock::ScopedLockType lock (scopeLock);
    auto start = (scopeWritePos.load() - numSamples + scopeSize) % scopeSize;

    for (int i = 0; i < numSamples; ++i)
    {
        left[i] = scopeLeft[(size_t) start];
        right[i] = scopeRight[(size_t) start];
        start = (start + 1) % scopeSize;
    }
}

void IlanaSynthAudioProcessor::copyScopeDataSmooth (float* left, float* right, int numSamples) const
{
    numSamples = juce::jlimit (0, scopeSize, numSamples);

    const juce::SpinLock::ScopedLockType lock (scopeLock);
    const auto block = scopeBlockSamples.load();
    const auto sinceBlockMs = juce::Time::getMillisecondCounterHiRes() - scopeBlockMs.load();
    const auto advanced = (int) juce::jlimit (0.0, (double) block, sinceBlockMs * 0.001 * currentSampleRate);
    const auto lag = juce::jlimit (0, scopeSize - numSamples, block - advanced);
    auto start = ((scopeWritePos.load() - lag - numSamples) % scopeSize + scopeSize) % scopeSize;

    for (int i = 0; i < numSamples; ++i)
    {
        left[i] = scopeLeft[(size_t) start];
        right[i] = scopeRight[(size_t) start];
        start = (start + 1) % scopeSize;
    }
}

const Wavetable* IlanaSynthAudioProcessor::getTableForChoice (int choiceIndex) const
{
    const auto& factory = FactoryTables::get().tables;
    const auto factoryCount = (int) factory.size();

    if (choiceIndex < factoryCount)
        return factory[(size_t) juce::jlimit (0, factoryCount - 1, choiceIndex)].get();

    if (choiceIndex >= factoryCount + numUserSlots)
    {
        // A built-in table is built the first time it is chosen (message thread,
        // or here when rendering offline); until then the note plays a stand-in.
        const auto index = juce::jmin (choiceIndex - factoryCount - numUserSlots, BuiltInTables::count() - 1);
        if (const auto* ready = BuiltInTables::table (index))
            return ready;
        if (isNonRealtime())
            if (const auto* built = BuiltInTables::build (index))
                return built;

        BuiltInTables::want (index);
        return factory[(size_t) (index % factoryCount)].get();
    }

    const auto slot = juce::jlimit (0, numUserSlots - 1, choiceIndex - factoryCount);
    const juce::SpinLock::ScopedLockType lock (tableLock);
    return userTables[(size_t) slot].get();
}

void IlanaSynthAudioProcessor::flushAsyncUpdates()
{
    cancelPendingUpdate();
    handleAsyncUpdate();
}

const SampleData* IlanaSynthAudioProcessor::getSampleForOsc (int oscIndex) const
{
    if (oscIndex < 0 || oscIndex >= numSampleOscs)
        return nullptr;

    const auto factoryIndex = (int) getRawParam (sampleFactoryIds[(size_t) oscIndex]);

    if (factoryIndex > 0 && factoryIndex <= (int) factorySamples.size())
        return factorySamples[(size_t) (factoryIndex - 1)].get();

    const SampleData* user = nullptr;

    {
        const juce::SpinLock::ScopedLockType lock (sampleLock);
        user = sampleSlots[(size_t) oscIndex].get();
    }

    // Granular with nothing loaded: grains from the vocal sample rather than silence.
    if (user == nullptr && ! factorySamples.empty())
        if ((int) getRawParam (stringParamIds[(size_t) oscIndex][0]) == 3) // osc mode: Granular
            return factorySamples[(size_t) juce::jmin (1, (int) factorySamples.size() - 1)].get();

    return user;
}

juce::String IlanaSynthAudioProcessor::detectSampleRoot (int oscIndex)
{
    const auto* sample = getSampleForOsc (oscIndex);
    if (sample == nullptr || sample->getNumSamples() < 2)
        return "Load a sample first.";
    if (! sample->zones.empty())
        return "A multisample keeps the root note of each zone.";

    const auto found = PitchDetect::detect (sample->buffer, sample->sampleRate);
    if (! found.found())
        return "No clear pitch found. Set ROOT by hand.";

    const auto note = juce::jlimit (0, 127, (int) std::lround (found.midiNote));
    const auto cents = juce::jlimit (-50, 50, (int) std::lround ((found.midiNote - note) * 100.0));
    const auto prefix = juce::String (OscillatorIds::prefixes[(size_t) oscIndex]);
    const auto set = [this, &prefix] (const char* suffix, float value)
    {
        if (auto* parameter = apvts.getParameter (prefix + suffix))
        {
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
            parameter->endChangeGesture();
        }
    };
    // The cents go to the oscillator's FINE (its play-pitch offset), the same
    // correction a sample that is a little off its note needs.
    set ("_sample_root", (float) note);
    set ("_fine", (float) -cents);
    return "Root set to " + describeValue (prefix + "_sample_root", (float) note) + (cents != 0 ? ", FINE " + juce::String (-cents) + " ct" : juce::String())
           + " (" + juce::String (found.frequency, 1) + " Hz).";
}

std::shared_ptr<const SampleData> IlanaSynthAudioProcessor::getSharedSampleForOsc (int oscIndex) const
{
    if (oscIndex < 0 || oscIndex >= numSampleOscs)
        return nullptr;

    const auto factoryIndex = (int) getRawParam (sampleFactoryIds[(size_t) oscIndex]);
    if (factoryIndex > 0 && factoryIndex <= (int) factorySamples.size())
        return factorySamples[(size_t) (factoryIndex - 1)];

    std::shared_ptr<const SampleData> user;
    {
        const juce::SpinLock::ScopedLockType lock (sampleLock);
        user = sampleSlots[(size_t) oscIndex];
    }
    if (user == nullptr && ! factorySamples.empty())
        if ((int) getRawParam (stringParamIds[(size_t) oscIndex][0]) == 3)
            return factorySamples[(size_t) juce::jmin (1, (int) factorySamples.size() - 1)];
    return user;
}

namespace
{
// "3:24" or "45 s", for the import messages.
juce::String describeDuration (double seconds)
{
    const auto whole = juce::roundToInt (std::floor (seconds));
    if (whole < 60)
        return juce::String (whole) + " s";
    return juce::String (whole / 60) + ":" + juce::String (whole % 60).paddedLeft ('0', 2);
}
} // namespace

std::shared_ptr<SampleData> IlanaSynthAudioProcessor::decodeAudioFile (const juce::File& file, juce::String& error)
{
    error.clear();
    if (! file.existsAsFile())
    {
        error = "the file is not there any more.";
        return nullptr;
    }
    if (file.getSize() == 0)
    {
        error = "the file is empty.";
        return nullptr;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        if (formats.findFormatForFileExtension (file.getFileExtension()) == nullptr)
            error = "this kind of file (" + file.getFileExtension().toLowerCase() + ") can't be read here. WAV, AIFF, FLAC, OGG and MP3 can.";
        else
            error = "the audio in it could not be decoded. It may be damaged, or use a codec that isn't supported "
                    "(for example compressed WAV). Saving it again as a plain WAV usually helps.";
        return nullptr;
    }

    if (reader->lengthInSamples <= 1 || reader->sampleRate <= 0.0)
    {
        error = "it holds no audio.";
        return nullptr;
    }

    const auto maxSamples = (juce::int64) (reader->sampleRate * 120.0);
    const auto numSamples = (int) juce::jmin (reader->lengthInSamples, maxSamples);

    if (numSamples <= 1)
    {
        error = "it holds no audio.";
        return nullptr;
    }

    const auto numChannels = juce::jlimit (1, 2, (int) reader->numChannels);

    auto data = std::make_shared<SampleData>();
    data->sampleRate = reader->sampleRate;
    data->name = file.getFileNameWithoutExtension();
    data->buffer.setSize (numChannels, numSamples);

    if (! reader->read (&data->buffer, 0, numSamples, 0, true, true))
    {
        error = "reading the audio stopped part way. The file may be damaged.";
        return nullptr;
    }

    // Samples stop at two minutes: say so, with how long the file was.
    if (reader->lengthInSamples > maxSamples)
        data->loadWarning = "Cut at 2:00 of " + describeDuration ((double) reader->lengthInSamples / reader->sampleRate)
                            + ": samples stop at 120 seconds.";
    if (reader->numChannels > 2)
        data->loadWarning << (data->loadWarning.isEmpty() ? "" : " ") << "Only the first two of its "
                          << (int) reader->numChannels << " channels are played.";

    scrubAudio (data->buffer);
    return data;
}

bool IlanaSynthAudioProcessor::loadUserSample (int oscIndex, const juce::File& file, bool fromUser)
{
    ++sampleEpoch;
    ++dataEpoch; // what the editor draws changes
    lastSampleError.clear();
    if (oscIndex < 0 || oscIndex >= numSampleOscs)
        return false;

    // A file chosen by hand starts clean; a reload (a patch, undo, a preset)
    // brings back the file's saved options (step 36: an edited zone map).
    juce::ValueTree options;
    if (fromUser)
        setSampleOptions (oscIndex, {});
    else if (const auto xml = juce::parseXML (getSampleOptions (oscIndex)))
        if (const auto tree = juce::ValueTree::fromXml (*xml); tree.isValid() && tree.getProperty ("path").toString() == file.getFullPathName())
            options = tree;

    // Roadmap step 43: every way out says why, so a file that does nothing
    // is never a mystery.
    const auto fail = [this, &file] (const juce::String& why)
    {
        lastSampleError = "Couldn't load " + file.getFileName() + ": " + why;
        return false;
    };

    if (! file.existsAsFile())
        return fail ("the file is not there any more.");
    if (file.getSize() == 0)
        return fail ("the file is empty.");

    std::shared_ptr<SampleData> data;
    juce::String error;
    const auto multi = MultiSample::isMultiSampleFile (file);
    MultiSample::SfzReport sfz;
    // SoundFont and SFZ instruments: a multisample, its regions picked per note.
    if (multi)
    {
        // Step 38: a SoundFont reloads the preset picked from its list.
        data = MultiSample::load (file, error, &sfz, (int) options.getProperty ("bank", -1), (int) options.getProperty ("program", -1));
        if (data == nullptr)
            return fail (error.isNotEmpty() ? error + "." : juce::String ("the instrument could not be read."));
    }
    else
    {
        data = decodeAudioFile (file, error);
        if (data == nullptr)
            return fail (error);
    }

    // Step 32: a plain sample saved sliced is cut again the same way; the
    // zone edits after it refer to its slices.
    if (options.isValid() && ! multi && options.getChildWithName ("Slices").isValid())
        data = SampleSlicer::slice (*data, SampleSlicer::read (options.getChildWithName ("Slices")));
    if (options.isValid())
        data = applyZoneOptions (std::move (data), options, file);

    setUserSample (oscIndex, std::move (data), file.getFullPathName());
    if (multi)
    {
        juce::StringArray lost;
        for (const auto& missing : sfz.missing())
            lost.addIfNotAlreadyThere (missing.getFileName());
        {
            const juce::SpinLock::ScopedLockType lock (stateLock);
            sfzMissingFiles[(size_t) oscIndex] = lost;
        }
        ++missingSamplesVersion;
    }
    useLoadedSample (oscIndex, fromUser);
    return true;
}

SampleSlicer::Settings IlanaSynthAudioProcessor::getSampleSlices (int oscIndex) const
{
    if (oscIndex < 0 || oscIndex >= numSampleOscs || (int) getRawParam (sampleFactoryIds[(size_t) oscIndex]) > 0)
        return {};
    const juce::SpinLock::ScopedLockType lock (sampleLock);
    const auto& current = sampleSlots[(size_t) oscIndex];
    return current != nullptr ? SampleSlicer::settingsOf (*current) : SampleSlicer::Settings {};
}

bool IlanaSynthAudioProcessor::setSampleSlices (int oscIndex, const SampleSlicer::Settings& settings, const juce::String& undoName)
{
    if (oscIndex < 0 || oscIndex >= numSampleOscs || (int) getRawParam (sampleFactoryIds[(size_t) oscIndex]) > 0)
        return false;
    std::shared_ptr<SampleData> current;
    {
        const juce::SpinLock::ScopedLockType lock (sampleLock);
        current = sampleSlots[(size_t) oscIndex];
    }
    // A plain sample, or one already sliced (cut again from the whole).
    if (current == nullptr || current->getNumSamples() < 2 || (! current->zones.empty() && current->sliceMode == 0))
        return false;

    const auto whole = SampleSlicer::wholeOf (*current);
    auto data = SampleSlicer::slice (*whole, settings);

    juce::String path;
    {
        const juce::SpinLock::ScopedLockType lock (stateLock);
        path = samplePaths[(size_t) oscIndex];
    }

    beginEdit (undoName);
    if (path.isNotEmpty())
    {
        // Saved with the file's options: how it is cut. Edits to the old
        // slices (the zone map) don't carry over to new ones.
        juce::ValueTree options ("SampleOptions");
        if (const auto xml = juce::parseXML (getSampleOptions (oscIndex)))
            if (const auto old = juce::ValueTree::fromXml (*xml); old.isValid() && old.getProperty ("path").toString() == path)
                options = old.createCopy();
        options.setProperty ("path", path, nullptr);
        options.removeChild (options.getChildWithName ("Zones"), nullptr);
        options.removeChild (options.getChildWithName ("Slices"), nullptr);
        if (data->sliceMode != 0)
        {
            juce::ValueTree slices ("Slices");
            SampleSlicer::write (slices, SampleSlicer::settingsOf (*data));
            options.appendChild (slices, nullptr);
        }
        setUserSample (oscIndex, data, path);
        setSampleOptions (oscIndex, options.getNumChildren() > 0 || options.getNumProperties() > 1 ? options.toXmlString() : juce::String());
    }
    else
        setUserSample (oscIndex, data, {}); // kept in the patch: its slices travel with it
    endEdit();
    return true;
}

std::pair<int, int> IlanaSynthAudioProcessor::getSoundFontPreset (int oscIndex) const
{
    if (const auto xml = juce::parseXML (getSampleOptions (oscIndex)))
        if (const auto tree = juce::ValueTree::fromXml (*xml); tree.isValid() && tree.getProperty ("path").toString() == getSamplePath (oscIndex))
            return { (int) tree.getProperty ("bank", -1), (int) tree.getProperty ("program", -1) };
    return { -1, -1 };
}

bool IlanaSynthAudioProcessor::selectSoundFontPreset (int oscIndex, int bank, int program)
{
    const juce::File file (getSamplePath (oscIndex));
    if (! file.hasFileExtension ("sf2"))
        return false;
    // A new preset starts with its own zones: an edited map belonged to the old one.
    juce::ValueTree options ("SampleOptions");
    options.setProperty ("path", file.getFullPathName(), nullptr);
    options.setProperty ("bank", bank, nullptr);
    options.setProperty ("program", program, nullptr);
    const auto previous = getSampleOptions (oscIndex);
    setSampleOptions (oscIndex, options.toXmlString());
    if (loadUserSample (oscIndex, file, false))
        return true;
    setSampleOptions (oscIndex, previous);
    return false;
}

juce::String IlanaSynthAudioProcessor::getSampleOptions (int oscIndex) const
{
    if (oscIndex < 0 || oscIndex >= numSampleOscs)
        return {};
    const juce::SpinLock::ScopedLockType lock (stateLock);
    return sampleOptions[(size_t) oscIndex];
}

void IlanaSynthAudioProcessor::setSampleOptions (int oscIndex, const juce::String& options)
{
    if (oscIndex < 0 || oscIndex >= numSampleOscs)
        return;
    ++dataEpoch;
    const juce::SpinLock::ScopedLockType lock (stateLock);
    sampleOptions[(size_t) oscIndex] = options;
}

void IlanaSynthAudioProcessor::retargetSampleOptions (int oscIndex, const juce::String& newPath)
{
    if (const auto xml = juce::parseXML (getSampleOptions (oscIndex)))
        if (auto tree = juce::ValueTree::fromXml (*xml); tree.isValid())
        {
            tree.setProperty ("path", newPath, nullptr);
            setSampleOptions (oscIndex, tree.toXmlString());
        }
}

std::shared_ptr<SampleData> IlanaSynthAudioProcessor::applyZoneOptions (std::shared_ptr<SampleData> loaded, const juce::ValueTree& options,
                                                                        const juce::File& file)
{
    const auto map = options.getChildWithName ("Zones");
    if (! map.isValid() || map.getNumChildren() == 0 || loaded == nullptr)
        return loaded;

    // Each saved zone: one of the file's own (by its index) or an added
    // audio file, with its edited keys, velocities, root and level.
    std::map<juce::String, std::shared_ptr<SampleData>> added;
    added[file.getFullPathName()] = loaded->zones.empty() ? loaded : nullptr;
    std::vector<SampleZone> zones;
    juce::StringArray missing;
    for (const auto& entry : map)
    {
        SampleZone zone;
        const auto origin = (int) entry.getProperty ("origin", -1);
        const auto path = entry.getProperty ("file").toString();
        if (origin >= 0 && origin < (int) loaded->zones.size())
            zone = loaded->zones[(size_t) origin];
        else if (path.isNotEmpty())
        {
            auto& audio = added[path];
            if (audio == nullptr)
            {
                juce::String error;
                audio = decodeAudioFile (juce::File (path), error);
            }
            if (audio == nullptr)
            {
                missing.addIfNotAlreadyThere (juce::File (path).getFileName());
                continue;
            }
            zone.data = audio;
            zone.file = path;
        }
        else
            continue;
        zone.loKey = juce::jlimit (0, 127, (int) entry.getProperty ("loKey", zone.loKey));
        zone.hiKey = juce::jlimit (zone.loKey, 127, (int) entry.getProperty ("hiKey", zone.hiKey));
        zone.loVel = juce::jlimit (1, 127, (int) entry.getProperty ("loVel", zone.loVel));
        zone.hiVel = juce::jlimit (zone.loVel, 127, (int) entry.getProperty ("hiVel", zone.hiVel));
        zone.rootNote = juce::jlimit (-24.0, 151.0, (double) entry.getProperty ("root", zone.rootNote));
        zone.gain = juce::jlimit (0.0f, 8.0f, (float) (double) entry.getProperty ("gain", (double) zone.gain));
        zone.reverse = (bool) entry.getProperty ("reverse", zone.reverse);
        zones.push_back (std::move (zone));
    }
    if (zones.empty())
        return loaded;

    auto data = std::make_shared<SampleData>();
    data->name = loaded->name;
    data->zones = std::move (zones);
    data->loadWarning = loaded->loadWarning;
    if (! missing.isEmpty())
        data->loadWarning << (data->loadWarning.isEmpty() ? "" : " ") << "Zones whose files are gone were left out ("
                          << missing.joinIntoString (", ") << ").";
    MultiSample::finish (*data);
    data->copySlicingFrom (*loaded); // step 32: a sliced sample still shows the whole
    return data;
}

std::vector<SampleZone> IlanaSynthAudioProcessor::getSampleZones (int oscIndex) const
{
    if (oscIndex < 0 || oscIndex >= numSampleOscs || (int) getRawParam (sampleFactoryIds[(size_t) oscIndex]) > 0)
        return {};
    std::shared_ptr<SampleData> user;
    {
        const juce::SpinLock::ScopedLockType lock (sampleLock);
        user = sampleSlots[(size_t) oscIndex];
    }
    return user != nullptr ? user->zones : std::vector<SampleZone> {};
}

bool IlanaSynthAudioProcessor::setSampleZones (int oscIndex, std::vector<SampleZone> zones, const juce::String& undoName)
{
    if (oscIndex < 0 || oscIndex >= numSampleOscs || zones.empty() || (int) getRawParam (sampleFactoryIds[(size_t) oscIndex]) > 0)
        return false;
    std::shared_ptr<SampleData> current;
    {
        const juce::SpinLock::ScopedLockType lock (sampleLock);
        current = sampleSlots[(size_t) oscIndex];
    }
    if (current == nullptr)
        return false;
    zones.erase (std::remove_if (zones.begin(), zones.end(), [] (const SampleZone& zone) { return zone.data == nullptr; }), zones.end());
    if (zones.empty())
        return false;
    for (auto& zone : zones)
    {
        zone.loKey = juce::jlimit (0, 127, zone.loKey);
        zone.hiKey = juce::jlimit (zone.loKey, 127, zone.hiKey);
        zone.loVel = juce::jlimit (1, 127, zone.loVel);
        zone.hiVel = juce::jlimit (zone.loVel, 127, zone.hiVel);
    }

    auto data = std::make_shared<SampleData>();
    data->name = current->name;
    data->loadWarning = current->loadWarning;
    data->zones = zones;
    MultiSample::finish (*data);
    data->copySlicingFrom (*current); // step 32: a sliced sample still shows the whole
    // (finish numbers zones by place only when they came from no file: an
    // edited zone keeps the origin it was read with.)

    juce::String path;
    {
        const juce::SpinLock::ScopedLockType lock (stateLock);
        path = samplePaths[(size_t) oscIndex];
    }

    beginEdit (undoName);
    if (path.isNotEmpty())
    {
        // Saved as edits to the file: which of its zones (or which added
        // file) each one plays, with its keys, velocities, root and level.
        juce::ValueTree options ("SampleOptions");
        if (const auto xml = juce::parseXML (getSampleOptions (oscIndex)))
            if (const auto old = juce::ValueTree::fromXml (*xml); old.isValid() && old.getProperty ("path").toString() == path)
                options = old.createCopy();
        options.setProperty ("path", path, nullptr);
        options.removeChild (options.getChildWithName ("Zones"), nullptr);
        juce::ValueTree map ("Zones");
        for (const auto& zone : zones)
        {
            juce::ValueTree entry ("Zone");
            if (zone.file.isNotEmpty())
                entry.setProperty ("file", zone.file, nullptr);
            else
                entry.setProperty ("origin", zone.origin, nullptr);
            entry.setProperty ("loKey", zone.loKey, nullptr);
            entry.setProperty ("hiKey", zone.hiKey, nullptr);
            entry.setProperty ("loVel", zone.loVel, nullptr);
            entry.setProperty ("hiVel", zone.hiVel, nullptr);
            entry.setProperty ("root", zone.rootNote, nullptr);
            entry.setProperty ("gain", (double) zone.gain, nullptr);
            if (zone.reverse)
                entry.setProperty ("reverse", true, nullptr);
            map.appendChild (entry, nullptr);
        }
        options.appendChild (map, nullptr);
        setUserSample (oscIndex, data, path);
        setSampleOptions (oscIndex, options.toXmlString());
    }
    else
        setUserSample (oscIndex, data, {}); // kept in the patch: the zones travel with it
    endEdit();
    return true;
}

bool IlanaSynthAudioProcessor::addSampleZone (int oscIndex, const juce::File& file, int loKey, int hiKey, int loVel, int hiVel, int rootKey,
                                              juce::String& error)
{
    if (oscIndex < 0 || oscIndex >= numSampleOscs)
        return false;
    auto audio = decodeAudioFile (file, error);
    if (audio == nullptr)
    {
        error = "Couldn't add " + file.getFileName() + ": " + error;
        return false;
    }
    audio->findOnsets();

    std::shared_ptr<SampleData> current;
    juce::String path;
    {
        const juce::SpinLock::ScopedLockType lock (sampleLock);
        current = (int) getRawParam (sampleFactoryIds[(size_t) oscIndex]) > 0 ? nullptr : sampleSlots[(size_t) oscIndex];
    }
    {
        const juce::SpinLock::ScopedLockType lock (stateLock);
        path = samplePaths[(size_t) oscIndex];
    }

    std::vector<SampleZone> zones;
    if (current != nullptr && ! current->zones.empty())
        zones = current->zones;
    else if (current != nullptr && current->getNumSamples() > 1)
    {
        // A plain sample becomes the map's first zone: every key, rooted where it plays now.
        SampleZone first;
        first.data = current;
        first.file = path;
        first.rootNote = getRawParam (sampleRootIds[(size_t) oscIndex]); // its ROOT (step 29)
        zones.push_back (first);
    }

    SampleZone zone;
    zone.data = audio;
    zone.file = file.getFullPathName();
    zone.loKey = juce::jlimit (0, 127, juce::jmin (loKey, hiKey));
    zone.hiKey = juce::jlimit (zone.loKey, 127, juce::jmax (loKey, hiKey));
    zone.loVel = juce::jlimit (1, 127, juce::jmin (loVel, hiVel));
    zone.hiVel = juce::jlimit (zone.loVel, 127, juce::jmax (loVel, hiVel));
    zone.rootNote = juce::jlimit (0, 127, rootKey);
    zones.push_back (zone);

    if (current == nullptr)
    {
        // Nothing loaded: the added file is the whole multisample.
        auto data = std::make_shared<SampleData>();
        data->name = file.getFileNameWithoutExtension();
        data->zones = zones;
        MultiSample::finish (*data);
        beginEdit ("Add a zone");
        setUserSample (oscIndex, data, {});
        endEdit();
        if (auto* parameter = apvts.getParameter (sampleFactoryIds[(size_t) oscIndex]))
            if ((int) getRawParam (sampleFactoryIds[(size_t) oscIndex]) > 0)
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (0.0f));
        return true;
    }
    return setSampleZones (oscIndex, std::move (zones), "Add a zone");
}

void IlanaSynthAudioProcessor::useLoadedSample (int oscIndex, bool fromUser)
{
    // A chosen or dropped file replaces a factory sample source, as a bounce does.
    // (Not on a state restore: a patch may keep a path next to a factory pick.)
    if (! fromUser || (int) getRawParam (sampleFactoryIds[(size_t) oscIndex]) == 0)
        return;

    if (auto* parameter = apvts.getParameter (sampleFactoryIds[(size_t) oscIndex]))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (0.0f));
}

void IlanaSynthAudioProcessor::setUserSample (int oscIndex, std::shared_ptr<SampleData> data, const juce::String& path)
{
    ++sampleEpoch;
    ++dataEpoch; // what the editor draws changes
    if (oscIndex < 0 || oscIndex >= numSampleOscs)
        return;

    // Step 35: the attacks a random start can land on, found once here
    // (every user sample and multisample region comes in this way).
    if (data != nullptr)
    {
        data->findOnsets();
        for (auto& zone : data->zones)
            if (zone.data != nullptr)
                zone.data->findOnsets();
    }

    const auto embedded = data != nullptr && path.isEmpty() ? data : nullptr;
    {
        const juce::SpinLock::ScopedLockType lock (sampleLock);
        auto& retired = retiredSamples[(size_t) oscIndex];
        auto& index = retiredIndex[(size_t) oscIndex];
        retired[(size_t) index] = std::move (sampleSlots[(size_t) oscIndex]);
        index = (index + 1) % (int) retired.size();
        sampleSlots[(size_t) oscIndex] = std::move (data);
    }

    const juce::SpinLock::ScopedLockType lock (stateLock);
    samplePaths[(size_t) oscIndex] = path;
    embeddedSamples[(size_t) oscIndex] = embedded;
    sfzMissingFiles[(size_t) oscIndex].clear();
    encodedSamples[(size_t) oscIndex] = {};
    // The latest sample wins over a clear or reload still queued.
    pendingSampleClear[(size_t) oscIndex] = false;
    pendingSamplePaths[(size_t) oscIndex].clear();
}

bool IlanaSynthAudioProcessor::isSampleEmbedded (int oscIndex) const
{
    if (oscIndex < 0 || oscIndex >= numSampleOscs)
        return false;
    const juce::SpinLock::ScopedLockType lock (stateLock);
    return embeddedSamples[(size_t) oscIndex] != nullptr;
}

bool IlanaSynthAudioProcessor::loadUserWavetable (int slot, const juce::File& file, Wavetable::LoadMode mode)
{
    ++dataEpoch; // what the editor draws changes
    if (slot < 0 || slot >= numUserSlots)
        return false;

    WavetableDoc doc;
    if (! WavetableDoc::loadFromFile (file, doc, mode))
        return false;

    return setUserTable (slot, doc);
}

void IlanaSynthAudioProcessor::swapUserTable (int slot, std::shared_ptr<Wavetable> table)
{
    ++dataEpoch; // what the editor draws changes
    const juce::SpinLock::ScopedLockType lock (tableLock);
    auto& index = retiredTableIndex[(size_t) slot];
    retiredTables[(size_t) (slot * 3 + index)] = std::move (userTables[(size_t) slot]);
    index = (index + 1) % 3;
    userTables[(size_t) slot] = std::move (table);
}

bool IlanaSynthAudioProcessor::setUserTable (int slot, const WavetableDoc& doc)
{
    ++dataEpoch; // what the editor draws changes
    if (slot < 0 || slot >= numUserSlots || doc.frames.empty())
        return false;

    auto table = std::make_shared<Wavetable>();
    table->buildFromFrames (doc.frames);
    table->setName (doc.name);
    swapUserTable (slot, std::move (table));

    const juce::SpinLock::ScopedLockType lock (stateLock);
    userTableDocs[(size_t) slot] = std::make_shared<const WavetableDoc> (doc);
    userTablePaths[(size_t) slot] = doc.sourcePath;
    userTableModes[(size_t) slot] = doc.sourceMode;
    return true;
}

// A slot's default is a factory table. It shares the factory copy (the
// factory tables live for the whole program), so 16 slots cost nothing.
void IlanaSynthAudioProcessor::resetUserTableToDefault (int slot)
{
    ++dataEpoch; // what the editor draws changes
    if (slot < 0 || slot >= numUserSlots)
        return;

    const auto& factory = FactoryTables::get().tables;
    swapUserTable (slot, std::shared_ptr<Wavetable> (std::shared_ptr<Wavetable>(),
                                                     factory[(size_t) (slot % (int) factory.size())].get()));

    const juce::SpinLock::ScopedLockType lock (stateLock);
    userTablePaths[(size_t) slot].clear();
    userTableDocs[(size_t) slot].reset();
}

WavetableDoc IlanaSynthAudioProcessor::getUserTableDoc (int slot) const
{
    slot = juce::jlimit (0, numUserSlots - 1, slot);
    std::shared_ptr<const WavetableDoc> doc;
    {
        const juce::SpinLock::ScopedLockType lock (stateLock);
        doc = userTableDocs[(size_t) slot];
    }
    if (doc != nullptr)
        return *doc;
    auto fallback = WavetableDoc::fromFactory (slot % TableFactory::getNumFactoryTables());
    fallback.name = "User " + juce::String (slot + 1);
    return fallback;
}

std::shared_ptr<const WavetableDoc> IlanaSynthAudioProcessor::getUserTableDocPtr (int slot) const
{
    if (slot < 0 || slot >= numUserSlots)
        return nullptr;
    const juce::SpinLock::ScopedLockType lock (stateLock);
    return userTableDocs[(size_t) slot];
}

bool IlanaSynthAudioProcessor::isUserSlotEdited (int slot) const
{
    if (slot < 0 || slot >= numUserSlots)
        return false;
    const juce::SpinLock::ScopedLockType lock (stateLock);
    return userTableDocs[(size_t) slot] != nullptr;
}

int IlanaSynthAudioProcessor::findFreeUserSlot() const
{
    const auto factoryCount = TableFactory::getNumFactoryTables();
    std::array<bool, (size_t) numUserSlots> used {};
    for (const auto* prefix : OscillatorIds::prefixes)
        if (const auto* value = apvts.getRawParameterValue (juce::String (prefix) + "_table"))
        {
            const auto slot = juce::roundToInt (value->load()) - factoryCount;
            if (slot >= 0 && slot < numUserSlots)
                used[(size_t) slot] = true;
        }
    for (int slot = 0; slot < numUserSlots; ++slot)
        if (! used[(size_t) slot] && ! isUserSlotEdited (slot))
            return slot;
    return -1;
}

juce::String IlanaSynthAudioProcessor::getTableNotice() const
{
    const juce::SpinLock::ScopedLockType lock (stateLock);
    return tableNotice;
}

void IlanaSynthAudioProcessor::clearTableNotice()
{
    {
        const juce::SpinLock::ScopedLockType lock (stateLock);
        tableNotice.clear();
    }
    ++tableNoticeVersion;
}

