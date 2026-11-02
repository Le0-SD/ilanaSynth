#pragma once

// One effects engine: every effect type's memory (delay lines, filters,
// envelopes, phases, ramps) and the code that runs it (roadmap step 17,
// "every node owns its state", docs/ROUTING-CPU.md A4).
//
// The rack's slots share the processor's main engine, one set of state per
// effect type, as they always did, so a patch that never asks for more renders
// exactly as before. A slot made its own INSTANCE of an effect gets an engine
// of its own (FxInstances.h), so two Delays or two Reverbs no longer share a
// delay line. The engine reads its knobs through the processor's getParam:
// while an instance runs, getParam answers with that instance's own values
// (readParam's per-thread override), so the effect code is the same for both.
//
// Included at the end of PluginProcessor.h (it is a nested class and needs the
// processor's ParamRef, BlockRamp and FxRamp).
class IlanaSynthAudioProcessor::FxEngine
{
public:
    explicit FxEngine (IlanaSynthAudioProcessor& ownerToUse)
        : owner (ownerToUse),
          currentSampleRate (ownerToUse.currentSampleRate),
          currentBpm (ownerToUse.currentBpm),
          hostPpq (ownerToUse.hostPpq),
          hostPlaying (ownerToUse.hostPlaying),
          lfoRandom (ownerToUse.lfoRandom),
          liveDry (ownerToUse.liveDry),
          liveInputSamples (ownerToUse.liveInputSamples),
          tapStepIds (ownerToUse.tapStepIds),
          gateStepIds (ownerToUse.gateStepIds),
          euclidDisplayStep (ownerToUse.euclidDisplayStep)
    {
    }

    // The effects' part of prepareToPlay, and of a patch change (cutPatchTails).
    void prepare (double sampleRate, int samplesPerBlock, int expectedBlockSize);
    void reset();
    void resetRamps() noexcept
    {
        for (auto& ramp : fxRamps)
            ramp.reset();
    }

    void processSlot (int type, juce::AudioBuffer<float>& buffer);

    // Makes an Airwindows algorithm ahead of its use (off the audio thread).
    void preloadAirwindows (int algorithm) { airwindowsModule.preload (algorithm); }
    void preloadAirwindowsCategory (int category, int choice);

    void loadReverbIr (const juce::File& file);

private:
    friend class IlanaSynthAudioProcessor;

    IlanaSynthAudioProcessor& owner;
    double& currentSampleRate;
    std::atomic<double>& currentBpm;
    std::atomic<double>& hostPpq;
    std::atomic<bool>& hostPlaying;
    juce::Random& lfoRandom;
    juce::AudioBuffer<float>& liveDry;
    int& liveInputSamples;
    std::array<ParamRef, 16>& tapStepIds;
    std::array<ParamRef, 16>& gateStepIds;
    std::atomic<int>& euclidDisplayStep;

    float getParam (const char* id) const { return owner.getParam (id); }
    float getParam (const ParamRef& ref) const { return owner.getParam (ref); }
    float getFxMod (Mod::Destination destination, float depth) const { return owner.getFxMod (destination, depth); }
    EqSettings getEqSettings() const { return owner.getEqSettings(); }

    void processAmp (juce::AudioBuffer<float>& buffer);
    void processDrive (juce::AudioBuffer<float>& buffer);
    void processCrush (juce::AudioBuffer<float>& buffer);
    void processCompressor (juce::AudioBuffer<float>& buffer);
    void processComb (juce::AudioBuffer<float>& buffer);
    void processPhaser (juce::AudioBuffer<float>& buffer);
    void processChorus (juce::AudioBuffer<float>& buffer);
    void processHaas (juce::AudioBuffer<float>& buffer);
    void processDelay (juce::AudioBuffer<float>& buffer);
    void processStutter (juce::AudioBuffer<float>& buffer);
    void processSmear (juce::AudioBuffer<float>& buffer);
    void processFreeze (juce::AudioBuffer<float>& buffer);
    void processReverb (juce::AudioBuffer<float>& buffer);
    void processFlanger (juce::AudioBuffer<float>& buffer);
    void processDimension (juce::AudioBuffer<float>& buffer);
    void processGate (juce::AudioBuffer<float>& buffer);
    void processTapeStop (juce::AudioBuffer<float>& buffer);
    void processTilt (juce::AudioBuffer<float>& buffer);
    void processUtility (juce::AudioBuffer<float>& buffer);
    void processAirwindows (juce::AudioBuffer<float>& buffer);
    void processAirwindowsCategory (juce::AudioBuffer<float>& buffer, int category);
    void processVocoder (juce::AudioBuffer<float>& buffer);
    void processOtt (juce::AudioBuffer<float>& buffer);
    void processLimiter (juce::AudioBuffer<float>& buffer);
    void processWidener (juce::AudioBuffer<float>& buffer);
    void processTremolo (juce::AudioBuffer<float>& buffer);
    void processFreqShift (juce::AudioBuffer<float>& buffer);
    void processRingMod (juce::AudioBuffer<float>& buffer);
    void processOctaver (juce::AudioBuffer<float>& buffer);
    void processVowel (juce::AudioBuffer<float>& buffer);
    void processFeedback (juce::AudioBuffer<float>& buffer);
    void processEq (juce::AudioBuffer<float>& buffer);
    void processFilter (juce::AudioBuffer<float>& buffer);

    //==========================================================================
    // State (moved verbatim from the processor; same initial values).
    int crushCounter = 0;
    float crushHold[2] { 0.0f, 0.0f };
    float delayDampState[2] { 0.0f, 0.0f };

