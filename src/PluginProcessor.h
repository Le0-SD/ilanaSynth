#pragma once

#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "SectionColours.h"

#include <array>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "dsp/Biquad.h"
#include "dsp/Generative.h"
#include "dsp/FreeverbFast.h"
#include "dsp/GranularPitchShift.h"
#include "dsp/GranularSmear.h"
#include "dsp/IlanaSynth.h"
#include "dsp/KarplusStrong.h"
#include "MtsEsp.h"
#include "dsp/LfoCurve.h"
#include "dsp/LfoShape.h"
#include "dsp/Evolve.h"
#include "dsp/FmAlgorithms.h"
#include "dsp/Mseg.h"
#include "dsp/SpectralFreeze.h"
#include "dsp/StretchRenderer.h"
#include "dsp/Svf.h"
#include "dsp/FilterUnit.h"
#include "dsp/SympatheticStrings.h"
#include "dsp/AcousticKeys.h"
#include "dsp/Modulation.h"
#include "dsp/OscillatorIds.h"
#include "dsp/VoiceRouting.h"
#include "dsp/airwindows/AirwindowsModule.h"
#include "dsp/airwindows/Categories.h"
#include "dsp/Vocoder.h"
#include "dsp/SamplePlayer.h"
#include "dsp/SampleSlicer.h"
#include "dsp/SpectralCache.h"
#include "dsp/Wavetable.h"
#include "dsp/WavetableDoc.h"
#include "TuningState.h"
#include "ClipState.h"
#include "processor/FxPatch.h"

namespace PatchGraph { class Graph; class Runner; }

