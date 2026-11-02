#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "SampleInterpolation.h"
#include "SampleOnsets.h"
#include "SampleRange.h"
#include "SampleTempo.h"

#include <array>
#include <atomic>
#include <cmath>
#include <iterator>
#include <memory>
#include <vector>

struct SampleData;

// Float audio files can carry NaN, Inf or absurd values; a NaN would latch in
// the filters. Non-finite samples become 0 and the rest is held to +-16.
inline void scrubAudio (juce::AudioBuffer<float>& buffer) noexcept
{
    for (int c = 0; c < buffer.getNumChannels(); ++c)
    {
        auto* p = buffer.getWritePointer (c);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            p[i] = std::isfinite (p[i]) ? juce::jlimit (-16.0f, 16.0f, p[i]) : 0.0f;
    }
}

// One region of a multisample (a SoundFont or SFZ instrument): the audio a
// range of keys and velocities plays, the key it was recorded at and its
// loop.
struct SampleZone
{
    std::shared_ptr<SampleData> data;
    int loKey = 0, hiKey = 127, loVel = 1, hiVel = 127;
    double rootNote = 60.0; // fractional: tuning folded in
    float gain = 1.0f;
    bool loop = false;
    int loopStart = 0, loopEnd = 0; // samples into data
    // Roadmap step 36: where the zone came from, so an edited map can be
    // saved as edits to its file: its index in the file it was read from,
    // or (origin -1) the audio file it was added from.
    int origin = -1;
    juce::String file;
    // Roadmap step 37: the order of a round-robin alternate among the zones
    // covering the same note (an SFZ's seq_position; else the file's order).
    int sequence = 0;
    // Roadmap step 32: a slice (or any zone) can play backwards on its own.
    bool reverse = false;

    bool covers (int note, int velocity) const noexcept
    {
        return note >= loKey && note <= hiKey && velocity >= loVel && velocity <= hiVel;
    }

    double rootHz() const noexcept { return 440.0 * std::exp2 ((rootNote - 69.0) / 12.0); }
};

// Roadmap step 37: the plugin's round-robin counters, one per oscillator and
// key, shared by every voice, so each new note on a key steps on to the next
// of its alternates.
struct SampleRoundRobin
{
    std::array<std::array<std::atomic<unsigned>, 128>, 8> counters {};

    unsigned next (int osc, int note) noexcept
    {
        return counters[(size_t) (osc & 7)][(size_t) (note & 127)].fetch_add (1u, std::memory_order_relaxed);
    }
};
// The pitch a plain (zoneless) sample was recorded at, from the ROOT parameter.
// 60 gives the literal C3 frequency every older patch used, bit for bit.
inline double plainRootHz (float rootNote) noexcept
{
    return rootNote == 60.0f ? 261.6255653005986 : 440.0 * std::exp2 (((double) rootNote - 69.0) / 12.0);
}

struct SampleData
{
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    juce::String name;
    // A multisample's regions. Empty for a plain sample; otherwise buffer
    // holds the region under middle C, for the displays.
    std::vector<SampleZone> zones;
    // Roadmap step 43: what the load left out (a file cut at 120 seconds,
    // an SFZ's missing files), for the display. Empty when all of it came in.
    juce::String loadWarning;
    // Roadmap step 35: where its attacks start (frames), for a random start
    // that lands on one. Found once when the sample comes in (findOnsets).
    std::vector<int> onsets;
    bool onsetsFound = false;
    // Roadmap step 32: a plain sample cut into slices, each a zone on its own
    // key. buffer stays the whole sample (for the display); sliceStarts are
    // the cuts in frames, from 0 to its length. sliceMode 0: not sliced.
    int sliceMode = 0;
    int sliceCount = 8;
    float sliceSensitivity = 0.5f;
    int sliceBaseKey = 36;
    std::vector<float> sliceMarkers; // the hand-placed cuts (fractions)
    std::vector<int> sliceStarts;
    bool isSliced() const noexcept { return sliceMode != 0 && ! zones.empty(); }
    void copySlicingFrom (const SampleData& other)
    {
        sliceMode = other.sliceMode;
        sliceCount = other.sliceCount;
        sliceSensitivity = other.sliceSensitivity;
        sliceBaseKey = other.sliceBaseKey;
        sliceMarkers = other.sliceMarkers;
        sliceStarts = other.sliceStarts;
        if (other.sliceMode != 0)
        {
            buffer = other.buffer;
            sampleRate = other.sampleRate;
        }
    }

