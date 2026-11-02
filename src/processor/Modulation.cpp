#include "ProcessorInternal.h"

void IlanaSynthAudioProcessor::setLfoCustomPoint (int lfoIndex, int step, float value)
{
    ++dataEpoch; // what the editor draws changes
    if (lfoIndex < 0 || lfoIndex >= numLfos || step < 0 || step >= lfoDrawSteps)
        return;

    const juce::SpinLock::ScopedLockType lock (lfoShapeLock);
    lfoCustom[(size_t) lfoIndex][(size_t) step] = juce::jlimit (-1.0f, 1.0f, value);
}

float IlanaSynthAudioProcessor::getLfoCustomPoint (int lfoIndex, int step) const
{
    if (lfoIndex < 0 || lfoIndex >= numLfos || step < 0 || step >= lfoDrawSteps)
        return 0.0f;

    const juce::SpinLock::ScopedLockType lock (lfoShapeLock);
    return lfoCustom[(size_t) lfoIndex][(size_t) step];
}

LfoCurve IlanaSynthAudioProcessor::getLfoCurve (int lfoIndex) const
{
    const juce::SpinLock::ScopedLockType lock (lfoShapeLock);
    return lfoCurves[(size_t) juce::jlimit (0, numLfos - 1, lfoIndex)];
}

void IlanaSynthAudioProcessor::setLfoCurve (int lfoIndex, const LfoCurve& curve)
{
    ++dataEpoch; // what the editor draws changes
    if (lfoIndex < 0 || lfoIndex >= numLfos)
        return;

    auto sanitised = curve;
    sanitised.sanitise();

    std::array<float, LfoCurve::tableSize> table {};
    sanitised.renderTable (table.data());

    const juce::SpinLock::ScopedLockType lock (lfoShapeLock);
    lfoCurves[(size_t) lfoIndex] = std::move (sanitised);
    lfoCurveTables[(size_t) lfoIndex] = table;
}

int IlanaSynthAudioProcessor::legacyMsegTargetLfo() const
{
    const auto param = [this] (const juce::String& id)
    {
        const auto* value = apvts.getRawParameterValue (id);
        return value != nullptr ? value->load() : 0.0f;
    };
    const auto routed = [this] (Mod::Source source)
    {
        for (int slot = 0; slot < Mod::maxSlots; ++slot)
        {
            const auto routing = readModSlot (slot);
            if (routing.destination != 0 && (routing.source == source || routing.aux == source))
                return true;
        }
        return false;
    };

    // A one-shot MSEG is an envelope, which an LFO can't be; as an
    // oscillator's ENVELOPE or warp envelope it plays per note.
    if (param ("mseg_loop") <= 0.5f || ! routed (Mod::Source::Mseg))
        return -1;
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
        if (juce::roundToInt (param (prefix + "_amp_env")) == 16 || juce::roundToInt (param (prefix + "_pd_env")) == 17)
            return -1;
    }

    // LFO 4 draws from the generator LFO 1-3, the Clocked S&H and the
    // gate's Random pattern share: at a new rate its draws land elsewhere,
    // so it takes the MSEG only when none of those is heard.
    auto sharedRandomHeard = routed (Mod::Source::ClockSh);
    for (int slot = 1; slot <= numFxSlots; ++slot)
        sharedRandomHeard = sharedRandomHeard || juce::roundToInt (param ("fx_slot" + juce::String (slot))) == 16;
    for (int lfo = 0; lfo < 3; ++lfo)
    {
        const auto shape = juce::roundToInt (param ("lfo" + juce::String (lfo + 1) + "_shape"));
        const auto random = shape == LfoShapes::SampleHold || LfoShapes::isStateful (shape) || LfoSimShapes::isSim (shape);
        sharedRandomHeard = sharedRandomHeard
                            || (random && (routed (Mod::lfoSourceFor (lfo)) || routed (Mod::lfoBSourceFor (lfo))));
    }

    for (int lfo = sharedRandomHeard ? 4 : 0; lfo < numLfos; ++lfo)
        if (! isLfoShown (lfo))
            return lfo;
    return -1;
}

