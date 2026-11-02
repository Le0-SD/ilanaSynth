#pragma once

// The Modal Piano exciter: a grand piano rebuilt from its partials.
//
// Where the Piano exciter (PianoString) models the hammer and string, this
// one plays back what a reference grand measured: for each recorded note and
// velocity, every partial's frequency and its level over time
// (ModalPianoTable, from tools/build_modal_piano.py). A note between two
// recorded ones blends their two nearest notes and velocity layers. Each
// partial is a sine (a complex rotator; eight at a time with AVX, else
// four with SSE) whose level follows the measured curve at a block rate.
// Partials that have gone silent, or sit 90 dB under the loudest one, stop
// running in groups of eight. The first few tens of
// milliseconds are the recording's own attack (hammer thump and all),
// resampled to the note's pitch, crossfading into the partials.
//
// In stereo, as the reference's microphones heard it: each partial and board
// resonance has its own balance and phase between left and right (steady
// over a note, so measured once per note), the attack is the recording's two
// channels, and the noise is two uncorrelated draws. process() returns the
// middle, (left + right) / 2, and side() the rest, (left - right) / 2.
//
// The knobs move away from the recording, which they reproduce at their
// middle (DECAY, STIFF, HAMMER) or at 0 (DAMP):
//   DECAY   stretches or shortens the whole decay (x0.35 .. x2.8)
//   DAMP    makes the upper partials die away faster
//   STIFF   0 is a harmonic string, the middle the measured stretch, 1 twice it
//   HAMMER  plays the timbre of a softer or harder strike at the same loudness
//   DAMPER  how fast the dampers stop the note on release (0 rings on)
// Eco keeps the first half of the partials.

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <complex>
#include <cmath>

#if defined(_M_X64) || defined(__x86_64__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
 #define ILANA_MODAL_SSE 1
 #include <xmmintrin.h>
 #include <immintrin.h>
 // The AVX path is compiled for AVX on its own and picked at run time (the
 // build targets SSE2), as the WEST folder's is.
 #if defined(__GNUC__) || defined(__clang__)
  #define ILANA_MODAL_AVX_TARGET __attribute__ ((target ("avx")))
 #else
  #define ILANA_MODAL_AVX_TARGET
 #endif
#else
 #define ILANA_MODAL_SSE 0
#endif

#include "ModalPianoTable.h"

