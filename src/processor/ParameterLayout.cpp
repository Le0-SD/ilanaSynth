#include "ProcessorInternal.h"

juce::AudioProcessorValueTreeState::ParameterLayout IlanaSynthAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Hosts show the oscillators as the pages do, "OSC 1 Level" (UI review
    // 8, I8-40); the names below keep the shorter "Osc1" spelling.
    const auto hostName = [] (juce::String name)
    {
        for (int osc = 1; osc <= 6; ++osc)
            name = name.replace ("Osc" + juce::String (osc), "OSC " + juce::String (osc));
        return name;
    };

    const auto addFloat = [&layout, hostName] (const juce::String& id, const juce::String& name, float min, float max,
                                     float def, float skew = 1.0f, float interval = 0.0f)
    {
        // Continuous unless a step is asked for: a fixed step of a thousandth
        // of the range is coarse at the dense end of a skewed range (cutoff
        // moved in 20 Hz steps, comb tunings sat cents off).
        const juce::NormalisableRange<float> range (min, max, juce::jmax (0.0f, interval), skew);
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, hostName (name), range, def,
            juce::AudioParameterFloatAttributes()
                .withStringFromValueFunction ([id] (float value, int) { return describeValue (id, value); })
                // Typed values ("97 %", "250 ms", "1.2 kHz"): the number, read in
                // whichever unit makes it display as typed, so text -> value ->
                // text comes back the same (hosts and CLAP validators check).
                .withValueFromStringFunction ([id, range] (const juce::String& text)
                {
                    const auto typed = text.trim();
                    // Every number in the text ("x32", "F1 +10 %", "-6.0 dB").
                    const auto numbersIn = [] (const juce::String& source)
                    {
                        std::vector<float> numbers;
                        for (int i = 0; i < source.length();)
                        {
                            const auto c = source[i];
                            const auto startsNumber = juce::CharacterFunctions::isDigit (c)
                                                      || ((c == '-' || c == '+' || c == '.') && i + 1 < source.length()
                                                          && (juce::CharacterFunctions::isDigit (source[i + 1]) || source[i + 1] == '.'));
                            if (! startsNumber)
                            {
                                ++i;
                                continue;
                            }
                            auto end = i + 1;
                            while (end < source.length() && (juce::CharacterFunctions::isDigit (source[end]) || source[end] == '.'))
                                ++end;
                            numbers.push_back (source.substring (i, end).getFloatValue());
                            i = end;
                        }
                        return numbers;
                    };
                    const auto numbers = numbersIn (typed);
                    if (numbers.empty())
                    {
                        // A word the knob shows ("C", "Auto", "Off"): the value
                        // that shows it; otherwise the start of the range.
                        for (const auto candidate : { 0.0f, range.start, range.end })
                            if (range.getRange().contains (candidate) || candidate == range.end)
                                if (describeValue (id, candidate).equalsIgnoreCase (typed))
                                    return candidate;
                        return range.start;
                    }
                    auto best = juce::jlimit (range.start, range.end, numbers.back());
                    auto bestDistance = std::numeric_limits<float>::max();
                    for (const auto number : numbers)
                        for (const auto scale : { 1.0f, 0.01f, 1000.0f, 0.001f, 100.0f, -1.0f, -0.01f, -1000.0f, -0.001f })
                        {
                            const auto candidate = range.snapToLegalValue (juce::jlimit (range.start, range.end, number * scale));
                            const auto shown = describeValue (id, candidate);
                            if (shown == typed)
                                return candidate;
                            const auto distance = std::abs (candidate - number * scale);
                            if (distance < bestDistance && scale > 0.0f)
                            {
                                bestDistance = distance;
                                best = candidate;
                            }
                        }

                    // Any other scaling (dB/oct as 6x the value, ...): bisect
                    // the range for the value whose shown number matches.
                    const auto shownNumber = [&id, &numbersIn] (float v)
                    {
                        const auto n = numbersIn (describeValue (id, v));
                        return n.empty() ? v : n.front();
                    };
                    auto lo = 0.0f, hi = 1.0f;
                    const auto rising = shownNumber (range.convertFrom0to1 (1.0f)) >= shownNumber (range.convertFrom0to1 (0.0f));
                    for (int step = 0; step < 40; ++step)
                    {
                        const auto mid = 0.5f * (lo + hi);
                        const auto below = shownNumber (range.convertFrom0to1 (mid)) < numbers.front();
                        (below == rising ? lo : hi) = mid;
                    }
                    // The bisection ends on the edge of the shown number's
                    // rounding: look at the values around it (legal steps, or
                    // small steps of the knob's travel when continuous).
                    const auto found = range.snapToLegalValue (range.convertFrom0to1 (0.5f * (lo + hi)));
                    for (int k = 0; k <= 60; ++k)
                        for (const auto sign : { 1.0f, -1.0f })
                        {
                            const auto candidate = range.interval > 0.0f
                                ? range.snapToLegalValue (juce::jlimit (range.start, range.end, found + sign * (float) k * range.interval))
                                : range.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, hi + sign * (float) k * 1.0e-5f));
                            if (describeValue (id, candidate) == typed)
                                return candidate;
                        }

                    // No value shows the text as typed: read the number in a
                    // unit the knob really shows, not whichever scale lands
                    // in range (99000 on CUTOFF read as 990 Hz). The units
                    // are the scales whose number reads back as the same
                    // text somewhere on the knob ("Hz" 1 and "kHz" 1000 on a
                    // cutoff, "%" 0.01 on a 0-1 amount); a typed unit ("k",
                    // "ms") picks among them; past both ends the number
                    // clamps to the nearer end.
                    const auto unitWord = [] (const juce::String& text)
                    {
                        return text.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ%");
                    };
                    struct Unit { float scale; juce::String word; };
                    std::vector<Unit> units;
                    for (int k = 0; k <= 48; ++k)
                    {
                        const auto value = range.convertFrom0to1 ((float) k / 48.0f);
                        const auto shown = describeValue (id, value);
                        const auto shownNumbers = numbersIn (shown);
                        if (shownNumbers.size() != 1 || shownNumbers.front() == 0.0f)
                            continue;
                        for (const auto scale : { 1.0f, 0.01f, 1000.0f, 0.001f, 100.0f })
                        {
                            // (Unclamped: at an end every scale would clamp back to it.)
                            const auto scaled = shownNumbers.front() * scale;
                            if (scaled < range.start || scaled > range.end
                                || describeValue (id, range.snapToLegalValue (scaled)) != shown)
                                continue;
                            const Unit unit { scale, unitWord (shown) };
                            if (std::none_of (units.begin(), units.end(), [&unit] (const Unit& u) { return u.scale == unit.scale && u.word == unit.word; }))
                                units.push_back (unit);
                        }
                    }
                    if (units.empty())
                        return best;

                    const auto typedWord = unitWord (typed);
                    std::vector<Unit> chosen;
                    if (typedWord.isNotEmpty())
                        for (const auto& unit : units)
                            if (unit.word.equalsIgnoreCase (typedWord) || unit.word.startsWithIgnoreCase (typedWord))
                                chosen.push_back (unit);
                    if (chosen.empty())
                        chosen = units;

                    const auto number = numbers.front();
                    const auto inRange = [&range] (float v) { return v >= range.start && v <= range.end; };
                    // In range and shown in the same unit first (5 on a
                    // cutoff is 5 kHz, 500 is 500 Hz), then anything in range.
                    for (const auto sameWord : { true, false })
                        for (const auto& unit : chosen)
                        {
                            const auto candidate = number * unit.scale;
                            if (inRange (candidate) && (! sameWord || unitWord (describeValue (id, candidate)) == unit.word))
                                return range.snapToLegalValue (candidate);
                        }
                    // Past both ends: the end nearest the number as typed.
                    auto clamped = range.start;
                    auto miss = std::numeric_limits<float>::max();
                    for (const auto& unit : chosen)
                    {
                        const auto candidate = number * unit.scale;
                        const auto end = juce::jlimit (range.start, range.end, candidate);
                        if (std::abs (candidate - end) < miss)
                        {
                            miss = std::abs (candidate - end);
                            clamped = end;
                        }
                    }
                    return range.snapToLegalValue (clamped);
                })));
    };

    // Integer parameters use the same value text as the others (units, note
    // names, "3 strings"), which ParamInfo already describes for them.
    const auto addInt = [&layout, hostName] (const juce::String& id, const juce::String& name, int min, int max, int def)
    {
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { id, 1 }, hostName (name), min, max, def,
            juce::AudioParameterIntAttributes()
                .withStringFromValueFunction ([id] (int value, int) { return describeValue (id, (float) value); })
                .withValueFromStringFunction ([id, min, max] (const juce::String& text)
                {
                    // A word the knob shows ("Free" is not note F).
                    for (const auto candidate : { min, max })
                        if (describeValue (id, (float) candidate).equalsIgnoreCase (text.trim()))
                            return candidate;
                    // Any value's own text (the Operator EG's break point
                    // shows a note name that isn't its number).
                    if (max - min <= 256)
                        for (auto candidate = min; candidate <= max; ++candidate)
                            if (describeValue (id, (float) candidate).equalsIgnoreCase (text.trim()))
                                return candidate;

                    // Note names ("C#3", C3 = 60) as well as plain numbers
                    // and numbered items ("#29").
                    const auto trimmed = text.trim().toUpperCase();
                    if (trimmed.startsWithChar ('#'))
                        return trimmed.substring (1).getIntValue();
                    const auto letter = trimmed.isNotEmpty() ? trimmed[0] : 0;
                    const int offsets[] { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G

                    if (letter >= 'A' && letter <= 'G')
                    {
                        auto note = offsets[letter - 'A'];
                        auto rest = trimmed.substring (1);

                        if (rest.startsWithChar ('#')) { ++note; rest = rest.substring (1); }
                        else if (rest.startsWithChar ('B')) { --note; rest = rest.substring (1); }

                        return note + 12 * (rest.getIntValue() + 2);
                    }

                    return trimmed.getIntValue();
                })));
    };

    const auto addBool = [&layout, hostName] (const juce::String& id, const juce::String& name, bool def)
    {
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { id, 1 }, hostName (name), def));
    };

    // Choices (modes, types, sources, destinations, shapes, FX slots) are not
    // host-automatable: hosts store automation as a normalised value, so a
    // list that grows in an update would move recorded lanes to other
    // entries. Saved states keep the index and load unchanged; the knobs,
    // switches and macros stay automatable.
    const auto addChoice = [&layout, hostName] (const juce::String& id, const juce::String& name,
                                      const juce::StringArray& choices, int def)
    {
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { id, 1 }, hostName (name), choices, def,
            juce::AudioParameterChoiceAttributes().withAutomatable (false)));
    };

    // The oscillator types; FM / DX7 is appended (OscMode, OscillatorIds.h).
    juce::StringArray oscModeChoices;
    for (const auto* modeName : OscMode::names)
        oscModeChoices.add (modeName);

    // OSC 1 and 2 retain their original parameter order and defaults.
    for (int osc = 0; osc < 2; ++osc)
    {
        const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        const auto first = osc == 0;
        const auto id = [&prefix] (const char* suffix) { return prefix + "_" + suffix; };

        addBool (id ("on"), name + " On", first);
        addChoice (id ("table"), name + " Table", getOscTableChoices(), first ? 7 : 1);
        addFloat (id ("frame"), name + " Frame", 0.0f, 1.0f, first ? 0.35f : 0.0f);
        addFloat (id ("level"), name + " Level", 0.0f, 1.0f, first ? 0.8f : 0.6f);
        addFloat (id ("pan"), name + " Pan", -1.0f, 1.0f, 0.0f);
        addInt (id ("semi"), name + " Semi", -24, 24, 0);
        addFloat (id ("fine"), name + " Fine", -100.0f, 100.0f, 0.0f, 1.0f, 1.0f);
        addInt (id ("unison"), name + " Unison", 1, VoiceParams::maxUnison, 1);
        addFloat (id ("detune"), name + " Detune", 0.0f, 200.0f, 15.0f, 0.6f);
        addFloat (id ("spread"), name + " Spread", 0.0f, 1.0f, 0.5f);
        addChoice (id ("warp"), name + " Warp", Warp::getNames(), 0);
        addFloat (id ("warp_amt"), name + " Warp Amount", 0.0f, 1.0f, 0.0f);
        addChoice (id ("spectral"), name + " Spectral Warp", SpectralWarp::getNames(), 0);
        addFloat (id ("spectral_amt"), name + " Spectral Amount", 0.0f, 1.0f, 0.5f);
        addChoice (id ("uni_mode"), name + " Unison Mode", UnisonMode::getNames(), 0);
        addFloat (id ("uni_blend"), name + " Unison Blend", 0.0f, 1.0f, 1.0f);
        addFloat (id ("uni_frame"), name + " Unison Frame Spread", 0.0f, 1.0f, 0.0f);
        addFloat (id ("uni_warp"), name + " Unison Warp Spread", 0.0f, 1.0f, 0.0f);
        addChoice (id ("scale"), name + " Scale Quantize", Scales::getNames(), 0);
        addChoice (id ("scale_root"), name + " Scale Root", Scales::getRootNames(), 0);
        addChoice (id ("route"), name + " Filter Route", FilterRoute::getNames(), 0);
    }

    // OSC 3: a full oscillator like OSC 1 and 2 (its ids keep the old "sub_"
    // prefix so saved patches still line up; see migrateLegacyOsc3).
    addBool ("sub_on", "Osc3 On", false);
    addFloat ("sub_level", "Osc3 Level", 0.0f, 1.0f, 0.6f);

    // The dedicated sub oscillator.
    addBool ("subosc_on", "Sub On", false);
    addFloat ("subosc_level", "Sub Level", 0.0f, 1.0f, 0.5f);
    addChoice ("sub_shape", "Sub Shape", { "Sine", "Square", "Saw" }, 0);
    addChoice ("sub_octave", "Sub Octave", { "-1 Oct", "-2 Oct" }, 0);
    addChoice ("subosc_route", "Sub + Noise Route", FilterRoute::getNames(), 0);
    addChoice ("sub_mode", "Osc3 Mode", oscModeChoices, 0);
    addChoice ("osc1_sample_factory", "Osc1 Sample Source",
               { "User File", "Metal Hit", "Vocal Ah", "Sub Tone", "Vinyl Loop", "Noise Rise" }, 0);
    addChoice ("osc2_sample_factory", "Osc2 Sample Source",
               { "User File", "Metal Hit", "Vocal Ah", "Sub Tone", "Vinyl Loop", "Noise Rise" }, 0);
    addChoice ("sub_sample_factory", "Osc3 Sample Source",
               { "User File", "Metal Hit", "Vocal Ah", "Sub Tone", "Vinyl Loop", "Noise Rise" }, 0);
    addChoice ("sub_table", "Osc3 Table", getOscTableChoices(), 0);
    addFloat ("sub_frame", "Osc3 Frame", 0.0f, 1.0f, 0.0f);
    addFloat ("sub_pan", "Osc3 Pan", -1.0f, 1.0f, 0.0f);
    addInt ("sub_semi", "Osc3 Semi", -24, 24, 0);
    addFloat ("sub_fine", "Osc3 Fine", -100.0f, 100.0f, 0.0f, 1.0f, 1.0f);
    addInt ("sub_unison", "Osc3 Unison", 1, VoiceParams::maxUnison, 1);
    addFloat ("sub_detune", "Osc3 Detune", 0.0f, 200.0f, 12.0f, 0.6f);
    addFloat ("sub_spread", "Osc3 Spread", 0.0f, 1.0f, 0.0f);
    addChoice ("sub_warp", "Osc3 Warp", Warp::getNames(), 0);
    addFloat ("sub_warp_amt", "Osc3 Warp Amount", 0.0f, 1.0f, 0.0f);
    addChoice ("sub_spectral", "Osc3 Spectral Warp", SpectralWarp::getNames(), 0);
    addFloat ("sub_spectral_amt", "Osc3 Spectral Amount", 0.0f, 1.0f, 0.5f);
    addChoice ("sub_uni_mode", "Osc3 Unison Mode", UnisonMode::getNames(), 0);
    addFloat ("sub_uni_blend", "Osc3 Unison Blend", 0.0f, 1.0f, 1.0f);
    addFloat ("sub_uni_frame", "Osc3 Unison Frame Spread", 0.0f, 1.0f, 0.0f);
    addFloat ("sub_uni_warp", "Osc3 Unison Warp Spread", 0.0f, 1.0f, 0.0f);
    addChoice ("sub_scale", "Osc3 Scale Quantize", Scales::getNames(), 0);
    addChoice ("sub_scale_root", "Osc3 Scale Root", Scales::getRootNames(), 0);
    addChoice ("sub_route", "Osc3 Filter Route", FilterRoute::getNames(), 0);
    addChoice ("sub_excite", "Osc3 Excite", { "Burst", "Noise", "Saw", "Pulse", "Bow", "Bright Hammer", "Osc In", "Tine", "Reed", "Piano", "Feedback", "Modal Piano" }, 0);
    addFloat ("sub_string_decay", "Osc3 String Decay", 0.0f, 1.0f, 0.75f);
    addFloat ("sub_string_damp", "Osc3 String Damp", 0.0f, 1.0f, 0.35f);
    addFloat ("sub_string_sustain", "Osc3 String Sustain", 0.0f, 1.0f, 0.0f);
    addFloat ("sub_string_stiffness", "Osc3 Stiffness", 0.0f, 1.0f, 0.0f);
    addFloat ("sub_string_pickup", "Osc3 Pickup Position", 0.0f, 1.0f, 0.0f);
    addFloat ("sub_string_excite_pos", "Osc3 Excitation Position", 0.0f, 1.0f, 0.0f);
    addFloat ("sub_string_pick_hardness", "Osc3 Pick Hardness", 0.0f, 1.0f, 1.0f);
    addFloat ("sub_string_pick_pos", "Osc3 Pick Position", 0.0f, 1.0f, 0.0f);
    addBool ("sub_string_slap", "Osc3 Slap", false);
    addChoice ("sub_chord", "Osc3 Chord", { "Off", "Octave", "Fifth", "Power", "Major", "Minor", "Sus4" }, 0);
    addFloat ("noise_level", "Noise Level", 0.0f, 1.0f, 0.0f);

    // Filter 1
    addChoice ("f1_type", "F1 Type", FilterType::getNames(), 0);
    addChoice ("f1_slope", "F1 Slope", { "12 dB", "24 dB" }, 0);
    addFloat ("f1_cutoff", "F1 Cutoff", 20.0f, 20000.0f, 12000.0f, 0.25f);
    addFloat ("f1_reso", "F1 Reso", 0.0f, 1.0f, 0.25f);
    addFloat ("f1_drive", "F1 Drive", 1.0f, 10.0f, 1.5f);
    addFloat ("f1_env", "F1 Env", -5.0f, 5.0f, 0.0f); // (1.5 before the knob went: see migrateFilterEnvAmounts)
    addFloat ("f1_keytrack", "F1 Key Track", -1.0f, 1.0f, 0.0f);
    addFloat ("f1_morph", "F1 Morph", 0.0f, 1.0f, 0.0f);

    // Filter 2
    addChoice ("f2_type", "F2 Type", FilterType::getNames(), 0);
    addChoice ("f2_slope", "F2 Slope", { "12 dB", "24 dB" }, 0);
    addFloat ("f2_cutoff", "F2 Cutoff", 20.0f, 20000.0f, 20000.0f, 0.25f);
    addFloat ("f2_reso", "F2 Reso", 0.0f, 1.0f, 0.0f);
    addFloat ("f2_drive", "F2 Drive", 1.0f, 10.0f, 1.0f);
    addFloat ("f2_env", "F2 Env", -5.0f, 5.0f, 0.0f);
    addFloat ("f2_keytrack", "F2 Key Track", -1.0f, 1.0f, 0.0f);
    addFloat ("f2_morph", "F2 Morph", 0.0f, 1.0f, 0.0f);
    addBool ("filters_parallel", "Filters Parallel", false);
    addFloat ("filter_balance", "Filter Balance", -1.0f, 1.0f, 0.0f);

    // Amp envelope
    addFloat ("amp_attack", "Amp Attack", 0.001f, 5.0f, 0.005f, 0.35f);
    addFloat ("amp_decay", "Amp Decay", 0.005f, 5.0f, 0.3f, 0.35f);
    addFloat ("amp_sustain", "Amp Sustain", 0.0f, 1.0f, 0.8f);
    addFloat ("amp_release", "Amp Release", 0.005f, 10.0f, 0.25f, 0.35f);

    // Filter envelope
    addFloat ("fe_attack", "FE Attack", 0.001f, 5.0f, 0.001f, 0.35f);
    addFloat ("fe_decay", "FE Decay", 0.005f, 5.0f, 0.5f, 0.35f);
    addFloat ("fe_sustain", "FE Sustain", 0.0f, 1.0f, 0.3f);
    addFloat ("fe_release", "FE Release", 0.005f, 10.0f, 0.3f, 0.35f);

    // Mod envelope
    addFloat ("me_attack", "ME Attack", 0.001f, 5.0f, 0.05f, 0.35f);
    addFloat ("me_decay", "ME Decay", 0.005f, 5.0f, 0.4f, 0.35f);
    addFloat ("me_sustain", "ME Sustain", 0.0f, 1.0f, 0.5f);
    addFloat ("me_release", "ME Release", 0.005f, 10.0f, 0.3f, 0.35f);

    // Filter 2 envelope
    addFloat ("f2e_attack", "F2E Attack", 0.001f, 5.0f, 0.01f, 0.35f);
    addFloat ("f2e_decay", "F2E Decay", 0.005f, 5.0f, 0.4f, 0.35f);
    addFloat ("f2e_sustain", "F2E Sustain", 0.0f, 1.0f, 0.4f);
    addFloat ("f2e_release", "F2E Release", 0.005f, 10.0f, 0.3f, 0.35f);
    addFloat ("f2e_curve", "F2E Tension", -1.0f, 1.0f, 0.0f);

    // Envelope 4
    addFloat ("e4_attack", "E4 Attack", 0.001f, 5.0f, 0.05f, 0.35f);
    addFloat ("e4_decay", "E4 Decay", 0.005f, 5.0f, 0.4f, 0.35f);
    addFloat ("e4_sustain", "E4 Sustain", 0.0f, 1.0f, 0.5f);
    addFloat ("e4_release", "E4 Release", 0.005f, 10.0f, 0.3f, 0.35f);
    addFloat ("e4_curve", "E4 Tension", -1.0f, 1.0f, 0.0f);

    addFloat ("amp_curve", "Amp Tension", -1.0f, 1.0f, 0.0f);
    addFloat ("fe_curve", "FE Tension", -1.0f, 1.0f, 0.0f);
    addFloat ("me_curve", "ME Tension", -1.0f, 1.0f, 0.0f);

    // Global
    addFloat ("amp_velocity", "Amp Velocity", 0.0f, 1.0f, 0.3f);
    addFloat ("filter_velocity", "Filter Velocity", 0.0f, 1.0f, 0.0f); // (scales the FILTER ENV source by velocity; the old default 0.5 belonged to the removed ENV AMOUNT)
    addFloat ("glide", "Glide", 0.0f, 2.0f, 0.0f, 0.13f); // (skew 0.13: 10 ms sits at half the travel; most glides are 0-10 ms, the longer ones effects)
    addFloat ("bend_range", "Bend Range", 0.0f, 24.0f, 2.0f, 1.0f, 1.0f);
    addFloat ("drift", "Drift", 0.0f, 1.0f, 0.0f);
    addFloat ("tune_loose", "Loose Tuning", 0.0f, 40.0f, 0.0f); // cents
    addBool ("tune_loose_both", "Loose Tuning Both Ways", true);

    // Oscillator cross-modulation
    addFloat ("fm_amount", "FM Osc2 > Osc1", 0.0f, 1.0f, 0.0f);
    addFloat ("fm_feedback", "FM Osc1 Feedback", 0.0f, 1.0f, 0.0f);
    addFloat ("fm_1to2", "FM Osc1 > Osc2", 0.0f, 1.0f, 0.0f);
    addFloat ("fm_1to3", "FM Osc1 > Osc3", 0.0f, 1.0f, 0.0f);
    addFloat ("fm_2to3", "FM Osc2 > Osc3", 0.0f, 1.0f, 0.0f);
    addFloat ("fm_3to1", "FM Osc3 > Osc1", 0.0f, 1.0f, 0.0f);
    addFloat ("fm_3to2", "FM Osc3 > Osc2", 0.0f, 1.0f, 0.0f);
    addFloat ("fm_fb2", "FM Osc2 Feedback", 0.0f, 1.0f, 0.0f);
    addFloat ("fm_fb3", "FM Osc3 Feedback", 0.0f, 1.0f, 0.0f);
    addChoice ("fm_mode", "FM Mode", { "Phase", "Through-Zero", "Exponential" }, 0);
    // OSC 1-3 here; OSC 4-6 add theirs with the M3b parameters below (this
    // loop once ran over all six and declared those three twice).
    for (int osc = 0; osc < 3; ++osc)
        addBool (juce::String (OscillatorIds::prefixes[(size_t) osc]) + "_out",
                 "Osc" + juce::String (osc + 1) + " Output", true);
    addFloat ("ring_mod", "Ring Mod", 0.0f, 1.0f, 0.0f);
    addBool ("hard_sync", "Hard Sync", false);
    addFloat ("f1_fm", "F1 Audio FM", -1.0f, 1.0f, 0.0f);
    addFloat ("f2_fm", "F2 Audio FM", -1.0f, 1.0f, 0.0f);

    // Voice behaviour
    addChoice ("osc1_chord", "Osc1 Chord", { "Off", "Octave", "Fifth", "Power", "Major", "Minor", "Sus4" }, 0);
    addChoice ("osc2_chord", "Osc2 Chord", { "Off", "Octave", "Fifth", "Power", "Major", "Minor", "Sus4" }, 0);
    addFloat ("voice_spread", "Voice Spread", 0.0f, 1.0f, 0.0f);
    addFloat ("unison_random", "Unison Random", 0.0f, 1.0f, 0.0f);
    addBool ("mpe_mode", "MPE Mode", false);
    addChoice ("voice_mode", "Voice Mode", { "Poly", "Mono", "Legato" }, 0);
    addInt ("poly_voices", "Poly Voices", 1, numVoices, numVoices);
    addBool ("glide_legato", "Glide Legato Only", false);

    // Resonator bank
    addBool ("res_on", "Resonator On", false);
    addFloat ("res_amount", "Res Amount", 0.0f, 1.0f, 0.5f);
    addFloat ("res_decay", "Res Decay", 0.0f, 1.0f, 0.7f);
    addFloat ("res_offset", "Res Offset", -12.0f, 12.0f, 0.0f, 1.0f, 1.0f);
    addFloat ("res_keytrack", "Res Key Track", 0.0f, 1.0f, 1.0f);
    addChoice ("body_type", "Body Type", { "Classic", "Bar", "Plate", "Bell", "Shell", "Guitar" }, 0);
    addFloat ("body_material", "Body Material", 0.0f, 1.0f, 0.0f);
    addFloat ("body_size", "Body Size", 0.0f, 1.0f, 0.5f);
    addChoice ("body_coupling_mode", "Body Coupling", { "Off", "String to body", "Body to string", "Strings" }, 0);
    addFloat ("body_coupling", "Body Coupling Amount", 0.0f, 1.0f, 0.0f);

    // Clocked sample and hold + MSEG
    addChoice ("clock_div", "Clock S&H Div", getSyncDivisionNames(), 3);
    addBool ("mseg_loop", "MSEG Loop", true);
    addFloat ("mseg_rate", "MSEG Rate", 0.02f, 8.0f, 0.5f, 0.4f);

    for (int point = 1; point <= 4; ++point)
    {
        const float levels[4] { 0.0f, 1.0f, 0.0f, -1.0f };
        addFloat ("mseg_level" + juce::String (point), "MSEG Level " + juce::String (point),
                  -1.0f, 1.0f, levels[point - 1]);
        addFloat ("mseg_time" + juce::String (point), "MSEG Time " + juce::String (point),
                  0.05f, 1.0f, 0.25f);
    }

    // String (Karplus-Strong) oscillators
    for (int osc = 1; osc <= 2; ++osc)
    {
        const auto prefix = "osc" + juce::String (osc);

        addChoice (prefix + "_mode", "Osc" + juce::String (osc) + " Mode", oscModeChoices, 0);
        addChoice (prefix + "_excite", "Osc" + juce::String (osc) + " Excite", { "Burst", "Noise", "Saw", "Pulse", "Bow", "Bright Hammer", "Osc In", "Tine", "Reed", "Piano", "Feedback", "Modal Piano" }, 0);
        addFloat (prefix + "_string_decay", "Osc" + juce::String (osc) + " String Decay", 0.0f, 1.0f, 0.75f);
        addFloat (prefix + "_string_damp", "Osc" + juce::String (osc) + " String Damp", 0.0f, 1.0f, 0.35f);
        addFloat (prefix + "_string_sustain", "Osc" + juce::String (osc) + " String Sustain", 0.0f, 1.0f, 0.0f);
        addFloat (prefix + "_string_stiffness", "Osc" + juce::String (osc) + " Stiffness", 0.0f, 1.0f, 0.0f);
        addFloat (prefix + "_string_pickup", "Osc" + juce::String (osc) + " Pickup Position", 0.0f, 1.0f, 0.0f);
        addFloat (prefix + "_string_excite_pos", "Osc" + juce::String (osc) + " Excitation Position", 0.0f, 1.0f, 0.0f);
        addFloat (prefix + "_string_pick_hardness", "Osc" + juce::String (osc) + " Pick Hardness", 0.0f, 1.0f, 1.0f);
        addFloat (prefix + "_string_pick_pos", "Osc" + juce::String (osc) + " Pick Position", 0.0f, 1.0f, 0.0f);
        addBool (prefix + "_string_slap", "Osc" + juce::String (osc) + " Slap", false);
    }

    for (const auto* prefix : { "osc1", "osc2", "sub" })
    {
        const juce::String oscLabel = juce::String (prefix) == "sub" ? juce::String ("Osc3") : juce::String (prefix).replace ("osc", "Osc");
        addFloat (juce::String (prefix) + "_bow_pressure", oscLabel + " Bow Pressure", 0.0f, 1.0f, 0.5f);
        addFloat (juce::String (prefix) + "_bow_speed", oscLabel + " Bow Speed", 0.0f, 1.0f, 0.5f);
        addFloat (juce::String (prefix) + "_bridge_buzz", oscLabel + " Bridge Buzz", 0.0f, 1.0f, 0.0f);
        addFloat (juce::String (prefix) + "_fret_rattle", oscLabel + " Fret Rattle", 0.0f, 1.0f, 0.0f);
        addFloat (juce::String (prefix) + "_string_split", oscLabel + " String Split", 0.0f, 1.0f, 0.0f);
        addFloat (juce::String (prefix) + "_string_click", oscLabel + " String Click", 0.0f, 1.0f, 1.0f);
        const auto id = juce::String (prefix);

        addBool (id + "_sample_tuned", oscLabel + " Sample Tuned", true);
        addBool (id + "_sample_loop", oscLabel + " Sample Loop", false);
        addBool (id + "_sample_reverse", oscLabel + " Sample Reverse", false);
        addFloat (id + "_sample_start", oscLabel + " Sample Start", 0.0f, 1.0f, 0.0f);
        addFloat (id + "_sample_end", oscLabel + " Sample End", 0.0f, 1.0f, 1.0f);
        addFloat (id + "_sample_fade_in", oscLabel + " Sample Fade In", 0.0f, 1.0f, 0.0f);
        addFloat (id + "_sample_fade_out", oscLabel + " Sample Fade Out", 0.0f, 1.0f, 0.0f);

        // Granular mode reads the same sample; its position is Sample Start.
        addFloat (id + "_grain_size", oscLabel + " Grain Size", 10.0f, 500.0f, 80.0f, 0.4f);
        addFloat (id + "_grain_density", oscLabel + " Grain Density", 0.0f, 1.0f, 0.5f);
        addFloat (id + "_grain_spray", oscLabel + " Grain Spray", 0.0f, 1.0f, 0.15f);
        addFloat (id + "_grain_pitch", oscLabel + " Grain Pitch Spray", 0.0f, 1.0f, 0.0f);
        addFloat (id + "_grain_spread", oscLabel + " Grain Stereo Spread", 0.0f, 1.0f, 0.6f);
    }

    // Arpeggiator
    addBool ("arp_on", "Arp On", false);
    addChoice ("arp_mode", "Arp Mode",
               { "Up", "Down", "UpDown", "Random", "DownUp", "Converge", "Walk", "Chord", "Scale Random" }, 0);
    addFloat ("arp_chance", "Arp Chance", 0.0f, 1.0f, 1.0f);

    // Generative: scale snapping and note spray
    // Named apart from the Scala tuning's scale (review 6, I6-30); the IDs stay.
    addChoice ("gen_scale", "Snap To Key", Scales::getNames(), 0);
    addChoice ("gen_root", "Key Root", Scales::getRootNames(), 0);
    addBool ("sym_on", "Sympathetic Strings", false);
    addFloat ("sym_amount", "Sympathetic Amount", 0.0f, 1.0f, 0.5f);
    addFloat ("sym_decay", "Sympathetic Decay", 0.0f, 1.0f, 0.75f);
    addInt ("sym_count", "Sympathetic Strings Count", 1, 6, 3);
    addBool ("sym_manual", "Sympathetic Manual Tuning", false);
    constexpr int defaultSymNotes[] { 48, 55, 60, 64, 67, 72 };
    for (int i = 1; i <= 6; ++i)
        addInt ("sym_note" + juce::String (i), "Sympathetic Note " + juce::String (i), 36, 96,
                defaultSymNotes[i - 1]);
    addBool ("gen_snap", "Snap Played Notes", false);
    addBool ("spray_on", "Note Spray", false);
    addInt ("spray_count", "Spray Notes", 1, 8, 3);
    addInt ("spray_range", "Spray Range", 1, 24, 12);
    addChoice ("spray_direction", "Spray Direction", { "Up", "Down", "Both" }, 2);
    addFloat ("spray_spread", "Spray Spread", 0.0f, 2000.0f, 0.0f, 0.4f);
    addFloat ("spray_chance", "Spray Chance", 0.0f, 1.0f, 1.0f);
    addFloat ("spray_velocity", "Spray Velocity", 0.0f, 1.0f, 0.3f);
    addChoice ("arp_div", "Arp Div", getSyncDivisionNames(), 3);
    addInt ("arp_octaves", "Arp Octaves", 1, 4, 1);
    addFloat ("arp_gate", "Arp Gate", 0.05f, 1.0f, 0.5f);

    // LFOs
    const juce::StringArray lfoShapes { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Draw", "Steps", "Curve",
                                       "Smooth Random", "Drunk", "Chaos (classic)", "Bounce (classic)", "Pendulum (classic)",
                                       "Spring (classic)", "Friction (classic)",
                                       // M8.1 (appended: patches store the index)
                                       "Random S&H", "Sine Random", "Perlin", "Drunk Walk",
                                       "Lorenz", "Rossler", "Duffing", "Logistic Map", "Henon Map", "Double Pendulum",
                                       "Bounce", "Pendulum", "Spring", "Friction" };

    for (int lfo = 1; lfo <= numLfos; ++lfo)
    {
        const auto prefix = "lfo" + juce::String (lfo);

        addChoice (prefix + "_shape", "LFO" + juce::String (lfo) + " Shape", lfoShapes, 0);
        addFloat (prefix + "_rate", "LFO" + juce::String (lfo) + " Rate", 0.01f, 40.0f, 4.0f, 0.3f);
        addBool (prefix + "_sync", "LFO" + juce::String (lfo) + " Sync", false);
        addChoice (prefix + "_div", "LFO" + juce::String (lfo) + " Div", getSyncDivisionNames(), 2);
        addBool (prefix + "_retrig", "LFO" + juce::String (lfo) + " Retrig", false);
        addFloat (prefix + "_phase", "LFO" + juce::String (lfo) + " Start Phase", 0.0f, 1.0f, 0.0f);
        addBool (prefix + "_key", "LFO" + juce::String (lfo) + " Key Track", false);
        addFloat (prefix + "_phys_a", "LFO" + juce::String (lfo) + " Physics A", 0.0f, 1.0f, 0.5f);
        addFloat (prefix + "_phys_b", "LFO" + juce::String (lfo) + " Physics B", 0.0f, 1.0f, 0.5f);
        addBool (prefix + "_kick", "LFO" + juce::String (lfo) + " Pendulum Kick", false);

        for (int step = 0; step < 16; ++step)
            addFloat (prefix + "_step" + juce::String (step + 1), "LFO" + juce::String (lfo) + " Step " + juce::String (step + 1),
                      -1.0f, 1.0f, (float) std::sin (juce::MathConstants<double>::twoPi * (double) step / 16.0));
    }

    // Mod matrix
    const auto sourceNames = Mod::getSourceNames();
    const auto destinationNames = Mod::getDestinationNames();
    const auto addModSlotParameters = [&] (int slot)
    {
        const auto prefix = "mod" + juce::String (slot);
        const auto name = "Mod" + juce::String (slot);

        addChoice (prefix + "_src", name + " Src", sourceNames, 0);
        addChoice (prefix + "_dst", name + " Dst", destinationNames, 0);
        addFloat (prefix + "_amt", name + " Amt", -1.0f, 1.0f, 0.0f);
        addFloat (prefix + "_curve", name + " Curve", -1.0f, 1.0f, 0.0f);
        addChoice (prefix + "_pol", name + " Polarity", { "Auto", "Unipolar", "Bipolar", "Negative" }, 0);
        addChoice (prefix + "_aux", name + " Via", sourceNames, 0);
        addBool (prefix + "_byp", name + " Bypass", false);
    };
    constexpr int legacyModSlots = 32;

    for (int slot = 1; slot <= legacyModSlots; ++slot)
        addModSlotParameters (slot);

    // Macros
    for (int macro = 1; macro <= 4; ++macro)
        addFloat ("macro" + juce::String (macro), "Macro " + juce::String (macro), 0.0f, 1.0f, 0.0f);

    // Effects
    const juce::StringArray fxTypes { "None", "Amp", "Drive", "Crush", "Comp", "Comb", "Phaser",
                                      "Chorus", "Haas", "Delay", "Stutter", "Smear", "Freeze", "Reverb",
                                      "Flanger", "Dimension", "Trance Gate", "TapeStop", "Tilt", "Utility",
                                      "OTT", "Limiter", "Widener", "Tremolo", "FreqShift", "RingMod",
                                      "Octaver", "Vowel", "Feedback", "EQ", "Airwindows", "Vocoder",
                                      // 32-41: the Airwindows category modules (append only)
                                      "AW Tape", "AW Saturation", "AW Reverb", "AW Delay", "AW Modulation",
                                      "AW Dynamics", "AW EQ", "AW Console", "AW Lo-Fi", "AW Stereo",
                                      // 42: the synth's filters as an effect (append only)
                                      "Filter" };
    for (int slot = 0; slot < numFxSlots; ++slot)
    {
        addChoice ("fx_slot" + juce::String (slot + 1), "FX Slot " + juce::String (slot + 1),
                   fxTypes, 0);
        addBool ("fx_slot" + juce::String (slot + 1) + "_bypass", "FX Bypass " + juce::String (slot + 1), false);
        addBool ("fx_slot" + juce::String (slot + 1) + "_solo", "FX Solo " + juce::String (slot + 1), false);
        addFloat ("fx_slot" + juce::String (slot + 1) + "_mix", "FX Blend " + juce::String (slot + 1),
                  0.0f, 1.0f, 1.0f);
    }

    addFloat ("fx_eq_low_freq", "EQ Low Freq", 20.0f, 800.0f, 120.0f, 0.4f);
    addFloat ("fx_eq_low_gain", "EQ Low Gain", -18.0f, 18.0f, 0.0f);
    addFloat ("fx_eq_mid_freq", "EQ Mid Freq", 100.0f, 12000.0f, 1000.0f, 0.3f);
    addFloat ("fx_eq_mid_gain", "EQ Mid Gain", -18.0f, 18.0f, 0.0f);
    addFloat ("fx_eq_mid_q", "EQ Mid Q", 0.2f, 8.0f, 0.9f, 0.4f);
    addFloat ("fx_eq_high_freq", "EQ High Freq", 1000.0f, 18000.0f, 6000.0f, 0.4f);
    addFloat ("fx_eq_high_gain", "EQ High Gain", -18.0f, 18.0f, 0.0f);

    addFloat ("fx_feedback_amount", "Feedback Amount", 0.0f, 0.95f, 0.6f);
    addFloat ("fx_feedback_delay", "Feedback Delay", 1.0f, 100.0f, 12.0f, 0.4f);
    addFloat ("fx_feedback_tone", "Feedback Tone", 0.0f, 1.0f, 0.5f);
    addFloat ("fx_feedback_mix", "Feedback Mix", 0.0f, 1.0f, 0.5f);

    addBool ("fx_stutter_reverse", "Stutter Reverse", false);
    addFloat ("fx_stutter_pitch", "Stutter Pitch", -12.0f, 12.0f, 0.0f, 1.0f, 1.0f);

    addFloat ("fx_flanger_rate", "Flanger Rate", 0.05f, 8.0f, 0.5f, 0.4f);
    addFloat ("fx_flanger_depth", "Flanger Depth", 0.0f, 1.0f, 0.5f);
    addFloat ("fx_flanger_feedback", "Flanger Feedback", 0.0f, 0.9f, 0.5f);
    addFloat ("fx_flanger_mix", "Flanger Mix", 0.0f, 1.0f, 0.5f);

    addFloat ("fx_dim_rate", "Dimension Rate", 0.05f, 4.0f, 0.4f, 0.4f);
    addFloat ("fx_dim_depth", "Dimension Depth", 0.0f, 1.0f, 0.6f);
    addFloat ("fx_dim_mix", "Dimension Mix", 0.0f, 1.0f, 0.6f);

    addChoice ("fx_gate_div", "Gate Div", getSyncDivisionNames(), 4);
    addChoice ("fx_gate_pattern", "Gate Pattern",
               { "Straight", "Offbeat", "Triplet", "Gallop", "Random", "Dual", "Build", "Break", "Custom" }, 0);
    addInt ("fx_gate_steps", "Gate Steps", 2, 16, 16);
    addFloat ("fx_gate_swing", "Gate Swing", 0.0f, 0.5f, 0.0f);

    for (int step = 1; step <= 16; ++step)
        addFloat ("fx_gate_step" + juce::String (step), "Gate Step " + juce::String (step), 0.0f, 1.0f, 1.0f);
    addFloat ("fx_gate_smooth", "Gate Smooth", 0.0f, 1.0f, 0.2f);
    addFloat ("fx_gate_mix", "Gate Mix", 0.0f, 1.0f, 1.0f);

    addBool ("fx_tape_stop_trigger", "Tape Stop", false);
    addFloat ("fx_tape_stop_time", "Stop Time", 0.05f, 4.0f, 0.6f, 0.5f);
    addFloat ("fx_tape_stop_mix", "Stop Mix", 0.0f, 1.0f, 1.0f);

    addFloat ("fx_tilt", "Tilt", -1.0f, 1.0f, 0.0f);
    addFloat ("fx_tilt_level", "Tilt Level", -24.0f, 24.0f, 0.0f);

    addFloat ("fx_util_gain", "Utility Gain", -24.0f, 24.0f, 0.0f);
    addBool ("fx_util_mono", "Mono Maker", false);
    addBool ("fx_util_invert", "Polarity Invert", false);

    addFloat ("fx_ott_amount", "OTT Amount", 0.0f, 1.0f, 0.5f);
    addFloat ("fx_ott_mix", "OTT Mix", 0.0f, 1.0f, 1.0f);

    addFloat ("fx_limit_ceiling", "Limiter Ceiling", -24.0f, 0.0f, -0.3f);
    addFloat ("fx_limit_release", "Limiter Release", 10.0f, 1000.0f, 120.0f, 0.4f);

    addFloat ("fx_width", "Width", 0.0f, 2.0f, 1.5f);
    addFloat ("fx_width_mix", "Width Mix", 0.0f, 1.0f, 0.5f);

    addFloat ("fx_trem_rate", "Tremolo Rate", 0.05f, 20.0f, 4.0f, 0.4f);
    addFloat ("fx_trem_depth", "Tremolo Depth", 0.0f, 1.0f, 0.7f);
    addChoice ("fx_trem_shape", "Tremolo Shape", { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H" }, 0);

    addFloat ("fx_shifter_shift", "Freq Shift", -2000.0f, 2000.0f, 0.0f, 1.0f, 1.0f);
    addFloat ("fx_shifter_mix", "Shift Mix", 0.0f, 1.0f, 0.5f);

    addFloat ("fx_ring_freq", "Ring Freq", 1.0f, 5000.0f, 220.0f, 0.3f);
    addFloat ("fx_ring_mix", "Ring Mix", 0.0f, 1.0f, 0.5f);

    addFloat ("fx_octaver_mix", "Octaver Mix", 0.0f, 1.0f, 0.5f);

    addFloat ("fx_vowel_morph", "Vowel Morph", 0.0f, 1.0f, 0.0f);
    addFloat ("fx_vowel_mix", "Vowel Mix", 0.0f, 1.0f, 1.0f);

    addFloat ("fx_delay_time_r", "Delay Time R", 1.0f, 2000.0f, 500.0f, 0.4f);
    addFloat ("fx_delay_duck", "Delay Duck", 0.0f, 1.0f, 0.0f);

    addChoice ("fx_reverb_type", "Reverb Type", { "Room", "Hall", "Plate", "Shimmer", "Spring", "Gated", "IR" }, 0);

    addChoice ("fx_amp_mode", "Amp Mode", { "Tube", "Fuzz", "Clean" }, 0);
    addFloat ("fx_amp_drive", "Amp Drive", 1.0f, 20.0f, 4.0f);
    addFloat ("fx_amp_bass", "Amp Bass", 0.0f, 2.0f, 1.0f);
    addFloat ("fx_amp_mid", "Amp Mid", 0.0f, 2.0f, 1.0f);
    addFloat ("fx_amp_treble", "Amp Treble", 0.0f, 2.0f, 1.0f);
    addFloat ("fx_amp_level", "Amp Level", 0.0f, 2.0f, 1.0f);

    addFloat ("fx_comp_threshold", "Comp Threshold", -60.0f, 0.0f, -18.0f);
    addFloat ("fx_comp_ratio", "Comp Ratio", 1.0f, 20.0f, 4.0f);
    addFloat ("fx_comp_attack", "Comp Attack", 0.1f, 100.0f, 10.0f, 0.4f);
    addFloat ("fx_comp_release", "Comp Release", 10.0f, 1000.0f, 150.0f, 0.4f);
    addFloat ("fx_comp_makeup", "Comp Makeup", 0.0f, 24.0f, 6.0f);
    addFloat ("fx_comp_mix", "Comp Mix", 0.0f, 1.0f, 1.0f);

    addFloat ("fx_haas_delay", "Haas Delay", 1.0f, 40.0f, 14.0f);
    addFloat ("fx_haas_mix", "Haas Mix", 0.0f, 1.0f, 0.6f);

    addFloat ("fx_drive_amount", "Drive Amount", 1.0f, 20.0f, 3.0f);
    addFloat ("fx_drive_mix", "Drive Mix", 0.0f, 1.0f, 1.0f);
    addFloat ("fx_fold", "Fold", 0.0f, 1.0f, 0.0f);
    addChoice ("fx_drive_type", "Drive Type", { "Soft Clip", "Hard Clip" }, 0);
    addChoice ("fx_fold_type", "Fold Type", { "Linear", "Sine" }, 0);
    addBool ("fx_drive_on", "Drive On", true);

    addBool ("fx_crush_on", "Crush On", false);
    addFloat ("fx_crush_bits", "Crush Bits", 1.0f, 16.0f, 8.0f, 1.0f, 1.0f);
    addFloat ("fx_crush_down", "Crush Down", 1.0f, 32.0f, 2.0f, 1.0f, 1.0f);
    addFloat ("fx_crush_mix", "Crush Mix", 0.0f, 1.0f, 1.0f);

    addBool ("fx_chorus_on", "Chorus On", false);
    addFloat ("fx_chorus_rate", "Chorus Rate", 0.05f, 8.0f, 0.8f, 0.4f);
    addFloat ("fx_chorus_depth", "Chorus Depth", 0.0f, 1.0f, 0.3f);
    addFloat ("fx_chorus_mix", "Chorus Mix", 0.0f, 1.0f, 0.5f);

    addBool ("fx_delay_on", "Delay On", false);
    addFloat ("fx_delay_time", "Delay Time", 1.0f, 2000.0f, 375.0f, 0.4f);
    addBool ("fx_delay_sync", "Delay Sync", false);
    addChoice ("fx_delay_div", "Delay Div", getSyncDivisionNames(), 3);
    addFloat ("fx_delay_feedback", "Delay Feedback", 0.0f, 0.95f, 0.35f);
    addFloat ("fx_delay_damping", "Delay Damping", 0.0f, 1.0f, 0.3f);
    addBool ("fx_delay_pingpong", "Delay Ping-Pong", false);
    addFloat ("fx_delay_mix", "Delay Mix", 0.0f, 1.0f, 0.3f);
    addFloat ("fx_delay_pitch", "Tape Pitch", -12.0f, 12.0f, 0.0f, 1.0f, 1.0f);
    addFloat ("fx_delay_wow", "Tape Wow", 0.0f, 1.0f, 0.0f);

    addBool ("fx_taps_on", "Taps On", false);
    addChoice ("fx_taps_pattern", "Taps Pattern", { "1/16 x2", "Gallop", "Triplet", "Echo 123", "Stutter 4", "Reverse", "Custom" }, 0);

    for (int step = 1; step <= 16; ++step)
        addFloat ("fx_taps_step" + juce::String (step), "Taps Step " + juce::String (step), 0.0f, 1.0f, 0.0f);
    addFloat ("fx_taps_mix", "Taps Mix", 0.0f, 1.0f, 0.5f);

    addBool ("fx_smear_on", "Smear On", false);
    addFloat ("fx_smear_size", "Smear Size", 20.0f, 300.0f, 120.0f);
    addFloat ("fx_smear_density", "Smear Density", 2.0f, 40.0f, 12.0f);
    addFloat ("fx_smear_mix", "Smear Mix", 0.0f, 1.0f, 0.5f);

    addBool ("fx_freeze_on", "Freeze On", false);
    addFloat ("fx_freeze_mix", "Freeze Mix", 0.0f, 1.0f, 0.8f);

    addBool ("fx_stutter_on", "Stutter On", false);
    addChoice ("fx_stutter_div", "Stutter Div", getSyncDivisionNames(), 4);
    addFloat ("fx_stutter_mix", "Stutter Mix", 0.0f, 1.0f, 1.0f);

    addBool ("fx_comb_on", "Comb On", false);
    addFloat ("fx_comb_freq", "Comb Freq", 20.0f, 2000.0f, 220.0f, 0.3f);
    addFloat ("fx_comb_feedback", "Comb Feedback", 0.0f, 0.97f, 0.6f);
    addFloat ("fx_comb_mix", "Comb Mix", 0.0f, 1.0f, 0.5f);

    addBool ("fx_phaser_on", "Phaser On", false);
    addFloat ("fx_phaser_rate", "Phaser Rate", 0.05f, 8.0f, 0.5f, 0.4f);
    addFloat ("fx_phaser_depth", "Phaser Depth", 0.0f, 1.0f, 0.5f);
    addFloat ("fx_phaser_feedback", "Phaser Feedback", 0.0f, 0.9f, 0.4f);
    addFloat ("fx_phaser_mix", "Phaser Mix", 0.0f, 1.0f, 0.5f);

    addBool ("fx_reverb_on", "Reverb On", false);
    addFloat ("fx_reverb_size", "Reverb Size", 0.0f, 1.0f, 0.5f);
    addFloat ("fx_reverb_damping", "Reverb Damping", 0.0f, 1.0f, 0.5f);
    addFloat ("fx_reverb_width", "Reverb Width", 0.0f, 1.0f, 1.0f);
    addFloat ("fx_reverb_mix", "Reverb Mix", 0.0f, 1.0f, 0.25f);

    addFloat ("master", "Master", -60.0f, 12.0f, -6.0f, 1.0f, 0.1f);
    addBool ("master_clip", "Master Soft Clip", true);
    addFloat ("master_clip_gain", "Clip Gain", 0.0f, 24.0f, 0.0f, 1.0f, 0.1f);
    addBool ("oversampling", "Oversample", false);
    addChoice ("os_factor", "Oversampling Factor", { "2x", "4x" }, 0);

    // M3b additions are appended so all existing parameter IDs and choice
    // indices keep their saved meanings.
    for (int osc = 4; osc <= 6; ++osc)
    {
        const auto prefix = "osc" + juce::String (osc);
        const auto name = "Osc" + juce::String (osc);
        const auto id = [&prefix] (const char* suffix) { return prefix + "_" + suffix; };
        addBool (id ("on"), name + " On", false);
        addChoice (id ("mode"), name + " Mode", oscModeChoices, 0);
        addChoice (id ("table"), name + " Table", getOscTableChoices(), 0);
        addFloat (id ("frame"), name + " Frame", 0.0f, 1.0f, 0.0f);
        addFloat (id ("level"), name + " Level", 0.0f, 1.0f, 0.6f);
        addFloat (id ("pan"), name + " Pan", -1.0f, 1.0f, 0.0f);
        addInt (id ("semi"), name + " Semi", -24, 24, 0);
        addFloat (id ("fine"), name + " Fine", -100.0f, 100.0f, 0.0f, 1.0f, 1.0f);
        addInt (id ("unison"), name + " Unison", 1, VoiceParams::maxUnison, 1);
        addFloat (id ("detune"), name + " Detune", 0.0f, 200.0f, 15.0f, 0.6f);
        addFloat (id ("spread"), name + " Spread", 0.0f, 1.0f, 0.5f);
        addChoice (id ("warp"), name + " Warp", Warp::getNames(), 0);
        addFloat (id ("warp_amt"), name + " Warp Amount", 0.0f, 1.0f, 0.0f);
        addChoice (id ("spectral"), name + " Spectral Warp", SpectralWarp::getNames(), 0);
        addFloat (id ("spectral_amt"), name + " Spectral Amount", 0.0f, 1.0f, 0.5f);
        addChoice (id ("uni_mode"), name + " Unison Mode", UnisonMode::getNames(), 0);
        addFloat (id ("uni_blend"), name + " Unison Blend", 0.0f, 1.0f, 1.0f);
        addFloat (id ("uni_frame"), name + " Unison Frame Spread", 0.0f, 1.0f, 0.0f);
        addFloat (id ("uni_warp"), name + " Unison Warp Spread", 0.0f, 1.0f, 0.0f);
        addChoice (id ("scale"), name + " Scale Quantize", Scales::getNames(), 0);
        addChoice (id ("scale_root"), name + " Scale Root", Scales::getRootNames(), 0);
        addChoice (id ("route"), name + " Filter Route", FilterRoute::getNames(), 0);
        addChoice (id ("chord"), name + " Chord", { "Off", "Octave", "Fifth", "Power", "Major", "Minor", "Sus4" }, 0);
        addChoice (id ("excite"), name + " Excite", { "Burst", "Noise", "Saw", "Pulse", "Bow", "Bright Hammer", "Osc In", "Tine", "Reed", "Piano", "Feedback", "Modal Piano" }, 0);
        addFloat (id ("string_decay"), name + " String Decay", 0.0f, 1.0f, 0.75f);
        addFloat (id ("string_damp"), name + " String Damp", 0.0f, 1.0f, 0.35f);
        addFloat (id ("string_sustain"), name + " String Sustain", 0.0f, 1.0f, 0.0f);
        addFloat (id ("string_stiffness"), name + " Stiffness", 0.0f, 1.0f, 0.0f);
        addFloat (id ("string_pickup"), name + " Pickup", 0.0f, 1.0f, 0.0f);
        addFloat (id ("string_excite_pos"), name + " Excitation Position", 0.0f, 1.0f, 0.0f);
        addFloat (id ("string_pick_hardness"), name + " Pick Hardness", 0.0f, 1.0f, 1.0f);
        addFloat (id ("string_pick_pos"), name + " Pick Position", 0.0f, 1.0f, 0.0f);
        addBool (id ("string_slap"), name + " Slap", false);
        addFloat (id ("string_click"), name + " String Click", 0.0f, 1.0f, 1.0f);
        addFloat (id ("bow_pressure"), name + " Bow Pressure", 0.0f, 1.0f, 0.5f);
        addFloat (id ("bow_speed"), name + " Bow Speed", 0.0f, 1.0f, 0.5f);
        addFloat (id ("bridge_buzz"), name + " Bridge Buzz", 0.0f, 1.0f, 0.0f);
        addFloat (id ("fret_rattle"), name + " Fret Rattle", 0.0f, 1.0f, 0.0f);
        addChoice (id ("sample_factory"), name + " Sample Source",
                   { "User File", "Metal Hit", "Vocal Ah", "Sub Tone", "Vinyl Loop", "Noise Rise" }, 0);
        addBool (id ("sample_tuned"), name + " Sample Tuned", true);
        addBool (id ("sample_loop"), name + " Sample Loop", false);
        addBool (id ("sample_reverse"), name + " Sample Reverse", false);
        addFloat (id ("sample_start"), name + " Sample Start", 0.0f, 1.0f, 0.0f);
        addFloat (id ("sample_end"), name + " Sample End", 0.0f, 1.0f, 1.0f);
        addFloat (id ("sample_fade_in"), name + " Sample Fade In", 0.0f, 1.0f, 0.0f);
        addFloat (id ("sample_fade_out"), name + " Sample Fade Out", 0.0f, 1.0f, 0.0f);
        addFloat (id ("grain_size"), name + " Grain Size", 10.0f, 500.0f, 80.0f, 0.4f);
        addFloat (id ("grain_density"), name + " Grain Density", 0.0f, 1.0f, 0.5f);
        addFloat (id ("grain_spray"), name + " Grain Spray", 0.0f, 1.0f, 0.15f);
        addFloat (id ("grain_pitch"), name + " Grain Pitch Spray", 0.0f, 1.0f, 0.0f);
        addFloat (id ("grain_spread"), name + " Grain Stereo Spread", 0.0f, 1.0f, 0.6f);
        addBool (id ("out"), name + " Output", true);
    }

    for (int source = 1; source <= OscillatorIds::count; ++source)
        for (int target = 1; target <= OscillatorIds::count; ++target)
        {
            if (source <= 3 && target <= 3)
                continue; // all nine original cells keep their parameter IDs

            const auto id = source == target ? "fm_fb" + juce::String (source)
                                              : "fm_" + juce::String (source) + "to" + juce::String (target);
            const auto name = source == target ? "FM Osc" + juce::String (source) + " Feedback"
                                                : "FM Osc" + juce::String (source) + " > Osc" + juce::String (target);
            addFloat (id, name, 0.0f, 1.0f, 0.0f);
        }

    juce::StringArray ampEnvelopeChoices;
    // Labels only; the saved value is the index, so naming the first five is safe.
    for (int env = 1; env <= 16; ++env)
        // Upper case, as the chips and tabs name them (review 7, V7-36).
        ampEnvelopeChoices.add (env == 1 ? "AMP ENV" : env == 2 ? "FILT ENV" : env == 3 ? "FILT 2 ENV"
                                : env == 4 ? "MOD ENV" : "ENV " + juce::String (env));
    ampEnvelopeChoices.add ("MSEG"); // M5: appended, index 16
    ampEnvelopeChoices.add ("OP ENV"); // appended, index 17: the Operator Env (OperatorEgParams.h), upper case as its neighbours
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const auto prefix = juce::String (OscillatorIds::prefixes[(size_t) osc]);
        addChoice (prefix + "_amp_env", "Osc" + juce::String (osc + 1) + " Amp Envelope", ampEnvelopeChoices, 0);
    }
    for (int env = 6; env <= 16; ++env)
    {
        const auto prefix = "env" + juce::String (env);
        const auto name = "ENV " + juce::String (env);
        addFloat (prefix + "_attack", name + " Attack", 0.001f, 5.0f, 0.05f, 0.35f);
        addFloat (prefix + "_decay", name + " Decay", 0.005f, 5.0f, 0.4f, 0.35f);
        addFloat (prefix + "_sustain", name + " Sustain", 0.0f, 1.0f, 0.5f);
        addFloat (prefix + "_release", name + " Release", 0.005f, 10.0f, 0.3f, 0.35f);
        addFloat (prefix + "_curve", name + " Tension", -1.0f, 1.0f, 0.0f);
        addFloat (prefix + "_velocity", name + " Velocity", 0.0f, 1.0f, 0.0f);
    }
    addChoice ("quality", "Quality", { "Eco", "Normal", "High" }, 1);

    // M4 acoustic keys. Everything defaults to off, so old patches are unchanged.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const auto prefix = juce::String (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addFloat (prefix + "_hammer_hard", name + " Hammer Hardness", 0.0f, 1.0f, 0.5f);
        addFloat (prefix + "_couple", name + " String Coupling", 0.0f, 1.0f, 0.0f);
        addFloat (prefix + "_damper", name + " Damper", 0.0f, 1.0f, 0.0f);
        addFloat (prefix + "_register", name + " Register Map", 0.0f, 1.0f, 0.0f);
    }
    addFloat ("stretch", "Stretch Tuning", 0.0f, 1.0f, 0.0f);
    addBool ("sb_on", "Soundboard", false);
    addFloat ("sb_mix", "Soundboard Mix", 0.0f, 1.0f, 0.5f);
    addFloat ("sb_tone", "Soundboard Tone", 0.0f, 1.0f, 0.5f);
    addFloat ("sb_size", "Soundboard Size", 0.0f, 1.0f, 0.5f);
    addFloat ("pedal_res", "Pedal Resonance", 0.0f, 1.0f, 0.0f);
    addFloat ("mech_key", "Key Noise", 0.0f, 1.0f, 0.0f);
    addFloat ("mech_damper", "Damper Noise", 0.0f, 1.0f, 0.0f);
    addFloat ("mech_pedal", "Pedal Noise", 0.0f, 1.0f, 0.0f);

    // M5 deep FM and M6 phase distortion. All default to the old behaviour.
    juce::StringArray warpEnvelopeChoices { "Off" };
    for (int env = 1; env <= 16; ++env)
        warpEnvelopeChoices.add ("ENV " + juce::String (env));
    warpEnvelopeChoices.add ("MSEG");

    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const auto prefix = juce::String (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addChoice (prefix + "_tune", name + " Tuning", OscTuning::getModeNames(), OscTuning::Semitones);
        addFloat (prefix + "_ratio", name + " Ratio", 0.0625f, 32.0f, 1.0f, 0.3f, 0.0001f);
        addChoice (prefix + "_ratio_snap", name + " Ratio Snap", OscTuning::getSnapNames(), 1);
        addFloat (prefix + "_fixed_hz", name + " Fixed Frequency", 1.0f, 20000.0f, 440.0f, 0.25f, 0.01f);
        addFloat (prefix + "_key_level", name + " Key Level Scaling", -1.0f, 1.0f, 0.0f);
        addChoice (prefix + "_fb_type", name + " Feedback Type", FmFeedback::getNames(), FmFeedback::Plain);
        addChoice (prefix + "_warp2", name + " Warp 2", Warp::getStageTwoNames(), 0);
        addFloat (prefix + "_warp2_amt", name + " Warp 2 Amount", 0.0f, 1.0f, 0.0f);
        addChoice (prefix + "_pd_env", name + " Warp Envelope", warpEnvelopeChoices, 0);
        addFloat (prefix + "_pd_env_amt", name + " Warp Envelope Amount", -1.0f, 1.0f, 1.0f);
    }

    for (int osc = 1; osc <= OscillatorIds::count; ++osc)
        addFloat ("fm_noise" + juce::String (osc), "FM Noise > Osc" + juce::String (osc), 0.0f, 1.0f, 0.0f);
    addFloat ("fm_noise_color", "FM Noise Colour", 0.0f, 1.0f, 1.0f);

    for (int env = 0; env < 16; ++env)
    {
        const auto prefix = envelopePrefix (env);
        const auto name = "ENV " + juce::String (env + 1);
        addFloat (prefix + "_delay", name + " Delay", 0.0f, 5.0f, 0.0f, 0.35f);
        addFloat (prefix + "_hold", name + " Hold", 0.0f, 5.0f, 0.0f, 0.35f);
        addFloat (prefix + "_keyrate", name + " Key Rate Scaling", 0.0f, 1.0f, 0.0f);
    }

    // M6b: mod slots 33-64, appended so slots 1-32 keep their parameter IDs.
    for (int slot = legacyModSlots + 1; slot <= Mod::maxSlots; ++slot)
        addModSlotParameters (slot);

    // Velocity for ENV 3-5, so every envelope has one. 0 keeps old patches.
    for (const auto* prefix : { "f2e", "me", "e4" })
        addFloat (juce::String (prefix) + "_velocity", "ENV " + juce::String (prefix[0] == 'f' ? 3 : prefix[0] == 'm' ? 4 : 5)
                                                           + " Velocity", 0.0f, 1.0f, 0.0f);

    // M7.1: the Generative card. Euclidean rhythm, probability sequencer and
    // strum. All off by default, so old patches play as before.
    addBool ("euc_on", "Euclid On", false);
    addChoice ("euc_target", "Euclid Target", { "Notes", "Exciter", "Trance Gate" }, 0);
    addInt ("euc_steps", "Euclid Steps", 2, 32, 16);
    addInt ("euc_hits", "Euclid Hits", 0, 32, 5);
    addInt ("euc_rotate", "Euclid Rotate", 0, 31, 0);
    addChoice ("euc_div", "Euclid Rate", getSyncDivisionNames(), 4);
    addFloat ("euc_gate", "Euclid Gate", 0.05f, 1.0f, 0.5f);

    addBool ("pseq_on", "Prob Seq On", false);
    addChoice ("pseq_div", "Prob Seq Rate", getSyncDivisionNames(), 4);
    addInt ("pseq_length", "Prob Seq Length", 1, 16, 16);
    addFloat ("pseq_gate", "Prob Seq Gate", 0.05f, 1.0f, 0.5f);

    for (int step = 1; step <= 16; ++step)
    {
        const auto n = juce::String (step);
        addFloat ("pseq_chance" + n, "Prob Seq Chance " + n, 0.0f, 1.0f, 1.0f);
        addInt ("pseq_range" + n, "Prob Seq Range " + n, 0, 24, 0);
        addInt ("pseq_ratchet" + n, "Prob Seq Ratchet " + n, 1, 4, 1);
    }

    addChoice ("spray_strum", "Strum", { "Off", "Up", "Down" }, 0);
    addFloat ("spray_strum_time", "Strum Time", 2.0f, 250.0f, 30.0f, 0.5f);

    // M7.5 audio input (ilanaSynth FX). Everything defaults to off, and the
    // instrument has no input, so it sounds as before.
    addFloat ("in_gain", "Input Gain", -24.0f, 24.0f, 0.0f, 1.0f, 0.1f);
    addFloat ("in_dry", "Input Dry", 0.0f, 1.0f, 0.0f);
    addFloat ("in_body", "Input to Body", 0.0f, 1.0f, 0.0f);
    addFloat ("in_strings", "Input to Strings", 0.0f, 1.0f, 0.0f);
    addChoice ("in_trigger", "Input Trigger", { "Off", "Gate", "Drone" }, 0);
    addFloat ("in_threshold", "Input Threshold", -60.0f, 0.0f, -30.0f, 1.0f, 0.1f);
    addInt ("in_note", "Input Note", 24, 96, 48);
    addFloat ("in_attack", "Input Env Attack", 0.1f, 200.0f, 5.0f, 0.4f);
    addFloat ("in_release", "Input Env Release", 5.0f, 2000.0f, 150.0f, 0.4f);
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
        addBool (juce::String (OscillatorIds::prefixes[(size_t) osc]) + "_grain_live",
                 "Osc" + juce::String (osc + 1) + " Live Grains", false);

    // M7.3 electric pianos: the pickup of the Tine and Reed excites.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const auto prefix = juce::String (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addFloat (prefix + "_ep_distance", name + " Pickup Distance", 0.0f, 1.0f, 0.5f);
        addFloat (prefix + "_ep_position", name + " Pickup Offset", 0.0f, 1.0f, 0.5f);
    }

    // M8.1: the simulated LFO shapes' named parameters (mapped per shape,
    // see LfoSimInfo), SMOOTH for every shape, triggers and output B.
    // SMOOTH 0 and the rest only affect the new shapes, so old patches are
    // unchanged.
    for (int lfo = 1; lfo <= numLfos; ++lfo)
    {
        const auto prefix = "lfo" + juce::String (lfo);
        const auto name = "LFO" + juce::String (lfo);
        for (int param = 1; param <= LfoSimInfo::numParams; ++param)
            addFloat (prefix + "_p" + juce::String (param), name + " Param " + juce::String (param), 0.0f, 1.0f, 0.5f);
        addFloat (prefix + "_smooth", name + " Smooth", 0.0f, 1.0f, 0.0f);
        addChoice (prefix + "_axis", name + " Output", { "X", "Y", "Z", "Mix" }, 0);
        addChoice (prefix + "_trigger", name + " Trigger", { "Note", "Free", "Beat", "Generative" }, 0);
        addBool (prefix + "_loop", name + " Loop", false);
        addInt (prefix + "_seed", name + " Seed", 0, 999, 0);
        addFloat (prefix + "_stereo", name + " Stereo", 0.0f, 1.0f, 0.0f);
        addBool (prefix + "_fire", name + " Fire", false);
    }

    // M8.2: the soundboard model. Classic keeps old patches as they were.
    addChoice ("sb_model", "Soundboard Model", { "Classic", "Dense" }, 0);

    // M8.5: the Feedback exciter's amp (GAIN) and how far it stands from
    // the guitar (DISTANCE); FEEDBACK is the string's SUSTAIN.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const auto prefix = juce::String (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addFloat (prefix + "_fb_gain", name + " Feedback Gain", 0.0f, 1.0f, 0.5f);
        addFloat (prefix + "_fb_distance", name + " Feedback Distance", 0.0f, 1.0f, 0.5f);
    }

    // M8.5: Evolve (each macro drifts within a range) and the vector pad.
    for (int macro = 1; macro <= 4; ++macro)
    {
        addFloat ("macro" + juce::String (macro) + "_evolve", "Macro " + juce::String (macro) + " Evolve", 0.0f, 1.0f, 0.0f);
        addFloat ("macro" + juce::String (macro) + "_evolve_rate", "Macro " + juce::String (macro) + " Evolve Rate", 0.01f, 2.0f, 0.1f, 0.4f);
    }
    addBool ("vec_on", "Vector On", false);
    addFloat ("vec_x", "Vector X", 0.0f, 1.0f, 0.5f);
    addFloat ("vec_y", "Vector Y", 0.0f, 1.0f, 0.5f);
    {
        const juce::StringArray oscillators { "OSC 1", "OSC 2", "OSC 3", "OSC 4", "OSC 5", "OSC 6" };
        const char* corners[] { "a", "b", "c", "d" };
        for (int c = 0; c < 4; ++c)
            addChoice (juce::String ("vec_") + corners[c], "Vector Corner " + juce::String::charToString ((juce::juce_wchar) ('A' + c)), oscillators, c);
    }
    addBool ("vec_path", "Vector Path", false);
    addFloat ("vec_rate", "Vector Path Rate", 0.01f, 10.0f, 0.2f, 0.3f);
    for (int point = 1; point <= numVectorPoints; ++point)
    {
        const auto angle = juce::MathConstants<double>::twoPi * (point - 1) / numVectorPoints;
        addFloat ("vec_px" + juce::String (point), "Vector Path X" + juce::String (point), 0.0f, 1.0f, (float) (0.5 + 0.35 * std::cos (angle)));
        addFloat ("vec_py" + juce::String (point), "Vector Path Y" + juce::String (point), 0.0f, 1.0f, (float) (0.5 + 0.35 * std::sin (angle)));
    }
    addFloat ("vec_drift", "Vector Drift", 0.0f, 1.0f, 0.0f);
    addFloat ("vec_drift_rate", "Vector Drift Rate", 0.01f, 2.0f, 0.1f, 0.4f);

    // M8.3: the west-coast voice (off by default).
    addBool ("west_on", "West On", false);
    addChoice ("west_pos", "West Position", { "After Filters", "Replace Filter 2" }, 0);
    addFloat ("west_fold", "West Fold", 0.0f, 1.0f, 0.3f);
    addFloat ("west_sym", "West Symmetry", -1.0f, 1.0f, 0.0f);
    addInt ("west_stages", "West Stages", 1, 4, 2);
    addChoice ("west_mode", "West Gate Mode", { "Combo", "Low Pass", "VCA" }, 0);
    addFloat ("west_decay", "West Decay", 0.1f, 4.0f, 1.0f, 0.5f);
    addFloat ("west_res", "West Resonance", 0.0f, 1.0f, 0.2f);
    addFloat ("west_strike", "West Strike", 0.0f, 1.0f, 1.0f);
    addFloat ("west_open", "West Open", 0.0f, 1.0f, 0.0f);
    {
        juce::StringArray sources { "Note Strike" };
        const auto names = Mod::getSourceNames();
        for (int i = 1; i < names.size(); ++i)
            sources.add (names[i]);
        addChoice ("west_src", "West Strike Source", sources, 0);
    }

    // Scala microtuning (the .scl/.kbm text is saved in the patch; off is 12-TET).
    addBool ("tuning_on", "Tuning On", false);

    // Macros 5-8 and their Evolve (appended; 1-4 keep their place).
    for (int macro = 5; macro <= Mod::numMacros; ++macro)
        addFloat ("macro" + juce::String (macro), "Macro " + juce::String (macro), 0.0f, 1.0f, 0.0f);
    for (int macro = 5; macro <= Mod::numMacros; ++macro)
    {
        addFloat ("macro" + juce::String (macro) + "_evolve", "Macro " + juce::String (macro) + " Evolve", 0.0f, 1.0f, 0.0f);
        addFloat ("macro" + juce::String (macro) + "_evolve_rate", "Macro " + juce::String (macro) + " Evolve Rate", 0.01f, 2.0f, 0.1f, 0.4f);
    }

    // The Airwindows FX module (type 30): Chris Johnson's algorithms, the
    // choice in src/dsp/airwindows/Registry.h's order (append only); five
    // knobs mapped onto the chosen algorithm's own, at the first one's defaults.
    {
        juce::StringArray names;
        for (const auto& info : airwindows::registry())
            names.add (info.name);
        addChoice ("fx_aw_algo", "Airwindows Algorithm", names, 0);
        const auto& first = airwindows::registry().front();
        for (int knob = 0; knob < airwindows::Module::numKnobs; ++knob)
            addFloat ("fx_aw_p" + juce::String (knob + 1), "Airwindows " + juce::String (knob + 1), 0.0f, 1.0f,
                      knob < first.numKnobs ? first.knobs[knob].defaultValue : 0.5f);
        addFloat ("fx_aw_mix", "Airwindows Mix", 0.0f, 1.0f, 1.0f);
    }

    // The Airwindows category modules (types 32-41, airwindows/Categories.h):
    // each picks from its own few effects; knobs default to its first one's.
    for (const auto& category : airwindows::categoryModules())
    {
        const juce::String prefix = juce::String ("fx_") + category.id;
        const juce::String name (category.label);
        juce::StringArray names;
        for (auto algorithm : category.algorithms)
            names.add (airwindows::registry()[(size_t) algorithm].name);
        addChoice (prefix + "_algo", name + " Effect", names, 0);
        const auto& first = airwindows::registry()[(size_t) category.algorithms.front()];
        for (int knob = 0; knob < airwindows::Module::numKnobs; ++knob)
            addFloat (prefix + "_p" + juce::String (knob + 1), name + " " + juce::String (knob + 1), 0.0f, 1.0f,
                      knob < first.numKnobs ? first.knobs[knob].defaultValue : 0.5f);
        addFloat (prefix + "_mix", name + " Mix", 0.0f, 1.0f, 1.0f);
    }

    // FX splitters: which part of the signal each slot works on (Full keeps
    // the whole signal, as before), and the two crossover frequencies.
    for (int slot = 0; slot < numFxSlots; ++slot)
        addChoice ("fx_slot" + juce::String (slot + 1) + "_band", "FX Band " + juce::String (slot + 1),
                   { "Full", "Low", "Mid", "High", "Mid (M/S)", "Side (M/S)" }, 0);
    addFloat ("fx_split_low", "FX Split Low", 40.0f, 2000.0f, 250.0f, 0.4f);
    addFloat ("fx_split_high", "FX Split High", 400.0f, 12000.0f, 2500.0f, 0.4f);

    // The vocoder (FX type 31): the carrier is the signal in the slot, the
    // modulator the audio input (FX plugin) or, without one, an internal TALK.
    addChoice ("fx_voc_source", "Vocoder Modulator", { "Auto", "Input", "Talk" }, 0);
    addInt ("fx_voc_bands", "Vocoder Bands", 8, 24, 16);
    addFloat ("fx_voc_width", "Vocoder Width", 0.25f, 4.0f, 1.0f, 0.5f);
    addFloat ("fx_voc_attack", "Vocoder Attack", 0.5f, 50.0f, 5.0f, 0.4f);
    addFloat ("fx_voc_release", "Vocoder Release", 5.0f, 500.0f, 60.0f, 0.4f);
    addFloat ("fx_voc_formant", "Vocoder Formant", -12.0f, 12.0f, 0.0f);
    addFloat ("fx_voc_unvoiced", "Vocoder Unvoiced", 0.0f, 1.0f, 0.3f);
    addFloat ("fx_voc_rate", "Vocoder Talk Rate", 0.1f, 8.0f, 1.0f, 0.5f);
    addFloat ("fx_voc_level", "Vocoder Level", -12.0f, 12.0f, 0.0f);
    addFloat ("fx_voc_mix", "Vocoder Mix", 0.0f, 1.0f, 1.0f);

    // Clip sequencer (the notes are saved in the patch, see ClipState).
    addBool ("clip_on", "Clip On", false);
    addChoice ("clip_index", "Clip", { "1", "2", "3", "4", "5", "6", "7", "8" }, 0);
    addChoice ("clip_mode", "Clip Mode", { "Key transpose", "Host play" }, 0);

    // The Operator EG (an oscillator's ENVELOPE choice 17): the DX7's
    // envelope generator, scaling and sensitivities per operator, and its
    // pitch envelope and LFO for the voice. Defaults are the DX7 init voice.
    const auto addEgField = [&] (const OperatorEg::Field& field, const juce::String& id, const juce::String& name)
    {
        if (field.choice)
        {
            juce::StringArray names;
            if (field.maximum == 3)
                for (const auto* curve : OperatorEg::curveNames)
                    names.add (curve);
            else
                for (const auto* wave : OperatorEg::lfoWaveNames)
                    names.add (wave);
            addChoice (id, name, names, field.defaultValue);
        }
        else if (field.maximum == 1)
            addBool (id, name, field.defaultValue != 0);
        else
            addInt (id, name, 0, field.maximum, field.defaultValue);
    };
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
        for (const auto& field : OperatorEg::operatorFields())
            addEgField (field, juce::String (OscillatorIds::prefixes[(size_t) osc]) + field.suffix,
                        "Osc" + juce::String (osc + 1) + " " + field.name);
    for (const auto& field : OperatorEg::voiceFields())
        addEgField (field, field.suffix, field.name);
    addInt (OperatorEg::keyOffsetId, "OP ENV Scale Shift", -24, 24, 0);

    // Review 6 (appended): the DX7 algorithm the FM routing came from (a
    // .syx voice, or a click on the grid's DX7 page), 0 for none. Display
    // only: DX7 algorithms 1-2, 3-4 and 5-6 share a routing here and differ
    // only in where the feedback goes, so this names the one it was.
    {
        juce::StringArray algorithmNames { "None" };
        for (int algorithm = 1; algorithm <= 32; ++algorithm)
            algorithmNames.add ("DX7 " + juce::String (algorithm));
        addChoice (OperatorEg::dx7AlgorithmId, "DX7 Algorithm", algorithmNames, 0);
    }

    // The arp's step lanes (review 6): how many steps loop, and each step's
    // velocity, gate (a share of GATE; 0 rests) and transpose. The defaults
    // play every step as before.
    addInt ("arp_steps", "Arp Steps", 1, 16, 16);
    for (int step = 1; step <= 16; ++step)
    {
        const auto n = juce::String (step);
        addInt ("arp_vel" + n, "Arp Velocity " + n, 1, 127, 100);
        addFloat ("arp_len" + n, "Arp Step Gate " + n, 0.0f, 2.0f, 1.0f);
        addInt ("arp_pitch" + n, "Arp Transpose " + n, -12, 12, 0);
    }

    // The preset's own output level (review 6, master level): factory loads
    // put their level here so MASTER reads 0 dB on every factory preset.
    // Added to MASTER in dB; not shown on any page, not automatable, and 0
    // (as in every older patch) leaves the output as it was.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output_trim", 1 }, "Preset Level", juce::NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float value, int) { return describeValue ("output_trim", value); })
            .withAutomatable (false)));

    // UI review 6 (V5-18): each envelope's attack, decay and release bend on
    // their own, added to its CURVE. 0 bends them alike, as before.
    for (int env = 0; env < 16; ++env)
    {
        const auto prefix = envelopePrefix (env);
        const auto name = "ENV " + juce::String (env + 1);
        addFloat (prefix + "_acurve", name + " Attack Curve", -1.0f, 1.0f, 0.0f);
        addFloat (prefix + "_dcurve", name + " Decay Curve", -1.0f, 1.0f, 0.0f);
        addFloat (prefix + "_rcurve", name + " Release Curve", -1.0f, 1.0f, 0.0f);
    }

    // Review 7 (I7-4): the reverb leaves the dry signal as it is and adds its
    // wet on top, so a reverb at MIX 0 passes the dry signal exactly (the DX7
    // voices' SPACE macro opens it). Off, as in every older patch: the dry
    // follows MIX as before.
    addBool ("fx_reverb_keep_dry", "Reverb Keep Dry", false);

    // Review 8 (V8-15): the heard noise's colour (SUB + NOISE), on the FM
    // noise's scale. White, as in every older patch.
    addFloat ("noise_color", "Noise Colour", 0.0f, 1.0f, 1.0f);

    // Design round 2: the FX rack's routing. SERIES (each effect feeds the
    // next one) as in every older patch; PARALLEL (every effect hears the
    // rack's input, their outputs averaged; see processEffectsParallel).
    addChoice ("fx_routing", "FX Routing", { "Series", "Parallel" }, 0);

    // CPU round 7: the voices render on several cores (the same sound; off
    // renders them on one, as before).
    addBool ("multi_core", "Multi-Core Voices", true);

    // At most this many voices ring on after their key is up (released or
    // held by the pedal); past it the oldest fade out. Off as before.
    addChoice ("sustain_voices", "Sustain Voices", { "Off", "4", "6", "8", "12", "16" }, 0);

    // Vital parity: a stereo route into a filter cutoff moves the left and
    // right channels in opposite directions. Off, as in every older patch.
    for (int slot = 1; slot <= Mod::maxSlots; ++slot)
        addBool ("mod" + juce::String (slot) + "_stereo", "Mod" + juce::String (slot) + " Stereo", false);

    // The compact SUB and NOISE cards on PLAY: NOISE has its own switch (on,
    // as before: its level alone decided) and six colours (White is the old
    // noise), and SUB has its own COLOUR (0, the plain wave, as before; the
    // old noise_color stays as the noise's tone control, now off the card).
    addBool ("noise_on", "Noise On", true);
    addChoice ("noise_type", "Noise Type", { "White", "Pink", "Brown", "Blue", "Violet", "Grey" }, 0);
    addFloat ("sub_colour", "Sub Colour", 0.0f, 1.0f, 0.0f);

    // Roadmap step 17: the FX PATCHER. On, the rack's slots run as the nodes
    // of the patcher's cable graph (FxPatch.h) instead of SERIES / PARALLEL.
    // Off, as in every older patch.
    addBool ("fx_patcher", "FX Patcher", false);

    // Roadmap step 16: reroutable oscillators. On, the FILTER tab's ROUTING
    // cables decide where each source goes (F1, F2, WEST, OUT, any mix) and
    // in which order the stages run (dsp/VoiceRouting.h); off, as in every
    // older patch, the fixed chain (the _route menus, SERIAL / PARALLEL,
    // WEST's position). Each cable_* holds one source's or stage's
    // destinations as a mask; the defaults are today's default chain.
    addBool ("route_cables", "Routing Cables", false);
    {
        juce::StringArray masks;
        for (int mask = 0; mask < 32; ++mask)
            masks.add (VoiceRouting::maskNames()[mask]);
        const auto defaults = VoiceRouting::defaultPatch();
        for (int source = 0; source < VoiceRouting::numSources; ++source)
            addChoice (VoiceRouting::cableParamId (source), VoiceRouting::cableParamName (source), masks,
                       VoiceRouting::toParamMask (defaults.source[(size_t) source]));
        // (BODY's cable is appended at the end of the layout.)
        for (int stage = 0; stage < VoiceRouting::body; ++stage)
            addChoice (VoiceRouting::cableParamId (VoiceRouting::numSources + stage), VoiceRouting::cableParamName (VoiceRouting::numSources + stage),
                       masks, VoiceRouting::toParamMask (defaults.stage[(size_t) stage]));
    }

    // FX type 42, FILTER (ilana 2026-10-11: feed an effect back into a
    // filter): any of the voice filters (FilterType, dsp/FilterUnit.h) run
    // once on the rack's stereo signal, so a PATCH loop can go through one.
    // SPREAD opens the left side and closes the right, up to an octave apart.
    addChoice ("fx_filter_type", "Filter FX Type", FilterType::getNames(), FilterType::LowPass);
    addChoice ("fx_filter_slope", "Filter FX Slope", { "12 dB", "24 dB" }, 0);
    addFloat ("fx_filter_cutoff", "Filter FX Cutoff", 20.0f, 20000.0f, 2000.0f, 0.25f);
    addFloat ("fx_filter_reso", "Filter FX Reso", 0.0f, 1.0f, 0.3f);
    addFloat ("fx_filter_drive", "Filter FX Drive", 1.0f, 10.0f, 1.0f);
    addFloat ("fx_filter_morph", "Filter FX Morph", 0.0f, 1.0f, 0.0f);
    addFloat ("fx_filter_spread", "Filter FX Spread", 0.0f, 1.0f, 0.0f);

    // Roadmap step 40: a sample follows the host tempo. SYNC names the
    // sample's own tempo: its BPM, or its played region's length in beats or
    // bars (4/4); STRETCH keeps the pitch apart from the speed (Repitch: one
    // rate, as before). Off, as in every older patch.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addChoice (prefix + "_sample_sync", name + " Sample Sync", SampleTempo::syncNames(), 0);
        addFloat (prefix + "_sample_bpm", name + " Sample BPM", 40.0f, 240.0f, 120.0f);
        addChoice (prefix + "_sample_stretch", name + " Sample Time", { "Repitch", "Stretch", "Signalsmith", "Bungee", "PGHI", "Paulstretch" }, 0);
    }

    // Roadmap step 27: independent loop points. LOOP PTS gives LOOP its own
    // span (fractions of the sample, held inside START..END): the attack
    // plays once and the span repeats. Off, as in every older patch, LOOP
    // wraps the whole played range.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addBool (prefix + "_sample_loop_own", name + " Sample Loop Points", false);
        addFloat (prefix + "_sample_loop_start", name + " Sample Loop Start", 0.0f, 1.0f, 0.25f);
        addFloat (prefix + "_sample_loop_end", name + " Sample Loop End", 0.0f, 1.0f, 0.75f);
        // Roadmap step 28: the loop crossfade (0 = hard wrap, as in every older
        // patch; 1 = half the loop) and the zero-crossing snap for the markers.
        addFloat (prefix + "_sample_loop_xfade", name + " Loop Xfade", 0.0f, 1.0f, 0.0f);
        addBool (prefix + "_sample_snap", name + " Sample Snap To Zero", false);
    }

    // Roadmap step 29: the note a plain sample was recorded at (a MIDI note, C3 = 60 as on the keyboard strip). 60 is what every older
    // patch assumed, so they play as before. A multisample keeps its zones' roots.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
        addFloat (juce::String (OscillatorIds::prefixes[(size_t) osc]) + "_sample_root",
                  "Osc" + juce::String (osc + 1) + " Sample Root Note", 0.0f, 127.0f, 60.0f, 1.0f, 1.0f);

    // Review (E-7): a MIDI program change no longer replaces the patch
    // unless asked (it loaded a factory preset, silently, before).
    addBool ("midi_program_change", "MIDI Program Change Loads Presets", false);

    // The NOISE FM operator's noise: Colour is the old one-pole noise
    // (FM NOISE COLOUR sets it, every older patch); the others are the NOISE
    // card's six colours as FM noise (colour then has no say).
    addChoice ("fm_noise_type", "FM Noise Type", { "Colour", "White", "Pink", "Brown", "Blue", "Violet", "Grey" }, 0);

    // Roadmap step 45: granular controls (append only). The grain position
    // is its own parameter, apart from Sample Start; patches saved before it
    // take Sample Start's value on load. SCAN moves the position through the
    // sample (1 = the sample's own speed, negative = backwards); FREEZE holds
    // it where it is.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const auto prefix = juce::String (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addFloat (prefix + "_grain_pos", name + " Grain Position", 0.0f, 1.0f, 0.0f);
        addFloat (prefix + "_grain_scan", name + " Grain Scan", -2.0f, 2.0f, 0.0f);
        addBool (prefix + "_grain_freeze", name + " Grain Freeze", false);
    }

    // Roadmap step 33: how a sample treats the key's release. Gate, as in
    // every older patch: LOOP decides and the envelope ends the note. One
    // Shot plays START to END once and keeps playing after the release.
    // Sustain Loop loops while the key is down, then plays on to END.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addChoice (prefix + "_sample_playback", name + " Sample Playback", { "Gate", "One Shot", "Sustain Loop" }, 0);
    }

    // Roadmap step 34: the loop runs forward and back instead of jumping to
    // its start. Off, as in every older patch.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addBool (prefix + "_sample_pingpong", name + " Ping-Pong", false);
    }

    // Roadmap step 35: each note starts at a random place up to this far
    // into the played range; TRANSIENT SAFE moves it back onto the attack
    // before it. 0, as in every older patch.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addFloat (prefix + "_sample_start_rand", name + " Random Start", 0.0f, 1.0f, 0.0f);
        addBool (prefix + "_sample_start_safe", name + " Transient Safe", false);
    }

    // Roadmap step 39: how the sample is read between its samples. Linear,
    // as in every older patch; Sinc + Anti-Alias filters what a strong
    // upward transposition would fold back.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addChoice (prefix + "_sample_quality", name + " Sample Quality", { "Linear", "Cubic", "Sinc", "Sinc + Anti-Alias" }, 0);
    }

    // Roadmap step 36: a key no zone covers plays the nearest zone, as in
    // every older patch, or nothing.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addChoice (prefix + "_sample_unmapped", name + " Outside Zones", { "Nearest Zone", "Silence" }, 0);
    }

    // Roadmap step 37: alternates for the same key take turns (Cycle) or
    // are drawn at random; VELOCITY BLEND crossfades neighbouring velocity
    // layers. Off and 0, as in every older patch.
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
    {
        const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
        const auto name = "Osc" + juce::String (osc + 1);
        addChoice (prefix + "_sample_round_robin", name + " Round Robin", { "Off", "Cycle", "Random" }, 0);
        addFloat (prefix + "_sample_vel_blend", name + " Velocity Blend", 0.0f, 1.0f, 0.0f);
    }

    // The physical body as a stage of the ROUTING cables (dsp/VoiceRouting.h):
    // where it goes. Nothing cabled into it, it is the body of every older patch.
    {
        juce::StringArray masks;
        for (int mask = 0; mask < 32; ++mask)
            masks.add (VoiceRouting::maskNames()[mask]);
        addChoice (VoiceRouting::cableParamId (VoiceRouting::numSources + VoiceRouting::body),
                   VoiceRouting::cableParamName (VoiceRouting::numSources + VoiceRouting::body), masks,
                   VoiceRouting::toParamMask (VoiceRouting::defaultPatch().stage[(size_t) VoiceRouting::body]));
    }

    // AW Delay (FX type 35) to the host's tempo, as the main Delay's SYNC and
    // DIV (airwindows/DelaySync.h). Off, as in every older patch.
    addBool ("fx_awdelay_sync", "AW Delay Sync", false);
    addChoice ("fx_awdelay_div", "AW Delay Div", getSyncDivisionNames(), 3);

    // FRAME BLEND: how a wavetable reads between two frames as its frame
    // position moves (FrameBlend in dsp/Wavetable.h). Crossfade, as in every
    // older patch.
    {
        juce::StringArray blends;
        for (const auto* blendName : FrameBlend::names)
            blends.add (blendName);

        for (int osc = 0; osc < OscillatorIds::count; ++osc)
            addChoice (juce::String (OscillatorIds::prefixes[(size_t) osc]) + "_frame_blend",
                       "Osc" + juce::String (osc + 1) + " Frame Blend", blends, FrameBlend::crossfade);
    }

    // OSC 4-6's STRING SPLIT (OSC 1-3 had theirs; a swap of oscillators
    // would have lost it, ilana 2026-10-26). Zero, the plain string, as their
    // voices read it before.
    for (int osc = 4; osc <= 6; ++osc)
        addFloat ("osc" + juce::String (osc) + "_string_split", "Osc" + juce::String (osc) + " String Split", 0.0f, 1.0f, 0.0f);

    return layout;
}