class IlanaSynthAudioProcessor : public juce::AudioProcessor,
                                 private juce::AsyncUpdater
{
    // First member, so it runs before any string is built: the strings'
    // seeds start over for each instance, and every instance renders alike.
    struct StringSeedStart
    {
        StringSeedStart() { KarplusStrong::restartSeeds(); }
    } stringSeedStart;

public:
    // M7.4: 16 patch tables (was 4 user slots; the choices were appended).
    static constexpr int numUserSlots = 256; // patch tables; the editor lists only the ones in use
    static constexpr int numFxSlots = 10;
    // Tests: keep the effects running through silence (no rack sleep).
    inline static bool disableFxSleep = false;
    // Tests: how often the effects' safety net caught a non-finite sample (reset to 0) or one beyond +-16 (clamped).
    inline static std::atomic<long> sanitisedNonFinite { 0 }, sanitisedClamped { 0 };
    // Tests: -1 follows MULTI-CORE, 0 one core, 1 several.
    inline static int forceVoiceThreads = -1;
    static constexpr int numFxTypes = 42; // 30: Airwindows, 31: Vocoder, 32-41: Airwindows categories, 42: Filter

    EqSettings getEqSettings() const;
    static constexpr int numLfos = Mod::numLfoSources;
    // LFO 1-4 keep lfoBuffers channels 0-3; the clocked S&H and MSEG sit at
    // 4 and 5, and LFO 5-16 follow.
    static constexpr int lfoChannel (int lfo) { return lfo < 4 ? lfo : lfo + 2; }
    // M8.1: each LFO's output B follows, from channel numLfos + 2.
    static constexpr int lfoChannelB (int lfo) { return numLfos + 2 + lfo; }
    static constexpr int numLfoChannels = 2 * numLfos + 2;
    static constexpr int maxDestinations = 1024;

    float getFxMod (Mod::Destination destination, float depth) const
    {
        return modDisplayValues[(size_t) destination].load() * depth;
    }
    float getCompGainReduction() const;
    // Display only (the FX cards' meters): the limiter's deepest gain in the
    // last block, and OTT's LOW / MID / HIGH gains at the block's end.
    float getLimiterGainReduction() const;
    float getOttBandGain (int band) const;
    // FREEZE's spectrum (the held one while HOLD is on) for its card's picture.
    void getFreezeBands (std::array<float, SpectralFreeze::numBands>& out) const;
    float getFxSlotCpu (int slot) const { return fxSlotCpu[(size_t) juce::jlimit (0, numFxSlots - 1, slot)].load(); }
    // What a slot did to the signal in the last blocks: its input and output
    // levels (RMS, linear, smoothed), both 0 while the slot is empty or
    // bypassed. A reading for the rack's signal rail only; it never touches
    // the audio.
    float getFxSlotInLevel (int slot) const { return fxSlotIn[(size_t) juce::jlimit (0, numFxSlots - 1, slot)].load(); }
    float getFxSlotOutLevel (int slot) const { return fxSlotOut[(size_t) juce::jlimit (0, numFxSlots - 1, slot)].load(); }
    // Puts a module type into an FX slot and switches on the module's own
    // enable flag, so a freshly added effect is audible straight away.
    void assignFxSlot (int slot, int type);
    // Makes an Airwindows algorithm ahead of its use (the editor picks one).
    void preloadAirwindows (int algorithm);
    void preloadAirwindowsCategory (int category, int choice);
    void randomizeFxChain();
    bool saveFxChainToFile (const juce::File& file);
    bool loadFxChainFromFile (const juce::File& file);
    void loadReverbIr (const juce::File& file);
    void switchFxChain();
    void copyFxChainToOtherBank();
    bool isShowingChainA() const { return showingChainA; }
    IlanaSynthAudioProcessor();
    ~IlanaSynthAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 10.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

   #if JUCE_LINUX || JUCE_BSD
    // Keeps the VST3 host's run loop alive; see HostRunLoopKeepAlive in
    // PluginProcessor.cpp.
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return &hostRunLoopKeepAlive; }
   #endif

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    const Wavetable* getWavetable (int index) const { return getTableForChoice (index); }
    bool isSpectralWarpReady (int osc) const { return spectralCache->isReady (osc); }
    float getLfoLiveValue (int lfo) const { return lfoLastValues[(size_t) juce::jlimit (0, numLfos - 1, lfo)].load(); }
    // M8.1: a simulated LFO shape's settings (the card's picture reads them too).
    LfoSimSettings readLfoSimSettings (int lfo) const;
    // M8.5: Evolve and the vector pad.
    static constexpr int numVectorPoints = 8;
    float macroValue (int macro) const;
    static std::array<float, 4> vectorWeights (float x, float y);
    juce::Point<float> getVectorPosition() const { return { vectorX.load(), vectorY.load() }; }
    int getVectorCorner (int corner) const;
    bool isVectorPathOn() const;
    juce::Point<float> getVectorPathPoint (int point) const;
    float getMacroDrift (int macro) const { return macroDrift[(size_t) juce::jlimit (0, Mod::numMacros - 1, macro)].load(); }
    void freezeEvolve();

    // M8.3: the loudest voice's WEST gate conductance, for the card.
    float getWestGateLevel() const { return westGateDisplay.load(); }
    // The vocoder's band levels as it last ran (FX display; 0 while idle).
    float getVocoderBandLevel (int band) const;
    float getLfoLiveValueB (int lfo) const { return lfoLastValuesB[(size_t) juce::jlimit (0, numLfos - 1, lfo)].load(); }

    // The spectrally warped table an oscillator is playing, for display
    // (null when its warp is off or still building).
    std::shared_ptr<const Wavetable> getSpectralDisplayTable (int osc, int tableChoice) const
    {
        static const char* ids[] { "osc1_spectral", "osc2_spectral", "sub_spectral" };
        const auto* mode = apvts.getRawParameterValue (ids[juce::jlimit (0, 2, osc)]);
        return spectralCache->getForDisplay (juce::jlimit (0, 2, osc), tableChoice, mode != nullptr ? (int) mode->load() : 0);
    }

    // Synth-wide modulation of a destination this block (loudest voice for
    // per-voice sources), for knob rings and the effects.
    float getModDisplay (int destination) const
    {
        return juce::isPositiveAndBelow (destination, maxDestinations) ? modDisplayValues[(size_t) destination].load() : 0.0f;
    }

    float getModDisplay (Mod::Destination destination) const { return getModDisplay ((int) destination); }

    // Reads one mod slot's settings from the parameters (message thread).
    Mod::Slot readModSlot (int slotIndex) const;
    int getNumUsedModSlots() const;

    float getLfoPhase (int index) const
    {
        return lfoPhaseDisplays[(size_t) juce::jlimit (0, numLfos - 1, index)].load();
    }
    float getDisplayPhase() const { return displayPhase.load(); }
    float getDisplayFrequency() const { return displayFrequency.load(); }
    float getSamplePosition (int oscIndex) const
    {
        return displaySamplePositions[(size_t) juce::jlimit (0, OscillatorIds::count - 1, oscIndex)].load();
    }

    float getWavetablePhase (int oscIndex) const
    {
        return oscDisplayPhases[(size_t) juce::jlimit (0, OscillatorIds::count - 1, oscIndex)].load();
    }
    double getCurrentBpm() const { return currentBpm.load(); }
    float getArpStepRateHz() const;
    float getSourceDisplayValue (int sourceIndex) const;
    // 1 (off), 2 or 4. Switching reprepares the voices, so it happens on
    // the message thread (the audio thread asks for it asynchronously).
    void setOversampling (int factor);
    bool isOversampling() const { return oversamplingFactor.load() > 1; }
    int getOversamplingFactor() const { return oversamplingFactor.load(); }
    double getCurrentSampleRate() const { return displaySampleRate.load(); }

    juce::UndoManager& getUndoManager() { return undoManager; }

    // Undo (UI review 4): every edit made by a gesture other than a knob
    // drag (JUCE's attachments start those) is one named undo step.
    // beginEdit starts the step at the gesture's start; endEdit closes it at
    // its end. Edits to data that isn't a parameter (drawn LFO steps and
    // curves, remap curves, clips) made in between join the step as one
    // action that puts the data back through the same setters. performEdit
    // wraps a one-shot edit (a menu item, a double-click). Message thread only.
    void beginEdit (const juce::String& name);
    void endEdit();
    void performEdit (const juce::String& name, const std::function<void()>& edit)
    {
        beginEdit (name);
        edit();
        endEdit();
    }
    // A hash of that data, for the editor's EDITED marker (cached until the
    // data epoch moves).
    juce::int64 getPatchDataHash() const;

    juce::StringArray getFactoryPresetNames() const;
    juce::StringArray getFactoryPresetCategories() const;
    juce::Array<juce::File> getUserPresetFiles() const;
    // The four macro names a factory preset loads with (its own, the
    // voicing's, then the automatic ones), worked out without loading it;
    // an empty string is a macro with no name.
    juce::StringArray getFactoryMacroNames (int factoryIndex) const;
    juce::StringArray getAllPresetNames() const;
    // The browser's view of every preset (factory, then user files): its
    // category (DX7 voices under the kind of sound they are), its tags
    // (factory presets from src/PresetTags.h, written by
    // tools/tag_presets.py), and the DX7 bank it comes from ("" when it isn't
    // a DX7 voice: ROM1A..., or an imported bank's folder).
    juce::StringArray getAllPresetCategories() const;
    juce::StringArray getAllPresetTags() const;
    juce::StringArray getAllPresetBanks() const;
    // For each preset, the earlier one it repeats parameter for parameter
    // (the ROM cartridges carry some DX7 voices twice), or -1.
    juce::Array<int> getPresetRepeats() const;
    // Who made a preset and what its author says about it (user presets
    // carry what Save As asked for; factory presets name their source).
    struct PresetInfo
    {
        juce::String author, comment;
    };
    PresetInfo getPresetInfo (int index) const;
    bool isUserPreset (int index) const { return index >= (int) getFactoryPresetNames().size(); }
    int getNumAllPresets() const;
    // False when a user preset's file is gone or unreadable (nothing changed).
    bool loadPresetByIndex (int index);
    // A user preset by its file (the browser resolves a click to a path, so a
    // changed folder can't shift which file loads). False as above.
    bool loadUserPreset (const juce::File& file) { ++dataEpoch; return loadPresetFromFile (file); }
    // The file the open patch was loaded from or saved to ({} for a factory
    // preset or a session). Two files may share a base name, so Save goes by
    // this. Message thread only.
    juce::File getLoadedPresetFile() const { return loadedPresetFile; }
    void setLoadedPresetFile (const juce::File& file) { loadedPresetFile = file; }

    // Category and tags travel with the patch (state tree) and are read back
    // from user preset files for the browser. Message thread only.
    static juce::StringArray getPresetCategoryChoices()
    {
        return { "Bass", "Lead", "Brass", "Wind", "Pluck", "Pad", "Keys", "Chords", "Arp", "Drone", "FX", "Other" };
    }

    void setPresetMeta (const juce::String& category, const juce::String& tags)
    {
        apvts.state.setProperty ("presetCategory", category, nullptr);
        apvts.state.setProperty ("presetTags", tags, nullptr);
    }

    void setPresetInfo (const juce::String& author, const juce::String& comment)
    {
        apvts.state.setProperty ("presetAuthor", author, nullptr);
        apvts.state.setProperty ("presetComment", comment, nullptr);
    }

    juce::String getPresetAuthor() const { return apvts.state.getProperty ("presetAuthor").toString(); }
    juce::String getPresetComment() const { return apvts.state.getProperty ("presetComment").toString(); }
    juce::String getPresetCategory() const { return apvts.state.getProperty ("presetCategory").toString(); }
    juce::String getPresetTags() const { return apvts.state.getProperty ("presetTags").toString(); }
    void loadFactoryPreset (int index);
    // Builds the built-in wavetables the oscillators have chosen (message thread).
    void ensureBuiltInTables();
    // The critic's trims (src/PresetTrims.h) for a factory preset: its level,
    // and the depth of its macros' routings. ILANA_NO_TRIMS turns them off
    // (for the tuning tool's own renders).
    // The diversity pass's redesigns (src/PresetVoicing.h): parameter
    // changes and macro rewiring over the recipe, applied before the trims.
    // Off in tests that check what a recipe itself loads.
    static inline bool presetVoicingEnabled = true;
    static void applyPresetVoicing (const char* presetName, std::vector<std::pair<juce::String, float>>& values,
                                    std::array<juce::String, 4>& macroNames);
    static void applyPresetTrims (const char* presetName, const juce::String& category,
                                  std::vector<std::pair<juce::String, float>>& values);
    // MASTER's value in a preset's values moves to output_trim (review 6).
    static void moveLevelToTrim (std::vector<std::pair<juce::String, float>>& values);
    bool savePresetToFile (const juce::File& file);
    bool loadPresetFromFile (const juce::File& file);

    // The loaded preset's name lives in the state tree so it survives editor
    // re-opens and host session reloads. Message thread only.
    juce::String getCurrentPresetName() const { return apvts.state.getProperty ("presetName").toString(); }
    void setCurrentPresetName (const juce::String& name) { apvts.state.setProperty ("presetName", name, nullptr); }

    // Macro names travel with the patch (state tree), so presets and host
    // sessions keep them. Message thread only.
    juce::String getMacroName (int macroIndex) const
    {
        const auto name = apvts.state.getProperty ("macroName" + juce::String (macroIndex + 1)).toString();
        return name.isNotEmpty() ? name : "Macro " + juce::String (macroIndex + 1);
    }

    void setMacroName (int macroIndex, const juce::String& name)
    {
        ++dataEpoch;
        apvts.state.setProperty ("macroName" + juce::String (macroIndex + 1), name, nullptr);
    }

    // Routes a source to a destination in the first free slot; returns the
    // slot index or -1 when all slots are taken.
    int assignModSlot (int sourceIndex, int destination, float depth);

    // Gives a patch without macro mappings a sensible set (tone, timbre,
    // drive, space), chosen from what the patch uses. Silent at macro 0.
    // Macros marked in keep (the preset voicing's own) are left alone.
    void applyDefaultMacros (const std::array<bool, 4>& keep = {});

    static void migrateLegacyOsc3 (const std::function<float (const juce::String&, float)>& get,
                                   const std::function<void (const juce::String&, float)>& set);
    // Before the FM / DX7 type (2026-10-06) an operator was a Wavetable
    // oscillator tuned by ratio or fixed Hz, or on the Operator EG: such an
    // oscillator becomes FM / DX7 (OscMode::fmOperator), which renders the
    // same. Works on stored values (get falls back to the default); returns
    // how many oscillators moved. Idempotent.
    // The string and exciter knobs were rescaled so each has a clear effect
    // across its travel (see tools/knob_audit.py); this moves a saved patch's
    // old values to the new scales so it keeps its sound. Returns how many
    // values it changed.
    static int migrateStringKnobs (const std::function<float (const juce::String&, float)>& get,
                                   const std::function<void (const juce::String&, float)>& set);
    static int migrateOperatorModes (const std::function<float (const juce::String&, float)>& get,
                                     const std::function<void (const juce::String&, float)>& set);
    // ENV AMOUNT and the filter envelope's VELOCITY knob are gone: the matrix
    // does their job. A patch that sets Filter 1's or Filter 2's ENV AMOUNT
    // gets matrix routes with the same sound (FILTER ENV to the cutoff, and
    // the same again via VELOCITY for the share Filter 1's VELOCITY knob gave
    // to velocity) and the knob goes to 0. Returns how many filters moved.
    // A patch with no free slots keeps its amount (it still plays, unseen).
    static int migrateFilterEnvAmounts (const std::function<float (const juce::String&, float)>& get,
                                        const std::function<void (const juce::String&, float)>& set);
    // An envelope's TENSION knob is gone: it was added to the attack, decay
    // and release curves (the dots on the graph), so it moves into them.
    static int migrateEnvelopeTension (const std::function<float (const juce::String&, float)>& get,
                                       const std::function<void (const juce::String&, float)>& set);
    // An oscillator's WARP ENV and its ENV AMT are gone: the matrix routes
    // that envelope to the oscillator's warp amounts instead.
    static int migrateWarpEnvelopes (const std::function<float (const juce::String&, float)>& get,
                                     const std::function<void (const juce::String&, float)>& set);
    // All three, for a patch from before the knobs went.
    static void migrateRedundantKnobs (const std::function<float (const juce::String&, float)>& get,
                                       const std::function<void (const juce::String&, float)>& set);
    // A route saved before 100 % meant a knob's whole range (2026-10-23)
    // gets its amount scaled so it moves the target as far as before
    // (Mod::Range::legacyDepthFactor). Returns the routes changed.
    static int migrateModDepthRanges (const std::function<float (const juce::String&, float)>& get,
                                      const std::function<void (const juce::String&, float)>& set);
    // The same on the live parameters (a factory preset, written for the old ranges).
    void migrateModDepthRanges();
    // The same on the live parameters (after a bounce or a factory preset).
    void migrateOperatorModes();
    bool clearModSlotsForTarget (int destination);
    void clearModSlot (int slotIndex);
    // Two slots routing the same source to the same destination (with the
    // same via, polarity, curve and bypass, and no drawn remap) play as one
    // slot with the depths added, so they can be merged without changing
    // the sound. canMergeModSlots says why not when they can't be (they
    // differ, or the sum would pass 100%). mergeModSlots folds `from` into
    // `into`; mergeDuplicateModSlots merges every such pair (factory presets
    // load merged) and returns how many slots it freed. Message thread.
    bool canMergeModSlots (int into, int from, juce::String* reason = nullptr) const;
    bool mergeModSlots (int into, int from);
    int mergeDuplicateModSlots();
    void setModSlotValue (int slotIndex, const juce::String& field, float value);
    juce::String getModSlotParamId (int slotIndex, const juce::String& field) const;

    static constexpr int scopeSize = 4096;
    void copyScopeData (float* left, float* right, int numSamples) const;
    // The same window, but ending part-way into the newest block by the time
    // since that block arrived (one block behind at most): the scope moves
    // smoothly at the display's rate however large the host's buffer is.
    void copyScopeDataSmooth (float* left, float* right, int numSamples) const;

    static constexpr int lfoDrawSteps = 64;
    void setLfoCustomPoint (int lfoIndex, int step, float value);
    float getLfoCustomPoint (int lfoIndex, int step) const;

    // The drawable "Curve" LFO shape (message thread).
    static constexpr int curveShape = 8;
    LfoCurve getLfoCurve (int lfoIndex) const;
    void setLfoCurve (int lfoIndex, const LfoCurve& curve);

    // The old four-point MSEG module (UI review 9, I9-2): a looping, routed
    // MSEG that no oscillator plays as its ENVELOPE becomes an LFO drawn the
    // same (SHAPE › MSEG, its four points, its RATE) with its routes, so a
    // patch has one MSEG editor. Patch loads run it; the LFO is the first
    // free one whose random draws nothing else hears (LFO 1-4 share a
    // generator with the Clocked S&H and the gate). legacyMsegTargetLfo is
    // that LFO, or -1 when the MSEG stays a module. Message thread.
    int legacyMsegTargetLfo() const;
    bool moveLegacyMsegToLfo();

    // A drawn remap curve per mod slot (Vital's per-route remap). A straight
    // line from -1 to 1 is off; the slot then shapes as before.
    static LfoCurve identityRemap() { LfoCurve curve; curve.points = { { 0.0f, -1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } }; return curve; }
    static bool isIdentityRemap (const LfoCurve& curve);
    LfoCurve getModRemap (int slotIndex) const;
    bool isModRemapOn (int slotIndex) const;
    void setModRemap (int slotIndex, const LfoCurve& curve);
    void resetModRemap (int slotIndex) { setModRemap (slotIndex, identityRemap()); }
    void resetAllModRemaps();
    float getLfoCurveValue (int lfoIndex, double phase) const;

    void triggerPreviewNote (int midiNote, bool isOn, float velocity = 0.7f);
    void panic() { synth.allNotesOff (0, false); }

    // A whole patch's parameters swapped in under playing notes (the A/B
    // button): the next block's first sample continues from the last one's
    // (the step is cancelled, then eased out over 2 ms) instead of jumping.
    // Call it after the new values are in.
    void easeNextBlock() { easeStep = true; }
    // The whole patch as a state tree, for the editor's A/B slots.
    juce::ValueTree captureWholePatch() { return buildFullState(); }
    void restoreWholePatch (const juce::ValueTree& state) { applyFullState (state); }
    float getCpuUsage() const { return cpuUsage.load(); }

    // The loudest output sample per channel since the last call (for the meter).
    float takeOutputPeak (int channel) { return outputPeaks[(size_t) juce::jlimit (0, 1, channel)].exchange (0.0f); }
    // Tools only (ilanaSnapshot --flair): the meters read this buffer's peaks, as if it were the output.
    void setOutputPeakForTest (const juce::AudioBuffer<float>& buffer)
    {
        for (int channel = 0; channel < juce::jmin (2, buffer.getNumChannels()); ++channel)
            outputPeaks[(size_t) channel].store (juce::jmax (outputPeaks[(size_t) channel].load(), buffer.getMagnitude (channel, 0, buffer.getNumSamples())));
    }
    // For views that animate with the playing (a Physical oscillator's string): notes
    // started so far, and the last block's peak (not reset by reading).
    unsigned getNoteOnCount() const { return noteOnCount.load(); }
    float getOutputPeak() const { return outputLevelDisplay.load(); }
    int getActiveVoiceCount() const { return activeVoiceCount.load(); }
    // Voices still rendering, tails included (getActiveVoiceCount counts only
    // those with their envelopes up). Audio thread or tests only.
    int getRenderingVoiceCount() const
    {
        auto count = 0;
        for (int i = 0; i < synth.getNumVoices(); ++i)
            if (synth.getVoice (i)->isVoiceActive())
                ++count;
        return count;
    }

    // Changes whenever something the editor draws may have changed: any
    // parameter, an edit to data that isn't a parameter (LFO curves and
    // drawn steps, tables, samples, macro names, a restored state), a table
    // or reveal change, a new note, or a block rendered while something
    // sounds (voices, output or input). Views compare it to
    // skip repainting while nothing moves.
    // For the editor's EDITED marker: the preset name and the parameters'
    // fingerprint when it was loaded or saved (kept here so it outlives the
    // editor). Message thread only.
    std::optional<std::pair<juce::String, juce::int64>> loadedPresetFingerprint;

    unsigned getDataEpoch() const { return dataEpoch.load(); }
    // Counts sample loads (a new sample can reuse a freed one's address).
    unsigned getSampleEpoch() const { return sampleEpoch.load(); }

    juce::uint64 getUiEpoch() const
    {
        return (juce::uint64) paramEpoch.load() + ((juce::uint64) liveEpoch.load() << 32)
             + (juce::uint64) dataEpoch.load() * 15485863u
             + (juce::uint64) revealVersion.load() * 7919u + (juce::uint64) tableNoticeVersion.load() * 104729u
             + (juce::uint64) noteOnCount.load() * 1299709u;
    }

    // LFO phase of every sounding voice (for tests and diagnostics).
    std::vector<float> getVoiceLfoPhasesForTest (int lfo)
    {
        std::vector<float> phases;

        for (int i = 0; i < synth.getNumVoices(); ++i)
            if (auto* voice = dynamic_cast<Voice*> (synth.getVoice (i)))
                if (voice->isVoiceActive())
                    phases.push_back (voice->getLfoPhase (lfo));

        return phases;
    }
    void startMacroLearn (int macroIndex);
    void cancelMacroLearn();
    int getMacroLearnTarget() const { return macroLearn.load(); }
    int getMacroCc (int macroIndex) const { return macroCc[juce::jlimit (0, Mod::numMacros - 1, macroIndex)].load(); }
    // MIDI learn for any automatable parameter (macros keep their own CCs
    // above): the next controller moved drives it, one CC per parameter.
    // Saved with the patch as "midiCcMap"; a state without it (and a factory
    // preset) leaves the current map alone, as it belongs to the controller.
    void startParamLearn (const juce::String& parameterId);
    void cancelParamLearn();
    juce::String getParamLearnTarget() const;
    int getParamCc (const juce::String& parameterId) const; // -1: none
    void clearParamCc (const juce::String& parameterId);
    juce::File getUserPresetDirectory() const;
    // Points the user preset folder elsewhere (the UI test's temporary
    // folder, so it never touches the user's own presets). Empty: the default.
    static inline juce::File userPresetDirectoryOverride;
    float getEnvMonitorAmp() const { return envMonitorAmp.load(); }
    float getEnvMonitorFilter() const { return envMonitorFilter.load(); }
    float getEnvMonitorFilter2() const { return envMonitorFilter2.load(); }
    float getEnvMonitorMod() const { return envMonitorMod.load(); }
    float getEnvMonitorEnv4() const { return envMonitorEnv4.load(); }
    // The Operator Env of the loudest voice: seconds since its note began
    // and since its key was let go (-1: held, or no such note).
    float getOpEnvMonitorSeconds() const { return monitorOpEnvSeconds.load(); }
    float getOpEnvMonitorRelease() const { return monitorOpEnvRelease.load(); }
    float getEnvMonitorExtra (int index) const { return envMonitorExtra[(size_t) juce::jlimit (0, 10, index)].load(); }
    // ENV 1-16 (amp, filter, filter 2, mod, ENV 5, ENV 6-16) of the voice the
    // monitors follow: stage plus progress (TensionAdsr::getDisplayPosition).
    float getEnvMonitorPosition (int env) const { return envMonitorPositions[(size_t) juce::jlimit (0, 15, env)].load(); }
    // The same voice's level where ENV 1-16's running attack or release began
    // (0 in other stages), and a count that goes up with every
    // note on (the playhead restarts on it, so a stage shorter than a block
    // is still drawn).
    float getEnvMonitorStageStart (int env) const { return envMonitorStageStarts[(size_t) juce::jlimit (0, 15, env)].load(); }
    // ENV 1-16's stage lengths as that voice runs them (delay, attack, hold,
    // decay, release in seconds).
    std::array<float, 5> getEnvMonitorSeconds (int env) const
    {
        std::array<float, 5> out;
        for (size_t i = 0; i < 5; ++i)
            out[i] = envMonitorSeconds[(size_t) juce::jlimit (0, 15, env) * 5 + i].load();
        return out;
    }
    int getNoteSerial() const { return noteSerial.load(); }
    // The keys the synth is playing (host, arp, SEQ, spray, the on-screen
    // keyboard), for the keyboard strip: the note-on velocity (1-127) while
    // a key is down, 0 once it is let go. Written by the audio thread.
    int getUiNoteVelocity (int note) const { return juce::isPositiveAndBelow (note, 128) ? (int) uiNoteVelocity[(size_t) note].load (std::memory_order_relaxed) : 0; }
    // Goes up whenever one of those changes, so the strip can skip idle frames.
    juce::uint32 getUiNoteEpoch() const { return uiNoteEpoch.load (std::memory_order_relaxed); }
    // The MSEG's place in its cycle (0..1), for its playhead.
    float getMsegPhase() const { return msegPhaseDisplay.load(); }

    // Which optional modules the patch shows, Phase Plant style: a few by
    // default and a "+" to add more. Saved with the patch; the UI also always
    // shows a module that is in use.
    enum class Module { Oscillator = 0, Envelope, Lfo };
    static constexpr int defaultRevealMask = 0b111;
    // A new patch (Init) shows OSC 1 and 2, so PLAY's oscillators fit without scrolling; OSC 3 shows once it
    // is added or switched on (ilana 2026-10-22). A saved state and the other factory presets keep theirs.
    static constexpr int newPatchOscRevealMask = 0b011;
    bool isRevealed (Module kind, int index) const
    {
        return ((revealMasks[(size_t) kind].load() >> index) & 1) != 0;
    }
    void setRevealed (Module kind, int index, bool shouldShow)
    {
        auto& mask = revealMasks[(size_t) kind];
        const auto bit = 1 << index;
        mask.store (shouldShow ? (mask.load() | bit) : (mask.load() & ~bit));
        ++revealVersion;
    }
    // Bumped whenever a mask changes (including on patch load) so editors can
    // relayout without listening to every parameter.
    int getRevealVersion() const { return revealVersion.load(); }
    // Shows an oscillator and switches it on, as adding one should sound.
    void addOscillator (int index);
    // Adds it as that type: FM / DX7 starts as a sine operator tuned by
    // ratio on the Operator EG (a DX7 operator's init state).
    void addOscillator (int index, int mode);
    void removeOscillator (int index);
    // Oscillator slots (OscSlots.cpp, ilana 2026-10-26). A swap trades two
    // oscillators whole: settings, sample, FM routes, PATCH cable, vector
    // corners and the mod routes into them. moveOscillator puts a shown
    // oscillator at a position among the shown ones (the card order) and
    // returns its new slot; duplicateOscillator copies one into the first
    // hidden slot, right after it, and returns that slot (-1: all six are
    // shown). Message thread; wrap each in performEdit.
    void swapOscillators (int a, int b);
    int moveOscillator (int osc, int position);
    int duplicateOscillator (int from);

    // Copy, Paste and Init on one module (src/processor/ModuleClipboard.cpp):
    // its parameters by suffix, plus an oscillator's sample and patch table
    // and an LFO's drawn curve and steps. Pastes onto the same kind of
    // module, in any patch. Message thread; wrap each in performEdit.
    enum class ModuleKind { Oscillator, Filter, Envelope, Lfo };
    struct ModuleSnapshot
    {
        ModuleKind kind = ModuleKind::Oscillator;
        juce::String name; // "LFO 2", for the Paste item
        std::vector<std::pair<juce::String, float>> values; // suffix, normalised value
        LfoCurve curve;
        std::array<float, lfoDrawSteps> draw {};
        std::shared_ptr<SampleData> embedded, liveSample;
        juce::String samplePath, sampleOptions;
        int tableSlot = -1;
        std::shared_ptr<const WavetableDoc> table;
    };
    static juce::String modulePrefix (ModuleKind kind, int index);
    static juce::String moduleName (ModuleKind kind, int index);
    std::vector<juce::String> moduleParameterIds (ModuleKind kind, int index) const;
    ModuleSnapshot copyModule (ModuleKind kind, int index) const;
    bool pasteModule (const ModuleSnapshot& snapshot, int index);
    void initModule (ModuleKind kind, int index);
    // Sets an oscillator's type (OscMode). Leaving FM / DX7 drops what only
    // an operator has, so the new type's card shows everything that plays:
    // TUNING goes back to semitones, and a Wavetable leaves the Operator EG
    // for the Amp Env. Choosing FM / DX7 changes nothing else (the same
    // sound, with the operator's controls). Message thread; wrap it in
    // performEdit for one undo step.
    void setOscillatorMode (int index, int mode);
    bool isOscillatorShown (int index) const;

    // M5: sets the FM matrix and the operators' outputs to one of the
    // FmAlgorithms (adding the operators it needs). Existing routes keep
    // their amounts. Message thread.
    void applyFmAlgorithm (int index);
    // The same for the DX7's algorithm 1-32 (all six operators), which the
    // FM page then names.
    void applyDx7Algorithm (int number);
    // The algorithm the current routing matches, or -1.
    int findMatchingFmAlgorithm() const;
    // The DX7 algorithm (1-32) the routing is, or 0: six operators on, the
    // DX7's routes and carriers, its feedback operator or none.
    int findMatchingDx7Algorithm() const;
    // [source][target] FM parameter id, 0-based.
    static juce::String fmRouteId (int source, int target);