bool IlanaSynthAudioProcessor::moveLegacyMsegToLfo()
{
    // ILANA_KEEP_MSEG_MODULE=1 keeps the module, to compare the two.
    const auto lfo = legacyMsegTargetLfo();
    if (lfo < 0 || juce::SystemStats::getEnvironmentVariable ("ILANA_KEEP_MSEG_MODULE", {}).isNotEmpty())
        return false;

    const auto param = [this] (const juce::String& id)
    {
        const auto* value = apvts.getRawParameterValue (id);
        return value != nullptr ? value->load() : 0.0f;
    };
    const auto set = [this] (const juce::String& id, float value)
    {
        if (auto* parameter = apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };

    // Its four points at their places in the cycle and back to the first at
    // the end, joined by straight lines, as the looping module plays them.
    float levels[Mseg::numPoints], times[Mseg::numPoints], total = 0.0f;
    for (int i = 0; i < Mseg::numPoints; ++i)
    {
        levels[i] = juce::jlimit (-1.0f, 1.0f, param ("mseg_level" + juce::String (i + 1)));
        times[i] = juce::jmax (0.01f, param ("mseg_time" + juce::String (i + 1)));
        total += times[i];
    }

    LfoCurve curve;
    curve.points.clear();
    auto x = 0.0f;
    for (int i = 0; i < Mseg::numPoints; ++i)
    {
        curve.points.push_back ({ x, levels[i], 0.0f });
        x += times[i] / total;
    }
    curve.points.push_back ({ 1.0f, levels[0], 0.0f });
    setLfoCurve (lfo, curve);

    const auto prefix = "lfo" + juce::String (lfo + 1);
    set (prefix + "_shape", (float) curveShape);
    set (prefix + "_rate", param ("mseg_rate"));
    for (const auto* off : { "_sync", "_retrig", "_key", "_phase", "_smooth" })
        set (prefix + off, 0.0f);

    const auto lfoSource = Mod::lfoSourceFor (lfo);
    for (int slot = 0; slot < Mod::maxSlots; ++slot)
    {
        const auto routing = readModSlot (slot);
        if (routing.destination == 0)
            continue;
        if (routing.source == Mod::Source::Mseg)
            setModSlotValue (slot, "src", (float) (int) lfoSource);
        if (routing.aux == Mod::Source::Mseg)
            setModSlotValue (slot, "aux", (float) (int) lfoSource);
        if (routing.destination == (int) Mod::Destination::MsegRate)
        {
            // The same octaves on the LFO's rate as on the MSEG's.
            setModSlotValue (slot, "dst", (float) (int) Mod::lfoRateDestinationFor (lfo));
            setModSlotValue (slot, "amt", routing.depth * Mod::Range::msegRateOctaves / Mod::Range::lfoRateOctaves);
        }
    }

    setRevealed (Module::Lfo, lfo, true);
    return true;
}

bool IlanaSynthAudioProcessor::isIdentityRemap (const LfoCurve& curve)
{
    const auto& p = curve.points;
    return p.size() == 2 && p[0].y == -1.0f && p[1].y == 1.0f && std::abs (p[0].tension) < 1.0e-3f;
}

LfoCurve IlanaSynthAudioProcessor::getModRemap (int slotIndex) const
{
    const juce::SpinLock::ScopedLockType lock (remapLock);
    const auto index = (size_t) juce::jlimit (0, Mod::maxSlots - 1, slotIndex);
    return modRemapOn[index].load() ? modRemaps[index] : identityRemap();
}

bool IlanaSynthAudioProcessor::isModRemapOn (int slotIndex) const
{
    return juce::isPositiveAndBelow (slotIndex, Mod::maxSlots) && modRemapOn[(size_t) slotIndex].load();
}

void IlanaSynthAudioProcessor::setModRemap (int slotIndex, const LfoCurve& curve)
{
    if (! juce::isPositiveAndBelow (slotIndex, Mod::maxSlots))
        return;

    ++dataEpoch; // what the editor draws changes
    auto sanitised = curve;
    sanitised.sanitise();
    const auto on = ! isIdentityRemap (sanitised);

    RemapTable table {};
    for (int i = 0; i < Mod::remapSize; ++i)
        table[(size_t) i] = sanitised.valueAt ((double) i / (double) Mod::remapSize);
    table[(size_t) Mod::remapSize] = sanitised.points.back().y;

    {
        const juce::SpinLock::ScopedLockType lock (remapLock);
        modRemaps[(size_t) slotIndex] = std::move (sanitised);
        modRemapTables[(size_t) slotIndex] = table;
        modRemapOn[(size_t) slotIndex].store (on);
    }
    ++remapEpoch;
}

void IlanaSynthAudioProcessor::resetAllModRemaps()
{
    for (int slot = 0; slot < Mod::maxSlots; ++slot)
        if (isModRemapOn (slot))
            resetModRemap (slot);
}

float IlanaSynthAudioProcessor::getLfoCurveValue (int lfoIndex, double phase) const
{
    const juce::SpinLock::ScopedLockType lock (lfoShapeLock);
    const auto& table = lfoCurveTables[(size_t) juce::jlimit (0, numLfos - 1, lfoIndex)];
    const auto position = (phase - std::floor (phase)) * (double) LfoCurve::tableSize;
    const auto index = (int) position % LfoCurve::tableSize;
    const auto next = (index + 1) % LfoCurve::tableSize;
    const auto frac = (float) (position - std::floor (position));
    return table[(size_t) index] + (table[(size_t) next] - table[(size_t) index]) * frac;
}

void IlanaSynthAudioProcessor::triggerPreviewNote (int midiNote, bool isOn, float velocity)
{
    if (! juce::isPositiveAndBelow (midiNote, 128))
        return;

    const auto scope = previewFifo.write (1);

    if (scope.blockSize1 > 0)
        previewEvents[(size_t) scope.startIndex1] = { midiNote, juce::jlimit (0.0f, 1.0f, velocity), isOn };
    else if (scope.blockSize2 > 0)
        previewEvents[(size_t) scope.startIndex2] = { midiNote, juce::jlimit (0.0f, 1.0f, velocity), isOn };
}

void IlanaSynthAudioProcessor::startMacroLearn (int macroIndex)
{
    paramLearn.store (-1);
    macroLearn.store (juce::jlimit (0, Mod::numMacros - 1, macroIndex));
}

namespace
{
juce::String parameterIdOf (const juce::AudioProcessorParameter* parameter)
{
    if (const auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*> (parameter))
        return withId->paramID;

    return {};
}
} // namespace

void IlanaSynthAudioProcessor::startParamLearn (const juce::String& parameterId)
{
    if (auto* parameter = apvts.getParameter (parameterId))
    {
        macroLearn.store (-1);
        paramLearn.store (parameter->getParameterIndex());
    }
}

void IlanaSynthAudioProcessor::cancelParamLearn()
{
    paramLearn.store (-1);
}

juce::String IlanaSynthAudioProcessor::getParamLearnTarget() const
{
    const auto index = paramLearn.load();
    return juce::isPositiveAndBelow (index, getParameters().size()) ? parameterIdOf (getParameters()[index]) : juce::String();
}

int IlanaSynthAudioProcessor::getParamCc (const juce::String& parameterId) const
{
    if (const auto* parameter = apvts.getParameter (parameterId))
        for (int cc = 0; cc < (int) ccParameter.size(); ++cc)
            if (ccParameter[(size_t) cc].load() == parameter->getParameterIndex())
                return cc;

    return -1;
}

void IlanaSynthAudioProcessor::clearParamCc (const juce::String& parameterId)
{
    if (const auto* parameter = apvts.getParameter (parameterId))
        for (auto& mapped : ccParameter)
            if (mapped.load() == parameter->getParameterIndex())
                mapped.store (-1);
}

// "cc=parameterId" pairs, comma-separated.
juce::String IlanaSynthAudioProcessor::getParamCcMapText() const
{
    juce::StringArray pairs;
    const auto& parameters = getParameters();

    for (int cc = 0; cc < (int) ccParameter.size(); ++cc)
        if (const auto index = ccParameter[(size_t) cc].load(); juce::isPositiveAndBelow (index, parameters.size()))
            pairs.add (juce::String (cc) + "=" + parameterIdOf (parameters[index]));

    return pairs.joinIntoString (",");
}

void IlanaSynthAudioProcessor::setParamCcMapText (const juce::String& text)
{
    for (auto& mapped : ccParameter)
        mapped.store (-1);

    for (const auto& pair : juce::StringArray::fromTokens (text, ",", ""))
    {
        const auto cc = pair.upToFirstOccurrenceOf ("=", false, false).getIntValue();

        if (const auto* parameter = apvts.getParameter (pair.fromFirstOccurrenceOf ("=", false, false));
            parameter != nullptr && juce::isPositiveAndBelow (cc, (int) ccParameter.size()))
            ccParameter[(size_t) cc].store (parameter->getParameterIndex());
    }
}

void IlanaSynthAudioProcessor::cancelMacroLearn()
{
    macroLearn.store (-1);
}

juce::File IlanaSynthAudioProcessor::getUserPresetDirectory() const
{
    if (userPresetDirectoryOverride != juce::File())
        return userPresetDirectoryOverride;

    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("ilanaSynth Presets");
}

juce::String IlanaSynthAudioProcessor::getModSlotParamId (int slotIndex, const juce::String& field) const
{
    return "mod" + juce::String (slotIndex + 1) + "_" + field;
}

void IlanaSynthAudioProcessor::setModSlotValue (int slotIndex, const juce::String& field, float value)
{
    if (auto* parameter = apvts.getParameter (getModSlotParamId (slotIndex, field)))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

Mod::Slot IlanaSynthAudioProcessor::readModSlot (int slotIndex) const
{
    Mod::Slot slot;

    if (! juce::isPositiveAndBelow (slotIndex, Mod::maxSlots))
        return slot;

    const auto& raw = modSlotRaw[(size_t) slotIndex];
    const auto read = [] (const std::atomic<float>* value) { return value != nullptr ? value->load() : 0.0f; };

    slot.source = (Mod::Source) juce::jlimit (0, (int) Mod::Source::Count - 1, (int) read (raw.src));
    slot.destination = juce::jlimit (0, Mod::getNumDestinations() - 1, (int) read (raw.dst));
    slot.depth = read (raw.amt);
    slot.curve = read (raw.curve);
    slot.polarity = (Mod::Polarity) juce::jlimit (0, 3, (int) read (raw.polarity));
    slot.aux = (Mod::Source) juce::jlimit (0, (int) Mod::Source::Count - 1, (int) read (raw.aux));
    slot.bypass = read (raw.bypass) > 0.5f;
    slot.stereo = read (raw.stereo) > 0.5f;
    // The editor's copy; processBlock points the audio thread's own.
    slot.remap = modRemapOn[(size_t) slotIndex].load() ? modRemapTables[(size_t) slotIndex].data() : nullptr;
    return slot;
}

int IlanaSynthAudioProcessor::getNumUsedModSlots() const
{
    auto used = 0;

    for (int i = 0; i < Mod::maxSlots; ++i)
    {
        const auto slot = readModSlot (i);

        if (slot.source != Mod::Source::None || slot.destination != 0)
            ++used;
    }

    return used;
}

void IlanaSynthAudioProcessor::clearModSlot (int slotIndex)
{
    setModSlotValue (slotIndex, "src", 0.0f);
    setModSlotValue (slotIndex, "dst", 0.0f);
    setModSlotValue (slotIndex, "amt", 0.0f);
    setModSlotValue (slotIndex, "curve", 0.0f);
    setModSlotValue (slotIndex, "pol", 0.0f);
    setModSlotValue (slotIndex, "aux", 0.0f);
    setModSlotValue (slotIndex, "byp", 0.0f);
    setModSlotValue (slotIndex, "stereo", 0.0f);
    resetModRemap (slotIndex);
}

bool IlanaSynthAudioProcessor::clearModSlotsForTarget (int destination)
{
    auto cleared = false;

    for (int i = 0; i < Mod::maxSlots; ++i)
    {
        if (readModSlot (i).destination != destination)
            continue;

        clearModSlot (i);
        cleared = true;
    }

    return cleared;
}

// M8.5: a macro's value with Evolve's drift.
float IlanaSynthAudioProcessor::macroValue (int macro) const
{
    const auto index = juce::jlimit (0, Mod::numMacros - 1, macro);
    return juce::jlimit (0.0f, 1.0f, getParam (macroIds[(size_t) index]) + macroDrift[(size_t) index].load());
}

std::array<float, 4> IlanaSynthAudioProcessor::vectorWeights (float x, float y)
{
    // Equal power: A top left, B top right, C bottom left, D bottom right.
    x = juce::jlimit (0.0f, 1.0f, x);
    y = juce::jlimit (0.0f, 1.0f, y);
    return { std::sqrt ((1.0f - x) * y), std::sqrt (x * y), std::sqrt ((1.0f - x) * (1.0f - y)), std::sqrt (x * (1.0f - y)) };
}

int IlanaSynthAudioProcessor::getVectorCorner (int corner) const
{
    const char* ids[] { "vec_a", "vec_b", "vec_c", "vec_d" };
    return (int) getParam (ids[juce::jlimit (0, 3, corner)]);
}

bool IlanaSynthAudioProcessor::isVectorPathOn() const { return getParam ("vec_path") > 0.5f; }

juce::Point<float> IlanaSynthAudioProcessor::getVectorPathPoint (int point) const
{
    const auto index = juce::jlimit (0, numVectorPoints - 1, point);
    return { getParam (vectorPathIds[(size_t) index].first), getParam (vectorPathIds[(size_t) index].second) };
}

// The point `phase` (0..1) of the way round the closed path.
static juce::Point<float> pointOnPath (const std::array<juce::Point<float>, IlanaSynthAudioProcessor::numVectorPoints>& points, double phase)
{
    std::array<float, IlanaSynthAudioProcessor::numVectorPoints> lengths {};
    auto total = 0.0f;
    for (int i = 0; i < IlanaSynthAudioProcessor::numVectorPoints; ++i)
    {
        lengths[(size_t) i] = points[(size_t) i].getDistanceFrom (points[(size_t) ((i + 1) % IlanaSynthAudioProcessor::numVectorPoints)]);
        total += lengths[(size_t) i];
    }
    if (total <= 1.0e-6f)
        return points[0];
    auto remaining = (float) (phase - std::floor (phase)) * total;
    for (int i = 0; i < IlanaSynthAudioProcessor::numVectorPoints; ++i)
    {
        if (remaining <= lengths[(size_t) i] || i == IlanaSynthAudioProcessor::numVectorPoints - 1)
        {
            const auto t = lengths[(size_t) i] > 0.0f ? remaining / lengths[(size_t) i] : 0.0f;
            return points[(size_t) i] + (points[(size_t) ((i + 1) % IlanaSynthAudioProcessor::numVectorPoints)] - points[(size_t) i]) * juce::jlimit (0.0f, 1.0f, t);
        }
        remaining -= lengths[(size_t) i];
    }
    return points[0];
}

void IlanaSynthAudioProcessor::updateEvolveAndVector (int numSamples)
{
    const auto seconds = (double) numSamples / juce::jmax (1.0, baseSampleRate);

    // Evolve.
    std::array<float, Mod::numMacros> amounts {}, rates {};
    for (int m = 0; m < Mod::numMacros; ++m)
    {
        amounts[(size_t) m] = getParam (evolveIds[(size_t) m].first);
        rates[(size_t) m] = getParam (evolveIds[(size_t) m].second);
    }
    evolve.advance (seconds, amounts, rates);
    for (int m = 0; m < Mod::numMacros; ++m)
        macroDrift[(size_t) m].store (evolve.offset (m));

    // The vector pad: hand position, or the path, then drift.
    vectorGains.fill (1.0f);
    if (getParam ("vec_on") < 0.5f)
    {
        vectorX.store (getParam ("vec_x"));
        vectorY.store (getParam ("vec_y"));
        return;
    }
    auto position = juce::Point<float> (getParam ("vec_x"), getParam ("vec_y"));
    if (isVectorPathOn())
    {
        vectorPathPhase = std::fmod (vectorPathPhase + seconds * (double) getParam ("vec_rate"), 1.0);
        std::array<juce::Point<float>, numVectorPoints> points;
        for (int i = 0; i < numVectorPoints; ++i)
            points[(size_t) i] = getVectorPathPoint (i);
        // The hand position offsets the path from its centre.
        position = pointOnPath (points, vectorPathPhase) + (position - juce::Point<float> (0.5f, 0.5f));
    }
    const auto drift = getParam ("vec_drift");
    if (drift > 0.0f)
    {
        const auto rate = getParam ("vec_drift_rate");
        vectorDrift.advance (seconds, { drift * 0.5f, drift * 0.5f, 0.0f, 0.0f }, { rate, rate * 1.37f, 0.1f, 0.1f });
        position += { vectorDrift.offset (0), vectorDrift.offset (1) };
    }
    position = { juce::jlimit (0.0f, 1.0f, position.x), juce::jlimit (0.0f, 1.0f, position.y) };
    vectorX.store (position.x);
    vectorY.store (position.y);

    const auto weights = vectorWeights (position.x, position.y);
    std::array<float, OscillatorIds::count> power {};
    std::array<bool, OscillatorIds::count> used {};
    for (int c = 0; c < 4; ++c)
    {
        const auto osc = juce::jlimit (0, OscillatorIds::count - 1, getVectorCorner (c));
        power[(size_t) osc] += weights[(size_t) c] * weights[(size_t) c];
        used[(size_t) osc] = true;
    }
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
        if (used[(size_t) osc])
            vectorGains[(size_t) osc] = std::sqrt (power[(size_t) osc]);
}

void IlanaSynthAudioProcessor::freezeEvolve()
{
    for (int m = 0; m < Mod::numMacros; ++m)
    {
        const auto frozen = macroValue (m);
        if (auto* parameter = apvts.getParameter ("macro" + juce::String (m + 1)))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (frozen));
        if (auto* amount = apvts.getParameter ("macro" + juce::String (m + 1) + "_evolve"))
            amount->setValueNotifyingHost (0.0f);
        macroDrift[(size_t) m].store (0.0f);
    }
    evolve.reset();
}

LfoSimSettings IlanaSynthAudioProcessor::readLfoSimSettings (int lfo) const
{
    const auto& ids = lfoIds[(size_t) juce::jlimit (0, numLfos - 1, lfo)];
    LfoSimSettings settings;
    settings.shape = (int) getParam (ids.shape);
    if (! LfoSimShapes::isSim (settings.shape))
        return settings;
    for (int param = 0; param < LfoSimInfo::numParams; ++param)
        settings.p[(size_t) param] = getParam (ids.sim[(size_t) param]);
    settings.axis = (int) getParam (ids.axis);
    settings.loop = getParam (ids.loop) > 0.5f;
    settings.seed = (int) getParam (ids.seed);
    settings.stereo = getParam (ids.stereo);
    return settings;
}

void IlanaSynthAudioProcessor::renderLfos (int numSamples, const juce::MidiBuffer& midiMessages)
{
    if (lfoBuffers.getNumSamples() < numSamples)
        lfoBuffers.setSize (numLfoChannels, numSamples, false, false, true);

    // The host's tempo and beat, checked: a bad BPM (0, negative, NaN) keeps
    // the last good one (120 at first), and no playhead means not playing.
    auto positionKnown = false;

    if (auto* transport = getPlayHead())
    {
        if (const auto position = transport->getPosition())
        {
            positionKnown = true;

            if (const auto bpm = position->getBpm())
                if (std::isfinite (*bpm) && *bpm > 0.0)
                    currentBpm = juce::jlimit (20.0, 999.0, *bpm);

            const auto ppq = position->getPpqPosition();
            const auto ppqGood = ppq.hasValue() && std::isfinite (*ppq);
            hostPlaying = position->getIsPlaying() && ppqGood;

            if (ppqGood)
                hostPpq = *ppq;
        }
    }

    if (! positionKnown)
        hostPlaying = false;

    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();

        if (message.isController())
        {
            const auto controllerNumber = message.getControllerNumber();
            const auto controllerValue = (float) message.getControllerValue() / 127.0f;

            if (controllerNumber == 1)
                modWheelValue = controllerValue;
            else if (controllerNumber == 11)
                expressionValue = controllerValue;
            else if (controllerNumber == 64)
                pedalChannelDown[(size_t) juce::jlimit (0, 15, message.getChannel() - 1)] = message.getControllerValue() >= 64;
            else if (message.isAllNotesOff() || message.isAllSoundOff())
            {
                msegKeyDown.fill (false);
                msegKeysDown = 0;
            }

            const auto learnTarget = macroLearn.load();

            if (learnTarget >= 0)
            {
                macroCc[juce::jlimit (0, Mod::numMacros - 1, learnTarget)].store (controllerNumber);
                macroLearn.store (-1);
                triggerAsyncUpdate();
            }

            for (int macro = 0; macro < Mod::numMacros; ++macro)
            {
                if (macroCc[macro].load() == controllerNumber)
                {
                    pendingMacros[macro].store (controllerValue);
                    macroPendingFlags[macro].store (true);
                    macrosPending.store (true);
                    triggerAsyncUpdate();
                }
            }

            // Any other parameter's learned CC (one CC per parameter).
            if (juce::isPositiveAndBelow (controllerNumber, (int) ccParameter.size()))
            {
                if (const auto learning = paramLearn.exchange (-1); learning >= 0)
                {
                    for (auto& mapped : ccParameter)
                        if (mapped.load() == learning)
                            mapped.store (-1);

                    ccParameter[(size_t) controllerNumber].store (learning);
                }

                if (ccParameter[(size_t) controllerNumber].load() >= 0)
                {
                    pendingCcValues[(size_t) controllerNumber].store (controllerValue);
                    ccValuePending[(size_t) controllerNumber].store (true);
                    ccPending.store (true);
                    triggerAsyncUpdate();
                }
            }
        }
        else if (message.isChannelPressure())
        {
            aftertouchValue = synth.getMaxChannelPressure (message.getChannel(), (float) message.getChannelPressureValue() / 127.0f);
        }
        else if (message.isNoteOn())
        {
            // A one-shot MSEG starts again with the first key of a phrase.
            const auto note = message.getNoteNumber();

            if (! msegKeyDown[(size_t) note])
            {
                msegKeyDown[(size_t) note] = true;

                if (msegKeysDown++ == 0 && getParam (msegLoopRef) <= 0.5f)
                    mseg.reset();
            }
        }
        else if (message.isNoteOff())
        {
            const auto note = message.getNoteNumber();

            if (msegKeyDown[(size_t) note])
            {
                msegKeyDown[(size_t) note] = false;
                --msegKeysDown;
            }
        }
        else if (message.isProgramChange() && getParam (midiProgramChangeRef) > 0.5f)
        {
            pendingProgramChange.store (message.getProgramChangeNumber());
            triggerAsyncUpdate();
        }
    }

    bool retriggers[numLfos] {};

    for (int lfo = 0; lfo < numLfos; ++lfo)
        retriggers[lfo] = getParam (lfoIds[(size_t) lfo].retrig) > 0.5f;

    for (const auto metadata : midiMessages)
    {
        if (! metadata.getMessage().isNoteOn())
            continue;

        for (int lfo = 0; lfo < numLfos; ++lfo)
        {
            const auto& ids = lfoIds[(size_t) lfo];
            const auto shape = (int) getParam (ids.shape);
            if (retriggers[lfo])
            {
                lfoPhases[(size_t) lfo] = 0.0;
                lfoChaos[(size_t) lfo].resetPhysics (shape, getParam (ids.physA));
                lfoPreviousShapes[(size_t) lfo] = shape;
            }
            if (shape == LfoShapes::Pendulum && getParam (ids.kick) > 0.5f)
            {
                if (lfoPreviousShapes[(size_t) lfo] != shape)
                {
                    lfoChaos[(size_t) lfo].resetPhysics (shape, getParam (ids.physA));
                    lfoPreviousShapes[(size_t) lfo] = shape;
                }
                lfoChaos[(size_t) lfo].kick (metadata.getMessage().getFloatVelocity());
            }
        }
    }

    // M8.1: the simulated shapes' settings and their triggers in this block,
    // as sample offsets (MIDI offsets are at the base rate).
    const auto oversampling = juce::jmax (1, juce::roundToInt (currentSampleRate / juce::jmax (1.0, baseSampleRate)));
    LfoSimSettings simSettings[numLfos];
    bool simShape[numLfos] {};
    int simTriggers[numLfos][8] {};
    int numSimTriggers[numLfos] {};
    float smoothCoefficients[numLfos] {};
    const auto addTrigger = [&] (int lfo, int offset)
    {
        if (numSimTriggers[lfo] < 8)
            simTriggers[lfo][numSimTriggers[lfo]++] = juce::jlimit (0, juce::jmax (0, numSamples - 1), offset);
    };

    // Steps of a beat grid in this block: calls hit (offset, step number).
    const auto forEachGridStep = [this, numSamples] (double beats, double& freePhase, long long& last, auto&& hit)
    {
        const auto perSample = (juce::jmax (20.0, currentBpm.load()) / 60.0) / juce::jmax (0.001, beats) / currentSampleRate;
        const auto start = hostPlaying.load() ? hostPpq.load() / beats : freePhase;
        if ((double) last > start + 1.0)
            last = (long long) std::floor (start) - 1;
        const auto end = start + (double) numSamples * perSample;
        for (auto k = (long long) std::ceil (start - 1.0e-9); (double) k < end; ++k)
        {
            if (k <= last)
                continue;
            last = k;
            hit ((int) std::ceil (((double) k - start) / perSample), k);
        }
        // (Not wrapped: a wrap that isn't a multiple of the pattern's length
        // jumps the pattern; a double keeps its precision for ages.)
        freePhase = end;
    };

    // The Generative steps: Euclid's hits, else the probability sequencer's
    // steps. Shared by every LFO set to Generative.
    int generativeSteps[16] {};
    int numGenerativeSteps = 0;
    {
        auto wanted = false;
        for (int lfo = 0; lfo < numLfos; ++lfo)
            wanted = wanted || (LfoSimShapes::isSim ((int) getParam (lfoIds[(size_t) lfo].shape))
                                && (int) getParam (lfoIds[(size_t) lfo].trigger) == 3);
        const auto euclid = getParam ("euc_on") > 0.5f;
        if (wanted && (euclid || getParam ("pseq_on") > 0.5f))
        {
            const auto beats = getSyncDivisionBeats ((int) getParam (euclid ? "euc_div" : "pseq_div"));
            const auto steps = juce::jlimit (2, 32, (int) getParam ("euc_steps"));
            const auto hits = juce::jlimit (0, 32, (int) getParam ("euc_hits"));
            const auto rotate = juce::jlimit (0, 31, (int) getParam ("euc_rotate"));
            forEachGridStep (beats, lfoGenerativePhase, lfoGenerativeLast, [&] (int offset, long long k)
            {
                if ((! euclid || euclidHit ((int) (((k % steps) + steps) % steps), hits, steps, rotate)) && numGenerativeSteps < 16)
                    generativeSteps[numGenerativeSteps++] = offset;
            });
        }
        else
        {
            lfoGenerativeLast = -1;
        }
    }

    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        simSettings[lfo] = readLfoSimSettings (lfo);
        simShape[lfo] = LfoSimShapes::isSim (simSettings[lfo].shape);
        const auto& ids = lfoIds[(size_t) lfo];

        const auto fire = getParam (ids.fire) > 0.5f;
        if (fire && ! lfoFireWas[(size_t) lfo])
        {
            addTrigger (lfo, 0);
            ++lfoTriggerCounts[(size_t) lfo];
        }
        lfoFireWas[(size_t) lfo] = fire;

        if (! simShape[lfo])
            continue;

        const auto mode = (int) getParam (ids.trigger);
        if (mode == 0)
        {
            for (const auto metadata : midiMessages)
                if (metadata.getMessage().isNoteOn())
                {
                    addTrigger (lfo, metadata.samplePosition * oversampling);
                    if (simSettings[lfo].shape == LfoSimShapes::Pendulum && getParam (ids.kick) > 0.5f)
                        lfoSims[(size_t) lfo].trigger (simSettings[lfo], lfoSimSeedCounter++, true,
                                                       metadata.getMessage().getFloatVelocity());
                }
        }
        else if (mode == 2)
        {
            forEachGridStep (getSyncDivisionBeats ((int) getParam (ids.div)), lfoBeatPhase[(size_t) lfo], lfoBeatLast[(size_t) lfo],
                             [&] (int offset, long long) { addTrigger (lfo, offset); ++lfoTriggerCounts[(size_t) lfo]; });
        }
        else if (mode == 3)
        {
            for (int step = 0; step < numGenerativeSteps; ++step)
            {
                addTrigger (lfo, generativeSteps[step]);
                ++lfoTriggerCounts[(size_t) lfo];
            }
        }
        std::sort (simTriggers[lfo], simTriggers[lfo] + numSimTriggers[lfo]);
    }

    const auto makeRate = [this] (const ParamRef& syncId, const ParamRef& rateId, const ParamRef& divId,
                                  Mod::Destination rateDestination)
    {
        auto rate = getParam (rateId);

        if (getParam (syncId) > 0.5f)
        {
            const auto beats = getSyncDivisionBeats ((int) getParam (divId));
            rate = (float) ((currentBpm.load() / 60.0) / beats);
        }

        // Rate modulation from the previous block's synth-wide evaluation.
        const auto rateMod = modDisplayValues[(size_t) rateDestination].load();
        return juce::jlimit (0.001f, 200.0f, rate * std::exp2 (rateMod * Mod::Range::lfoRateOctaves));
    };

    float lfoRates[numLfos] {};
    float lfoShapes[numLfos] {};
    float lfoPhysA[numLfos] {}, lfoPhysB[numLfos] {};
    float lfoSteps[numLfos][16] {};
    double lfoIncrements[numLfos] {};

    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        const auto& ids = lfoIds[(size_t) lfo];

        lfoRates[lfo] = makeRate (ids.sync, ids.rate, ids.div,
                                  Mod::lfoRateDestinationFor (lfo));
        lfoShapes[lfo] = (float) (int) getParam (ids.shape);
        lfoPhysA[lfo] = getParam (ids.physA);
        lfoPhysB[lfo] = getParam (ids.physB);
        if (lfoPreviousShapes[(size_t) lfo] != (int) lfoShapes[lfo])
        {
            lfoPreviousShapes[(size_t) lfo] = (int) lfoShapes[lfo];
            if (LfoShapes::isPhysics ((int) lfoShapes[lfo]))
                lfoChaos[(size_t) lfo].resetPhysics ((int) lfoShapes[lfo], lfoPhysA[lfo]);
        }
        lfoIncrements[lfo] = (double) lfoRates[lfo] / currentSampleRate;
        smoothCoefficients[lfo] = LfoSmoother::coefficientFor (getParam (ids.smooth), (double) lfoRates[lfo], currentSampleRate);
        lfoSims[(size_t) lfo].sampleRate = currentSampleRate;

        // (Only the Steps shape reads them: 16 reads per LFO per block otherwise.)
        if ((int) lfoShapes[lfo] == 7)
            for (int step = 0; step < 16; ++step)
                lfoSteps[lfo][step] = getParam (ids.steps[(size_t) step]);
    }

    float* lfoBufferPointers[numLfos] {};

    float* lfoBufferPointersB[numLfos] {};

    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        lfoBufferPointers[lfo] = lfoBuffers.getWritePointer (lfoChannel (lfo));
        lfoBufferPointersB[lfo] = lfoBuffers.getWritePointer (lfoChannelB (lfo));
        juce::FloatVectorOperations::clear (lfoBufferPointersB[lfo], numSamples);
    }
    int nextSimTrigger[numLfos] {};

    // LFO 1-4 always render, as before the pool. LFO 5-16 render only when a
    // mod slot uses them; otherwise their phase just moves on for the cards.
    bool renderLfo[numLfos] {};
    for (int lfo = 0; lfo < numLfos; ++lfo)
        renderLfo[lfo] = simShape[lfo] ? lfoRouted[(size_t) lfo] || lfoRoutedB[(size_t) lfo]
                                       : lfo < 4 || lfoRouted[(size_t) lfo] || lfoRoutedB[(size_t) lfo];
    // The ones that render, in order: the per-sample loop walks only these.
    int renderList[numLfos] {};
    auto numRendering = 0;
    for (int lfo = 0; lfo < numLfos; ++lfo)
        if (renderLfo[lfo])
            renderList[numRendering++] = lfo;

    auto* clockBuffer = lfoBuffers.getWritePointer (4);
    auto* msegBuffer = lfoBuffers.getWritePointer (5);
    static_assert (lfoChannel (3) == 3 && lfoChannel (4) == 6, "clock and MSEG keep channels 4 and 5");

    const auto clockBeats = getSyncDivisionBeats ((int) getParam ("clock_div"));
    const auto clockRate = (currentBpm.load() / 60.0) / juce::jmax (0.001, clockBeats);
    const auto clockIncrement = clockRate / currentSampleRate;

    {
        const float levels[4] { getParam ("mseg_level1"), getParam ("mseg_level2"),
                                getParam ("mseg_level3"), getParam ("mseg_level4") };
        const float times[4] { getParam ("mseg_time1"), getParam ("mseg_time2"),
                               getParam ("mseg_time3"), getParam ("mseg_time4") };

        const auto rateMod = modDisplayValues[(size_t) Mod::Destination::MsegRate].load();
        mseg.setParams (levels, times, getParam ("mseg_rate") * std::exp2 (rateMod * Mod::Range::msegRateOctaves), getParam ("mseg_loop") > 0.5f);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        for (int index = 0; index < numRendering; ++index)
        {
            const auto lfo = renderList[index];
            const auto phase = lfoPhases[(size_t) lfo];
            const auto stepIndex = juce::jlimit (0, 15, (int) (phase * 16.0));
            const auto shape = (int) lfoShapes[lfo];

            if (simShape[lfo])
            {
                auto& sim = lfoSims[(size_t) lfo];
                while (nextSimTrigger[lfo] < numSimTriggers[lfo] && simTriggers[lfo][nextSimTrigger[lfo]] <= i)
                {
                    sim.trigger (simSettings[lfo], lfoSimSeedCounter++);
                    ++nextSimTrigger[lfo];
                }
                float a = 0.0f, b = 0.0f;
                sim.next (simSettings[lfo], lfoIncrements[lfo], a, b);
                if (smoothCoefficients[lfo] < 1.0f)
                    lfoSmoothers[(size_t) lfo].process (a, b, smoothCoefficients[lfo]);
                lfoBufferPointers[lfo][i] = a;
                lfoBufferPointersB[lfo][i] = b;
                auto next = phase + lfoIncrements[lfo];
                lfoPhases[(size_t) lfo] = next - std::floor (next);
                continue;
            }

            auto& chaos = lfoChaos[(size_t) lfo];
            const auto stateful = LfoShapes::isStateful (shape);

            if (shape == LfoShapes::Chaos)
                chaos.advance (lfoIncrements[lfo]);
            else if (LfoShapes::isPhysics (shape))
                chaos.advancePhysics (shape, lfoIncrements[lfo], lfoPhysA[lfo], lfoPhysB[lfo]);

            // LFO 1-4 always run (they share the clock's random generator,
            // so skipping their draws would shift it), but an unrouted one's
            // wave is only needed for the card's last value.
            if (lfoRouted[(size_t) lfo] || stateful || shape == 7 || i == numSamples - 1)
                lfoBufferPointers[lfo][i] = shape == 7
                                                ? lfoSteps[lfo][stepIndex]
                                                : (stateful ? chaos.value (shape, phase)
                                                            : lfoValue (shape, phase, lfoSampleHolds[(size_t) lfo].load(),
                                                                        activeLfoCustom[(size_t) lfo].data(),
                                                                        activeLfoCurveTables[(size_t) lfo].data()));
            else
                lfoBufferPointers[lfo][i] = 0.0f;

            // M8.1: SMOOTH and output B (a quarter cycle on for the
            // periodic shapes). Both off leave the classic path untouched.
            if (smoothCoefficients[lfo] < 1.0f || lfoRoutedB[(size_t) lfo])
            {
                auto a = lfoBufferPointers[lfo][i];
                auto b = a;
                if (lfoRoutedB[(size_t) lfo] && ! stateful && shape != 5)
                {
                    const auto quarter = phase + 0.25 - std::floor (phase + 0.25);
                    b = shape == 7 ? lfoSteps[lfo][juce::jlimit (0, 15, (int) (quarter * 16.0))]
                                   : lfoValue (shape, quarter, lfoSampleHolds[(size_t) lfo].load(),
                                               activeLfoCustom[(size_t) lfo].data(), activeLfoCurveTables[(size_t) lfo].data());
                }
                if (smoothCoefficients[lfo] < 1.0f)
                    lfoSmoothers[(size_t) lfo].process (a, b, smoothCoefficients[lfo]);
                lfoBufferPointers[lfo][i] = a;
                lfoBufferPointersB[lfo][i] = b;
            }

            auto nextPhase = phase + lfoIncrements[lfo];

            if (nextPhase >= 1.0)
            {
                nextPhase -= std::floor (nextPhase);
                lfoSampleHolds[(size_t) lfo].store (randomForLfo (lfo).nextFloat() * 2.0f - 1.0f);

                if (stateful && ! LfoShapes::isPhysics (shape))
                    chaos.onCycle (shape, randomForLfo (lfo));
            }

            lfoPhases[(size_t) lfo] = nextPhase;
        }

        clockBuffer[i] = clockShValue;
        msegBuffer[i] = mseg.getNextValue();

        clockShPhase += clockIncrement;

        if (clockShPhase >= 1.0)
        {
            clockShPhase -= std::floor (clockShPhase);
            clockShValue = lfoRandom.nextFloat() * 2.0f - 1.0f;
        }
    }

    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        if (renderLfo[lfo])
        {
            if (numSamples > 0)
            {
                lfoLastValues[(size_t) lfo].store (lfoBufferPointers[lfo][numSamples - 1]);
                lfoLastValuesB[(size_t) lfo].store (lfoBufferPointersB[lfo][numSamples - 1]);
            }
            continue;
        }

        // Unrouted: silence for the voices, the phase moves on for the cards.
        juce::FloatVectorOperations::clear (lfoBufferPointers[lfo], numSamples);
        auto phase = lfoPhases[(size_t) lfo] + lfoIncrements[lfo] * (double) numSamples;
        phase -= std::floor (phase);
        lfoPhases[(size_t) lfo] = phase;
        const auto shape = (int) lfoShapes[lfo];
        lfoLastValuesB[(size_t) lfo].store (0.0f);
        lfoLastValues[(size_t) lfo].store (LfoShapes::isStateful (shape) || shape == 7 || LfoSimShapes::isSim (shape)
                                               ? 0.0f
                                               : lfoValue (shape, phase, lfoSampleHolds[(size_t) lfo].load(),
                                                           activeLfoCustom[(size_t) lfo].data(),
                                                           activeLfoCurveTables[(size_t) lfo].data()));
    }

    modWheelDisplay.store (modWheelValue);
    aftertouchDisplay.store (aftertouchValue);
    expressionDisplay.store (expressionValue);
    clockShDisplay.store (clockShValue);
    msegDisplay.store (numSamples > 0 ? msegBuffer[numSamples - 1] : 0.0f);
    msegPhaseDisplay.store ((float) mseg.getPhase());
}

