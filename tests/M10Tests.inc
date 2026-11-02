// M10: the preset pack's plumbing: 80 more factory tables (and the patch
// migration that moves User table choices past them), choices read back
// exactly, and presets that bounce another preset at load. Included by
// TableTest.cpp.

namespace M10
{
float rawValue (IlanaSynthAudioProcessor& processor, const juce::String& id)
{
    if (const auto* value = processor.apvts.getRawParameterValue (id))
        return value->load();
    return -1.0f;
}

// A state saved before v1.3 (tableSchema 2) with the given table choices.
std::unique_ptr<IlanaSynthAudioProcessor> loadSchema2 (const std::vector<std::pair<juce::String, float>>& tables, int schema = 2)
{
    IlanaSynthAudioProcessor source;
    juce::MemoryBlock block;
    source.getStateInformation (block);
    std::unique_ptr<juce::XmlElement> xml (juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize()));
    auto state = juce::ValueTree::fromXml (*xml);
    state.setProperty ("tableSchema", schema, nullptr);
    for (const auto& [id, value] : tables)
        for (int i = 0; i < state.getNumChildren(); ++i)
            if (state.getChild (i).getProperty ("id").toString() == id)
                state.getChild (i).setProperty ("value", value, nullptr);
    juce::MemoryBlock data;
    std::unique_ptr<juce::XmlElement> out (state.createXml());
    juce::AudioProcessor::copyXmlToBinary (*out, data);
    auto target = std::make_unique<IlanaSynthAudioProcessor>();
    target->setStateInformation (data.getData(), (int) data.getSize());
    return target;
}

void runPresetIdTest()
{
    IlanaSynthAudioProcessor processor;
    juce::StringArray unknown;
    for (const auto& preset : Presets::getFactoryPresets())
        for (const auto& value : preset.values)
            if (processor.apvts.getParameter (value.id) == nullptr)
                unknown.addIfNotAlreadyThere (juce::String (preset.name) + ": " + value.id);
    check (unknown.isEmpty(), "every factory preset value names a real parameter" + (unknown.isEmpty() ? juce::String() : " (" + unknown.joinIntoString (", ") + ")"));
}

// The resampled presets bounce another preset when they load.
void runBouncePresetTest()
{
    IlanaSynthAudioProcessor processor;
    const auto names = processor.getFactoryPresetNames();
    processor.loadFactoryPreset (names.indexOf ("Bounced Bell Grains"));
    const auto* sample = processor.getSampleForOsc (0);
    check (processor.isSampleEmbedded (0) && sample != nullptr && sample->getNumSamples() > 48000
               && juce::roundToInt (rawValue (processor, "osc1_mode")) == 3,
           "Bounced Bell Grains loads with Church Bell Shell bounced onto OSC 1, played as grains");

    processor.loadFactoryPreset (names.indexOf ("Tabled Cello"));
    const auto choice = juce::roundToInt (rawValue (processor, "osc1_table"));
    const auto slot = choice - TableFactory::getNumFactoryTables();
    check (slot >= 0 && processor.isUserSlotEdited (slot) && juce::roundToInt (rawValue (processor, "osc1_mode")) == 0,
           "Tabled Cello loads with Cello Section cut into a patch wavetable");

    // Loading a plain preset afterwards clears the bounce.
    processor.loadFactoryPreset (names.indexOf ("Bounced Bell Grains"));
    processor.loadFactoryPreset (0);
    processor.flushAsyncUpdates();
    check (! processor.isSampleEmbedded (0), "a plain preset loaded after a resampled one drops its bounce");
}

// Every parameter ID once (pluginval found osc4-6_out declared twice: a
// host sees two parameters with one ID and state restore sets only one).
void runUniqueIdTest()
{
    IlanaSynthAudioProcessor processor;
    std::set<juce::String> seen;
    juce::StringArray duplicates;
    for (auto* parameter : processor.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
            if (! seen.insert (withId->paramID).second)
                duplicates.add (withId->paramID);
    check (duplicates.isEmpty(), "every parameter ID is declared once" + (duplicates.isEmpty() ? juce::String() : " (" + duplicates.joinIntoString (", ") + ")"));

    // And each restores from a saved state.
    IlanaSynthAudioProcessor source;
    juce::Random random (9);
    for (auto* parameter : source.getParameters())
        parameter->setValueNotifyingHost (random.nextFloat());
    // (A random Wavetable tuned by ratio or on OP ENV is an operator from
    // before the FM / DX7 type: it loads as FM / DX7, the same sound. Made
    // so here, so the round trip compares like with like.)
    source.migrateOperatorModes();
    juce::MemoryBlock state;
    source.getStateInformation (state);
    IlanaSynthAudioProcessor target;
    target.setStateInformation (state.getData(), (int) state.getSize());
    juce::StringArray wrong;
    for (int i = 0; i < source.getParameters().size(); ++i)
        if (source.getParameters()[i]->getCurrentValueAsText() != target.getParameters()[i]->getCurrentValueAsText())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (target.getParameters()[i]))
                wrong.add (withId->paramID);
    check (wrong.isEmpty(), "every parameter comes back from a saved state" + (wrong.isEmpty() ? juce::String() : " (" + wrong.joinIntoString (", ") + ")"));
}