private:
    void applyFmRouting (const FmAlgorithms::Algorithm& algorithm, int dx7Number);
public:
    // An operator's sounding ratio after SNAP (for display).
    double getSnappedRatio (int osc) const;
    // Added to the patch, or routed in the matrix (message thread).
    bool isLfoShown (int index) const;
    // Colours for LFO cards and chips: the first four as before, the rest
    // around the hue wheel.
    static juce::Colour lfoColour (int index);

    // Colour coding: the colour this patch gives a section
    // (SectionColours keys; 0 = its default), saved with the patch.
    juce::uint32 getSectionColour (int key) const
    {
        return key >= 0 && key < SectionColours::count ? sectionColours[(size_t) key].load() : 0u;
    }
    void setSectionColour (int key, juce::uint32 argb)
    {
        if (key >= 0 && key < SectionColours::count)
        {
            sectionColours[(size_t) key].store (argb);
            ++sectionColourVersion;
        }
    }
    SectionColours::Table getSectionColours() const
    {
        SectionColours::Table table {};
        for (int key = 0; key < SectionColours::count; ++key)
            table[(size_t) key] = sectionColours[(size_t) key].load();
        return table;
    }
    int getSectionColourVersion() const { return sectionColourVersion.load(); }

    bool loadUserWavetable (int slot, const juce::File& file,
                            Wavetable::LoadMode mode = Wavetable::LoadMode::Automatic);

#if ILANA_FX
    static constexpr bool isEffectBuild = true;
#else
    static constexpr bool isEffectBuild = false;