    // Finds the attacks once (message thread, at load: about 10 ms for two
    // minutes of stereo).
    void findOnsets()
    {
        if (onsetsFound || getNumSamples() < 2)
            return;
        const float* channels[2] { buffer.getReadPointer (0), buffer.getReadPointer (juce::jmin (1, getNumChannels() - 1)) };
        onsets = SampleOnsets::find (channels, juce::jmin (2, getNumChannels()), getNumSamples(), sampleRate);
        onsetsFound = true;
    }

    int getNumChannels() const noexcept { return buffer.getNumChannels(); }
    int getNumSamples() const noexcept { return buffer.getNumSamples(); }

    // The region a note plays: the one that covers it, else the nearest by
    // key (a velocity outside every range takes the closest layer), or none
    // when silenceOutside asks for silence there (step 36).
    const SampleZone* zoneFor (int note, int velocity, bool silenceOutside = false) const noexcept
    {
        if (silenceOutside)
        {
            for (const auto& zone : zones)
                if (zone.data != nullptr && zone.covers (note, velocity))
                    return &zone;
            return nullptr;
        }

        return nearestZone (note, velocity);
    }

    // Roadmap step 37: with round robin (1 Cycle, 2 Random) a note covered
    // by several zones plays the pick-th of them in their sequence order;
    // off (0), the first, as before.
    const SampleZone* zoneFor (int note, int velocity, bool silenceOutside, int roundRobin, unsigned pick) const noexcept
    {
        if (roundRobin == 0)
            return zoneFor (note, velocity, silenceOutside);
        constexpr int maxAlternates = 64;
        std::array<const SampleZone*, maxAlternates> covering {};
        auto count = 0;
        for (const auto& zone : zones)
            if (zone.data != nullptr && zone.covers (note, velocity) && count < maxAlternates)
                covering[(size_t) count++] = &zone;
        if (count <= 1)
            return zoneFor (note, velocity, silenceOutside);
        // In sequence order; zones with the same sequence keep the file's order.
        std::stable_sort (covering.begin(), covering.begin() + count,
                          [] (const SampleZone* a, const SampleZone* b) { return a->sequence < b->sequence; });
        return covering[(size_t) (pick % (unsigned) count)];
    }

    // Roadmap step 37: velocity blending. A velocity within width * 32 steps
    // of the zone's layer edge also plays the layer across that edge, the
    // two mixed equal-power so the sum stays as loud: partnerGain for it and
    // keep for the zone. Null (keep 1) when nothing blends.
    const SampleZone* blendPartner (const SampleZone& zone, int note, int velocity, float width, int roundRobin, unsigned pick,
                                    float& keep, float& partnerGain) const noexcept
    {
        keep = 1.0f;
        partnerGain = 0.0f;
        if (width <= 0.0f)
            return nullptr;
        const auto reach = juce::jlimit (0.0f, 1.0f, width) * 32.0f;
        const auto v = (float) velocity;
        const SampleZone* partner = nullptr;
        auto towardPartner = 0.0f; // 0..0.5: how far the mix leans to the partner
        if (zone.hiVel < 127 && (float) zone.hiVel + 0.5f - v < reach)
        {
            partner = zoneFor (note, zone.hiVel + 1, true, roundRobin, pick);
            towardPartner = (v - ((float) zone.hiVel + 0.5f) + reach) / (2.0f * reach);
        }
        else if (zone.loVel > 1 && v - ((float) zone.loVel - 0.5f) < reach)
        {
            partner = zoneFor (note, zone.loVel - 1, true, roundRobin, pick);
            towardPartner = (reach - (v - ((float) zone.loVel - 0.5f))) / (2.0f * reach);
        }
        if (partner == nullptr || partner == &zone)
            return nullptr;
        towardPartner = juce::jlimit (0.0f, 0.5f, towardPartner);
        keep = std::cos (towardPartner * juce::MathConstants<float>::halfPi);
        partnerGain = std::sin (towardPartner * juce::MathConstants<float>::halfPi);
        return partner;
    }

private:
    const SampleZone* nearestZone (int note, int velocity) const noexcept
    {
        const SampleZone* best = nullptr;
        auto bestDistance = 1 << 30;
        for (const auto& zone : zones)
        {
            if (zone.data == nullptr)
                continue;
            if (zone.covers (note, velocity))
                return &zone;
            const auto keyDistance = note < zone.loKey ? zone.loKey - note : (note > zone.hiKey ? note - zone.hiKey : 0);
            const auto velDistance = velocity < zone.loVel ? zone.loVel - velocity : (velocity > zone.hiVel ? velocity - zone.hiVel : 0);
            const auto distance = keyDistance * 128 + velDistance;
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = &zone;
            }
        }
        return best;
    }
};

