// ILANA_PRESET_PROFILE=<factory preset>: hold ILANA_PROFILE_NOTES notes
// (default 4) of that preset at 44.1 kHz; print % of one core, then profile.
void runPresetProfile (const juce::String& name)
{
   #if JUCE_WINDOWS || JUCE_LINUX
    IlanaSynthAudioProcessor processor;
    const auto index = processor.getFactoryPresetNames().indexOf (name);

    if (index < 0)
    {
        std::cout << "no factory preset named " << name << std::endl;
        return;
    }

    const auto blockSize = juce::SystemStats::getEnvironmentVariable ("ILANA_PROFILE_BLOCK", "512").getIntValue();
    processor.prepareToPlay (44100.0, blockSize);
    processor.loadFactoryPreset (index);

    // ILANA_PROFILE_SET="osc1_mode=1,osc1_excite=9": parameter overrides.
    for (const auto& pair : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_PROFILE_SET", ""), ",", ""))
        if (pair.contains ("="))
            setParam (processor, pair.upToFirstOccurrenceOf ("=", false, false).trim(),
                      pair.fromFirstOccurrenceOf ("=", false, false).getFloatValue());

    processor.panic();
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer noteOns;
    const auto notes = juce::SystemStats::getEnvironmentVariable ("ILANA_PROFILE_NOTES", "4").getIntValue();

    for (int note = 0; note < notes; ++note)
        noteOns.addEvent (juce::MidiMessage::noteOn (1, 60 + note * 4, (juce::uint8) 100), 0);

    processor.processBlock (buffer, noteOns);
    const auto seconds = juce::SystemStats::getEnvironmentVariable ("ILANA_PROFILE_SECONDS", "20").getIntValue();
    const auto blocks = 44100 * seconds / blockSize;
    // ILANA_PROFILE_RESTRIKE=<blocks>: strike the notes again that often.
    const auto restrike = juce::SystemStats::getEnvironmentVariable ("ILANA_PROFILE_RESTRIKE", "0").getIntValue();
    const auto start = juce::Time::getMillisecondCounterHiRes();
    auto slowest = 0.0;
   #if JUCE_WINDOWS
    SamplingProfiler profiler;
    profiler.start();
   #endif

    for (int block = 0; block < blocks; ++block)
    {
        buffer.clear();
        juce::MidiBuffer midi;
        if (restrike > 0 && block % restrike == restrike - 1)
            midi = noteOns;
        const auto blockStart = juce::Time::getMillisecondCounterHiRes();
        processor.processBlock (buffer, midi);
        slowest = juce::jmax (slowest, juce::Time::getMillisecondCounterHiRes() - blockStart);
    }

   #if JUCE_WINDOWS
    profiler.stop();
   #endif
    const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
    std::cout << "preset " << name << ", " << notes << " notes: "
              << juce::String (100.0 * elapsed / (blocks * (double) blockSize / 44100.0), 1) << " % of one core, slowest block "
              << juce::String (100.0 * slowest / 1000.0 / (blockSize / 44100.0), 0) << " %" << std::endl;
   #if JUCE_WINDOWS
    profiler.report();
   #endif
   #else
    juce::ignoreUnused (name);
   #endif
}

// ILANA_LIBRARY_BENCH=1: hold 4 notes of every factory preset (or those in
// ILANA_LIBRARY_BENCH_ONLY="A|B") for ILANA_PROFILE_SECONDS (default 3) at
// 44.1 kHz, 256-sample blocks, and print each one's % of one core, then the
// median, mean and the slowest ten. Compare builds by alternating runs.
void runLibraryBench()
{
    Voice::disableOpenFilterBypass = juce::SystemStats::getEnvironmentVariable ("ILANA_NO_OPEN_BYPASS", "").isNotEmpty();
    IlanaSynthAudioProcessor processor;
    const auto blockSize = 256;
    processor.prepareToPlay (44100.0, blockSize);
    const auto names = processor.getFactoryPresetNames();
    const auto only = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_LIBRARY_BENCH_ONLY", ""), "|", "");
    const auto seconds = juce::SystemStats::getEnvironmentVariable ("ILANA_PROFILE_SECONDS", "3").getIntValue();
    std::vector<std::pair<double, juce::String>> results;
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int index = 0; index < names.size(); ++index)
    {
        if (! only.isEmpty() && ! only.contains (names[index]))
            continue;

        processor.loadFactoryPreset (index);
        processor.panic();
        juce::MidiBuffer noteOns;

        for (int note = 0; note < 4; ++note)
            noteOns.addEvent (juce::MidiMessage::noteOn (1, 48 + note * 5, (juce::uint8) 100), 0);

        buffer.clear();
        processor.processBlock (buffer, noteOns);
        const auto blocks = 44100 * seconds / blockSize;
        const auto start = juce::Time::getMillisecondCounterHiRes();

        for (int block = 0; block < blocks; ++block)
        {
            buffer.clear();
            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }

        const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
        const auto percent = 100.0 * elapsed / (blocks * (double) blockSize / 44100.0);
        results.emplace_back (percent, names[index]);
        std::cout << "bench " << names[index] << ": " << juce::String (percent, 2) << " %"
                  << " f1 " << (int) *processor.apvts.getRawParameterValue ("f1_type")
                  << " f2 " << (int) *processor.apvts.getRawParameterValue ("f2_type")
                  << " f2cut " << (int) *processor.apvts.getRawParameterValue ("f2_cutoff") << std::endl;
    }

    if (results.empty())
        return;

    std::vector<double> values;
    for (const auto& r : results)
        values.push_back (r.first);
    std::sort (values.begin(), values.end());
    std::sort (results.begin(), results.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });
    auto sum = 0.0;
    for (auto v : values)
        sum += v;
    std::cout << "LIBRARY BENCH " << results.size() << " presets: median " << juce::String (values[values.size() / 2], 2)
              << " %, mean " << juce::String (sum / (double) values.size(), 2) << " %" << std::endl;
    for (size_t i = 0; i < std::min<size_t> (10, results.size()); ++i)
        std::cout << "  slowest " << results[i].second << ": " << juce::String (results[i].first, 2) << " %" << std::endl;
}