#endif
    // M7.5: the input's level and envelope for the INPUT page (0..1).
    float getInputLevel() const { return inputLevelDisplay.load(); }
    float getInputEnvelope() const { return inputEnvDisplay.load(); }

    // M7.4 patch tables (message thread). setUserTable builds the table from
    // the doc and swaps it in; the doc is saved with the patch.
    bool setUserTable (int slot, const WavetableDoc& doc);
    // Gives a slot its default table back (what a listen in the table browser leaves behind).
    // The slot's doc; a slot never edited gives its default (factory) table.
    WavetableDoc getUserTableDoc (int slot) const;
    // The same doc by identity (null: default), for the wavetable editor to
    // notice that a load or an undo changed the table under it.
    std::shared_ptr<const WavetableDoc> getUserTableDocPtr (int slot) const;
    // Counts patch loads (a preset, a session or host state): work started
    // for the old patch checks it before applying its result.
    unsigned getPatchGeneration() const { return patchGeneration.load(); }
    bool isUserSlotEdited (int slot) const;
    // An unedited slot that no oscillator plays, or -1.
    int findFreeUserSlot() const;
    // Set when a saved table could not be restored (see WavetableDoc).
    juce::String getTableNotice() const;
    int getTableNoticeVersion() const { return tableNoticeVersion.load(); }
    void clearTableNotice();
    // fromUser: a file the player chose or dropped; it replaces a factory
    // sample source (the SOURCE menu goes back to the loaded sample).
    bool loadUserSample (int oscIndex, const juce::File& file, bool fromUser = false);

    // Roadmap step 42: the sample browser's audition. startSamplePreview decodes
    // the file on the calling (message) thread, up to previewSeconds of it, sets
    // a gain from its RMS so every preview plays at the same level, and hands it
    // to the audio thread, which mixes it into the output after the effects (no
    // lock, no allocation there). It returns false for a file it cannot read.
    // stopSamplePreview fades it out. Nothing of this reaches a preset.
    bool startSamplePreview (const juce::File& file);
    void stopSamplePreview();
    bool isSamplePreviewPlaying() const noexcept { return previewPlaying.load(); }
    // The gain the running preview was given (1 with none).
    float getSamplePreviewGain() const noexcept { return previewGain.load(); }

    // Portable sample presets (roadmap step 31; src/processor/SamplePortability.cpp).
    // Embedded audio is held to this many bytes of 16-bit samples in the patch.
    static constexpr juce::int64 maxEmbeddedSampleBytes = 64 * 1024 * 1024;
    struct MissingSample
    {
        int osc = 0;
        juce::String path;         // the file the patch names
        juce::StringArray partial; // an SFZ that loaded but could not find these audio files
    };
    juce::String getSamplePath (int oscIndex) const; // empty: embedded, factory or none
    // Roadmap step 38: a SoundFont's preset by bank and program, kept with
    // the patch. Returns false if it couldn't load.
    bool selectSoundFontPreset (int oscIndex, int bank, int program);
    // Roadmap step 32: cuts the oscillator's plain sample into slices on
    // their own keys (mode off puts the whole sample back): one undo step,
    // kept with the patch. False for a multisample or no sample.
    bool setSampleSlices (int oscIndex, const SampleSlicer::Settings& settings, const juce::String& undoName);
    SampleSlicer::Settings getSampleSlices (int oscIndex) const;
    // The preset an SF2 oscillator plays (bank, program), or -1, -1 for the first.
    std::pair<int, int> getSoundFontPreset (int oscIndex) const;
    bool isSampleFromFile (int oscIndex) const;
    std::vector<MissingSample> getMissingSamples() const;
    // True once per change of the missing list, while it holds something (the editor's prompt).
    bool takeMissingSamplesPrompt();
    // Keep the audio in the patch instead of the file's path (message thread).
    bool embedSample (int oscIndex, juce::String& error);
    int embedAllSamples (juce::String& error); // how many were embedded; error holds the first refusal
    struct CollectResult
    {
        int oscillators = 0, files = 0;
        juce::StringArray missing;
        juce::String error;
        bool cancelled = false;
    };
    // Copies every sample file the patch plays into `folder` (an SFZ with its
    // audio, in a folder of its own) and points the patch at the copies. Any
    // thread; progress gets 0..1 and returns false to cancel.
    CollectResult collectSamples (const juce::File& folder, const std::function<bool (float)>& progress = {});
    bool relinkSample (int oscIndex, const juce::File& file);
    // Files in `folder` that have a missing sample's name (any thread). Relink each with relinkSample.
    std::vector<std::pair<int, juce::File>> findMissingSampleFiles (const juce::File& folder, bool recursive,
                                                                    const std::function<bool()>& shouldStop = {}) const;
    // Roadmap step 43: why the last loadUserSample failed, in a sentence for
    // the person (message thread).
    juce::String getLastSampleError() const { return lastSampleError; }
    // Roadmap step 44: writes an oscillator's sample (a resampled sound too)
    // as a 24-bit WAV at its own rate. playedRange: START to END as the card
    // plays it (REVERSE and the fades applied, pitch not); else all of it. A
    // multisample writes its shown region (the one under middle C).
    bool exportSampleWav (int oscIndex, const juce::File& target, bool playedRange, juce::String& error) const;

    // Roadmap step 36: the zone map. The zones the oscillator plays (a copy;
    // empty for a plain sample).
    std::vector<SampleZone> getSampleZones (int oscIndex) const;
    // Replaces the zones (their audio is shared, not copied): one undo step.
    // A sample loaded from a file keeps its file and saves the map as edits
    // to it; a sample kept in the patch (a resample) keeps the new zones.
    bool setSampleZones (int oscIndex, std::vector<SampleZone> zones, const juce::String& undoName);
    // Adds a zone playing an audio file; a plain sample becomes the map's
    // first zone (over every key, rooted at middle C).
    bool addSampleZone (int oscIndex, const juce::File& file, int loKey, int hiKey, int loVel, int hiVel, int rootKey, juce::String& error);
    // A file's audio as a plain sample (step 43's messages in error, what
    // was left out in loadWarning); null on failure.
    static std::shared_ptr<SampleData> decodeAudioFile (const juce::File& file, juce::String& error);
    // The audio thread calls this at the top of each block's rack (a staged slot swap goes in here, FX-14).
    void beginFxBlock() noexcept;
    // The FX rack alone over a buffer (the audio path calls it; public for the tests).
    void processEffects (juce::AudioBuffer<float>& buffer);
    void processEffectsUnlessAsleep (juce::AudioBuffer<float>& buffer);
    void processEffectsParallel (juce::AudioBuffer<float>& buffer);
    void runFxSlot (int slot, juce::AudioBuffer<float>& buffer);
    bool isFxSlotActive (int index);
    bool fxSlotRuns (int index);
    void clearFxSlotMeters (int index);
    // Roadmap step A: the rack as a patch graph (PatchGraphState.h,
    // rackGraph) and the rack run through a compiled graph instead of the
    // fixed loop. Not on the audio path yet: the tests and ilanaFxCpuBench
    // use them to show a graph renders the fixed rack sample for sample and
    // what it costs; step 17 switches the audio path over.
    // ignoreBypass: the cables as if no slot were bypassed (turning the patcher on).
    PatchGraph::Graph getFxRackGraph (bool ignoreBypass = false);
    void processEffectsGraph (juce::AudioBuffer<float>& buffer, PatchGraph::Runner& runner);

    //==========================================================================
    // Roadmap step 17: the FX patcher and effect instances (FxPatch.h,
    // FxPatcher.cpp). Message thread unless noted; the editor wraps each
    // user action in performEdit, so the parameters and this data undo
    // together.
    //==========================================================================
    // Roadmap step 16: reroutable oscillators (OscRouting.cpp). The cables
    // live in parameters (route_cables, cable_*), so presets, undo and chain
    // files carry them like any other setting. Message thread; the caller
    // wraps a user action in performEdit.
    bool areRouteCablesOn() const;
    // The cables as saved (the default chain until the first cable edit).
    VoiceRouting::Patch getRoutePatch() const;
    // The fixed chain (the _route menus, SERIAL / PARALLEL, WEST's
    // position) as cables: what the page shows while the cables are off.
    VoiceRouting::Patch getChainAsCables() const;
    // Writes the cables and the switch (and switches WEST on when a cable
    // now goes into it).
    void setRouteCables (const VoiceRouting::Patch& patch, bool on);

    FxPatchData::Patch getFxPatch() const;
    // The same, shared and not copied (a patch is never changed once published).
    std::shared_ptr<const FxPatchData::Patch> getFxPatchShared() const;
    void setFxPatch (const FxPatchData::Patch& patch);
    bool isFxPatcherOn() const;
    // On: the slots become the patcher graph's nodes; the first time, the
    // graph is the rack as it is (so turning it on changes nothing heard).
    void setFxPatcherOn (bool on);
    // A new effect in the first empty slot, wired in after the last one (on
    // the graph, in front of OUTPUT). A type the rack already has becomes a
    // new instance of it: a copy of its settings, on its own engine, with its
    // card in front. Returns the slot index, or -1 when every slot is in use.
    int addFxEffect (int type);
    // Takes a slot's effect out (its node too, the cables around it joined up).
    void removeFxEffect (int slotIndex);
    // Puts this instance's settings on its type's controls (the one in front
    // before keeps its settings as its own copy).
    void focusFxInstance (int slotIndex);
    // Whether this slot has its own engine and settings.
    bool isFxInstance (int slotIndex) const;
    // Whether this slot's card holds its type's controls (false: another
    // instance's card is in front and this one plays its own copy).
    bool isFxSlotInFront (int slotIndex) const;
    // A duplicate that shared its type's settings (older patches) gets its
    // own engine and its own copy of the settings.
    void makeFxInstance (int slotIndex);
    // Swaps two slots, with their instance data, engines and graph nodes.
    void swapFxSlots (int a, int b);
    // One of an instance's own settings (a slot whose card isn't in front:
    // the RACK shows it on the instance's own card). False: no such value.
    bool getFxInstanceValue (int slotIndex, const juce::String& id, float& value) const;
    void setFxInstanceValue (int slotIndex, const juce::String& id, float value);
    // The graph the patcher shows (a copy) and a change to it from the view.
    PatchGraph::Graph getFxPatchGraph() const;
    void setFxPatchGraph (const PatchGraph::Graph& graph);
    // The patcher's shortest loop delay in milliseconds (one render chunk).
    float getFxLoopLatencyMs() const;
    // The loop guard on the loop cable from -> to (node id, port): 1 while
    // it isn't holding the loop, under 1 while the loop rings by itself and
    // is held at 0 dB (PatchGraphRunner.h).
    float getFxLoopGuard (std::uint32_t fromNode, int fromPort, std::uint32_t toNode, int toPort) const;
    // The patcher's LFO boxes (FxLfo.h, FxPatch.h). Message thread; the
    // editor wraps each action in performEdit. addFxLfo returns the new box's
    // number, or 0 when all FxLfo::maxLfos are in.
    int addFxLfo (float x, float y);
    void removeFxLfo (int id);
    void setFxLfoSettings (int id, const FxLfo::Settings& settings);
    void moveFxLfo (int id, float x, float y);
    // A cable from an LFO box to a slot's box; taking it out drops the knobs it moved.
    void setFxLfoCable (int id, int slotIndex, bool connected);
    // Moves (amount != 0 with assign) or stops moving one knob of the slot's effect.
    void assignFxLfo (int id, int slotIndex, const juce::String& param, bool assign, float amount = 0.5f);
    void setFxLfoAmount (int id, int slotIndex, const juce::String& param, float amount);
    // The knobs an LFO may move on a slot: (parameter ID or "mix", name),
    // the continuous ones only.
    std::vector<std::pair<juce::String, juce::String>> getFxLfoTargets (int slotIndex) const;
    // The parameter ID a route's param names on a slot ("mix" is the slot's MIX).
    static juce::String fxLfoParamId (int slotIndex, const juce::String& param);
    // What an LFO box is doing now (display): its shape's value -1..1 and phase 0..1.
    float getFxLfoValue (int id) const;
    float getFxLfoPhase (int id) const;
    // Tests: the offset (0..1 range) an LFO puts on a slot's parameter in the last chunk.
    float getFxLfoOffset (int slotIndex, const juce::String& parameterId) const;

    // Tests: the rack's plan (what the audio thread runs) is current.
    bool isFxRackPlanCurrent();
    // Tests / tools: compile now what the audio thread would wait for.
    void compileFxPlansNow();
    // Puts audio on an oscillator's sample slot. An embedded sample (a
    // bounce) is saved inside the patch; a file-backed one by its path.
    void setUserSample (int oscIndex, std::shared_ptr<SampleData> data, const juce::String& path);
    bool isSampleEmbedded (int oscIndex) const;
    const SampleData* getSampleForOsc (int oscIndex) const;
    // Step 29: DETECT. Finds the pitch of a plain sample and sets its ROOT; the
    // message says what happened (a multisample, a noise, or the note found).
    juce::String detectSampleRoot (int oscIndex);
    void useLoadedSample (int oscIndex, bool fromUser);
    // The same, kept alive by the pointer (the stretch worker's view).
    std::shared_ptr<const SampleData> getSharedSampleForOsc (int oscIndex) const;
    // TIME's engines: copies still rendering, for the sample card.
    int getStretchRendersPending() const { return stretchRenderer != nullptr ? stretchRenderer->pendingCount() : 0; }
    void flushAsyncUpdates();

    // M8.6: resample to oscillator. The patch plays one note on a copy of
    // the processor, off the audio thread, and the result lands on an
    // oscillator: as its sample (Sample mode) or resynthesised into a patch
    // wavetable (Wavetable mode).
    struct BounceRequest
    {
        int targetOsc = 0;
        bool toTable = false;
        bool withFx = true;         // false: the voice alone, every effect off
        bool muteOthers = true;     // the other oscillators switched off after
        int note = 60, velocity = 100;
        double holdSeconds = 2.0, tailSeconds = 2.0;
        // Roadmap step 41: a multisample. One render per note on the grid
        // (noteLow, noteLow + spacing, ... up to noteHigh) and per velocity
        // layer, each becoming a zone of one embedded multisample on the
        // target oscillator. All the renders share one gain, so the
        // loudness between notes and layers is what the patch played.
        bool multi = false;
        int noteLow = 36, noteHigh = 84, spacing = 3, velocityLayers = 1;

        // The root notes of the grid and the velocities of the layers.
        std::vector<int> multiNotes() const;
        std::vector<int> multiVelocities() const;
        int numCaptures() const { return (int) (multiNotes().size() * multiVelocities().size()); }
    };
    // Embedded multisamples stay under this many bytes of 16-bit audio (the
    // patch carries them): about 3 minutes of mono at 48 kHz.
    static constexpr juce::int64 maxMultisampleBytes = 17 * 1024 * 1024;
    enum class BounceState { Idle, Rendering, Done, Failed };
    // Message thread. False if a bounce is already running.
    bool startBounce (const BounceRequest& request);
    BounceState getBounceState() const { return bounceState.load(); }
    float getBounceProgress() const { return bounceProgress.load(); }
    juce::String getBounceMessage() const;
    // The render itself: the state playing one note, trimmed and
    // normalised. Null if silent or cancelled. Any thread.
    static std::shared_ptr<SampleData> renderBounce (const juce::ValueTree& state, const BounceRequest& request,
                                                     std::atomic<float>* progress = nullptr,
                                                     const std::atomic<bool>* cancel = nullptr);
    static juce::AudioBuffer<float> playOneNote (const juce::ValueTree& state, int note, int velocity, double holdSeconds, double tailSeconds,
                                                 const std::function<void (float)>& progress, const std::atomic<bool>* cancel);
    // Puts a render on the target oscillator (message thread).
    bool applyBounce (const BounceRequest& request, std::shared_ptr<SampleData> audio, juce::String& message);

    // Scala microtuning, saved in the patch (TuningState). Loading a scale
    // switches tuning_on on; reset goes back to 12-TET and switches it off.
    // Message thread; false (and an error) for a malformed file.
    bool loadTuningScale (const juce::String& sclText, juce::String& error);
    bool loadTuningMapping (const juce::String& kbmText, juce::String& error);
    void resetTuning();
    const TuningState& getTuningState() const { return tuningState; }
    // The clip sequencer's clips, saved in the patch (ClipState). Message thread.
    ClipState& getClipState() { return clipState; }
    void clipsEdited() { ++dataEpoch; }
    // The beat the clip has reached (0 to its length), or -1 when it isn't playing.
    float getClipPlayhead() const { return clipPlayhead.load(); }
    // MTS-ESP: a tuning master in the session overrides the Scala tuning.
    bool isMtsEspConnected() const { return mtsEsp.hasMaster(); }
    juce::String getMtsEspScaleName() const { return juce::String::fromUTF8 (mtsEsp.scaleName()); }

    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;

