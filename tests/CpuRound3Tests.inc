// CPU round, 2026-11-01 ("add cpu optimizations in general"). Included by
// TableTest.cpp; ILANA_CPU3_TEST=1 runs only these. Every change in that
// round keeps the sound bit for bit, and each is checked here against the
// code it replaced:
//  - FreeverbFast against juce::Reverb (stereo and mono, parameter moves).
//  - OpenLowPass::processLinkedRun against process() per sample.
//  - TableFFT's SSE2 butterflies against the scalar ones they replaced.
//  - SamplePlayer::setParamsIsNoOp only says yes when setParams changes nothing.

namespace CpuRound3
{
// The scalar butterflies TableFFT used before (reference for the SSE2 path).
inline void referenceFft (int order, const std::complex<float>* input, std::complex<float>* output, bool inverse)
{
    const auto size = 1 << order;
    std::vector<std::complex<double>> twiddles ((size_t) size / 2), data ((size_t) size);
    std::vector<int> bitReversed ((size_t) size);
    for (int k = 0; k < size / 2; ++k)
    {
        const auto angle = -2.0 * 3.14159265358979323846 * k / size;
        twiddles[(size_t) k] = { std::cos (angle), std::sin (angle) };
    }
    for (int i = 0; i < size; ++i)
    {
        auto reversed = 0;
        for (int bit = 1, rest = i; bit < size; bit <<= 1, rest >>= 1)
            reversed = (reversed << 1) | (rest & 1);
        bitReversed[(size_t) i] = reversed;
    }
    for (int i = 0; i < size; ++i)
        data[(size_t) bitReversed[(size_t) i]] = std::complex<double> (input[i]);
    for (int length = 2; length <= size; length <<= 1)
    {
        const auto half = length / 2, stride = size / length;
        for (int start = 0; start < size; start += length)
            for (int k = 0; k < half; ++k)
            {
                const auto& t = twiddles[(size_t) (k * stride)];
                const auto wr = t.real(), wi = inverse ? -t.imag() : t.imag();
                auto& x = data[(size_t) (start + k)];
                auto& y = data[(size_t) (start + k + half)];
                const auto br = y.real() * wr - y.imag() * wi, bi = y.real() * wi + y.imag() * wr;
                y = { x.real() - br, x.imag() - bi };
                x = { x.real() + br, x.imag() + bi };
            }
    }
    const auto scale = inverse ? 1.0 / size : 1.0;
    for (int i = 0; i < size; ++i)
        output[i] = std::complex<float> (data[(size_t) i] * scale);
}
} // namespace CpuRound3

