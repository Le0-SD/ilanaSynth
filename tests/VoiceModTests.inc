// Phase 1: new filter models and voice modes.
void runFilterModelTests()
{
    constexpr double sampleRate = 48000.0;

    // Self-oscillation: silence in, a sustained bounded tone out, for the
    // SVF (Morph), Airwindows' Y low-pass above 0.98 and the ladder at full
    // feedback.
    for (const auto type : { (int) FilterType::Morph, (int) FilterType::LowPass, (int) FilterType::LadderLow })
    {
        FilterUnit filter;
        filter.setType (type, false);
        filter.setCoefficients (FilterUnit::makeCoefficients (type, sampleRate, 1000.0, 1.0));

        auto peak = 0.0f;
        auto lateEnergy = 0.0;
        auto finite = true;

        for (int i = 0; i < (int) sampleRate * 2; ++i)
        {
            const auto input = i < 32 ? 0.5f : 0.0f;
            const auto out = filter.process (input);
            finite = finite && std::isfinite (out);
            peak = juce::jmax (peak, std::abs (out));

            if (i > (int) sampleRate)
                lateEnergy += (double) out * out;
        }

        const auto lateRms = std::sqrt (lateEnergy / sampleRate);
        check (finite && peak < 4.0f && lateRms > 0.05,
               FilterType::getNames()[type] + " self-oscillates at max resonance and stays bounded (peak "
                   + juce::String (peak, 3) + ", late rms " + juce::String (lateRms, 3) + ")");
    }

    // At low level Airwindows' Y low-pass (24 dB) is the old SVF cascade
    // (its encode and decode curves are straight there; the difference is its
    // 20 kHz Butterworths), so the presets' tone balance carries over.
    {
        FilterUnit unit;
        unit.setType (FilterType::LowPass, true);
        const auto c = FilterUnit::makeCoefficients (FilterType::LowPass, sampleRate, 800.0, 0.7);
        unit.setCoefficients (c);

        Svf a, b;
        a.setMode (Svf::Mode::LowPass);
        b.setMode (Svf::Mode::LowPass);
        a.setCoefficients (Svf::makeCoefficients (sampleRate, 800.0, 0.7));
        b.setCoefficients (Svf::makeCoefficients (sampleRate, 800.0, 0.7));

        juce::Random random (7);
        auto maxDiff = 0.0f;

        for (int i = 0; i < 4800; ++i)
        {
            const auto x = (random.nextFloat() * 2.0f - 1.0f) * 0.002f;
            maxDiff = juce::jmax (maxDiff, std::abs (unit.process (x) - b.processSample (a.processSample (x))));
        }

        check (maxDiff < 0.002f * 0.05f, "24 dB Y low-pass matches the old SVF cascade at low level (max diff "
                                            + juce::String (maxDiff / 0.002f * 100.0f, 3) + " % of the input)");
    }

    // Ladder low-pass actually low-passes and high-pass actually high-passes.
    const auto toneGain = [sampleRate] (int type, double toneHz)
    {
        FilterUnit filter;
        filter.setType (type, true);
        filter.setCoefficients (FilterUnit::makeCoefficients (type, sampleRate, 1000.0, 0.2));

        auto in = 0.0, out = 0.0;

        for (int i = 0; i < 24000; ++i)
        {
            const auto x = 0.1f * (float) std::sin (juce::MathConstants<double>::twoPi * toneHz * i / sampleRate);
            const auto y = filter.process (x);

            if (i > 4800)
            {
                in += (double) x * x;
                out += (double) y * y;
            }
        }

        return std::sqrt (out / in);
    };

    const auto lpLow = toneGain (FilterType::LadderLow, 100.0);
    const auto lpHigh = toneGain (FilterType::LadderLow, 8000.0);
    const auto hpLow = toneGain (FilterType::LadderHigh, 100.0);
    const auto hpHigh = toneGain (FilterType::LadderHigh, 8000.0);

    check (lpLow > 0.5 && lpHigh < 0.02, "ladder LP passes lows and cuts highs (100 Hz " + juce::String (lpLow, 3)
                                             + ", 8 kHz " + juce::String (lpHigh, 4) + ")");
    check (hpHigh > 0.5 && hpLow < 0.02, "ladder HP passes highs and cuts lows (100 Hz " + juce::String (hpLow, 4)
                                             + ", 8 kHz " + juce::String (hpHigh, 3) + ")");
}

void setParam (IlanaSynthAudioProcessor& processor, const juce::String& id, float value)
{
    if (auto* parameter = processor.apvts.getParameter (id))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

void runVoiceModeTests()
{
    const auto render = [] (IlanaSynthAudioProcessor& processor, juce::MidiBuffer midi, int blocks)
    {
        juce::AudioBuffer<float> buffer (2, 256);
        auto peak = 0.0f;

        for (int block = 0; block < blocks; ++block)
        {
            buffer.clear();
            juce::MidiBuffer blockMidi;

            if (block == 0)
                blockMidi = midi;

            processor.processBlock (buffer, blockMidi);
            peak = juce::jmax (peak, buffer.getMagnitude (0, 256));
        }

        return peak;
    };

    const auto chord = []
    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 10);
        midi.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100), 20);
        return midi;
    };

    {
        IlanaSynthAudioProcessor processor;
        processor.prepareToPlay (48000.0, 256);
        render (processor, chord(), 20);
        check (processor.getActiveVoiceCount() == 3, "poly mode plays a 3-note chord on 3 voices ("
                                                         + juce::String (processor.getActiveVoiceCount()) + ")");
    }

    {
        IlanaSynthAudioProcessor processor;
        setParam (processor, "poly_voices", 2.0f);
        processor.prepareToPlay (48000.0, 256);
        render (processor, chord(), 20);
        check (processor.getActiveVoiceCount() <= 2, "poly voice limit of 2 is respected ("
                                                         + juce::String (processor.getActiveVoiceCount()) + ")");
    }

    for (const auto mode : { 1, 2 })
    {
        IlanaSynthAudioProcessor processor;
        setParam (processor, "voice_mode", (float) mode);
        processor.prepareToPlay (48000.0, 256);
        const auto peak = render (processor, chord(), 20);
        const auto name = mode == 1 ? juce::String ("mono") : juce::String ("legato");

        check (processor.getActiveVoiceCount() == 1 && peak > 0.01f,
               name + " mode plays a chord on one voice (" + juce::String (processor.getActiveVoiceCount()) + ")");

        // Release the top note while the others are held: the voice must keep
        // sounding (falls back to 64), then go quiet once everything is up.
        juce::MidiBuffer releaseTop;
        releaseTop.addEvent (juce::MidiMessage::noteOff (1, 67), 0);
        const auto fallbackPeak = render (processor, releaseTop, 10);

        juce::MidiBuffer releaseAll;
        releaseAll.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        releaseAll.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
        render (processor, releaseAll, 400);

        check (fallbackPeak > 0.01f && processor.getActiveVoiceCount() == 0,
               name + " mode falls back to held notes, then releases (fallback peak "
                   + juce::String (fallbackPeak, 3) + ")");
    }

    // Legato keeps the amp envelope running between overlapping notes; mono
    // retriggers it. Slow attack makes the difference measurable.
    const auto levelAfterSecondNote = [&render] (int mode)
    {
        IlanaSynthAudioProcessor processor;
        setParam (processor, "voice_mode", (float) mode);
        setParam (processor, "amp_attack", 1.0f);
        processor.prepareToPlay (48000.0, 256);

        juce::MidiBuffer first;
        first.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        render (processor, first, 200); // ~1.07 s: attack complete

        juce::MidiBuffer second;
        second.addEvent (juce::MidiMessage::noteOn (1, 62, (juce::uint8) 100), 0);
        return render (processor, second, 2);
    };

    {
        IlanaSynthAudioProcessor processor;
        processor.setMacroName (2, "Grit");
        juce::MemoryBlock state;
        processor.getStateInformation (state);

        IlanaSynthAudioProcessor restored;
        restored.setStateInformation (state.getData(), (int) state.getSize());
        check (restored.getMacroName (2) == "Grit" && restored.getMacroName (0) == "Macro 1",
               "macro names survive a host state round trip ('" + restored.getMacroName (2) + "')");

        restored.loadFactoryPreset (0);
        check (restored.getMacroName (2) == "Macro 3", "loading Init resets macro names");

        restored.loadFactoryPreset (1);
        check (restored.getMacroName (0) == "TONE", "legacy presets get default macro names ('" + restored.getMacroName (0) + "')");
        check (restored.getPresetCategory() == "Bass", "factory preset load sets its category");

        const auto file = juce::File::createTempFile ("ilanapreset");
        restored.setPresetMeta ("Keys", "warm, vintage");
        check (restored.savePresetToFile (file), "preset with metadata saves");

        IlanaSynthAudioProcessor reloaded;
        check (reloaded.loadPresetFromFile (file) && reloaded.getPresetCategory() == "Keys"
                   && reloaded.getPresetTags() == "warm, vintage",
               "preset category and tags survive a save/load ('" + reloaded.getPresetCategory() + "')");
        file.deleteFile();
    }

    const auto legatoLevel = levelAfterSecondNote (2);
    const auto monoLevel = levelAfterSecondNote (1);

    check (legatoLevel > monoLevel * 0.9f && legatoLevel > 0.05f,
           "legato does not retrigger the envelope (legato " + juce::String (legatoLevel, 3)
               + ", mono " + juce::String (monoLevel, 3) + ")");
}