private:
    TuningState tuningState;
    ClipState clipState;

    // The patch data that isn't a parameter, as one undo action sees it
    // (beginEdit / endEdit; State.cpp). Remaps that are off hold the
    // straight line.
    struct PatchData
    {
        std::shared_ptr<const FxPatchData::Patch> fx;
        // Patch tables and samples travel with the patch, as do the macro
        // names and CCs, the reveal masks and the tuning (their text).
        std::array<std::shared_ptr<const WavetableDoc>, (size_t) numUserSlots> tables;
        std::array<std::shared_ptr<SampleData>, (size_t) OscillatorIds::count> embedded;
        std::array<juce::String, (size_t) OscillatorIds::count> samplePaths;
        // Step 36: a file's edited zone map (and SF2 preset) and the sample
        // as it played, so undo puts it back without reading the file again.
        std::array<juce::String, (size_t) OscillatorIds::count> sampleOptions;
        std::array<std::shared_ptr<SampleData>, (size_t) OscillatorIds::count> liveSamples;
        std::array<juce::String, (size_t) Mod::numMacros> macroNames;
        std::array<int, (size_t) Mod::numMacros> macroCcs {};
        std::array<int, 3> reveal {};
        juce::String tuningScale, tuningMapping;
        std::array<std::array<float, lfoDrawSteps>, (size_t) numLfos> draws {};
        std::array<LfoCurve, (size_t) numLfos> curves;
        std::array<LfoCurve, (size_t) Mod::maxSlots> remaps;
        std::shared_ptr<const ClipState::Clips> clips;
    };
    struct PatchDataEdit;
    float readNormalised (const juce::String& id) const;
    void writeNormalised (const juce::String& id, float value);
    PatchData capturePatchData() const;
    // Puts back the parts of data that differ from reference.
    void restorePatchData (const PatchData& data, const PatchData& reference);
    static bool samePatchData (const PatchData& a, const PatchData& b);
    std::optional<PatchData> pendingEditData; // between beginEdit and endEdit
    mutable juce::uint64 hashedDataEpoch = ~(juce::uint64) 0;
    mutable juce::int64 cachedDataHash = 0;
    // A table doc's content hash, kept while the slot holds the same doc.
    mutable std::array<std::pair<std::shared_ptr<const WavetableDoc>, juce::uint64>, (size_t) numUserSlots> docHashes;
    juce::File loadedPresetFile;
    std::atomic<unsigned> patchGeneration { 0 };
    unsigned bounceGeneration = 0; // the patch a running bounce was started on

    MtsEspClient mtsEsp;
    Tuning mtsTuning; // filled from the master each block while one is connected
    void handleAsyncUpdate() override;

    // M8.6 bounce: the render thread hands its result over through these
    // (under stateLock) and the async update applies it.
    class BounceThread;
    std::unique_ptr<BounceThread> bounceThread;
    std::atomic<BounceState> bounceState { BounceState::Idle };
    std::atomic<float> bounceProgress { 0.0f };
    BounceRequest pendingBounce;
    std::shared_ptr<SampleData> bounceResult;
    bool bounceReady = false;
    juce::String bounceMessage;
    juce::String lastSampleError; // step 43, message thread
    // Step 36 / 38: what a file-backed sample needs besides its path (an SF2
    // preset, an edited zone map), as XML of a "SampleOptions" tree whose
    // path property names the file it belongs to. Under stateLock.
    std::array<juce::String, (size_t) OscillatorIds::count> sampleOptions;
    juce::String getSampleOptions (int oscIndex) const;
    void setSampleOptions (int oscIndex, const juce::String& options);
    void retargetSampleOptions (int oscIndex, const juce::String& newPath); // the file moved (collect, relink)
    // Puts a saved zone map back on a freshly loaded file's sample.
    std::shared_ptr<SampleData> applyZoneOptions (std::shared_ptr<SampleData> loaded, const juce::ValueTree& options, const juce::File& file);
    static juce::ValueTree encodeSample (const SampleData& data);
    static std::shared_ptr<SampleData> decodeSample (const juce::ValueTree& tree, int depth = 0);

    struct ParamCacheEntry
    {
        const char* id = nullptr;
        std::uint32_t hash = 0;
        const std::atomic<float>* value = nullptr;
        int index = -1;        // in paramCacheIds (an FX instance's own values are kept by it)
        int destination = -1;
        bool discrete = false; // a choice, int or bool: read back rounded
        float fallback = 0.0f; // the default, read in place of a non-finite value
    };

    // A parameter ID that remembers where its value lives: the first read
    // looks it up, later reads go straight to it. Assigned and passed
    // around like the juce::String it wraps.
    class ParamRef
    {
    public:
        ParamRef() = default;
        ParamRef (const juce::String& newId) : id (newId) {}
        ParamRef (const char* newId) : id (newId) {}
        ParamRef (const ParamRef& other) : id (other.id) {}

        ParamRef& operator= (const ParamRef& other)
        {
            id = other.id;
            entry.store (nullptr, std::memory_order_relaxed);
            return *this;
        }

        const char* toRawUTF8() const { return id.toRawUTF8(); }
        operator const juce::String&() const { return id; }
        operator juce::StringRef() const { return id; }

        juce::String id;
        mutable std::atomic<const ParamCacheEntry*> entry { nullptr };
    };

    float getParam (const char* id) const;
    float getParam (const ParamRef& ref) const;
    // The stored value without modulation (as apvts.getRawParameterValue).
    float getRawParam (const ParamRef& ref) const;
    const ParamCacheEntry* findParam (const char* id) const;
    float readParam (const ParamCacheEntry& entry) const;
    // The calling thread's FX instance values (see readParam); returns the old ones.
    static const float* swapFxParamValues (const float* values) noexcept;
    const Wavetable* getTableForChoice (int choiceIndex) const;
    void renderLfos (int numSamples, const juce::MidiBuffer& midiMessages);
    float staticSourceValue (int sourceIndex) const;
    juce::String captureFxChain();
    juce::ValueTree buildFullState();
    void applyFullState (const juce::ValueTree& state);
    void applyFxChain (const juce::String& state);
    void processChunk (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages);
    void updateLatency();
    juce::MidiBuffer chunkMidi;
    void processArpeggiator (juce::MidiBuffer& midiMessages, int numSamples, juce::MidiBuffer& output);
    int selectArpNote (int mode, int octaves);

    IlanaSynth synth;

    juce::MidiBuffer midiForSynth;
    juce::MidiBuffer generatedMidi;
    NoteSpray noteSpray;
    std::unique_ptr<SpectralCache> spectralCache;
    std::unique_ptr<StretchRenderer> stretchRenderer; // TIME's engines: rendered copies of samples
    juce::Array<int> arpHeldNotes;
    juce::Array<int> arpChordActive;
    juce::Array<int> arpChordNotes;
    juce::Random arpRandom;
    int arpStepIndex = 0;
    int arpDirection = 1;
    int arpCounter = 0;
    int arpGateRemaining = 0;
    int arpActiveNote = -1;
    bool arpWasEnabled = false;
    bool arpHostWasPlaying = false;
    // The arp keeps each key's MIDI channel (it plays on the last key's
    // channel; switching it on releases the pass-through voices) and follows the sustain pedal: keys let go while it is down stay held.
    std::array<juce::uint8, 128> arpHeldChannel {};
    std::array<bool, 128> arpSustainedKey {};
    int arpChannel = 1, arpActiveChannel = 1;
    bool arpPedalDown = false;
    // M7.1: probability sequencer and Euclid state. The sequencer has its
    // own random generator, so the arp's sequence is unchanged.
    juce::Random pseqRandom { 16180 };
    long long engineStepCount = 0;
    std::array<ParamRef, 16> pseqChanceIds, pseqRangeIds, pseqRatchetIds;
    // The arp's step lanes (review 6): velocity, gate and transpose per step
    // over ARP STEPS steps. At their defaults (100, 100 %, 0) every step plays
    // as before.
    std::array<ParamRef, 16> arpVelocityIds, arpLengthIds, arpPitchIds;
    ParamRef arpStepsRef { "arp_steps" };
    void addEuclidExciterHits (juce::MidiBuffer& midi, int numSamples);
    // Clip sequencer: plays the current clip into the synth's MIDI.
    void processClip (juce::MidiBuffer& midi, int numSamples);
    struct ClipActiveNote { int note; double endBeat; };
    ParamRef clipOnRef { "clip_on" }, clipIndexRef { "clip_index" }, clipModeRef { "clip_mode" };
    juce::MidiBuffer clipScratch;
    std::vector<ClipActiveNote> clipActive;
    juce::Array<int> clipHeld; // keys down, in press order (Key transpose)
    std::array<juce::uint8, 128> clipHeldVelocity {}; // their velocities, to give them back
    double clipBase = 0.0;     // the beat at the block's first sample
    double clipExpected = 0.0; // where the host's beat should be next block
    bool clipWasOn = false, clipRunning = false, clipUsedHost = false;
    int clipLastIndex = -1, clipLastMode = -1;
    std::atomic<float> clipPlayhead { -1.0f };
    int arpRatchetNote = 0, arpRatchetsLeft = 0, arpRatchetInterval = 0, arpRatchetCounter = 0;
    double euclidExciterPhase = 0.0;
    long long euclidExciterLastStep = -1;


    std::vector<std::shared_ptr<Wavetable>> userTables;
    std::array<std::shared_ptr<Wavetable>, (size_t) (numUserSlots * 3)> retiredTables;
    std::array<int, (size_t) numUserSlots> retiredTableIndex {};
    void resetUserTableToDefault (int slot);
    void swapUserTable (int slot, std::shared_ptr<Wavetable> table);
    std::array<std::shared_ptr<const WavetableDoc>, (size_t) numUserSlots> userTableDocs; // null: default
    juce::String tableNotice;
    std::atomic<int> tableNoticeVersion { 0 };
    mutable juce::SpinLock tableLock;

    std::array<std::array<ParamRef, 11>, OscillatorIds::count> stringParamIds;
    std::array<std::array<ParamRef, 6>, OscillatorIds::count> bowBuzzIds;
    std::array<std::array<ParamRef, 4>, OscillatorIds::count> keysParamIds;
    std::array<std::array<ParamRef, 2>, OscillatorIds::count> electricParamIds;
    std::array<std::array<ParamRef, 2>, OscillatorIds::count> feedbackParamIds;

    // M7.5 audio input (ilanaSynth FX). The instrument has no input, so all
    // of this stays silent there.
    void captureLiveInput (juce::AudioBuffer<float>& buffer);
    void prepareLiveInput (int numSamples, int factor, juce::MidiBuffer& midi);
    std::array<ParamRef, OscillatorIds::count> grainLiveIds;
    ParamRef inGainRef { "in_gain" }, inDryRef { "in_dry" }, inBodyRef { "in_body" }, inStringsRef { "in_strings" },
        inTriggerRef { "in_trigger" }, inThresholdRef { "in_threshold" }, inNoteRef { "in_note" },
        inAttackRef { "in_attack" }, inReleaseRef { "in_release" };
    ParamRef tuningOnRef { "tuning_on" };
    ParamRef masterRef { "master" }, outputTrimRef { "output_trim" };
    ParamRef fxRoutingRef { "fx_routing" };
    ParamRef multiCoreRef { "multi_core" };
    ParamRef sustainVoicesRef { "sustain_voices" };
    ParamRef midiProgramChangeRef { "midi_program_change" }, msegLoopRef { "mseg_loop" };
    // The Airwindows category modules (FX types 32-41), one engine each;
    // their refs: algo, p1..p5, mix.
    static constexpr int numAwCategories = 10;
    static std::array<std::array<ParamRef, 7>, numAwCategories> makeAwCategoryRefs()
    {
        std::array<std::array<ParamRef, 7>, numAwCategories> refs;
        for (int c = 0; c < numAwCategories; ++c)
        {
            const auto prefix = juce::String ("fx_") + airwindows::categoryModules()[(size_t) c].id;
            refs[(size_t) c][0] = ParamRef (prefix + "_algo");
            for (int k = 0; k < 5; ++k)
                refs[(size_t) c][(size_t) k + 1] = ParamRef (prefix + "_p" + juce::String (k + 1));
            refs[(size_t) c][6] = ParamRef (prefix + "_mix");
        }
        return refs;
    }
    juce::AudioBuffer<float> liveDry;       // the input as it came in (for DRY)
    std::vector<float> liveVoice, liveEnvVoice; // at the voice rate, after INPUT GAIN
    SampleData liveHistory;                 // the last few seconds, for live grains
    int liveHistoryWrite = 0;
    float liveLast = 0.0f, liveEnvState = 0.0f;
    bool liveGateOpen = false;
    int liveGateNote = -1;
    int liveInputSamples = 0;               // valid samples in liveDry this block
    // DRY is delayed by the reported latency so it lines up with the
    // oversampled wet path (a blend would comb-filter otherwise).
    void delayLiveDry (int numSamples);
    juce::AudioBuffer<float> dryDelayRing;
    int dryDelayWrite = 0, dryDelaySamples = 0;
    std::atomic<bool> liveRetrigger { false }; // a patch loaded: restart the drone
    // A patch loaded: stop the old one's voices and effect tails at the next
    // block, easing from the last output sample to silence rather than
    // stepping to it.
    std::atomic<bool> patchCut { false };
    std::atomic<bool> easeStep { false };
    // VoiceParams::exciterLevels; saved as the state's "exciterLevels".
    std::atomic<int> exciterLevels { Voice::currentExciterLevels };
    void updateExciterLevelMatch (int savedLevels);