void runCpuRound3Tests()
{
    std::cout << "--- CPU round 2026-11-01 (bit-exact) ---" << std::endl;
    juce::Random random (20261101);

    // Reverb: the same output as juce::Reverb, bit for bit.
    {
        auto same = true;
        for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        {
            juce::Reverb reference;
            FreeverbFast fast;
            reference.setSampleRate (rate);
            fast.setSampleRate (rate);
            const auto block = 300;
            std::vector<float> aL (block), aR (block), bL (block), bR (block), aM (block), bM (block);
            for (int pass = 0; pass < 200; ++pass)
            {
                juce::Reverb::Parameters p;
                p.roomSize = random.nextFloat();
                p.damping = random.nextFloat();
                p.wetLevel = random.nextFloat();
                p.dryLevel = random.nextFloat();
                p.width = random.nextFloat();
                p.freezeMode = pass % 37 == 5 ? 1.0f : 0.0f;
                if (pass % 3 == 0)
                {
                    reference.setParameters (p);
                    fast.setParameters (p);
                }
                for (int i = 0; i < block; ++i)
                {
                    aL[(size_t) i] = bL[(size_t) i] = pass < 150 ? random.nextFloat() * 2.0f - 1.0f : 0.0f;
                    aR[(size_t) i] = bR[(size_t) i] = pass < 150 ? random.nextFloat() * 2.0f - 1.0f : 0.0f;
                    aM[(size_t) i] = bM[(size_t) i] = aL[(size_t) i];
                }
                reference.processStereo (aL.data(), aR.data(), block);
                fast.processStereo (bL.data(), bR.data(), block);
                same = same && std::memcmp (aL.data(), bL.data(), sizeof (float) * (size_t) block) == 0
                       && std::memcmp (aR.data(), bR.data(), sizeof (float) * (size_t) block) == 0;
                if (pass == 120)
                {
                    reference.reset();
                    fast.reset();
                }
            }
            // Mono on fresh ones (each keeps its own state).
            juce::Reverb referenceMono;
            FreeverbFast fastMono;
            referenceMono.setSampleRate (rate);
            fastMono.setSampleRate (rate);
            for (int pass = 0; pass < 60; ++pass)
            {
                for (int i = 0; i < block; ++i)
                    aM[(size_t) i] = bM[(size_t) i] = random.nextFloat() * 2.0f - 1.0f;
                referenceMono.processMono (aM.data(), block);
                fastMono.processMono (bM.data(), block);
                same = same && std::memcmp (aM.data(), bM.data(), sizeof (float) * (size_t) block) == 0;
            }
        }
        check (same, "FreeverbFast is juce::Reverb bit for bit (stereo, mono, freeze, reset, 44.1/48/96 kHz)");
    }

    // The open Y Low Pass: a linked run is process() sample by sample, and
    // the pair block copies the left state where the sides first differ.
    {
        Airwindows::OpenLowPass reference, fast;
        reference.set (44100.0, 20000.0, 0.2);
        fast.set (44100.0, 20000.0, 0.2);
        const auto n = 512;
        std::vector<float> inL (n), inR (n), outL (n), outR (n);
        for (int i = 0; i < n; ++i)
            inL[(size_t) i] = inR[(size_t) i] = random.nextFloat() * 2.0f - 1.0f;
        inR[400] += 0.25f; // the sides part here
        const auto count = fast.processLinkedRun (inL.data(), inR.data(), outL.data(), outR.data(), n);
        auto same = count == 400;
        for (int i = 0; i < count; ++i)
        {
            const auto y = reference.process (inL[(size_t) i]);
            same = same && std::memcmp (&y, &outL[(size_t) i], sizeof (float)) == 0
                   && std::memcmp (&y, &outR[(size_t) i], sizeof (float)) == 0;
        }
        // The state carries on as process()'s does.
        const auto y1 = reference.process (0.5f), y2 = fast.process (0.5f);
        same = same && std::memcmp (&y1, &y2, sizeof (float)) == 0;
        check (same, "OpenLowPass::processLinkedRun is process() bit for bit, stops where the sides differ");
    }

    // TableFFT: the SSE2 butterflies are the scalar ones bit for bit.
    {
        auto same = true;
        for (const auto order : { 1, 2, 5, 11 })
        {
            const auto size = 1 << order;
            const TableFFT fft (order);
            std::vector<std::complex<float>> input ((size_t) size), a ((size_t) size), b ((size_t) size);
            for (auto& value : input)
                value = { random.nextFloat() * 2.0f - 1.0f, random.nextFloat() * 2.0f - 1.0f };
            for (const auto inverse : { false, true })
            {
                fft.perform (input.data(), a.data(), inverse);
                CpuRound3::referenceFft (order, input.data(), b.data(), inverse);
                same = same && std::memcmp (a.data(), b.data(), sizeof (std::complex<float>) * (size_t) size) == 0;
            }
        }
        check (same, "TableFFT is the scalar FFT bit for bit (forward and inverse, 2 to 2048 points)");
    }

    // SamplePlayer: a skipped setParams is one that would change nothing.
    {
        SamplePlayer player;
        SamplePlayer::Params none;
        check (player.setParamsIsNoOp (none), "an idle sample player with no sample skips a no-sample setParams");
        auto stretched = none;
        stretched.stretch = true;
        check (! player.setParamsIsNoOp (stretched), "a stretch change is not skipped");
        player.setParams (stretched);
        check (player.setParamsIsNoOp (stretched), "...and is skipped once set");
        SampleData data;
        data.buffer.setSize (1, 1000);
        data.buffer.clear();
        auto withSample = none;
        withSample.sample = &data;
        check (! player.setParamsIsNoOp (withSample), "a sample arriving is not skipped");
        player.setParams (withSample);
        check (! player.setParamsIsNoOp (none), "a sample leaving is not skipped");
    }
}