    juce::AudioBuffer<float> stutterBuffer;
    int stutterLength = 0;
    int stutterWrite = 0;
    int stutterRead = 0;
    bool stutterRecording = false;
    bool stutterWasOn = false;
    // Waiting for sound to capture (switched on, or the playing stopped);
    // the loop's fade-out once its input has been silent a whole loop.
    bool stutterArmed = false;
    int stutterSilent = 0;
    float stutterFade = 1.0f;

    // The Airwindows module (FX type 30): only the chosen algorithm runs.
    airwindows::Module airwindowsModule;
    ParamRef awAlgoRef { "fx_aw_algo" }, awMixRef { "fx_aw_mix" };
    std::array<ParamRef, airwindows::Module::numKnobs> awKnobRefs { ParamRef ("fx_aw_p1"), ParamRef ("fx_aw_p2"),
        ParamRef ("fx_aw_p3"), ParamRef ("fx_aw_p4"), ParamRef ("fx_aw_p5") };
    std::vector<float> airwindowsMonoRight;
    // The Airwindows category modules (FX types 32-41), one engine each;
    // their refs: algo, p1..p5, mix.
    std::array<airwindows::Module, numAwCategories> awCategoryModules;
    std::array<std::array<ParamRef, 7>, numAwCategories> awCategoryRefs = makeAwCategoryRefs();
    // The vocoder (FX type 31).
    Vocoder vocoder;
    std::vector<float> vocoderModulator;
    ParamRef vocSourceRef { "fx_voc_source" }, vocBandsRef { "fx_voc_bands" }, vocWidthRef { "fx_voc_width" },
        vocAttackRef { "fx_voc_attack" }, vocReleaseRef { "fx_voc_release" }, vocFormantRef { "fx_voc_formant" },
        vocUnvoicedRef { "fx_voc_unvoiced" }, vocRateRef { "fx_voc_rate" }, vocLevelRef { "fx_voc_level" },
        vocMixRef { "fx_voc_mix" };
    std::array<std::atomic<float>, 24> vocoderBandDisplay {};

    juce::dsp::Chorus<float> chorus;
    juce::dsp::Phaser<float> phaser;
    juce::dsp::Convolution convolution;
    std::atomic<bool> reverbIrLoaded { false };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine { 96000 };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> combLine { 96000 };
    FreeverbFast reverb; // juce::Reverb, bit for bit, without the divisions

    GranularPitchShift tapeShift[2];
    GranularPitchShift shimmerShift[2];
    GranularSmear smear[2];
    SpectralFreeze freeze; // both channels in one FFT pair

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> haasLine { 4800 };
    float compEnvelope[2] { 0.0f, 0.0f };
    float ampLowState[2] { 0.0f, 0.0f };
    float ampHighState[2] { 0.0f, 0.0f };

    double wowPhase = 0.0;

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> flangerLine { 4800 };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> dimLine { 9600 };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> springComb { 4800 };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> feedbackLine { 9600 };
    float feedbackState[2] {};
    double stutterPosition = 0.0;
    float stutterRate = 1.0f;
    double flangerPhase = 0.0;
    double dimPhase = 0.0;
    double tremoloPhase = 0.0;
    double shifterPhase = 0.0;
    double ringPhase = 0.0;
    double gatePhase = 0.0;
    double gateLastPpq = std::numeric_limits<double>::quiet_NaN(); // the host beat the gate last saw
    long long gateSamplesSincePpq = 0;
    float gateEnvelope = 1.0f;
    juce::uint16 gateRandomMask = 0xFFFF;
    int gateCycleCount = 8;
    int gateLastStep = -1;
    std::atomic<int> gateDisplayStep { -1 };

    float tapeStopRate = 1.0f;
    juce::AudioBuffer<float> tapeStopBuffer;
    int tapeStopWrite = 0;
    double tapeStopRead = 0.0;
    float tapeStopLagFade = 1.0f; // 1 = playing the lagging tape; fades to 0 to rejoin live
    float tiltLowState[2] {};

    // FILTER (type 42): one voice filter per side, the cutoff glided in
    // octaves (a jump would click), and its knobs.
    FilterUnit filterUnits[2];
    float filterOctaves[2] {};
    bool filterGlideStarted = false;
    ParamRef filterTypeRef { "fx_filter_type" }, filterSlopeRef { "fx_filter_slope" }, filterCutoffRef { "fx_filter_cutoff" },
        filterResoRef { "fx_filter_reso" }, filterDriveRef { "fx_filter_drive" }, filterMorphRef { "fx_filter_morph" },
        filterSpreadRef { "fx_filter_spread" };
    float tiltHighState[2] {};
    std::array<BlockRamp, (size_t) FxRamp::Count> fxRamps;

    float ottLowState[2][2] {};
    float ottHighState[2][2] {};
    float ottEnvelope[2][3] {};
    float limiterEnvelope[2] {};
    float shifterAllpass[2] {};
    float shifterDelay[2] {};
    float gatedReverbEnvelope = 0.0f;
    float duckEnvelope = 0.0f;
    GranularPitchShift octaverShift[2];
    Biquad eqBands[2][3];
    Svf vowelFilters[2][3];
    juce::AudioBuffer<float> reverbScratch;
    // KEEP DRY's wet copy, and how long its wet is still added after MIX
    // reaches 0 (review 7); reverbWetOnly while that copy is processed.
    juce::AudioBuffer<float> reverbDryScratch;
    int reverbKeepDryHold = 0;
    bool reverbWetOnly = false;
    std::atomic<float> compGainReduction { 1.0f };
    std::atomic<float> limiterGainReduction { 1.0f };
    std::array<std::atomic<float>, 3> ottBandGain { 1.0f, 1.0f, 1.0f };

    JUCE_DECLARE_NON_COPYABLE (FxEngine)
};