// ILANA_BODY_BENCH=1: a Physical patch (ILANA_BODY_BENCH_PRESET, default
// Felt Hammer Board) with the BODY on, each body type and STRING TO BODY
// coupling mode in turn, at 44.1 kHz in 512-sample blocks: "held" holds
// ILANA_BODY_BENCH_VOICES (default 12) notes for 2 s and releases them for
// 2 s; "clip" plays that chord for 0.4 s every 0.5 s for 6 s, as a looped
// clip does. Prints ms per block, % of one core, the voices sounding and the
// output peak (and any non-finite samples). ILANA_BODY_BENCH_TYPE / _COUPLING
// pick one case, _AMOUNT sets body_coupling (0.5), _SET="id=value,..." sets
// more parameters (real values), _AS_IS plays the preset unchanged, _ONSET
// times one note's rise per type, _ALL ranks every preset by Shell's cost,
// _SECONDS sets the held run's length (4), _CLIP_ONLY skips it, _DUMP=<dir>
// writes each run's output as raw floats. Voices are counted with their
// body tails (getRenderingVoiceCount).
void runBodyBench()
{
    IlanaSynthAudioProcessor processor;
    const auto blockSize = 512;
    processor.prepareToPlay (44100.0, blockSize);
    const auto names = processor.getFactoryPresetNames();
    const auto presetName = juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_PRESET", "Felt Hammer Board");
    const auto voices = juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_VOICES", "12").getIntValue();
    const auto onlyType = juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_TYPE", "-1").getIntValue();
    const auto onlyCoupling = juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_COUPLING", "-1").getIntValue();
    const auto keepPreset = juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_AS_IS", "").isNotEmpty();
    const auto index = names.indexOf (presetName);
    if (index < 0)
    {
        std::cout << "no preset " << presetName << std::endl;
        return;
    }
    const auto set = [&processor] (const char* id, float value)
    {
        if (auto* param = processor.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    };
    const juce::StringArray types { "Classic", "Bar", "Plate", "Bell", "Shell" };
    juce::AudioBuffer<float> buffer (2, blockSize);
    const auto run = [&] (const juce::String& label, bool clip)
    {
        processor.panic();
        buffer.clear();
        juce::MidiBuffer silence;
        processor.processBlock (buffer, silence);
        const auto seconds = clip ? 6.0 : juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_SECONDS", "4").getDoubleValue();
        const auto blocks = (int) (44100.0 * seconds / blockSize);
        auto elapsed = 0.0, slowest = 0.0, voiceSum = 0.0;
        auto peakVoices = 0, nonFinite = 0, lastSounding = -1;
        // ILANA_BODY_BENCH_DUMP=<folder>: each run's output as raw floats.
        std::unique_ptr<juce::FileOutputStream> dump;
        if (const auto folder = juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_DUMP", ""); folder.isNotEmpty())
        {
            const auto file = juce::File (folder).getChildFile (label.replaceCharacter (' ', '_') + (clip ? "_clip" : "_held") + ".f32");
            file.deleteFile();
            dump = std::make_unique<juce::FileOutputStream> (file);
        }
        auto outPeak = 0.0f;
        for (int block = 0; block < blocks; ++block)
        {
            juce::MidiBuffer midi;
            const auto t0 = (double) block * blockSize / 44100.0, t1 = t0 + blockSize / 44100.0;
            const auto inBlock = [&] (double t) { return t >= t0 && t < t1 ? juce::jlimit (0, blockSize - 1, (int) ((t - t0) * 44100.0)) : -1; };
            for (double t = 0.0; t < seconds; t += clip ? 0.5 : 100.0)
            {
                if (const auto on = inBlock (t); on >= 0)
                    for (int v = 0; v < voices; ++v)
                        midi.addEvent (juce::MidiMessage::noteOn (1, 40 + v * 3, (juce::uint8) 100), on);
                if (const auto off = inBlock (t + (clip ? 0.4 : 2.0)); off >= 0)
                    for (int v = 0; v < voices; ++v)
                        midi.addEvent (juce::MidiMessage::noteOff (1, 40 + v * 3), off);
            }
            buffer.clear();
            const auto start = juce::Time::getMillisecondCounterHiRes();
            processor.processBlock (buffer, midi);
            const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
            elapsed += ms;
            slowest = juce::jmax (slowest, ms);
            if (dump != nullptr)
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    dump->write (buffer.getReadPointer (ch), sizeof (float) * (size_t) blockSize);
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                for (int i = 0; i < blockSize; ++i)
                {
                    const auto x = buffer.getSample (ch, i);
                    if (! std::isfinite (x))
                        ++nonFinite;
                    else
                        outPeak = juce::jmax (outPeak, std::abs (x));
                }
            const auto rendering = processor.getRenderingVoiceCount();
            if (rendering > 0)
                lastSounding = block;
            voiceSum += rendering;
            peakVoices = juce::jmax (peakVoices, rendering);
        }
        const auto perBlock = elapsed / blocks;
        std::cout << "body " << label << (clip ? " clip" : " held") << ": " << juce::String (perBlock, 3)
                  << " ms/block, " << juce::String (100.0 * perBlock / (1000.0 * blockSize / 44100.0), 1)
                  << " % of one core, slowest " << juce::String (slowest, 2) << " ms, voices "
                  << juce::String (voiceSum / blocks, 1) << " (peak " << peakVoices << ")"
                  << (clip ? juce::String() : ", last voice ends " + juce::String ((lastSounding + 1) * blockSize / 44100.0 - 2.0, 2) + " s after release")
                  << ", out peak "
                  << juce::String (outPeak, 3) << (nonFinite > 0 ? ", NON-FINITE " + juce::String (nonFinite) : juce::String()) << std::endl;
    };
    // ILANA_BODY_BENCH_ALL=1: every factory preset with the BODY on, 6 notes
    // held 1 s, Bar / Plate / Bell / Shell; prints Shell's time against the
    // others' mean, the worst first.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_ALL", "").isNotEmpty())
    {
        std::vector<std::pair<double, juce::String>> rows;
        juce::AudioBuffer<float> one (2, blockSize);
        for (int preset = 0; preset < names.size(); ++preset)
        {
            double ms[5] {};
            for (int type = 1; type < 5; ++type)
            {
                processor.loadFactoryPreset (preset);
                set ("res_on", 1.0f);
                set ("body_type", (float) type);
                processor.panic();
                const auto blocks = (int) (44100.0 / blockSize);
                for (int block = 0; block < blocks; ++block)
                {
                    juce::MidiBuffer midi;
                    if (block == 0)
                        for (int v = 0; v < 6; ++v)
                            midi.addEvent (juce::MidiMessage::noteOn (1, 48 + v * 4, (juce::uint8) 100), 0);
                    one.clear();
                    const auto start = juce::Time::getMillisecondCounterHiRes();
                    processor.processBlock (one, midi);
                    ms[type] += juce::Time::getMillisecondCounterHiRes() - start;
                }
            }
            const auto others = (ms[1] + ms[2] + ms[3]) / 3.0;
            rows.push_back ({ ms[4] / juce::jmax (1.0e-6, others), names[preset] + ": shell " + juce::String (ms[4], 1) + " ms, bar "
                                                                      + juce::String (ms[1], 1) + ", plate " + juce::String (ms[2], 1)
                                                                      + ", bell " + juce::String (ms[3], 1) });
        }
        std::sort (rows.begin(), rows.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < std::min<size_t> (25, rows.size()); ++i)
            std::cout << juce::String (rows[i].first, 2) << "x  " << rows[i].second << std::endl;
        return;
    }
    // ILANA_BODY_BENCH_ONSET=1: one note (C4) per body type, 2 s; the time
    // its 512-sample RMS takes to reach half (-6 dB) of its peak.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_ONSET", "").isNotEmpty())
    {
        for (int type = 0; type < 5; ++type)
        {
            processor.loadFactoryPreset (index);
            set ("res_on", 1.0f);
            set ("body_type", (float) type);
            for (const auto& pair : juce::StringArray::fromTokens (
                     juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_SET", ""), ",", ""))
                if (pair.contains ("="))
                    set (pair.upToFirstOccurrenceOf ("=", false, false).toRawUTF8(),
                         pair.fromFirstOccurrenceOf ("=", false, false).getFloatValue());
            processor.panic();
            std::vector<float> rms;
            for (int block = 0; block < (int) (2.0 * 44100.0 / blockSize); ++block)
            {
                juce::MidiBuffer midi;
                if (block == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                buffer.clear();
                processor.processBlock (buffer, midi);
                rms.push_back (buffer.getRMSLevel (0, 0, blockSize));
            }
            const auto peak = *std::max_element (rms.begin(), rms.end());
            const auto peakAt = (int) (std::max_element (rms.begin(), rms.end()) - rms.begin());
            auto half = 0;
            while (half < (int) rms.size() && rms[(size_t) half] < 0.5f * peak)
                ++half;
            std::cout << "onset " << types[type] << ": -6 dB at " << juce::String (half * blockSize / 44.1, 0) << " ms, peak at "
                      << juce::String (peakAt * blockSize / 44.1, 0) << " ms, peak rms " << juce::String (peak, 3) << std::endl;
        }
        return;
    }
    if (keepPreset)
    {
        processor.loadFactoryPreset (index);
        run (presetName, false);
        processor.loadFactoryPreset (index);
        run (presetName, true);
        return;
    }
    for (int coupling = 0; coupling < 4; ++coupling)
        for (int type = 0; type < 5; ++type)
        {
            if ((onlyType >= 0 && type != onlyType) || (onlyCoupling >= 0 && coupling != onlyCoupling))
                continue;
            for (const auto clip : { false, true })
            {
                if (! clip && juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_CLIP_ONLY", "").isNotEmpty())
                    continue;
                processor.loadFactoryPreset (index);
                set ("res_on", 1.0f);
                set ("body_type", (float) type);
                set ("body_coupling_mode", (float) coupling);
                if (coupling > 0)
                    set ("body_coupling", juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_AMOUNT", "0.5").getFloatValue());
                for (const auto& pair : juce::StringArray::fromTokens (
                         juce::SystemStats::getEnvironmentVariable ("ILANA_BODY_BENCH_SET", ""), ",", ""))
                    if (pair.contains ("="))
                        set (pair.upToFirstOccurrenceOf ("=", false, false).toRawUTF8(),
                             pair.fromFirstOccurrenceOf ("=", false, false).getFloatValue());
                run (types[type] + " coupling " + juce::String (coupling), clip);
            }
        }
}

// ILANA_OPEN_FILTER_CHECK=1: the gain of Filter 2's default (Low Pass wide
// open, resonance 0 and 0.3) against a straight wire, per frequency, at
// 44.1 and 48 kHz, for sines at -6 dBFS and -20 dBFS.
void runOpenFilterCheck()
{
    for (const auto rate : { 44100.0, 48000.0 })
        for (const auto reso : { 0.0, 0.3 })
            for (const auto level : { 0.5f, 0.1f })
            {
                std::cout << "rate " << rate << " reso " << reso << " level " << level << ":";
                for (const auto hz : { 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0 })
                {
                    FilterUnit filter;
                    filter.prepare (rate);
                    filter.setType (FilterType::LowPass, false);
                    filter.setCoefficients (FilterUnit::makeCoefficients (FilterType::LowPass, rate, juce::jmin (20000.0, rate * 0.45), reso));
                    double in = 0.0, out = 0.0;
                    const auto n = (int) rate;
                    for (int i = 0; i < n; ++i)
                    {
                        const auto x = level * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate);
                        const auto y = filter.process (x);
                        if (i > n / 4) { in += (double) x * x; out += (double) y * y; }
                    }
                    Airwindows::OpenLowPass open;
                    open.set (rate, juce::jmin (20000.0, rate * 0.45), reso);
                    double openOut = 0.0;
                    for (int i = 0; i < n; ++i)
                    {
                        const auto x = level * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate);
                        const auto y = open.process (x);
                        if (i > n / 4) openOut += (double) y * y;
                    }
                    std::cout << " " << hz << "Hz " << juce::String (10.0 * std::log10 (out / in), 2)
                              << "/" << juce::String (10.0 * std::log10 (openOut / in), 2);
                }
                std::cout << std::endl;
            }
}

// ILANA_NULL_CHECK=1: render every factory preset (4 notes, 2 s at 44.1 kHz)
// with Voice::disableOpenFilterBypass off and on, and print the difference
// against the signal in dB (a CPU shortcut must null below -60 dB or so).
// A drawn remap curve per mod slot: shape() applies it over the slot's
// range, it saves and loads with the patch, a factory preset or a cleared
// slot drops it, and the voices hear it.
void runModRemapTests()
{
    std::array<float, Mod::remapSize + 1> identity {}, invert {};
    for (int i = 0; i <= Mod::remapSize; ++i)
    {
        identity[(size_t) i] = 2.0f * (float) i / (float) Mod::remapSize - 1.0f;
        invert[(size_t) i] = -identity[(size_t) i];
    }

    Mod::Slot slot;
    slot.source = Mod::Source::Macro1; // unipolar
    slot.remap = identity.data();
    check (std::abs (Mod::shape (slot, 0.3f) - 0.3f) < 1.0e-5f, "remap: a straight line leaves a unipolar source as it was");
    slot.remap = invert.data();
    check (std::abs (Mod::shape (slot, 0.25f) - 0.75f) < 1.0e-5f, "remap: an inverted line flips a unipolar source");
    slot.source = Mod::Source::Lfo1; // bipolar
    check (std::abs (Mod::shape (slot, 0.5f) + 0.5f) < 1.0e-5f, "remap: an inverted line flips a bipolar source");
    slot.remap = identity.data();
    check (std::abs (Mod::shape (slot, -0.4f) + 0.4f) < 1.0e-5f, "remap: a straight line leaves a bipolar source as it was");

    IlanaSynthAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);
    processor.loadFactoryPreset (0);
    check (! processor.isModRemapOn (3), "remap: off by default");
    processor.setModRemap (3, IlanaSynthAudioProcessor::identityRemap());
    check (! processor.isModRemapOn (3), "remap: a straight line counts as off");
    LfoCurve flipped;
    flipped.points = { { 0.0f, 1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f } };
    processor.setModRemap (3, flipped);
    check (processor.isModRemapOn (3) && processor.readModSlot (3).remap != nullptr, "remap: a drawn curve is on");

    juce::MemoryBlock saved;
    processor.getStateInformation (saved);
    IlanaSynthAudioProcessor loaded;
    loaded.setStateInformation (saved.getData(), (int) saved.getSize());
    check (loaded.isModRemapOn (3) && loaded.getModRemap (3).toString() == flipped.toString(), "remap: saved and loaded with the patch");
    loaded.loadFactoryPreset (0);
    check (! loaded.isModRemapOn (3), "remap: a factory preset clears it");
    processor.clearModSlot (3);
    check (! processor.isModRemapOn (3), "remap: clearing the slot clears it");

    // Macro 1 at full pulls Amp Level all the way down; inverted by the
    // remap, the macro sends nothing and the note plays.
    const auto render = [] (bool remap)
    {
        IlanaSynthAudioProcessor p;
        p.prepareToPlay (48000.0, 256);
        p.loadFactoryPreset (0);
        if (auto* macro = p.apvts.getParameter ("macro1"))
            macro->setValueNotifyingHost (1.0f);
        p.clearModSlot (0);
        p.setModSlotValue (0, "src", (float) Mod::Source::Macro1);
        p.setModSlotValue (0, "dst", (float) Mod::Destination::AmpLevel);
        p.setModSlotValue (0, "amt", -1.0f);
        if (remap)
        {
            LfoCurve curve;
            curve.points = { { 0.0f, 1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f } };
            p.setModRemap (0, curve);
        }
        juce::AudioBuffer<float> buffer (2, 256);
        auto sum = 0.0;
        for (int block = 0; block < 60; ++block)
        {
            buffer.clear();
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            p.processBlock (buffer, midi);
            for (int i = 0; i < 256; ++i)
                sum += (double) buffer.getSample (0, i) * buffer.getSample (0, i);
        }
        return std::sqrt (sum / (60.0 * 256.0));
    };
    const auto plain = render (false), remapped = render (true);
    check (plain < 1.0e-4 && remapped > 1.0e-3,
           "remap: the voices hear it (rms " + juce::String (plain, 6) + " plain, " + juce::String (remapped, 6) + " remapped)");
}

void runNullCheck()
{
    IlanaSynthAudioProcessor processor;
    const auto blockSize = 256;
    processor.prepareToPlay (44100.0, blockSize);
    const auto names = processor.getFactoryPresetNames();
    const auto only = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_LIBRARY_BENCH_ONLY", ""), "|", "");
    const auto blocks = 44100 * 2 / blockSize;
    auto worst = -300.0;
    juce::String worstName;

    // A fresh processor per render: it is seeded the same way every time.
    const auto render = [&] (int index)
    {
        IlanaSynthAudioProcessor processor;
        processor.prepareToPlay (44100.0, blockSize);
        processor.loadFactoryPreset (index);
        // ILANA_NULL_SET="id=value,...": override parameters after loading.
        for (const auto& pair : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_NULL_SET", ""), ",", ""))
            if (auto* parameter = processor.apvts.getParameter (pair.upToFirstOccurrenceOf ("=", false, false)))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (pair.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
        processor.panic();
        juce::AudioBuffer<float> out (2, blocks * blockSize);
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer noteOns;
        for (int note = 0; note < 4; ++note)
            noteOns.addEvent (juce::MidiMessage::noteOn (1, 48 + note * 5, (juce::uint8) 100), 0);
        for (int block = 0; block < blocks; ++block)
        {
            buffer.clear();
            juce::MidiBuffer midi;
            if (block == 0)
                midi = noteOns;
            if (block == blocks / 2)
                for (int note = 0; note < 4; ++note)
                    midi.addEvent (juce::MidiMessage::noteOff (1, 48 + note * 5), 0);
            processor.processBlock (buffer, midi);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, block * blockSize, buffer, ch, 0, blockSize);
        }
        return out;
    };

    for (int index = 0; index < names.size(); ++index)
    {
        if (! only.isEmpty() && ! only.contains (names[index]))
            continue;

        Voice::disableOpenFilterBypass = true;
        const auto reference = render (index);
        Voice::disableOpenFilterBypass = juce::SystemStats::getEnvironmentVariable ("ILANA_NULL_CONTROL", "").isNotEmpty();
        const auto candidate = render (index);
        Voice::disableOpenFilterBypass = false;
        double signal = 0.0, diff = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < reference.getNumSamples(); ++i)
            {
                const auto a = (double) reference.getSample (ch, i), b = (double) candidate.getSample (ch, i);
                signal += a * a;
                diff += (a - b) * (a - b);
            }
        const auto db = 10.0 * std::log10 ((diff + 1.0e-30) / (signal + 1.0e-30));
        std::cout << "null " << names[index] << ": " << juce::String (db, 1) << " dB" << std::endl;
        if (db > worst)
        {
            worst = db;
            worstName = names[index];
        }
    }

    std::cout << "NULL CHECK worst " << worstName << ": " << juce::String (worst, 1) << " dB" << std::endl;
}