// Pedal resonance, the soundboard and mechanical noises: after the voices,
// at the base rate. Each part costs nothing while it is off.
void IlanaSynthAudioProcessor::processAcousticKeys (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi)
{
    const auto keyNoise = getParam ("mech_key");
    const auto damperNoise = getParam ("mech_damper");
    const auto pedalNoise = getParam ("mech_pedal");
    const auto pedalAmount = getParam ("pedal_res");
    using Kind = MechanicalNoise::Kind;

    for (const auto metadata : midi)
    {
        const auto& message = metadata.getMessage();
        const auto offset = metadata.samplePosition;

        if (message.isNoteOff())
        {
            const auto note = message.getNoteNumber();
            mechanicalNoise.trigger (Kind::KeyRelease, note, keyNoise, offset);

            // With the pedal down the damper stays up until the pedal lifts.
            if (keysPedalDown)
                pedalHeldNotes[(size_t) note] = true;
            else
                mechanicalNoise.trigger (Kind::Damper, note, damperNoise, offset);
        }
        else if (message.isNoteOn())
        {
            pedalHeldNotes[(size_t) message.getNoteNumber()] = false;
        }
        else if (message.isSustainPedalOn() && ! keysPedalDown)
        {
            keysPedalDown = true;
            mechanicalNoise.trigger (Kind::PedalDown, -1, pedalNoise, offset);
            pedalResonance.setPedal (true, pedalAmount);
        }
        else if (message.isSustainPedalOff() && keysPedalDown)
        {
            keysPedalDown = false;
            mechanicalNoise.trigger (Kind::PedalUp, -1, pedalNoise, offset);
            pedalResonance.setPedal (false, pedalAmount);

            // Every damper that was held up lands at once.
            auto landed = 0;
            for (int note = 0; note < 128; ++note)
                if (std::exchange (pedalHeldNotes[(size_t) note], false) && landed < 6)
                {
                    mechanicalNoise.trigger (Kind::Damper, note, damperNoise * 0.6f, offset + landed * 24);
                    ++landed;
                }
        }
    }

    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    const auto numSamples = buffer.getNumSamples();

    pedalResonance.setStretch (getParam ("stretch"));
    if (pedalAmount > 0.0f && pedalResonance.isRinging())
        pedalResonance.process (left, right, numSamples, pedalAmount);

    const auto soundboardOn = getParam ("sb_on") > 0.5f;
    if (soundboardOn)
    {
        if (! soundboardWasOn)
        {
            soundboard.reset();
            denseSoundboard.reset();
        }
        if ((int) getParam ("sb_model") == 1)
            denseSoundboard.process (left, right, numSamples, getParam ("sb_mix"), getParam ("sb_tone"), getParam ("sb_size"));
        else
            soundboard.process (left, right, numSamples, getParam ("sb_mix"), getParam ("sb_tone"), getParam ("sb_size"));
    }
    soundboardWasOn = soundboardOn;

    mechanicalNoise.process (left, right, numSamples);
}