// Phase 2: warps, unison, per-voice LFOs, the extended matrix.
float renderPeakAndCentroid (IlanaSynthAudioProcessor& processor, int note, int blocks, double& centroid,
                             std::vector<float>* capture = nullptr, int velocity = 100)
{
    juce::AudioBuffer<float> buffer (2, 512);
    std::vector<float> samples;
    auto peak = 0.0f;

    for (int block = 0; block < blocks; ++block)
    {
        buffer.clear();
        juce::MidiBuffer midi;

        if (block == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) velocity), 0);

        processor.processBlock (buffer, midi);
        peak = juce::jmax (peak, buffer.getMagnitude (0, 512));

        for (int i = 0; i < 512; ++i)
            samples.push_back (buffer.getSample (0, i));
    }

    // Spectral centroid of the last 8192 samples.
    constexpr int order = 13;
    constexpr int size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> work ((size_t) size * 2, 0.0f);
    const auto start = samples.size() - (size_t) size;

    for (int i = 0; i < size; ++i)
        work[(size_t) i] = samples[start + (size_t) i]
                           * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size));

    fft.performFrequencyOnlyForwardTransform (work.data());
    double weighted = 0.0, total = 0.0;

    for (int bin = 1; bin < size / 2; ++bin)
    {
        weighted += bin * 48000.0 / size * work[(size_t) bin];
        total += work[(size_t) bin];
    }

    centroid = total > 0.0 ? weighted / total : 0.0;

    if (capture != nullptr)
        *capture = samples;

    return peak;
}

void runWarpTests()
{
    const auto names = Warp::getNames();
    double plainCentroid = 0.0;

    {
        IlanaSynthAudioProcessor processor;
        setParam (processor, "sub_on", 0.0f);
        setParam (processor, "osc1_table", 8.0f); // Sine: warps add harmonics
        setParam (processor, "f1_cutoff", 20000.0f);
        processor.prepareToPlay (48000.0, 512);
        renderPeakAndCentroid (processor, 45, 40, plainCentroid);
    }

    for (int mode = 1; mode < Warp::Count; ++mode)
    {
        IlanaSynthAudioProcessor processor;
        setParam (processor, "sub_on", 0.0f);
        setParam (processor, "osc1_table", 8.0f);
        setParam (processor, "f1_cutoff", 20000.0f);
        setParam (processor, "osc1_warp", (float) mode);
        setParam (processor, "osc1_warp_amt", 0.7f);

        if (mode == Warp::Fm || mode == Warp::Ring)
        {
            setParam (processor, "osc2_on", 1.0f);
            setParam (processor, "osc2_level", 0.01f);
            setParam (processor, "osc2_semi", 7.0f);
        }

        processor.prepareToPlay (48000.0, 512);
        double centroid = 0.0;
        const auto peak = renderPeakAndCentroid (processor, 45, 40, centroid);

        check (std::isfinite (peak) && peak > 0.02f && peak < 4.0f && centroid > plainCentroid * 1.15,
               "warp " + names[mode] + " adds harmonics to a sine (centroid " + juce::String (plainCentroid, 0)
                   + " -> " + juce::String (centroid, 0) + " Hz, peak " + juce::String (peak, 2) + ")");
    }

    // FM Noise listens to the NOISE card's generator even with its switch off and
    // its level at zero (nothing of the noise itself is heard).
    {
        IlanaSynthAudioProcessor processor;
        setParam (processor, "sub_on", 0.0f);
        setParam (processor, "osc1_table", 8.0f);
        setParam (processor, "f1_cutoff", 20000.0f);
        setParam (processor, "noise_on", 0.0f);
        setParam (processor, "noise_level", 0.0f);
        setParam (processor, "osc1_warp", (float) Warp::NoiseFm);
        setParam (processor, "osc1_warp_amt", 0.7f);
        processor.prepareToPlay (48000.0, 512);
        double centroid = 0.0;
        renderPeakAndCentroid (processor, 45, 40, centroid);
        check (centroid > plainCentroid * 1.15, "FM Noise works with the NOISE switch off (centroid "
                                                   + juce::String (plainCentroid, 0) + " -> " + juce::String (centroid, 0) + " Hz)");
    }
}

void runPerVoiceSpectralTest()
{
    // A patch (user) table takes spectral warps too, not only the factory tables.
    {
        const auto render = [] (bool warped)
        {
            IlanaSynthAudioProcessor processor;
            processor.setNonRealtime (true);
            processor.setUserTable (0, WavetableDoc::fromFactory (10));
            setParam (processor, "sub_on", 0.0f);
            setParam (processor, "osc1_table", (float) TableFactory::getNumFactoryTables());
            setParam (processor, "osc1_spectral", warped ? (float) SpectralWarp::LowPass : 0.0f);
            setParam (processor, "osc1_spectral_amt", 0.8f);
            processor.prepareToPlay (48000.0, 512);
            double centroid = 0.0;
            renderPeakAndCentroid (processor, 45, 40, centroid);
            return centroid;
        };

        const auto plain = render (false);
        const auto warped = render (true);
        check (warped < plain * 0.9,
               "a spectral warp works on a patch table (centroid " + juce::String (plain, 0) + " -> " + juce::String (warped, 0) + " Hz)");
    }

    // A velocity route into SPEC AMT is worked out per voice: a hard note and a soft note morph differently.
    const auto render = [] (int velocity, bool routed)
    {
        IlanaSynthAudioProcessor processor;
        processor.setNonRealtime (true);
        setParam (processor, "sub_on", 0.0f);
        setParam (processor, "osc1_table", 10.0f);
        setParam (processor, "osc1_spectral", (float) SpectralWarp::LowPass);
        setParam (processor, "osc1_spectral_amt", 0.0f);

        if (routed)
        {
            setParam (processor, "mod1_src", (float) Mod::Source::Velocity);
            setParam (processor, "mod1_dst", (float) Mod::destinationForParamId ("osc1_spectral_amt"));
            setParam (processor, "mod1_amt", 0.8f);
        }

        processor.prepareToPlay (48000.0, 512);
        double centroid = 0.0;
        renderPeakAndCentroid (processor, 45, 40, centroid, nullptr, velocity);
        return centroid;
    };

    const auto soft = render (20, true);
    const auto hard = render (127, true);
    const auto plain = render (127, false);
    check (hard < soft * 0.9 && hard < plain * 0.9,
           "a velocity route into SPEC AMT morphs each note by its own amount (centroid soft " + juce::String (soft, 0)
               + " Hz, hard " + juce::String (hard, 0) + " Hz, unrouted " + juce::String (plain, 0) + " Hz)");
}