class ModalPiano
{
public:
    static constexpr int maxPartials = 100;
    static constexpr int maxModes = 16;     // the board's resonances
    static constexpr int group = 8;         // slots run and skipped together (one AVX register, two SSE)
    static constexpr int maxSlots = (maxPartials + maxModes + group - 1) / group * group;
    static constexpr int blockSize = 32;
    // Tests and benches turn it off to compare with every partial running
    // (CPU round 2026-10-31): skipping groups of silent partials, and
    // partials 90 dB under the loudest one (relativeCut).
    inline static bool skipSilentGroups = true;
    inline static float relativeCut = 90.0f;
    // ILANA_NO_AVX (or a CPU without it) keeps the SSE path.
    static bool& useAvx()
    {
       #if ILANA_MODAL_SSE
        static bool use = juce::SystemStats::hasAVX()
                          && juce::SystemStats::getEnvironmentVariable ("ILANA_NO_AVX", "").isEmpty();
       #else
        static bool use = false;
       #endif
        return use;
    }

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        useAvx(); // (its first call reads the environment: here, not on the audio thread)
        designNoise();
        reset();
    }

    void reset()
    {
        count = slots = 0;
        attackLeft = 0;
        silent = true;
    }

    void setNote (double hz, int midiNote)
    {
        if (hz == frequency && midiNote == note)
            return;
        frequency = juce::jlimit (8.0, sampleRate * 0.45, hz);
        note = midiNote;
        pitchDirty = true;
    }

    void setParams (float newDecay, float newDamp, float newStiffness, float newHardness, float newDamper, bool newEco)
    {
        timeScale = std::exp2 ((juce::jlimit (0.0f, 1.0f, newDecay) - 0.5f) * 3.0f);
        const auto damp = juce::jlimit (0.0f, 1.0f, newDamp);
        dampRate = 40.0f * damp * damp;
        if (const auto stretch = 2.0f * juce::jlimit (0.0f, 1.0f, newStiffness); stretch != stiffness)
        {
            stiffness = stretch;
            pitchDirty = true;
        }
        hardness = juce::jlimit (0.0f, 1.0f, newHardness);
        damper = juce::jlimit (0.0f, 1.0f, newDamper);
        eco = newEco;
    }

    void trigger (float velocity, float randomUnit)
    {
        const auto& table = ModalPianoTable::get();
        count = slots = 0;
        attackLeft = 0;
        silent = true;
        if (! table.valid())
            return;

        const auto played = 127.0f * juce::jlimit (0.0f, 1.0f, velocity);
        // HAMMER: the timbre of a softer or harder strike (up to 50 velocity
        // steps either way), brought back to this velocity's loudness.
        const auto timbre = played + (hardness - 0.5f) * 100.0f;
        Corners at, loudness;
        findCorners (table, note, timbre, at);
        findCorners (table, note, played, loudness);
        auto gainDb = energyDb (table, loudness) - energyDb (table, at);
        // Below the softest recorded layer, quieter still.
        const auto softest = (float) table.velocities.front();
        if (played < softest)
            gainDb += 30.0f * std::log10 (juce::jmax (1.0f, played) / softest);

        // The partials: frequency ratios blended in log, levels in dB.
        auto partials = 0;
        for (const auto& c : at.entries)
            partials = juce::jmax (partials, c->partials);
        partials = juce::jmin (partials, eco ? maxPartials / 2 : maxPartials);
        const auto top = juce::jmin (16000.0, sampleRate * 0.45);
        auto seed = (std::uint32_t) (randomUnit * 4294967040.0f) | 1u;

        for (int k = 0; k < partials; ++k)
        {
            auto logRatio = 0.0f;
            for (int c = 0; c < 4; ++c)
                logRatio += at.weights[c] * std::log (ratioOf (*at.entries[c], k));
            const auto ratio = std::exp (logRatio);
            if (ratio * frequency > top)
                break;
            measured[k] = ratio;
            harmonic[k] = (float) (k + 1);
            // Random start phases: a real strike's partials don't line up.
            seed = seed * 1664525u + 22695477u;
            const auto phase = (float) (seed >> 8) * (juce::MathConstants<float>::twoPi / 16777216.0f);
            re[k] = std::cos (phase);
            im[k] = std::sin (phase);
            amp[k] = 0.0f;
            count = k + 1;
        }
        corners = at;
        levelOffset = gainDb;

        // The nearest recorded note gives the attack and the board's
        // resonances (fixed in Hz, they don't follow the key), at the two
        // velocity layers round the timbre.
        const auto nearest = at.noteWeight < 0.5f ? at.noteIndex : at.noteIndex + (at.noteWeight > 0.0f ? 1 : 0);
        modeA = &table.at (nearest, at.velocityIndex);
        modeB = &table.at (nearest, juce::jmin (at.velocityIndex + 1, (int) table.velocities.size() - 1));
        slots = count;
        const auto modes = juce::jmin ((int) modeA->modes.size(), (int) modeB->modes.size(), maxModes);
        for (int m = 0; m < modes; ++m)
        {
            const auto k = slots++;
            modeHz[m] = modeA->modes[(size_t) m];
            seed = seed * 1664525u + 22695477u;
            const auto phase = (float) (seed >> 8) * (juce::MathConstants<float>::twoPi / 16777216.0f);
            re[k] = std::cos (phase);
            im[k] = std::sin (phase);
            amp[k] = 0.0f;
        }
        for (int k = slots; k < roundedCount(); ++k)
        {
            measured[k] = harmonic[k] = re[k] = im[k] = amp[k] = 0.0f;
            rotCos[k] = 1.0f;
            rotSin[k] = 0.0f;
            turnCos[k] = turnSin[k] = 0.0f;
            blockTurn[k] = 0.0;
        }
        updatePitch();

        // The stereo picture: the nearest note's, per partial and mode.
        for (int k = 0; k < roundedCount(); ++k)
        {
            auto balance = 0.0f, phase = 0.0f;
            if (k < count && k < (int) modeA->balance.size())
            {
                balance = modeA->balance[(size_t) k];
                phase = modeA->phase[(size_t) k];
            }
            else if (k >= count && k < slots)
            {
                balance = modeA->modeBalance[(size_t) (k - count)];
                phase = modeA->modePhase[(size_t) (k - count)];
            }
            // Each channel's gain, at the two channels' power average of 1.
            const auto ratio = std::pow (10.0f, balance / 20.0f);
            const auto right = std::sqrt (2.0f / (1.0f + ratio * ratio));
            gainLeft[k] = ratio * right;
            rightSin[k] = right * std::cos (phase);
            rightCos[k] = right * std::sin (phase);
        }

        // The attack, resampled to this note's pitch.
        attackA = &modeA->attack;
        attackB = &modeB->attack;
        attackRightA = &modeA->attackRight;
        attackRightB = &modeB->attackRight;
        attackMix = at.velocityWeight;
        attackGain = std::pow (10.0f, gainDb / 20.0f);
        const auto anchorHz = 440.0 * std::exp2 ((table.notes[(size_t) nearest] - 69) / 12.0);
        attackStep = frequency / anchorHz * table.rate / sampleRate;
        const auto usable = juce::jmin ((double) std::min ({ attackA->size(), attackB->size(), attackRightA->size(), attackRightB->size() }) - 2.0,
                                        0.08 * table.rate);
        attackLength = juce::jmax (1, (int) (usable / attackStep));
        fadeStart = juce::jmin ((int) (0.025 * sampleRate), attackLength / 3);
        attackLeft = attackLength;
        attackPosition = 0.0;
        attackFade = 1.0f;
        partialRamp = 0.0f;
        hasNoise = ! modeA->noiseLevels.empty();
        for (auto& band : noise)
        {
            band.left = band.right = {};
            band.gain = band.step = 0.0f;
        }
        noisePower.fill (0.0f);
        noiseSeed = seed;
        noiseSeedRight = seed * 2654435761u + 12345u;
        lastSide = 0.0f;

        elapsed = 0;
        released = -1;
        segment = -1;
        loudestPartial = -200.0f;
        std::fill (std::begin (groupLive), std::end (groupLive), false);
        std::fill (std::begin (missedTurn), std::end (missedTurn), 0.0);
        untilBlock = 0;
        silent = slots == 0 && attackLeft == 0;
    }

    // The middle of the two channels; side() is the rest.
    float process (bool noteHeld)
    {
        if (silent)
        {
            lastSide = 0.0f;
            return 0.0f;
        }
        if (! noteHeld && released < 0)
            released = elapsed;
        if (untilBlock == 0)
            nextBlock();
        --untilBlock;
        ++elapsed;

        // Each partial: sin on the left; on the right, the same sine shifted
        // by its phase (sin (x + p) = sin x cos p + cos x sin p). The
        // rotator's length is the partial's level: its turn (turnCos/Sin)
        // also grows or shrinks it to the next block's level, so no
        // separate level ramp runs per sample (Mathews & Smith's phasor
        // filter, SMAC 2003).
        auto left = 0.0f, right = 0.0f;
       #if ILANA_MODAL_SSE
        if (useAvx())
            sumPartialsAvx (left, right);
        else
        {
            auto sumLeft = _mm_setzero_ps(), sumRight = _mm_setzero_ps();
            for (int g = 0; g < liveGroups; ++g)
                for (int k = liveGroup[g]; k < liveGroup[g] + group; k += 4)
                {
                    const auto r = _mm_load_ps (re + k), i = _mm_load_ps (im + k);
                    const auto c = _mm_load_ps (turnCos + k), s = _mm_load_ps (turnSin + k);
                    const auto nextRe = _mm_sub_ps (_mm_mul_ps (r, c), _mm_mul_ps (i, s));
                    const auto nextIm = _mm_add_ps (_mm_mul_ps (r, s), _mm_mul_ps (i, c));
                    _mm_store_ps (re + k, nextRe);
                    _mm_store_ps (im + k, nextIm);
                    sumLeft = _mm_add_ps (sumLeft, _mm_mul_ps (nextIm, _mm_load_ps (gainLeft + k)));
                    sumRight = _mm_add_ps (sumRight, _mm_add_ps (_mm_mul_ps (nextIm, _mm_load_ps (rightSin + k)),
                                                                 _mm_mul_ps (nextRe, _mm_load_ps (rightCos + k))));
                }
            alignas (16) float lanes[4];
            _mm_store_ps (lanes, sumLeft);
            left = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
            _mm_store_ps (lanes, sumRight);
            right = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
        }
       #else
        for (int g = 0; g < liveGroups; ++g)
            for (int k = liveGroup[g]; k < liveGroup[g] + group; ++k)
            {
                const auto r = re[k];
                re[k] = r * turnCos[k] - im[k] * turnSin[k];
                im[k] = r * turnSin[k] + im[k] * turnCos[k];
                left += im[k] * gainLeft[k];
                right += im[k] * rightSin[k] + re[k] * rightCos[k];
            }
       #endif
        if (hasNoise)
        {
            // Uniform, unit variance; a draw for each channel.
            noiseSeed = noiseSeed * 1664525u + 1013904223u;
            noiseSeedRight = noiseSeedRight * 1664525u + 1013904223u;
            const auto whiteLeft = ((float) (noiseSeed >> 8) * (2.0f / 16777216.0f) - 1.0f) * 1.7320508f;
            const auto whiteRight = ((float) (noiseSeedRight >> 8) * (2.0f / 16777216.0f) - 1.0f) * 1.7320508f;
            for (auto& band : noise)
            {
                left += band.gain * band.filter (band.left, whiteLeft);
                right += band.gain * band.filter (band.right, whiteRight);
                band.gain += band.step;
            }
        }
        left *= partialRamp;
        right *= partialRamp;
        partialRamp += partialRampStep;

        if (attackLeft > 0)
        {
            const auto i = (int) attackPosition;
            const auto f = (float) (attackPosition - i);
            const auto at = [i, f] (const std::vector<float>& x) { return x[(size_t) i] + f * (x[(size_t) i + 1] - x[(size_t) i]); };
            const auto gain = attackGain * attackFade;
            const auto fromLeft = at (*attackA) + attackMix * (at (*attackB) - at (*attackA));
            const auto fromRight = at (*attackRightA) + attackMix * (at (*attackRightB) - at (*attackRightA));
            left += fromLeft * gain;
            right += fromRight * gain;
            attackFade += attackFadeStep;
            attackPosition += attackStep;
            --attackLeft;
        }
        lastSide = 0.5f * (left - right) * outputGain;
        return 0.5f * (left + right) * outputGain;
    }

    // (left - right) / 2 of the sample process() last returned.
    float side() const { return lastSide; }

   #if ILANA_MODAL_SSE
    // The partials, eight to an AVX register.
    ILANA_MODAL_AVX_TARGET void sumPartialsAvx (float& left, float& right)
    {
        auto sumLeft = _mm256_setzero_ps(), sumRight = _mm256_setzero_ps();
        for (int g = 0; g < liveGroups; ++g)
        {
            const auto k = liveGroup[g];
            const auto r = _mm256_loadu_ps (re + k), i = _mm256_loadu_ps (im + k);
            const auto c = _mm256_loadu_ps (turnCos + k), s = _mm256_loadu_ps (turnSin + k);
            const auto nextRe = _mm256_sub_ps (_mm256_mul_ps (r, c), _mm256_mul_ps (i, s));
            const auto nextIm = _mm256_add_ps (_mm256_mul_ps (r, s), _mm256_mul_ps (i, c));
            _mm256_storeu_ps (re + k, nextRe);
            _mm256_storeu_ps (im + k, nextIm);
            sumLeft = _mm256_add_ps (sumLeft, _mm256_mul_ps (nextIm, _mm256_loadu_ps (gainLeft + k)));
            sumRight = _mm256_add_ps (sumRight, _mm256_add_ps (_mm256_mul_ps (nextIm, _mm256_loadu_ps (rightSin + k)),
                                                               _mm256_mul_ps (nextRe, _mm256_loadu_ps (rightCos + k))));
        }
        left = sumOf (sumLeft);
        right = sumOf (sumRight);
    }
    static ILANA_MODAL_AVX_TARGET float sumOf (__m256 x)
    {
        const auto sum = _mm_add_ps (_mm256_castps256_ps128 (x), _mm256_extractf128_ps (x, 1));
        alignas (16) float lanes[4];
        _mm_store_ps (lanes, sum);
        return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
    }
   #endif