class SamplePlayer
{
public:
    struct Params
    {
        const SampleData* sample = nullptr;
        bool loop = false;
        bool reverse = false;
        float start = 0.0f;
        float end = 1.0f;
        float fadeIn = 0.0f;
        float fadeOut = 0.0f;
        float gain = 1.0f;
        // A multisample region's own loop (samples); used when loopEnd > loopStart.
        int loopStart = 0, loopEnd = 0;
        // Roadmap step 27: with LOOP on, the loop is its own span (fractions of
        // the sample, held inside START..END): the note plays from START once
        // and then repeats loopFrom..loopTo, never going back to START. Off,
        // as before: LOOP wraps the whole played range.
        bool ownLoop = false;
        float loopFrom = 0.25f, loopTo = 0.75f;
        // Roadmap step 28: at the wrap, the loop's end is blended into the
        // sound just before its start (reversed: the sound after its end) for
        // this fraction of half the loop. 0, as before, is a hard wrap.
        // LOOP only; not the SF2 / SFZ zone loops or STRETCH.
        float loopCrossfade = 0.0f;
        // Roadmap step 40: STRETCH keeps the pitch apart from the speed. The
        // read position moves at the time rate and short overlapping grains
        // read it at the pitch rate (setRates). Off, as before: one read at
        // one rate (setPlaybackRatio).
        bool stretch = false;
        // The same sound swapped for a rendered copy of it (or back): a
        // playing note carries on from the same place in it, with a short
        // crossfade, rather than starting again.
        bool keepPosition = false;
        // Roadmap step 33: how a note's release treats the sample. Gate, as
        // before: LOOP (and a region's loop) decide, the envelope ends it.
        // One Shot: plays START to END once, loops ignored, and the voice
        // holds it past the key's release. Sustain Loop: loops while the key
        // is down (LOOP or not), then plays on out of the loop to END.
        int playback = 0;
        // Roadmap step 34: the loop runs forward and back instead of jumping
        // to its start (whichever span loops: LOOP PTS, a region's loop or
        // the whole START..END). Off, as before.
        bool pingPong = false;
        // Roadmap step 35: each note starts up to this fraction of the way
        // into the played range (a fresh random offset per note and unison
        // voice). With startSafe the offset moves back to the attack before
        // it, onto a zero crossing, so no note starts mid-hit. 0, as before.
        float startRandom = 0.0f;
        bool startSafe = false;
        // Roadmap step 39: how the read between samples is worked out
        // (SampleInterpolation::Quality): Linear, as before; Cubic; Sinc;
        // Sinc with anti-aliasing for strong upward transposition. STRETCH's
        // grains stay linear.
        int quality = 0;
    };

    enum Playback { gate = 0, oneShot = 1, sustainLoop = 2 };