// 2026-10-30, ilana: "spectral warps dont automate cleanly, theyre stepping". An automated SPEC AMT
// sweep used to jump between 64 rebuilt copies of the table; now it builds finer copies while the amount
// moves and fades from each to the next. Measured as spectral flux (how much the spectrum changes from one
// block to the next): steps put it in a few blocks, a glide spreads it out. ILANA_SWEEP_WAV=<folder>
// writes each sweep as a WAV.
void runSpectralSweepTest()
{
    static const auto live = juce::SystemStats::getEnvironmentVariable ("ILANA_SWEEP_REALTIME", "").isNotEmpty();
    const auto sweep = [] (int mode, std::vector<float>& left, std::vector<float>& right)
    {
        // ILANA_SWEEP_REALTIME=1: the live path instead (the warps built on their worker, blocks paced at
        // about real time); printed only, as it depends on the machine.
        IlanaSynthAudioProcessor processor;
        processor.setNonRealtime (! live);
        setParam (processor, "sub_on", 0.0f);
        setParam (processor, "osc1_table", 10.0f);
        setParam (processor, "osc1_spectral", (float) mode);
        setParam (processor, "osc1_spectral_amt", 0.0f);
        processor.prepareToPlay (48000.0, 512);

        constexpr int blocks = 300; // 3.2 s: 0.3 s still, then 0 -> 1 over 2.6 s, then still
        juce::AudioBuffer<float> buffer (2, 512);

        for (int block = 0; block < blocks; ++block)
        {
            const auto t = juce::jlimit (0.0f, 1.0f, (float) (block - 28) / 244.0f);
            setParam (processor, "osc1_spectral_amt", t);
            buffer.clear();
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 45, (juce::uint8) 100), 0);
            processor.processBlock (buffer, midi);
            if (live)
                juce::Thread::sleep (10);

            for (int i = 0; i < 512; ++i)
            {
                left.push_back (buffer.getSample (0, i));
                right.push_back (buffer.getSample (1, i));
            }
        }
    };

    // Spectra of 2048-sample windows a block (512) apart, each scaled to sum 1; the flux between neighbours;
    // its 95th percentile over its median. Steps make a few windows change a lot: about 2.6-8 before the fix.
    const auto spikiness = [] (const std::vector<float>& samples, int first, int last)
    {
        constexpr int order = 11, size = 1 << order, hop = 512;
        juce::dsp::FFT fft (order);
        std::vector<float> previous, work ((size_t) size * 2);
        std::vector<double> flux;

        for (int start = first; start + size <= last; start += hop)
        {
            std::fill (work.begin(), work.end(), 0.0f);
            for (int i = 0; i < size; ++i)
                work[(size_t) i] = samples[(size_t) (start + i)] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size));
            fft.performFrequencyOnlyForwardTransform (work.data());
            double total = 0.0;
            for (int bin = 0; bin <= size / 2; ++bin)
                total += work[(size_t) bin];
            for (int bin = 0; bin <= size / 2; ++bin)
                work[(size_t) bin] = (float) (work[(size_t) bin] / juce::jmax (1.0e-9, total));

            if (! previous.empty())
            {
                double change = 0.0;
                for (int bin = 0; bin <= size / 2; ++bin)
                    change += std::abs (work[(size_t) bin] - previous[(size_t) bin]);
                flux.push_back (change);
            }

            previous.assign (work.begin(), work.begin() + size / 2 + 1);
        }

        auto sorted = flux;
        std::sort (sorted.begin(), sorted.end());
        return sorted[sorted.size() * 95 / 100] / juce::jmax (1.0e-12, sorted[sorted.size() / 2]);
    };

    const auto folder = juce::SystemStats::getEnvironmentVariable ("ILANA_SWEEP_WAV", "");

    for (const auto mode : { (int) SpectralWarp::HarmonicCut, (int) SpectralWarp::Shift, (int) SpectralWarp::LowPass,
                             (int) SpectralWarp::Stretch })
    {
        std::vector<float> left, right;
        sweep (mode, left, right);
        // The moving part only (from 0.4 s to 2.9 s).
        const auto score = spikiness (left, 48000 * 4 / 10, 48000 * 29 / 10);
        const auto name = SpectralWarp::getNames()[mode];

        if (folder.isNotEmpty())
        {
            juce::AudioBuffer<float> out (2, (int) left.size());
            std::copy (left.begin(), left.end(), out.getWritePointer (0));
            std::copy (right.begin(), right.end(), out.getWritePointer (1));
            auto file = juce::File (folder).getChildFile ("sweep-" + name.removeCharacters (" /") + ".wav");
            file.deleteFile();
            juce::WavAudioFormat wav;
            if (auto stream = std::unique_ptr<juce::OutputStream> (file.createOutputStream()))
                if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (stream.get(), 48000.0, 2, 24, {}, 0)))
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
                }
        }

        std::cout << "  spectral sweep " << name << ": flux 95th percentile / median " << juce::String (score, 2) << std::endl;
        if (! live)
            check (score < 2.0, "an automated " + name + " sweep glides instead of stepping (flux 95th percentile / median "
                                + juce::String (score, 2) + ")");
    }

    // The bank's fade itself: switching between two warped copies mid-note leaves no jump in the wave.
    {
        Wavetable base;
        base.buildFromFrames (TableFactory::generate (10));
        std::unique_ptr<Wavetable> dark (SpectralWarp::warpTable (base, SpectralWarp::LowPass, 0.9f));
        std::unique_ptr<Wavetable> bright (SpectralWarp::warpTable (base, SpectralWarp::LowPass, 0.0f));
        const auto render = [&] (bool fromCache)
        {
            UnisonBank bank;
            bank.setSampleRate (48000.0);
            bank.setWavetable (dark.get(), true);
            const float gain = 1.0f, panL = 1.0f, panR = 1.0f;
            bank.setWeights (&gain, &panL, &panR, 1);
            const double ratio = 1.0;
            bank.setFrequencies (110.0, &ratio, 1);
            WavetableOscillator::FrameRead frames;
            auto jump = 0.0f, last = 0.0f;
            for (int i = 0; i < 9600; ++i)
            {
                if (i == 4800)
                {
                    bank.setWavetable (bright.get(), fromCache);
                    bank.setFrequencies (110.0, &ratio, 1);
                }
                const auto value = bank.render (0.0, frames, 1.0, false).mono;
                if (i > 4790 && i < 4810)
                    jump = juce::jmax (jump, std::abs (value - last));
                last = value;
            }
            return jump;
        };
        const auto hard = render (false), faded = render (true);
        check (faded < hard * 0.5f, "a switch between warped copies fades (largest step " + juce::String (hard, 3)
                                        + " hard, " + juce::String (faded, 3) + " faded)");
    }

    // A voice that stopped mid-fade and restarts on the same copy does not
    // keep fading from the old one (it may have been freed: bug hunt
    // 2026-10-31, a use-after-free that crashed).
    {
        Wavetable base;
        base.buildFromFrames (TableFactory::generate (10));
        std::unique_ptr<Wavetable> first (SpectralWarp::warpTable (base, SpectralWarp::LowPass, 0.2f));
        std::unique_ptr<Wavetable> second (SpectralWarp::warpTable (base, SpectralWarp::LowPass, 0.4f));
        UnisonBank bank;
        bank.setSampleRate (48000.0);
        const float gain = 1.0f, pan = 1.0f;
        const double ratio = 1.0;
        bank.setWeights (&gain, &pan, &pan, 1);
        bank.setWavetable (first.get(), true);
        bank.setFrequencies (110.0, &ratio, 1);
        WavetableOscillator::FrameRead frames;
        for (int i = 0; i < 4800; ++i)
            bank.render (0.0, frames, 1.0, false);
        bank.setWavetable (second.get(), true);
        for (int i = 0; i < 480; ++i)
            bank.render (0.0, frames, 1.0, false);
        const auto fadingAtStop = bank.isFadingTable();
        bank.switchTableHard();
        bank.setWavetable (second.get(), true);
        check (fadingAtStop && ! bank.isFadingTable(), "a new note drops the last note's unfinished table fade");
    }
}

void runStereoModulationTest()
{
    // A stereo route on a filter cutoff opens one side and closes the other; the same route unflagged moves both alike.
    const auto sideDifference = [] (bool stereo)
    {
        IlanaSynthAudioProcessor processor;
        processor.setNonRealtime (true);
        setParam (processor, "sub_on", 0.0f);
        setParam (processor, "f1_cutoff", 400.0f);
        setParam (processor, "mod1_src", (float) Mod::Source::Velocity);
        setParam (processor, "mod1_dst", (float) Mod::Destination::Filter1Cutoff);
        setParam (processor, "mod1_amt", 1.0f);
        setParam (processor, "mod1_stereo", stereo ? 1.0f : 0.0f);
        processor.prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> buffer (2, 512);
        auto left = 0.0, right = 0.0;

        for (int block = 0; block < 40; ++block)
        {
            buffer.clear();
            juce::MidiBuffer midi;

            if (block == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 57, (juce::uint8) 127), 0);

            processor.processBlock (buffer, midi);

            if (block > 10)
                for (int i = 0; i < 512; ++i)
                {
                    left += (double) buffer.getSample (0, i) * buffer.getSample (0, i);
                    right += (double) buffer.getSample (1, i) * buffer.getSample (1, i);
                }
        }

        check (std::isfinite (left) && std::isfinite (right) && left + right > 0.0, "stereo-routed filter renders audio");
        return std::abs (std::log ((left + 1.0e-9) / (right + 1.0e-9)));
    };

    const auto off = sideDifference (false);
    const auto on = sideDifference (true);
    check (on > off + 0.3,
           "a stereo cutoff route makes the left and right levels differ (log ratio off " + juce::String (off, 2)
               + ", on " + juce::String (on, 2) + ")");
}