float IlanaSynthAudioProcessor::globalSourceValue (Mod::Source source) const
{
    if (const auto lfoIndex = Mod::lfoBIndexFor (source); lfoIndex >= 0)
        return lfoLastValuesB[(size_t) lfoIndex].load();
    if (source >= Mod::Source::Env6 && source <= Mod::Source::Env16)
        return getEnvMonitorExtra ((int) source - (int) Mod::Source::Env6);
    if (const auto lfoIndex = Mod::lfoIndexFor (source); lfoIndex >= 0)
    {
        const auto& lfoId = lfoIds[(size_t) lfoIndex];
        const auto shape = (int) getParam (lfoId.shape);
        const auto phase = lfoPhases[(size_t) lfoIndex];

        if (LfoSimShapes::isSim (shape) || getParam (lfoId.smooth) > 0.0f)
            return lfoLastValues[(size_t) lfoIndex].load();

        if (LfoShapes::isStateful (shape))
            return lfoChaos[(size_t) lfoIndex].value (shape, phase);

        return shape == 7 ? getParam (lfoId.steps[(size_t) juce::jlimit (0, 15, (int) (phase * 16.0))])
                          : lfoValue (shape, phase, lfoSampleHolds[(size_t) lfoIndex].load(),
                                      activeLfoCustom[(size_t) lfoIndex].data(),
                                      activeLfoCurveTables[(size_t) lfoIndex].data());
    }

    switch (source)
    {
        case Mod::Source::AmpEnv:     return envMonitorAmp.load();
        case Mod::Source::FilterEnv:  return envMonitorFilter.load();
        case Mod::Source::FilterEnv2: return envMonitorFilter2.load();
        case Mod::Source::ModEnv:     return envMonitorMod.load();
        case Mod::Source::Env4:       return envMonitorEnv4.load();
        case Mod::Source::Velocity:   return monitorVelocity.load();
        case Mod::Source::KeyTrack:   return monitorKeyTrack.load();
        case Mod::Source::Random:     return monitorRandom.load();
        case Mod::Source::ClockSh:    return clockShValue;
        case Mod::Source::Mseg:       return lfoBuffers.getNumSamples() > 0 ? lfoBuffers.getSample (5, 0) : 0.0f;
        case Mod::Source::OpLfo:      return monitorOpLfo.load();
        case Mod::Source::OpPitchEnv: return monitorOpPitch.load();
        default:                      return staticSourceValue ((int) source);
    }
}