private:
    struct Corners
    {
        std::array<const ModalPianoTable::Entry*, 4> entries {}; // lo/hi note x lo/hi velocity
        std::array<float, 4> weights {};
        int noteIndex = 0, velocityIndex = 0;
        float noteWeight = 0.0f, velocityWeight = 0.0f;
    };

    static void bracket (const std::vector<int>& values, float x, int& index, float& weight)
    {
        index = 0;
        weight = 0.0f;
        const auto last = (int) values.size() - 1;
        if (last <= 0 || x <= (float) values.front())
            return;
        if (x >= (float) values.back())
        {
            index = last;
            return;
        }
        while (index < last - 1 && (float) values[(size_t) index + 1] <= x)
            ++index;
        weight = (x - (float) values[(size_t) index]) / (float) (values[(size_t) index + 1] - values[(size_t) index]);
    }

    static void findCorners (const ModalPianoTable& table, int midiNote, float velocity, Corners& c)
    {
        bracket (table.notes, (float) midiNote, c.noteIndex, c.noteWeight);
        bracket (table.velocities, velocity, c.velocityIndex, c.velocityWeight);
        const auto nextNote = juce::jmin (c.noteIndex + 1, (int) table.notes.size() - 1);
        const auto nextVelocity = juce::jmin (c.velocityIndex + 1, (int) table.velocities.size() - 1);
        c.entries = { &table.at (c.noteIndex, c.velocityIndex), &table.at (nextNote, c.velocityIndex),
                      &table.at (c.noteIndex, nextVelocity), &table.at (nextNote, nextVelocity) };
        c.weights = { (1.0f - c.noteWeight) * (1.0f - c.velocityWeight), c.noteWeight * (1.0f - c.velocityWeight),
                      (1.0f - c.noteWeight) * c.velocityWeight, c.noteWeight * c.velocityWeight };
    }

    // A partial past an entry's last one: the stretch carried on from it.
    static float ratioOf (const ModalPianoTable::Entry& e, int k)
    {
        if (k < e.partials)
            return juce::jmax (0.05f, e.ratios[(size_t) k]);
        if (e.partials < 2)
            return (float) (k + 1);
        const auto last = (float) e.partials;
        const auto b = juce::jmax (1.0e-7f, ((e.ratios.back() / last) * (e.ratios.back() / last) - 1.0f) / (last * last));
        const auto n = (float) (k + 1);
        return n * std::sqrt (1.0f + b * n * n);
    }

    static float levelOf (const ModalPianoTable::Entry& e, int k, int t, int times)
    {
        return k < e.partials ? e.levels[(size_t) (k * times + t)] : -160.0f;
    }

    // The strike's loudness: all partials' power at the first time.
    static float energyDb (const ModalPianoTable& table, const Corners& c)
    {
        const auto times = (int) table.times.size();
        auto power = 1.0e-20;
        for (int k = 0; k < maxPartials; ++k)
        {
            auto level = 0.0f;
            for (int i = 0; i < 4; ++i)
                level += c.weights[(size_t) i] * levelOf (*c.entries[(size_t) i], k, 0, times);
            power += std::pow (10.0, level / 10.0);
        }
        return (float) (10.0 * std::log10 (power));
    }

    // The rotator's length is the partial's level. Each block it is set
    // back to exactly the level it should be at (no drift), then its turn
    // is scaled so the length goes there and on to the next block's level
    // along an exponential, which is how a level that falls in dB falls.
    // A silent partial keeps a tiny length (-160 dB) so its phase stays.
    void setLevel (int k, float target)
    {
        static constexpr float floorLevel = 1.0e-8f;
        const auto from = amp[k] > 0.0f ? amp[k] : floorLevel;
        const auto length2 = re[k] * re[k] + im[k] * im[k];
        auto scale = length2 > 0.0f ? from / std::sqrt (length2) : 1.0f;
        auto start = from;
        if (amp[k] == 0.0f && target > 0.0f)
        {
            // Starting (the strike, or back from silence): at its level at once.
            scale *= target / floorLevel;
            start = target;
        }
        re[k] *= scale;
        im[k] *= scale;
        const auto to = target > 0.0f ? target : floorLevel;
        const auto growth = start == to ? 1.0f : std::exp2 (std::log2 (to / start) * (1.0f / (float) blockSize));
        turnCos[k] = rotCos[k] * growth;
        turnSin[k] = rotSin[k] * growth;
        amp[k] = target;
    }

    int roundedCount() const { return (slots + group - 1) / group * group; }

    void updatePitch()
    {
        pitchDirty = false;
        for (int k = 0; k < count; ++k)
        {
            const auto ratio = harmonic[k] + (measured[k] - harmonic[k]) * stiffness;
            hz[k] = (float) (ratio * frequency);
            const auto w = juce::MathConstants<double>::twoPi * juce::jmin ((double) hz[k], sampleRate * 0.49) / sampleRate;
            rotCos[k] = (float) std::cos (w);
            rotSin[k] = (float) std::sin (w);
            blockTurn[k] = blockSize * (double) std::atan2 (rotSin[k], rotCos[k]);
        }
        for (int k = count; k < slots; ++k)
        {
            hz[k] = modeHz[k - count];
            const auto w = juce::MathConstants<double>::twoPi * hz[k] / sampleRate;
            rotCos[k] = (float) std::cos (w);
            rotSin[k] = (float) std::sin (w);
            blockTurn[k] = blockSize * (double) std::atan2 (rotSin[k], rotCos[k]);
        }
    }

    // Every block: each partial's level on the measured curve (log time
    // between the table's times), the knobs' extra losses, and a ramp to it.
    void nextBlock()
    {
        untilBlock = blockSize;
        if (pitchDirty)
            updatePitch();

        const auto& table = ModalPianoTable::get();
        const auto times = (int) table.times.size();
        const auto now = (double) (elapsed + blockSize) / sampleRate;
        const auto t = now / timeScale;

        auto s = 0;
        while (s < times - 2 && t >= table.times[(size_t) s + 1])
            ++s;
        if (s != segment)
        {
            segment = s;
            for (int k = 0; k < count; ++k)
            {
                auto a = 0.0f, b = 0.0f;
                for (int i = 0; i < 4; ++i)
                {
                    a += corners.weights[(size_t) i] * levelOf (*corners.entries[(size_t) i], k, s, times);
                    b += corners.weights[(size_t) i] * levelOf (*corners.entries[(size_t) i], k, s + 1, times);
                }
                levelA[k] = a + levelOffset;
                levelB[k] = b + levelOffset;
            }
            if (hasNoise)
                for (int b = 0; b < ModalPianoTable::noiseBandCount; ++b)
                {
                    auto a = 0.0f, c = 0.0f;
                    for (int i = 0; i < 4; ++i)
                    {
                        const auto& e = *corners.entries[(size_t) i];
                        a += corners.weights[(size_t) i] * e.noiseLevels[(size_t) (b * times + s)];
                        c += corners.weights[(size_t) i] * e.noiseLevels[(size_t) (b * times + s + 1)];
                    }
                    noiseA[b] = a + levelOffset;
                    noiseB[b] = c + levelOffset;
                }
            const auto w = corners.velocityWeight;
            for (int k = count; k < slots; ++k)
            {
                const auto m = (size_t) (k - count);
                const auto at = [times] (const ModalPianoTable::Entry& e, size_t mode, int t) { return e.modeLevels[mode * (size_t) times + (size_t) t]; };
                levelA[k] = at (*modeA, m, s) + w * (at (*modeB, m, s) - at (*modeA, m, s)) + levelOffset;
                levelB[k] = at (*modeA, m, s + 1) + w * (at (*modeB, m, s + 1) - at (*modeA, m, s + 1)) + levelOffset;
            }
        }

        const auto t0 = table.times[(size_t) s], t1 = table.times[(size_t) s + 1];
        const auto lastTime = table.times.back();
        // Before the first time: held. Past the last: the last segment's
        // fall per second carries on (at least 1 dB/s).
        const auto u = (float) juce::jlimit (0.0, 1.0, (std::log (t + 0.005) - std::log (t0 + 0.005))
                                                         / (std::log (t1 + 0.005) - std::log (t0 + 0.005)));
        const auto beyond = (float) juce::jmax (0.0, t - lastTime);
        const auto releasedFor = released < 0 ? 0.0f : (float) ((elapsed - released) / sampleRate);
        const auto releaseRate = damper * 250.0f * juce::jlimit (0.4f, 1.5f, std::sqrt ((float) frequency / 262.0f));
        const auto seconds = (float) now;

        // A partial this far under the loudest one (last block's) is not
        // heard next to it: it goes quiet as if under -140 dB.
        const auto cut = skipSilentGroups ? loudestPartial - relativeCut : -1000.0f;
        auto loudest = -200.0f, loudestNow = -200.0f;
        bool slotLive[maxSlots];
        for (int k = 0; k < slots; ++k)
        {
            auto db = levelA[k] + u * (levelB[k] - levelA[k]);
            if (beyond > 0.0f)
                db += juce::jmin (-1.0f, (levelB[k] - levelA[k]) / (t1 - t0)) * beyond;
            db -= (dampRate * juce::jmin (8.0f, hz[k] / 1000.0f)) * seconds
                  + releaseRate * releasedFor;
            loudest = juce::jmax (loudest, db);
            loudestNow = juce::jmax (loudestNow, db);
            const auto target = db < -140.0f || db < cut ? 0.0f : std::pow (10.0f, db / 20.0f);
            slotLive[k] = target > 0.0f || amp[k] > 0.0f;
            setLevel (k, target);
        }
        loudestPartial = loudestNow;
        for (int k = slots; k < roundedCount(); ++k)
            slotLive[k] = false;

        // Only groups with a sounding partial run this block. A silent
        // group's rotators stop too (CPU round 2026-10-31: most of a note's
        // upper partials are silent within a second, many from the strike).
        liveGroups = 0;
        for (int k = 0; k < roundedCount(); k += group)
        {
            auto live = ! skipSilentGroups;
            for (int j = k; j < k + group; ++j)
                live = live || slotLive[j];
            auto& wasLive = groupLive[k / group];
            if (live)
            {
                liveGroup[liveGroups++] = k;
                // Back from silence (the measured curve rises again, or a
                // DECAY move): catch its rotators up with the turns they
                // missed, so it comes back in the phase it would have had.
                if (! wasLive)
                    for (int j = k; j < k + group; ++j)
                    {
                        const auto c = std::cos (missedTurn[j]), sn = std::sin (missedTurn[j]);
                        const auto r = (double) re[j], i = (double) im[j];
                        re[j] = (float) (r * c - i * sn);
                        im[j] = (float) (r * sn + i * c);
                        missedTurn[j] = 0.0;
                    }
            }
            else
                for (int j = k; j < k + group; ++j)
                {
                    missedTurn[j] += blockTurn[j];
                    if (missedTurn[j] > juce::MathConstants<double>::twoPi)
                        missedTurn[j] -= juce::MathConstants<double>::twoPi;
                }
            wasLive = live;
        }

        if (hasNoise)
        {
            // The table's noise is the median FFT bin: back to power per bin,
            // then to the variance each band's filter needs at this rate
            // (the filters overlap: a few Gauss-Seidel passes find the
            // non-negative powers that come closest).
            const auto& table = ModalPianoTable::get();
            std::array<float, ModalPianoTable::noiseBandCount> variance {};
            const auto toVariance = (float) ModalPianoTable::noiseFftSize / 6.0f * (float) (sampleRate / table.rate) / 0.6931472f;
            for (int b = 0; b < ModalPianoTable::noiseBandCount; ++b)
            {
                auto db = noiseA[b] + u * (noiseB[b] - noiseA[b]);
                if (beyond > 0.0f)
                    db += juce::jmin (-1.0f, (noiseB[b] - noiseA[b]) / (t1 - t0)) * beyond;
                db -= releaseRate * releasedFor;
                loudest = juce::jmax (loudest, db);
                variance[(size_t) b] = db < -140.0f ? 0.0f : std::pow (10.0f, db / 10.0f) * toVariance;
            }
            auto& power = noisePower;
            for (int pass = 0; pass < 6; ++pass)
                for (int b = 0; b < ModalPianoTable::noiseBandCount; ++b)
                {
                    auto others = 0.0f;
                    for (int j = 0; j < ModalPianoTable::noiseBandCount; ++j)
                        if (j != b)
                            others += noiseLeak[b][j] * power[(size_t) j];
                    power[(size_t) b] = juce::jmax (0.0f, (variance[(size_t) b] - others) / noiseLeak[b][b]);
                }
            for (int j = 0; j < ModalPianoTable::noiseBandCount; ++j)
            {
                auto& band = noise[(size_t) j];
                band.step = (std::sqrt (power[(size_t) j]) - band.gain) / (float) blockSize;
            }
        }

        // The crossfade from the recording's attack into the partials, as
        // per-sample ramps to where it is at the block's end (a gain stepped
        // once a block buzzes: steps 1.5 kHz apart, heard as a hiss on top).
        const auto atEnd = (int) (attackLength - attackLeft) + blockSize;
        auto fadeTarget = 1.0f, rampTarget = 0.0f;
        if (atEnd > fadeStart)
        {
            const auto x = juce::jlimit (0.0f, 1.0f, (float) (atEnd - fadeStart) / (float) juce::jmax (1, attackLength - fadeStart));
            fadeTarget = std::cos (x * juce::MathConstants<float>::halfPi);
            rampTarget = std::sin (x * juce::MathConstants<float>::halfPi);
        }
        attackFadeStep = (fadeTarget - attackFade) / (float) blockSize;
        partialRampStep = (rampTarget - partialRamp) / (float) blockSize;

        if (loudest < -130.0f && attackLeft == 0)
            silent = true;
    }

    double sampleRate = 44100.0, frequency = 261.63;
    int note = 60;
    float timeScale = 1.0f, dampRate = 0.0f, stiffness = 1.0f, hardness = 0.5f, damper = 0.5f;
    bool eco = false, pitchDirty = true, silent = true;
    // The table's sinusoid levels are the reference's in dBFS (about 20 dB
    // quiet); Voice::exciterTrim brings the Modal Piano up to the other
    // oscillators' level, so this stays 1 (Modal Grand, saved without the
    // trim, keeps its level).
    static constexpr float outputGain = 1.0f;

    Corners corners;
    float levelOffset = 0.0f;
    int count = 0, slots = 0, segment = -1, untilBlock = 0; // partials, then partials and modes
    long long elapsed = 0, released = -1;

    alignas (32) float re[maxSlots] {}, im[maxSlots] {}, rotCos[maxSlots] {}, rotSin[maxSlots] {};
    alignas (32) float turnCos[maxSlots] {}, turnSin[maxSlots] {}; // this block's turn, with the level's change
    float amp[maxSlots] {}; // each rotator's length at the block's start (0: silent)
    float loudestPartial = -200.0f;
    int liveGroup[maxSlots / group] {}; // the first slot of each group that sounds this block
    int liveGroups = 0;
    bool groupLive[maxSlots / group] {};
    double missedTurn[maxSlots] {}; // radians a silent slot's rotator has not turned
    double blockTurn[maxSlots] {};  // radians its rotator turns in a block
    // The stereo picture: the left's gain, and the right's as the parts of
    // the sine and cosine that make its gain and phase.
    alignas (32) float gainLeft[maxSlots] {}, rightSin[maxSlots] {}, rightCos[maxSlots] {};
    float lastSide = 0.0f;
    float measured[maxSlots] {}, harmonic[maxSlots] {}, hz[maxSlots] {};
    float levelA[maxSlots] {}, levelB[maxSlots] {};

    float modeHz[maxModes] {};

    // The noise between the partials: white noise through one filter per
    // band of the table's (a low-pass, then octave band-passes), each two
    // biquads deep: the bass's noise is ~50 dB above the treble's, and a
    // second-order skirt would leak more than that into the top bands.
    struct NoiseBand
    {
        struct State { float x1 = 0, x2 = 0, y1 = 0, y2 = 0, u1 = 0, u2 = 0, z1 = 0, z2 = 0; };
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        State left, right;
        float gain = 0, step = 0;

        float filter (State& st, float white)
        {
            const auto y = b0 * white + b1 * st.x1 + b2 * st.x2 - a1 * st.y1 - a2 * st.y2;
            st.x2 = st.x1; st.x1 = white;
            st.y2 = st.y1; st.y1 = y;
            const auto z = b0 * y + b1 * st.u1 + b2 * st.u2 - a1 * st.z1 - a2 * st.z2;
            st.u2 = st.u1; st.u1 = y;
            st.z2 = st.z1; st.z1 = z;
            return z;
        }
    };
    std::array<NoiseBand, ModalPianoTable::noiseBandCount> noise {};
    float noiseA[ModalPianoTable::noiseBandCount] {}, noiseB[ModalPianoTable::noiseBandCount] {};
    float noiseLeak[ModalPianoTable::noiseBandCount][ModalPianoTable::noiseBandCount] {};
    std::array<float, ModalPianoTable::noiseBandCount> noisePower {};
    std::uint32_t noiseSeed = 1, noiseSeedRight = 2;
    bool hasNoise = false;

    void designNoise()
    {
        constexpr int bands = ModalPianoTable::noiseBandCount;
        const auto nyquist = 0.49 * sampleRate;
        for (int j = 0; j < bands; ++j)
        {
            const auto lo = (double) ModalPianoTable::noiseBands[j][0], hi = juce::jmin ((double) ModalPianoTable::noiseBands[j][1], nyquist);
            auto& f = noise[(size_t) j];
            if (j == 0)
            {
                // Below the band's top: the bass's noise falls ~30 dB by the
                // next band, and the skirt must be well down by then.
                const auto w = juce::MathConstants<double>::twoPi * 0.6 * hi / sampleRate, alpha = std::sin (w) / (2.0 * 0.7071), c = std::cos (w);
                const auto a0 = 1.0 + alpha;
                f.b0 = f.b2 = (float) ((1.0 - c) / 2.0 / a0);
                f.b1 = (float) ((1.0 - c) / a0);
                f.a1 = (float) (-2.0 * c / a0);
                f.a2 = (float) ((1.0 - alpha) / a0);
            }
            else
            {
                const auto centre = std::sqrt (lo * juce::jmax (lo * 1.01, hi)), q = 4.0; // (narrower than the band, for the same reason)
                const auto w = juce::MathConstants<double>::twoPi * centre / sampleRate, alpha = std::sin (w) / (2.0 * q), c = std::cos (w);
                const auto a0 = 1.0 + alpha;
                f.b0 = (float) (alpha / a0);
                f.b1 = 0.0f;
                f.b2 = (float) (-alpha / a0);
                f.a1 = (float) (-2.0 * c / a0);
                f.a2 = (float) ((1.0 - alpha) / a0);
            }
        }

        // noiseLeak[b][j]: band j's filter's mean power gain over band b
        // (log-spaced: the table's median sits near the band's middle).
        for (int b = 0; b < bands; ++b)
        {
            const auto lo = juce::jmax (20.0, (double) ModalPianoTable::noiseBands[b][0]);
            const auto hi = juce::jmin ((double) ModalPianoTable::noiseBands[b][1], nyquist);
            for (int j = 0; j < bands; ++j)
            {
                const auto& f = noise[(size_t) j];
                auto sum = 0.0;
                constexpr int points = 48;
                for (int i = 0; i < points; ++i)
                {
                    const auto hz = lo * std::pow (hi / lo, (i + 0.5) / points);
                    const auto z = std::polar (1.0, -juce::MathConstants<double>::twoPi * hz / sampleRate);
                    const auto h = ((double) f.b0 + (double) f.b1 * z + (double) f.b2 * z * z)
                                   / (1.0 + (double) f.a1 * z + (double) f.a2 * z * z);
                    sum += std::norm (h) * std::norm (h); // (two stages)
                }
                noiseLeak[b][j] = (float) (hi > lo ? sum / points : (b == j ? 1.0 : 0.0));
            }
            noiseLeak[b][b] = juce::jmax (1.0e-6f, noiseLeak[b][b]);
        }
    }
    const ModalPianoTable::Entry* modeA = nullptr;
    const ModalPianoTable::Entry* modeB = nullptr;
    const std::vector<float>* attackA = nullptr;
    const std::vector<float>* attackB = nullptr;
    const std::vector<float>* attackRightA = nullptr;
    const std::vector<float>* attackRightB = nullptr;
    float attackMix = 0.0f, attackGain = 1.0f, attackFade = 1.0f, partialRamp = 0.0f;
    float attackFadeStep = 0.0f, partialRampStep = 0.0f;
    double attackPosition = 0.0, attackStep = 1.0;
    int attackLength = 1, attackLeft = 0, fadeStart = 0;
};