// Stereo everywhere (2026-10-30, ilana: "stereo frame, stereo everything"): a stereo route into a filter's
// resonance, morph or drive, or a wavetable oscillator's FRAME or WARP, puts energy in the side (L - R); the same
// route unflagged leaves the patch mono. A stereo flag at depth 0 on a one-voice oscillator (the left / right
// twin) sounds as the plain voice.
static double stereoSideShare (int destination, bool stereo, float depth, std::function<void (IlanaSynthAudioProcessor&)> setup,
                               juce::AudioBuffer<float>* keep = nullptr, Mod::Source source = Mod::Source::Velocity)
{
    IlanaSynthAudioProcessor processor;
    processor.setNonRealtime (true);
    setParam (processor, "sub_on", 0.0f);
    if (setup != nullptr)
        setup (processor);
    setParam (processor, "mod1_src", (float) source);
    setParam (processor, "mod1_dst", (float) destination);
    setParam (processor, "mod1_amt", depth);
    setParam (processor, "mod1_stereo", stereo ? 1.0f : 0.0f);
    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    auto side = 0.0, total = 0.0;
    if (keep != nullptr)
        keep->setSize (2, 512 * 30);
    for (int block = 0; block < 30; ++block)
    {
        buffer.clear();
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 57, (juce::uint8) 127), 0);
        processor.processBlock (buffer, midi);
        if (keep != nullptr)
            for (int ch = 0; ch < 2; ++ch)
                keep->copyFrom (ch, block * 512, buffer, ch, 0, 512);
        if (block > 8)
            for (int i = 0; i < 512; ++i)
            {
                const auto l = (double) buffer.getSample (0, i), r = (double) buffer.getSample (1, i);
                side += (l - r) * (l - r);
                total += l * l + r * r;
            }
    }
    check (std::isfinite (side) && total > 0.0, "stereo route renders audio (destination " + juce::String (destination) + ")");
    return side / (total + 1.0e-12);
}

void runStereoEverywhereTest()
{
    using D = Mod::Destination;
    const auto resonant = [] (IlanaSynthAudioProcessor& p) { setParam (p, "f1_cutoff", 900.0f); setParam (p, "f1_reso", 0.4f); };
    const auto morphing = [] (IlanaSynthAudioProcessor& p) { setParam (p, "f1_cutoff", 900.0f); setParam (p, "f1_type", 11.0f); setParam (p, "f1_morph", 0.5f); };
    const auto driven = [] (IlanaSynthAudioProcessor& p) { setParam (p, "f1_cutoff", 2000.0f); setParam (p, "f1_drive", 2.0f); };
    const auto warped = [] (IlanaSynthAudioProcessor& p) { setParam (p, "osc1_warp", 1.0f); setParam (p, "osc1_warp_amt", 0.3f); };
    struct Case { D destination; std::function<void (IlanaSynthAudioProcessor&)> setup; const char* name; };
    const Case cases[] { { D::Filter1Reso, resonant, "Filter 1 resonance" }, { D::Filter1Morph, morphing, "Filter 1 morph" },
                         { D::Filter1Drive, driven, "Filter 1 drive" }, { D::Osc1Frame, nullptr, "OSC 1 frame (one voice: the twin)" },
                         { D::Osc1Warp, warped, "OSC 1 warp (one voice: the twin)" } };
    for (const auto& c : cases)
    {
        const auto off = stereoSideShare ((int) c.destination, false, 0.6f, c.setup);
        const auto on = stereoSideShare ((int) c.destination, true, 0.6f, c.setup);
        check (off < 1.0e-6 && on > 1.0e-3,
               juce::String ("a stereo route into ") + c.name + " splits left and right (side share off " + juce::String (off, 7)
                   + ", on " + juce::String (on, 5) + ")");
    }

    // The twin at depth 0 plays the plain voice.
    juce::AudioBuffer<float> plain, twin;
    stereoSideShare ((int) D::Osc1Frame, false, 0.0f, nullptr, &plain);
    stereoSideShare ((int) D::Osc1Frame, true, 0.0f, nullptr, &twin);
    auto worst = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < plain.getNumSamples(); ++i)
            worst = juce::jmax (worst, std::abs (plain.getSample (ch, i) - twin.getSample (ch, i)));
    check (worst < 1.0e-5f, "a one-voice oscillator's stereo twin at depth 0 sounds as the plain voice (largest difference "
                                + juce::String (worst, 8) + ")");
}

// Stereo physical strings (2026-10-30, ilana: "cant physical oscs still do stereo"): a stereo route into a string
// knob plays one string as a left / right twin (or spreads the unison strings); unflagged it stays mono, and the
// twin at depth 0 is the plain string.
void runStereoStringsTest()
{
    const auto string = [] (int excite, int unison)
    {
        return [excite, unison] (IlanaSynthAudioProcessor& p)
        {
            setParam (p, "osc1_mode", 1.0f);
            setParam (p, "osc1_excite", (float) excite);
            setParam (p, "osc1_unison", (float) unison);
            setParam (p, "osc1_spread", 1.0f);
            setParam (p, "osc1_string_stiffness", 0.3f);
            setParam (p, "osc1_string_excite_pos", 0.3f);
            setParam (p, "osc1_string_decay", 0.6f);
            setParam (p, "macro1", 1.0f);
        };
    };
    // (EXCITE POSITION shapes the pluck as the note starts: a macro, already up then, drives every case.)
    struct Case { const char* id; int excite; int unison; const char* name; };
    const Case cases[] { { "osc1_string_decay", 0, 1, "decay (pluck twin)" }, { "osc1_string_damp", 0, 1, "damping (pluck twin)" },
                         { "osc1_string_stiffness", 0, 1, "stiffness (pluck twin)" },
                         { "osc1_string_excite_pos", 0, 1, "excite position (pluck twin)" },
                         { "osc1_bow_pressure", 4, 1, "bow pressure (bow twin)" }, { "osc1_hammer_hard", 9, 1, "hammer (piano twin)" },
                         { "osc1_string_damp", 0, 3, "damping (three unison strings)" } };
    for (const auto& c : cases)
    {
        const auto destination = Mod::destinationForParamId (c.id);
        check (Mod::canBeStereoAnywhere (destination), juce::String ("the matrix offers STEREO on ") + c.id);
        // Unison strings sit apart already; there the flag must add to the side, not make it.
        const auto off = stereoSideShare (destination, false, 0.5f, string (c.excite, c.unison), nullptr, Mod::Source::Macro1);
        const auto on = stereoSideShare (destination, true, 0.5f, string (c.excite, c.unison), nullptr, Mod::Source::Macro1);
        check (c.unison > 1 ? on > off * 1.15 + 1.0e-4 : (off < 1.0e-6 && on > 1.0e-3),
               juce::String ("a stereo route into a string's ") + c.name + " splits left and right (side share off "
                   + juce::String (off, 7) + ", on " + juce::String (on, 5) + ")");
    }

    juce::AudioBuffer<float> plain, twin;
    const auto decay = Mod::destinationForParamId ("osc1_string_decay");
    // (A route from the mod wheel, at rest: the twin plays, with nothing to split it.)
    stereoSideShare (decay, false, 0.5f, string (0, 1), &plain, Mod::Source::ModWheel);
    stereoSideShare (decay, true, 0.5f, string (0, 1), &twin, Mod::Source::ModWheel);
    auto worst = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < plain.getNumSamples(); ++i)
            worst = juce::jmax (worst, std::abs (plain.getSample (ch, i) - twin.getSample (ch, i)));
    check (worst < 1.0e-5f, "a string's stereo twin with its route at rest sounds as the plain string (largest difference "
                                + juce::String (worst, 8) + ")");
}

// The twin in one call (KarplusStrong::processPair, 2026-10-30, ilana: "make stereo cheaper"): both sides played
// together must be exactly the two strings played one after the other, for every exciter (the shared paths and
// the ones that fall back), with the knobs moving mid-note, STIFF and the pickup off, and the key released.
void runStringPairTest()
{
    using KS = KarplusStrong;
    struct Knobs { float damp, decay, stiff, pickup, position, hammer, bow; };
    const auto set = [] (KS& k, int excite, const Knobs& n)
    {
        k.setParams ((KS::Excite) excite, 0.5f, n.damp, n.decay);
        k.setPhysicalParams (n.stiff, n.pickup, n.position, 0.6f, 0.0f, false);
        k.setBowAndBuzz (n.bow, 0.5f, 0.0f, 0.0f);
        k.setKeysParams (n.hammer, 0.3f);
        k.setEco (false);
    };
    auto worst = 0.0f;
    juce::String where;
    for (int excite = 0; excite < KS::numExcites; ++excite)
        for (int variant = 0; variant < 2; ++variant)
            for (const auto hz : { 55.0, 196.0, 1760.0 })
            {
                const Knobs left { 0.25f, 0.85f, variant == 0 ? 0.45f : 0.0f, variant == 0 ? 0.1f : 0.0f, 0.2f, 0.4f, 0.4f };
                const Knobs right { 0.55f, 0.70f, variant == 0 ? 0.15f : 0.0f, variant == 0 ? 0.3f : 0.0f, 0.3f, 0.6f, 0.6f };
                KS a (5), b (5), pairLeft (5), pairRight (5);
                for (auto* k : { &a, &b, &pairLeft, &pairRight })
                    k->prepare (48000.0);
                set (a, excite, left); set (pairLeft, excite, left);
                set (b, excite, right); set (pairRight, excite, right);
                for (auto* k : { &a, &b, &pairLeft, &pairRight })
                {
                    k->setFrequency (hz);
                    k->trigger (0.8f);
                }
                for (int i = 0; i < 24000; ++i)
                {
                    if (i == 8000)
                    {
                        auto movedLeft = left, movedRight = right;
                        movedLeft.damp += 0.1f;
                        movedLeft.pickup = 0.2f;
                        movedRight.decay -= 0.05f;
                        set (a, excite, movedLeft); set (pairLeft, excite, movedLeft);
                        set (b, excite, movedRight); set (pairRight, excite, movedRight);
                    }
                    const auto held = i < 20000;
                    const auto input = 0.3f * std::sin ((float) i * 0.05f);
                    const auto expectedLeft = a.process (0.0f, held, input);
                    const auto expectedRight = b.process (0.0f, held, input);
                    float gotLeft = 0.0f, gotRight = 0.0f;
                    pairLeft.processPair (pairRight, 0.0f, held, input, gotLeft, gotRight);
                    const auto difference = juce::jmax (std::abs (gotLeft - expectedLeft), std::abs (gotRight - expectedRight));
                    if (difference > worst)
                    {
                        worst = difference;
                        where = "exciter " + juce::String (excite) + " at " + juce::String (hz) + " Hz";
                    }
                }
            }
    check (worst == 0.0f, "a stereo twin played in one call is the two strings bit for bit"
                              + (worst > 0.0f ? " (largest difference " + juce::String (worst, 9) + ", " + where + ")" : juce::String()));
}