void IlanaSynthAudioProcessor::evaluateGlobalModulation (const Mod::Slot* slots, int numSlots)
{
    float totals[maxDestinations] {};

    // Param destinations are applied inside getParam(), so clear them first
    // or the source values read below would include last block's offsets.
    anyParamModulation = false;
    std::fill (paramDestinationOffsets.begin(), paramDestinationOffsets.end(), 0.0f);
    stringStereoOffsets = {};
    stringStereoRouted = {};

    for (int i = 0; i < numSlots; ++i)
    {
        const auto& slot = slots[i];

        if (! juce::isPositiveAndBelow (slot.destination, maxDestinations))
            continue;

        // A spectral amount fed by a per-note source is worked out by each voice.
        if (Mod::isVoiceSpectralSlot (slot))
            continue;

        auto value = Mod::shape (slot, globalSourceValue (slot.source));

        if (slot.aux != Mod::Source::None)
            value *= Mod::auxScale (slot.aux, globalSourceValue (slot.aux));

        // A stereo route into a string knob moves the sides apart, not the knob.
        if (slot.stereo)
            if (const auto target = Mod::StereoString::targetFor (slot.destination); target.osc >= 0)
            {
                stringStereoOffsets[(size_t) target.osc][(size_t) target.field] += slot.depth * value;
                stringStereoRouted[(size_t) target.osc] = stringStereoRouted[(size_t) target.osc] || slot.depth != 0.0f;
                continue;
            }

        totals[slot.destination] += slot.depth * value;
    }

    // (Relaxed: the editor only shows these; a sequentially consistent
    // store is a locked exchange, and this was 1024 of them every block.)
    for (int d = 0; d < maxDestinations; ++d)
        modDisplayValues[(size_t) d].store (totals[d], std::memory_order_relaxed);

    // Which parameter destinations take the synth-wide offset, and their
    // signs, worked out once: the OSC 4-6 FM cells are modulated per voice
    // instead, and the Operator Env's stage rates are times on their knobs
    // (review 7, I7-3): a positive amount lengthens the stage, so it lowers
    // the DX7 rate the parameter stores. Every rate: ATTACK, DECAY 1 / 2 and
    // RELEASE of each operator and of OP PITCH.
    struct GlobalTarget
    {
        int index, destination;
        float sign;
    };
    static const auto globalParamDestinations = []
    {
        std::vector<GlobalTarget> list;
        const auto& entries = Mod::getParamDestinations();
        for (int i = 0; i < (int) entries.size(); ++i)
        {
            if (Mod::extendedFmCellFor (Mod::paramDestinationFor (i)) >= 0)
                continue;
            const juce::String id (entries[(size_t) i].id);
            const auto reversed = id.endsWith ("_eg_r1") || id.endsWith ("_eg_r2") || id.endsWith ("_eg_r3") || id.endsWith ("_eg_r4")
                                  || id.startsWith ("opeg_pitch_r");
            list.push_back ({ i, Mod::paramDestinationFor (i), reversed ? -1.0f : 1.0f });
        }
        return list;
    }();

    for (const auto& entry : globalParamDestinations)
    {
        const auto i = entry.index;
        if (i >= (int) paramDestinations.size())
            continue;
        const auto offset = totals[entry.destination] * entry.sign;

        if (offset != 0.0f && paramDestinations[(size_t) i].parameter != nullptr)
        {
            paramDestinationOffsets[(size_t) i] = offset;
            anyParamModulation = true;
        }
    }
}

