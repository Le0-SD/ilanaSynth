// M8.2: the Piano exciter (PianoString). Included by TableTest.cpp.

namespace M82
{
struct Strike
{
    double contactMs = 0.0, peakForceLevel = 0.0;
    float rms50ms = 0.0f, rms1s = 0.0f, peak = 0.0f;
    bool finite = true;
};

Strike strike (int note, float velocity, float hardness = 0.5f, double seconds = 1.2, bool eco = false)
{
    KarplusStrong string;
    string.prepare (48000.0);
    string.setParams (KarplusStrong::Excite::Piano, 0.0f, 0.5f, 0.5f);
    string.setPhysicalParams (0.5f, 0.0f, 0.0f, 1.0f, 0.0f, false);
    string.setKeysParams (hardness, 0.8f);
    string.setEco (eco);
    string.setFrequency (440.0 * std::pow (2.0, (note - 69) / 12.0));
    string.trigger (velocity);
    Strike result;
    double sum50 = 0.0, sum1 = 0.0;
    auto n1 = 0;
    const auto samples = (int) (seconds * 48000.0);
    for (int i = 0; i < samples; ++i)
    {
        const auto y = string.process (0.0f, true);
        result.finite = result.finite && std::isfinite (y);
        result.peak = juce::jmax (result.peak, std::abs (y));
        if (i < 2400)
            sum50 += (double) y * y;
        if (i >= 48000 && i < 48000 + 4800)
        {
            sum1 += (double) y * y;
            ++n1;
        }
    }
    result.rms50ms = (float) std::sqrt (sum50 / 2400.0);
    result.rms1s = (float) std::sqrt (sum1 / juce::jmax (1, n1));
    return result;
}

// ILANA_PIANO_PROBE=1: prints the strike for a few notes and velocities.
void probe()
{
    for (int note : { 21, 33, 45, 60, 72, 84, 96, 108 })
        for (float velocity : { 20.0f / 127.0f, 60.0f / 127.0f, 124.0f / 127.0f })
        {
            const auto s = strike (note, velocity);
            std::cout << "note " << note << " vel " << juce::roundToInt (velocity * 127.0f)
                      << "  rms50ms " << juce::Decibels::gainToDecibels (s.rms50ms)
                      << "  rms@1s " << juce::Decibels::gainToDecibels (s.rms1s)
                      << "  peak " << s.peak << (s.finite ? "" : " NaN") << std::endl;
        }
}
// The render of one piano string, for the checks below.
std::vector<float> renderString (int note, float velocity, float hardness, double seconds, bool eco = false, int* contact = nullptr,
                                 float strikeKnob = 0.0f)
{
    KarplusStrong string;
    string.prepare (48000.0);
    string.setParams (KarplusStrong::Excite::Piano, 0.0f, 0.5f, 0.5f);
    string.setPhysicalParams (0.5f, 0.0f, strikeKnob, 1.0f, 0.0f, false);
    string.setKeysParams (hardness, 0.8f);
    string.setEco (eco);
    string.setFrequency (440.0 * std::pow (2.0, (note - 69) / 12.0));
    string.trigger (velocity);
    std::vector<float> out ((size_t) (seconds * 48000.0));
    for (auto& y : out)
        y = string.process (0.0f, true);
    juce::ignoreUnused (contact);
    return out;
}

// Magnitude of the component at hz over [from, from + length) samples.
double magnitudeAt (const std::vector<float>& x, double hz, int from, int length)
{
    std::complex<double> sum;
    const auto w = juce::MathConstants<double>::twoPi * hz / 48000.0;
    for (int i = 0; i < length && from + i < (int) x.size(); ++i)
    {
        const auto window = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / length);
        sum += (double) x[(size_t) (from + i)] * window * std::polar (1.0, -w * i);
    }
    return std::abs (sum);
}

// The partial near n f0: its frequency (searched +-40 cents around the
// inharmonic estimate) and level.
std::pair<double, double> partial (const std::vector<float>& x, double f0, int n, double b)
{
    const auto guess = n * f0 * std::sqrt (1.0 + b * n * n);
    auto best = guess, level = 0.0;
    for (double cents = -40.0; cents <= 40.0; cents += 0.5)
    {
        const auto hz = guess * std::pow (2.0, cents / 1200.0);
        const auto m = magnitudeAt (x, hz, 4800, 48000);
        if (m > level)
        {
            level = m;
            best = hz;
        }
    }
    return { best, level };
}