// CPU round 2026-10-31: a stereo route's twin string sleeps while the route
// rests (bit for bit the same), and a held note whose strings have died away
// stops rendering (its output differs only below -110 dB).
void runCpuSleepTests()
{
    const auto render = [] (int excite, bool stereo, int moveAt, bool twinSleep, bool heldSleep, double& seconds)
    {
        Voice::disableTwinSleep = ! twinSleep;
        Voice::disableHeldSilence = ! heldSleep;
        IlanaSynthAudioProcessor processor;
        processor.setNonRealtime (true);
        setParam (processor, "sub_on", 0.0f);
        setParam (processor, "osc1_mode", 1.0f);
        setParam (processor, "osc1_excite", (float) excite);
        setParam (processor, "osc1_string_decay", 0.3f);
        setParam (processor, "macro1", 0.0f);
        setParam (processor, "mod1_src", (float) Mod::Source::Macro1);
        setParam (processor, "mod1_dst", (float) Mod::destinationForParamId ("osc1_string_damp"));
        setParam (processor, "mod1_amt", 0.3f);
        setParam (processor, "mod1_stereo", stereo ? 1.0f : 0.0f);
        processor.prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> buffer (2, 256);
        std::vector<float> out;
        for (int block = 0; block < 1500; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                for (const auto note : { 52, 59, 64 })
                    midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
            if (block == 1300)
                for (const auto note : { 52, 59, 64 })
                    midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
            if (moveAt >= 0 && block == moveAt)
                setParam (processor, "macro1", 0.7f);
            buffer.clear();
            const auto start = juce::Time::getMillisecondCounterHiRes();
            processor.processBlock (buffer, midi);
            seconds += (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
            for (int channel = 0; channel < 2; ++channel)
                out.insert (out.end(), buffer.getReadPointer (channel), buffer.getReadPointer (channel) + 256);
        }
        Voice::disableTwinSleep = Voice::disableHeldSilence = false;
        return out;
    };
    const auto largest = [] (const std::vector<float>& a, const std::vector<float>& b)
    {
        auto diff = 0.0f;
        for (size_t i = 0; i < a.size(); ++i)
            diff = juce::jmax (diff, std::abs (a[i] - b[i]));
        return diff;
    };

    // The twin (pluck, Piano, Modal Piano), the route resting all along or
    // moving at block 40: bit for bit what the awake twin plays.
    auto worstTwin = 0.0f;
    juce::String where;
    for (const auto excite : { 0, 9, 11 })
        for (const auto moveAt : { -1, 40 })
        {
            double unused = 0.0;
            const auto difference = largest (render (excite, true, moveAt, false, false, unused),
                                             render (excite, true, moveAt, true, false, unused));
            if (difference > worstTwin)
            {
                worstTwin = difference;
                where = " (exciter " + juce::String (excite) + ", " + juce::String (difference, 9) + ")";
            }
        }
    check (worstTwin == 0.0f, "a resting stereo string route's twin sleeps, bit for bit the same" + where);

    // Held sleep: dying plucks held 5 s, then let go: the same output to
    // within -110 dB, and cheaper.
    double awakeSeconds = 0.0, sleepSeconds = 0.0;
    auto worstHeld = 0.0f;
    for (int round = 0; round < 3; ++round)
    {
        const auto a = render (0, false, -1, true, false, awakeSeconds);
        const auto b = render (0, false, -1, true, true, sleepSeconds);
        worstHeld = juce::jmax (worstHeld, largest (a, b));
    }
    check (worstHeld < 3.0e-6f, "a held note whose string died away sleeps, within -110 dB (largest "
                                    + juce::String (juce::Decibels::gainToDecibels (worstHeld, -300.0f), 1) + " dB)");
    check (sleepSeconds < awakeSeconds, "held sleep is cheaper (" + juce::String (sleepSeconds / awakeSeconds, 2) + "x the time)");
}

// ilana 2026-10-31 ("fix this"): a held note asleep after its string died
// wakes when a filter is then pushed into self-oscillation, and screams as
// it would have awake.
void runHeldSleepFilterWakeTest()
{
    const auto lateLevel = [] (int type, bool sleep, float& screamDb)
    {
        Voice::disableHeldSilence = ! sleep;
        IlanaSynthAudioProcessor processor;
        processor.setNonRealtime (true);
        setParam (processor, "sub_on", 0.0f);
        setParam (processor, "osc1_mode", 1.0f);
        setParam (processor, "osc1_excite", 0.0f);
        setParam (processor, "osc1_string_decay", 0.3f);
        setParam (processor, "f1_type", (float) type);
        setParam (processor, "f1_cutoff", 800.0f);
        setParam (processor, "f1_reso", 0.1f);
        processor.prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> buffer (2, 256);
        auto peak = 0.0f;
        for (int block = 0; block < 1500; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 52, (juce::uint8) 100), 0);
            if (block == 900)
                setParam (processor, "f1_reso", 1.0f);
            buffer.clear();
            processor.processBlock (buffer, midi);
            if (block >= 1200)
                peak = juce::jmax (peak, buffer.getMagnitude (0, 0, 256));
        }
        Voice::disableHeldSilence = false;
        screamDb = juce::Decibels::gainToDecibels (peak, -200.0f);
    };
    auto checked = 0;
    for (int type = 0; type < 40; ++type)
    {
        float awake = 0.0f, asleep = 0.0f;
        lateLevel (type, false, awake);
        if (awake < -40.0f)
            continue; // this model does not self-oscillate here
        lateLevel (type, true, asleep);
        if (juce::SystemStats::getEnvironmentVariable ("ILANA_WAKE_PRINT", "").isNotEmpty())
            std::cout << "  filter type " << type << ": awake " << awake << " dB, slept " << asleep << " dB" << std::endl;
        check (asleep > awake - 6.0f, "filter type " + juce::String (type) + " pushed into self-oscillation wakes a sleeping held note ("
                                          + juce::String (asleep, 1) + " dB against " + juce::String (awake, 1) + " dB awake)");
        ++checked;
    }
    check (checked > 0, "some filter self-oscillates in the wake test (" + juce::String (checked) + " models)");
}

// Review 2026-11-01: held sleep listens to the strings, not only to the
// output. A pluck the amp holds silent for its first second (amp DELAY), or
// whose level a route holds at zero, still rings, so it must not sleep: it
// sounds when the amp opens (or the route lets go), as it would awake.
void runHeldSleepGainTest()
{
    const auto peakAfter = [] (int scenario, bool sleep)
    {
        Voice::disableHeldSilence = ! sleep;
        IlanaSynthAudioProcessor processor;
        processor.setNonRealtime (true);
        setParam (processor, "sub_on", 0.0f);
        setParam (processor, "osc1_mode", 1.0f);
        setParam (processor, "osc1_excite", 0.0f);
        setParam (processor, "osc1_string_decay", 0.9f);
        if (scenario == 0)
            setParam (processor, "amp_delay", 1.0f);
        else
        {
            setParam (processor, "macro1", 1.0f);
            processor.assignModSlot ((int) Mod::Source::Macro1, (int) Mod::Destination::Osc1Level, -1.0f);
        }
        processor.prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> buffer (2, 256);
        auto peak = 0.0f;
        for (int block = 0; block < 300; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 52, (juce::uint8) 100), 0);
            if (scenario == 1 && block == 100)
                setParam (processor, "macro1", 0.0f);
            buffer.clear();
            processor.processBlock (buffer, midi);
            // (The amp opens at block 188, the route lets go at 100.)
            if (block >= (scenario == 0 ? 190 : 102) && block < (scenario == 0 ? 290 : 202))
                peak = juce::jmax (peak, buffer.getMagnitude (0, 0, 256));
        }
        Voice::disableHeldSilence = false;
        return juce::Decibels::gainToDecibels (peak, -200.0f);
    };
    for (int scenario = 0; scenario < 2; ++scenario)
    {
        const auto awake = peakAfter (scenario, false), slept = peakAfter (scenario, true);
        const juce::String what = scenario == 0 ? "an amp DELAY" : "a route holding the level at zero";
        check (awake > -60.0f, "held sleep gain test: the pluck sounds after " + what + " (" + juce::String (awake, 1) + " dB)");
        check (std::abs (slept - awake) < 0.5f, "a held pluck behind " + what + " does not sleep (" + juce::String (slept, 1)
                                                    + " dB against " + juce::String (awake, 1) + " dB awake)");
    }
}