    void prepare (double newSampleRate)
    {
        grainLength = juce::jmax (64, 2 * juce::roundToInt (0.023 * newSampleRate));
        window.resize ((size_t) grainLength);
        // A periodic Hann: two grains half a length apart sum to exactly one.
        for (int i = 0; i < grainLength; ++i)
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / grainLength));
    }

    void reset()
    {
        active = false;
        released = false;
        bounced = false;
        tail.remaining = 0;
        position = regionStart;
    }

    // randomUnit (0..1) places the start for a random start (step 35).
    void trigger (float randomUnit = 0.0f)
    {
        if (params.sample == nullptr || params.sample->getNumSamples() <= 1)
        {
            active = false;
            return;
        }

        position = params.reverse ? regionEnd - 1.0 : regionStart;
        if (params.startRandom > 0.0f && randomUnit > 0.0f)
            position = randomStart (randomUnit);
        active = true;
        released = false;
        bounced = false;
        tail.remaining = 0;

        if (params.stretch)
            startGrains();
    }

    // True when setParams (newParams) would change nothing that is ever
    // read again: no sample before or after, nothing playing, and the same
    // stretch flag (the one field a later sample's setParams compares). The
    // voice asks every block for every oscillator's eight players, and
    // most oscillators play no sample.
    bool setParamsIsNoOp (const Params& newParams) const noexcept
    {
        return newParams.sample == nullptr && params.sample == nullptr && ! active
               && newParams.stretch == params.stretch && lastLength == 0;
    }

    void setParams (const Params& newParams)
    {
        const auto newSample = newParams.sample != params.sample;
        const auto newStretch = newParams.stretch && ! params.stretch;
        const auto* oldSample = params.sample;
        // The old length was kept, not read through oldSample: an idle voice's
        // pointer may belong to a copy the stretch pool has since freed.
        const auto oldLength = lastLength;
        params = newParams;
        lastLength = params.sample != nullptr ? params.sample->getNumSamples() : 0;

        if (params.sample == nullptr || params.sample->getNumSamples() <= 1)
        {
            active = false;
            return;
        }

        const auto length = (double) params.sample->getNumSamples();
        regionStart = juce::jlimit (0.0, length - 1.0, (double) juce::jlimit (0.0f, 0.999f, params.start) * length);
        regionEnd = juce::jlimit (regionStart + 1.0, length, (double) juce::jlimit (0.0f, 1.0f, params.end) * length);
        regionLength = regionEnd - regionStart;

        ownLoopOn = false;
        if (params.ownLoop && loopCapable())
        {
            const auto resolved = SampleRange::resolveLoop (regionStart, regionEnd, (double) params.loopFrom * length,
                                                            (double) params.loopTo * length, minLoopLength);
            ownLoopOn = resolved.valid;
            ownLoopStart = resolved.from;
            ownLoopEnd = resolved.to;
        }

        // The crossfade's reach: the loop's span (the whole played range
        // without LOOP PTS), blended over at most half of it and only as far
        // as there is sound beyond the loop's far side to blend in.
        xfadeLength = 0.0;
        const auto zoneLoop = ! ownLoopOn && params.loopEnd > params.loopStart;
        if (loopCapable() && params.loopCrossfade > 0.0f && ! params.stretch && ! zoneLoop && ! params.pingPong)
        {
            xfadeStart = ownLoopOn ? ownLoopStart : regionStart;
            xfadeEnd = ownLoopOn ? ownLoopEnd : regionEnd;
            const auto room = params.reverse ? length - xfadeEnd : xfadeStart;
            xfadeLength = juce::jmin ((double) juce::jlimit (0.0f, 1.0f, params.loopCrossfade) * 0.5 * (xfadeEnd - xfadeStart), room - 2.0);
            if (xfadeLength < 2.0)
                xfadeLength = 0.0;
        }

        if (newSample && params.keepPosition && oldSample != nullptr && oldLength > 1)
        {
            if (active)
            {
                if (params.stretch)
                {
                    tail = { oldSample, grains, tailLength() };
                    position *= (double) params.sample->getNumSamples() / (double) oldLength;
                    startGrains();
                }
                else
                {
                    position *= (double) params.sample->getNumSamples() / (double) oldLength;
                }
            }
        }
        else if (newSample)
            trigger();
        else if (newStretch && active)
            startGrains();
    }

    void setPlaybackRatio (double newRatio)
    {
        ratio = juce::jlimit (0.0001, 64.0, std::abs (newRatio));
    }

    // STRETCH: the read position moves at timeRate source samples per output
    // sample, the grains read at pitchRate. (Each includes the sample rates' ratio.)
    void setRates (double timeRate, double pitchRate)
    {
        ratio = juce::jlimit (0.0001, 64.0, std::abs (timeRate));
        pitch = juce::jlimit (0.0001, 64.0, std::abs (pitchRate));
    }

    // The grains' length in output samples (about 46 ms); set by prepare.
    int getGrainLength() const noexcept { return grainLength; }

    bool isActive() const noexcept { return active; }

    // Step 33: the key let go. A Sustain Loop leaves its loop and plays on
    // to END; the other modes don't change.
    void release() noexcept
    {
        released = true;
        bounced = false; // a ping-pong heading back turns to play on out
    }

    // A One Shot still playing: the voice holds its release until it ends.
    bool holdsRelease() const noexcept { return active && params.playback == oneShot; }

    double getSourcePosition() const noexcept { return position; }

    void process (float& left, float& right)
    {
        left = 0.0f;
        right = 0.0f;

        if (! active || params.sample == nullptr)
            return;

        const auto looping = loopingNow();

        // Step 34: a ping-pong loop turns at its ends instead of wrapping.
        if (looping && params.pingPong)
            bounceInLoop();
        // The loop's own span (roadmap step 27): the first pass through the
        // attack is not repeated. Reversed, the note runs the other way and
        // wraps when it falls below the loop's start.
        else if (ownLoopOn && looping)
        {
            const auto length = ownLoopEnd - ownLoopStart;
            if (params.reverse)
            {
                if (position < ownLoopStart)
                    position = ownLoopEnd - std::fmod (ownLoopStart - position, length);
            }
            else if (position >= ownLoopEnd)
                position = ownLoopStart + std::fmod (position - ownLoopStart, length);
        }
        // A region's sustain loop: forward only, inside the played range.
        else if (zoneLoopPlays() && params.loopEnd > params.loopStart && ! params.reverse && position >= (double) params.loopEnd
            && params.loopEnd <= regionEnd && params.loopStart >= regionStart)
            position -= (double) (params.loopEnd - params.loopStart);

        if ((position >= regionEnd || position < regionStart) && ! (looping && params.pingPong))
        {
            if (! looping)
            {
                // START modulated above the playhead moves it up; only the
                // end (or a reverse note's start) finishes a one-shot.
                if (! params.reverse && position < regionStart)
                    position = regionStart;
                else
                {
                    active = false;
                    return;
                }
            }
            else if (params.reverse)
            {
                while (position < regionStart)
                    position += regionLength;
            }
            else
            {
                while (position >= regionEnd)
                    position -= regionLength;
            }
        }

        if (params.stretch)
        {
            processStretched (left, right);
            return;
        }

        const auto* sample = params.sample;
        const auto numSamples = sample->getNumSamples();
        const auto index = juce::jlimit (0, numSamples - 1, (int) std::floor (position));
        const auto next = juce::jmin (index + 1, numSamples - 1);
        const auto frac = (float) (position - (double) index);

        const auto quality = (SampleInterpolation::Quality) juce::jlimit (0, 3, params.quality);
        const auto readChannel = [&] (int channel)
        {
            const auto* data = sample->buffer.getReadPointer (juce::jmin (channel, sample->getNumChannels() - 1));
            if (quality != SampleInterpolation::Quality::Linear)
                return SampleInterpolation::read (data, numSamples, position, quality, ratio);
            return data[index] + (data[next] - data[index]) * frac;
        };

        auto readL = readChannel (0);
        auto readR = sample->getNumChannels() > 1 ? readChannel (1) : readL;

        // Step 28: near the wrap the loop's end gives way to the sound just
        // beyond its other side, so the jump lands on matching material.
        if (xfadeLength > 0.0 && looping)
        {
            const auto distance = params.reverse ? position - xfadeStart : xfadeEnd - position;
            if (distance >= 0.0 && distance < xfadeLength)
            {
                const auto partner = params.reverse ? xfadeEnd + distance : xfadeStart - distance;
                const auto partnerIndex = juce::jlimit (0, numSamples - 1, (int) std::floor (partner));
                const auto partnerNext = juce::jmin (partnerIndex + 1, numSamples - 1);
                const auto partnerFrac = (float) (partner - (double) partnerIndex);
                const auto readPartner = [&] (int channel)
                {
                    const auto* data = sample->buffer.getReadPointer (juce::jmin (channel, sample->getNumChannels() - 1));
                    if (quality != SampleInterpolation::Quality::Linear)
                        return SampleInterpolation::read (data, numSamples, partner, quality, ratio);
                    return data[partnerIndex] + (data[partnerNext] - data[partnerIndex]) * partnerFrac;
                };
                // Linear: the blend never exceeds the louder of the two reads.
                const auto blend = (float) (1.0 - distance / xfadeLength);
                const auto keep = 1.0f - blend;
                readL = readL * keep + readPartner (0) * blend;
                readR = readR * keep + readPartner (sample->getNumChannels() > 1 ? 1 : 0) * blend;
            }
        }

        const auto posSinceStart = params.reverse ? (regionEnd - position) : (position - regionStart);
        const auto posToEnd = params.reverse ? (position - regionStart) : (regionEnd - position);

        auto gain = 1.0;

        const auto fadeInLength = (double) params.fadeIn * regionLength;

        if (fadeInLength > 1.0)
            gain *= juce::jlimit (0.0, 1.0, posSinceStart / fadeInLength);

        const auto fadeOutLength = (double) params.fadeOut * regionLength;

        if (fadeOutLength > 1.0)
            gain *= juce::jlimit (0.0, 1.0, posToEnd / fadeOutLength);

        gain *= (double) params.gain;
        left = readL * (float) gain;
        right = readR * (float) gain;

        position += direction() * ratio;
    }