double centroid (const std::vector<float>& x, int from, int length)
{
    juce::dsp::FFT fft (13);
    std::vector<float> data (2 * 8192, 0.0f);
    for (int i = 0; i < 8192 && i < length && from + i < (int) x.size(); ++i)
        data[(size_t) i] = x[(size_t) (from + i)] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / 8192.0f));
    fft.performFrequencyOnlyForwardTransform (data.data());
    auto num = 0.0, den = 0.0;
    for (int k = 1; k < 4096; ++k)
    {
        num += (double) data[(size_t) k] * k * 48000.0 / 8192.0;
        den += (double) data[(size_t) k];
    }
    return num / juce::jmax (1.0e-12, den);
}

void runTests()
{
    // Tuning: the fundamental sits within a few cents of the note (the
    // stiffness stretches it by sqrt(1 + B) at most).
    auto worstCents = 0.0;
    for (int note : { 24, 36, 48, 57, 60, 72, 84, 96 })
    {
        const auto f0 = 440.0 * std::pow (2.0, (note - 69) / 12.0);
        const auto x = renderString (note, 0.6f, 0.5f, 1.3);
        const auto [hz, level] = partial (x, f0, 1, 0.0);
        juce::ignoreUnused (level);
        worstCents = juce::jmax (worstCents, std::abs (1200.0 * std::log2 (hz / f0)));
        if (std::abs (1200.0 * std::log2 (hz / f0)) > 3.0)
            std::cout << "INFO: note " << note << " fundamental " << 1200.0 * std::log2 (hz / f0) << " cents" << std::endl;
    }
    check (worstCents < 4.0, "Piano strings are in tune (worst fundamental " + juce::String (worstCents, 2) + " cents off)");

    // Stretched partials: partial 8 of C2 sits sharp of 8 f0, as B says.
    {
        const auto f0 = 440.0 * std::pow (2.0, (36 - 69) / 12.0);
        const auto x = renderString (36, 0.7f, 0.5f, 1.3);
        const auto [f1, l1] = partial (x, f0, 1, 0.0);
        const auto [f8, l8] = partial (x, f0, 8, 3.0e-4);
        juce::ignoreUnused (l1, l8);
        const auto stretch = 1200.0 * std::log2 (f8 / (8.0 * f1));
        check (stretch > 0.5 && stretch < 40.0, "C2's 8th partial is stretched sharp (" + juce::String (stretch, 1) + " cents)");
    }

    // A real hammer: louder is brighter, and a harder felt is brighter.
    for (int note : { 36, 60, 84 })
    {
        const auto soft = centroid (renderString (note, 0.2f, 0.5f, 0.3), 480, 8192);
        const auto loud = centroid (renderString (note, 1.0f, 0.5f, 0.3), 480, 8192);
        const auto hard = centroid (renderString (note, 0.6f, 1.0f, 0.3), 480, 8192);
        const auto felt = centroid (renderString (note, 0.6f, 0.0f, 0.3), 480, 8192);
        check (loud > soft * 1.15 && hard > felt * 1.05,
               "note " + juce::String (note) + ": louder is brighter (" + juce::String (soft, 0) + " -> " + juce::String (loud, 0)
                   + " Hz centroid), harder felt is brighter (" + juce::String (felt, 0) + " -> " + juce::String (hard, 0) + " Hz)");
    }

    // A strike at 1/8 (EXCITE POS 0.25) thins the partials with a node
    // there (the fitted default sits between two partials' nodes).
    {
        const auto f0 = 440.0 * std::pow (2.0, (48 - 69) / 12.0);
        const auto x = renderString (48, 0.7f, 0.5f, 1.3, false, nullptr, 0.25f);
        std::vector<double> levels;
        for (int n = 1; n <= 14; ++n)
            levels.push_back (partial (x, f0, n, 3.0e-4).second);
        // The dip nearest 1 / strike fraction.
        auto dip = 0;
        auto lowest = 1.0e30;
        for (int n = 6; n <= 14; ++n)
            if (levels[(size_t) n - 1] < lowest)
            {
                lowest = levels[(size_t) n - 1];
                dip = n;
            }
        const auto neighbours = 0.5 * (levels[(size_t) dip - 2] + levels[(size_t) juce::jmin (13, dip)]);
        check (lowest < neighbours * 0.5, "the strike point notches partial " + juce::String (dip) + " ("
                                              + juce::String (juce::Decibels::gainToDecibels (lowest / neighbours), 1) + " dB under its neighbours)");
    }

    // Stable at every extreme, and Eco still sounds.
    {
        auto finite = true;
        auto quiet = false;
        for (int note : { 21, 40, 60, 90, 108 })
            for (float velocity : { 0.01f, 1.0f })
                for (float hardness : { 0.0f, 1.0f })
                    for (bool eco : { false, true })
                    {
                        const auto s = strike (note, velocity, hardness, 1.0, eco);
                        finite = finite && s.finite && s.peak < 8.0f;
                        quiet = quiet || s.peak < 1.0e-5f;
                    }
        check (finite && ! quiet, "the Piano exciter is stable at every note, velocity, hardness and quality");
    }

    // The preset plays at a sensible level (its tuning is in the preset
    // tuning test, old patches in the fingerprints).
    {
        IlanaSynthAudioProcessor processor;
        const auto index = processor.getFactoryPresetNames().indexOf ("Felt Hammer Board");
        check (index >= 0, "the Felt Hammer Board preset exists");
        if (index >= 0)
        {
            processor.loadFactoryPreset (index);
            processor.prepareToPlay (48000.0, 512);
            juce::AudioBuffer<float> buffer (2, 512);
            auto peak = 0.0f;
            for (int block = 0; block < 200; ++block)
            {
                juce::MidiBuffer midi;
                if (block == 0)
                    for (int note : { 36, 48, 60, 64, 67, 72 })
                        midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
                buffer.clear();
                processor.processBlock (buffer, midi);
                peak = juce::jmax (peak, buffer.getMagnitude (0, 512));
            }
            check (peak > 0.05f && peak < 1.5f, "Felt Hammer Board plays a chord at a sensible level (peak " + juce::String (peak, 3) + ")");
        }
    }

    // The Modal Piano: its table loads, every note and knob extreme plays
    // and stays finite, it sounds at its key, the dampers stop it, and it
    // plays in stereo.
    {
        const auto& table = ModalPianoTable::get();
        check (table.valid() && table.notes.size() == 88 && table.velocities.size() == 3,
               "the Modal Piano table loads (" + juce::String ((int) table.notes.size()) + " notes x "
                   + juce::String ((int) table.velocities.size()) + " velocities)");

        // The left channel (and the right, into `right`).
        const auto play = [] (int note, float velocity, float knobs, bool eco, int samples, int releaseAt = -1,
                              std::vector<float>* right = nullptr)
        {
            ModalPiano piano;
            piano.prepare (48000.0);
            piano.setNote (440.0 * std::pow (2.0, (note - 69) / 12.0), note);
            piano.setParams (knobs, knobs == 0.5f ? 0.0f : knobs, knobs, knobs, knobs, eco);
            piano.trigger (velocity, 0.37f);
            std::vector<float> out ((size_t) samples);
            if (right != nullptr)
                right->resize ((size_t) samples);
            for (int i = 0; i < samples; ++i)
            {
                const auto middle = piano.process (releaseAt < 0 || i < releaseAt);
                out[(size_t) i] = middle + piano.side();
                if (right != nullptr)
                    (*right)[(size_t) i] = middle - piano.side();
            }
            return out;
        };

        auto finite = true, quiet = false;
        for (int note : { 21, 40, 60, 90, 108 })
            for (float velocity : { 0.01f, 0.5f, 1.0f })
                for (float knobs : { 0.0f, 0.5f, 1.0f })
                    for (bool eco : { false, true })
                    {
                        auto peak = 0.0f;
                        for (auto v : play (note, velocity, knobs, eco, 24000))
                        {
                            finite = finite && std::isfinite (v);
                            peak = juce::jmax (peak, std::abs (v));
                        }
                        finite = finite && peak < 4.0f;
                        quiet = quiet || peak < 1.0e-6f;
                    }
        check (finite && ! quiet, "the Modal Piano is stable at every note, velocity, knob extreme and quality");

        // C4: the strongest partial in its first second is the fundamental.
        {
            const auto x = play (60, 0.5f, 0.5f, false, 48000);
            auto best = 0.0, bestHz = 0.0;
            for (double hz = 100.0; hz < 1000.0; hz += 0.5)
            {
                std::complex<double> sum;
                for (int i = 4800; i < 48000; ++i)
                    sum += (double) x[(size_t) i] * std::polar (1.0, -juce::MathConstants<double>::twoPi * hz * i / 48000.0);
                if (std::abs (sum) > best)
                {
                    best = std::abs (sum);
                    bestHz = hz;
                }
            }
            check (std::abs (bestHz - 261.63) < 1.5, "the Modal Piano's C4 sounds at its key (" + juce::String (bestHz, 1) + " Hz)");
        }

        // DAMPER all the way: 0.5 s after the key comes up the note is 40 dB down.
        {
            const auto x = play (60, 0.8f, 1.0f, false, 72000, 24000);
            const auto rms = [&x] (int from) { auto e = 0.0; for (int i = from; i < from + 2400; ++i) e += x[(size_t) i] * x[(size_t) i]; return std::sqrt (e / 2400.0); };
            const auto drop = juce::Decibels::gainToDecibels (rms (48000 + 2400) / juce::jmax (1.0e-12, rms (21600)));
            check (drop < -40.0, "the Modal Piano's dampers stop the note (" + juce::String (drop, 1) + " dB in 0.5 s)");
        }

        // Silent groups of partials stop running (CPU round 2026-10-31): the
        // sound stays within -100 dB of every partial running, also while
        // DECAY moves (it can bring a faded partial back), and it is cheaper.
        {
            auto worst = -300.0f;
            double timeAll = 0.0, timeSkip = 0.0;
            // ILANA_MODAL_CUT=dB: try another relative cut.
            if (const auto cut = juce::SystemStats::getEnvironmentVariable ("ILANA_MODAL_CUT", ""); cut.isNotEmpty())
                ModalPiano::relativeCut = cut.getFloatValue();
            const auto render = [] (int note, float velocity, float knobs, bool moveDecay, double& seconds)
            {
                ModalPiano piano;
                piano.prepare (48000.0);
                piano.setNote (440.0 * std::pow (2.0, (note - 69) / 12.0), note);
                piano.setParams (knobs, knobs == 0.5f ? 0.0f : knobs, knobs, knobs, knobs, false);
                piano.trigger (velocity, 0.37f);
                std::vector<float> out (48000 * 4);
                const auto start = juce::Time::getMillisecondCounterHiRes();
                for (int i = 0; i < (int) out.size(); ++i)
                {
                    if (moveDecay && i % 480 == 0)
                    {
                        // Down for two seconds, then back up past where it began.
                        const auto t = (float) i / (float) out.size();
                        const auto decay = t < 0.5f ? 0.5f - t : t * 2.0f - 1.0f;
                        piano.setParams (decay, 0.0f, 0.5f, 0.5f, 0.5f, false);
                    }
                    out[(size_t) i] = piano.process (i < 48000 * 3) + piano.side();
                }
                seconds += (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
                return out;
            };
            for (int note : { 21, 48, 60, 72, 96, 108 })
                for (float velocity : { 0.2f, 0.9f })
                    for (float knobs : { 0.0f, 0.5f, 1.0f })
                        for (bool moveDecay : { false, true })
                        {
                            if (moveDecay && knobs != 0.5f)
                                continue;
                            ModalPiano::skipSilentGroups = false;
                            const auto all = render (note, velocity, knobs, moveDecay, timeAll);
                            ModalPiano::skipSilentGroups = true;
                            const auto skip = render (note, velocity, knobs, moveDecay, timeSkip);
                            auto peak = 1.0e-9f, diff = 0.0f;
                            for (size_t i = 0; i < all.size(); ++i)
                            {
                                peak = juce::jmax (peak, std::abs (all[i]));
                                diff = juce::jmax (diff, std::abs (all[i] - skip[i]));
                            }
                            const auto db = juce::Decibels::gainToDecibels (diff / peak, -300.0f);
                            if (db > -80.0f) std::cout << "  modal skip " << note << " " << velocity << " " << knobs << " " << (int) moveDecay << ": " << db << " dB" << std::endl;
                            worst = juce::jmax (worst, db);
                        }
            if (const auto cuts = juce::SystemStats::getEnvironmentVariable ("ILANA_MODAL_CUT", ""); cuts.isNotEmpty())
                std::cout << "  relative cut " << ModalPiano::relativeCut << " dB: worst " << worst << " dB, time " << timeSkip / timeAll << "x" << std::endl;
            check (worst < -80.0f && timeSkip < timeAll, "the Modal Piano sounds the same, within -80 dB, and costs less with quiet partials skipped (worst "
                                        + juce::String (worst, 1) + " dB; time " + juce::String (timeSkip / timeAll, 2) + "x)");
        }

        // Stereo: the reference's microphones are far out of phase at C4
        // (correlation about -0.9), and the two channels are about as loud.
        {
            std::vector<float> right;
            const auto left = play (60, 0.5f, 0.5f, false, 48000, -1, &right);
            auto lr = 0.0, ll = 0.0, rr = 0.0;
            for (int i = 4800; i < 48000; ++i)
            {
                lr += (double) left[(size_t) i] * right[(size_t) i];
                ll += (double) left[(size_t) i] * left[(size_t) i];
                rr += (double) right[(size_t) i] * right[(size_t) i];
            }
            const auto correlation = lr / std::sqrt (juce::jmax (1.0e-30, ll * rr));
            const auto balance = 10.0 * std::log10 (juce::jmax (1.0e-30, ll) / juce::jmax (1.0e-30, rr));
            check (correlation < 0.0 && std::abs (balance) < 6.0,
                   "the Modal Piano plays in stereo (C4: correlation " + juce::String (correlation, 2)
                       + ", balance " + juce::String (balance, 1) + " dB)");
        }

        // Level: through the whole synth, a Modal Piano C4 at velocity 100
        // peaks within 3 dB of a default wavetable osc (it was 17 dB under).
        {
            const auto peakOf = [] (bool modal)
            {
                IlanaSynthAudioProcessor processor;
                if (modal)
                {
                    setParam (processor, "osc1_mode", 1.0f);
                    setParam (processor, "osc1_excite", 11.0f);
                }
                processor.prepareToPlay (48000.0, 256);
                juce::AudioBuffer<float> buffer (2, 256);
                auto peak = 0.0f;
                for (int start = 0; start < 24000; start += 256)
                {
                    juce::MidiBuffer midi;
                    if (start == 0)
                        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                    buffer.clear();
                    processor.processBlock (buffer, midi);
                    peak = juce::jmax (peak, buffer.getMagnitude (0, 256));
                }
                return juce::Decibels::gainToDecibels (peak);
            };
            const auto modal = peakOf (true), wavetable = peakOf (false);
            check (std::abs (modal - wavetable) < 3.0f, "the Modal Piano plays at a wavetable osc's level (C4 peak "
                                                            + juce::String (modal, 1) + " dB vs " + juce::String (wavetable, 1) + " dB)");
        }

        // A voice that played another key first plays this one as a fresh
        // voice would (2026-11-02, ilana: "it mostly sounds like a sine"):
        // the note started at the voice's last pitch, and the Modal Piano
        // took its recorded key from that, so A1 after a high key kept its few
        // partials, pitched down (-26 dB above 80 Hz).
        {
            const auto overtoneShare = [] (bool playHighFirst)
            {
                IlanaSynthAudioProcessor processor;
                setParam (processor, "osc1_mode", 1.0f);
                setParam (processor, "osc1_excite", 11.0f);
                setParam (processor, "osc1_damper", 0.5f); // (the chord stops on release)
                processor.prepareToPlay (48000.0, 256);
                juce::AudioBuffer<float> buffer (2, 256);
                std::vector<float> out;
                const auto lowStart = playHighFirst ? 6 * 48000 : 0;
                for (int start = 0; start < lowStart + 48000; start += 256)
                {
                    juce::MidiBuffer midi;
                    // (A chord on every voice, so whichever voice A1 gets has played a high key.)
                    for (int n = 76; n < 108 && playHighFirst; ++n)
                    {
                        if (start == 0)
                            midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 100), 0);
                        if (start == 256 * 60)
                            midi.addEvent (juce::MidiMessage::noteOff (1, n), 0);
                    }
                    if (start == lowStart)
                        midi.addEvent (juce::MidiMessage::noteOn (1, 33, (juce::uint8) 124), 0);
                    buffer.clear();
                    processor.processBlock (buffer, midi);
                    if (start >= lowStart)
                        for (int i = 0; i < 256; ++i)
                            out.push_back (buffer.getSample (0, i));
                }
                // The overtones' share: harmonics 2-8 against the
                // fundamental (55 Hz), 0.3-1 s in (a plain DFT at each).
                const auto powerAt = [&out] (double hz)
                {
                    std::complex<double> sum;
                    for (int i = 14400; i < 48000; ++i)
                        sum += (double) out[(size_t) i] * std::polar (1.0, -juce::MathConstants<double>::twoPi * hz * i / 48000.0);
                    return std::norm (sum);
                };
                auto overtones = 0.0;
                for (int k = 2; k <= 8; ++k)
                    overtones += powerAt (55.0 * k * (1.0 + 0.0002 * k * k)) + powerAt (55.0 * k);
                return 10.0 * std::log10 (juce::jmax (1.0e-30, overtones) / juce::jmax (1.0e-30, powerAt (55.0)));
            };
            const auto fresh = overtoneShare (false), reused = overtoneShare (true);
            check (std::abs (fresh - reused) < 6.0, "a Modal Piano note on a voice that played a high key first keeps its overtones (A1 harmonics 2-8 against the fundamental: "
                                                      + juce::String (reused, 1) + " dB vs " + juce::String (fresh, 1) + " dB on a fresh voice)");
        }

        // A session saved before that trim (exciterLevels 1) keeps its Modal Piano where it was heard;
        // saved again it stays so, and one saved now (2) plays the trim (bug hunt 2026-10-31).
        {
            const auto peakAfterLoad = [] (int savedLevels, int& resaved)
            {
                juce::MemoryBlock state;
                {
                    IlanaSynthAudioProcessor source;
                    setParam (source, "osc1_mode", 1.0f);
                    setParam (source, "osc1_excite", 11.0f);
                    source.getStateInformation (state);
                }
                auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
                xml->setAttribute ("exciterLevels", savedLevels);
                juce::AudioProcessor::copyXmlToBinary (*xml, state);
                IlanaSynthAudioProcessor processor;
                processor.setStateInformation (state.getData(), (int) state.getSize());
                juce::MemoryBlock again;
                processor.getStateInformation (again);
                resaved = juce::AudioProcessor::getXmlFromBinary (again.getData(), (int) again.getSize())->getIntAttribute ("exciterLevels");
                processor.prepareToPlay (48000.0, 256);
                juce::AudioBuffer<float> buffer (2, 256);
                auto peak = 0.0f;
                for (int start = 0; start < 24000; start += 256)
                {
                    juce::MidiBuffer midi;
                    if (start == 0)
                        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                    buffer.clear();
                    processor.processBlock (buffer, midi);
                    peak = juce::jmax (peak, buffer.getMagnitude (0, 256));
                }
                return juce::Decibels::gainToDecibels (peak);
            };
            int resavedOld = -1, resavedNew = -1;
            const auto old = peakAfterLoad (1, resavedOld), current = peakAfterLoad (2, resavedNew);
            check (current - old > 15.0f && resavedOld == 1 && resavedNew == 2,
                   "a Modal Piano saved before its trim keeps its level (" + juce::String (old, 1) + " dB vs "
                       + juce::String (current, 1) + " dB now; saved again as " + juce::String (resavedOld) + ")");
        }
    }
}
} // namespace M82

void runM82PianoTests() { M82::runTests(); }