// ILANA_STEREO_BENCH=1: what a stereo string route costs a whole voice (8 notes of a pluck and of the Piano
// exciter, with and without a stereo route into DAMP), for docs/ROUTING-CPU.md.
void runStereoStringBench()
{
    for (const auto excite : { 0, 4, 9 })
    {
        double best[2] { 1.0e9, 1.0e9 };
        const auto rounds = juce::SystemStats::getEnvironmentVariable ("ILANA_STEREO_BENCH_ROUNDS", "5").getIntValue();
        for (int round = 0; round < rounds; ++round)
            for (int stereo = 0; stereo < 2; ++stereo)
            {
                // (ILANA_STEREO_BENCH_ONLY=0 or 1 times one side, for an instruction count.)
                const auto only = juce::SystemStats::getEnvironmentVariable ("ILANA_STEREO_BENCH_ONLY", "");
                if (only.isNotEmpty() && only.getIntValue() != stereo)
                    continue;
                IlanaSynthAudioProcessor processor;
                processor.setNonRealtime (true);
                setParam (processor, "sub_on", 0.0f);
                setParam (processor, "osc1_mode", 1.0f);
                setParam (processor, "osc1_excite", (float) excite);
                setParam (processor, "osc1_string_stiffness", 0.3f);
                setParam (processor, "osc1_string_decay", 0.8f);
                setParam (processor, "macro1", 1.0f);
                setParam (processor, "mod1_src", (float) Mod::Source::Macro1);
                setParam (processor, "mod1_dst", (float) Mod::destinationForParamId ("osc1_string_damp"));
                setParam (processor, "mod1_amt", 0.2f);
                setParam (processor, "mod1_stereo", (float) stereo);
                processor.prepareToPlay (48000.0, 256);
                juce::AudioBuffer<float> buffer (2, 256);
                double seconds = 0.0;
                for (int block = 0; block < 750; ++block)
                {
                    juce::MidiBuffer midi;
                    if (block % 188 == 0)
                        for (const auto note : { 40, 47, 52, 55, 59, 64, 67, 71 })
                            midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
                    buffer.clear();
                    const auto start = juce::Time::getMillisecondCounterHiRes();
                    processor.processBlock (buffer, midi);
                    seconds += (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
                }
                best[stereo] = juce::jmin (best[stereo], seconds);
            }
        std::printf ("stereo string bench, %s: mono %.1f ms, stereo route %.1f ms (+%.0f%%)\n", excite == 0 ? "pluck" : excite == 4 ? "bow" : "piano",
                     best[0] * 1000.0, best[1] * 1000.0, 100.0 * (best[1] / best[0] - 1.0));
    }
}

void runPerVoiceLfoSpectralTest()
{
    // An LFO that retriggers per note, routed into SPEC AMT, morphs each note from its own start; a free LFO does not.
    const auto brightness = [] (bool retrig, int delayBlocks)
    {
        IlanaSynthAudioProcessor processor;
        processor.setNonRealtime (true);
        setParam (processor, "sub_on", 0.0f);
        setParam (processor, "osc1_table", 10.0f);
        setParam (processor, "osc1_spectral", (float) SpectralWarp::LowPass);
        setParam (processor, "osc1_spectral_amt", 0.4f);
        setParam (processor, "lfo1_rate", 2.0f);
        setParam (processor, "lfo1_retrig", retrig ? 1.0f : 0.0f);
        setParam (processor, "mod1_src", (float) Mod::Source::Lfo1);
        setParam (processor, "mod1_dst", (float) Mod::destinationForParamId ("osc1_spectral_amt"));
        setParam (processor, "mod1_amt", 0.9f);
        processor.prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> buffer (2, 512);
        auto energy = 0.0, change = 0.0;

        for (int block = 0; block < delayBlocks + 60; ++block)
        {
            buffer.clear();
            juce::MidiBuffer midi;

            if (block == delayBlocks)
                midi.addEvent (juce::MidiMessage::noteOn (1, 45, (juce::uint8) 100), 0);

            processor.processBlock (buffer, midi);

            if (block >= delayBlocks + 40)
                for (int i = 1; i < 512; ++i)
                {
                    const auto x = (double) buffer.getSample (0, i), d = x - buffer.getSample (0, i - 1);
                    energy += x * x;
                    change += d * d;
                }
        }

        return std::sqrt (change / juce::jmax (1.0e-12, energy));
    };

    const auto retrigGap = std::abs (brightness (true, 0) - brightness (true, 23)) / brightness (true, 0);
    const auto freeGap = std::abs (brightness (false, 0) - brightness (false, 23)) / brightness (false, 0);
    check (retrigGap < 0.05 && freeGap > 0.1,
           "a retriggered LFO into SPEC AMT morphs from each note's start (late-note gap retrig " + juce::String (retrigGap, 3)
               + ", free " + juce::String (freeGap, 3) + ")");
}

void runAudioImportModesTest()
{
    // Vocode: each frame's harmonics follow the audio spectrum at that point (bin 100 early, bin 300 late).
    {
        std::vector<float> audio (16384);
        for (size_t i = 0; i < audio.size(); ++i)
        {
            const auto bin = i < audio.size() / 2 ? 100.0 : 300.0;
            audio[i] = (float) std::sin (2.0 * juce::MathConstants<double>::pi * bin * (double) i / 2048.0);
        }

        std::vector<std::vector<float>> frames;
        Wavetable::vocode (audio, frames);
        check (frames.size() == 32, "vocode makes 32 frames");

        const auto peakHarmonic = [] (const std::vector<float>& frame)
        {
            TableFFT fft (11);
            std::vector<std::complex<float>> in (2048), out (2048);
            for (size_t i = 0; i < 2048; ++i)
                in[i] = frame[i];
            fft.perform (in.data(), out.data(), false);
            int best = 1;
            for (int k = 1; k < 1024; ++k)
                if (std::abs (out[(size_t) k]) > std::abs (out[(size_t) best]))
                    best = k;
            return best;
        };

        if (frames.size() == 32)
        {
            const auto early = peakHarmonic (frames.front()), late = peakHarmonic (frames.back());
            check (std::abs (early - 100) <= 2 && std::abs (late - 300) <= 2,
                   "vocode frames follow the spectrum (peak harmonic " + juce::String (early) + " -> " + juce::String (late) + ")");
            auto peak = 0.0f;
            for (const auto v : frames[10])
                peak = juce::jmax (peak, std::abs (v));
            check (peak > 0.85f && peak < 0.95f, "vocode frames are normalised");
        }
    }

    // Time slice: equal slices in time order, each squeezed to one cycle.
    {
        std::vector<float> audio (32 * 500);
        for (size_t i = 0; i < audio.size(); ++i)
            audio[i] = (float) i / (float) audio.size();

        std::vector<std::vector<float>> frames;
        Wavetable::timeSlice (audio, frames);
        check (frames.size() == 32 && frames[0].size() == 2048, "time slice makes 32 frames of 2048");

        if (frames.size() == 32)
            check (std::abs (frames[16][0] - 0.5f) < 0.01f && frames[31][0] > frames[1][0],
                   "time slice frames follow the file in time order");
    }
}

void runUnisonTests()
{
    // 16 voices render, and blend 0 leaves only the centre voice(s).
    const auto widthOf = [] (int voices, float blend, int mode)
    {
        IlanaSynthAudioProcessor processor;
        setParam (processor, "sub_on", 0.0f);
        setParam (processor, "osc1_unison", (float) voices);
        setParam (processor, "osc1_detune", 30.0f);
        setParam (processor, "osc1_spread", 1.0f);
        setParam (processor, "osc1_uni_blend", blend);
        setParam (processor, "osc1_uni_mode", (float) mode);
        processor.prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> buffer (2, 512);
        auto side = 0.0, mid = 0.0;

        for (int block = 0; block < 60; ++block)
        {
            buffer.clear();
            juce::MidiBuffer midi;

            if (block == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 57, (juce::uint8) 100), 0);

            processor.processBlock (buffer, midi);

            if (block > 10)
                for (int i = 0; i < 512; ++i)
                {
                    const auto l = buffer.getSample (0, i), r = buffer.getSample (1, i);
                    mid += (double) (l + r) * (l + r);
                    side += (double) (l - r) * (l - r);
                }
        }

        return std::sqrt (side / juce::jmax (1.0e-12, mid));
    };

    // Every stack mode renders bounded audio; the interval stacks change the sound.
    {
        const auto render = [] (int mode)
        {
            IlanaSynthAudioProcessor processor;
            setParam (processor, "sub_on", 0.0f);
            setParam (processor, "osc1_unison", 5.0f);
            setParam (processor, "osc1_detune", 10.0f);
            setParam (processor, "osc1_uni_mode", (float) mode);
            processor.prepareToPlay (48000.0, 512);
            double centroid = 0.0;
            const auto peak = renderPeakAndCentroid (processor, 45, 40, centroid);
            return std::make_pair (peak, centroid);
        };

        const auto classic = render (UnisonMode::Classic);
        auto all = true, different = true;

        for (int mode = UnisonMode::CenterDrop; mode < UnisonMode::Count; ++mode)
        {
            const auto result = render (mode);
            all = all && std::isfinite (result.first) && result.first > 0.02f && result.first < 4.0f;
            different = different && std::abs (result.second - classic.second) > 1.0;
        }

        check (all, "every appended unison stack mode renders bounded audio");
        check (different, "the appended unison stack modes change the sound against Classic");
    }

    // Frame and warp spreads change a stack's sound, stay bounded, and do nothing at 0 or with one voice.
    {
        const auto render = [] (int voices, const char* spreadId, float spread)
        {
            IlanaSynthAudioProcessor processor;
            setParam (processor, "sub_on", 0.0f);
            setParam (processor, "osc1_unison", (float) voices);
            setParam (processor, "osc1_detune", 10.0f);
            setParam (processor, "osc1_table", 6.0f);
            setParam (processor, "osc1_frame", 0.5f);
            setParam (processor, "osc1_warp", 7.0f);
            setParam (processor, "osc1_warp_amt", 0.3f);
            setParam (processor, spreadId, spread);
            processor.prepareToPlay (48000.0, 512);
            double centroid = 0.0;
            const auto peak = renderPeakAndCentroid (processor, 45, 40, centroid);
            return std::make_pair (peak, centroid);
        };

        for (const auto* id : { "osc1_uni_frame", "osc1_uni_warp" })
        {
            const auto off = render (5, id, 0.0f);
            const auto on = render (5, id, 1.0f);
            const auto single = render (1, id, 1.0f);
            const auto singleOff = render (1, id, 0.0f);
            check (std::isfinite (on.first) && on.first > 0.02f && on.first < 4.0f && std::abs (on.second - off.second) > 1.0,
                   juce::String (id) + " changes a five-voice stack and stays bounded (centroid "
                       + juce::String (off.second, 0) + " -> " + juce::String (on.second, 0) + " Hz)");
            check (std::abs (single.second - singleOff.second) < 0.5,
                   juce::String (id) + " leaves a single voice alone");
        }
    }

    // Scale quantise: C major snaps note 61 (C#) to C or D, so the pitch moves by a semitone from the plain note.
    {
        const auto frequencyOf = [] (int scale, int note)
        {
            IlanaSynthAudioProcessor processor;
            setParam (processor, "sub_on", 0.0f);
            setParam (processor, "osc1_table", 0.0f);
            setParam (processor, "osc1_scale", (float) scale);
            processor.prepareToPlay (48000.0, 512);
            double centroid = 0.0;
            renderPeakAndCentroid (processor, note, 40, centroid);
            return centroid;
        };

        const auto plain = frequencyOf (0, 61);
        const auto snapped = frequencyOf (1, 61);
        const auto inKey = frequencyOf (1, 60);
        const auto inKeyPlain = frequencyOf (0, 60);
        check (std::abs (snapped - plain) > 1.0 && std::abs (inKey - inKeyPlain) < 0.5,
               "an oscillator's scale snap moves an out-of-key note and leaves an in-key one (61: "
                   + juce::String (plain, 0) + " -> " + juce::String (snapped, 0) + " Hz)");
    }

    const auto full16 = widthOf (16, 1.0f, UnisonMode::Classic);
    const auto blend0 = widthOf (15, 0.0f, UnisonMode::Classic);
    const auto hyper = widthOf (16, 1.0f, UnisonMode::Hypersaw);
    const auto octaves = widthOf (9, 1.0f, UnisonMode::Octaves);

    check (full16 > 0.2 && std::isfinite (hyper) && hyper > 0.1 && std::isfinite (octaves) && octaves > 0.1,
           "16-voice unison, hypersaw and octave stacks render wide (" + juce::String (full16, 2) + ", "
               + juce::String (hyper, 2) + ", " + juce::String (octaves, 2) + ")");
    check (blend0 < 0.01, "unison blend 0 keeps only the centre voice (width " + juce::String (blend0, 4) + ")");
}