float IlanaSynthAudioProcessor::getArpStepRateHz() const
{
    const auto beats = getSyncDivisionBeats ((int) getParam ("arp_div"));

    return (float) ((currentBpm.load() / 60.0) / juce::jmax (0.001, beats));
}

float IlanaSynthAudioProcessor::getSourceDisplayValue (int sourceIndex) const
{
    if (const auto lfoIndex = Mod::lfoBIndexFor ((Mod::Source) sourceIndex); lfoIndex >= 0)
        return lfoLastValuesB[(size_t) lfoIndex].load();
    const auto source = (Mod::Source) juce::jlimit (0, (int) Mod::Source::Count - 1, sourceIndex);
    if (source >= Mod::Source::Env6 && source <= Mod::Source::Env16)
        return getEnvMonitorExtra ((int) source - (int) Mod::Source::Env6);

    if (const auto lfoIndex = Mod::lfoIndexFor (source); lfoIndex >= 0)
    {
        const auto prefix = "lfo" + juce::String (lfoIndex + 1);
        const auto shape = (int) getParam ((prefix + "_shape").toRawUTF8());
        const auto phase = (double) lfoPhaseDisplays[(size_t) lfoIndex].load();

        if (LfoShapes::isStateful (shape) || LfoSimShapes::isSim (shape))
            return lfoLastValues[(size_t) lfoIndex].load();

        if (shape == 7)
            return getParam ((prefix + "_step"
                              + juce::String (juce::jlimit (0, 15, (int) (phase * 16.0)) + 1)).toRawUTF8());

        std::array<float, lfoDrawSteps> custom {};
        std::array<float, LfoCurve::tableSize> curve {};

        {
            const juce::SpinLock::ScopedLockType lock (lfoShapeLock);
            custom = lfoCustom[(size_t) lfoIndex];
            curve = lfoCurveTables[(size_t) lfoIndex];
        }

        return lfoValue (shape, phase, lfoSampleHolds[(size_t) lfoIndex].load(), custom.data(), curve.data());
    }

    switch (source)
    {
        case Mod::Source::Macro1:     return macroValue (0);
        case Mod::Source::Macro2:     return macroValue (1);
        case Mod::Source::Macro3:     return macroValue (2);
        case Mod::Source::Macro4:     return macroValue (3);
        case Mod::Source::Macro5:     return macroValue (4);
        case Mod::Source::Macro6:     return macroValue (5);
        case Mod::Source::Macro7:     return macroValue (6);
        case Mod::Source::Macro8:     return macroValue (7);
        case Mod::Source::VectorX:    return vectorX.load();
        case Mod::Source::VectorY:    return vectorY.load();
        case Mod::Source::ModWheel:   return modWheelDisplay.load();
        case Mod::Source::Aftertouch: return aftertouchDisplay.load();
        case Mod::Source::Expression: return expressionDisplay.load();
        case Mod::Source::ClockSh:    return clockShDisplay.load();
        case Mod::Source::AmpEnv:     return envMonitorAmp.load();
        case Mod::Source::FilterEnv:  return envMonitorFilter.load();
        case Mod::Source::FilterEnv2: return envMonitorFilter2.load();
        case Mod::Source::ModEnv:     return envMonitorMod.load();
        case Mod::Source::Env4:       return envMonitorEnv4.load();
        case Mod::Source::Velocity:   return monitorVelocity.load();
        case Mod::Source::KeyTrack:   return monitorKeyTrack.load();
        case Mod::Source::Random:     return monitorRandom.load();
        case Mod::Source::Mseg:       return msegDisplay.load();
        case Mod::Source::InputEnv:   return inputEnvDisplay.load();
        case Mod::Source::OpLfo:      return monitorOpLfo.load();
        case Mod::Source::OpPitchEnv: return monitorOpPitch.load();
        default:                      return 0.0f;
    }
}