public:
    // Loads one DX7 voice as the current patch (named after it): ordinary
    // parameters, the operators on the Operator EG (Dx7Presets.h).
    void loadDx7Voice (const Dx7::Voice& voice, const juce::String& name);
    // A .syx bank (32 voices) or single voice, saved as user presets under
    // DX7/<file name>/. Returns how many were imported; message says why not.
    int importDx7File (const juce::File& file, juce::String& message);

private:
    float lastOutput[2] {}, declick[2] {};
    bool fxAsleep = false;
    int fxSilentSamples = 0;
    std::array<std::array<std::array<float, 2>, 2>, 2> dcBlock {}; // [before/after the effects][channel][x, y]
    void cutPatchTails();
    std::atomic<float> inputLevelDisplay { 0.0f }, inputEnvDisplay { 0.0f };
    struct OscCoreIds
    {
        ParamRef on, table, frame, level, pan, semi, fine, unison, detune, spread, spectral, spectralAmount, chord, out, frameBlend;
    };
    std::array<OscCoreIds, OscillatorIds::count> oscCoreIds;
    VoiceParams voiceParamsShared; // this block's voice parameters, built in place (idle voices read it at note start)

    // Parameter IDs built once, so the audio thread never allocates strings.
    struct ModSlotIds { ParamRef src, dst, amt, curve, polarity, aux, bypass, stereo; };
    struct ModSlotRaw
    {
        std::atomic<float>* src = nullptr;
        std::atomic<float>* dst = nullptr;
        std::atomic<float>* amt = nullptr;
        std::atomic<float>* curve = nullptr;
        std::atomic<float>* polarity = nullptr;
        std::atomic<float>* aux = nullptr;
        std::atomic<float>* bypass = nullptr;
        std::atomic<float>* stereo = nullptr;
    };
    std::array<ModSlotRaw, (size_t) Mod::maxSlots> modSlotRaw;
    std::array<ModSlotIds, (size_t) Mod::maxSlots> modSlotIds;
    struct LfoIds
    {
        ParamRef shape, rate, sync, div, retrig, phase, key, physA, physB, kick;
        std::array<ParamRef, 16> steps;
        std::array<ParamRef, LfoSimInfo::numParams> sim;
        ParamRef smooth, axis, trigger, loop, seed, stereo, fire;
    };
    struct OscShapeIds { ParamRef warp, warpAmount, unisonMode, unisonBlend, route, frameSpread, warpSpread, scale, scaleRoot; };
    std::array<OscShapeIds, OscillatorIds::count> oscShapeIds;
    // M5/M6 operator and phase-distortion settings.
    struct OperatorIds
    {
        ParamRef tune, ratio, snap, fixedHz, keyLevel, feedbackType, warp2, warp2Amount, pdEnv, pdEnvAmount;
    };
    std::array<OperatorIds, OscillatorIds::count> operatorIds;
    std::array<ParamRef, OscillatorIds::count> fmNoiseIds;
    // DAHDSR extras and rate key scaling for ENV 1..16.
    struct EnvelopeExtraIds { ParamRef delay, hold, keyRate, attackCurve, decayCurve, releaseCurve; };
    std::array<EnvelopeExtraIds, 16> envelopeExtraIds;
    std::array<ParamRef, Mseg::numPoints> msegLevelIds, msegTimeIds;
    // ENV 6..16's ADSR, curve and velocity.
    struct ExtraEnvIds { ParamRef attack, decay, sustain, release, curve, velocity; };
    std::array<ExtraEnvIds, 11> extraEnvIds;
    // Every FM matrix cell ([source][target]), each oscillator's amp
    // envelope choice and sample source.
    std::array<std::array<ParamRef, OscillatorIds::count>, OscillatorIds::count> fmMatrixIds;
    std::array<ParamRef, OscillatorIds::count> oscAmpEnvIds, sampleFactoryIds;
    // The Operator EG's parameters, in OperatorEg::operatorFields() and
    // voiceFields() order.
    std::array<std::array<ParamRef, 17>, OscillatorIds::count> operatorEgIds;
    std::array<ParamRef, 15> operatorEgVoiceIds;
    ParamRef operatorEgKeyOffsetId { OperatorEg::keyOffsetId };

    std::array<LfoIds, (size_t) numLfos> lfoIds;
    std::array<int, (size_t) numLfos> lfoPreviousShapes = [] { std::array<int, (size_t) numLfos> shapes {}; shapes.fill (-1); return shapes; }();
    // Which LFOs a mod slot uses: LFO 5-16 only render in full when routed.
    std::array<bool, (size_t) numLfos> lfoRouted {};
    struct FxSlotIds { ParamRef type, bypass, solo, mix, band; };
    std::array<FxSlotIds, (size_t) numFxSlots> fxSlotIds;
    std::array<ParamRef, 16> tapStepIds;
    std::array<ParamRef, 16> gateStepIds;
    std::array<std::array<ParamRef, 7>, OscillatorIds::count> sampleParamIds;
    std::array<std::array<ParamRef, 3>, OscillatorIds::count> sampleTempoIds; // sync, bpm, stretch
    std::array<ParamRef, OscillatorIds::count> sampleRootIds; // step 29
    std::array<std::array<ParamRef, 4>, OscillatorIds::count> sampleLoopIds;  // own loop on, start, end (step 27), crossfade (step 28)
    std::array<std::array<ParamRef, 8>, OscillatorIds::count> samplePlayIds;  // the sampler steps: playback mode (33), ping-pong (34), random start and its attack snap (35), quality (39), outside zones (36), round robin and velocity blend (37)
    SampleRoundRobin sampleRoundRobin; // step 37: shared by the voices
    std::array<std::array<ParamRef, 8>, OscillatorIds::count> grainParamIds;

    static constexpr int numSampleOscs = OscillatorIds::count;
    std::vector<std::shared_ptr<SampleData>> sampleSlots;
    std::array<std::shared_ptr<SampleData>, 5> factorySamples;
    std::array<std::array<std::shared_ptr<SampleData>, 3>, (size_t) numSampleOscs> retiredSamples;
    std::array<int, (size_t) numSampleOscs> retiredIndex {};
    mutable juce::SpinLock sampleLock;

    // Sample browser preview (step 42). The message thread fills previewPending
    // under previewLock and bumps previewSerial; the audio thread try-locks to
    // pick it up. Clips it replaced wait in previewRetired, so the audio thread
    // never frees one.
    struct PreviewClip
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate = 44100.0;
        float gain = 1.0f;
    };
    void mixSamplePreview (juce::AudioBuffer<float>& buffer) noexcept;
    mutable juce::SpinLock previewLock;
    std::shared_ptr<PreviewClip> previewPending;
    std::array<std::shared_ptr<PreviewClip>, 4> previewRetired;
    int previewRetiredIndex = 0;
    std::atomic<int> previewSerial { 0 };
    std::atomic<bool> previewPlaying { false };
    std::atomic<float> previewGain { 1.0f };
    std::shared_ptr<PreviewClip> previewActive; // audio thread only
    int previewSeenSerial = 0, previewFadeIn = 0, previewFadeOut = 0;
    double previewPosition = 0.0;
    mutable juce::SpinLock stateLock;
    std::array<juce::String, (size_t) numSampleOscs> samplePaths;
    std::array<std::shared_ptr<SampleData>, (size_t) numSampleOscs> embeddedSamples; // under stateLock
    // The last encoding of each embedded sample (gzip of a multisample takes
    // a second), reused while the oscillator holds the same audio.
    std::array<std::pair<std::shared_ptr<SampleData>, juce::ValueTree>, (size_t) numSampleOscs> encodedSamples; // under stateLock
    std::array<juce::String, (size_t) numSampleOscs> pendingSamplePaths;
    std::array<bool, (size_t) numSampleOscs> pendingSampleClear {};
    // A path the state names whose file would not load: the slot is empty,
    // and a save still writes the path (the file may be back next time).
    std::array<juce::String, (size_t) numSampleOscs> missingSamplePaths;
    // An SFZ that loaded without some of its audio files (under stateLock).
    std::array<juce::StringArray, (size_t) numSampleOscs> sfzMissingFiles;
    std::atomic<int> missingSamplesVersion { 0 };
    int missingSamplesPrompted = 0;
    std::array<juce::String, (size_t) numUserSlots> missingTablePaths;
    int missingTableModes[numUserSlots] {};
    bool samplesReloadPending = false;
    std::array<juce::String, (size_t) numUserSlots> userTablePaths;
    std::array<juce::String, (size_t) numUserSlots> pendingUserTablePaths;
    std::array<int, (size_t) numUserSlots> userTableModes {};

    struct UserPresetMeta
    {
        juce::int64 modified = -1;
        juce::String category, tags, author, comment;
    };

    const UserPresetMeta& getUserPresetMeta (const juce::File& file) const;
    mutable std::unordered_map<std::string, UserPresetMeta> userPresetMetaCache;
    std::array<int, (size_t) numUserSlots> pendingUserTableModes {};
    std::array<bool, (size_t) numUserSlots> pendingUserTableClear {};
    bool userTablesReloadPending = false;

    juce::AudioBuffer<float> lfoBuffers;
    std::array<double, (size_t) numLfos> lfoPhases {};
    std::array<std::atomic<float>, (size_t) numLfos> lfoSampleHolds {};
    std::array<std::atomic<float>, (size_t) numLfos> lfoLastValues {};
    std::array<LfoChaos, (size_t) numLfos> lfoChaos;
    // M8.1: the simulated shapes, SMOOTH, output B and the triggers.
    std::array<LfoSim, (size_t) numLfos> lfoSims;
    std::atomic<float> westGateDisplay { 0.0f };
    // M8.5
    void updateEvolveAndVector (int numSamples);
    MacroEvolve evolve, vectorDrift;
    std::array<std::atomic<float>, Mod::numMacros> macroDrift {};
    std::atomic<float> vectorX { 0.5f }, vectorY { 0.5f };
    std::array<float, OscillatorIds::count> vectorGains = [] { std::array<float, OscillatorIds::count> g {}; g.fill (1.0f); return g; }();
    double vectorPathPhase = 0.0;
    std::array<ParamRef, Mod::numMacros> macroIds;
    std::array<std::pair<ParamRef, ParamRef>, Mod::numMacros> evolveIds;
    std::array<std::pair<ParamRef, ParamRef>, numVectorPoints> vectorPathIds;
    std::array<LfoSmoother, (size_t) numLfos> lfoSmoothers;
    std::array<bool, (size_t) numLfos> lfoRoutedB {};
    std::array<std::atomic<float>, (size_t) numLfos> lfoLastValuesB {};
    std::array<bool, (size_t) numLfos> lfoFireWas {};
    std::array<long long, (size_t) numLfos> lfoBeatLast = [] { std::array<long long, (size_t) numLfos> a {}; a.fill (-1); return a; }();
    std::array<double, (size_t) numLfos> lfoBeatPhase {};
    long long lfoGenerativeLast = -1;
    double lfoGenerativePhase = 0.0;
    std::array<unsigned, (size_t) numLfos> lfoTriggerCounts {};
    std::uint32_t lfoSimSeedCounter = 1;
    juce::Random lfoRandom;
    juce::Random lfoPoolRandom { 31415 };
    juce::Random& randomForLfo (int lfo) { return lfo < 4 ? lfoRandom : lfoPoolRandom; }

    float modWheelValue = 0.0f;
    float aftertouchValue = 0.0f;
    float expressionValue = 1.0f;
    std::atomic<float> modWheelDisplay { 0.0f };
    std::atomic<float> aftertouchDisplay { 0.0f };
    std::atomic<float> expressionDisplay { 1.0f };
    std::atomic<float> clockShDisplay { 0.0f };
    std::atomic<float> msegDisplay { 0.0f };
    std::atomic<float> msegPhaseDisplay { 0.0f };

    double currentSampleRate = 44100.0;
    double baseSampleRate = 44100.0;
    int expectedBlockSize = 512;
    std::atomic<double> currentBpm { 120.0 };
    std::atomic<double> displaySampleRate { 44100.0 };

    std::array<std::atomic<float>, (size_t) maxDestinations> modDisplayValues {};

    // Modulation of plain parameters (effects, master...): offsets in the
    // parameter's normalised range, applied inside getParam().
    struct ParamDestination
    {
        std::atomic<float>* raw = nullptr;
        juce::RangedAudioParameter* parameter = nullptr;
    };
    std::vector<ParamDestination> paramDestinations;
    std::unordered_map<const std::atomic<float>*, int> rawToParamDestination;

    // getParam's lookup: an open-addressed hash of every parameter ID, built
    // once in the constructor and read-only after (so any thread can use
    // it). The value tree's own lookup is a string-keyed map, and a block
    // makes over a thousand reads.
    std::vector<ParamCacheEntry> paramCache;
    std::vector<juce::String> paramCacheIds;
    std::uint32_t paramCacheMask = 0;
    void buildParamCache();
    std::array<float, (size_t) maxDestinations> paramDestinationOffsets {};
    bool anyParamModulation = false;
    // Stereo routes into a physical string's knobs (Mod::StereoString): the
    // left strings' offset per oscillator and knob (the right ones get minus
    // it), and which oscillators have such a route at all (even at 0 now).
    std::array<std::array<float, Mod::StereoString::numFields>, 6> stringStereoOffsets {};
    std::array<bool, 6> stringStereoRouted {};

    // The last note played and the loudest voice's per-voice sources, for
    // modulation that has to be synth-wide.
    std::atomic<float> monitorVelocity { 0.0f };
    std::atomic<float> monitorKeyTrack { 0.0f };
    std::atomic<float> monitorRandom { 0.0f };
    float lfoStepValues[numLfos][16] {};
    float globalSourceValue (Mod::Source source) const;
    void evaluateGlobalModulation (const Mod::Slot* slots, int numSlots);
    std::array<std::atomic<float>, (size_t) numLfos> lfoPhaseDisplays {};
    std::atomic<float> displayPhase { 0.0f };
    std::atomic<float> displayFrequency { 0.0f };
    std::array<std::atomic<float>, OscillatorIds::count> displaySamplePositions {};
    std::array<std::atomic<float>, OscillatorIds::count> oscDisplayPhases {};

    std::vector<float> scopeLeft, scopeRight;
    std::atomic<int> scopeWritePos { 0 };
    std::atomic<int> scopeBlockSamples { 0 };
    std::atomic<double> scopeBlockMs { 0.0 };
    mutable juce::SpinLock scopeLock;

    std::array<std::array<float, lfoDrawSteps>, (size_t) numLfos> lfoCustom {};
    std::array<std::array<float, lfoDrawSteps>, (size_t) numLfos> activeLfoCustom {};
    std::array<LfoCurve, (size_t) numLfos> lfoCurves;
    std::array<std::array<float, LfoCurve::tableSize>, (size_t) numLfos> lfoCurveTables {};
    std::array<std::array<float, LfoCurve::tableSize>, (size_t) numLfos> activeLfoCurveTables {};
    mutable juce::SpinLock lfoShapeLock;

    using RemapTable = std::array<float, (size_t) Mod::remapSize + 1>;
    std::array<LfoCurve, (size_t) Mod::maxSlots> modRemaps;
    std::array<RemapTable, (size_t) Mod::maxSlots> modRemapTables {};
    std::array<std::atomic<bool>, (size_t) Mod::maxSlots> modRemapOn {};
    std::array<RemapTable, (size_t) Mod::maxSlots> activeModRemapTables {};
    std::array<bool, (size_t) Mod::maxSlots> activeModRemapOn {};
    std::atomic<int> remapEpoch { 0 };
    int activeRemapEpoch = -1;
    mutable juce::SpinLock remapLock;

    // On-screen keyboard notes, queued lock-free from the message thread so
    // none are lost between blocks; they join the MIDI input (and so go
    // through scale snap, note spray and the arpeggiator).
    struct PreviewEvent { int note = 0; float velocity = 0.0f; bool isOn = false; };
    static constexpr int previewQueueSize = 128;
    juce::AbstractFifo previewFifo { previewQueueSize };
    std::array<PreviewEvent, (size_t) previewQueueSize> previewEvents {};
    std::atomic<float> cpuUsage { 0.0f };
    std::atomic<int> activeVoiceCount { 0 };
    std::atomic<int> macroLearn { -1 };
    std::atomic<int> macroCc[Mod::numMacros] {};

    std::atomic<int> pendingProgramChange { -1 };
    std::atomic<float> pendingMacros[Mod::numMacros] {};
    std::atomic<bool> macroPendingFlags[Mod::numMacros] {};
    std::atomic<bool> macrosPending { false };
    // Parameter MIDI learn: CC -> index into getParameters() (-1: none), the
    // parameter waiting for a CC, and values queued for the message thread.
    std::array<std::atomic<int>, 128> ccParameter;
    std::atomic<int> paramLearn { -1 };
    std::array<std::atomic<float>, 128> pendingCcValues;
    std::array<std::atomic<bool>, 128> ccValuePending;
    std::atomic<bool> ccPending { false };
    juce::String getParamCcMapText() const;
    void setParamCcMapText (const juce::String& text);
    std::atomic<float> envMonitorAmp { 0.0f };
    std::atomic<float> envMonitorFilter { 0.0f };
    std::atomic<float> envMonitorFilter2 { 0.0f };
    std::atomic<float> envMonitorMod { 0.0f };
    std::atomic<float> envMonitorEnv4 { 0.0f };
    // The loudest voice's Operator Env LFO and pitch envelope (review 6).
    std::atomic<float> monitorOpLfo { 0.0f }, monitorOpPitch { 0.0f };
    std::atomic<float> monitorOpEnvSeconds { -1.0f }, monitorOpEnvRelease { -1.0f };
    std::array<std::atomic<float>, 11> envMonitorExtra {};
    std::array<std::atomic<float>, 16> envMonitorPositions {};
    std::array<std::atomic<float>, 16> envMonitorStageStarts {};
    std::array<std::atomic<float>, 80> envMonitorSeconds {};
    std::atomic<int> noteSerial { 0 };
    std::array<std::atomic<juce::uint8>, 128> uiNoteVelocity {};
    std::atomic<juce::uint32> uiNoteEpoch { 0 };
    std::array<std::atomic<int>, 3> revealMasks { newPatchOscRevealMask, defaultRevealMask, defaultRevealMask };
    std::atomic<int> revealVersion { 0 };
    std::array<std::atomic<juce::uint32>, SectionColours::count> sectionColours {};
    std::atomic<int> sectionColourVersion { 0 };

    struct ParamEpoch : juce::AudioProcessorParameter::Listener
    {
        explicit ParamEpoch (std::atomic<unsigned>& e) : epoch (e) {}
        void parameterValueChanged (int, float) override { ++epoch; }
        void parameterGestureChanged (int, bool) override {}
        std::atomic<unsigned>& epoch;
    };
    std::atomic<unsigned> paramEpoch { 0 }, liveEpoch { 0 }, dataEpoch { 0 }, sampleEpoch { 0 };
    ParamEpoch paramEpochListener { paramEpoch };

   #if JUCE_LINUX || JUCE_BSD
    struct HostRunLoopKeepAlive final : juce::VST3ClientExtensions
    {
        void setIHostApplication (Steinberg::FUnknown* host) override;
    };
    HostRunLoopKeepAlive hostRunLoopKeepAlive;
   #endif

    juce::dsp::Oversampling<float> oversampler2x { 2, 1,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
    juce::dsp::Oversampling<float> oversampler4x { 2, 2,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
    std::atomic<int> oversamplingFactor { 1 };
    int wantedOversamplingFactor() const;
    juce::dsp::Oversampling<float>& activeOversampler() { return oversamplingFactor.load() == 4 ? oversampler4x : oversampler2x; }
    juce::MidiBuffer scaledMidiBuffer;
    Mseg mseg;
    std::array<bool, 128> msegKeyDown {}; // keys down, to restart a one-shot MSEG on the first
    int msegKeysDown = 0;
    // The sustain pedal as the host sent it (per channel), to give the
    // synth again after a reset that forgets it (prepare, oversampling, a new patch).
    std::array<bool, 16> pedalChannelDown {};
    void restoreSustainPedal();
    // E-12: the keys held down (as the synth was told, in order), so a change of
    // oversampling, which re-prepares the voices and cuts them, can play them
    // again. The audio thread writes it; the message thread reads it only
    // while processing is suspended.
    struct HeldKey
    {
        juce::uint8 channel = 1, note = 0, velocity = 0;
    };
    static constexpr int maxHeldKeys = 64;
    std::array<HeldKey, (size_t) maxHeldKeys> heldKeys {};
    int numHeldKeys = 0;
    std::array<HeldKey, (size_t) maxHeldKeys> resumeKeys {}; // to play at the next block (written while suspended)
    std::atomic<int> numResumeKeys { 0 };
    void trackHeldKeys (const juce::MidiBuffer& midi) noexcept;
    void playResumedKeys (juce::MidiBuffer& midi, int blockSamples) noexcept;
    SympatheticStrings sympatheticStrings;
    bool sympatheticWasOn = false;

    // M4 acoustic keys, shared by every voice (base rate, after the voices).
    Soundboard soundboard;
    DenseSoundboard denseSoundboard;
    PedalResonance pedalResonance;
    MechanicalNoise mechanicalNoise;
    bool keysPedalDown = false, soundboardWasOn = false;
    std::array<bool, 128> pedalHeldNotes {};
    void processAcousticKeys (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi);

    double clockShPhase = 0.0;
    float clockShValue = 0.0f;

    std::array<std::atomic<float>, numFxSlots> fxSlotCpu {};
    std::array<std::atomic<float>, numFxSlots> fxSlotIn {}, fxSlotOut {};
    juce::String chainA, chainB;
    bool chainBValid = false;
    bool showingChainA = true;
    std::atomic<int> engineDisplayStep { -1 };
    std::atomic<int> euclidDisplayStep { -1 };
    std::atomic<double> hostPpq { 0.0 };
    std::atomic<bool> hostPlaying { false };
    std::array<std::atomic<float>, 2> outputPeaks {};
    std::atomic<unsigned> noteOnCount { 0 };
    std::atomic<float> outputLevelDisplay { 0.0f };

public:
    // Pattern built-ins as step levels (for the editor and the Custom copy).
    static float gatePatternLevel (int pattern, int step);
    int getGateDisplayStep() const;

    // The note engine's current step number (arp, probability sequencer or
    // Euclid), or -1 while no key is held. For the Generative card's playhead.
    int getEngineDisplayStep() const { return engineDisplayStep.load(); }

    // The Euclid rhythm's current step (whichever target plays it), or -1.
    int getEuclidDisplayStep() const { return euclidDisplayStep.load(); }

    // Tests only: what the last block sent to the voices.
    const juce::MidiBuffer& getSynthMidiForTest() const { return midiForSynth; }

    // Tests only: one arpeggiator note choice from the currently held notes.
    int pickArpNoteForTest (int mode, int octaves) { return selectArpNote (mode, octaves); }

private:
    // Knobs read once a block (an effect's MIX, a slot's blend, MASTER) are
    // eased across the block from where the last block ended, so a knob move
    // or a host's automation step is a ramp, not a step at the block edge. A
    // steady knob gives its value exactly (the ramp's start is its end), so
    // nothing changes while nothing moves. Reset with a patch change.
    struct BlockRamp
    {
        float last = std::numeric_limits<float>::quiet_NaN();
        float from = 0.0f, step = 0.0f;

        void begin (float target, int numSamples) noexcept
        {
            from = std::isfinite (last) ? last : target;
            step = numSamples > 0 ? (target - from) / (float) numSamples : 0.0f;
            last = target;
        }
        float at (int i) const noexcept { return from + step * (float) i; }
        bool moving() const noexcept { return step != 0.0f; }
        void reset() noexcept { last = std::numeric_limits<float>::quiet_NaN(); }
    };
    enum class FxRamp
    {
        DriveMix, CrushMix, CombMix, DelayMix, StutterMix, SmearMix, CompMix, HaasMix, FlangerMix, DimMix, GateMix,
        TapeStopMix, OttMix, WidthMix, ShifterMix, RingMix, OctaverMix, VowelMix, FeedbackMix,
        DriveAmount, FoldAmount, DelayTimeL, DelayTimeR, AmpDrive, AmpLevel, CompMakeup, HaasDelay, TiltLow, TiltHigh,
        UtilGain, LimitCeiling, Count
    };
    std::array<BlockRamp, (size_t) numFxSlots> slotBlendRamps;
    // A slot that is bypassed runs one more block with its wet part ramped
    // to 0 (fxSlotRuns), and is faded back in when it returns; one that was
    // skipped for longer than the longest delay memory starts it empty.
    std::array<bool, (size_t) numFxSlots> fxSlotLive {}, fxSlotFaded {};
    std::array<std::int64_t, (size_t) numFxSlots> fxSlotLastRun {};
    // The effect a slot ran last block (0: none), so a change of type crossfades from the old one.
    std::array<int, (size_t) numFxSlots> fxSlotPrevType {};
    std::int64_t fxClock = 0; // samples the rack has run (processEffects)
    int fxBlockLength = 0;
    BlockRamp masterRamp;
    void resetBlockRamps() noexcept
    {
        resetFxEngineRamps();
        for (auto& ramp : slotBlendRamps)
            ramp.reset();
        fxSlotLive.fill (false);
        fxSlotFaded.fill (false);
        fxSlotPrevType.fill (0);
        masterRamp.reset();
    }

    juce::AudioBuffer<float> fxScratch, fxScratchOld;
    // Roadmap step 17: the effects' memory lives in engines (FxEngine.h).
    // fxMain is the rack's shared one (one set per effect type, as always);
    // a slot that is its own instance of an effect runs on its own engine.
    class FxEngine;
    std::unique_ptr<FxEngine> fxMain;
    std::array<std::atomic<FxEngine*>, (size_t) numFxSlots> slotEngines {}; // null: the shared engine
    FxEngine& engineForSlot (int slotIndex) const;
    void resetFxEngineRamps() noexcept;
    void prepareFxEngines (double sampleRate, int samplesPerBlock);
    void resetFxEngines();
    // The engine an effect type's card shows (its meters and pictures).
    const FxEngine& displayEngine (int type) const;
    int fxEngineBlockSize = 512;

    // Roadmap step 17: the FX patch (FxPatcher.cpp).
    std::shared_ptr<const FxPatchData::Patch> fxPatch = std::make_shared<FxPatchData::Patch>(); // under stateLock off the message thread
    std::array<std::unique_ptr<FxEngine>, (size_t) numFxSlots> ownedEngines;
    std::array<std::unique_ptr<std::vector<float>>, (size_t) numFxSlots> ownedValues;
    std::array<std::atomic<const float*>, (size_t) numFxSlots> slotValues {};
    // The LFO boxes as the audio thread runs them: made from the patch on
    // the message thread (applyFxPatchRuntime), swapped in whole.
    struct FxLfoProgram
    {
        std::array<FxLfo::Settings, FxLfo::maxLfos> settings {};
        std::array<bool, FxLfo::maxLfos> used {};
        struct Route
        {
            int lfo = 0;  // 0-based
            int slot = 0; // 0-based
            int index = -1; // ParamCacheEntry::index
            juce::RangedAudioParameter* parameter = nullptr;
            float amount = 0.0f;
        };
        std::array<Route, FxLfo::maxRoutes> routes {};
        int numRoutes = 0;
    };
public:
    // What one slot reads while it runs: the offsets on its parameters (readParam).
    struct FxLfoSlotMods
    {
        int count = 0;
        std::array<int, FxLfo::maxRoutes> index {};
        std::array<juce::RangedAudioParameter*, FxLfo::maxRoutes> parameter {};
        std::array<float, FxLfo::maxRoutes> offset {};
    };
    static const FxLfoSlotMods* swapFxLfoMods (const FxLfoSlotMods* mods) noexcept;
private:
    std::unique_ptr<FxLfoProgram> ownedFxLfoProgram;
    std::atomic<const FxLfoProgram*> fxLfoProgram { nullptr };
    std::array<FxLfo::State, FxLfo::maxLfos> fxLfoStates {};
    std::array<FxLfoSlotMods, (size_t) numFxSlots> fxLfoSlotMods {};
    std::array<std::atomic<float>, FxLfo::maxLfos> fxLfoShownValue {}, fxLfoShownPhase {};
    double fxLfoLastPpq = -1.0;
    juce::int64 fxLfoSamplesSincePpq = 0;
    unsigned fxLfoLastNoteCount = 0; // noteOnCount at the last chunk (RETRIG)
    bool fxLfoRunning = false;
    void advanceFxLfos (const FxLfoProgram& program, int numSamples);
    void processEffectsNow (juce::AudioBuffer<float>& buffer);
    void resetFxLfos();

    struct RetiredFx
    {
        juce::uint32 when = 0;
        std::unique_ptr<FxEngine> engine;
        std::unique_ptr<std::vector<float>> values;
        std::unique_ptr<FxLfoProgram> lfoProgram;
    };
    std::vector<RetiredFx> retiredFx;
    juce::CriticalSection fxEditLock; // the patch's edits (message thread, or a host's state load)
    void publishFxPatch (FxPatchData::Patch patch);
    juce::String fxSlotLabel (int slotIndex) const;
    void retireFx (std::unique_ptr<FxEngine> engine, std::unique_ptr<std::vector<float>> values);
    void freeRetiredFx (bool all);
    void applyFxPatchRuntime();
    std::unique_ptr<FxLfoProgram> buildFxLfoProgram (const FxPatchData::Patch& patch, const int* slotTypes) const;
    bool fxLfoProgramDiffers (const FxLfoProgram& program) const;
    // FX-14: a slot swap reaches the audio thread whole. The message thread
    // fills fxSwapStage and publishes the patch plan tagged with the swap's
    // number; applyFxSwap (the audio thread at the top of the rack, or the
    // message thread when no block is running) then moves the engines, value
    // tables, LFO program and the ten parameter values in one go, between two
    // blocks. A patch plan whose tag is not the applied number is not run
    // (the fixed loop runs for that block, which is consistent).
    struct FxSwapStage
    {
        int slot[2] { -1, -1 };
        FxEngine* engine[2] {};
        const float* values[2] {};
        bool lfoChanged = false;
        const FxLfoProgram* lfo = nullptr;
        std::atomic<float>* raw[5][2] {};
    };
    FxSwapStage fxSwapStage;
    std::uint64_t fxSwapGen = 0; // the last swap's number (message thread, under fxEditLock)
    std::atomic<std::uint64_t> fxSwapRequested { 0 }, fxSwapApplied { 0 };
    juce::SpinLock fxSwapApplyLock;
    std::atomic<juce::uint32> fxLastRackMs { 0 };
    std::atomic<juce::Thread::ThreadID> fxRackThread { nullptr };
    void applyFxSwap (bool mayWait = false) noexcept;
    void reconcileFxPatch();
    std::vector<std::pair<juce::String, float>> snapshotFxParams (int type) const;
    void writeFxParams (const std::vector<std::pair<juce::String, float>>& values);
    const std::vector<juce::String>& fxParamIds (int type) const;
    mutable std::array<std::vector<juce::String>, (size_t) numFxTypes + 1> fxParamIdCache;
    std::unique_ptr<PatchGraph::Runner> fxRackRunner, fxPatchRunner;
    juce::SpinLock fxCompileLock; // the rack runner's publish: message thread or an offline render
    std::atomic<bool> fxRackCompileWanted { false }, fxReconcileWanted { false };
    std::uint64_t fxRackSignature();
    void compileFxRackPlan (std::uint64_t signature);
    void compileFxPatchPlan();
    void processEffectsFixed (juce::AudioBuffer<float>& buffer);
    void processEffectsPatched (juce::AudioBuffer<float>& buffer);
    ParamRef fxPatcherRef { "fx_patcher" };
    // The ROUTING cables, compiled when they change (a few dozen
    // operations, no allocation, so the audio thread does it: rule B1's
    // "per patch, not per sample").
    const VoiceRouting::Table& currentRoutingTable();
    ParamRef routeCablesRef { "route_cables" };
    std::array<ParamRef, VoiceRouting::numCableParams> cableRefs { "cable_osc1", "cable_osc2", "cable_osc3", "cable_osc4", "cable_osc5",
                                                                   "cable_osc6", "cable_subnoise", "cable_f1", "cable_f2", "cable_west", "cable_body" };
    VoiceRouting::Table routingTable;
    std::uint64_t routingSignature = ~(std::uint64_t) 0;
    struct FxSlotTypeListener : juce::AudioProcessorParameter::Listener
    {
        explicit FxSlotTypeListener (IlanaSynthAudioProcessor& p) : processor (p) {}
        void parameterValueChanged (int, float) override;
        void parameterGestureChanged (int, bool) override {}
        IlanaSynthAudioProcessor& processor;
    };
    FxSlotTypeListener fxSlotTypeListener { *this };
    // FX splitters: a slot set to a band processes only that part of the
    // signal (Linkwitz-Riley crossovers, or mid/side) and the rest passes
    // around it, so an untouched band sums back exactly.
    struct SplitFilter
    {
        // Two cascaded Butterworth sections per channel (24 dB/oct, LR4) cut
        // the band; the crossovers' allpasses give the phase the band's
        // complement needs (LR4 low + high = a second-order allpass).
        std::array<std::array<juce::IIRFilter, 2>, 2> lowA, lowB;
        std::array<juce::IIRFilter, 2> allLow, allHigh;
        int lastBand = 0;
        float lastLow = 0.0f, lastHigh = 0.0f;
    };
    std::array<SplitFilter, (size_t) numFxSlots> fxSplit;
    juce::AudioBuffer<float> fxBand;
    // PARALLEL routing: the rack's input, and one branch's copy of it.
    juce::AudioBuffer<float> fxParallelIn, fxBranch;
    void processSlotBand (int slot, int type, int band, juce::AudioBuffer<float>& buffer, bool solo, float blend, float blendFrom);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IlanaSynthAudioProcessor)
};

#include "processor/FxEngine.h"