void runPerVoiceLfoTest()
{
    // Two notes started half a second apart with a retriggered LFO on pitch:
    // each voice's LFO starts at its own note-on, so a slow LFO puts the two
    // notes at different points of the cycle. Checked through the voices.
    IlanaSynthAudioProcessor processor;
    setParam (processor, "lfo1_retrig", 1.0f);
    setParam (processor, "lfo1_rate", 0.5f);
    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);

    for (int block = 0; block < 50; ++block)
    {
        buffer.clear();
        juce::MidiBuffer midi;

        if (block == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);

        if (block == 47)
            midi.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100), 0);

        processor.processBlock (buffer, midi);
    }

    const auto phases = processor.getVoiceLfoPhasesForTest (0);
    check (phases.size() == 2 && std::abs (phases[0] - phases[1]) > 0.2f,
           "retriggered LFO runs per voice (phases " + juce::String (phases.size() > 0 ? phases[0] : -1.0f, 3) + ", "
               + juce::String (phases.size() > 1 ? phases[1] : -1.0f, 3) + ")");
}

void runMatrixTests()
{
    const auto levelWith = [] (std::function<void (IlanaSynthAudioProcessor&)> setup)
    {
        IlanaSynthAudioProcessor processor;
        setParam (processor, "master_clip", 0.0f);
        setup (processor);
        processor.prepareToPlay (48000.0, 512);
        double centroid = 0.0;
        return renderPeakAndCentroid (processor, 57, 30, centroid);
    };

    const auto base = levelWith ([] (auto&) {});

    // Slot 20 works like slot 1: macro 1 at full pulls Amp Level down.
    const auto viaSlot20 = levelWith ([] (IlanaSynthAudioProcessor& p)
    {
        setParam (p, "macro1", 1.0f);
        setParam (p, "mod20_src", (float) Mod::Source::Macro1);
        setParam (p, "mod20_dst", (float) Mod::Destination::AmpLevel);
        setParam (p, "mod20_amt", -0.8f);
    });
    check (viaSlot20 < base * 0.4f, "mod slot 20 routes (peak " + juce::String (base, 3) + " -> " + juce::String (viaSlot20, 3) + ")");

    const auto bypassed = levelWith ([] (IlanaSynthAudioProcessor& p)
    {
        setParam (p, "macro1", 1.0f);
        setParam (p, "mod20_src", (float) Mod::Source::Macro1);
        setParam (p, "mod20_dst", (float) Mod::Destination::AmpLevel);
        setParam (p, "mod20_amt", -0.8f);
        setParam (p, "mod20_byp", 1.0f);
    });
    check (std::abs (bypassed - base) < base * 0.05f, "bypassed slot does nothing");

    // Via: macro 2 at zero scales the routing away.
    const auto viaZero = levelWith ([] (IlanaSynthAudioProcessor& p)
    {
        setParam (p, "macro1", 1.0f);
        setParam (p, "mod3_src", (float) Mod::Source::Macro1);
        setParam (p, "mod3_dst", (float) Mod::Destination::AmpLevel);
        setParam (p, "mod3_amt", -0.8f);
        setParam (p, "mod3_aux", (float) Mod::Source::Macro2);
    });
    check (std::abs (viaZero - base) < base * 0.05f, "via source at zero mutes the routing");

    // Modulating a plain parameter: macro 1 into the master volume.
    const auto masterMod = levelWith ([] (IlanaSynthAudioProcessor& p)
    {
        setParam (p, "macro1", 1.0f);
        setParam (p, "mod5_src", (float) Mod::Source::Macro1);
        setParam (p, "mod5_dst", (float) Mod::destinationForParamId ("master"));
        setParam (p, "mod5_amt", -0.3f);
    });
    check (masterMod < base * 0.5f, "macro can modulate the master volume parameter (peak " + juce::String (masterMod, 3) + ")");

    // Shape: polarity and curve.
    Mod::Slot slot;
    slot.source = Mod::Source::Macro1;
    slot.polarity = Mod::Polarity::Bipolar;
    check (std::abs (Mod::shape (slot, 0.0f) + 1.0f) < 1.0e-6f && std::abs (Mod::shape (slot, 1.0f) - 1.0f) < 1.0e-6f,
           "bipolar polarity maps a 0..1 source to -1..1");
    slot.polarity = Mod::Polarity::Natural;
    slot.curve = 1.0f;
    check (Mod::shape (slot, 0.5f) < 0.1f, "positive curve bends the response (0.5 -> "
                                              + juce::String (Mod::shape (slot, 0.5f), 3) + ")");

    check (Mod::numExplicitDestinations + Mod::numLegacyParamDestinations == Mod::firstNewExplicitDestination
               && Mod::getDestinationNames().size() == Mod::getNumDestinations(),
           "parameter destinations end exactly where the OSC 4-6 destinations begin");
    check (Mod::getExplicitDestinationNames().size() == Mod::numExplicitDestinations,
           "destination names match the destination list ("
               + juce::String (Mod::getExplicitDestinationNames().size()) + " / " + juce::String (Mod::numExplicitDestinations) + ")");
    check (Mod::getNumDestinations() < IlanaSynthAudioProcessor::maxDestinations, "destination count fits the display table");
}