float IlanaSynthAudioProcessor::staticSourceValue (int sourceIndex) const
{
    switch ((Mod::Source) juce::jlimit (0, (int) Mod::Source::Count - 1, sourceIndex))
    {
        case Mod::Source::Macro1:     return macroValue (0);
        case Mod::Source::Macro2:     return macroValue (1);
        case Mod::Source::Macro3:     return macroValue (2);
        case Mod::Source::Macro4:     return macroValue (3);
        case Mod::Source::Macro5:     return macroValue (4);
        case Mod::Source::Macro6:     return macroValue (5);
        case Mod::Source::Macro7:     return macroValue (6);
        case Mod::Source::Macro8:     return macroValue (7);
        case Mod::Source::VectorX:    return vectorX.load();
        case Mod::Source::VectorY:    return vectorY.load();
        case Mod::Source::ModWheel:   return modWheelValue;
        case Mod::Source::Aftertouch: return aftertouchValue;
        case Mod::Source::Expression: return expressionValue;
        default:                      return 0.0f;
    }
}

// Review 6 (V5-8, S5-9): duplicate matrix rows. Both rows add
// depth * shape (source), so one row with the summed depth is the same
// routing, as long as everything else that shapes it matches.
bool IlanaSynthAudioProcessor::canMergeModSlots (int into, int from, juce::String* reason) const
{
    const auto fail = [reason] (const char* why)
    {
        if (reason != nullptr)
            *reason = why;
        return false;
    };

    if (into == from || ! juce::isPositiveAndBelow (into, Mod::maxSlots) || ! juce::isPositiveAndBelow (from, Mod::maxSlots))
        return fail ("not two rows");

    const auto a = readModSlot (into);
    const auto b = readModSlot (from);

    if (a.source != b.source || a.destination != b.destination || a.source == Mod::Source::None || a.destination == 0)
        return fail ("they route different things");
    if (a.aux != b.aux)
        return fail ("their via sources differ");
    if (a.polarity != b.polarity)
        return fail ("their polarities differ");
    if (a.bypass != b.bypass)
        return fail ("one is switched off");
    if (std::abs (a.curve - b.curve) > 1.0e-6f)
        return fail ("their curves differ");
    if (isModRemapOn (into) || isModRemapOn (from))
        return fail ("one has a drawn remap curve");
    if (std::abs (a.depth + b.depth) > 1.0f + 1.0e-6f)
        return fail ("together they would pass 100%");

    return true;
}

bool IlanaSynthAudioProcessor::mergeModSlots (int into, int from)
{
    if (! canMergeModSlots (into, from))
        return false;

    const auto depth = readModSlot (into).depth + readModSlot (from).depth;
    setModSlotValue (into, "amt", juce::jlimit (-1.0f, 1.0f, depth));
    clearModSlot (from);
    return true;
}

int IlanaSynthAudioProcessor::mergeDuplicateModSlots()
{
    auto merged = 0;

    for (int into = 0; into < Mod::maxSlots; ++into)
        for (int from = into + 1; from < Mod::maxSlots; ++from)
            if (mergeModSlots (into, from))
                ++merged;

    return merged;
}