private:
    // Step 33: whether the note can loop at all (LOOP, or a Sustain Loop),
    // and whether it loops now (a Sustain Loop only until the release).
    bool loopCapable() const noexcept
    {
        return params.playback == sustainLoop || (params.playback == gate && params.loop);
    }

    bool loopingNow() const noexcept
    {
        return params.playback == sustainLoop ? ! released : params.playback == gate && params.loop;
    }

    // A region's own sustain loop: always in Gate, as before; never in a
    // One Shot; in a Sustain Loop until the release.
    bool zoneLoopPlays() const noexcept
    {
        return params.playback == gate || (params.playback == sustainLoop && ! released);
    }

    // Step 35: a start up to startRandom of the way in, kept before an own
    // loop's end (a note still reaches its loop), moved back onto an attack
    // and a zero crossing when startSafe asks.
    double randomStart (float randomUnit) const noexcept
    {
        const auto& sample = *params.sample;
        const auto limit = ownLoopOn ? (params.reverse ? regionEnd - ownLoopStart : ownLoopEnd - regionStart) : regionLength;
        const auto offset = (double) juce::jlimit (0.0f, 1.0f, params.startRandom) * (double) juce::jlimit (0.0f, 1.0f, randomUnit)
                            * juce::jmax (0.0, limit - 2.0);
        auto start = params.reverse ? regionEnd - 1.0 - offset : regionStart + offset;
        if (params.startSafe)
        {
            const auto frame = (int) start;
            if (! params.reverse)
            {
                const auto attack = SampleOnsets::atOrBefore (sample.onsets, frame);
                start = attack >= 0 && (double) attack >= regionStart ? (double) attack : regionStart;
            }
            const float* channels[2] { sample.buffer.getReadPointer (0), sample.buffer.getReadPointer (juce::jmin (1, sample.getNumChannels() - 1)) };
            const auto radius = juce::jmax (1, (int) (0.002 * juce::jmax (8000.0, sample.sampleRate)));
            start = (double) SampleOnsets::nearestZeroCrossing (channels, juce::jmin (2, sample.getNumChannels()), sample.getNumSamples(), (int) start, radius);
        }
        return juce::jlimit (regionStart, regionEnd - 1.0, start);
    }

    // Which way the read moves: REVERSE, turned by a ping-pong bounce.
    double direction() const noexcept { return (params.reverse ? -1.0 : 1.0) * (bounced ? -1.0 : 1.0); }

    // Step 34: folds the position back into the looping span, turning the
    // read at each end (a fast read may cross the span more than once).
    void bounceInLoop() noexcept
    {
        auto from = regionStart, to = regionEnd;
        if (ownLoopOn)
            from = ownLoopStart, to = ownLoopEnd;
        else if (zoneLoopPlays() && params.loopEnd > params.loopStart && params.loopEnd <= regionEnd && params.loopStart >= regionStart)
            from = (double) params.loopStart, to = (double) params.loopEnd;
        const auto length = to - from;
        if (length < 2.0)
            return;
        // The last readable place before the span's end.
        const auto top = to - 1.0e-6;
        const auto forward = direction() > 0.0;
        if (forward && position > top)
        {
            const auto over = std::fmod (position - top, 2.0 * length);
            position = over < length ? top - over : from + (over - length);
            bounced = over < length ? ! bounced : bounced;
        }
        else if (! forward && position < from)
        {
            const auto over = std::fmod (from - position, 2.0 * length);
            position = over < length ? from + over : top - (over - length);
            bounced = over < length ? ! bounced : bounced;
        }
    }

    struct Grain
    {
        double read = 0.0; // source position
        int age = 0;       // output samples since it started
        bool on = false;
    };

    // Both channels' sum at a whole sample, for the grain search.
    static float monoAt (const SampleData& sample, int index) noexcept
    {
        if (index < 0 || index >= sample.getNumSamples())
            return 0.0f;
        auto sum = sample.buffer.getSample (0, index);
        if (sample.getNumChannels() > 1)
            sum += sample.buffer.getSample (1, index);
        return sum;
    }

    void startGrains()
    {
        if (grainLength <= 0)
            prepare (44100.0);
        // The first grain starts at its peak, so a note's attack is not faded
        // in; the second starts with it, reading the same audio.
        grains[0] = { position, grainLength / 2, true };
        grains[1] = { position, 0, true };
        nextGrain = 0; // the older grain is the next to be replaced
        hopCountdown = grainLength / 2;
    }

    // WSOLA: the new grain starts near the read position, where its audio
    // best continues what the running grain plays now (a short correlation
    // search, coarse then fine), so overlapping grains add in phase. About
    // 10 multiply-adds per output sample at 44.1 kHz.
    double alignedStart (double target, double continuation) const noexcept
    {
        const auto& sample = *params.sample;
        const auto numSamples = sample.getNumSamples();
        const auto rate = juce::jmax (8000.0, sample.sampleRate);
        const auto direction = params.reverse ? -1 : 1;
        const auto searchRange = (int) (0.010 * rate);
        const auto span = (int) (0.008 * rate);
        const auto coarse = juce::jmax (1, (int) (rate / 5512.5)); // 8 at 44.1 kHz
        const auto fine = juce::jmax (1, coarse / 4);
        const auto from = (int) std::floor (continuation);
        const auto* left = sample.buffer.getReadPointer (0);
        const auto* right = sample.buffer.getReadPointer (sample.getNumChannels() > 1 ? 1 : 0);

        // Correlation of the window at start with the one at from, over the
        // first window's level. Indices outside the sample read silence.
        const auto score = [&] (int start, int step)
        {
            auto cross = 0.0f, energy = 1.0e-9f;
            const auto last = (span - 1) / step * step;
            const auto inside = [numSamples] (int a, int b) { return juce::jmin (a, b) >= 0 && juce::jmax (a, b) < numSamples; };
            if (inside (start, start + last * direction) && inside (from, from + last * direction))
            {
                for (int j = 0; j < span; j += step)
                {
                    const auto a = start + j * direction, b = from + j * direction;
                    const auto x = left[a] + right[a];
                    cross += x * (left[b] + right[b]);
                    energy += x * x;
                }
            }
            else
            {
                for (int j = 0; j < span; j += step)
                {
                    const auto x = monoAt (sample, start + j * direction);
                    cross += x * monoAt (sample, from + j * direction);
                    energy += x * x;
                }
            }
            return cross / std::sqrt (energy);
        };

        const auto centre = (int) std::floor (target);
        auto best = centre;
        auto bestScore = -1.0e30f;
        for (int offset = -searchRange; offset <= searchRange; offset += coarse)
            if (const auto value = score (centre + offset, coarse); value > bestScore)
            {
                bestScore = value;
                best = centre + offset;
            }

        // (Fine scores are compared only with each other: the step changes the scale.)
        const auto rough = best;
        bestScore = -1.0e30f;
        for (int offset = -coarse + 1; offset < coarse; ++offset)
            if (const auto value = score (rough + offset, fine); value > bestScore)
            {
                bestScore = value;
                best = rough + offset;
            }

        return (double) best + (target - std::floor (target));
    }

    void processStretched (float& left, float& right)
    {
        const auto& sample = *params.sample;
        const auto direction = this->direction();

        if (hopCountdown <= 0)
        {
            // The younger grain is at its peak: the older one has ended.
            const auto& running = grains[(size_t) (1 - nextGrain)];
            grains[(size_t) nextGrain] = { alignedStart (position, running.read), 0, true };
            nextGrain = 1 - nextGrain;
            hopCountdown = grainLength / 2;
        }

        float sumL = 0.0f, sumR = 0.0f;
        const auto numSamples = sample.getNumSamples();
        const auto* dataL = sample.buffer.getReadPointer (0);
        const auto* dataR = sample.buffer.getReadPointer (sample.getNumChannels() > 1 ? 1 : 0);
        for (auto& grain : grains)
        {
            if (! grain.on)
                continue;
            if (grain.age >= grainLength)
            {
                grain.on = false;
                continue;
            }
            const auto weight = window[(size_t) grain.age];
            // Linear read; silence outside the sample.
            const auto index = (int) std::floor (grain.read);
            if (index >= 0 && index < numSamples)
            {
                const auto next = juce::jmin (index + 1, numSamples - 1);
                const auto frac = (float) (grain.read - (double) index);
                sumL += (dataL[index] + (dataL[next] - dataL[index]) * frac) * weight;
                sumR += (dataR[index] + (dataR[next] - dataR[index]) * frac) * weight;
            }
            grain.read += direction * pitch;
            ++grain.age;
        }

        // After a swap to a rendered copy: the old sound's grains fade out.
        if (tail.remaining > 0)
        {
            const auto fade = (float) tail.remaining / (float) tailLength();
            const auto& old = *tail.sample;
            const auto* oldL = old.buffer.getReadPointer (0);
            const auto* oldR = old.buffer.getReadPointer (old.getNumChannels() > 1 ? 1 : 0);
            for (auto& grain : tail.grains)
            {
                if (! grain.on || grain.age >= grainLength)
                {
                    grain.on = false;
                    continue;
                }
                const auto index = (int) std::floor (grain.read);
                if (index >= 0 && index < old.getNumSamples())
                {
                    const auto next = juce::jmin (index + 1, old.getNumSamples() - 1);
                    const auto frac = (float) (grain.read - (double) index);
                    const auto weight = window[(size_t) grain.age] * fade;
                    sumL += (oldL[index] + (oldL[next] - oldL[index]) * frac) * weight;
                    sumR += (oldR[index] + (oldR[next] - oldR[index]) * frac) * weight;
                }
                grain.read += direction * pitch;
                ++grain.age;
            }
            --tail.remaining;
        }

        const auto posSinceStart = params.reverse ? (regionEnd - position) : (position - regionStart);
        const auto posToEnd = params.reverse ? (position - regionStart) : (regionEnd - position);

        auto gain = 1.0;
        const auto fadeInLength = (double) params.fadeIn * regionLength;
        if (fadeInLength > 1.0)
            gain *= juce::jlimit (0.0, 1.0, posSinceStart / fadeInLength);
        const auto fadeOutLength = (double) params.fadeOut * regionLength;
        if (fadeOutLength > 1.0)
            gain *= juce::jlimit (0.0, 1.0, posToEnd / fadeOutLength);
        gain *= (double) params.gain;

        left = sumL * (float) gain;
        right = sumR * (float) gain;
        position += direction * ratio;
        --hopCountdown;
    }

    // The old sound's grains after a swap, fading out (they read the old
    // sample, which stays alive at least seconds after it's let go).
    struct Tail
    {
        const SampleData* sample = nullptr;
        std::array<Grain, 2> grains {};
        int remaining = 0;
    };

    int tailLength() const noexcept { return juce::jmax (1, grainLength / 4); }

    Params params;
    int lastLength = 0; // params.sample's length, kept so the old sample is never dereferenced
    Tail tail;
    std::vector<float> window;
    std::array<Grain, 2> grains {};
    int grainLength = 0, nextGrain = 0, hopCountdown = 0;
    double pitch = 1.0;
    double position = 0.0;
    double regionStart = 0.0;
    double regionEnd = 0.0;
    double regionLength = 0.0;
    // The loop's own span in sample frames, when one is in use.
    static constexpr double minLoopLength = 16.0;
    double ownLoopStart = 0.0, ownLoopEnd = 0.0;
    // The loop crossfade (step 28): the wrapping span and the blend's length.
    double xfadeStart = 0.0, xfadeEnd = 0.0, xfadeLength = 0.0;
    bool ownLoopOn = false;
    double ratio = 1.0;
    bool active = false;
    bool released = false; // step 33: the key is up
    bool bounced = false;  // step 34: a ping-pong loop is running back
};