void runStaleModulationTest()
{
    // A routing removed while a voice is sounding must not leave its last
    // value stuck on the target (it once left presets detuned after a
    // vibrato preset).
    IlanaSynthAudioProcessor processor;
    setParam (processor, "sub_on", 0.0f);
    setParam (processor, "lfo1_rate", 3.0f);
    setParam (processor, "mod1_src", (float) Mod::Source::Lfo1);
    setParam (processor, "mod1_dst", (float) Mod::Destination::Osc1Pitch);
    setParam (processor, "mod1_amt", 0.05f);
    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    std::vector<float> samples;

    for (int block = 0; block < 140; ++block)
    {
        buffer.clear();
        juce::MidiBuffer midi;

        if (block == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 57, (juce::uint8) 100), 0);

        if (block == 13)
            setParam (processor, "mod1_amt", 0.0f);

        processor.processBlock (buffer, midi);

        if (block > 40)
            for (int i = 0; i < 512; ++i)
                samples.push_back (buffer.getSample (0, i));
    }

    const auto frequency = fundamentalOf (samples, 48000.0);
    check (std::abs (frequency - 220.0) < 0.5, "pitch returns to the note after a routing is removed ("
                                                   + juce::String (frequency, 2) + " Hz)");
}

void runPhase2StateAndCpuTest()
{
    // New parameters survive a host round trip.
    {
        IlanaSynthAudioProcessor processor;
        setParam (processor, "osc2_warp", (float) Warp::Mirror);
        setParam (processor, "osc2_warp_amt", 0.4f);
        setParam (processor, "osc1_uni_mode", (float) UnisonMode::Hypersaw);
        setParam (processor, "mod27_src", (float) Mod::Source::Lfo3);
        setParam (processor, "mod27_dst", (float) Mod::destinationForParamId ("fx_ott_amount"));
        setParam (processor, "mod27_amt", -0.25f);
        setParam (processor, "mod27_pol", 2.0f);

        juce::MemoryBlock state;
        processor.getStateInformation (state);

        IlanaSynthAudioProcessor restored;
        restored.setStateInformation (state.getData(), (int) state.getSize());
        const auto slot = restored.readModSlot (26);

        check ((int) restored.apvts.getRawParameterValue ("osc2_warp")->load() == Warp::Mirror
                   && (int) restored.apvts.getRawParameterValue ("osc1_uni_mode")->load() == UnisonMode::Hypersaw
                   && slot.source == Mod::Source::Lfo3 && slot.destination == Mod::destinationForParamId ("fx_ott_amount")
                   && std::abs (slot.depth + 0.25f) < 1.0e-3f && slot.polarity == Mod::Polarity::Bipolar,
               "warp, unison mode and an extended mod slot survive a state round trip");
    }

    // CPU. "Heavy" is a realistic big patch (8-note chord, two 16-voice
    // hypersaw oscillators, ladder filter, a busy matrix) and must stay well
    // inside real time. "Extreme" (16 notes x three 16-voice oscillators with
    // warps) is only reported.
    const auto measure = [] (int notes, int oscillators)
    {
        IlanaSynthAudioProcessor processor;
        const char* const prefixes[] { "osc1", "osc2", "sub" };

        for (int osc = 0; osc < oscillators; ++osc)
        {
            const juce::String prefix (prefixes[osc]);
            setParam (processor, prefix + "_unison", 16.0f);
            setParam (processor, prefix + "_uni_mode", (float) UnisonMode::Hypersaw);
            setParam (processor, prefix + "_warp", (float) Warp::Sync);
            setParam (processor, prefix + "_warp_amt", 0.5f);
        }

        setParam (processor, "osc2_on", 1.0f);
        setParam (processor, "sub_level", oscillators > 2 ? 0.5f : 0.0f);
        setParam (processor, "f1_type", (float) FilterType::LadderLow);

        for (int slot = 1; slot <= 16; ++slot)
        {
            setParam (processor, "mod" + juce::String (slot) + "_src", (float) (1 + slot % 20));
            setParam (processor, "mod" + juce::String (slot) + "_dst", (float) (1 + (slot * 5) % 88));
            setParam (processor, "mod" + juce::String (slot) + "_amt", 0.2f);
        }

        processor.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer noteOns;

        for (int note = 0; note < notes; ++note)
            noteOns.addEvent (juce::MidiMessage::noteOn (1, 40 + note * 2, (juce::uint8) 100), 0);

        const auto blocks = juce::SystemStats::getEnvironmentVariable ("ILANA_BENCH_BLOCKS", "94").getIntValue(); // ~1 s
        const auto start = juce::Time::getMillisecondCounterHiRes();

        for (int block = 0; block < blocks; ++block)
        {
            buffer.clear();
            juce::MidiBuffer midi;

            if (block == 0)
                midi = noteOns;

            processor.processBlock (buffer, midi);
        }

        const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
        return std::make_pair (elapsed / (blocks * 512.0 / 48000.0), std::isfinite (buffer.getMagnitude (0, 512)));
    };

    const auto heavy = measure (8, 2);
    const auto extreme = measure (16, 3);

    std::cout << "INFO: CPU heavy patch " << juce::String (heavy.first * 100.0, 1) << " %, extreme patch "
              << juce::String (extreme.first * 100.0, 1) << " % of one core" << std::endl;
    checkTiming (heavy.first < 0.5 && heavy.second && extreme.second, "heavy patch (8 notes x 32 unison voices) renders well inside real time");
}

// Filter 2 crossing its open line (Low Pass above ~19 kHz, low resonance) under a held note: the switch between
// the cheap open copy and the full filter fades, as Filter 1's does, instead of restarting from rest (bug hunt
// 2026-10-31: ramping Granular Pad's F2 KEY TRACK clicked once, at the crossing).
void runFilter2CrossingTest()
{
    // The voices' free random (drift) changed the worst jump from run to run
    // (1.7x to 4x on the same build): seeded, as the fingerprint build is.
    fixedRandomSeeds() = true;
    struct Restore { ~Restore() { fixedRandomSeeds() = false; } } restore;
    const auto worstClick = [] (bool bypass)
    {
        Voice::disableOpenFilterBypass = ! bypass;
        IlanaSynthAudioProcessor processor;
        processor.setNonRealtime (true);
        processor.prepareToPlay (48000.0, 512);
        processor.loadFactoryPreset (processor.getFactoryPresetNames().indexOf ("Granular Pad"));
        auto* keyTrack = processor.apvts.getParameter ("f2_keytrack");
        std::vector<float> x;
        juce::AudioBuffer<float> buffer (2, 512);
        for (int done = 0; done < 48000 * 14 / 10; done += 512)
        {
            const auto t = (double) done / 48000.0;
            keyTrack->setValueNotifyingHost ((float) juce::jlimit (0.3, 0.7, 0.3 + 0.4 * (t - 0.3)));
            buffer.clear();
            juce::MidiBuffer midi;
            if (done == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
            processor.processBlock (buffer, midi);
            for (int i = 0; i < 512; ++i)
                x.push_back (buffer.getSample (0, i) + buffer.getSample (1, i));
        }
        Voice::disableOpenFilterBypass = false;
        // The worst second difference against the largest one within 10 ms around it (3 samples either side left out).
        std::vector<float> e (x.size(), 0.0f);
        for (size_t i = 2; i < x.size(); ++i)
            e[i] = std::abs (x[i] - 2.0f * x[i - 1] + x[i - 2]);
        auto worst = 0.0f;
        for (size_t i = 16800; i < 62400; ++i)
        {
            auto around = 1.0e-9f;
            for (size_t j = i - 480; j <= i + 480; ++j)
                if (j + 3 < i || j > i + 3)
                    around = juce::jmax (around, e[j]);
            worst = juce::jmax (worst, e[i] / around);
        }
        return worst;
    };
    const auto withSwitch = worstClick (true), without = worstClick (false);
    check (withSwitch < without * 1.5f + 0.5f, "Filter 2 crossing its open line doesn't click (worst jump "
                                                   + juce::String (withSwitch, 2) + "x its surroundings; "
                                                   + juce::String (without, 2) + "x with the full filter throughout)");
}