// A value shown as text and typed back comes back as the same text (hosts
// let you type values; the CLAP validator checks this for every parameter).
void runTextRoundTripTest()
{
    IlanaSynthAudioProcessor processor;
    juce::Random random (10);
    juce::StringArray wrong;
    for (auto* parameter : processor.getParameters())
        for (int trial = 0; trial < 400; ++trial)
        {
            const auto value = trial == 0 ? 0.0f : trial == 1 ? 1.0f : random.nextFloat();
            const auto text = parameter->getText (value, 1024);
            const auto back = parameter->getText (parameter->getValueForText (text), 1024);
            if (back != text)
            {
                wrong.add (parameter->getName (64) + ": '" + text + "' -> '" + back + "'");
                break;
            }
        }
    check (wrong.isEmpty(), "every parameter's text reads back as the same value" + (wrong.isEmpty() ? juce::String()
               : " (" + juce::String (wrong.size()) + " wrong, e.g. " + wrong.joinIntoString ("; ").substring (0, 600) + ")"));
}

// A typed number past a knob's ends clamps to the nearer end in the knob's
// own unit (ilana typed 99000 on CUTOFF and got 990 Hz), and a bare number
// or a typed unit still reads as before.
void runTypedValueTest()
{
    IlanaSynthAudioProcessor processor;
    auto& state = processor.apvts;
    const auto typed = [&state] (const juce::String& id, const juce::String& text)
    {
        auto* parameter = state.getParameter (id);
        return parameter->getText (parameter->getValueForText (text), 1024);
    };
    const auto expect = [&typed] (const juce::String& id, const juce::String& text, const juce::String& shown)
    {
        const auto got = typed (id, text);
        check (got == shown, id + ": typed '" + text + "' sets " + shown + " (got " + got + ")");
    };
    const auto top = [&state] (const juce::String& id) { auto* p = state.getParameter (id); return p->getText (1.0f, 1024); };
    const auto bottom = [&state] (const juce::String& id) { auto* p = state.getParameter (id); return p->getText (0.0f, 1024); };

    for (const auto* id : { "f1_cutoff", "f2_cutoff", "fx_filter_cutoff" })
    {
        expect (id, "99000", top (id));
        expect (id, "99000 Hz", top (id));
        expect (id, "99 kHz", top (id));
        expect (id, "1000000", top (id));
        expect (id, "5", typed (id, "5 kHz"));
        expect (id, "500", typed (id, "500 Hz"));
        expect (id, "20000", top (id));
        expect (id, "2k", typed (id, "2 kHz"));
        expect (id, "3", typed (id, "3 kHz"));
        expect (id, "0.001", bottom (id));
        expect (id, "-50", bottom (id));
    }
    expect ("amp_release", "99999", top ("amp_release"));
    expect ("amp_release", "250", typed ("amp_release", "250 ms"));
    expect ("amp_attack", "-3", bottom ("amp_attack"));
    expect ("sub_level", "150", top ("sub_level"));
    expect ("sub_level", "50", typed ("sub_level", "50 %"));
    expect ("sub_level", "-20", bottom ("sub_level"));

    // Every knob that shows one rising number with a unit: its top number
    // times 10000, typed bare, sets the top in any of its units (the quirk
    // was a smaller unit that happened to land in range).
    juce::StringArray wrong;
    for (auto* parameter : processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::AudioParameterFloat*> (parameter);
        if (ranged == nullptr)
            continue;
        const auto high = parameter->getText (1.0f, 1024), low = parameter->getText (0.0f, 1024);
        const auto highNumber = high.retainCharacters ("0123456789.").getFloatValue();
        const auto lowNumber = low.retainCharacters ("0123456789.").getFloatValue();
        if (high.containsAnyOf ("-+") || low.containsAnyOf ("-+") || ! high.containsAnyOf ("0123456789")
            || highNumber <= 0.0f || high.retainCharacters ("0123456789").isEmpty()
            || high.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ%") == "")
            continue;
        // Rising display (a number that falls as the knob turns is left alone).
        if (lowNumber > highNumber && low.retainCharacters ("kms") == high.retainCharacters ("kms"))
            continue;
        const auto text = juce::String ((juce::int64) std::llround (highNumber * 10000.0f));
        const auto got = parameter->getText (parameter->getValueForText (text), 1024);
        if (got != high)
            wrong.add (ranged->paramID + " '" + text + "' -> '" + got + "' (top '" + high + "')");
    }
    check (wrong.isEmpty(), "a number far past a knob's top sets its top" + (wrong.isEmpty() ? juce::String()
               : " (" + juce::String (wrong.size()) + " wrong, e.g. " + wrong.joinIntoString ("; ").substring (0, 1500) + ")"));
}

void runTests()
{
    runTextRoundTripTest();
    runTypedValueTest();
    runUniqueIdTest();
    runPresetIdTest();
    runBouncePresetTest();
    const auto factory = TableFactory::getNumFactoryTables();
    check (factory == 134 && TableFactory::getFactoryTableNames().size() == factory
               && TableFactory::getFactoryTableCategories().size() == factory,
           "134 factory tables, each with a name and a category");

    // Old patches: a User choice moves past the new tables, on all six
    // oscillators; a factory choice stays.
    {
        auto p = loadSchema2 ({ { "osc1_table", 39.0f }, { "osc2_table", 41.0f }, { "osc4_table", 55.0f }, { "osc6_table", 40.0f } });
        check (juce::roundToInt (rawValue (*p, "osc1_table")) == 39 && juce::roundToInt (rawValue (*p, "osc2_table")) == factory + 1
                   && juce::roundToInt (rawValue (*p, "osc4_table")) == factory + 15 && juce::roundToInt (rawValue (*p, "osc6_table")) == factory,
               "a pre-1.3 patch's User table choices move past the new factory tables (all six oscillators)");
        juce::MemoryBlock again;
        p->getStateInformation (again);
        IlanaSynthAudioProcessor reloaded;
        reloaded.setStateInformation (again.getData(), (int) again.getSize());
        check (juce::roundToInt (rawValue (reloaded, "osc2_table")) == factory + 1, "a migrated patch is not migrated twice");
    }

    // Step 19: a patch saved with 120 factory tables (tableSchema 3) keeps its
    // factory choices and moves its User choices past the 14 new tables.
    {
        auto p = loadSchema2 ({ { "osc1_table", 119.0f }, { "osc2_table", 120.0f }, { "osc5_table", 123.0f } }, 3);
        check (juce::roundToInt (rawValue (*p, "osc1_table")) == 119 && juce::roundToInt (rawValue (*p, "osc2_table")) == factory
                   && juce::roundToInt (rawValue (*p, "osc5_table")) == factory + 3,
               "a 120-table patch's User choices move past the step 19 tables");
        auto q = loadSchema2 ({ { "osc1_table", 40.0f } }, 2);
        check (juce::roundToInt (rawValue (*q, "osc1_table")) == factory, "a pre-1.3 User 1 lands on User 1 after both moves");
    }

    // Choices come back exactly (the stored value of index 7 of 136 is
    // 6.9999995): the vector pad's corners read through the same path.
    {
        IlanaSynthAudioProcessor processor;
        auto exact = true;
        for (int osc = 0; osc < OscillatorIds::count; ++osc)
        {
            setParam (processor, "vec_b", (float) osc);
            exact = exact && processor.getVectorCorner (1) == osc;
        }
        check (exact, "choice parameters read back as their exact index");
    }

    // The new tables: finite, full-scale, 64 frames, and all different.
    {
        auto ok = true;
        std::vector<std::vector<float>> middles;
        for (int i = 40; i < factory; ++i)
        {
            const auto frames = TableFactory::generate (i);
            auto peak = 0.0f;
            auto finite = frames.size() == 64;
            for (const auto& frame : frames)
                for (auto v : frame)
                {
                    finite = finite && std::isfinite (v);
                    peak = juce::jmax (peak, std::abs (v));
                }
            if (! finite || peak < 0.5f || peak > 1.01f)
            {
                ok = false;
                std::cout << "INFO: table " << TableFactory::getFactoryTableNames()[i] << " peak " << peak << std::endl;
            }
            middles.push_back (frames.empty() ? std::vector<float> (2048) : frames[32]);
        }
        check (ok, "the 80 new tables are finite and full-scale, 64 frames each");

        auto duplicates = 0;
        for (size_t a = 0; a < middles.size(); ++a)
            for (size_t b = a + 1; b < middles.size(); ++b)
            {
                double ab = 0, aa = 0, bb = 0;
                for (size_t i = 0; i < 2048; ++i)
                {
                    ab += middles[a][i] * middles[b][i];
                    aa += middles[a][i] * middles[a][i];
                    bb += middles[b][i] * middles[b][i];
                }
                if (std::abs (ab) / std::sqrt (aa * bb + 1e-30) > 0.999)
                {
                    ++duplicates;
                    std::cout << "INFO: tables " << TableFactory::getFactoryTableNames()[(int) a + 40] << " and "
                              << TableFactory::getFactoryTableNames()[(int) b + 40] << " match" << std::endl;
                }
            }
        check (duplicates == 0, "no two new tables are the same wave");

        const auto categories = TableFactory::getFactoryTableCategories();
        const auto order = TableFactory::getCategoryOrder();
        auto known = true;
        for (const auto& c : categories)
            known = known && order.contains (c);
        check (known, "every table's category is one the browser shows");
    }
}
} // namespace M10

void runM10Tests()
{
    M10::runTests();
}
