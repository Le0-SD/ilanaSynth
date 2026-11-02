// Split verbatim from PluginEditor.cpp. Included only from PluginEditor.cpp,
// after its includes; the contents stay in the same anonymous namespace.
#pragma once

namespace
{
// What an oscillator is in the patch's FM, read the same way by PLAY's
// strips and OSC's cards, so an operator reads as one (its ratio, its
// Operator EG, what it modulates) wherever it shows (UI review 6, V3, S3,
// I6-5).
namespace OscRole
{
inline juce::String prefix (int osc) { return OscillatorIds::prefixes[(size_t) juce::jlimit (0, OscillatorIds::count - 1, osc)]; }

inline float read (const IlanaSynthAudioProcessor& p, const juce::String& id)
{
    const auto* value = p.apvts.getRawParameterValue (id);
    return value != nullptr ? value->load() : 0.0f;
}

// ENVELOPE choice 17 (appended): the Operator EG.
constexpr int operatorEgChoice = 17;

inline bool usesOperatorEg (const IlanaSynthAudioProcessor& p, int osc)
{
    return juce::roundToInt (read (p, prefix (osc) + "_amp_env")) == operatorEgChoice;
}

inline int tuning (const IlanaSynthAudioProcessor& p, int osc)
{
    return juce::jlimit (0, OscTuning::Count - 1, juce::roundToInt (read (p, prefix (osc) + "_tune")));
}

// The oscillators this one modulates (its feedback aside).
inline std::vector<int> targets (const IlanaSynthAudioProcessor& p, int osc)
{
    std::vector<int> result;

    for (int target = 0; target < OscillatorIds::count; ++target)
        if (target != osc && p.isOscillatorShown (target) && read (p, FmDiagram::routeId (osc, target)) > 0.001f)
            result.push_back (target);

    return result;
}

// The oscillators that modulate this one.
inline std::vector<int> sources (const IlanaSynthAudioProcessor& p, int osc)
{
    std::vector<int> result;

    for (int source = 0; source < OscillatorIds::count; ++source)
        if (source != osc && p.isOscillatorShown (source) && read (p, FmDiagram::routeId (source, osc)) > 0.001f)
            result.push_back (source);

    return result;
}

inline int mode (const IlanaSynthAudioProcessor& p, int osc)
{
    return juce::jlimit (0, OscMode::count - 1, juce::roundToInt (read (p, prefix (osc) + "_mode")));
}

// An operator is an oscillator of the FM / DX7 type (2026-10-06; before, a
// wavetable tuned by ratio or fixed Hz or on the Operator EG, which now loads
// as one). One in an FM route but of another type stays an oscillator with
// FM, as on Vital (UI review 7, V7-15).
inline bool isOperator (const IlanaSynthAudioProcessor& p, int osc)
{
    return mode (p, osc) == OscMode::fmOperator;
}

// The type menu's order: FM / DX7 next to Wavetable (both play tables);
// the saved index is unchanged.
inline const std::array<int, OscMode::count>& modeMenuOrder()
{
    static const std::array<int, OscMode::count> order { OscMode::wavetable, OscMode::fmOperator, OscMode::physical,
                                                         OscMode::sample, OscMode::granular, OscMode::live };
    return order;
}

inline juce::String modeName (int mode)
{
    return OscMode::names[(size_t) juce::jlimit (0, OscMode::count - 1, mode)];
}

// Picks an oscillator's type as one undo step, through the processor (so
// leaving FM / DX7 drops the operator's tuning and Operator EG).
inline void chooseMode (IlanaSynthAudioProcessor& p, int osc, int newMode)
{
    if (newMode < 0 || newMode >= OscMode::count || newMode == mode (p, osc))
        return;
    p.performEdit ("OSC " + juce::String (osc + 1) + " type " + modeName (newMode),
                   [&p, osc, newMode] { p.setOscillatorMode (osc, newMode); });
}

// The type menu's items in its order (the UI tests read them).
inline juce::StringArray modeMenuItems()
{
    juce::StringArray items;
    for (const auto entry : modeMenuOrder())
        items.add (modeName (entry));
    return items;
}

// A type menu (oscN_mode) that picks through chooseMode: its list in the
// menu's order, the arrow keys step through the same order.
inline void bindModeMenu (ComboControl& control, IlanaSynthAudioProcessor& p, int osc)
{
    auto* box = &control.getComboBox();
    control.setPopupOverride ([box, &p, osc]
    {
        juce::PopupMenu menu;
        const auto current = mode (p, osc);
        for (const auto entry : modeMenuOrder())
            menu.addItem (entry + 1, modeName (entry), true, entry == current);

        juce::Component::SafePointer<juce::ComboBox> safe (box);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (box), [safe, &p, osc] (int result)
        {
            if (safe != nullptr && result > 0)
                chooseMode (p, osc, result - 1);
        });
    });
    control.setNudgeOverride ([&p, osc] (int step)
    {
        const auto& order = modeMenuOrder();
        const auto at = (int) (std::find (order.begin(), order.end(), mode (p, osc)) - order.begin());
        const auto next = juce::jlimit (0, OscMode::count - 1, at + step);
        chooseMode (p, osc, order[(size_t) next]);
    });
}

// The operator's own choices in an oscillator's TUNING and ENVELOPE menus:
// Ratio, Fixed Hz and OP ENV belong to FM / DX7. Elsewhere TUNING keeps only
// Semitones (Ratio and Fixed Hz greyed, so the menu says where they are),
// and a Wavetable's ENVELOPE has no OP ENV. The saved indices are unchanged.
// `current`: the ENVELOPE parameter's index, shown once the menu is rebuilt
// (its attachment could not show OP ENV while the item was missing).
inline void showOperatorChoices (juce::ComboBox* tune, juce::ComboBox* envelope, bool asOperator, bool envelopeChoice, int current)
{
    if (tune != nullptr)
        for (const auto choice : { OscTuning::Ratio, OscTuning::Fixed })
            if (tune->isItemEnabled (choice + 1) != asOperator)
                tune->setItemEnabled (choice + 1, asOperator);

    if (envelope == nullptr)
        return;

    const auto id = OperatorEg::envelopeChoice + 1;
    // (An oscillator that is not an operator but still holds OP ENV keeps the item, so the menu
    // shows its setting rather than a blank: N16-3.)
    envelopeChoice = envelopeChoice || current == OperatorEg::envelopeChoice;
    const auto has = envelope->indexOfItemId (id) >= 0;
    if (has == envelopeChoice)
    {
        if (envelope->getSelectedId() != current + 1 && envelope->indexOfItemId (current + 1) >= 0)
            envelope->setSelectedId (current + 1, juce::dontSendNotification);
        return;
    }

    // Rebuilt in place, with any section headings, so the saved index keeps
    // its item.
    struct Entry { juce::String text; int itemId; bool heading; };
    std::vector<Entry> entries;
    for (juce::PopupMenu::MenuItemIterator it (*envelope->getRootMenu(), false); it.next();)
    {
        const auto& item = it.getItem();
        if (item.itemID == id || (item.isSectionHeader && item.text == "FM PAGE"))
            continue;
        entries.push_back ({ item.text, item.itemID, item.isSectionHeader });
    }
    const auto sectioned = std::any_of (entries.begin(), entries.end(), [] (const Entry& e) { return e.heading; });
    envelope->clear (juce::dontSendNotification);
    for (const auto& entry : entries)
    {
        if (entry.heading)
            envelope->addSectionHeading (entry.text);
        else if (entry.itemId != 0)
            envelope->addItem (entry.text, entry.itemId);
    }
    if (envelopeChoice)
    {
        if (sectioned)
            envelope->addSectionHeading ("FM PAGE");
        envelope->addItem ("OP ENV", id);
    }
    envelope->setSelectedId (current + 1, juce::dontSendNotification);
}

inline juce::String oscList (const std::vector<int>& oscs, const juce::String& before)
{
    juce::StringArray names;
    for (const auto osc : oscs)
        names.add (before + juce::String (osc + 1));
    return names.joinIntoString (", ");
}

// "OSC 2 30 %": the FM routes' depths named beside the oscillators, so the
// caption on OSC carries the number the FM page and the matrix edit (review
// 11, I11-4). `from`: the routes into osc, else out of it.
inline juce::String depthList (const IlanaSynthAudioProcessor& p, int osc, const std::vector<int>& others, bool from)
{
    juce::StringArray names;
    for (const auto other : others)
        names.add ("OSC " + juce::String (other + 1) + juce::String::fromUTF8 (" \xc2\xb7 ")
                   + juce::String (juce::roundToInt (read (p, from ? FmDiagram::routeId (other, osc) : FmDiagram::routeId (osc, other)) * 100.0f)) + "%");
    return names.joinIntoString (", ");
}

// "OUT", "MOD → 1, 3", "OUT, MOD → 2" or "SILENT": an oscillator's part in
// the FM routing, in the FM diagram's words, whether or not it is an
// operator (I8-19); empty for an oscillator in no FM route.
inline juce::String describe (const IlanaSynthAudioProcessor& p, int osc)
{
    const auto modulated = targets (p, osc);
    if (! isOperator (p, osc) && modulated.empty() && sources (p, osc).empty())
        return {};

    const auto out = read (p, prefix (osc) + "_out") > 0.5f;
    if (modulated.empty())
        return out ? "OUT" : "SILENT";

    juce::StringArray numbers;
    for (const auto target : modulated)
        numbers.add (juce::String (target + 1));

    // The arrow the matrix uses, not a greater-than sign (review 11, I11-16).
    return juce::String (out ? "OUT, MOD " : "MOD ") + juce::String::fromUTF8 ("\xe2\x86\x92 ") + numbers.joinIntoString (", ");
}

// A modulator: it feeds other oscillators and is not itself heard. Its
// A modulator's level is still its OUTPUT: a DX7 operator shows one level, named
// OUTPUT everywhere (UI-CONVENTIONS, review 15 I15-1).
inline bool isModulator (const IlanaSynthAudioProcessor& p, int osc)
{
    return read (p, prefix (osc) + "_out") <= 0.5f && ! targets (p, osc).empty();
}

inline const char* outputKnobName (const IlanaSynthAudioProcessor& p, int osc)
{
    juce::ignoreUnused (p, osc);
    return "OUTPUT";
}

// PLAY's strip role line: what the oscillator does with its OUTPUT. A carrier
// goes "TO OUTPUT", a modulator "MODULATES 1, 3" (I12-3); the tabs keep the
// short form of describe().
inline juce::String describeLong (const IlanaSynthAudioProcessor& p, int osc);
inline juce::String roleLine (const IlanaSynthAudioProcessor& p, int osc)
{
    if (describe (p, osc).isEmpty())
        return {};

    const auto out = read (p, prefix (osc) + "_out") > 0.5f;
    const auto modulated = targets (p, osc);

    // A wavetable oscillator in an FM route says it in the OSC card's words
    // and with its amounts: "TO OUTPUT, FM INTO OSC 1 30 %" (I15-12).
    if (! isOperator (p, osc))
        return (out ? juce::String ("TO OUTPUT") : juce::String ("SILENT")) + (describeLong (p, osc).isNotEmpty() ? ", " + describeLong (p, osc) : juce::String());

    if (modulated.empty())
        return out ? "TO OUTPUT" : "SILENT";

    juce::StringArray numbers;
    for (const auto target : modulated)
        numbers.add (juce::String (target + 1));

    return juce::String (out ? "TO OUTPUT, MODULATES " : "MODULATES ") + numbers.joinIntoString (", ");
}

// The same in words, for tooltips and the OSC card's header.
inline juce::String describeLong (const IlanaSynthAudioProcessor& p, int osc)
{
    if (! isOperator (p, osc))
    {
        juce::StringArray parts;
        if (const auto from = sources (p, osc); ! from.empty())
            parts.add ("FM FROM " + depthList (p, osc, from, true)); // upper case as on the tab (review 8, S8-30)
        if (const auto into = targets (p, osc); ! into.empty())
            parts.add ("FM INTO " + depthList (p, osc, into, false));
        return parts.joinIntoString (", ");
    }

    const auto out = read (p, prefix (osc) + "_out") > 0.5f;
    const auto modulated = targets (p, osc);
    juce::StringArray parts;
    parts.add ("FM operator");

    if (out)
        parts.add (modulated.empty() ? "a carrier (heard)" : "heard");
    else if (modulated.empty())
        parts.add ("silent (OUT off, modulates nothing)");

    if (! modulated.empty())
    {
        juce::StringArray names;
        for (const auto target : modulated)
            names.add ("OSC " + juce::String (target + 1));
        parts.add ("modulates " + names.joinIntoString (", "));
    }

    if (usesOperatorEg (p, osc))
        parts.add ("plays its OP ENV");

    return parts.joinIntoString (", ");
}
} // namespace OscRole

// A frame scrubber under the wave picture (review 12, S12-2): 64 ticks across
// the table, the thumb on the oscillator's FRAME; a drag sets it.
class FrameScrubber : public juce::Slider
{
public:
    FrameScrubber()
    {
        setSliderStyle (juce::Slider::LinearHorizontal);
        setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        setTooltip ("Frame\nDrag across the table's frames: the same as the picture's own drag and the FRAME knob.");
    }

    void setColour (juce::Colour newColour) { colour = newColour; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (2.0f, 0.0f);
        const auto position = (float) valueToProportionOfLength (getValue());
        constexpr int ticks = 64;
        for (int i = 0; i < ticks; ++i)
        {
            const auto t = (float) i / (float) (ticks - 1);
            const auto x = area.getX() + t * area.getWidth();
            const auto major = i % 8 == 0;
            g.setColour ((t <= position ? colour : IlanaTheme::Ui::text3).withAlpha (t <= position ? 0.9f : 0.5f));
            g.fillRect (juce::Rectangle<float> (1.0f, major ? 8.0f : 5.0f).withCentre ({ x, area.getCentreY() }));
        }
        const auto thumb = juce::Rectangle<float> (4.0f, (float) getHeight() - 4.0f).withCentre ({ area.getX() + position * area.getWidth(), area.getCentreY() });
        g.setColour (juce::Colours::white.withAlpha (isMouseOverOrDragging() ? 1.0f : 0.85f));
        g.fillRoundedRectangle (thumb, 2.0f);
    }

private:
    juce::Colour colour = IlanaTheme::accent();
};

// An operator's Operator Env as a small picture, where a plain oscillator
// shows its wave (PLAY's strips: UI review 7, I7-19): the engine's own run
// at C3, time on the graphs' square-root scale, levels down to -60 dB.
struct OperatorEnvThumb
{
    // Re-reads the operator; true when its shape changed.
    bool update (const IlanaSynthAudioProcessor& p, int osc)
    {
        const auto now = OperatorEnv::read (p, OscRole::prefix (osc));

        if (valid && now == settings)
            return false;

        settings = now;
        valid = true;
        const auto run = OperatorEnv::run (settings);
        const auto total = juce::jmax (1.0, OperatorEnv::blocksToSeconds ((int) run.values.size() - 1));
        const auto step = std::max<size_t> (1, run.values.size() / 400);
        points.clear();

        for (size_t i = 0; i < run.values.size(); i += step)
            points.push_back ({ (float) std::sqrt (OperatorEnv::blocksToSeconds ((int) i) / total),
                                juce::jlimit (0.0f, 1.0f, 1.0f + (float) run.values[i] / 10.0f) });

        keyUp = (float) std::sqrt (OperatorEnv::blocksToSeconds (run.stageEnd[2]) / total);
        return true;
    }

    void paint (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour, bool lit) const
    {
        IlanaTheme::paintWell (g, area, 6.0f);
        const auto plot = area.reduced (8.0f, 8.0f);
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        const float dashes[] { 3.0f, 3.0f };
        const auto x = plot.getX() + keyUp * plot.getWidth();
        g.drawDashedLine ({ x, plot.getY(), x, plot.getBottom() }, dashes, 2, 1.0f);

        juce::Path line, fill;
        for (size_t i = 0; i < points.size(); ++i)
        {
            const juce::Point<float> point (plot.getX() + points[i].x * plot.getWidth(), plot.getBottom() - points[i].y * plot.getHeight());
            if (i == 0)
            {
                line.startNewSubPath (point);
                fill.startNewSubPath (point.x, plot.getBottom());
            }
            else
                line.lineTo (point);
            fill.lineTo (point);
        }
        if (points.empty())
            return;
        fill.lineTo (plot.getX() + points.back().x * plot.getWidth(), plot.getBottom());
        fill.closeSubPath();
        g.setColour (colour.withAlpha (lit ? 0.16f : 0.06f));
        g.fillPath (fill);
        g.setColour (colour.withAlpha (lit ? 0.9f : 0.3f));
        g.strokePath (line, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    OperatorEnv::Settings settings;
    bool valid = false;
    std::vector<juce::Point<float>> points;
    float keyUp = 0.5f;
};

// EXCITE's menu in groups, under the one set of exciter names (UI review 7,
// I7-27). The items keep their ids, so the saved choice is unchanged.
inline void groupExciteMenu (ComboControl& control)
{
    auto& box = control.getComboBox();
    // (Read before renaming: getSelectedId() only matches while the shown
    // text equals the item's text. Re-selected so the box shows the new name.)
    const auto selectedId = box.getSelectedId();

    for (int excite = 0; excite < juce::jmin (box.getNumItems(), Exciters::names().size()); ++excite)
        box.changeItemText (excite + 1, Exciters::name (excite));

    box.setSelectedId (0, juce::dontSendNotification);
    box.setSelectedId (selectedId, juce::dontSendNotification);

    control.setPopupOverride ([combo = &box]
    {
        juce::PopupMenu menu;

        for (const auto& [group, members] : Exciters::groups())
        {
            menu.addSectionHeader (group);

            for (const auto excite : members)
                menu.addItem (excite + 1, Exciters::name (excite), true, combo->getSelectedId() == excite + 1);
        }

        juce::Component::SafePointer<juce::ComboBox> safe (combo);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (combo), [safe] (int result)
        {
            if (safe != nullptr && result > 0)
                safe->setSelectedId (result);
        });
    });
}



// WARP's menu in groups, and FM and Ring saying which oscillator they use
// (review 23: they read a fixed one, OSC 2 into OSC 1 and OSC 1 into the others, and
// the menu never said). The items keep their ids, so the saved choice is unchanged.
inline void groupWarpMenu (ComboControl& control, int osc, std::function<bool (int)> isShown)
{
    auto& box = control.getComboBox();
    const auto source = osc == 0 ? 1 : 0;
    const auto selectedId = box.getSelectedId();
    const auto fmText = "FM from OSC " + juce::String (source + 1);
    const auto ringText = "Ring with OSC " + juce::String (source + 1);

    if (box.getNumItems() > Warp::NoiseFm)
        box.changeItemText (Warp::NoiseFm + 1, "FM from Noise");

    if (box.getNumItems() > Warp::Ring)
    {
        box.changeItemText (Warp::Fm + 1, fmText);
        box.changeItemText (Warp::Ring + 1, ringText);
        box.setSelectedId (0, juce::dontSendNotification);
        box.setSelectedId (selectedId, juce::dontSendNotification);
    }

    box.getProperties().set ("valueDot", (int) IlanaTheme::oscColour (source).getARGB());
    box.getProperties().set ("valueDotPrefixes", "FM from OSC|Ring with");

    control.setPopupOverride ([combo = &box, source, osc, isShown, fmText, ringText]
    {
        juce::PopupMenu menu;
        const auto current = combo->getSelectedId();
        const auto add = [&] (int mode, const juce::String& text, const juce::String& note = {}, bool enabled = true)
        {
            juce::PopupMenu::Item item (text);
            item.itemID = mode + 1;
            item.isEnabled = enabled;
            item.isTicked = current == mode + 1;
            item.shortcutKeyDescription = note;
            menu.addItem (item);
        };
        const auto names = Warp::getNames();
        const auto name = [&] (int mode) { return names[mode]; };

        add (Warp::Off, "Off");
        menu.addSectionHeader ("PHASE");
        for (int mode = Warp::Sync; mode <= Warp::Quantize; ++mode)
            add (mode, name (mode));
        menu.addSectionHeader ("PHASE DISTORTION");
        for (int mode = Warp::PdSaw; mode <= Warp::PdRes3; ++mode)
            add (mode, name (mode));
        menu.addSectionHeader ("SHAPERS");
        add (Warp::Formant, name (Warp::Formant));
        add (Warp::Squeeze, name (Warp::Squeeze));
        menu.addSectionHeader ("USES ANOTHER OSCILLATOR");
        const auto arrow = juce::String::fromUTF8 (" \xe2\x86\x92 ");
        const auto note = "OSC " + juce::String (source + 1) + arrow + "OSC " + juce::String (osc + 1);
        add (Warp::Fm, fmText, note, isShown (source));
        add (Warp::Ring, ringText, "OSC " + juce::String (source + 1) + juce::String::fromUTF8 (" \xc3\x97 ") + "OSC " + juce::String (osc + 1), isShown (source));
        add (Warp::NoiseFm, "FM from Noise", "NOISE" + arrow + "OSC " + juce::String (osc + 1));

        juce::Component::SafePointer<juce::ComboBox> safe (combo);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (combo), [safe] (int result)
        {
            if (safe != nullptr && result > 0)
                safe->setSelectedId (result);
        });
    });
}

// The unison block's little picture (the OSC cards' UNISON row): one bar per
// voice, spread across the width as far as DETUNE reaches, the outer voices
// shorter. A picture only; it takes no clicks.
class UnisonSpreadView : public juce::Component, public juce::SettableTooltipClient
{
public:
    UnisonSpreadView (IlanaSynthAudioProcessor& p, int index)
        : processorRef (p), prefix (OscillatorIds::prefixes[(size_t) index])
    {
        setInterceptsMouseClicks (false, true);
        setTooltip ("The unison voices spread by the detune amount (picture only)");
    }

    void setColour (juce::Colour newColour)
    {
        colour = newColour;
        repaint();
    }

    // Re-reads the voices and the detune; repaints when they changed.
    void update()
    {
        const auto voiceCount = juce::jlimit (1, 16, juce::roundToInt (read ("_unison")));
        const auto detune = read ("_detune");
        const auto active = read ("_unison") > 1.5f;

        if (voiceCount != voices || std::abs (detune - spread) > 0.002f * juce::jmax (1.0f, std::abs (detune)) || active != isActive)
        {
            voices = voiceCount;
            spread = detune;
            isActive = active;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        const auto count = voices;
        const auto detuneNorm = [this]
        {
            if (auto* parameter = processorRef.apvts.getParameter (prefix + "_detune"))
                return parameter->convertTo0to1 (spread);
            return 0.5f;
        }();
        const auto half = area.getWidth() * 0.5f - 6.0f;
        const auto reach = half * (0.18f + 0.82f * juce::jlimit (0.0f, 1.0f, detuneNorm));

        // (The track the voices stand on, as the mock draws it.)
        const auto baseline = area.getBottom() - 1.5f;
        g.setColour (IlanaTheme::Ui::line);
        g.drawHorizontalLine ((int) baseline, area.getX(), area.getRight());

        for (int i = 0; i < count; ++i)
        {
            const auto position = count == 1 ? 0.0f : ((float) i - (float) (count - 1) * 0.5f) / ((float) (count - 1) * 0.5f);
            const auto closeness = 1.0f - std::abs (position);
            const auto height = juce::jmin (area.getHeight() - 4.0f, 8.0f + 14.0f * closeness);
            const auto bar = juce::Rectangle<float> (4.0f, height).withBottomY (baseline - 0.5f).withCentre ({ area.getCentreX() + position * reach, baseline - 0.5f - height * 0.5f });
            g.setColour (colour.withAlpha (0.45f + 0.5f * closeness));
            g.fillRoundedRectangle (bar, 2.0f);
        }
    }

private:
    // The parameter with what modulates it right now, as the voices hear it
    // (DETUNE moved by an LFO or envelope spreads the bars).
    float read (const char* suffix) const
    {
        const auto id = prefix + suffix;
        const auto* value = processorRef.apvts.getRawParameterValue (id);
        const auto plain = value != nullptr ? value->load() : 0.0f;

        if (const auto config = modRingConfigFor (id); config.destination != 0)
            if (const auto* parameter = processorRef.apvts.getParameter (id))
            {
                return config.modulated (parameter->getNormalisableRange(), plain, processorRef.getModDisplay (config.destination));
            }

        return plain;
    }

    IlanaSynthAudioProcessor& processorRef;
    juce::String prefix;
    juce::Colour colour = IlanaTheme::accent();
    int voices = 0;
    float spread = -1.0f;
    bool isActive = false;
};

// A Physical oscillator's card, right of its string and exciter: the
// string's partials as bars (their levels from the strike point, DAMP and
// STIFF). A picture only.
class StringPartialsView : public juce::Component,
                           public juce::SettableTooltipClient,
                           private IlanaAnim::FrameTimer
{
public:
    StringPartialsView (IlanaSynthAudioProcessor& p, int index)
        : processorRef (p), prefix (OscillatorIds::prefixes[(size_t) index])
    {
        setTooltip ("PARTIALS: the string's overtones, 1 to 16 from the left, as they sound.\n"
                    "DAMP: fades the upper ones (more damp, fewer bars on the right).\n"
                    "STIFF: pushes them sharp and uneven.\n"
                    "POSITION: where the string is struck, which mutes the partials that have a node there.");
        startPollingHz (30);
    }

    void setColour (juce::Colour newColour)
    {
        colour = newColour;
        repaint();
    }

    // A compact view has no title of its own (its panel labels it): the bars fill it.
    void setCompact (bool shouldBeCompact)
    {
        if (compact == shouldBeCompact)
            return;
        compact = shouldBeCompact;
        repaint();
    }

    // Re-reads the settings (the frame timer also does, so the bars glide).
    void update()
    {
        if (eased[0] < 0.0f)
            follow (0.0f);
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (IlanaTheme::Ui::text3);
        g.setFont (IlanaTheme::font (IlanaTheme::TextSize::tiny, true));
        // A tall view (two rows) names itself above the bars, a one-row view beside them.
        const auto tall = ! compact && area.getHeight() >= 44.0f;
        auto titleArea = juce::Rectangle<float>();
        if (! compact)
        {
            titleArea = tall ? area.removeFromTop (14.0f) : area.removeFromLeft (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), "PARTIALS") + 2.0f);
            IlanaTheme::drawFitted (g, "PARTIALS", titleArea, juce::Justification::centredLeft, 1);
        }

        // While a control moves, the view says which one and what it does to
        // the bars (a tall view: beside the title).
        if (tall && caption.isNotEmpty() && hold > 0.0f)
        {
            g.setColour (colour.withAlpha (juce::jlimit (0.0f, 1.0f, hold * 2.0f)));
            g.setFont (IlanaTheme::font (IlanaTheme::TextSize::tiny));
            g.drawText (caption, titleArea.withLeft (titleArea.getX() + juce::GlyphArrangement::getStringWidth (IlanaTheme::font (IlanaTheme::TextSize::tiny, true), "PARTIALS") + 8.0f)
                                     .withRight (getLocalBounds().toFloat().getRight()).toNearestInt(),
                        juce::Justification::centredLeft, true);
        }

        if (! tall && ! compact)
            area.removeFromLeft (8.0f);
        const auto plot = area.reduced (0.0f, 2.0f);

        const auto damp = eased[0], stiff = eased[1];
        const auto pos = juce::jlimit (0.05f, 0.5f, eased[2] > 0.001f ? eased[2] : 0.25f);
        constexpr int count = 16;
        const auto slot = plot.getWidth() / (float) count;
        for (int n = 1; n <= count; ++n)
        {
            const auto comb = 0.3f + 0.7f * std::abs (std::sin (juce::MathConstants<float>::pi * (float) n * pos));
            // (Drawn on a square-root scale, so the upper partials still show in a 28 px row.)
            const auto level = std::sqrt (juce::jlimit (0.02f, 1.0f, comb / std::pow ((float) n, 0.55f + 1.4f * damp) * (1.0f + 0.4f * stiff * (float) (n % 3))));
            const auto bar = juce::Rectangle<float> (juce::jmin (tall ? 7.0f : 4.0f, slot * 0.6f), plot.getHeight() * level)
                                 .withCentre ({ plot.getX() + slot * ((float) n - 0.5f), plot.getBottom() - plot.getHeight() * level * 0.5f });
            // The control being moved lights the bars it shapes: DAMP the
            // upper half, STIFF every third, POSITION the ones it mutes.
            const auto shaped = hold > 0.0f
                                && ((moving == 0 && n > count / 2) || (moving == 1 && n % 3 != 0) || (moving == 2 && comb < 0.55f));
            g.setColour ((shaped ? colour.brighter (0.35f) : colour).withAlpha (juce::jlimit (0.0f, 1.0f, 0.45f + 0.5f * level + (shaped ? 0.2f * juce::jmin (1.0f, hold * 2.0f) : 0.0f))));
            g.fillRoundedRectangle (bar, 2.0f);
        }
    }

private:
    bool compact = false;

    float read (const char* suffix) const
    {
        const auto* value = processorRef.apvts.getRawParameterValue (prefix + suffix);
        return value != nullptr ? value->load() : 0.0f;
    }

    void timerCallback() override { follow (frameSeconds()); }

    // Eases the shown settings to the real ones (a quick glide, so a knob
    // drag reads as motion), and counts down how long the caption stays.
    void follow (float seconds)
    {
        const std::array<float, 3> now { read ("_string_damp"), read ("_string_stiffness"), read ("_string_excite_pos") };
        auto changed = false;

        for (size_t i = 0; i < 3; ++i)
        {
            if (std::abs (now[i] - target[i]) > 0.0005f)
            {
                if (target[i] >= 0.0f)
                {
                    moving = (int) i;
                    hold = 1.2f;
                    static const char* const names[] { "DAMP fades the upper partials", "STIFF pushes them sharp",
                                                       "POSITION mutes the partials with a node there" };
                    caption = names[i];
                }

                target[i] = now[i];
            }

            if (eased[i] < 0.0f)
                eased[i] = now[i];
            else if (std::abs (eased[i] - target[i]) > 0.0005f)
            {
                eased[i] = seconds > 0.0f ? IlanaAnim::approach (eased[i], target[i], 0.35f, seconds * 60.0f) : target[i];
                changed = true;
            }
            else
                eased[i] = target[i];
        }

        if (hold > 0.0f)
        {
            hold = juce::jmax (0.0f, hold - seconds);
            changed = true;
        }

        if (changed || target != lastTarget)
        {
            lastTarget = target;
            repaint();
        }
    }

    IlanaSynthAudioProcessor& processorRef;
    juce::String prefix;
    juce::Colour colour = IlanaTheme::accent();
    std::array<float, 3> target { -1.0f, -1.0f, -1.0f }, eased { -1.0f, -1.0f, -1.0f }, lastTarget { -2.0f, -2.0f, -2.0f };
    int moving = -1;
    float hold = 0.0f;
    juce::String caption;
};

// The right-hand cells of an oscillator's PITCH row: an oscillator in an FM
// route says so in a chip in its source's colour (a link to the FM page), one
// in none shows its output level as a bar.
// The sample's name in the card header: a packed menu box ("SAMPLE  Rhodes C3.wav") whose click
// opens the load menu instead of a list.
class SampleNameBox : public juce::ComboBox
{
public:
    SampleNameBox()
    {
        setInterceptsMouseClicks (true, false);
        getProperties().set ("packedMenu", true);
    }

    std::function<void()> onPopup;
    void showPopup() override { if (onPopup != nullptr) onPopup(); }

    void setCaptionAndName (const juce::String& caption, const juce::String& name)
    {
        getProperties().set ("textPrefix", caption);
        if (getText() != name)
        {
            clear (juce::dontSendNotification);
            addItem (name, 1);
            setSelectedId (1, juce::dontSendNotification);
        }
        repaint();
    }
};

class OscStatusView : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    OscStatusView (IlanaSynthAudioProcessor& p, int index)
        : processorRef (p), osc (index), colour (IlanaTheme::oscColour (index)) {}

    std::function<void()> onClick;

    // Which half it draws: the FM chip, the output bar, or (as before) the chip
    // else the bar. The packed cards keep the chip in the header and the bar in MIX.
    enum class Part { either, chipOnly, barOnly };
    void setPart (Part newPart) { part = newPart; repaint(); }
    // The width of the level text at the bar's end (PLAY keeps its wider one, v26).
    void setValueWidth (float width) { valueWidth = width; repaint(); }

    // Re-reads the role and the level; repaints when they changed.
    void update (const juce::String& chipTextIn, juce::Colour chipColourIn, bool isLink, const juce::String& tip)
    {
        const auto* levelValue = processorRef.apvts.getRawParameterValue (juce::String (OscillatorIds::prefixes[(size_t) osc]) + "_level");
        const auto newLevel = levelValue != nullptr ? juce::jlimit (0.0f, 1.0f, levelValue->load()) : 0.0f;

        if (chipTextIn != chipText || chipColourIn != chipColour || isLink != link || ! juce::approximatelyEqual (newLevel, level))
        {
            chipText = chipTextIn;
            chipColour = chipColourIn;
            link = isLink;
            level = newLevel;
            setMouseCursor (link ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            setTooltip (tip);
            repaint();
        }
    }

    void setColour (juce::Colour newColour)
    {
        colour = newColour;
        repaint();
    }

    bool hitTest (int, int) override { return link && chipText.isNotEmpty(); }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (link && onClick != nullptr && ! event.mouseWasDraggedSinceMouseDown() && getLocalBounds().contains (event.getPosition()))
            onClick();
    }

    // The chip's text as drawn (the UI test reads it).
    const juce::String& getChipText() const { return chipText; }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();

        if (part == Part::barOnly)
        {
            // The packed MIX panel's row: OUT, the bar, the level in dB.
            g.setColour (IlanaTheme::Ui::text3);
            g.setFont (IlanaTheme::font (IlanaTheme::TextSize::packedLabel, true).withKerningFactor (0.09f));
            IlanaTheme::drawFitted (g, "OUT", area.withWidth (24.0f).toNearestInt(), juce::Justification::centredLeft, 1);
            const auto db = level > 0.0005f ? juce::String (juce::roundToInt (juce::Decibels::gainToDecibels (level))) + " dB" : juce::String ("-inf");
            const auto valueW = valueWidth;
            const auto track = juce::Rectangle<float> (area.getX() + 26.0f, area.getCentreY() - 2.5f, juce::jmax (0.0f, area.getWidth() - 26.0f - valueW - 4.0f), 5.0f);
            g.setColour (IlanaTheme::Ui::track);
            g.fillRoundedRectangle (track, 2.5f);
            g.setColour (colour.withAlpha (0.95f));
            g.fillRoundedRectangle (track.withWidth (juce::jmax (5.0f, track.getWidth() * level)), 2.5f);
            g.setColour (IlanaTheme::Ui::text3);
            g.setFont (IlanaTheme::font (IlanaTheme::TextSize::packedLabel, false, true));
            IlanaTheme::drawFitted (g, db, area.withLeft (area.getRight() - valueW).toNearestInt(), juce::Justification::centredRight, 1);
            return;
        }

        if (chipText.isNotEmpty() && part != Part::barOnly)
        {
            const auto font = IlanaTheme::font (6.8f, true).withKerningFactor (0.06f);
            const auto width = juce::jmin (area.getWidth(), (float) juce::GlyphArrangement::getStringWidthInt (juce::Font (font), chipText) + 24.0f);
            const auto chip = juce::Rectangle<float> (width, 12.0f).withCentre ({ area.getX() + width * 0.5f, area.getCentreY() });
            g.setColour (chipColour.withAlpha (0.2f));
            g.fillRoundedRectangle (chip, 6.0f);
            g.setColour (chipColour.withAlpha (0.75f));
            g.drawRoundedRectangle (chip.reduced (0.5f), 6.0f, 1.0f);
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ chip.getX() + 7.5f, chip.getCentreY() }));
            g.setColour (IlanaTheme::Ui::text);
            g.setFont (font);
            IlanaTheme::drawFitted (g, chipText, chip.withTrimmedLeft (17.0f).withTrimmedRight (7.0f), juce::Justification::centredLeft, 1);
            return;
        }

        if (part == Part::chipOnly)
            return;

        g.setColour (IlanaTheme::Ui::text3);
        g.setFont (IlanaTheme::font (IlanaTheme::TextSize::tiny));
        IlanaTheme::drawFitted (g, "OUT", area.withWidth (30.0f), juce::Justification::centredLeft, 1);
        const auto track = juce::Rectangle<float> (area.getX() + 34.0f, area.getCentreY() - 3.0f, juce::jmax (0.0f, area.getWidth() - 40.0f), 6.0f);
        g.setColour (IlanaTheme::Ui::track);
        g.fillRoundedRectangle (track, 3.0f);
        g.setColour (colour.withAlpha (0.9f));
        g.fillRoundedRectangle (track.withWidth (track.getWidth() * level), 3.0f);
    }

private:
    IlanaSynthAudioProcessor& processorRef;
    int osc;
    juce::Colour colour, chipColour;
    juce::String chipText;
    bool link = false;
    float level = -1.0f;
    Part part = Part::either;
    float valueWidth = 28.0f;
};

// A small quiet action in the packed cards' panels (+ WARP): dashed, in the
// packed name size. Solid draws it as a field instead.
class PackedChip : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    std::function<void()> onClick;

    void setText (const juce::String& newText) { text = newText; repaint(); }
    const juce::String& getText() const { return text; }
    void setSolid (bool shouldBeSolid) { solid = shouldBeSolid; repaint(); }
    int preferredWidth() const { return juce::GlyphArrangement::getStringWidthInt (chipFont(), text) + 14; }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const auto lit = isMouseOver() ? 1.0f : 0.0f;
        if (solid)
        {
            g.setColour (IlanaTheme::Ui::raised.interpolatedWith (IlanaTheme::Ui::hover, 0.5f * lit));
            g.fillRoundedRectangle (bounds, 4.0f);
            g.setColour (IlanaTheme::Ui::line);
            g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
        }
        else
        {
            juce::Path outline;
            outline.addRoundedRectangle (bounds, 4.0f);
            juce::Path dashed;
            const float dashes[] { 3.0f, 2.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (IlanaTheme::Ui::line.interpolatedWith (IlanaTheme::Ui::text3, 0.6f * lit));
            g.fillPath (dashed);
        }
        g.setColour ((solid ? IlanaTheme::Ui::text : IlanaTheme::Ui::text3).interpolatedWith (IlanaTheme::Ui::text, 0.5f * lit));
        g.setFont (chipFont());
        IlanaTheme::drawFitted (g, text, getLocalBounds().reduced (3, 0), juce::Justification::centred, 1);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseUp (const juce::MouseEvent& event) override
    {
        if (isEnabled() && onClick != nullptr && ! event.mouseWasDraggedSinceMouseDown() && getLocalBounds().contains (event.getPosition()))
            onClick();
    }

private:
    static juce::Font chipFont() { return IlanaTheme::font (7.8f, true).withKerningFactor (0.08f); }
    juce::String text;
    bool solid = false;
};

// The sympathetic strings' six notes (STRINGS' Manual tuning): a small panel
// over the page, above the STRINGS card. A click anywhere outside it closes it.
class TuningPopover : public juce::Component
{
public:
    static constexpr int pad = 8, headerH = 22, dial = 36, cell = 44, height = headerH + 6 + 6 + dial + 10 + pad + 2;
    static constexpr int widthFor (int count) { return pad * 2 + count * cell; }

    ~TuningPopover() override { juce::Desktop::getInstance().removeGlobalMouseListener (this); }

    void visibilityChanged() override
    {
        if (isVisible())
            juce::Desktop::getInstance().addGlobalMouseListener (this);
        else
            juce::Desktop::getInstance().removeGlobalMouseListener (this);
    }

    std::function<bool (juce::Component*)> isOwnButton; // (the chevron toggles it itself)
    std::function<void()> onClose;

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (IlanaTheme::Ui::raised);
        g.fillRoundedRectangle (area, 7.0f);
        g.setColour (IlanaTheme::Ui::line.brighter (0.25f));
        g.drawRoundedRectangle (area, 7.0f, 1.0f);
        g.setColour (IlanaTheme::accent());
        g.setFont (IlanaTheme::font (IlanaTheme::TextSize::packedBody, true).withKerningFactor (0.06f));
        g.drawText ("STRING TUNING", juce::Rectangle<int> (pad, 0, getWidth() - 2 * pad, headerH), juce::Justification::centredLeft, false);
        g.setColour (IlanaTheme::Ui::line);
        g.fillRect (1, headerH, getWidth() - 2, 1);
    }

    void resized() override
    {
        auto x = pad;
        for (auto* child : getChildren())
        {
            if (auto* knob = dynamic_cast<KnobControl*> (child))
                knob->setBounds (KnobControl::packedBoundsFor ({ x + (cell - 40) / 2, headerH + 6 + 6, 40, dial + 10 }));
            x += cell;
        }
    }

private:
    void mouseDown (const juce::MouseEvent& event) override
    {
        if (! isVisible() || event.eventComponent == nullptr)
            return;
        if (event.eventComponent == this || isParentOf (event.eventComponent))
            return;
        if (isOwnButton != nullptr && isOwnButton (event.eventComponent))
            return;
        if (onClose != nullptr)
            onClose();
    }
};

// PLAY's drop tile (mockup v4, ilana 2026-10-26): with one or two oscillators the room under the cards
// is one dashed tile holding the two add buttons, and an audio file or SoundFont dropped on it lands
// in a new oscillator.
class OscDropTile : public juce::Component,
                    public juce::FileDragAndDropTarget
{
public:
    std::function<void (const juce::File&)> onFileDropped;

    // The add buttons sit on the tile; the caption goes under them.
    void setCaptionTop (int y) { captionTop = y; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        juce::Path outline, dashed;
        outline.addRoundedRectangle (area, 8.0f);
        const float dashes[] { 4.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
        if (dragHover)
        {
            g.setColour (IlanaTheme::accent().withAlpha (0.08f));
            g.fillRoundedRectangle (area, 8.0f);
        }
        g.setColour (dragHover ? IlanaTheme::accent() : IlanaTheme::Ui::line);
        g.fillPath (dashed);
        if (captionTop > 0 && captionTop + 14 < getHeight())
        {
            g.setColour (dragHover ? IlanaTheme::Ui::text : IlanaTheme::Ui::text3);
            g.setFont (IlanaTheme::font (IlanaTheme::TextSize::label));
            g.drawText (dragHover ? "drop to add it as a new oscillator" : "or drop a wavetable, sample or .sfz here",
                        juce::Rectangle<int> (0, captionTop, getWidth(), 16), juce::Justification::centred, false);
        }
    }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        if (files.size() != 1)
            return false;
        const juce::File file (files[0]);
        for (const auto* extension : { "wav", "aif", "aiff", "flac", "ogg", "mp3", "m4a", "sf2", "sfz" })
            if (file.hasFileExtension (extension))
                return true;
        return false;
    }

    void fileDragEnter (const juce::StringArray&, int, int) override { setDragHover (true); }
    void fileDragExit (const juce::StringArray&) override { setDragHover (false); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        setDragHover (false);
        if (files.size() == 1 && onFileDropped != nullptr)
            onFileDropped (juce::File (files[0]));
    }

private:
    void setDragHover (bool hover)
    {
        dragHover = hover;
        repaint();
    }

    bool dragHover = false;
    int captionTop = 0;
};

class OscPage : public juce::Component,
                private IlanaTheme::UiScaleListener,
                private IlanaTheme::IdentityColourListener,
                private juce::AudioProcessorValueTreeState::Listener,
                private juce::AsyncUpdater,
                private juce::Timer
{
    struct PhysicalControls
    {
        PhysicalControls (juce::AudioProcessorValueTreeState& state, const juce::String& prefix)
            : stiffness (state, prefix + "_string_stiffness", "STIFF"),
              pickup (state, prefix + "_string_pickup", "PICKUP"),
              click (state, prefix + "_string_click", "CLICK"),
              excitePos (state, prefix + "_string_excite_pos", "EXCITE POS"),
              hardness (state, prefix + "_string_pick_hardness", "HARDNESS"),
              pickPos (state, prefix + "_string_pick_pos", "PICK POS"),
              slap (state, prefix + "_string_slap", "SLAP"),
              bowPressure (state, prefix + "_bow_pressure", "GRIP"),
              bowSpeed (state, prefix + "_bow_speed", "SPEED"),
              bridgeBuzz (state, prefix + "_bridge_buzz", "BRIDGE BUZZ"),
              fretRattle (state, prefix + "_fret_rattle", "FRET RATTLE"),
              hammer (state, prefix + "_hammer_hard", "HAMMER"),
              couple (state, prefix + "_couple", "COUPLING"), // the section says STRING (I8-25)
              damper (state, prefix + "_damper", "DAMPER"),
              registerMap (state, prefix + "_register", "REGISTER"),
              epDistance (state, prefix + "_ep_distance", "DISTANCE"),
              epPosition (state, prefix + "_ep_position", "OFFSET"),
              fbGain (state, prefix + "_fb_gain", "AMP GAIN"),
              fbDistance (state, prefix + "_fb_distance", "DISTANCE"),
              // RESONANCE (review 23): the patch's sympathetic strings and soundboard model, as the strip below has them.
              resAmount (state, "sym_amount", "AMOUNT"),
              resOn (state, "sym_on", "SYMPATHETIC"),
              resBoard (state, "sb_model", "BOARD") {}

        void setColour (juce::Colour colour)
        {
            for (auto* knob : { &stiffness, &pickup, &click, &excitePos, &hardness, &pickPos, &bowPressure, &bowSpeed,
                                &bridgeBuzz, &fretRattle, &hammer, &couple, &damper, &registerMap,
                                &epDistance, &epPosition, &fbGain, &fbDistance })
                knob->setIdentityColour (colour);
        }

        KnobControl stiffness, pickup, click, excitePos, hardness, pickPos;
        KnobControl bowPressure, bowSpeed, bridgeBuzz, fretRattle;
        KnobControl hammer, couple, damper, registerMap;
        KnobControl epDistance, epPosition; // M7.3 Tine / Reed pickup
        KnobControl fbGain, fbDistance;     // M8.5 feedback amp
        ToggleControl slap;
        KnobControl resAmount;
        ToggleControl resOn;
        ComboControl resBoard;
    };

    struct OscControls
    {
        OscControls (juce::AudioProcessorValueTreeState& state, const juce::String& prefix, int index)
            : on (state, prefix + "_on", "ON"),
              mode (state, prefix + "_mode", ""),
              table (state, prefix + "_table", "TABLE"),
              excite (state, prefix + "_excite", "EXCITE"),
              level (state, prefix + "_level", "LEVEL"),
              pan (state, prefix + "_pan", "PAN"),
              semi (state, prefix + "_semi", "SEMI"),
              fine (state, prefix + "_fine", "FINE"),
              unison (state, prefix + "_unison", "UNISON"),
              detune (state, prefix + "_detune", "DETUNE"),
              spread (state, prefix + "_spread", "SPREAD"),
              stringDecay (state, prefix + "_string_decay", "DECAY"),
              stringDamp (state, prefix + "_string_damp", "DAMP"),
              stringSustain (state, prefix + "_string_sustain", "SUSTAIN"),
              sampleTuned (state, prefix + "_sample_tuned", "TUNED"),
              sampleLoop (state, prefix + "_sample_loop", "LOOP"),
              sampleReverse (state, prefix + "_sample_reverse", "REVERSE"),
              sampleLoopOwn (state, prefix + "_sample_loop_own", "LOOP PTS"),
              sampleSnap (state, prefix + "_sample_snap", "SNAP"),
              sampleLoopXfade (state, prefix + "_sample_loop_xfade", "CROSSFADE"),
              samplePlayback (state, prefix + "_sample_playback", "PLAYBACK"),
              samplePingPong (state, prefix + "_sample_pingpong", "PING-PONG"),
              sampleStartSafe (state, prefix + "_sample_start_safe", "TRANSIENT SAFE"),
              sampleStartRandom (state, prefix + "_sample_start_rand", "RANDOM START"),
              sampleQuality (state, prefix + "_sample_quality", "QUALITY"),
              sampleStart (state, prefix + "_sample_start", "START"),
              sampleEnd (state, prefix + "_sample_end", "END"),
              sampleFadeIn (state, prefix + "_sample_fade_in", "FADE IN"),
              sampleFadeOut (state, prefix + "_sample_fade_out", "FADE OUT"),
              sampleRoot (state, prefix + "_sample_root", "ROOT"),
              sampleSync (state, prefix + "_sample_sync", "SYNC"),
              sampleStretch (state, prefix + "_sample_stretch", "TIME"),
              sampleBpm (state, prefix + "_sample_bpm", "BPM"),
              chord (state, prefix + "_chord", "CHORD"),
              ampEnv (state, prefix + "_amp_env", "ENVELOPE"),
              warp (state, prefix + "_warp", "WARP"),
              uniMode (state, prefix + "_uni_mode", "UNI MODE"),
              warpAmt (state, prefix + "_warp_amt", "WARP AMT"),
              uniBlend (state, prefix + "_uni_blend", "BLEND"),
              uniFrame (state, prefix + "_uni_frame", juce::String::fromUTF8 ("FRAME \xc2\xb1")),
              uniWarp (state, prefix + "_uni_warp", juce::String::fromUTF8 ("WARP \xc2\xb1")),
              spectral (state, prefix + "_spectral", "SPECTRAL"),
              frameBlend (state, prefix + "_frame_blend", "FRAME BLEND"),
              scale (state, prefix + "_scale", "SCALE"),
              scaleRoot (state, prefix + "_scale_root", "ROOT"),
              spectralAmt (state, prefix + "_spectral_amt", "SPEC AMT"),
              grainPosition (state, prefix + "_grain_pos", "POSITION"),
              grainScan (state, prefix + "_grain_scan", "SCAN"),
              grainFreeze (state, prefix + "_grain_freeze", "FREEZE"),
              grainSize (state, prefix + "_grain_size", "SIZE"),
              grainDensity (state, prefix + "_grain_density", "DENSITY"),
              grainSpray (state, prefix + "_grain_spray", "SPRAY"),
              grainPitch (state, prefix + "_grain_pitch", "PITCH RANDOM"),
              grainSpread (state, prefix + "_grain_spread", "STEREO"),
              grainLive (state, prefix + "_grain_live", "LIVE"),
              warp2 (state, prefix + "_warp2", "WARP 2"),
              warp2Amt (state, prefix + "_warp2_amt", "WARP 2 AMT"),
              tune (state, prefix + "_tune", "TUNING"),
              ratio (state, prefix + "_ratio", "RATIO"),
              fixedHz (state, prefix + "_fixed_hz", "FIXED"),
              egOut (state, prefix + "_eg_out", "OUTPUT"), // one level name (UI review 9, I9-7)
              trim (state, prefix + "_level", "VOICE LEVEL"),
              feedback (state, FmDiagram::routeId (index, index), "FEEDBACK"),
              feedbackType (state, prefix + "_fb_type", "FEEDBACK TYPE") {}

        // Every knob in the oscillator's own colour, as on PLAY.
        void setColour (juce::Colour colour)
        {
            for (auto* knob : { &egOut, &trim, &feedback, &ratio, &fixedHz, &warp2Amt, &level, &pan, &semi, &fine, &unison, &detune, &spread,
                                &stringDecay, &stringDamp, &stringSustain, &sampleStart, &sampleEnd, &sampleFadeIn,
                                &sampleLoopXfade, &sampleStartRandom, &sampleRoot, &sampleFadeOut, &sampleBpm, &warpAmt, &uniBlend, &uniFrame, &uniWarp, &spectralAmt, &grainPosition, &grainScan, &grainSize,
                                &grainDensity, &grainSpray, &grainPitch, &grainSpread })
                knob->setIdentityColour (colour);
        }

        ToggleControl on, sampleTuned, sampleLoop, sampleReverse, sampleLoopOwn, sampleSnap;
        ComboControl mode, table, excite, chord, ampEnv, warp, uniMode, spectral, frameBlend, scale, scaleRoot;
        // M6: the PD chain's second stage and the warp (DCW) envelope.
        ComboControl warp2;
        KnobControl warp2Amt;
        KnobControl level, pan, semi, fine, unison, detune, spread;
        KnobControl stringDecay, stringDamp, stringSustain;
        KnobControl sampleStart, sampleEnd, sampleFadeIn, sampleFadeOut, sampleLoopXfade, sampleRoot;
        // Step 40: the sample follows the host tempo.
        ComboControl sampleSync, sampleStretch;
        // The sampler steps: the PLAYBACK row (33...).
        ComboControl samplePlayback, sampleQuality;
        ToggleControl samplePingPong, sampleStartSafe;
        KnobControl sampleStartRandom;
        KnobControl sampleBpm;
        KnobControl warpAmt, uniBlend, uniFrame, uniWarp, spectralAmt;
        KnobControl grainPosition, grainScan, grainSize, grainDensity, grainSpray, grainPitch, grainSpread;
        ToggleControl grainFreeze, grainLive; // M7.5: grains from the live input (ilanaSynth FX)
        // An FM operator's tuning, as on the FM page (UI review 6, I6-5).
        ComboControl tune;
        KnobControl ratio, fixedHz;
        // An operator on the Operator Env: its OUTPUT (dB) and the
        // oscillator's LEVEL, as the FM card names them, and its
        // feedback (UI review 7, I7-2, I7-20).
        KnobControl egOut, trim, feedback;
        ComboControl feedbackType;
    };

public:
    std::function<void()> onModeChanged;

    explicit OscPage (IlanaSynthAudioProcessor& p)
        : processorRef (p),
          subShape (p.apvts, "sub_shape", "WAVE"),
          subOctave (p.apvts, "sub_octave", "OCTAVE"), noiseType (p.apvts, "noise_type", "TYPE")
          , symOn (p.apvts, "sym_on", "ON"), symManual (p.apvts, "sym_manual", "Manual tuning"), looseBoth (p.apvts, "tune_loose_both", "Sharp and flat")
          , symAmount (p.apvts, "sym_amount", "AMOUNT"), symDecay (p.apvts, "sym_decay", "DECAY")
          , symCount (p.apvts, "sym_count", "COUNT")
          , sbOn (p.apvts, "sb_on", "SOUNDBOARD"), sbModel (p.apvts, "sb_model", "MODEL"), sbMix (p.apvts, "sb_mix", "MIX"), sbTone (p.apvts, "sb_tone", "TONE")
          , sbSize (p.apvts, "sb_size", "SIZE"), stretch (p.apvts, "stretch", "STRETCH")
          , pedalRes (p.apvts, "pedal_res", "PEDAL RESONANCE"), mechKey (p.apvts, "mech_key", "KEY NOISE")
          , mechDamper (p.apvts, "mech_damper", "DAMPER NOISE"), mechPedal (p.apvts, "mech_pedal", "PEDAL NOISE")
    {
        IlanaTheme::identityColourListeners (&p).add (this);

        for (int i = 0; i < OscillatorIds::count; ++i)
        {
            const juce::String prefix (OscillatorIds::prefixes[(size_t) i]);
            controls[(size_t) i] = std::make_unique<OscControls> (p.apvts, prefix, i);
            controls[(size_t) i]->setColour (oscColour (i));
            groupExciteMenu (controls[(size_t) i]->excite);
            groupWarpMenu (controls[(size_t) i]->warp, i, [this] (int other) { return processorRef.isOscillatorShown (other); });
            waveDisplays[(size_t) i] = std::make_unique<WaveDisplay> (
                p, prefix + "_table", prefix + "_frame", prefix + "_unison",
                prefix + "_spread", prefix + "_detune", false, juce::String {},
                prefix + "_mode", i, oscColour (i), false);
            // The card's well opens on the cycle, as the design draws it; its
            // corner chip still steps to every frame at once (3D) and the
            // harmonics (UI review 5, V6; design round 2).
            waveDisplays[(size_t) i]->setViewMode (0);
            waveDisplays[(size_t) i]->onWavetableDrop = [this, i] (const juce::File& file) { askDroppedFile (i, file); };
            loadButtons[(size_t) i] = std::make_unique<juce::TextButton> ("LOAD...");
            editButtons[(size_t) i] = std::make_unique<juce::TextButton> ("EDIT");
            styleJumpLink (*editButtons[(size_t) i], "TABLE");
            // RESAMPLE: one name for the resampler, apart from the Bounce
            // LFO shapes (UI review 7, I7-25).
            bounceButtons[(size_t) i] = std::make_unique<juce::TextButton> ("RESAMPLE");
            aux[(size_t) i] = std::make_unique<CardAux> (p, i);
        }

        for (int i = 0; i < OscillatorIds::count; ++i)
        {
            const juce::String prefix (OscillatorIds::prefixes[(size_t) i]);
            physical[(size_t) i] = std::make_unique<PhysicalControls> (p.apvts, prefix);
            physical[(size_t) i]->setColour (oscColour (i));
            auto& physicalControls = *physical[(size_t) i];
            addChildComponents (physicalControls.stiffness, physicalControls.pickup, physicalControls.click, physicalControls.excitePos,
                                physicalControls.hardness, physicalControls.pickPos, physicalControls.slap,
                                physicalControls.bowPressure, physicalControls.bowSpeed,
                                physicalControls.bridgeBuzz, physicalControls.fretRattle,
                                physicalControls.hammer, physicalControls.couple,
                                physicalControls.damper, physicalControls.registerMap,
                                physicalControls.epDistance, physicalControls.epPosition,
                                physicalControls.fbGain, physicalControls.fbDistance,
                                physicalControls.resAmount, physicalControls.resOn, physicalControls.resBoard);
            physicalControls.resAmount.setIdentityColour (juce::Colour (0xffe9724c));
            physicalControls.resOn.setSwitchColour (juce::Colour (0xffe9724c));
            physicalControls.resOn.setPackedName (ToggleControl::NamePlace::leftFlush);
            physicalControls.resAmount.setInlineKnob (true);
            physicalControls.resBoard.setCompactLayout (true);
            // The card's one inline style, as the string's DECAY and DAMP.
            for (auto* knob : { &physicalControls.stiffness, &physicalControls.pickup, &physicalControls.click, &physicalControls.excitePos, &physicalControls.hardness,
                                &physicalControls.pickPos, &physicalControls.bowPressure, &physicalControls.bowSpeed, &physicalControls.bridgeBuzz,
                                &physicalControls.fretRattle, &physicalControls.hammer, &physicalControls.couple, &physicalControls.damper,
                                &physicalControls.registerMap, &physicalControls.epDistance, &physicalControls.epPosition, &physicalControls.fbGain,
                                &physicalControls.fbDistance })
                knob->setInlineKnob (true);
            physicalControls.slap.setInlineLabel (true);
            // (The short names the grid has room for, as the list names them.)
            physicalControls.bridgeBuzz.setLabelText ("BUZZ");
            physicalControls.fretRattle.setLabelText ("RATTLE");
            physicalControls.excitePos.setLabelText ("POSITION");
            physicalControls.couple.setLabelText ("COUPLE");
            // (Review 23's names: what each does to the sound; the parameters keep their ids and tooltips.)
            controls[(size_t) i]->stringDecay.setLabelText ("RING TIME");
            controls[(size_t) i]->stringDamp.setLabelText ("DARKEN");

            // The card's controls for each entry of the shared physical list.
            auto& osc = *controls[(size_t) i];
            auto& lookup = physicalLookup[(size_t) i];
            lookup["_string_decay"] = &osc.stringDecay;
            lookup["_string_damp"] = &osc.stringDamp;
            lookup["_string_sustain"] = &osc.stringSustain;
            lookup["_excite"] = &osc.excite;
            lookup["_string_stiffness"] = &physicalControls.stiffness;
            lookup["_register"] = &physicalControls.registerMap;
            lookup["_damper"] = &physicalControls.damper;
            lookup["_couple"] = &physicalControls.couple;
            lookup["_string_slap"] = &physicalControls.slap;
            lookup["_string_excite_pos"] = &physicalControls.excitePos;
            lookup["_string_pick_hardness"] = &physicalControls.hardness;
            lookup["_string_pick_pos"] = &physicalControls.pickPos;
            lookup["_hammer_hard"] = &physicalControls.hammer;
            lookup["_bow_pressure"] = &physicalControls.bowPressure;
            lookup["_bow_speed"] = &physicalControls.bowSpeed;
            lookup["_fb_gain"] = &physicalControls.fbGain;
            lookup["_fb_distance"] = &physicalControls.fbDistance;
            lookup["_string_pickup"] = &physicalControls.pickup;
            lookup["_string_click"] = &physicalControls.click;
            lookup["_bridge_buzz"] = &physicalControls.bridgeBuzz;
            lookup["_fret_rattle"] = &physicalControls.fretRattle;
            lookup["_ep_distance"] = &physicalControls.epDistance;
            lookup["_ep_position"] = &physicalControls.epPosition;
        }

        for (int i = 0; i < 6; ++i)
            symNotes[(size_t) i] = std::make_unique<KnobControl> (p.apvts, "sym_note" + juce::String (i + 1),
                                                                    "NOTE " + juce::String (i + 1));

        for (auto& item : controls)
        {
            auto& osc = *item;
            addChildComponents (osc.grainPosition, osc.grainScan, osc.grainFreeze, osc.grainSize, osc.grainDensity,
                                osc.grainSpray, osc.grainPitch, osc.grainSpread, osc.grainLive,
                                osc.spectral, osc.frameBlend, osc.spectralAmt, osc.warp, osc.uniMode, osc.scale, osc.scaleRoot,
                                osc.warpAmt, osc.uniBlend, osc.uniFrame, osc.uniWarp, osc.warp2, osc.warp2Amt);
            addChildComponents (osc.on, osc.mode, osc.table, osc.excite,
                                osc.level, osc.pan, osc.semi, osc.fine,
                                osc.unison, osc.detune, osc.spread, osc.stringDecay,
                                osc.stringDamp, osc.stringSustain, osc.sampleTuned,
                                osc.sampleLoop, osc.sampleReverse, osc.sampleLoopOwn, osc.sampleSnap, osc.sampleLoopXfade, osc.sampleStart,
                                osc.sampleEnd, osc.sampleFadeIn, osc.sampleFadeOut, osc.sampleRoot,
                                osc.sampleSync, osc.sampleBpm, osc.sampleStretch, osc.samplePlayback, osc.samplePingPong, osc.sampleStartRandom, osc.sampleStartSafe, osc.sampleQuality,
                                osc.chord, osc.ampEnv, osc.tune, osc.ratio, osc.fixedHz,
                                osc.egOut, osc.trim, osc.feedback, osc.feedbackType);
        }

        for (int i = 0; i < OscillatorIds::count; ++i)
        {
            const juce::String prefix (OscillatorIds::prefixes[(size_t) i]);
            auto& osc = *controls[(size_t) i];
            auto& card = *aux[(size_t) i];

            // The card's grid: knobs with their name and value to the right
            // of the dial, menus 18 px high under a small name.
            for (auto* knob : cardKnobs (osc))
                knob->setInlineKnob (true);
            for (auto* menu : { &osc.warp, &osc.warp2, &osc.spectral, &osc.tune, &osc.ampEnv, &osc.uniMode, &osc.scale, &osc.scaleRoot,
                                &osc.chord, &osc.excite, &osc.table, &osc.frameBlend, &osc.feedbackType, &osc.mode, &osc.sampleSync, &osc.sampleStretch,
                                &osc.samplePlayback, &osc.sampleQuality })
                menu->setCompactLayout (true);
            for (auto* toggle : { &osc.sampleTuned, &osc.sampleLoop, &osc.sampleReverse, &osc.sampleLoopOwn, &osc.sampleSnap, &osc.samplePingPong, &osc.sampleStartSafe, &osc.grainLive, &osc.grainFreeze })
                toggle->setInlineLabel (true);
            osc.on.setBareSwitch (true);
            osc.on.setSwitchColour (oscColour (i));
            // The type menu picks through the processor (one undo step; an
            // operator's tuning goes when it leaves FM / DX7).
            OscRole::bindModeMenu (osc.mode, processorRef, i);

            // The short names the grid has room for (the tooltips keep the full ones).
            osc.warpAmt.setLabelText ("AMT");
            osc.warp2Amt.setLabelText ("AMT");
            osc.spectralAmt.setLabelText ("AMT");
            osc.unison.setLabelText ("VOICES");
            osc.uniMode.setLabelText ("MODE");

            // An amount whose stage is Off keeps its place and shows a faded dash.
            osc.warpAmt.setDashWhen ([this, prefix] { return readChoice (prefix + "_warp") == 0; });
            osc.warp2Amt.setDashWhen ([this, prefix] { return readChoice (prefix + "_warp2") == 0; });
            osc.spectralAmt.setDashWhen ([this, prefix] { return readChoice (prefix + "_spectral") == 0; });
            // The sample's BPM counts only when SYNC asks for it (a length
            // gives the tempo itself).
            osc.sampleBpm.setDashWhen ([this, prefix] { return readChoice (prefix + "_sample_sync") != SampleTempo::bpm; });

            addChildComponent (card.stringView);
            card.stringView.setColour (oscColour (i));
            card.stringView.setCompact (true);
            addChildComponent (card.opEnvGraph);
            card.opEnvGraph.setReadOnly (true);
            card.opEnvGraph.setCompact (true);
            card.opEnvGraph.onOpen = [this, i]
            {
                if (auto* editor = findParentComponentOfClass<IlanaSynthAudioProcessorEditor>())
                    editor->showOperatorEnvelope (i);
            };
            card.spreadView.setColour (oscColour (i));
            card.partials.setColour (oscColour (i));
            addChildComponent (card.partials);
            card.status.setColour (oscColour (i));
            card.status.onClick = [i]
            {
                if (FmOperatorInfo::hooks().openOperator != nullptr)
                    FmOperatorInfo::hooks().openOperator (i);
            };
            addChildComponent (card.spreadView);
            addChildComponent (card.status);

            // The packed header and panels (review 23).
            const auto family = oscColour (i);
            card.typeBar.setColour (family);
            card.typeBar.setItems ({ "WAVETABLE", "PHYSICAL", "SAMPLE", "GRANULAR", "LIVE", "FM / DX7" },
                                   { "A table of waveforms: pick a frame, warp it, stack voices",
                                     "A plucked, struck or bowed string, a tine or a reed",
                                     "A recording, a multisample or an SF2 / SFZ",
                                     "A cloud of tiny grains cut from a sample",
                                     "The audio input, played as an oscillator",
                                     "An operator: a sine tuned by ratio, on its OP ENV" });
            card.typeBar.getChosen = [this, i]
            {
                const auto mode = getMode (i);
                return mode == OscMode::fmOperator ? 5 : mode;
            };
            card.typeBar.onChoose = [this, i] (int segment)
            {
                const int modes[] { OscMode::wavetable, OscMode::physical, OscMode::sample, OscMode::granular, OscMode::live, OscMode::fmOperator };
                OscRole::chooseMode (processorRef, i, modes[juce::jlimit (0, 5, segment)]);
                updateModeVisibility();
                updateEnabled();
            };
            card.routeBar.setColour (family);
            card.routeBar.setEqual (false);
            card.routeBar.setItems ({ "AUTO", "F1", "F2", "BOTH", "NONE" },
                                    { "Follows the Serial / Parallel switch (into Filter 1 when serial, both when parallel)",
                                      "Through Filter 1 only", "Through Filter 2 only", "Through both filters", "Straight to the output, past the filters" });
            card.routeBar.setTooltip ("Which filter this oscillator goes through");
            card.routeBar.getChosen = [this, i] { return routeToSegment (readChoice (juce::String (OscillatorIds::prefixes[(size_t) i]) + "_route")); };
            card.routeBar.onChoose = [this, i] (int segment)
            {
                if (auto* parameter = processorRef.apvts.getParameter (juce::String (OscillatorIds::prefixes[(size_t) i]) + "_route"))
                    processorRef.performEdit ("OSC " + juce::String (i + 1) + " filter route", [parameter, segment]
                    {
                        parameter->beginChangeGesture();
                        parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) segmentToRoute (segment)));
                        parameter->endChangeGesture();
                    });
            };
            card.outBar.setPart (OscStatusView::Part::barOnly);
            for (auto* step : { &card.prevTable, &card.nextTable })
            {
                step->setColour (juce::TextButton::buttonColourId, IlanaTheme::Ui::raised);
                step->setColour (juce::TextButton::textColourOffId, IlanaTheme::Ui::text);
            }
            card.prevTable.setTooltip ("The previous wavetable in the list");
            card.nextTable.setTooltip ("The next wavetable in the list");
            card.prevTable.onClick = [this, i] { stepTable (i, -1); };
            card.nextTable.onClick = [this, i] { stepTable (i, 1); };
            card.exciterPills.setColour (family);
            card.exciterPills.setItems (exciterIcons());
            card.exciterPills.getChosen = [this, i] { return readChoice (juce::String (OscillatorIds::prefixes[(size_t) i]) + "_excite"); };
            card.exciterPills.onChoose = [this, i] (int choice)
            {
                if (auto* parameter = processorRef.apvts.getParameter (juce::String (OscillatorIds::prefixes[(size_t) i]) + "_excite"))
                    processorRef.performEdit ("OSC " + juce::String (i + 1) + " exciter", [parameter, choice]
                    {
                        parameter->beginChangeGesture();
                        parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) choice));
                        parameter->endChangeGesture();
                    });
                updateModeVisibility();
                updateEnabled();
            };
            addChildComponent (card.exciterPills);
            addChildComponent (card.typeBar);
            addChildComponent (card.routeBar);
            addChildComponent (card.outBar);
            addChildComponent (card.prevTable);
            addChildComponent (card.nextTable);

            addChildComponent (waveDisplay (i));
            scrubbers[(size_t) i].setColour (oscColour (i));
            scrubberAttachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                p.apvts, prefix + "_frame", scrubbers[(size_t) i]));
            addChildComponent (scrubbers[(size_t) i]);
            setupLoadButton (loadButton (i), prefix + "_table", 0);
            addChildComponent (loadButton (i));
            auto& edit = *editButtons[(size_t) i];
            edit.setTooltip ("Edit this wavetable: draw frames, set harmonics, formulas and morphs.\n"
                             "A factory table is copied into one of the patch's 16 tables first.");
            edit.onClick = [this, i] { openTableEditor (i); };
            addChildComponent (edit);
            auto& bounce = *bounceButtons[(size_t) i];
            bounce.setTooltip ("Resample: play the whole patch (optionally with its effects) and put the result on this "
                               "oscillator: one note as a tuned sample or a wavetable, or a multisample (several notes "
                               "and velocity layers, one zone each). The result is saved inside the patch.");
            bounce.onClick = [this, i] { showBounceMenu (i); };
            addChildComponent (bounce);

            // The header's actions read as buttons, not as tabs (UI review 6,
            // V40): a raised box at the controls' height.
            for (auto* button : { &loadButton (i), &edit, &bounce })
                styleHeaderButton (*button);

            // The Operator Env has one editor, on FM's operator card; here a
            // picture of it and a link there (UI review 8, S8-4).
            styleJumpLink (card.opEnvButton, "OP ENV");
            card.opEnvButton.setTooltip ("This operator plays its OP ENV: edit it on the FM page");
            card.opEnvButton.onClick = [this, i]
            {
                if (auto* editor = findParentComponentOfClass<IlanaSynthAudioProcessorEditor>())
                    editor->showOperatorEnvelope (i);
            };
            addChildComponent (card.opEnvButton);

            // A Physical oscillator's card holds its whole string and exciter
            // (the PHYSICAL tab folded in, 2026-10-10); the body it shares
            // with the other voices is on FILTER, the soundboard in the strip
            // below.
            styleJumpLink (card.stringButton, "BODY");
            card.stringButton.setTooltip (juce::String::fromUTF8 ("The resonant body and the soundboard: PLAY's BODY \xc2\xb7 BOARD card"));
            card.stringButton.onClick = [this]
            {
                if (auto* editor = findParentComponentOfClass<IlanaSynthAudioProcessorEditor>())
                    editor->showPage ("BODY");
            };
            addChildComponent (card.stringButton);

            // The header's x removes the oscillator (ilana 2026-10-26: "an easier way to ... delete oscs").
            card.removeButton.setTooltip ("Remove this oscillator (undo brings it back). Drag the header to move the card; right-click it to duplicate");
            card.removeButton.onClick = [this, i] { removeOscillatorCard (i); };
            styleHeaderButton (card.removeButton);
            card.removeButton.getProperties().set ("packedButton", true);
            addChildComponent (card.removeButton);

            card.sampleLoadButton.setButtonText ("LOAD...");
            card.sampleLoadButton.setTooltip ("Load a sample or an SF2 / SFZ multisample, or pick a factory sample.  You can also drop a .wav or .sfz file on the picture");
            card.sampleLoadButton.onClick = [this, i] { waveDisplay (i).showSampleMenu (aux[(size_t) i]->sampleLoadButton); };
            styleHeaderButton (card.sampleLoadButton);
            addChildComponent (card.sampleLoadButton);

            // The sample's name in the header, a menu button as the table's is (a click is LOAD's menu).
            card.sampleNameButton.setTooltip ("The sample this oscillator plays. Click to load another or pick a factory sample");
            card.sampleNameButton.onPopup = [this, i] { waveDisplay (i).showSampleMenu (aux[(size_t) i]->sampleNameButton); };
            addChildComponent (card.sampleNameButton);
            addChildComponent (card.xfadeBar);

            // Roadmap step 44: the sample (a resampled sound too) out as a WAV.
            card.sampleSliceButton.setButtonText ("SLICE");
            card.sampleSliceButton.setTooltip ("Cut the sample into slices, each on its own key: by transients, by beat (equal parts) or at markers you place. "
                                               "Right-click the wave for the same choices; right-click a slice to play it backwards");
            card.sampleSliceButton.onClick = [this, i] { waveDisplay (i).showSliceMenu (aux[(size_t) i]->sampleSliceButton); };
            styleHeaderButton (card.sampleSliceButton);
            addChildComponent (card.sampleSliceButton);

            card.sampleExportButton.setButtonText ("EXPORT");
            card.sampleExportButton.setTooltip ("Save this sample as a WAV file: the played range (START to END, with REVERSE and the "
                                                "fades, as the card plays it) or the whole sample. A resampled sound is saved the same way.");
            card.sampleExportButton.onClick = [this, i] { showExportMenu (i); };
            styleHeaderButton (card.sampleExportButton);
            addChildComponent (card.sampleExportButton);

            // Roadmap step 36: the zone map editor.
            card.sampleZonesButton.setButtonText ("ZONES");
            card.sampleZonesButton.setTooltip ("Edit the zone map: drag each zone's keys and velocities, set its root note and level, "
                                               "add samples, hear a zone, and choose what plays outside the zones, round robin and "
                                               "velocity blending.");
            card.sampleZonesButton.onClick = [this, i]
            {
                if (auto* editor = findParentComponentOfClass<IlanaSynthAudioProcessorEditor>())
                    editor->openZoneEditor (i, oscColour (i));
            };
            styleHeaderButton (card.sampleZonesButton);
            addChildComponent (card.sampleZonesButton);

            // Step 29: DETECT finds the sample's pitch and sets ROOT to it.
            card.sampleDetectButton.setButtonText ("DETECT");
            card.sampleDetectButton.setTooltip ("Find the pitch of the sample and set ROOT to it. You can still change ROOT by hand");
            card.sampleDetectButton.onClick = [this, i]
            {
                const auto message = processorRef.detectSampleRoot (i);
                // Found: the ROOT knob shows the note. Otherwise say why not.
                if (! message.startsWith ("Root set"))
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Detect root note", message);
            };
            styleHeaderButton (card.sampleDetectButton);
            addChildComponent (card.sampleDetectButton);
        }

        // The TABLE lists open the wavetable browser.
        for (int i = 0; i < OscillatorIds::count; ++i)
        {
            auto* control = &controls[(size_t) i]->table;
            const auto id = juce::String (OscillatorIds::prefixes[(size_t) i]) + "_table";
            control->setPopupOverride ([this, i, control, id]
            {
                const auto colour = oscColour (i);
                juce::Component::SafePointer<OscPage> safe (this);
                TableBrowser::show (processorRef, id, colour, control->getComboBox(), [this, i] { loadButton (i).triggerClick(); },
                                    [safe, id] (const juce::File& file)
                                    {
                                        if (safe != nullptr)
                                            safe->loadTableFile (id, 0, file, Wavetable::LoadMode::Automatic);
                                    });
            });
        }

        // The strip under the oscillators: voice, stereo, noise, sub, strings
        // and soundboard in one card, no tabs.
        voiceSpread = std::make_unique<KnobControl> (p.apvts, "voice_spread", "SPREAD");
        unisonRandom = std::make_unique<KnobControl> (p.apvts, "unison_random", "PHASE");
        // The oscillators' analogue drift (the vector pad's drift is WANDER:
        // UI review 6, I6-25).
        drift = std::make_unique<KnobControl> (p.apvts, "drift", "DRIFT");
        // A random offset per note, up to 40 ct (LOOSE), sharp or flat or flat only.
        tuneLoose = std::make_unique<KnobControl> (p.apvts, "tune_loose", "LOOSE");
        // The switch is the sub's alone, and says so; the noise has its
        // own level and colour beside it (V8-14, V8-15).
        subOscOn = std::make_unique<ToggleControl> (p.apvts, "subosc_on", "SUB");
        subOscLevel = std::make_unique<KnobControl> (p.apvts, "subosc_level", "LEVEL", IlanaTheme::accent(), true);
        noiseStrip = std::make_unique<KnobControl> (p.apvts, "noise_level", "LEVEL", IlanaTheme::Ui::text2, false);
        noiseColourStrip = std::make_unique<KnobControl> (p.apvts, "noise_color", "COLOUR", IlanaTheme::Ui::text2, false);
        addChildComponents (subShape, subOctave, noiseType, *subOscLevel, *noiseStrip, *noiseColourStrip, *subOscOn, *voiceSpread, *unisonRandom, *drift, *tuneLoose,
                            symOn, symAmount, symDecay, symCount, symManual, looseBoth,
                            sbOn, sbModel, sbMix, sbTone, sbSize, stretch, pedalRes, mechKey, mechDamper, mechPedal);
        // The strings' six notes live in the tuning popover (MANUAL TUNING's chevron opens it).
        for (auto& note : symNotes)
            tuningPopover.addAndMakeVisible (*note);
        addChildComponent (tuningPopover);
        sbOn.showAsSwitch();

        for (auto* knob : stripKnobs())
        {
            knob->setInlineKnob (true);
            knob->setCapDialLikeRings (true);
        }
        for (auto* menu : { &subShape, &subOctave, &sbModel, &noiseType })
            menu->setCompactLayout (true);
        // Under its OCTAVE name the menu reads "-1" / "-2", as the mockup's
        // (re-selected so the box shows the new text, as the exciter menu does).
        {
            auto& box = subOctave.getComboBox();
            const auto selectedId = box.getSelectedId();
            for (int item = 0; item < box.getNumItems(); ++item)
                box.changeItemText (box.getItemId (item), box.getItemText (item).upToFirstOccurrenceOf (" ", false, false));
            box.setSelectedId (0, juce::dontSendNotification);
            box.setSelectedId (selectedId, juce::dontSendNotification);
        }
        symManual.setInlineLabel (true);
        looseBoth.setInlineLabel (true);
        // (The one-row strip, ilana's mockup 2026-10-22: each card's name in its header, its switch at the header's right.)
        for (auto* group : { subOscOn.get(), &symOn, &sbOn })
            group->setBareSwitch (true);
        subOscOn->getButton().setTooltip ("Switch the sub oscillator on or off");
        symOn.getButton().setTooltip ("Sympathetic strings: shared drone strings that ring with everything you play");
        sbOn.getButton().setTooltip ("The acoustic keys' body: soundboard, stretch tuning, sustain pedal (CC64) resonance and the action's noises");

        tuneButton.setTooltip ("The sympathetic strings' six notes (with Manual tuning on)");
        tuneButton.onClick = [this] { showTuning (! tuningPopover.isVisible() || tuningGrow.isClosing()); };
        tuningPopover.isOwnButton = [this] (juce::Component* c) { return c == &tuneButton || tuneButton.isParentOf (c) || c == &symManual || symManual.isParentOf (c); };
        tuningPopover.onClose = [this] { showTuning (false); };
        styleHeaderButton (tuneButton);
        addChildComponent (tuneButton);
        // Switching Manual tuning on opens its notes.
        symManual.getButton().onClick = [this]
        {
            if (symManual.getButton().getToggleState())
                showTuning (true);
            else
                showTuning (false);
        };

        // (Not drawn: the tests pick an oscillator by this list, as they did by the old tabs.)
        oscTabs.setName ("OSC tabs");
        oscTabs.onPick = [this] (int osc) { selectOscillator (osc); };
        addChildComponent (oscTabs);

        for (const auto* prefix : OscillatorIds::prefixes)
            for (const auto* suffix : listenedSuffixes)
                processorRef.apvts.addParameterListener (juce::String (prefix) + suffix, this);

        for (const auto* id : listenedIds)
            processorRef.apvts.addParameterListener (id, this);

        // Controls that do nothing in the oscillator's current settings dim
        // (one rule for every page, EffectRules).
        for (int i = 0; i < OscillatorIds::count; ++i)
        {
            const juce::String prefix (OscillatorIds::prefixes[(size_t) i]);
            auto& osc = *controls[(size_t) i];
            effectRules.add (osc.spectralAmt, effectRules.choiceIsNot (prefix + "_spectral", 0), "SPECTRAL is Off");
            effectRules.add (osc.warpAmt, effectRules.choiceIsNot (prefix + "_warp", 0), "WARP is Off");
            effectRules.add (osc.scaleRoot, effectRules.choiceIsNot (prefix + "_scale", 0), "SCALE is Off");

            effectRules.add (osc.detune, effectRules.isAbove (prefix + "_unison", 1.5f), "UNISON is 1");
            // Grains have no unison blend or spread: the knobs keep their
            // columns, dimmed, with a dash for the value.
            for (auto* knob : { &osc.uniBlend, &osc.uniFrame, &osc.uniWarp, &osc.spread })
            {
                effectRules.add (*knob, [this, i, voices = effectRules.isAbove (prefix + "_unison", 1.5f)] { return voices() && getMode (i) != 3; },
                                 "UNISON is 1 (a Granular oscillator has no unison blend or spread)");
                knob->setDashWhen ([this, i] { return getMode (i) == 3; });
            }

            // The sample's own controls dim until there is a sample (S14-3).
            for (juce::Component* control : { (juce::Component*) &osc.sampleLoop, (juce::Component*) &osc.sampleReverse, (juce::Component*) &osc.sampleSnap, (juce::Component*) &osc.sampleStart,
                                              (juce::Component*) &osc.sampleEnd, (juce::Component*) &osc.sampleFadeIn, (juce::Component*) &osc.sampleFadeOut,
                                              (juce::Component*) &osc.sampleSync, (juce::Component*) &osc.sampleBpm, (juce::Component*) &osc.sampleStretch,
                                              (juce::Component*) &osc.samplePlayback, (juce::Component*) &osc.sampleStartRandom, (juce::Component*) &osc.sampleQuality })
                effectRules.add (*control, [this, i]
                                 {
                                     const auto* sample = processorRef.getSampleForOsc (i);
                                     return sample != nullptr && sample->getNumSamples() >= 2;
                                 },
                                 "load a sample first");

            // Step 29: ROOT is for a plain sample; a multisample keeps its zone roots.
            effectRules.add (osc.sampleRoot, [this, i]
                             {
                                 const auto* sample = processorRef.getSampleForOsc (i);
                                 return sample != nullptr && sample->getNumSamples() >= 2 && sample->zones.empty();
                             },
                             "load a plain sample (a multisample has a root per zone)");

            // Step 27: LOOP PTS only acts while LOOP is on.
            effectRules.add (osc.sampleLoopOwn, [this, i, loopOn = effectRules.isOn (prefix + "_sample_loop")]
                             {
                                 const auto* sample = processorRef.getSampleForOsc (i);
                                 return sample != nullptr && sample->getNumSamples() >= 2 && loopOn();
                             },
                             "turn LOOP on (and load a sample) first");
            // Step 34: ping-pong turns the loop, so it needs one: LOOP or a Sustain Loop.
            effectRules.add (osc.samplePingPong, [this, i, loopOn = effectRules.isOn (prefix + "_sample_loop"), prefix]
                             {
                                 const auto* sample = processorRef.getSampleForOsc (i);
                                 const auto playback = juce::roundToInt (readFloat (prefix + "_sample_playback"));
                                 return sample != nullptr && sample->getNumSamples() >= 2 && playback != 1 && (loopOn() || playback == 2);
                             },
                             "turn LOOP on or pick Sustain Loop (and load a sample) first");
            // Step 35: TRANSIENT SAFE moves a random start, so it needs one.
            effectRules.add (osc.sampleStartSafe, [this, i, prefix]
                             {
                                 const auto* sample = processorRef.getSampleForOsc (i);
                                 return sample != nullptr && sample->getNumSamples() >= 2 && readFloat (prefix + "_sample_start_rand") > 0.0f;
                             },
                             "turn RANDOM START up first");
            // Step 28: the crossfade blends the loop's wrap, so it needs LOOP too.
            effectRules.add (osc.sampleLoopXfade, [this, i, loopOn = effectRules.isOn (prefix + "_sample_loop")]
                             {
                                 const auto* sample = processorRef.getSampleForOsc (i);
                                 return sample != nullptr && sample->getNumSamples() >= 2 && loopOn();
                             },
                             "turn LOOP on (and load a sample) first");
        }

        // One way to add an oscillator here: the dashed row under the last card
        // (UI review 6, S33).
        addButton.setTooltip ("Add the next oscillator, switched on");
        addButton.onClick = [this] { addOscillatorOfType (-1); };
        addChildComponent (addButton);
        // FM / DX7 has its own way in (an operator: a sine tuned by ratio on
        // the Operator EG), beside the plain one.
        addFmButton.setTooltip ("Add the next oscillator as an FM / DX7 operator: a sine tuned by ratio, on its OP ENV");
        addFmButton.onClick = [this] { addOscillatorOfType (OscMode::fmOperator); };
        addChildComponent (addFmButton);
        // On PLAY, with room under one or two cards, both buttons sit on the drop tile.
        addChildComponent (dropTile);
        dropTile.onFileDropped = [this] (const juce::File& file) { addOscillatorFromFile (file); };

        lastRevealVersion = processorRef.getRevealVersion();
        updateModeVisibility();
        updateEnabled();
        startTimerHz (5);
        IlanaTheme::uiScaleListeners().push_back (this);
    }

    // The zoom changed: text is laid out by its drawn width, so the cards lay out again.
    void uiScaleChanged() override
    {
        if (getWidth() > 0 && ! juce::approximatelyEqual (laidOutScale, IlanaTheme::uiScaleRef()))
        {
            laidOutScale = IlanaTheme::uiScaleRef();
            layoutPage();
            repaint();
        }
    }

    ~OscPage() override
    {
        IlanaTheme::identityColourListeners (&processorRef).remove (this);
        auto& listeners = IlanaTheme::uiScaleListeners();
        listeners.erase (std::remove (listeners.begin(), listeners.end(), static_cast<IlanaTheme::UiScaleListener*> (this)), listeners.end());
        for (const auto* prefix : OscillatorIds::prefixes)
            for (const auto* suffix : listenedSuffixes)
                processorRef.apvts.removeParameterListener (juce::String (prefix) + suffix, this);

        for (const auto* id : listenedIds)
            processorRef.apvts.removeParameterListener (id, this);
    }

    // Colour coding: an oscillator given another colour shows it on every
    // part of its card.
    void identityColoursChanged() override
    {
        for (int i = 0; i < OscillatorIds::count; ++i)
        {
            const auto colour = oscColour (i);
            auto& osc = *controls[(size_t) i];
            auto& card = *aux[(size_t) i];
            osc.setColour (colour);
            osc.on.setSwitchColour (colour);
            physical[(size_t) i]->setColour (colour);
            waveDisplay (i).setTraceColour (colour);
            for (auto* view : { &card.status, &card.outBar })
                view->setColour (colour);
            card.stringView.setColour (colour);
            card.spreadView.setColour (colour);
            card.partials.setColour (colour);
            card.typeBar.setColour (colour);
            card.routeBar.setColour (colour);
            scrubbers[(size_t) i].setColour (colour);
        }

        resized(); // (the packed layout sets the card switches' colours)
        repaint();
    }

    void parameterChanged (const juce::String&, float) override
    {
        // Parameter changes can arrive on the audio thread (host automation),
        // so defer the GUI work to the message thread.
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override
    {
        updateModeVisibility();
        updateEnabled();
    }

    // Patch loads change which oscillators are shown; FM routes (polled)
    // change what the cards say.
    void timerCallback() override
    {
        if (bouncingOsc >= 0)
            updateBounce();

        if (const auto version = processorRef.getRevealVersion(); version != lastRevealVersion)
        {
            lastRevealVersion = version;
            updateModeVisibility();
            updateEnabled();
        }
        else
        {
            updateTabItems();

            if (layoutKey() != shownKey)
                updateModeVisibility();
            else
                updateCardPictures();
        }

        effectRules.apply();

        // The TEMPO row's RENDERING note comes and goes with the renders.
        if (const auto rendering = processorRef.getStretchRendersPending() > 0; rendering != shownRendering)
        {
            shownRendering = rendering;
            repaint();
        }
    }

    bool shownRendering = false;

    void paint (juce::Graphics& g) override
    {
        IlanaTheme::paintPageBackground (g, getLocalBounds());

        for (const auto& card : cards)
            paintOscCard (g, card);

        if (! embedded)
            paintStrip (g);
    }

    // A right-click on a card's header: switch it off, duplicate or remove
    // it (the old tab's menu). A drag by the header moves the card (ilana
    // 2026-10-26: "drag to reorder"): a line shows where it lands.
    void mouseDown (const juce::MouseEvent& event) override
    {
        dragOsc = -1;
        for (const auto& card : cards)
            if (card.header.contains (event.getPosition()))
            {
                if (event.mods.isPopupMenu() && event.x < card.bounds.getX() + 28)
                    showColourPicker (card.osc);
                else if (event.mods.isPopupMenu())
                    showOscMenu (card.osc);
                else if (cards.size() > 1)
                    dragOsc = card.osc;
                return;
            }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (dragOsc < 0 || (! dragging && event.getDistanceFromDragStart() < 5))
            return;

        dragging = true;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        if (auto* viewport = findParentComponentOfClass<juce::Viewport>())
        {
            const auto inViewport = viewport->getLocalPoint (this, event.getPosition());
            viewport->autoScroll (inViewport.x, inViewport.y, 30, 12);
        }
        const auto position = dropPosition (event.y);
        if (position != dropAt)
        {
            dropAt = position;
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        const auto osc = dragOsc, position = dropAt;
        const auto moved = dragging && position >= 0 && position != positionOf (osc);
        dragOsc = dropAt = -1;
        dragging = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();

        if (moved)
        {
            const auto name = "OSC " + juce::String (osc + 1);
            auto& p = processorRef;
            auto landed = osc;
            p.performEdit ("Move " + name, [&p, &landed, osc, position] { landed = p.moveOscillator (osc, position); });
            selected = landed;
            updateModeVisibility();
            updateEnabled();
        }
    }

    // An open hand over a header's free space says it can be dragged.
    void mouseMove (const juce::MouseEvent& event) override
    {
        auto overHeader = false;
        for (const auto& card : cards)
            overHeader = overHeader || card.header.contains (event.getPosition());
        setMouseCursor (overHeader && cards.size() > 1 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (! dragging || dragOsc < 0)
            return;

        for (const auto& card : cards)
            if (card.osc == dragOsc)
            {
                g.setColour (oscColour (dragOsc));
                g.drawRoundedRectangle (card.bounds.toFloat().reduced (0.5f), 10.0f, 2.0f);
            }

        // The line between the cards it would land between (the gap's middle).
        if (dropAt < 0 || dropAt == positionOf (dragOsc))
            return;

        const auto target = dropAt > positionOf (dragOsc) ? dropAt + 1 : dropAt;
        const auto y = target < (int) cards.size() ? cards[(size_t) target].bounds.getY() - juce::jmax (1, gapNow()) / 2
                                                   : cards.back().bounds.getBottom() + juce::jmax (1, gapNow()) / 2;
        const auto& first = cards.front().bounds;
        g.setColour (oscColour (dragOsc));
        g.fillRoundedRectangle (juce::Rectangle<float> ((float) first.getX() + 4.0f, (float) y - 1.5f, (float) first.getWidth() - 8.0f, 3.0f), 1.5f);
    }

    // The card order (the shown oscillators, in slot order) of an oscillator.
    int positionOf (int osc) const
    {
        for (size_t i = 0; i < cards.size(); ++i)
            if (cards[i].osc == osc)
                return (int) i;
        return -1;
    }

    // Where a card dragged to y lands: the number of other cards above y's line.
    int dropPosition (int y) const
    {
        auto position = 0;
        for (const auto& card : cards)
            if (card.osc != dragOsc && y > card.bounds.getCentreY())
                ++position;
        return position;
    }

    // Duplicate: the copy lands under the original and is selected.
    void duplicate (int osc)
    {
        auto& p = processorRef;
        auto copy = -1;
        p.performEdit ("Duplicate OSC " + juce::String (osc + 1), [&p, &copy, osc] { copy = p.duplicateOscillator (osc); });
        if (copy >= 0)
            selected = copy;
        updateModeVisibility();
        updateEnabled();
    }

    static juce::Colour oscColour (int index) { return IlanaTheme::oscColour (index); }

    // The height the page needs: every shown card at its smallest, the add
    // row and the strip.
    int getMinimumHeight() const
    {
        return contentHeight (minimumHeights());
    }

    // The height the same cards would need as wavetable cards: PLAY sizes its module bands by this, so a taller
    // card (a sample, a string) never squeezes the bands: the oscillator area scrolls instead.
    int getReferenceHeight() const { return referenceHeightFor (numShown()); }

    // What PLAY sizes its two module bands by: always two wavetable cards and the add row, the layout the bands
    // were approved under (81 / 104 px). A third card scrolls a little rather than the bands shrinking (ilana
    // 2026-10-22; UI review 26 found three-oscillator presets squashing band 2 to 89 px).
    int getBandsReferenceHeight() const { return referenceHeightFor (2); }

    int referenceHeightFor (int shown) const
    {
        // (Sized by the wavetable card PLAY's bands were approved under, 28 px warp rows and 12 px chip: the OSC tab's
        // cards are 6 px taller, and the oscillators scroll that little rather than the bands shrinking, ilana 2026-10-22.)
        const auto wavetableCard = 1 + cardHeaderHeight + panelTop + panelTitleH + (2 * 28 + 1 + 3 + 12) + panelTail + 1 + cardPad + 1;
        // (Never more than the three cards PLAY shows at once: a fourth card, and the add row under a third, scroll.)
        const auto counted = juce::jmin (shown, embeddedAddRowBelow);
        auto total = topNow() + (embedded ? 0 : stripHeight()) + bottomNow() + counted * (wavetableCard + gapNow());
        if (counted < embeddedAddRowBelow)
            total += addRowHeight + gapNow();
        return total;
    }

    void resized() override
    {
        laidOutScale = IlanaTheme::uiScaleRef();
        layoutPage();
    }

    // The oscillator the UI test or the FM page's links pick: its card is
    // scrolled into view.
    void selectOscillator (int osc)
    {
        if (osc < 0 || osc >= OscillatorIds::count || ! processorRef.isOscillatorShown (osc))
            return;

        selected = osc;
        updateModeVisibility();
        updateEnabled();
        scrollTo (osc);
    }

    int getSelectedOscillator() const { return selected; }

    // PLAY hosts the cards alone: no VOICE / NOISE / SUB / STRINGS strip below them.
    void setEmbedded (bool shouldEmbed)
    {
        if (embedded == shouldEmbed)
            return;

        embedded = shouldEmbed;
        updateModeVisibility();
    }

    // The drop tile: a new oscillator takes the file the way its picture would (a wavetable card asks what the
    // audio is for; a SoundFont loads as a sample).
    void addOscillatorFromFile (const juce::File& file)
    {
        if (! file.existsAsFile() || ! canAdd())
            return;
        const auto before = numShown();
        addOscillatorOfType (-1);
        if (numShown() == before)
            return;
        juce::Component::SafePointer<OscPage> safeThis (this);
        const auto index = selected;
        juce::MessageManager::callAsync ([safeThis, index, file]
        {
            if (safeThis != nullptr)
                safeThis->waveDisplay (index).filesDropped ({ file.getFullPathName() }, 0, 0);
        });
    }

    // Adds the next oscillator (one undo step): as it was (-1: a Wavetable
    // unless it had a type), or as that type. The add row and the UI tests.
    void addOscillatorOfType (int mode)
    {
        for (int i = 0; i < OscillatorIds::count; ++i)
            if (! processorRef.isOscillatorShown (i))
            {
                processorRef.performEdit ((mode == OscMode::fmOperator ? "Add FM / DX7 OSC " : "Add OSC ") + juce::String (i + 1), [this, i, mode]
                {
                    if (mode >= 0)
                        processorRef.addOscillator (i, mode);
                    else
                        processorRef.addOscillator (i);
                });
                selected = i;
                break;
            }

        updateModeVisibility();
        updateEnabled();
        // The new card scrolls into view, then slides down and fades in.
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<OscPage> (this), osc = selected]
        {
            if (safe == nullptr)
                return;
            safe->scrollTo (osc);
            IlanaAnim::CardMotion::reveal (*safe, safe->getCardBounds (osc), [page = safe.getComponent()] (juce::Graphics& g)
                                           { IlanaTheme::paintPageBackground (g, page->getLocalBounds()); });
        });
    }

    // Removes an oscillator (one undo step); its card folds away.
    void removeOscillatorCard (int osc)
    {
        auto picture = IlanaAnim::CardMotion::picture (*this, getCardBounds (osc));
        const auto area = getCardBounds (osc);
        auto& p = processorRef;
        p.performEdit ("Remove OSC " + juce::String (osc + 1), [&p, osc] { p.removeOscillator (osc); });
        updateModeVisibility();
        updateEnabled();
        IlanaAnim::CardMotion::collapse (*this, area, std::move (picture));
    }

    // The strip under the cards: one row (STEREO, NOISE, SUB, STRINGS,
    // SOUNDBOARD, KEYS); VOICE is in the dock. This scrolls it into view.
    enum SharedTab { sharedVoice = 0, sharedSubNoise, sharedSympathetic, sharedKeys };

    void selectShared (int)
    {
        updateModeVisibility();
        scrollTo (-1);
    }

    // The UI test reads the menu's items.
    juce::StringArray getOscMenuItems (int band) const
    {
        return { isOff (band) ? "Switch on" : "Switch off", "Duplicate oscillator", "Remove oscillator" };
    }

    // The card's rectangle in the page (the UI tests).
    juce::Rectangle<int> getCardBounds (int osc) const
    {
        for (const auto& card : cards)
            if (card.osc == osc)
                return card.bounds;

        return {};
    }

private:
    static constexpr const char* listenedSuffixes[] { "_mode", "_on", "_excite", "_warp", "_warp2", "_spectral", "_tune", "_amp_env" };
    static constexpr const char* listenedIds[] { "sym_on", "sym_manual", "sym_count", "sb_on", "subosc_on", "noise_level", "voice_mode" };

    template <typename... Components>
    void addChildComponents (Components&... components)
    {
        (addChildComponent (components), ...);
    }

    static void styleHeaderButton (juce::TextButton& button)
    {
        button.setColour (juce::TextButton::buttonColourId, IlanaTheme::Ui::raised);
        button.setColour (juce::TextButton::textColourOffId, IlanaTheme::Ui::text);
    }

    // The page's measures, in design pixels. Every card is the same width; a
    // wavetable card is the one compact height; the page scrolls once the
    // cards and the strip no longer fit.
    // (Review 23: one margin all round and one gap, as the mockup's 10 px at 1.5x; the
    // header is 23 high.)
    static constexpr int sideMargin = 7, topMargin = 7, bottomMargin = 7, gap = 7;
    static constexpr int cardHeaderHeight = 23, compactCardMin = 118, compactCardMax = 130;
    // The packed cards' panels (wavetable, physical), in units: the mockup's css px / 1.5.
    static constexpr int cardPad = 5, panelGap = 4, panelTop = 5, panelTitleH = 12, panelTail = 4;
    static constexpr int pitchW = 160, mixW = 164, unisonW = 270;
    // (PLAY and OSC draw one card: the same columns on both, ilana 2026-10-21.)
    int pitchColumn() const { return pitchW; }
    int unisonColumn() const { return unisonW; }
    static constexpr int mainDial = 29, ampDial = 20, nameLine = 10, knobCell = 35, ampCell = 27, menuH = 16;
    static constexpr int ampRowH = 30, chipH = 14, filterH = 15, outH = 12, spreadH = 17;
    static constexpr int addRowHeight = 18;
    // The strip: one row of cards (ilana's mockup, 2026-10-22), in design px.
    static constexpr int stripCardH = 130, stripHeaderH = 24, stripGap = 6, stripPad = 8, stripDial = 36, stripCell = 44,
                         stripMenuW = 46, stripCellStereo = 42, stripWrapLine = 16, stripDialTop = 36, stripLaneTop = 98, stripLaneH = 22;
    static constexpr int wellWidth = 150, rowLabelWidth = 50, colGap = 6, rowGap = 4;
    static constexpr int sampleRowHeight = 48, sampleLabelWidth = 96, sampleColumns = 7, sampleWaveMin = 190;

    // fm: the FM / DX7 type, the operator's card (its tuning, the Operator EG
    // or another envelope, feedback); wavetable: the table's card.
    enum class Kind { wavetable, fm, physical, sample, grain, live };

    Kind kindOf (int osc) const
    {
        switch (getMode (osc))
        {
            case 1: return Kind::physical;
            case 2: return Kind::sample;
            case 3: return Kind::grain;
            case 4: return Kind::live;
            case OscMode::fmOperator: return Kind::fm;
            default: return Kind::wavetable;
        }
    }

    static bool isCompactKind (Kind kind) { return kind != Kind::sample; }

    // One cell of a card's grid: the control, its row, first column and width in columns.
    struct Slot
    {
        juce::Component* item;
        int row, col, span;
    };

    // The compact cards' three rows on one nine-column grid (the sample card's
    // two or three on seven): what stands in each cell. The same column holds
    // the same kind of control from card to card.
    std::vector<Slot> slotsFor (int index) const
    {
        auto& osc = *controls[(size_t) index];
        const auto kind = kindOf (index);
        const juce::String prefix (OscillatorIds::prefixes[(size_t) index]);
        std::vector<Slot> slots;
        const auto add = [&slots] (juce::Component& item, int row, int col, int span = 1) { slots.push_back ({ &item, row, col, span }); };
        switch (kind)
        {
            case Kind::wavetable:
            {
                // (Panels, review 23: the cells below are only the list of what shows;
                // layoutWavetablePanels places them; the table's menu is the header's.)
                add (osc.table, 0, 0);
                add (osc.frameBlend, 0, 0);
                add (osc.warp, 0, 0);
                add (osc.warpAmt, 0, 0);
                add (osc.spectral, 0, 0);
                add (osc.spectralAmt, 0, 0);
                add (osc.warp2, 0, 0);
                add (osc.warp2Amt, 0, 0);
                add (osc.level, 1, 0);
                add (osc.pan, 1, 0);
                add (osc.tune, 1, 0);
                const auto tuning = OscRole::tuning (processorRef, index);
                add (tuning == OscTuning::Ratio ? (juce::Component&) osc.ratio
                                                : tuning == OscTuning::Fixed ? (juce::Component&) osc.fixedHz : (juce::Component&) osc.semi, 1, 0);
                add (osc.fine, 1, 0);
                add (osc.ampEnv, 1, 0);
                add (osc.scale, 1, 0);
                if (readChoice (prefix + "_scale") != 0)
                    add (osc.scaleRoot, 1, 0);
                add (osc.chord, 1, 0);
                add (osc.uniMode, 2, 0);
                add (osc.unison, 2, 0);
                add (osc.detune, 2, 0);
                add (osc.uniBlend, 2, 0);
                add (osc.spread, 2, 0);
                add (osc.uniFrame, 2, 0);
                add (osc.uniWarp, 2, 0);
                break;
            }
            case Kind::fm:
            {
                // Panels (review 23): the list of what shows; layoutFmPanels places it.
                // OUTPUT is the Operator EG's level; on another envelope the
                // oscillator's LEVEL stands there. An operator has no unison spread
                // (review 8, I8-15); a table's shapers show only while one is in use.
                const auto tuning = OscRole::tuning (processorRef, index);
                add (osc.table, 0, 0);
                add (osc.feedback, 0, 0);
                add (osc.feedbackType, 0, 0);
                add (osc.semi, 1, 0);
                add (osc.fine, 1, 0);
                if (tuning == OscTuning::Ratio)
                    add (osc.ratio, 1, 0);
                if (tuning == OscTuning::Fixed)
                    add (osc.fixedHz, 1, 0);
                add (osc.tune, 1, 0);
                add (osc.scale, 1, 0);
                if (readChoice (prefix + "_scale") != 0)
                    add (osc.scaleRoot, 1, 0);
                add (osc.chord, 1, 0);
                add (OscRole::usesOperatorEg (processorRef, index) ? (juce::Component&) osc.egOut : (juce::Component&) osc.level, 2, 0);
                add (osc.pan, 2, 0);
                add (osc.ampEnv, 2, 0);
                // An operator has no unison (review 8, I8-15): on the Operator EG its VOICES knob
                // shows only once raised; on another envelope it stands in the last panel.
                if (! OscRole::usesOperatorEg (processorRef, index) || readFloat (prefix + "_unison") > 1.5f)
                    add (osc.unison, 3, 0);
                if (! OscRole::usesOperatorEg (processorRef, index))
                {
                    // (The whole unison, as the wavetable card's, so the panel is not left with one dial.)
                    add (osc.uniMode, 3, 0);
                    add (osc.detune, 3, 0);
                    add (osc.uniBlend, 3, 0);
                    add (osc.spread, 3, 0);
                    const std::pair<ComboControl*, KnobControl*> shapers[] { { &osc.warp, &osc.warpAmt }, { &osc.warp2, &osc.warp2Amt },
                                                                            { &osc.spectral, &osc.spectralAmt } };
                    for (const auto& [menu, amount] : shapers)
                        if (readChoice (prefix + juce::String (menu == &osc.warp ? "_warp" : menu == &osc.warp2 ? "_warp2" : "_spectral")) != 0)
                        {
                            add (*menu, 3, 0);
                            add (*amount, 3, 0);
                        }
                }
                break;
            }
            case Kind::physical:
            {
                // Panels (review 23): the list of what shows; layoutPhysicalPanels places it.
                const auto rows = physicalControlRows (juce::roundToInt (readFloat (prefix + "_excite")));
                const auto& lookup = physicalLookup[(size_t) index];
                for (const auto& row : rows)
                    for (const auto& spec : row.second)
                    {
                        const juce::String suffix (spec.suffix);
                        const auto found = lookup.find (suffix);
                        if (suffix != "_excite" && found != lookup.end() && processorRef.apvts.getParameter (prefix + suffix) != nullptr)
                            add (*found->second, 0, 0);
                    }
                auto& phys = *physical[(size_t) index];
                add (phys.resAmount, 1, 0);
                add (phys.resOn, 1, 0);
                add (phys.resBoard, 1, 0);
                add (osc.level, 1, 0);
                add (osc.pan, 1, 0);
                add (osc.semi, 1, 0);
                add (osc.fine, 1, 0);
                add (osc.chord, 1, 0);
                add (osc.uniMode, 2, 0);
                add (osc.unison, 2, 0);
                add (osc.detune, 2, 0);
                add (osc.uniBlend, 2, 0);
                add (osc.spread, 2, 0);
                break;
            }
            case Kind::grain:
            {
                // Panels (review 23): the list of what shows; layoutGranularPanels places it.
                add (osc.grainPosition, 0, 0);
                add (osc.grainScan, 0, 0);
                add (osc.grainFreeze, 0, 0);
                add (osc.grainSize, 0, 0);
                add (osc.grainDensity, 0, 0);
                add (osc.grainSpray, 0, 0);
                add (osc.grainPitch, 0, 0);
                if (IlanaSynthAudioProcessor::isEffectBuild)
                    add (osc.grainLive, 0, 0);
                add (osc.semi, 1, 0);
                add (osc.fine, 1, 0);
                add (osc.sampleTuned, 1, 0);
                add (osc.sampleReverse, 1, 0);
                add (osc.scale, 1, 0);
                if (readChoice (prefix + "_scale") != 0)
                    add (osc.scaleRoot, 1, 0);
                add (osc.chord, 1, 0);
                add (osc.level, 2, 0);
                add (osc.pan, 2, 0);
                add (osc.ampEnv, 2, 0);
                // BLEND and SPREAD keep their places, dimmed with a dash: grains have no
                // unison blend or spread (controls dim in place); STEREO is the grains' own.
                add (osc.uniMode, 3, 0);
                add (osc.unison, 3, 0);
                add (osc.detune, 3, 0);
                add (osc.uniBlend, 3, 0);
                add (osc.spread, 3, 0);
                add (osc.grainSpread, 3, 0);
                break;
            }
            case Kind::live:
                // The input itself: no pitch, shape or unison.
                add (osc.level, 0, 0);
                add (osc.pan, 0, 1);
                add (osc.ampEnv, 0, 5);
                break;
            case Kind::sample:
                // Panels (review 23): the list of what shows; layoutSamplePanels places it.
                add (osc.sampleTuned, 0, 0);
                add (osc.sampleLoop, 0, 0);
                add (osc.sampleReverse, 0, 0);
                add (osc.sampleLoopOwn, 0, 0);
                add (osc.sampleSnap, 0, 0);
                add (osc.sampleLoopXfade, 0, 0);
                add (osc.semi, 1, 0);
                add (osc.fine, 1, 0);
                add (osc.sampleRoot, 1, 0);
                add (osc.scale, 1, 0);
                if (readChoice (prefix + "_scale") != 0)
                    add (osc.scaleRoot, 1, 0);
                add (osc.chord, 1, 0);
                add (osc.level, 2, 0);
                add (osc.pan, 2, 0);
                add (osc.ampEnv, 2, 0);
                add (osc.uniMode, 3, 0);
                add (osc.unison, 3, 0);
                add (osc.detune, 3, 0);
                add (osc.uniBlend, 3, 0);
                add (osc.spread, 3, 0);
                add (osc.sampleStart, 4, 0);
                add (osc.sampleEnd, 4, 0);
                add (osc.sampleFadeIn, 4, 0);
                add (osc.sampleFadeOut, 4, 0);
                add (osc.sampleSync, 5, 0);
                add (osc.sampleStretch, 5, 0);
                add (osc.sampleBpm, 5, 0);
                add (osc.samplePlayback, 6, 0);
                add (osc.sampleQuality, 6, 0);
                add (osc.samplePingPong, 6, 0);
                add (osc.sampleStartSafe, 6, 0);
                add (osc.sampleStartRandom, 6, 0);
                break;
        }

        return slots;
    }

    // A tine's or reed's string and exciter fit one row (eight cells).
    bool physicalRowsMerged (int osc) const
    {
        return isElectric (osc);
    }

    // The rows a card shows: a Physical card adds its EXCITER row.
    // An operator's two rows still need the room its feedback knob's rings and the graph's labels take.
    int twoRowRoom (int osc) const { return kindOf (osc) == Kind::fm && rowsOf (osc) == 2 ? 10 : 0; }

    int rowsOf (int osc) const
    {
        const auto kind = kindOf (osc);
        if (kind == Kind::physical)
            return physicalRowsMerged (osc) ? 3 : 4;
        // An operator's third row (UNISON and any shapers in use) is only there while it holds something.
        if (kind == Kind::fm)
        {
            const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
            auto third = readFloat (prefix + "_unison") > 1.5f;
            for (const auto* stage : { "_warp", "_warp2", "_spectral" })
                third = third || readChoice (prefix + stage) != 0;
            return third ? 3 : 2;
        }
        return numRows (kind, kind != Kind::sample || unisonShown (osc));
    }

    bool showsRendering (int osc) const
    {
        return processorRef.getStretchRendersPending() > 0
               && readFloat (juce::String (OscillatorIds::prefixes[(size_t) osc]) + "_sample_stretch") > 1.5f;
    }

    // A card's row names: a Physical card's from its exciter.
    juce::String rowNameOf (int osc, int row) const
    {
        const auto kind = kindOf (osc);
        // TIME's engines render a copy in the background: the TEMPO row says so meanwhile.
        if (kind == Kind::sample && row == 2 && showsRendering (osc))
            return juce::String (juce::CharPointer_UTF8 ("TEMPO \xc2\xb7 RENDERING"));
        if (kind != Kind::physical)
            return rowName (kind, row);
        if (physicalRowsMerged (osc))
        {
            const auto tine = juce::roundToInt (readFloat (juce::String (OscillatorIds::prefixes[(size_t) osc]) + "_excite")) == 7;
            return row == 0 ? (tine ? "TINE" : "REED") : row == 1 ? "PITCH" : "UNISON";
        }
        return row == 0 ? "STRING" : row == 1 ? "EXCITER" : row == 2 ? "PITCH" : "UNISON";
    }

    // One sentence on what the exciter does, for the EXCITER row's free cells
    // (as the PHYSICAL tab had them, A16-4).
    // The exciter picker's tiles, in the parameter's order: each icon's glyph (18 x 16, from the approved
    // mockup v2, 2026-10-21) and, for its tooltip, the group and what the exciter does.
    static std::vector<IconPicker::Item> exciterIcons()
    {
        return {
            { "Burst", "Pluck. A short click of noise plucks the string.", { { "M1 9L3 9 4 3 5 13 6 5 7 11 8 6 9 10 11 8 17 8", 1.5f } } },
            { "Noise", "Pluck. A longer noise burst rubs the string.", { { "M1 9L2 5 3 11 4 4 5 12 6 7 7 10 8 2 9 13 10 6 11 11 12 4 13 9 14 6 15 12 16 7 17 9", 1.5f } } },
            { "Saw", "Pluck. A saw wave drives the string.", { { "M1 12L8 3V12L15 3V12H17", 1.5f } } },
            { "Pulse", "Pluck. A pulse wave drives the string.", { { "M1 12V4H7V12H12V4H17", 1.5f } } },
            { "Bow", "Bow. A steady bow keeps the string sounding.", { { "M2 12L15 2M5 14L17 4M3 5L15 13", 1.5f }, { "M4 6L6 7M11 10L13 11", 0.7f } } },
            { "Bright Hammer", "Strike. A hard felt strike: HAMMER sets the brightness.", { { "M4 3H12V7H4ZM8 7V13M1 14H17", 1.5f }, { "M14 2L15 4M16 6H18", 1.0f } } },
            { "Osc In", "Driven. Another oscillator drives the string.", { { "M1 8H7M4 5L7 8 4 11M9 8Q11 0 13 8T17 8", 1.5f } } },
            { "Tine", "Electric piano. A struck tine and its pickup.", { { "M2 3H13V5H5V13M2 3V14M11 9H17V14H11ZM8 12H11", 1.5f } } },
            { "Reed", "Electric piano. A struck reed and its pickup.", { { "M2 4H16L11 11H2ZM2 14H16M3 6H11", 1.5f } } },
            { "Piano", "Strike. A felt strike; HAMMER sets brightness.", { { "M1 3H17V14H1ZM6 3V14M12 3V14", 1.1f }, { "M4 3H7V9H4ZM10 3H13V9H10Z", 0.0f } } },
            { "Feedback", "Driven. The amp pushes sound back at the string.", { { "M15 5A6 6 0 1 0 15 11M12 2L16 5 12 7M5 8H11", 1.5f } } },
            { "Modal Piano", "Strike. A measured grand, partial by partial.", { { "M1 8H17V15H1ZM6 8V15M12 8V15", 1.1f }, { "M1 4Q3 1 5 4T9 4T13 4T17 4", 1.2f } } } };
    }

    juce::String physicalHint (int osc) const
    {
        static const char* const hints[] {
            "A short click of noise plucks the string.",
            "A longer noise burst rubs the string.",
            "A saw wave drives the string.",
            "A pulse wave drives the string.",
            "A steady bow keeps the string sounding.",
            "A hard felt strike: HAMMER sets the brightness.",
            "Another oscillator drives the string.",
            "A struck tine and its pickup.",
            "A struck reed and its pickup.",
            "A felt strike; HAMMER sets brightness.",
            "The amp pushes sound back at the string.",
            "A measured grand, partial by partial." };
        return hints[juce::jlimit (0, 11, juce::roundToInt (readFloat (juce::String (OscillatorIds::prefixes[(size_t) osc]) + "_excite")))];
    }

    bool unisonShown (int osc) const
    {
        return readFloat (juce::String (OscillatorIds::prefixes[(size_t) osc]) + "_unison") > 1.5f;
    }

    static juce::String rowName (Kind kind, int row)
    {
        switch (kind)
        {
            case Kind::wavetable: return row == 0 ? "SHAPE" : row == 1 ? "PITCH" : "UNISON";
            case Kind::fm: return row == 0 ? "PITCH" : row == 1 ? "WAVE" : "UNISON";
            // (Its third row is the wavetable's UNISON row, named as on every
            // card; its tooltip says they are copies of the string, I11-9.)
            case Kind::physical: return row == 0 ? "STRING" : row == 1 ? "PITCH" : "UNISON";
            case Kind::grain: return row == 0 ? "GRAINS" : row == 1 ? "PITCH" : "UNISON";
            case Kind::live: return "LEVEL";
            case Kind::sample: return row == 0 ? "SAMPLE" : row == 1 ? "PITCH & LEVEL" : row == 2 ? "TEMPO" : row == 3 ? "PLAYBACK" : "UNISON";
        }

        return {};
    }

    static int numRows (Kind kind, bool unisonRow)
    {
        return kind == Kind::live ? 1 : kind == Kind::sample ? (unisonRow ? 5 : 4) : 3;
    }

    // One card's measures, laid out by layoutPage and read by paint.
    struct CardGeometry
    {
        int osc = 0;
        Kind kind = Kind::wavetable;
        juce::Rectangle<int> bounds, header, well, wave, group, captionArea, statusArea;
        int rows = 3, columns = 9, rowHeight = 28, gridX = 0, gridWidth = 0, labelX = 0, labelWidth = rowLabelWidth;
        std::array<int, 6> rowY {};
        juce::Rectangle<int> hintArea;
        std::vector<int> separatorY;
        // The packed cards (review 23): the panels, their small names and rules.
        struct Panel { juce::Rectangle<int> area; juce::String title, note; juce::Colour colour; };
        struct Label { juce::Rectangle<int> area; juce::String text; int lines = 1; bool hint = false; float size = 0.0f; bool bright = false; };
        std::vector<Panel> panels;
        std::vector<Label> labels;
    };

    std::vector<CardGeometry> cards;
    juce::Rectangle<int> stripArea, addArea;
    int dragOsc = -1, dropAt = -1; // a card dragged by its header, and where it would land
    bool dragging = false;

    // (A switched-off oscillator keeps its whole card, dimmed, on both tabs; no card folds, the page scrolls
    // instead: ilana 2026-10-21 "dont fold ... just let me scroll", 2026-10-22 "yeah, dim".)

    // The kinds drawn as panels (WAVE, PITCH, MIX, UNISON...): the others keep the grid.
    static bool usesPanels (Kind kind) { return kind == Kind::wavetable || kind == Kind::physical || kind == Kind::live || kind == Kind::fm || kind == Kind::sample || kind == Kind::grain; }

    int warpRowH() const { return ampRowH; }
    int warpChipH() const { return chipH; }
    int warpDial() const { return ampDial; }

    // A wavetable card always shows WARP, SPECTRAL and WARP 2 (Off until picked), so adding a stage never
    // changes the card's height (ilana 2026-10-22: "adding warp 2 changes the size of the osc").
    static constexpr int wavetableWarpRows = 3;
    int waveContentHeight() const { return wavetableWarpRows * warpRowH() + (wavetableWarpRows - 1); }

    static constexpr int physTopH = 80, physBottomH = 61, sampleTopH = 96, sampleBottomH = 65, grainH = 111;

    int panelCardHeight (int osc) const
    {
        if (kindOf (osc) == Kind::physical)
            return 1 + cardHeaderHeight + physTopH + panelGap + physBottomH + cardPad + 1;

        // A sample card: the wavetable card's panel row, then a short row of RANGE, TEMPO and PLAYBACK,
        // the wave on the left over both.
        if (kindOf (osc) == Kind::sample)
            return 1 + cardHeaderHeight + sampleTopH + panelGap + sampleBottomH + cardPad + 1;

        // (A live or FM / DX7 card is the wavetable card's height with two warp rows.)
        // A granular card is one row of panels, as tall as its tallest panel (MIX) needs.
        if (kindOf (osc) == Kind::grain)
            return 1 + cardHeaderHeight + grainH + cardPad + 1;

        // (A live or FM / DX7 card keeps its two warp rows and the chip line under them.)
        const auto content = kindOf (osc) == Kind::wavetable ? waveContentHeight() : 2 * warpRowH() + 1 + 3 + warpChipH();
        return 1 + cardHeaderHeight + panelTop + panelTitleH + content + panelTail + 1 + cardPad + 1;
    }

    // What a card needs at least, by what it holds.
    int minimumCardHeight (int osc) const
    {
        if (usesPanels (kindOf (osc)))
            return panelCardHeight (osc);

        switch (kindOf (osc))
        {
            case Kind::sample:
            {
                const auto rows = numRows (Kind::sample, readFloat (juce::String (OscillatorIds::prefixes[(size_t) osc]) + "_unison") > 1.5f);
                return 1 + cardHeaderHeight + 8 + sampleWaveMin + 8 + sampleGroupHeight (rows) + 8 + 1;
            }
            // (A Physical card's fourth row, EXCITER, at the same row height and gap.)
            default: return compactCardMin + (rowsOf (osc) - 3) * (26 + rowGap) + twoRowRoom (osc);
        }
    }

    // What a compact card grows to with spare height.
    int maximumCardHeight (int osc) const
    {
        if (usesPanels (kindOf (osc)))
            return panelCardHeight (osc);

        // (A card beside folded ones keeps its condensed height: ilana
        // 2026-10-15, the one grown into their room was far too tall.)
        return compactCardMax + (rowsOf (osc) - 3) * (28 + rowGap) + twoRowRoom (osc);
    }

    static int sampleGroupHeight (int rows) { return 8 + rows * sampleRowHeight + (rows - 1); }

    std::vector<int> shownList() const
    {
        std::vector<int> shown;
        for (int osc = 0; osc < OscillatorIds::count; ++osc)
            if (processorRef.isOscillatorShown (osc))
                shown.push_back (osc);
        return shown;
    }

    std::vector<int> minimumHeights() const
    {
        std::vector<int> heights;
        for (const auto osc : shownList())
            heights.push_back (minimumCardHeight (osc));
        return heights;
    }

    // (PLAY too: past three cards the add row sits below them and the area scrolls to it, ilana 2026-10-23.)
    bool canAdd() const { return numShown() < OscillatorIds::count; }
    static constexpr int embeddedAddRowBelow = 3;

    int stripHeight() const { return stripCardH; }

    // Embedded on PLAY the cards stand closer (the mockup's 5 unit gaps), so three of them and the two module bands fit.
    int topNow() const { return embedded ? 2 : topMargin; }
    int gapNow() const { return embedded ? 0 : gap; }
    int bottomNow() const { return embedded ? 0 : bottomMargin; }

    int contentHeight (const std::vector<int>& heights) const
    {
        auto total = topNow() + (embedded ? 0 : stripHeight()) + bottomNow();
        for (const auto height : heights)
            total += height + gapNow();
        if (canAdd())
            total += addRowHeight + gapNow();
        return total;
    }

    void layoutPage()
    {
        cards.clear();
        const auto shown = shownList();
        auto heights = minimumHeights();
        const auto slack = getHeight() - contentHeight (heights);

        // Spare height goes to a sample's waveform, else to the wavetable
        // cards up to their roomy size; the strip stays at the foot.
        if (slack > 0 && ! heights.empty())
        {
            std::vector<size_t> samples, compacts;
            for (size_t i = 0; i < shown.size(); ++i)
            {
                const auto kind = kindOf (shown[i]);
                if (kind == Kind::sample)
                    continue;
                if (kind != Kind::live)
                    compacts.push_back (i);
            }

            if (! samples.empty())
                for (const auto i : samples)
                    heights[i] += slack / (int) samples.size();
            else if (! compacts.empty())
                for (const auto i : compacts)
                    heights[i] = juce::jmin (maximumCardHeight (shown[i]), heights[i] + slack / (int) compacts.size());
        }

        // The strip keeps one size however many cards there are (ilana 2026-10-21: it shrank with a second
        // oscillator); the page scrolls when cards and strip do not fit.

        const auto width = getWidth() - sideMargin * 2;
        auto y = topNow();

        for (size_t i = 0; i < shown.size(); ++i)
        {
            CardGeometry card;
            card.osc = shown[i];
            card.kind = kindOf (shown[i]);
            card.bounds = { sideMargin, y, width, heights[i] };
            layoutCard (card);
            cards.push_back (card);
            y += heights[i] + gapNow();
        }

        addArea = {};
        addButton.setVisible (canAdd());
        addFmButton.setVisible (canAdd());

        if (canAdd())
        {
            for (int i = 0; i < OscillatorIds::count; ++i)
                if (! processorRef.isOscillatorShown (i))
                {
                    addButton.setLabel ("+  ADD OSC " + juce::String (i + 1));
                    break;
                }

            // On PLAY with fewer than three cards, the room left under them is one drop tile (mockup v4) with the
            // two buttons side by side in its middle; otherwise the row adds a Wavetable, its right end FM / DX7.
            const auto tileHeight = getHeight() - y - 2;
            const auto tile = embedded && numShown() < embeddedAddRowBelow && tileHeight >= tileMinHeight;
            auto* host = tile ? static_cast<juce::Component*> (&dropTile) : static_cast<juce::Component*> (this);
            for (auto* button : { &addButton, &addFmButton })
            {
                if (button->getParentComponent() != host)
                    host->addAndMakeVisible (button);
                button->setSolid (tile);
            }
            dropTile.setVisible (tile);

            if (tile)
            {
                addArea = { sideMargin, y, width, tileHeight };
                dropTile.setBounds (addArea);
                const auto buttonsWidth = 2 * tileButtonWidth + 10;
                const auto buttonsTop = (tileHeight - tileButtonHeight - 22) / 2;
                auto row = juce::Rectangle<int> ((width - buttonsWidth) / 2, buttonsTop, buttonsWidth, tileButtonHeight);
                addButton.setBounds (row.removeFromLeft (tileButtonWidth));
                addFmButton.setBounds (row.removeFromRight (tileButtonWidth));
                dropTile.setCaptionTop (buttonsTop + tileButtonHeight + 6);
                y += tileHeight;
            }
            else
            {
                addArea = { sideMargin, y, width, addRowHeight };
                auto row = addArea;
                addFmButton.setBounds (row.removeFromRight (addFmWidth));
                row.removeFromRight (gap);
                addButton.setBounds (row);
                y += addRowHeight + gapNow();
            }
        }
        else
        {
            dropTile.setVisible (false);
        }

        // (Embedded on PLAY: the cards alone, no strip below them.)
        if (embedded)
        {
            stripArea = {};
            stripPanels.clear();
            return;
        }

        const auto stripY = juce::jmax (y, getHeight() - bottomMargin - stripHeight());
        stripArea = { sideMargin, stripY, width, stripHeight() };
        layoutStrip();
    }

    // A grid cell of a card, in the page.
    juce::Rectangle<int> cellRect (const CardGeometry& card, int row, int col, int span) const
    {
        const auto cellWidth = (float) (card.gridWidth - (card.columns - 1) * colGap) / (float) card.columns;
        const auto left = card.gridX + juce::roundToInt ((float) col * (cellWidth + (float) colGap));
        const auto right = card.gridX + juce::roundToInt ((float) (col + span) * (cellWidth + (float) colGap) - (float) colGap);
        return { left, card.rowY[(size_t) row], right - left, card.rowHeight };
    }

    void placeSlot (const CardGeometry& card, const Slot& slot) const
    {
        const auto cell = cellRect (card, slot.row, slot.col, slot.span);

        if (dynamic_cast<KnobControl*> (slot.item) != nullptr)
            slot.item->setBounds (cell.expanded (0, 3)); // (room for the modulation rings)
        else if (dynamic_cast<ComboControl*> (slot.item) != nullptr)
            slot.item->setBounds (cell.withSizeKeepingCentre (cell.getWidth(), 28));
        else
            slot.item->setBounds (cell);
    }

    // The four columns every panel row stands on, in page coordinates: WAVE takes
    // what PITCH, MIX and UNISON leave (the strip below uses the same ones).
    struct Columns { int x[4]; int w[4]; };

    Columns columnsFor (int left, int width, int pitch = 0, int unison = 0) const
    {
        if (pitch <= 0)
            pitch = pitchColumn();
        if (unison <= 0)
            unison = unisonColumn();
        Columns c;
        const auto total = width - 2 - 2 * cardPad;
        c.w[1] = pitch;
        c.w[2] = mixW;
        c.w[3] = unison;
        c.w[0] = total - pitch - mixW - unison - 3 * panelGap;
        auto x = left + 1 + cardPad;
        for (int i = 0; i < 4; ++i)
        {
            c.x[i] = x;
            x += c.w[i] + panelGap;
        }
        return c;
    }

    static juce::Rectangle<int> panelContent (juce::Rectangle<int> panel)
    {
        return { panel.getX() + 6, panel.getY() + panelTop + panelTitleH, panel.getWidth() - 12,
                 juce::jmax (0, panel.getHeight() - panelTop - panelTitleH - panelTail - 1) };
    }

    // A packed knob in a cell of `width` x (dial + name): its bounds with the room for rings.
    static void placeKnob (KnobControl& knob, int x, int y, int width, int dial)
    {
        knob.setBounds (KnobControl::packedBoundsFor ({ x, y, width, dial + nameLine }));
    }

    // The four unison dials after a menu of `menuWidth`: the first ring right after the menu, the last ring ending at the
    // panel's edge, one pitch between them (so no dead tail at the right of the panel).
    static void placeEvenDials (juce::Rectangle<int> c, int y, int menuWidth, std::initializer_list<KnobControl*> knobs, int dial)
    {
        const auto count = (int) knobs.size();
        const auto half = (float) dial * 0.5f;
        const auto first = (float) (c.getX() + menuWidth + 4) + half, last = (float) c.getRight() - half;
        const auto pitch = juce::jmax (33.0f, count > 1 ? (last - first) / (float) (count - 1) : 33.0f);
        auto index = 0;
        for (auto* knob : knobs)
            {
            // (The cell is as wide as the name needs, so the last one stays inside the panel.)
            const auto cellW = juce::jmin (pitch, 40.0f);
            placeKnob (*knob, juce::roundToInt (first + (float) index++ * pitch - cellW * 0.5f), y, juce::roundToInt (cellW), dial);
        }
    }

    static int packedButtonWidth (const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidthInt (juce::Font (IlanaTheme::font (IlanaTheme::TextSize::packedTextHeight, true).withKerningFactor (0.07f)), text) + 16;
    }

    // The filter route as the bar's five segments: AUTO F1 F2 BOTH NONE.
    static int routeToSegment (int route) { return route == FilterRoute::Filter1 ? 1 : route == FilterRoute::Filter2 ? 2 : route == FilterRoute::Both ? 3 : route == FilterRoute::Direct ? 4 : 0; }
    static int segmentToRoute (int segment) { return segment == 1 ? FilterRoute::Filter1 : segment == 2 ? FilterRoute::Filter2 : segment == 3 ? FilterRoute::Both : segment == 4 ? FilterRoute::Direct : FilterRoute::Default; }

    // The header (every kind): the name, the type bar, the table's menu and steps
    // (a wavetable), the FM chip, then at the right the actions and the switch.
    void layoutCardHeader (CardGeometry& card)
    {
        const auto index = card.osc;
        auto& osc = *controls[(size_t) index];
        auto& pieces = *aux[(size_t) index];
        const auto centre = card.header.getCentreY();
        osc.on.setBounds (juce::Rectangle<int> (30, 17).withCentre ({ card.header.getRight() - 6 - 15, centre }));
        auto right = card.header.withRight (osc.on.getX() - 6);
        if (pieces.removeButton.isVisible())
        {
            pieces.removeButton.setBounds (right.removeFromRight (17).withSizeKeepingCentre (17, 16));
            right.removeFromRight (4);
        }

        // (From the right: a sample card reads LOAD, RESAMPLE, EXPORT, SLICE, ZONES, DETECT.)
        for (auto* button : { &loadButton (index), &pieces.sampleDetectButton, &pieces.sampleZonesButton, &pieces.sampleSliceButton, &pieces.sampleExportButton, &pieces.opEnvButton, &pieces.stringButton,
                              editButtons[(size_t) index].get(), bounceButtons[(size_t) index].get(), &pieces.sampleLoadButton })
            if (button != nullptr && button->isVisible())
            {
                const auto width = packedButtonWidth (button->getButtonText());
                button->setBounds (right.removeFromRight (width).withSizeKeepingCentre (width, 16));
                right.removeFromRight (4);
            }

        // The name and the type bar.
        card.captionArea = {};
        auto x = card.bounds.getX() + 62;
        pieces.typeBar.setBounds ({ x, centre - 8, juce::jmin (pieces.typeBar.preferredWidth(), juce::jmax (0, right.getRight() - x - 4)), 17 });
        x = pieces.typeBar.getRight() + 7;

        if (card.kind == Kind::wavetable)
        {
            osc.table.setBounds (juce::Rectangle<int> (124, menuH).withPosition (x, centre - menuH / 2));
            x = osc.table.getRight() + 4;
            pieces.prevTable.setBounds ({ x, centre - 8, 24, 16 });
            pieces.nextTable.setBounds ({ x + 27, centre - 8, 24, 16 });
            x += 58;
            // FRAME BLEND: how the table reads between frames, beside the table it reads.
            // (4 px after the steps, as the steps sit 4 px after the table's menu.)
            x -= 3;
            osc.frameBlend.setBounds (juce::Rectangle<int> (juce::jlimit (0, 138, right.getRight() - x - 4), menuH).withPosition (x, centre - menuH / 2));
            x = osc.frameBlend.getRight() + 6;
        }

        if (card.kind == Kind::sample || card.kind == Kind::grain)
        {
            const auto width = juce::jlimit (0, 124, right.getRight() - x - 4);
            pieces.sampleNameButton.setBounds ({ x, centre - 8, width, 16 });
            x += width + 4;
        }

        const auto statusWidth = juce::jlimit (0, 190, right.getRight() - x - 2);
        card.statusArea = { x, centre - 9, statusWidth, 18 };
    }

    // A wavetable card as four panels (review 23): WAVE (the picture, FRAME, the warp
    // stages), PITCH, MIX and UNISON, on the page's four columns.
    void layoutWavetablePanels (CardGeometry& card)
    {
        const auto index = card.osc;
        auto& osc = *controls[(size_t) index];
        auto& pieces = *aux[(size_t) index];
        const auto tint = oscColour (index);
        const auto inner = card.bounds.reduced (1);
        const auto rowArea = inner.withTrimmedTop (cardHeaderHeight).reduced (cardPad, 0).withTrimmedBottom (cardPad);
        const auto cols = columnsFor (card.bounds.getX(), card.bounds.getWidth());
        const auto panelRect = [&] (int column) { return juce::Rectangle<int> (cols.x[column], rowArea.getY(), cols.w[column], rowArea.getHeight()); };
        const auto addPanel = [&] (int column, const juce::String& title) { card.panels.push_back ({ panelRect (column), title, {}, tint }); return panelContent (panelRect (column)); };
        const auto middle = [] (juce::Rectangle<int> area, int height) { return area.getY() + (area.getHeight() - height) / 2; };

        card.rows = 0;
        card.well = {};

        // WAVE
        {
            const auto c = addPanel (0, "WAVE");
            // (The warp menus and their rings keep their size at the panel's right edge; the picture takes everything left of them,
            // so no empty tail is left where FRAME's dial was. Dragging across the picture scrubs the frame.)
            const auto controlsW = 112 + 3 + ampCell;
            const auto picture = juce::Rectangle<int> (c.getX(), c.getY(), juce::jmax (juce::jmin (180, c.getWidth() / 2), c.getWidth() - 7 - controlsW), c.getHeight());
            waveDisplay (index).setBounds (picture);
            for (auto& item : scrubbers)
                item.setBounds ({});

            const auto columnX = picture.getRight() + 7;
            const auto columnW = juce::jmax (0, c.getRight() - columnX);
            const auto rows = wavetableWarpRows;
            const auto comboW = juce::jmax (40, juce::jmin (112, columnW - ampCell - 3));
            ComboControl* menus[] { &osc.warp, &osc.spectral, &osc.warp2 };
            KnobControl* amounts[] { &osc.warpAmt, &osc.spectralAmt, &osc.warp2Amt };
            auto y = middle (c, waveContentHeight());
            for (int r = 0; r < rows; ++r)
            {
                menus[r]->setBounds ({ columnX, y + (warpRowH() - menuH) / 2, comboW, menuH });
                // (The ring and its name sit 1 px low: their block, name included, centres on the menu beside them.)
                placeKnob (*amounts[r], columnX + comboW + 3, y + 1, ampCell, warpDial());
                y += warpRowH() + 1;
            }
        }

        // PITCH
        {
            const auto c = addPanel (1, "PITCH");
            const auto cellH = mainDial + nameLine;
            const auto top = middle (c, cellH + 3 + menuH);
            const auto tuning = OscRole::tuning (processorRef, index);
            KnobControl& first = tuning == OscTuning::Ratio ? osc.ratio : tuning == OscTuning::Fixed ? osc.fixedHz : osc.semi;
            placeKnob (first, c.getX(), top, knobCell, mainDial);
            placeKnob (osc.fine, c.getX() + knobCell, top, knobCell, mainDial);
            const auto tuneX = c.getX() + 2 * knobCell + 3;
            osc.tune.setBounds ({ tuneX, top + 4, c.getRight() - tuneX, menuH });
            const auto lower = top + cellH + 3;
            if (readChoice (juce::String (OscillatorIds::prefixes[(size_t) index]) + "_scale") != 0)
            {
                const auto rootW = 36, free = c.getWidth() - rootW - 8;
                osc.scale.setBounds ({ c.getX(), lower, free / 2, menuH });
                osc.scaleRoot.setBounds ({ c.getX() + free / 2 + 4, lower, rootW, menuH });
                osc.chord.setBounds ({ c.getX() + free / 2 + 4 + rootW + 4, lower, c.getRight() - (c.getX() + free / 2 + 4 + rootW + 4), menuH });
            }
            else
            {
                const auto half = (c.getWidth() - 4) / 2;
                osc.scale.setBounds ({ c.getX(), lower, half, menuH });
                osc.chord.setBounds ({ c.getX() + half + 4, lower, c.getWidth() - half - 4, menuH });
            }
        }

        // MIX
        {
            const auto c = addPanel (2, "MIX");
            const auto cellH = mainDial + nameLine;
            const auto top = middle (c, cellH + 3 + filterH + 3 + outH);
            placeKnob (osc.level, c.getX(), top, knobCell, mainDial);
            placeKnob (osc.pan, c.getX() + knobCell, top, knobCell, mainDial);
            const auto envX = c.getX() + 2 * knobCell + 3;
            osc.ampEnv.setBounds ({ envX, top + 4, c.getRight() - envX, menuH });
            const auto filterY = top + cellH + 3;
            const auto labelW = 28;
            // (The five segments take 5 px more than the content's width: AUTO, BOTH and NONE stay whole at the small zoom.)
            card.labels.push_back ({ { c.getX() - 2, filterY, labelW, filterH }, "FILTER" });
            pieces.routeBar.setBounds ({ c.getX() + labelW - 2, filterY, c.getWidth() - labelW + 5, filterH });
            pieces.outBar.setBounds ({ c.getX(), filterY + filterH + 3, c.getWidth(), outH });
        }

        // UNISON
        {
            const auto c = addPanel (3, "UNISON");
            // (The six knobs share the panel's whole width and the mode menu moves down beside the SPREAD picture:
            // with the menu on the knob row the cells were 34 px and SPREAD / FRAME ± stood 9 px apart at 175 %,
            // UI review 26.)
            const auto menuW = 54;
            const auto cell = juce::jmin (44, c.getWidth() / 6);
            const auto cellH = mainDial + nameLine;
            const auto top = middle (c, cellH + 4 + spreadH);
            auto x = c.getX() + (c.getWidth() - cell * 6) / 2;
            for (auto* knob : { &osc.unison, &osc.detune, &osc.uniBlend, &osc.spread, &osc.uniFrame, &osc.uniWarp })
            {
                placeKnob (*knob, x, top, cell, mainDial);
                x += cell;
            }
            const auto spreadY = top + cellH + 4;
            const auto labelW = 36;
            osc.uniMode.setBounds ({ c.getX(), spreadY + (spreadH - menuH) / 2, menuW, menuH });
            card.labels.push_back ({ { c.getX() + menuW + 6, spreadY, labelW, spreadH }, "SPREAD" });
            const auto barX = c.getX() + menuW + 6 + labelW;
            pieces.spreadView.setBounds ({ barX, spreadY, c.getRight() - barX, spreadH });
        }

        pieces.status.setBounds (card.statusArea);
    }

    // A physical card as panels (review 23): STRING full height at the left; EXCITER, TONE,
    // RESONANCE and MIX above; PITCH, UNISON and MORE below. Each control is the
    // existing one, placed by the part of the instrument it belongs to.
    // The v2 mockup's card measures (ilana's go, 2026-10-21): positions are the mockup's, in a panel's own
    // coordinates, the mockup's panels starting 28 under the card's top where the app's start 24 (its other
    // cards' header), so every y here is the mockup's less 4. The mockup's card is 1060 wide, the page's 1046:
    // the first column is the 16 px narrower one, PITCH / MIX / UNISON are the same.
    static constexpr int v2Top = 4;
    static juce::Rectangle<int> inPanel (juce::Rectangle<int> panel, int x, int y, int w, int h) { return { panel.getX() + x, panel.getY() + y, w, h }; }

    // A Physical card (v2 mockup): TONE, EXCITER, PITCH, MIX, UNISON on top; RESONANCE, MORE and the STRING picture under them.
    void layoutPhysicalPanels (CardGeometry& card)
    {
        const auto index = card.osc;
        auto& osc = *controls[(size_t) index];
        auto& phys = *physical[(size_t) index];
        auto& pieces = *aux[(size_t) index];
        const juce::String prefix (OscillatorIds::prefixes[(size_t) index]);
        const auto excite = juce::roundToInt (readFloat (prefix + "_excite"));
        const auto electric = isElectric (index);
        const auto tint = oscColour (index);
        const auto resonance = juce::Colour (0xffe9724c), grey = juce::Colour (0xff8b8f99);
        const auto cols = columnsFor (card.bounds.getX(), card.bounds.getWidth());
        const auto right = cols.x[3] + cols.w[3];
        const auto top = card.bounds.getY() + 28 - v2Top, lowY = top + physTopH + panelGap;

        card.rows = 0;
        card.well = {};

        const juce::Rectangle<int> tonePanel (cols.x[0], top, 156, physTopH);
        const juce::Rectangle<int> exciterPanel (cols.x[0] + 160, top, cols.w[0] - 160, physTopH);
        const auto pitchPanel = juce::Rectangle<int> (cols.x[1], top, cols.w[1], physTopH);
        const auto mixPanel = juce::Rectangle<int> (cols.x[2], top, cols.w[2], physTopH);
        const auto unisonPanel = juce::Rectangle<int> (cols.x[3], top, cols.w[3], physTopH);
        const juce::Rectangle<int> resPanel (cols.x[0], lowY, 156, physBottomH);
        // Which control goes where, from the exciter in use.
        const auto specs = physicalControlRows (excite);
        const auto& lookup = physicalLookup[(size_t) index];
        const auto find = [&] (const juce::String& suffix) -> juce::Component*
        {
            const auto found = lookup.find (suffix);
            return found != lookup.end() && processorRef.apvts.getParameter (prefix + suffix) != nullptr ? found->second : nullptr;
        };
        std::vector<KnobControl*> toneKnobs, exciterKnobs, moreKnobs;
        ToggleControl* slap = nullptr;
        const juce::StringArray toneSuffixes { "_string_decay", "_string_damp", "_string_stiffness" };
        for (const auto& spec : specs[0].second)
            if (auto* knob = dynamic_cast<KnobControl*> (find (spec.suffix)))
                (toneSuffixes.contains (spec.suffix) ? toneKnobs : moreKnobs).push_back (knob);
        for (const auto& spec : specs[1].second)
        {
            if (juce::String (spec.suffix) == "_string_slap")
                slap = dynamic_cast<ToggleControl*> (find (spec.suffix));
            else if (auto* knob = dynamic_cast<KnobControl*> (find (spec.suffix)))
                exciterKnobs.push_back (knob);
        }
        // (The mockup reads PICKUP right after the exciter's own knobs, before the amp's.)
        if (auto pickup = std::find (exciterKnobs.begin(), exciterKnobs.end(), &phys.pickup); pickup != exciterKnobs.end())
        {
            exciterKnobs.erase (pickup);
            const auto amp = std::find (exciterKnobs.begin(), exciterKnobs.end(), &phys.fbGain);
            exciterKnobs.insert (amp, &phys.pickup);
        }
        // (CLICK is the pickup's: it reads right after it.)
        if (auto click = std::find (exciterKnobs.begin(), exciterKnobs.end(), &phys.click); click != exciterKnobs.end())
        {
            exciterKnobs.erase (click);
            const auto pickup = std::find (exciterKnobs.begin(), exciterKnobs.end(), &phys.pickup);
            exciterKnobs.insert (pickup == exciterKnobs.end() ? pickup : pickup + 1, &phys.click);
        }

        // (A tine or reed, or Modal Piano, keeps only DAMPER under MORE, so MORE is one knob wide
        // and the picture takes the rest of the row: UI review 27 found DAMPER alone in a 270 px panel.)
        const auto narrowMore = electric || moreKnobs.size() <= 1;
        const auto moreX = cols.x[0] + 160;
        const juce::Rectangle<int> morePanel (moreX, lowY, narrowMore ? 62 : cols.x[1] - moreX, physBottomH);
        const auto stringX = narrowMore ? morePanel.getRight() + panelGap : cols.x[1] + panelGap;
        const juce::Rectangle<int> stringPanel (stringX, lowY, right - stringX, physBottomH);

        card.panels.push_back ({ tonePanel, "TONE", {}, tint });
        card.panels.push_back ({ exciterPanel, "EXCITER", {}, tint });
        card.panels.push_back ({ pitchPanel, "PITCH", {}, tint });
        card.panels.push_back ({ mixPanel, "MIX", {}, tint });
        card.panels.push_back ({ unisonPanel, "UNISON", {}, tint });
        card.panels.push_back ({ resPanel, "RESONANCE", {}, resonance });
        card.panels.push_back ({ morePanel, "MORE", {}, grey });
        card.panels.push_back ({ stringPanel, "STRING", {}, tint });

        // TONE: RING TIME, DARKEN, STIFF in 46 px cells; the partials' strip under them, named at its left.
        {
            auto x = tonePanel.getX() + 7;
            for (auto* knob : toneKnobs)
            {
                // (19, not 17: a modulated RING TIME's ring and live dot touched the TONE title, UI review 26.)
                placeKnob (*knob, x, tonePanel.getY() + 19, 46, mainDial);
                x += 46;
            }
            if (! electric)
            {
                pieces.partials.setCompact (true);
                pieces.partials.setBounds (inPanel (tonePanel, 55, 63, 93, 13));
                card.labels.push_back ({ inPanel (tonePanel, 7, 63, 46, 13), "PARTIALS" });
            }
            else
            {
                pieces.partials.setBounds ({});
            }
        }

        // EXCITER: the chosen exciter's name by the title, SLAP at the title's right, the icons, then the knobs centred.
        {
            CardGeometry::Label name { inPanel (exciterPanel, 65, 3, 140, 12), Exciters::name (excite).toUpperCase() };
            name.bright = true;
            card.labels.push_back (name);
            if (slap != nullptr)
                slap->setBounds ({ exciterPanel.getRight() - 64, exciterPanel.getY() + 3, 55, 12 });
            pieces.exciterPills.setBounds (inPanel (exciterPanel, 7, 15, exciterPanel.getWidth() - 14, 18));
            const auto n = (int) exciterKnobs.size();
            const auto cell = n > 0 ? juce::jmin (52, (exciterPanel.getWidth() - 14) / n) : 52;
            auto x = exciterPanel.getX() + (exciterPanel.getWidth() - n * cell) / 2;
            for (auto* knob : exciterKnobs)
            {
                placeKnob (*knob, x, exciterPanel.getY() + 37, cell, mainDial); // (37: clear of the icon row)
                x += cell;
            }
        }

        // PITCH: SEMI and FINE centred, CHORD under them.
        placeKnob (osc.semi, pitchPanel.getX() + 34, pitchPanel.getY() + 17, 46, mainDial);
        placeKnob (osc.fine, pitchPanel.getX() + 80, pitchPanel.getY() + 17, 46, mainDial);
        osc.chord.setBounds (inPanel (pitchPanel, 8, 62, pitchPanel.getWidth() - 16, menuH));

        // MIX: LEVEL and PAN, the FILTER route under them as on the wavetable card (UI review 27).
        placeKnob (osc.level, mixPanel.getX() + 34, mixPanel.getY() + 15, 48, mainDial);
        placeKnob (osc.pan, mixPanel.getX() + 82, mixPanel.getY() + 15, 48, mainDial);
        {
            const auto labelW = 28, filterY = mixPanel.getY() + physTopH - filterH - 6;
            card.labels.push_back ({ { mixPanel.getX() + 6, filterY, labelW, filterH }, "FILTER" });
            pieces.routeBar.setBounds ({ mixPanel.getX() + 6 + labelW, filterY, mixPanel.getWidth() - 12 - labelW + 3, filterH });
        }

        // UNISON: the mode menu, then VOICES, DETUNE, BLEND and SPREAD in 44 px cells.
        osc.uniMode.setBounds (inPanel (unisonPanel, 8, 25, 77, menuH));
        {
            auto x = unisonPanel.getX() + 89;
            for (auto* knob : { &osc.unison, &osc.detune, &osc.uniBlend, &osc.spread })
            {
                placeKnob (*knob, x, unisonPanel.getY() + 17, 44, mainDial);
                x += 44;
            }
            pieces.spreadView.setBounds ({});
        }

        // RESONANCE: AMOUNT, the SYMPATHETIC switch over the BOARD menu.
        placeKnob (phys.resAmount, resPanel.getX() + 5, resPanel.getY() + 16, 42, mainDial);
        phys.resOn.setBounds (inPanel (resPanel, 53, 17, 95, 12));
        phys.resBoard.setBounds (inPanel (resPanel, 53, 39, 95, menuH));

        // MORE: the string's other knobs from its left edge, in up to 46 px cells.
        {
            const auto n = (int) moreKnobs.size();
            const auto cell = n > 0 ? juce::jmin (46, (morePanel.getWidth() - 12) / n) : 46;
            auto x = morePanel.getX() + 8;
            for (auto* knob : moreKnobs)
            {
                placeKnob (*knob, x, morePanel.getY() + 16, cell, mainDial);
                x += cell;
            }
        }

        // STRING: the string (or, on a tine or reed, the table's picture) across the rest of the row.
        {
            const auto picture = inPanel (stringPanel, 8, 18, stringPanel.getWidth() - 16, 35);
            pieces.stringView.setBounds (picture);
            waveDisplay (index).setBounds (picture);
            pieces.opEnvGraph.setBounds ({});
        }

        pieces.status.setBounds (card.statusArea);
    }

    // An FM / DX7 card as four panels (review 23 language): WAVE (the cycle, FEEDBACK, the
    // WAVE and FB TYPE menus), PITCH, MIX and, on the Operator EG, ENVELOPE (its graph and
    // UNISON); on another envelope, UNISON (and the table's shapers while one is in use).
    void layoutFmPanels (CardGeometry& card)
    {
        const auto index = card.osc;
        auto& osc = *controls[(size_t) index];
        auto& pieces = *aux[(size_t) index];
        const juce::String prefix (OscillatorIds::prefixes[(size_t) index]);
        const auto tint = oscColour (index);
        const auto opEnv = OscRole::usesOperatorEg (processorRef, index);
        const auto inner = card.bounds.reduced (1);
        const auto rowArea = inner.withTrimmedTop (cardHeaderHeight).reduced (cardPad, 0).withTrimmedBottom (cardPad);
        const auto cols = columnsFor (card.bounds.getX(), card.bounds.getWidth());
        const auto panelRect = [&] (int column) { return juce::Rectangle<int> (cols.x[column], rowArea.getY(), cols.w[column], rowArea.getHeight()); };
        const auto addPanel = [&] (int column, const juce::String& title) { card.panels.push_back ({ panelRect (column), title, {}, tint }); return panelContent (panelRect (column)); };
        const auto middle = [] (juce::Rectangle<int> area, int height) { return area.getY() + (area.getHeight() - height) / 2; };
        const auto cellH = mainDial + nameLine;

        card.rows = 0;
        card.well = {};

        // WAVE: the picture, FEEDBACK, then the two menus one over the other.
        {
            const auto c = addPanel (0, "WAVE");
            const auto menuW = 108, feedbackCell = 38;
            const auto menuX = c.getRight() - menuW;
            const auto feedbackX = menuX - 5 - feedbackCell;
            waveDisplay (index).setBounds ({ c.getX(), c.getY(), juce::jmax (40, feedbackX - 5 - c.getX()), c.getHeight() });
            for (auto& item : scrubbers)
                item.setBounds ({});
            placeKnob (osc.feedback, feedbackX, middle (c, cellH), feedbackCell, mainDial);
            const auto stackH = 2 * menuH + 8;
            const auto top = middle (c, stackH);
            osc.table.setBounds ({ menuX, top, menuW, menuH });
            osc.feedbackType.setBounds ({ menuX, top + menuH + 8, menuW, menuH });
        }

        // PITCH: SEMI, FINE and RATIO or FIXED HZ, the TUNING menu; SCALE and CHORD under them.
        {
            const auto c = addPanel (1, "PITCH");
            const auto top = middle (c, cellH + 3 + menuH);
            const auto pitchCell = 33; // (the TUNING menu keeps room for "Ratio")
            const auto tuning = OscRole::tuning (processorRef, index);
            auto x = c.getX();
            placeKnob (osc.semi, x, top, pitchCell, mainDial);
            x += pitchCell;
            placeKnob (osc.fine, x, top, pitchCell, mainDial);
            x += pitchCell;
            if (tuning != OscTuning::Semitones)
            {
                placeKnob (tuning == OscTuning::Ratio ? osc.ratio : osc.fixedHz, x, top, pitchCell, mainDial);
                x += pitchCell;
            }
            osc.tune.setBounds ({ x + 3, top + 4, c.getRight() - x - 3, menuH });
            const auto lower = top + cellH + 3;
            if (readChoice (prefix + "_scale") != 0)
            {
                const auto rootW = 36, free = c.getWidth() - rootW - 8;
                osc.scale.setBounds ({ c.getX(), lower, free / 2, menuH });
                osc.scaleRoot.setBounds ({ c.getX() + free / 2 + 4, lower, rootW, menuH });
                osc.chord.setBounds ({ c.getX() + free / 2 + 4 + rootW + 4, lower, c.getRight() - (c.getX() + free / 2 + 4 + rootW + 4), menuH });
            }
            else
            {
                const auto half = (c.getWidth() - 4) / 2;
                osc.scale.setBounds ({ c.getX(), lower, half, menuH });
                osc.chord.setBounds ({ c.getX() + half + 4, lower, c.getWidth() - half - 4, menuH });
            }
        }

        // MIX: OUTPUT (or LEVEL), PAN, ENV, FILTER and OUT.
        {
            const auto c = addPanel (2, "MIX");
            auto& level = opEnv ? osc.egOut : osc.level;
            const auto top = middle (c, cellH + 3 + filterH + 3 + outH);
            placeKnob (level, c.getX(), top, knobCell, mainDial);
            placeKnob (osc.pan, c.getX() + knobCell, top, knobCell, mainDial);
            const auto envX = c.getX() + 2 * knobCell + 3;
            osc.ampEnv.setBounds ({ envX, top + 4, c.getRight() - envX, menuH });
            const auto filterY = top + cellH + 3;
            const auto labelW = 28;
            card.labels.push_back ({ { c.getX() - 2, filterY, labelW, filterH }, "FILTER" });
            pieces.routeBar.setBounds ({ c.getX() + labelW - 2, filterY, c.getWidth() - labelW + 5, filterH });
            pieces.outBar.setBounds ({ c.getX(), filterY + filterH + 3, c.getWidth(), outH });
        }

        // ENVELOPE (the Operator EG's graph, what it does and UNISON) or UNISON (and shapers in use).
        {
            const auto c = addPanel (3, opEnv ? "ENVELOPE" : "UNISON");
            pieces.opEnvGraph.setBounds ({});
            pieces.spreadView.setBounds ({});
            if (opEnv)
            {
                const auto raised = readFloat (prefix + "_unison") > 1.5f;
                const auto knobX = c.getRight() - (raised ? knobCell + 7 : 0);
                const auto room = knobX - c.getX() - (raised ? 7 : 0);
                const auto graphW = juce::jmax (40, room * 50 / 100), graphH = c.getHeight();
                const auto hintX = c.getX() + graphW + 8, hintW = knobX - hintX - (raised ? 7 : 0);
                pieces.opEnvGraph.setBounds ({ c.getX(), middle (c, graphH), graphW, graphH });
                card.labels.push_back ({ { hintX, c.getY() + 2, hintW, c.getHeight() - 4 }, "The operator's own envelope: its level over the note.", 4, true, 0.0f });
                if (raised)
                    placeKnob (osc.unison, c.getRight() - knobCell, middle (c, cellH), knobCell, mainDial);
            }
            else
            {
                // (A voice that is not a DX7 operator has a full unison, as the wavetable card's: the mode, VOICES, DETUNE, BLEND and
                // SPREAD in a row, and under it the SPREAD picture, or the table's shapers while any is in use.)
                const auto knobY = middle (c, cellH + 4 + spreadH);
                osc.uniMode.setBounds ({ c.getX(), knobY + 4, 55, menuH });
                placeEvenDials (c, knobY, 55, { &osc.unison, &osc.detune, &osc.uniBlend, &osc.spread }, mainDial);
                const auto lowerY = knobY + cellH + 4;
                const std::pair<ComboControl*, KnobControl*> shapers[] { { &osc.warp, &osc.warpAmt }, { &osc.warp2, &osc.warp2Amt },
                                                                        { &osc.spectral, &osc.spectralAmt } };
                const juce::StringArray stages { "_warp", "_warp2", "_spectral" };
                auto inUse = 0;
                for (int i = 0; i < 3; ++i)
                    inUse += readChoice (prefix + stages[i]) != 0 ? 1 : 0;
                if (inUse == 0)
                {
                    card.labels.push_back ({ { c.getX(), lowerY, 36, spreadH }, "SPREAD" });
                    pieces.spreadView.setBounds ({ c.getX() + 36, lowerY, c.getWidth() - 36, spreadH });
                }
                else
                {
                    const auto columnW = (c.getWidth() - 4 * (inUse - 1)) / inUse;
                    auto used = 0;
                    for (int i = 0; i < 3; ++i)
                        if (readChoice (prefix + stages[i]) != 0)
                        {
                            const auto x = c.getX() + used * (columnW + 4);
                            const auto menuW = juce::jmax (30, columnW - ampCell - 3);
                            shapers[i].first->setBounds ({ x, lowerY + (spreadH - menuH) / 2, menuW, menuH });
                            placeKnob (*shapers[i].second, x + menuW + 3, lowerY - 4, ampCell, ampDial);
                            ++used;
                        }
                }
            }
        }

        pieces.status.setBounds (card.statusArea);
    }

    // A sample card (v2 mockup): WAVE at the left over both rows (the name line, the wave and the keys, the
    // switches in one column beside them); PITCH, MIX and UNISON on the first row; RANGE, TEMPO and PLAYBACK under them.
    void layoutSamplePanels (CardGeometry& card)
    {
        const auto index = card.osc;
        auto& osc = *controls[(size_t) index];
        auto& pieces = *aux[(size_t) index];
        const juce::String prefix (OscillatorIds::prefixes[(size_t) index]);
        const auto tint = oscColour (index);
        const auto cols = columnsFor (card.bounds.getX(), card.bounds.getWidth());
        const auto topY = card.bounds.getY() + 28 - v2Top, lowY = topY + sampleTopH + panelGap;
        const auto addPanel = [&] (juce::Rectangle<int> area, const juce::String& title) { card.panels.push_back ({ area, title, {}, tint }); return area; };

        card.rows = 0;
        card.well = {};

        // WAVE: the picture (its name line, the wave, the keys) and the switches' column at the right.
        {
            const auto panel = addPanel ({ cols.x[0], topY, cols.w[0], sampleTopH + panelGap + sampleBottomH }, "WAVE");
            const auto toggleW = 136, toggleX = panel.getRight() - 10 - toggleW;
            waveDisplay (index).setBounds ({ panel.getX() + 8, panel.getY() + 15, toggleX - 11 - (panel.getX() + 8), 143 });
            for (auto& item : scrubbers)
                item.setBounds ({});
            auto y = panel.getY() + 38;
            for (auto* toggle : { &osc.sampleTuned, &osc.sampleLoop, &osc.sampleReverse, &osc.sampleLoopOwn, &osc.sampleSnap })
            {
                toggle->setBounds ({ toggleX, y, toggleW, 13 });
                y += 21;
            }
            osc.sampleLoopXfade.setBounds ({});
            pieces.xfadeBar.setBounds ({ toggleX, panel.getY() + 144, toggleW, 12 });
            pieces.xfadeBar.setAccent (tint);
        }

        // PITCH: SEMI, FINE and ROOT; SCALE and CHORD under them.
        {
            const auto panel = addPanel ({ cols.x[1], topY, cols.w[1], sampleTopH }, "PITCH");
            auto x = panel.getX() + 7;
            for (auto* knob : { &osc.semi, &osc.fine, &osc.sampleRoot })
            {
                placeKnob (*knob, x, panel.getY() + 21, 47, mainDial);
                x += 49;
            }
            const auto y = panel.getY() + 74;
            if (readChoice (prefix + "_scale") != 0)
            {
                osc.scale.setBounds (inPanel (panel, 8, 74 - 0, 52, menuH));
                osc.scaleRoot.setBounds (inPanel (panel, 63, 74, 36, menuH));
                osc.chord.setBounds ({ panel.getX() + 102, y, panel.getRight() - 8 - (panel.getX() + 102), menuH });
            }
            else
            {
                osc.scale.setBounds (inPanel (panel, 8, 74, 68, menuH));
                osc.chord.setBounds (inPanel (panel, 80, 74, 72, menuH));
            }
        }

        // MIX: LEVEL, PAN and ENV; the FILTER route and OUT under them.
        {
            const auto panel = addPanel ({ cols.x[2], topY, cols.w[2], sampleTopH }, "MIX");
            placeKnob (osc.level, panel.getX() + 5, panel.getY() + 19, 38, mainDial);
            placeKnob (osc.pan, panel.getX() + 43, panel.getY() + 19, 34, mainDial);
            osc.ampEnv.setBounds (inPanel (panel, 81, 25, 75, menuH));
            pieces.routeBar.setBounds (inPanel (panel, 8, 66, 148, 16));
            pieces.outBar.setBounds (inPanel (panel, 8, 84, 148, 12));
        }

        // UNISON: the mode menu, VOICES, DETUNE, BLEND and SPREAD; the spread bar under them.
        {
            const auto panel = addPanel ({ cols.x[3], topY, cols.w[3], sampleTopH }, "UNISON");
            osc.uniMode.setBounds (inPanel (panel, 8, 33, 77, menuH));
            auto x = panel.getX() + 89;
            for (auto* knob : { &osc.unison, &osc.detune, &osc.uniBlend, &osc.spread })
            {
                placeKnob (*knob, x, panel.getY() + 21, 44, mainDial);
                x += 44;
            }
            card.labels.push_back ({ inPanel (panel, 8, 81, 36, 12), "SPREAD" });
            pieces.spreadView.setBounds (inPanel (panel, 44, 81, panel.getWidth() - 52, 12));
        }

        // RANGE: START, END, FADE IN, FADE OUT.
        {
            const auto panel = addPanel ({ cols.x[1], lowY, cols.w[1], sampleBottomH }, "RANGE");
            auto x = panel.getX() + 4;
            for (auto* knob : { &osc.sampleStart, &osc.sampleEnd, &osc.sampleFadeIn, &osc.sampleFadeOut })
            {
                placeKnob (*knob, x, panel.getY() + 17, 38, mainDial);
                x += 38;
            }
        }

        // TEMPO: SYNC over TIME, and BPM.
        {
            const auto panel = addPanel ({ cols.x[2], lowY, cols.w[2], sampleBottomH }, "TEMPO");
            osc.sampleSync.setBounds (inPanel (panel, 8, 18, 100, menuH));
            osc.sampleStretch.setBounds (inPanel (panel, 8, 41, 100, menuH));
            placeKnob (osc.sampleBpm, panel.getX() + 114, panel.getY() + 17, 43, mainDial);
        }

        // PLAYBACK: MODE over QUALITY, PING-PONG over TRANSIENT SAFE, and RANDOM.
        {
            const auto panel = addPanel ({ cols.x[3], lowY, cols.w[3], sampleBottomH }, "PLAYBACK");
            osc.samplePlayback.setBounds (inPanel (panel, 8, 18, 111, menuH));
            osc.sampleQuality.setBounds (inPanel (panel, 8, 41, 111, menuH));
            osc.samplePingPong.setBounds (inPanel (panel, 127, 18, 91, 13));
            osc.sampleStartSafe.setBounds (inPanel (panel, 127, 42, 91, 13));
            placeKnob (osc.sampleStartRandom, panel.getX() + 220, panel.getY() + 17, 45, mainDial);
        }

        pieces.status.setBounds (card.statusArea);
    }

    // A granular card (v2 mockup): CLOUD (the picture with FREEZE on it, the six grain knobs in two rows), PITCH,
    // MIX and UNISON on the page's four columns.
    void layoutGranularPanels (CardGeometry& card)
    {
        const auto index = card.osc;
        auto& osc = *controls[(size_t) index];
        auto& pieces = *aux[(size_t) index];
        const juce::String prefix (OscillatorIds::prefixes[(size_t) index]);
        const auto tint = oscColour (index);
        const auto cols = columnsFor (card.bounds.getX(), card.bounds.getWidth());
        const auto topY = card.bounds.getY() + 28 - v2Top;
        const auto addPanel = [&] (int column, const juce::String& title)
        {
            const juce::Rectangle<int> area (cols.x[column], topY, cols.w[column], grainH);
            card.panels.push_back ({ area, title, {}, tint });
            return area;
        };

        card.rows = 0;
        card.well = {};

        // CLOUD: the picture takes what the knobs' 3 x 2 grid leaves; FREEZE (and LIVE) sit on its foot.
        {
            const auto panel = addPanel (0, "CLOUD");
            const auto knobsX = panel.getRight() - 13 - 3 * 50;
            const juce::Rectangle<int> picture (panel.getX() + 8, panel.getY() + 18, knobsX - 8 - (panel.getX() + 8), 85);
            waveDisplay (index).setBounds (picture);
            for (auto& item : scrubbers)
                item.setBounds ({});
            KnobControl* grains[] { &osc.grainPosition, &osc.grainScan, &osc.grainSize, &osc.grainDensity, &osc.grainSpray, &osc.grainPitch };
            for (int i = 0; i < 6; ++i)
                placeKnob (*grains[i], knobsX + (i % 3) * 50, panel.getY() + (i < 3 ? 17 : 65), 50, mainDial);
            osc.grainFreeze.setBounds (inPanel (panel, 14, 83, 40, 13));
            osc.grainFreeze.toFront (false);
            if (IlanaSynthAudioProcessor::isEffectBuild)
            {
                osc.grainLive.setBounds ({ picture.getX() + 4, picture.getY() + 4, juce::jmin (40, picture.getWidth() - 8), 12 });
                osc.grainLive.toFront (false);
            }
        }

        // PITCH: SEMI and FINE; TUNED and REVERSE; SCALE and CHORD.
        {
            const auto panel = addPanel (1, "PITCH");
            placeKnob (osc.semi, panel.getX() + 34, panel.getY() + 17, 46, mainDial);
            placeKnob (osc.fine, panel.getX() + 80, panel.getY() + 17, 46, mainDial);
            osc.sampleTuned.setBounds (inPanel (panel, 8, 65, 68, 13));
            osc.sampleReverse.setBounds (inPanel (panel, 80, 65, 72, 13));
            if (readChoice (prefix + "_scale") != 0)
            {
                osc.scale.setBounds (inPanel (panel, 8, 87, 52, menuH));
                osc.scaleRoot.setBounds (inPanel (panel, 63, 87, 36, menuH));
                osc.chord.setBounds ({ panel.getX() + 102, panel.getY() + 87, panel.getRight() - 8 - (panel.getX() + 102), menuH });
            }
            else
            {
                osc.scale.setBounds (inPanel (panel, 8, 87, 68, menuH));
                osc.chord.setBounds (inPanel (panel, 80, 87, 72, menuH));
            }
        }

        // MIX: LEVEL, PAN and ENV; the FILTER route and OUT under them.
        {
            const auto panel = addPanel (2, "MIX");
            placeKnob (osc.level, panel.getX() + 5, panel.getY() + 17, 38, mainDial);
            placeKnob (osc.pan, panel.getX() + 43, panel.getY() + 17, 34, mainDial);
            osc.ampEnv.setBounds (inPanel (panel, 81, 25, 75, menuH));
            pieces.routeBar.setBounds (inPanel (panel, 8, 66, 148, 16));
            pieces.outBar.setBounds (inPanel (panel, 8, 91, 148, 12));
        }

        // UNISON: the mode menu over VOICES, DETUNE, BLEND, SPREAD and the grains' STEREO; the spread bar under them.
        {
            const auto panel = addPanel (3, "UNISON");
            osc.uniMode.setBounds (inPanel (panel, 8, 17, 102, menuH));
            auto x = panel.getX() + 25;
            for (auto* knob : { &osc.unison, &osc.detune, &osc.uniBlend, &osc.spread, &osc.grainSpread })
            {
                placeKnob (*knob, x, panel.getY() + 41, 44, mainDial);
                x += 44;
            }
            card.labels.push_back ({ inPanel (panel, 8, 95, 36, 12), "SPREAD" });
            pieces.spreadView.setBounds (inPanel (panel, 44, 95, panel.getWidth() - 52, 12));
        }

        pieces.status.setBounds (card.statusArea);
    }

    // A live card as two panels (review 23 language): INPUT, the meter, over the
    // WAVE and PITCH columns; MIX over MIX and UNISON, LEVEL, PAN, ENV and FILTER on one
    // line and OUT under them.
    void layoutLivePanels (CardGeometry& card)
    {
        const auto index = card.osc;
        auto& osc = *controls[(size_t) index];
        auto& pieces = *aux[(size_t) index];
        const auto tint = oscColour (index);
        const auto inner = card.bounds.reduced (1);
        const auto rowArea = inner.withTrimmedTop (cardHeaderHeight).reduced (cardPad, 0).withTrimmedBottom (cardPad);
        const auto cols = columnsFor (card.bounds.getX(), card.bounds.getWidth());
        const auto middle = [] (juce::Rectangle<int> area, int height) { return area.getY() + (area.getHeight() - height) / 2; };
        const juce::Rectangle<int> inputPanel (cols.x[0], rowArea.getY(), cols.x[1] + cols.w[1] - cols.x[0], rowArea.getHeight());
        const juce::Rectangle<int> mixPanel (cols.x[2], rowArea.getY(), cols.x[3] + cols.w[3] - cols.x[2], rowArea.getHeight());

        card.rows = 0;
        card.well = {};
        card.panels.push_back ({ inputPanel, "INPUT", {}, tint });
        card.panels.push_back ({ mixPanel, "MIX", {}, tint });

        waveDisplay (index).setBounds (panelContent (inputPanel));
        for (auto& item : scrubbers)
            item.setBounds ({});

        const auto c = panelContent (mixPanel);
        const auto cellH = mainDial + nameLine;
        const auto top = middle (c, cellH + 3 + outH);
        placeKnob (osc.level, c.getX(), top, knobCell, mainDial);
        placeKnob (osc.pan, c.getX() + knobCell + 8, top, knobCell, mainDial);
        const auto envX = c.getX() + 2 * knobCell + 8 + 8;
        const auto envW = 100;
        osc.ampEnv.setBounds ({ envX, top + 4, envW, menuH });
        const auto labelW = 28;
        const auto filterX = envX + envW + 10;
        card.labels.push_back ({ { filterX, top + 4, labelW, filterH }, "FILTER" });
        pieces.routeBar.setBounds ({ filterX + labelW - 2, top + 4, c.getRight() - (filterX + labelW - 2) + 5, filterH });
        pieces.outBar.setBounds ({ c.getX(), top + cellH + 3, c.getWidth(), outH });
        pieces.status.setBounds (card.statusArea);
    }

    void layoutCard (CardGeometry& card)
    {
        const auto index = card.osc;
        auto& osc = *controls[(size_t) index];
        auto& pieces = *aux[(size_t) index];
        const auto inner = card.bounds.reduced (1);
        card.header = inner.withHeight (cardHeaderHeight);
        const auto body = inner.withTrimmedTop (cardHeaderHeight);

        layoutCardHeader (card);

        if (usesPanels (card.kind))
        {
            if (card.kind == Kind::physical)
                layoutPhysicalPanels (card);
            else if (card.kind == Kind::live)
                layoutLivePanels (card);
            else if (card.kind == Kind::fm)
                layoutFmPanels (card);
            else if (card.kind == Kind::sample)
                layoutSamplePanels (card);
            else if (card.kind == Kind::grain)
                layoutGranularPanels (card);
            else
                layoutWavetablePanels (card);
            return;
        }

        card.rows = rowsOf (index);
        card.columns = 9;
        const auto roomy = card.bounds.getHeight() > compactCardMax + (card.rows - 3) * (28 + rowGap) + twoRowRoom (index);
        card.rowHeight = juce::jlimit (26, 28, (body.getHeight() - (card.rows - 1) * rowGap - 8) / card.rows);
        const auto rowsHeight = card.rowHeight * card.rows + rowGap * (card.rows - 1);
        const auto padTop = juce::jmax (4, (body.getHeight() - rowsHeight) / 2);
        card.well = { body.getX() + 10, body.getY() + juce::jmin (5, padTop), wellWidth, body.getHeight() - 2 * juce::jmin (5, padTop) };
        card.labelX = card.well.getRight() + 10;
        card.gridX = card.labelX + rowLabelWidth + 8;
        card.gridWidth = inner.getRight() - 10 - card.gridX;

        const auto spacing = roomy ? juce::jmax (rowGap, (body.getHeight() - card.rowHeight * card.rows) / (card.rows + 1)) : rowGap;
        for (int r = 0; r < card.rows; ++r)
            card.rowY[(size_t) r] = roomy ? body.getY() + spacing + r * (card.rowHeight + spacing)
                                          : body.getY() + padTop + r * (card.rowHeight + rowGap);

        for (const auto& slot : slotsFor (index))
            placeSlot (card, slot);

        // The well: the wavetable (with its slim line), the operator's envelope
        // or the string.
        const auto electric = card.kind == Kind::physical && isElectric (index);
        waveDisplay (index).setBounds (card.well);
        pieces.opEnvGraph.setBounds (card.well);
        pieces.stringView.setBounds (card.well);
        for (auto& item : scrubbers)
            item.setBounds ({});
        juce::ignoreUnused (electric);

        // The right-hand cells: the FM chip or the output bar in PITCH's, the
        // unison picture in UNISON's.
        // The string's partials take the right-hand cells the string, the
        // exciter and PITCH leave free (two or three columns, down to PITCH);
        // free cells left in the EXCITER row say what the exciter does.
        card.hintArea = {};
        if (card.kind == Kind::physical)
        {
            const auto slots = slotsFor (index);
            const auto pitchRow = card.rows - 2;
            const auto used = [&slots] (int row, int col)
            {
                for (const auto& slot : slots)
                    if (slot.row == row && col >= slot.col && col < slot.col + slot.span)
                        return true;
                return false;
            };
            const auto freeRun = [&] (int fromRow, int fromCol)
            {
                for (int r = fromRow; r <= pitchRow; ++r)
                    for (int c = fromCol; c < 9; ++c)
                        if (used (r, c))
                            return false;
                return true;
            };
            auto top = pitchRow;
            while (top > 0 && freeRun (top - 1, 7))
                --top;
            const auto from = freeRun (top, 6) ? 6 : 7;
            pieces.partials.setBounds (cellRect (card, top, from, 9 - from).getUnion (cellRect (card, pitchRow, from, 9 - from)).reduced (4, 0));

            if (! physicalRowsMerged (index))
            {
                auto last = 0;
                for (const auto& slot : slots)
                    if (slot.row == 1)
                        last = juce::jmax (last, slot.col + slot.span);
                const auto to = top <= 1 ? from : 9;
                if (to - last >= 2)
                    card.hintArea = cellRect (card, 1, last, to - last).reduced (6, 0);
            }
        }

        pieces.status.setBounds (card.statusArea);
        // (A wavetable's UNISON row keeps FRM SPR and WRP SPR in columns 6 and 7: the picture takes the last cell, A16-1.)
        if (card.kind != Kind::live)
            pieces.spreadView.setBounds (card.kind == Kind::wavetable ? cellRect (card, 2, 8, 1) : cellRect (card, card.rows - 1, 6, 3));
    }

    void paintOscCard (juce::Graphics& g, const CardGeometry& card) const
    {
        const auto index = card.osc;
        const auto tint = oscColour (index);
        const auto off = isOff (index);

        if (off)
            g.beginTransparencyLayer (0.55f);

        IlanaTheme::paintCard (g, card.bounds.toFloat(), 10.0f, tint);
        g.setColour (IlanaTheme::Ui::line.withAlpha (0.6f));
        g.fillRect (juce::Rectangle<int> (card.bounds.getX() + 1, card.header.getBottom() - 1, card.bounds.getWidth() - 2, 1));

        const auto centre = card.header.getCentreY();
        IlanaTheme::paintTag (g, { (float) card.bounds.getX() + 14.0f, (float) centre }, tint);
        g.setColour (IlanaTheme::Ui::text);
        g.setFont (IlanaTheme::font (IlanaTheme::TextSize::packedTitle, true));
        IlanaTheme::drawFitted (g, "OSC " + juce::String (index + 1), juce::Rectangle<int> (card.bounds.getX() + 23, centre - 9, 38, 18), juce::Justification::centredLeft, 1);

        if (usesPanels (card.kind))
        {
            paintPanels (g, card);
            if (off)
                g.endTransparencyLayer();
            return;
        }

        // Each row named at its left, in the oscillator's colour.
        if (card.kind == Kind::sample)
            IlanaTheme::paintRecessedPanel (g, card.group.toFloat(), 8.0f);

        for (int r = 0; r < card.rows; ++r)
        {
            g.setColour (tint.withAlpha (0.8f));
            g.setFont (IlanaTheme::font (IlanaTheme::TextSize::label, true));
            // (An operator's third row is there only for UNISON or a shaper in use.)
            auto name = rowNameOf (index, r);
            if (card.kind == Kind::fm && r == 2)
            {
                auto shaper = false;
                for (const auto& slot : slotsFor (index))
                    shaper = shaper || (slot.row == 2 && slot.item != &controls[(size_t) index]->unison);
                if (! shaper && ! unisonShown (index))
                    continue;
                if (shaper)
                    name = unisonShown (index) ? "MORE" : "SHAPE";
            }
            IlanaTheme::drawFitted (g, name,
                                    juce::Rectangle<int> (card.labelX, card.rowY[(size_t) r], card.labelWidth, card.rowHeight), juce::Justification::centredLeft, 1);
        }

        if (! card.hintArea.isEmpty())
        {
            g.setColour (IlanaTheme::Ui::text3);
            g.setFont (IlanaTheme::font (IlanaTheme::TextSize::label));
            IlanaTheme::drawFitted (g, physicalHint (index), card.hintArea, juce::Justification::centredLeft, 2);
        }

        for (const auto y : card.separatorY)
        {
            g.setColour (IlanaTheme::Ui::line.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<int> (card.group.getX() + 8, y, card.group.getWidth() - 16, 1));
        }

        if (off)
            g.endTransparencyLayer();
    }

    // The panels' fill, a section colour's 2 px top rule, the small name and its note.
    void paintPanels (juce::Graphics& g, const CardGeometry& card) const
    {
        paintPanelList (g, card.panels);

        g.setColour (IlanaTheme::Ui::text3);
        for (const auto& label : card.labels)
        {
            g.setColour (label.bright ? IlanaTheme::Ui::text : IlanaTheme::Ui::text3);
            g.setFont (label.size > 0.0f ? IlanaTheme::font (label.size) : label.hint ? IlanaTheme::font (7.6f) : IlanaTheme::font (IlanaTheme::TextSize::packedLabel, true).withKerningFactor (0.09f));
            IlanaTheme::drawFitted (g, label.text, label.area, label.hint && label.size <= 0.0f ? juce::Justification::topLeft : juce::Justification::centredLeft, label.lines);
        }
    }

    void paintPanelList (juce::Graphics& g, const std::vector<CardGeometry::Panel>& panels) const
    {
        for (const auto& panel : panels)
        {
            const auto area = panel.area.toFloat();
            g.setColour (IlanaTheme::Ui::raised.interpolatedWith (IlanaTheme::Ui::panel, 0.55f));
            g.fillRoundedRectangle (area, 7.0f);
            {
                juce::Path shape;
                shape.addRoundedRectangle (area, 7.0f);
                const juce::Graphics::ScopedSaveState state (g);
                g.reduceClipRegion (panel.area.withHeight (2));
                g.setColour (panel.colour);
                g.fillPath (shape);
            }
            g.setColour (IlanaTheme::Ui::line);
            g.drawRoundedRectangle (area.reduced (0.5f), 6.5f, 1.0f);
            g.setColour (panel.colour);
            g.fillRect (panel.area.withHeight (2).reduced (5, 0));

            const auto titleFont = IlanaTheme::font (7.6f, true).withKerningFactor (0.13f);
            const auto titleArea = juce::Rectangle<int> (panel.area.getX() + 6, panel.area.getY() + panelTop, panel.area.getWidth() - 12, panelTitleH - 1);
            g.setColour (IlanaTheme::Ui::text2);
            g.setFont (titleFont);
            const auto titleWidth = juce::GlyphArrangement::getStringWidthInt (juce::Font (titleFont), panel.title) + 6;
            IlanaTheme::drawFitted (g, panel.title, titleArea, juce::Justification::centredLeft, 1);
            if (panel.note.isNotEmpty())
            {
                g.setColour (IlanaTheme::Ui::text3);
                g.setFont (IlanaTheme::font (IlanaTheme::TextSize::packedLabel));
                IlanaTheme::drawFitted (g, panel.note, titleArea.withTrimmedLeft (titleWidth), juce::Justification::centredLeft, 1);
            }
        }
    }

    // The engine's name in the header: an FM / DX7 oscillator says it is an
    // operator, and on what envelope.
    juce::String engineName (int index) const
    {
        if (getMode (index) == OscMode::fmOperator)
            return OscRole::usesOperatorEg (processorRef, index) ? "FM operator, OP ENV" : "FM operator";
        return OscRole::modeName (getMode (index));
    }

    // The strip (ilana's mockup, 2026-10-22): one row of six cards under the
    // oscillators, STEREO, NOISE, SUB, STRINGS, SOUNDBOARD and KEYS. Each card
    // has a 24 px header (its name; SUB, STRINGS and SOUNDBOARD their switch at
    // the right), one row of 36 px dials, and a lane at the foot for its menu or
    // switch. The strings' six notes open over the page from Manual tuning.
    std::vector<CardGeometry::Panel> stripPanels;

    static int textWidth (const juce::String& text, float size, bool bold = false)
    {
        return juce::GlyphArrangement::getStringWidthInt (juce::Font (IlanaTheme::font (size, bold)), text) + 2;
    }

    static constexpr float stripTitleText = 12.5f, stripLaneText = 11.0f, stripNameText = 11.5f; // (the mockup's sizes at 1060 px)

    // Dials and menus spread over a card with equal space around each, on the card's one dial row.
    void placeStripRow (juce::Rectangle<int> card, std::initializer_list<juce::Component*> items, int cell = stripCell) const
    {
        const auto content = card.reduced (stripPad, 0);
        const auto widthOf = [cell] (juce::Component* item) { return dynamic_cast<KnobControl*> (item) != nullptr ? cell : stripMenuW; };
        auto used = 0;
        for (auto* item : items)
            used += widthOf (item);
        const auto each = (float) juce::jmax (0, content.getWidth() - used) / (float) juce::jmax (1, (int) items.size());
        auto x = (float) content.getX() + each * 0.5f;
        for (auto* item : items)
        {
            const auto left = juce::roundToInt (x);
            if (auto* knob = dynamic_cast<KnobControl*> (item)) // (the name's room is two lines: two-word names wrap)
                knob->setBounds (KnobControl::packedBoundsFor ({ left, card.getY() + stripDialTop, cell, stripDial + nameLine + stripWrapLine }));
            else // (a compact menu: its name over an 18 px box, centred on the dials)
                item->setBounds ({ left, card.getY() + stripDialTop + 2, stripMenuW, 32 });
            x += (float) widthOf (item) + each;
        }
    }

    // A card's foot: a name, then its menu to the card's right edge.
    void placeLaneMenu (juce::Rectangle<int> card, const juce::String& name, ComboControl& menu)
    {
        const auto lane = juce::Rectangle<int> (card.getX() + stripPad, card.getY() + stripLaneTop, card.getWidth() - 2 * stripPad, stripLaneH);
        const auto nameW = textWidth (name, stripLaneText, true) + 4;
        laneLabels.push_back ({ lane.withWidth (nameW), name });
        menu.setBounds (lane.withTrimmedLeft (nameW + 2));
    }

    void layoutStrip()
    {
        stripPanels.clear();
        laneLabels.clear();

        // The mockup's widths (152, 108, 102, 160, 160, 324 of 1006 px), to the page's width.
        static constexpr int weights[] { 182, 104, 102, 147, 147, 324 };
        static const char* const titles[] { "STEREO", "NOISE", "SUB", "STRINGS", "SOUNDBOARD", "KEYS" };
        const auto avail = (float) (stripArea.getWidth() - 5 * stripGap);
        std::array<juce::Rectangle<int>, 6> card;
        auto x = stripArea.getX();
        for (size_t i = 0; i < card.size(); ++i)
        {
            const auto width = i + 1 == card.size() ? stripArea.getRight() - x : juce::roundToInt ((float) weights[i] * avail / 1006.0f);
            card[i] = { x, stripArea.getY(), width, stripCardH };
            stripPanels.push_back ({ card[i], titles[i], {}, IlanaTheme::accent() });
            x += width + stripGap;
        }

        // The switches at the headers' right.
        for (const auto& [toggle, index] : { std::pair<ToggleControl*, size_t> { subOscOn.get(), 2 }, { &symOn, 3 }, { &sbOn, 4 } })
            toggle->setBounds ({ card[index].getRight() - stripPad - 26, card[index].getY() + (stripHeaderH - 14) / 2 + 1, 26, 14 });

        placeStripRow (card[0], { voiceSpread.get(), unisonRandom.get(), drift.get(), tuneLoose.get() }, stripCellStereo);
        looseBoth.setBounds (card[0].withTrimmedLeft (stripPad).withTrimmedRight (stripPad).withTop (card[0].getY() + stripLaneTop).withHeight (stripLaneH).withSizeKeepingCentre (card[0].getWidth() - 2 * stripPad, 15));
        placeStripRow (card[1], { noiseStrip.get(), noiseColourStrip.get() });
        placeStripRow (card[2], { subOscLevel.get(), &subOctave }, 36);
        placeStripRow (card[3], { &symAmount, &symCount, &symDecay });
        placeStripRow (card[4], { &sbSize, &sbTone, &sbMix });
        placeStripRow (card[5], { &stretch, &pedalRes, &mechKey, &mechDamper, &mechPedal }, 62); // (room for RESONANCE)

        placeLaneMenu (card[1], "TYPE", noiseType);
        placeLaneMenu (card[2], "WAVE", subShape);
        placeLaneMenu (card[4], "MODEL", sbModel);
        {
            const auto lane = juce::Rectangle<int> (card[3].getX() + stripPad, card[3].getY() + stripLaneTop, card[3].getWidth() - 2 * stripPad, stripLaneH);
            tuneButton.setBounds (lane.withLeft (lane.getRight() - 22));
            symManual.setBounds (lane.withTrimmedRight (26).withSizeKeepingCentre (lane.getWidth() - 26, 15));
            stringsCard = card[3];
        }

        if (tuningPopover.isVisible())
            tuningPopover.setBounds (tuningBounds());
    }

    // The strings' six notes, over the cards right above the STRINGS card.
    juce::Rectangle<int> stringsCard;

    juce::Rectangle<int> tuningBounds() const
    {
        const auto width = TuningPopover::widthFor (6), height = TuningPopover::height;
        const auto left = juce::jlimit (sideMargin, getWidth() - sideMargin - width, stringsCard.getCentreX() - width / 2);
        return { left, stringsCard.getY() - height - 4, width, height };
    }

    void showTuning (bool show)
    {
        show = show && readBool ("sym_manual") && ! embedded;
        if (show == (tuningPopover.isVisible() && ! tuningGrow.isClosing()))
            return;

        if (show)
        {
            tuningPopover.setBounds (tuningBounds());
            tuningPopover.setVisible (true);
            tuningPopover.toFront (false);
            tuningGrow.open (getLocalPoint (&tuneButton, tuneButton.getLocalBounds().getCentre()).toFloat());
        }
        else
        {
            tuningGrow.close ([safe = juce::Component::SafePointer<juce::Component> (&tuningPopover)]
                              {
                                  if (safe != nullptr)
                                      safe->setVisible (false);
                              });
        }
        tuneButton.setToggleState (show, juce::dontSendNotification);
    }

public:
    bool isTuningShown() const { return tuningPopover.isVisible(); }

private:
    void paintStrip (juce::Graphics& g) const
    {
        for (const auto& panel : stripPanels)
        {
            const auto area = panel.area.toFloat();
            g.setColour (IlanaTheme::Ui::raised.interpolatedWith (IlanaTheme::Ui::panel, 0.55f));
            g.fillRoundedRectangle (area, 7.0f);
            {
                juce::Path shape;
                shape.addRoundedRectangle (area, 7.0f);
                const juce::Graphics::ScopedSaveState state (g);
                g.reduceClipRegion (panel.area.withHeight (2));
                g.setColour (panel.colour);
                g.fillPath (shape);
            }
            g.setColour (IlanaTheme::Ui::line);
            g.drawRoundedRectangle (area.reduced (0.5f), 6.5f, 1.0f);
            g.fillRect (panel.area.getX() + 1, panel.area.getY() + stripHeaderH, panel.area.getWidth() - 2, 1);

            g.setColour (panel.colour);
            g.setFont (IlanaTheme::font (stripTitleText, true).withKerningFactor (0.06f));
            IlanaTheme::drawFitted (g, panel.title, juce::Rectangle<int> (panel.area.getX() + stripPad, panel.area.getY() + 2, panel.area.getWidth() - 2 * stripPad - 30, stripHeaderH - 2),
                                    juce::Justification::centredLeft, 1);
        }

        g.setColour (IlanaTheme::Ui::text2);
        g.setFont (IlanaTheme::font (stripLaneText, true).withKerningFactor (0.06f));
        for (const auto& [area, name] : laneLabels)
            g.drawText (name, area, juce::Justification::centredLeft, false);
    }

    // The strip's controls in the packed size, like the cards'.
    void applyStripStyle()
    {
        for (auto* knob : { voiceSpread.get(), unisonRandom.get(), drift.get(), tuneLoose.get(), noiseStrip.get(), noiseColourStrip.get(), subOscLevel.get(),
                            &symAmount, &symCount, &symDecay, &sbSize, &sbTone, &sbMix, &stretch, &pedalRes, &mechKey, &mechDamper, &mechPedal })
        {
            knob->setPackedDial ((float) stripDial);
            knob->setPackedNameWrap (true, true); // (two-word names on two lines, as the mockup's)
            knob->setPackedNameSize (stripNameText);
        }
        for (auto& note : symNotes)
            note->setPackedDial ((float) stripDial);
        for (auto* menu : { &subShape, &sbModel, &noiseType })
        {
            menu->setPackedMenu (true);
            menu->getComboBox().getProperties().set ("textPrefix", juce::String()); // (the name stands before the box)
            menu->getComboBox().getProperties().set ("packedMenuSize", stripTitleText);
        }
        symManual.setPackedName (ToggleControl::NamePlace::right);
        symManual.setRightNameSize (stripNameText);
        looseBoth.setPackedName (ToggleControl::NamePlace::right);
        looseBoth.setRightNameSize (stripNameText);
        if (! tuneButton.getProperties().contains ("packedButton"))
            tuneButton.getProperties().set ("packedButton", true);
    }

    void scrollTo (int osc)
    {
        auto* viewport = findParentComponentOfClass<juce::Viewport>();

        if (viewport == nullptr)
            return;

        const auto area = osc >= 0 ? getCardBounds (osc) : stripArea;

        if (area.isEmpty())
            return;

        const auto top = viewport->getViewPositionY();
        const auto height = viewport->getViewHeight();

        if (area.getY() < top)
            viewport->setViewPosition (0, juce::jmax (0, area.getY() - topMargin));
        else if (area.getBottom() + bottomMargin > top + height)
            viewport->setViewPosition (0, area.getBottom() + bottomMargin - height);
    }

    juce::String layoutKey() const
    {
        juce::String key;
        for (const auto osc : shownList())
        {
            const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
            key << osc << ":" << getMode (osc) << (OscRole::usesOperatorEg (processorRef, osc) ? "o" : "-") << OscRole::tuning (processorRef, osc)
                << (readFloat (prefix + "_unison") > 1.5f ? "u" : "-") << (isElectric (osc) ? "e" : "-")
                << (getMode (osc) == 1 ? juce::String (juce::roundToInt (readFloat (prefix + "_excite"))) : juce::String()) << ";";
        }
        key << (readBool ("sym_manual") ? "m" : "-") << (canAdd() ? "+" : "=");
        return key;
    }

    juce::String shownKey;

    // What a card holds besides the controls every oscillator has: the pictures
    // the modes need (each card has its own, so all can be live at once) and
    // their buttons.
    struct CardAux
    {
        CardAux (IlanaSynthAudioProcessor& p, int index)
            : stringView (p, juce::String (OscillatorIds::prefixes[(size_t) index])), opEnvGraph (p),
              spreadView (p, index), partials (p, index), status (p, index), outBar (p, index),
              xfadeBar (p.apvts, juce::String (OscillatorIds::prefixes[(size_t) index]) + "_sample_loop_xfade", "CROSSFADE") {}

        PhysicalView stringView;
        OperatorEnvDisplay opEnvGraph;
        UnisonSpreadView spreadView;
        StringPartialsView partials;
        OscStatusView status;
        // The packed cards (review 23): the header's type bar and table steps, the
        // MIX panel's filter route and output bar, the WAVE panel's + WARP.
        SegmentBar typeBar, routeBar;
        OscStatusView outBar;
        juce::TextButton prevTable { juce::String::fromUTF8 ("\xe2\x80\xb9") }, nextTable { juce::String::fromUTF8 ("\xe2\x80\xba") };
        IconPicker exciterPills; // (the exciter types as small icons, the name on hover)
        SampleNameBox sampleNameButton;
        BarSliderControl xfadeBar; // the sample's CROSSFADE, a row of the toggle list
        juce::TextButton opEnvButton, stringButton, sampleLoadButton, sampleSliceButton, sampleExportButton, sampleZonesButton, sampleDetectButton;
        // The header's x: remove this oscillator. Drawn as two strokes, as big as
        // the buttons' capitals (a text x read small).
        struct RemoveButton : juce::TextButton
        {
            void paintButton (juce::Graphics& g, bool over, bool down) override
            {
                getLookAndFeel().drawButtonBackground (g, *this, findColour (buttonColourId), over, down);
                const auto centre = getLocalBounds().toFloat().getCentre();
                const auto arm = 3.0f;
                g.setColour (findColour (textColourOffId).withMultipliedAlpha (over ? 1.0f : 0.85f));
                g.drawLine (centre.x - arm, centre.y - arm, centre.x + arm, centre.y + arm, 1.4f);
                g.drawLine (centre.x - arm, centre.y + arm, centre.x + arm, centre.y - arm, 1.4f);
            }
        } removeButton;
    };

    // Every knob a card can show, for the one inline style.
    static std::vector<KnobControl*> cardKnobs (OscControls& osc)
    {
        return { &osc.level, &osc.pan, &osc.semi, &osc.fine, &osc.unison, &osc.detune, &osc.spread,
                 &osc.stringDecay, &osc.stringDamp, &osc.stringSustain, &osc.sampleStart, &osc.sampleEnd,
                 &osc.sampleFadeIn, &osc.sampleFadeOut, &osc.sampleLoopXfade, &osc.sampleStartRandom, &osc.sampleRoot, &osc.sampleBpm, &osc.warpAmt, &osc.uniBlend, &osc.uniFrame, &osc.uniWarp, &osc.spectralAmt,
                 &osc.grainPosition, &osc.grainScan, &osc.grainSize, &osc.grainDensity, &osc.grainSpray, &osc.grainPitch,
                 &osc.grainSpread, &osc.warp2Amt, &osc.ratio, &osc.fixedHz, &osc.egOut,
                 &osc.trim, &osc.feedback };
    }

    std::vector<KnobControl*> stripKnobs()
    {
        std::vector<KnobControl*> knobs { voiceSpread.get(), unisonRandom.get(),
                                          drift.get(), tuneLoose.get(), noiseStrip.get(), noiseColourStrip.get(), subOscLevel.get(), &symAmount,
                                          &symDecay, &symCount, &sbMix, &sbTone, &sbSize, &stretch, &pedalRes, &mechKey,
                                          &mechDamper, &mechPedal };
        for (auto& note : symNotes)
            knobs.push_back (note.get());
        return knobs;
    }

    bool readBool (const juce::String& id) const
    {
        if (const auto* value = processorRef.apvts.getRawParameterValue (id))
            return value->load() > 0.5f;

        return true;
    }

    float readFloat (const juce::String& id) const
    {
        const auto* value = processorRef.apvts.getRawParameterValue (id);
        return value != nullptr ? value->load() : 0.0f;
    }

    bool isOff (int index) const
    {
        return ! readBool (juce::String (OscillatorIds::prefixes[(size_t) juce::jlimit (0, OscillatorIds::count - 1, index)]) + "_on");
    }

    int numShown() const
    {
        auto count = 0;
        for (int i = 0; i < OscillatorIds::count; ++i)
            count += processorRef.isOscillatorShown (i) ? 1 : 0;
        return count;
    }

    // The hidden picker's order: the shown oscillators.
    int oscForTab (int tab) const
    {
        for (int osc = 0, index = 0; osc < OscillatorIds::count; ++osc)
            if (processorRef.isOscillatorShown (osc) && index++ == tab)
                return osc;

        return selected;
    }

    // The list the tests pick an oscillator from (not drawn), each entry's role
    // in its tooltip; also the OUTPUT name of an operator's level.
    void updateTabItems()
    {

        const auto shown = shownList();

        oscTabs.setOscillators (shown, [this] (int osc) { return ! isOff (osc); },
                                [this] (int osc)
                                {
                                    const auto role = OscRole::describe (processorRef, osc);
                                    return "OSC " + juce::String (osc + 1) + ": " + OscRole::modeName (getMode (osc))
                                           + (role.isNotEmpty() ? ", " + OscRole::describeLong (processorRef, osc) : juce::String())
                                           + (isOff (osc) ? ", switched off" : "") + ".  Right-click its header to switch it off or remove it.";
                                },
                                [this] (int osc)
                                {
                                    if (osc != selected || isOff (osc))
                                        return juce::String();
                                    // (The tag is the type: FM / DX7 for an operator.)
                                    return OscRole::modeName (getMode (osc)).toUpperCase();
                                });
        oscTabs.setSelectedOsc (selected);

        for (int osc = 0; osc < OscillatorIds::count; ++osc)
            if (const auto name = juce::String (OscRole::outputKnobName (processorRef, osc)); controls[(size_t) osc]->egOut.getLabelText() != name)
                controls[(size_t) osc]->egOut.setLabelText (name);
    }

    // What the PITCH row's right-hand cells say: the FM role in a chip as
    // long as the cells can hold, else shorter, else none (the output bar).
    void updateStatus (int index)
    {
        auto& view = aux[(size_t) index]->status;
        const auto tint = oscColour (index);
        juce::StringArray candidates;
        auto colour = tint;
        const auto sources = OscRole::sources (processorRef, index);
        const auto targets = OscRole::targets (processorRef, index);

        if (! sources.empty())
            colour = oscColour (sources.front());
        else if (! targets.empty())
            colour = oscColour (targets.front());

        if (OscRole::isOperator (processorRef, index))
        {
            const auto role = OscRole::describe (processorRef, index);
            candidates.add ("OPERATOR  " + role);
            candidates.add (role.isNotEmpty() ? role : juce::String ("OPERATOR"));
            candidates.add ("OP");
        }
        else if (const auto role = OscRole::describeLong (processorRef, index); role.isNotEmpty())
        {
            candidates.add (role);
            candidates.add (OscRole::describe (processorRef, index));
            candidates.add ("FM");
        }

        juce::String chip;
        const auto room = juce::jmax (40, view.getWidth() - 26);
        for (const auto& candidate : candidates)
            if (candidate.isNotEmpty() && juce::GlyphArrangement::getStringWidthInt (juce::Font (IlanaTheme::font (6.8f, true).withKerningFactor (0.06f)), candidate) <= room)
            {
                chip = candidate;
                break;
            }

        const auto link = chip.isNotEmpty() && FmOperatorInfo::hooks().openOperator != nullptr;
        const auto full = OscRole::describeLong (processorRef, index);
        view.update (chip, colour, link, chip.isEmpty() ? "The oscillator's output level" : full + (link ? "\nClick to open it on the FM page" : ""));
        aux[(size_t) index]->outBar.update ({}, tint, false, "The oscillator's output level (its LEVEL, before the filters)");
    }

    // The pictures that follow the sound: the unison's spread, the FM chip and
    // the output bar (polled with the timer).
    void updateCardPictures()
    {
        for (const auto index : shownList())
        {
            aux[(size_t) index]->spreadView.update();
            aux[(size_t) index]->partials.update();
            updateStatus (index);

            const auto* sample = processorRef.getSampleForOsc (index);
            aux[(size_t) index]->sampleNameButton.setCaptionAndName (kindOf (index) == Kind::grain ? "SOURCE" : "SAMPLE",
                                                                     sample != nullptr && sample->getNumSamples() >= 2 ? (sample->name.isNotEmpty() ? sample->name : juce::String ("Loaded sample")) : juce::String ("None"));
        }

        getProperties().set ("caption", OscRole::describeLong (processorRef, selected)); // for the UI test
    }

    // An oscillator card's right-click menu.
    // Colour coding: the swatches, by the card's dot.
    void showColourPicker (int band)
    {
        for (const auto& card : cards)
            if (card.osc == band)
                SectionColourPicker::show (processorRef, SectionColours::oscKey (band), "OSC " + juce::String (band + 1), *this,
                                           juce::Rectangle<int> (card.bounds.getX() + 4, card.header.getY(), 20, card.header.getHeight()));
    }

    void showOscMenu (int band)
    {
        const auto on = ! isOff (band);
        juce::PopupMenu menu;
        menu.addSectionHeader ("OSC " + juce::String (band + 1));
        menu.addItem (1, on ? "Switch off" : "Switch on");
        menu.addItem (3, "Duplicate oscillator", canAdd());
        menu.addItem (2, "Remove oscillator", numShown() > 1);
        ModuleMenu::addItems (menu, processorRef, ModuleMenu::Kind::Oscillator, band);
        menu.addSeparator();
        menu.addItem (4, "Colour...");
        juce::Component::SafePointer<OscPage> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                            [safe, band, on] (int result)
                            {
                                if (safe == nullptr || result == 0)
                                    return;

                                auto& p = safe->processorRef;
                                const auto name = "OSC " + juce::String (band + 1);

                                if (result == 4)
                                {
                                    safe->showColourPicker (band);
                                    return;
                                }

                                if (result == 2)
                                    safe->removeOscillatorCard (band);
                                else if (result == 3)
                                    safe->duplicate (band);
                                else if (auto* parameter = p.apvts.getParameter (juce::String (OscillatorIds::prefixes[(size_t) band]) + "_on"))
                                    p.performEdit (name + (on ? " off" : " on"), [parameter, on]
                                    {
                                        parameter->beginChangeGesture();
                                        parameter->setValueNotifyingHost (on ? 0.0f : 1.0f);
                                        parameter->endChangeGesture();
                                    });

                                safe->updateModeVisibility();
                                safe->updateEnabled();
                            });
    }

    WaveDisplay& waveDisplay (int index)
    {
        return *waveDisplays[(size_t) juce::jlimit (0, OscillatorIds::count - 1, index)];
    }

    juce::TextButton& loadButton (int index)
    {
        return *loadButtons[(size_t) juce::jlimit (0, OscillatorIds::count - 1, index)];
    }

    int getMode (int oscIndex) const
    {
        const auto id = juce::String (OscillatorIds::prefixes[(size_t) juce::jlimit (0, OscillatorIds::count - 1, oscIndex)]) + "_mode";
        const auto* value = processorRef.apvts.getRawParameterValue (id);
        return value != nullptr ? (int) value->load() : 0;
    }

    void setupLoadButton (juce::TextButton& button, const juce::String& tableId, int tableChoiceOffset)
    {
        button.setTooltip ("Load a wavetable (.wav of single-cycle frames), or turn any recording into a wavetable");
        button.onClick = [this, &button, tableId, tableChoiceOffset]
        {
            if (chooserOpen)
                return;

            juce::PopupMenu menu;
            menu.addItem (1, "Load wavetable file...");
            menu.addItem (2, "Make a wavetable from any audio...");
            menu.addItem (4, "Vocode: harmonics follow the audio's spectrum...");
            menu.addItem (5, "Time slice: equal slices of the audio, one cycle each...");
            menu.addSeparator();
            menu.addItem (3, "(Any audio: the pitch is detected and one cycle per frame is taken across the file)", false);

            juce::Component::SafePointer<OscPage> safeMenu (this);
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&button),
                                [safeMenu, tableId, tableChoiceOffset] (int result)
                                {
                                    if (safeMenu != nullptr && (result == 1 || result == 2 || result == 4 || result == 5))
                                        safeMenu->chooseTable (tableId, tableChoiceOffset,
                                                               result == 2 ? Wavetable::LoadMode::Resynthesize
                                                               : result == 4 ? Wavetable::LoadMode::Vocode
                                                               : result == 5 ? Wavetable::LoadMode::TimeSlice
                                                                             : Wavetable::LoadMode::Automatic);
                                });
        };
    }

    void chooseTable (const juce::String& tableId, int tableChoiceOffset, Wavetable::LoadMode mode)
    {
        chooserOpen = true;

        for (int index = 0; index < OscillatorIds::count; ++index)
            loadButton (index).setEnabled (false);

        if (tableChooser == nullptr)
            tableChooser = std::make_unique<juce::FileChooser> (
                "Load Wavetable or Audio",
                juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

        juce::Component::SafePointer<OscPage> safeThis (this);

        tableChooser->launchAsync (juce::FileBrowserComponent::openMode
                                       | juce::FileBrowserComponent::canSelectFiles,
                                   [safeThis, tableId, tableChoiceOffset, mode] (const juce::FileChooser& chooser)
                                   {
                                       if (safeThis == nullptr)
                                           return;

                                       safeThis->chooserOpen = false;
                                       safeThis->updateEnabled();

                                       const auto file = chooser.getResult();

                                       if (file.existsAsFile())
                                           safeThis->loadTableFile (tableId, tableChoiceOffset, file, mode);
                                   });
    }

    // Loads a chosen or dropped audio file as a wavetable into a User slot and
    // selects it on the oscillator whose table choice is tableId.
    void loadTableFile (const juce::String& tableId, int tableChoiceOffset, const juce::File& file, Wavetable::LoadMode mode)
    {
        const auto factoryCount = TableFactory::getNumFactoryTables();
        const auto domain = juce::jmax (0, readTableChoiceIndex (tableId) - tableChoiceOffset);
        // A User table selected is replaced; with a factory table
        // selected the file goes to a free User slot (never over
        // another oscillator's table).
        auto slot = domain >= factoryCount && domain < factoryCount + IlanaSynthAudioProcessor::numUserSlots
                        ? juce::jlimit (0, IlanaSynthAudioProcessor::numUserSlots - 1, domain - factoryCount)
                        : processorRef.findFreeUserSlot();

        if (slot < 0)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Load wavetable",
                                                    "All " + juce::String (IlanaSynthAudioProcessor::numUserSlots) + " patch tables are in use. Pick a User table to replace it.");
            return;
        }

        processorRef.beginEdit ("Load wavetable"); // the table and the choice: one undo step
        if (processorRef.loadUserWavetable (slot, file, mode))
        {
            if (auto* parameter = processorRef.apvts.getParameter (tableId))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) (tableChoiceOffset + factoryCount + slot)));
        }
        processorRef.endEdit();
    }

    // A file dropped on a wavetable oscillator's picture: the same ways the
    // LOAD button offers, plus using it as a sample.
    void askDroppedFile (int oscIndex, const juce::File& file)
    {
        const auto tableId = juce::String (OscillatorIds::prefixes[(size_t) oscIndex]) + "_table";
        juce::PopupMenu menu;
        menu.addSectionHeader (file.getFileName());
        menu.addItem (1, "Load as a wavetable");
        menu.addItem (2, "Make a wavetable from this audio");
        menu.addItem (4, "Vocode: harmonics follow the audio's spectrum");
        menu.addItem (5, "Time slice: equal slices, one cycle each");
        menu.addSeparator();
        menu.addItem (6, "Use as a sample instead");

        juce::Component::SafePointer<OscPage> safeThis (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&waveDisplay (oscIndex)),
                            [safeThis, tableId, file, oscIndex] (int result)
                            {
                                if (safeThis == nullptr || result == 0)
                                    return;

                                if (result == 6)
                                    safeThis->waveDisplay (oscIndex).loadSampleFile (file);
                                else
                                    safeThis->loadTableFile (tableId, 0, file,
                                                             result == 2 ? Wavetable::LoadMode::Resynthesize
                                                             : result == 4 ? Wavetable::LoadMode::Vocode
                                                             : result == 5 ? Wavetable::LoadMode::TimeSlice
                                                                           : Wavetable::LoadMode::Automatic);
                            });
    }

    // Every component of one oscillator's card, to hide before the shown ones
    // are switched on.
    std::vector<juce::Component*> componentsOf (int i)
    {
        auto& osc = *controls[(size_t) i];
        auto& phys = *physical[(size_t) i];
        auto& pieces = *aux[(size_t) i];
        return { &osc.on, &osc.sampleTuned, &osc.sampleLoop, &osc.sampleReverse, &osc.sampleLoopOwn, &osc.sampleSnap, &osc.mode, &osc.table, &osc.frameBlend,
                 &osc.excite, &osc.chord, &osc.ampEnv, &osc.warp, &osc.uniMode, &osc.scale, &osc.scaleRoot, &osc.spectral,
                 &osc.level, &osc.pan, &osc.semi, &osc.fine, &osc.unison, &osc.detune, &osc.spread,
                 &osc.stringDecay, &osc.stringDamp, &osc.stringSustain, &osc.sampleStart, &osc.sampleEnd,
                 &osc.sampleFadeIn, &osc.sampleFadeOut, &osc.sampleLoopXfade, &osc.sampleRoot, &osc.sampleSync, &osc.sampleBpm, &osc.sampleStretch, &osc.samplePlayback, &osc.samplePingPong, &osc.sampleStartRandom, &osc.sampleStartSafe, &osc.sampleQuality,
                 &osc.warpAmt, &osc.uniBlend, &osc.uniFrame, &osc.uniWarp, &osc.spectralAmt,
                 &osc.grainPosition, &osc.grainScan, &osc.grainFreeze, &osc.grainSize, &osc.grainDensity, &osc.grainSpray, &osc.grainPitch,
                 &osc.grainSpread, &osc.grainLive, &osc.warp2, &osc.warp2Amt,
                 &osc.tune, &osc.ratio, &osc.fixedHz, &osc.egOut, &osc.trim, &osc.feedback, &osc.feedbackType,
                 &phys.stiffness, &phys.pickup, &phys.click, &phys.excitePos, &phys.hardness,
                 &phys.pickPos, &phys.bowPressure, &phys.bowSpeed, &phys.bridgeBuzz, &phys.fretRattle,
                 &phys.hammer, &phys.couple, &phys.damper, &phys.registerMap, &phys.slap,
                 &phys.epDistance, &phys.epPosition, &phys.fbGain, &phys.fbDistance, &waveDisplay (i), &scrubbers[(size_t) i], &loadButton (i), editButtons[(size_t) i].get(),
                 bounceButtons[(size_t) i].get(), &pieces.stringView, &pieces.opEnvGraph, &pieces.spreadView, &pieces.partials, &pieces.status,
                 &pieces.opEnvButton, &pieces.stringButton, &pieces.sampleLoadButton, &pieces.sampleSliceButton, &pieces.sampleExportButton, &pieces.sampleZonesButton, &pieces.sampleDetectButton, &pieces.sampleNameButton,
                 &pieces.removeButton, &pieces.typeBar, &pieces.routeBar, &pieces.outBar, &pieces.prevTable, &pieces.nextTable, &pieces.exciterPills,
                 &phys.resAmount, &phys.resOn, &phys.resBoard };
    }

    // A card's controls in the packed style (the panels) or the grid's inline one: the
    // same controls serve every type, so this runs before each layout.
    void applyCardStyle (int index)
    {
        const auto kind = kindOf (index);
        const auto panels = usesPanels (kind);
        auto& osc = *controls[(size_t) index];

        for (auto* knob : { &osc.level, &osc.pan, &osc.semi, &osc.fine, &osc.ratio, &osc.fixedHz, &osc.unison, &osc.detune, &osc.spread,
                            &osc.uniBlend, &osc.uniFrame, &osc.uniWarp, &osc.egOut, &osc.sampleStart, &osc.sampleEnd,
                            &osc.sampleFadeIn, &osc.sampleFadeOut, &osc.sampleLoopXfade, &osc.sampleRoot, &osc.sampleBpm, &osc.sampleStartRandom,
                            &osc.grainPosition, &osc.grainScan, &osc.grainSize, &osc.grainDensity, &osc.grainSpray, &osc.grainPitch, &osc.grainSpread })
            knob->setPackedDial (panels ? (float) mainDial : 0.0f);
        // (VOICES at the same dial as its row on every card, as mockup v4: UI review 26 found it 7 px off the row's baseline.)
        osc.unison.setPackedDial (panels ? (float) mainDial : 0.0f);
        osc.feedback.setPackedDial (kind == Kind::fm ? (float) mainDial : 0.0f);
        osc.sampleLoopXfade.setPackedDial (panels ? (float) mainDial : 0.0f); // (the regular dial, third row of the toggle grid)
        // (The unit of an operator's OUTPUT and of a cloud's SIZE sits under the number, as on every packed dial,
        // so "0.0 dB" and "80 ms" keep clear of the ring.)
        osc.egOut.setPackedValueRoom (panels ? 12 : 0);
        // (The same for a ratio and a cloud's SCAN: "×1.00" and "0.00x" lose their last digit's room, not their size.)
        osc.ratio.setPackedValueRoom (panels ? 12 : 0);
        osc.fixedHz.setPackedValueRoom (panels ? 12 : 0);
        osc.grainScan.setPackedValueRoom (panels ? 12 : 0);
        osc.grainSize.setPackedValueRoom (panels ? 12 : 0);
        for (auto* knob : { &osc.warpAmt, &osc.warp2Amt, &osc.spectralAmt })
            knob->setPackedDial (panels ? (float) ampDial : 0.0f);

        for (auto* menu : { &osc.warp, &osc.warp2, &osc.spectral, &osc.tune, &osc.ampEnv, &osc.scale, &osc.scaleRoot, &osc.chord, &osc.uniMode, &osc.feedbackType, &osc.sampleSync, &osc.sampleStretch, &osc.samplePlayback, &osc.sampleQuality })
            menu->setPackedMenu (panels);
        osc.table.setPackedMenu (kind == Kind::wavetable || kind == Kind::fm);
        osc.frameBlend.setPackedMenu (kind == Kind::wavetable);
        waveDisplay (index).setFrameReadout (kind != Kind::wavetable);

        // Physical (review 23): its own knobs packed too, the switch with its name under it.
        const auto string = kind == Kind::physical;
        auto& phys = *physical[(size_t) index];
        for (auto* knob : { &phys.stiffness, &phys.pickup, &phys.click, &phys.excitePos, &phys.hardness, &phys.pickPos, &phys.bowPressure, &phys.bowSpeed,
                            &phys.bridgeBuzz, &phys.fretRattle, &phys.hammer, &phys.couple, &phys.damper, &phys.registerMap,
                            &phys.epDistance, &phys.epPosition, &phys.fbGain, &phys.fbDistance, &phys.resAmount,
                            &osc.stringDecay, &osc.stringDamp, &osc.stringSustain })
            knob->setPackedDial (string ? (float) mainDial : 0.0f);
        phys.slap.setPackedName (string ? ToggleControl::NamePlace::leftFlush : ToggleControl::NamePlace::none);
        // The sample card's switches: the pill with its name to the right, as the packed menus read.
        for (auto* toggle : { &osc.sampleTuned, &osc.sampleLoop, &osc.sampleReverse, &osc.sampleLoopOwn, &osc.sampleSnap, &osc.samplePingPong, &osc.sampleStartSafe,
                              &osc.grainFreeze, &osc.grainLive })
        {
            toggle->setPackedName (kind == Kind::sample || kind == Kind::grain ? ToggleControl::NamePlace::leftFlush : ToggleControl::NamePlace::none);
            toggle->setSwitchColour (oscColour (index)); // (the card's amber, not the app's orange)
        }
        // (FREEZE and LIVE are chips on the cloud picture: the pill first, its name after.)
        for (auto* toggle : { &osc.grainFreeze, &osc.grainLive })
        {
            const auto chip = kind == Kind::grain;
            auto& properties = toggle->getButton().getProperties();
            if (chip)
            {
                properties.remove ("switch");
                properties.set ("pill", true);
                properties.set ("pillFontSize", 6.0f); // the mock's small chip caption
            }
            else
            {
                properties.set ("switch", true);
                properties.remove ("pill");
            }
            toggle->getButton().setColour (juce::TextButton::buttonOnColourId, oscColour (index));
            toggle->setPackedName (ToggleControl::NamePlace::none);
            toggle->setBareSwitch (chip);
            toggle->getButton().repaint();
        }
        osc.sampleStartRandom.setLabelText (kind == Kind::sample ? "RANDOM" : "RANDOM START");
        osc.samplePlayback.setLabelText (kind == Kind::sample ? "MODE" : "PLAYBACK");
        phys.resBoard.setPackedMenu (string);
        osc.ampEnv.setLabelText (panels ? "ENV" : "ENVELOPE");
        osc.tune.setLabelText ("TUNING");
        // (These two say what they are by their value alone.)
        osc.scaleRoot.getComboBox().getProperties().set ("textPrefix", juce::String());
        osc.uniMode.getComboBox().getProperties().set ("textPrefix", juce::String());
        osc.scaleRoot.getComboBox().repaint();
        osc.uniMode.getComboBox().repaint();

        // The header's buttons: the packed size, whatever the type.
        auto& pieces = *aux[(size_t) index];
        for (auto* button : { &loadButton (index), &pieces.sampleLoadButton, &pieces.sampleDetectButton, &pieces.sampleZonesButton, &pieces.sampleSliceButton, &pieces.sampleExportButton, &pieces.opEnvButton, &pieces.stringButton,
                              editButtons[(size_t) index].get(), bounceButtons[(size_t) index].get(), &pieces.prevTable, &pieces.nextTable })
            if (button != nullptr && ! button->getProperties().contains ("packedButton"))
                button->getProperties().set ("packedButton", true);
        pieces.status.setPart (panels ? OscStatusView::Part::chipOnly : OscStatusView::Part::either);
    }

    // The cards show every shown oscillator with its own controls; the strip
    // its groups (the extras only while MORE is open).
    void updateModeVisibility()
    {
        if (! processorRef.isOscillatorShown (selected))
            for (int i = 0; i < OscillatorIds::count; ++i)
                if (processorRef.isOscillatorShown (i))
                {
                    selected = i;
                    break;
                }

        for (int i = 0; i < OscillatorIds::count; ++i)
            for (auto* component : componentsOf (i))
                component->setVisible (false);

        for (const auto index : shownList())
        {
            const auto kind = kindOf (index);
            applyCardStyle (index);
            auto& osc = *controls[(size_t) index];
            auto& pieces = *aux[(size_t) index];
            const juce::String prefix (OscillatorIds::prefixes[(size_t) index]);
            osc.stringSustain.setLabelText (juce::roundToInt (readFloat (prefix + "_excite")) == 10 ? "FEEDBACK" : "SUSTAIN");

            for (const auto& slot : slotsFor (index))
                slot.item->setVisible (true);

            osc.on.setVisible (true);
            // (The header's type bar replaces the type menu; it stays as the model of the choice.)
            osc.mode.setVisible (false);
            pieces.typeBar.setVisible (true);
            pieces.typeBar.refresh();
            const auto wavetableCard = usesPanels (kind) && kind == Kind::wavetable;
            pieces.prevTable.setVisible (wavetableCard);
            pieces.nextTable.setVisible (wavetableCard);
            pieces.routeBar.setVisible (wavetableCard || kind == Kind::live || kind == Kind::fm || kind == Kind::sample || kind == Kind::grain || kind == Kind::physical);
            pieces.outBar.setVisible (wavetableCard || kind == Kind::live || kind == Kind::fm || kind == Kind::sample || kind == Kind::grain);
            pieces.exciterPills.setVisible (kind == Kind::physical);
            if (kind == Kind::physical)
                pieces.exciterPills.refresh();
            if (wavetableCard || kind == Kind::live || kind == Kind::fm || kind == Kind::sample || kind == Kind::grain)
                pieces.routeBar.refresh();
            // An FM / DX7 oscillator plays a plain cycle: no table tools
            // (UI review 8, I8-15, S8-9, V8-16); on the Operator EG no
            // resampling either.
            const auto opEnv = kind == Kind::fm && OscRole::usesOperatorEg (processorRef, index);
            // An FM operator reads as one (UI review 9, I9-6): its type says
            // FM / DX7 and its table is its WAVE. Ratio, Fixed Hz and OP ENV
            // are its own: a Wavetable's menus don't offer them.
            osc.table.setLabelText (kind == Kind::fm ? "WAVE" : "TABLE");
            OscRole::showOperatorChoices (&osc.tune.getComboBox(), &osc.ampEnv.getComboBox(), kind == Kind::fm,
                                          getMode (index) != OscMode::wavetable, readChoice (prefix + "_amp_env"));

            editButtons[(size_t) index]->setVisible (kind == Kind::wavetable);
            // LOAD loads what the mode plays: a wavetable (LOAD...), or a
            // sample or SF2 / SFZ multisample (LOAD, UI review 4, V30; review
            // 7, I7-24).
            loadButton (index).setVisible (kind == Kind::wavetable);
            pieces.sampleLoadButton.setVisible (kind == Kind::sample || kind == Kind::grain);
            pieces.sampleNameButton.setVisible (kind == Kind::sample || kind == Kind::grain);
            pieces.sampleSliceButton.setVisible (kind == Kind::sample);
            pieces.sampleExportButton.setVisible (kind == Kind::sample);
            pieces.sampleZonesButton.setVisible (kind == Kind::sample);
            pieces.sampleDetectButton.setVisible (kind == Kind::sample);
            bounceButtons[(size_t) index]->setVisible (kind != Kind::live && ! opEnv);
            pieces.opEnvButton.setVisible (false); // (the envelope picture opens the editor on a click)
            pieces.stringButton.setVisible (kind == Kind::physical);
            pieces.removeButton.setVisible (numShown() > 1);

            // The well: the table (slim line), the string, the operator's
            // envelope, the sample, the live meter.
            const auto electric = kind == Kind::physical && isElectric (index);
            auto& wave = waveDisplay (index);
            wave.setVisible (kind != Kind::physical || electric);
            if (wave.isCompact() != (kind != Kind::sample))
                wave.setCompact (kind != Kind::sample);
            wave.setSlim (kind == Kind::wavetable);
            pieces.stringView.setVisible (kind == Kind::physical && ! electric);
            pieces.opEnvGraph.setVisible (opEnv);
            pieces.opEnvGraph.setSource (prefix, oscColour (index));
            pieces.partials.setVisible (kind == Kind::physical && ! electric);
            pieces.status.setVisible (true);
            pieces.xfadeBar.setVisible (kind == Kind::sample);
            pieces.xfadeBar.setAlpha (readFloat (prefix + "_sample_loop") > 0.5f ? 1.0f : 0.45f);
            pieces.spreadView.setVisible (kind != Kind::live && kind != Kind::physical && (kind != Kind::fm || ! opEnv));

        }

        // The strip.
        applyStripStyle();
        for (auto* item : std::initializer_list<juce::Component*> { voiceSpread.get(), unisonRandom.get(), drift.get(), tuneLoose.get(), &looseBoth, noiseStrip.get(), noiseColourStrip.get(),
                                                                    &noiseType, subOscOn.get(), &subShape, subOscLevel.get(), &subOctave, &symOn, &symAmount,
                                                                    &symCount, &symDecay, &symManual, &sbOn, &sbModel, &sbSize, &sbTone, &sbMix,
                                                                    &stretch, &pedalRes, &mechKey, &mechDamper, &mechPedal })
            item->setVisible (! embedded);

        // The chevron opens the six notes while Manual tuning is on.
        tuneButton.setVisible (! embedded);
        tuneButton.setEnabled (readBool ("sym_manual"));
        tuneButton.setAlpha (readBool ("sym_manual") ? 1.0f : IlanaTheme::dimmedAlpha);
        if (! readBool ("sym_manual") || embedded)
            showTuning (false);

        updateTabItems();
        shownKey = layoutKey();
        getProperties().set ("caption", OscRole::describeLong (processorRef, selected)); // for the UI test
        resized();
        updateCardPictures();
        repaint();
        if (onModeChanged != nullptr)
            onModeChanged();
    }

    // M7.3: the Tine and Reed excites (7, 8) have their own controls.
    bool isElectric (int index) const
    {
        const auto excite = juce::roundToInt (readFloat (juce::String (OscillatorIds::prefixes[(size_t) index]) + "_excite"));
        return getMode (index) == 1 && (excite == 7 || excite == 8);
    }

    static void setGroupEnabled (std::initializer_list<juce::Component*> controls, bool enabled)
    {
        for (auto* control : controls)
        {
            control->setEnabled (enabled);
            control->setAlpha (enabled ? 1.0f : IlanaTheme::dimmedAlpha);
        }
    }

    void updateEnabled()
    {
        // The sub's controls follow its switch; noise has its own level.
        const auto subIsOn = readBool ("subosc_on");
        for (auto* control : { static_cast<juce::Component*> (&subShape), static_cast<juce::Component*> (&subOctave),
                               static_cast<juce::Component*> (subOscLevel.get()) })
            if (control != nullptr && control->getAlpha() != (subIsOn ? 1.0f : IlanaTheme::dimmedAlpha))
                control->setAlpha (subIsOn ? 1.0f : IlanaTheme::dimmedAlpha);
        // COLOUR stays a live knob at any noise level (V15-14).
        if (noiseColourStrip->getAlpha() != 1.0f)
            noiseColourStrip->setAlpha (1.0f);

        const auto boardOn = readBool ("sb_on");
        sbModel.setAlpha (boardOn ? 1.0f : IlanaTheme::dimmedAlpha);
        for (auto* control : { &sbMix, &sbTone, &sbSize })
            control->setAlpha (boardOn ? 1.0f : IlanaTheme::dimmedAlpha);

        // The strings' controls dim while the strings are off; manual notes
        // past STRINGS are not sounding.
        const auto stringsOn = readBool ("sym_on");
        for (auto* control : { &symAmount, &symDecay, &symCount })
            control->setAlpha (stringsOn ? 1.0f : IlanaTheme::dimmedAlpha);
        symManual.setAlpha (stringsOn ? 1.0f : IlanaTheme::dimmedAlpha);
        const auto stringCount = juce::roundToInt (readFloat ("sym_count"));
        for (int i = 0; i < (int) symNotes.size(); ++i)
            symNotes[(size_t) i]->setAlpha (stringsOn && i < stringCount ? 1.0f : IlanaTheme::dimmedAlpha);

        for (int index = 0; index < OscillatorIds::count; ++index)
        {
            const juce::String prefix (OscillatorIds::prefixes[(size_t) index]);
            const auto enabled = readBool (prefix + "_on");
            auto& physicalControls = *physical[(size_t) index];
            setGroupEnabled ({ &physicalControls.stiffness, &physicalControls.pickup, &physicalControls.click,
                               &physicalControls.excitePos, &physicalControls.hardness,
                               &physicalControls.pickPos, &physicalControls.slap,
                               &physicalControls.bowPressure, &physicalControls.bowSpeed,
                               &physicalControls.bridgeBuzz, &physicalControls.fretRattle,
                               &physicalControls.hammer, &physicalControls.couple,
                               &physicalControls.damper, &physicalControls.registerMap,
                               &physicalControls.epDistance, &physicalControls.epPosition,
                               &physicalControls.fbGain, &physicalControls.fbDistance }, enabled);

            auto& osc = *controls[(size_t) index];
            setGroupEnabled ({ &osc.mode, &osc.table, &osc.frameBlend, &osc.excite, &osc.level,
                               &osc.pan, &osc.semi, &osc.fine, &osc.unison, &osc.detune,
                               &osc.spread, &osc.stringDecay, &osc.stringDamp, &osc.stringSustain,
                               &osc.sampleTuned, &osc.sampleLoop, &osc.sampleReverse, &osc.sampleLoopOwn, &osc.sampleSnap, &osc.sampleLoopXfade, &osc.sampleRoot,
                               &osc.sampleStart, &osc.sampleEnd, &osc.sampleFadeIn, &osc.sampleFadeOut,
                               &osc.sampleSync, &osc.sampleBpm, &osc.sampleStretch, &osc.samplePlayback, &osc.samplePingPong, &osc.sampleStartRandom, &osc.sampleStartSafe, &osc.sampleQuality,
                               &osc.chord, &osc.warp, &osc.warpAmt, &osc.spectral, &osc.spectralAmt,
                               &osc.grainPosition, &osc.grainScan, &osc.grainFreeze, &osc.grainSize, &osc.grainDensity,
                               &osc.grainSpray, &osc.grainPitch, &osc.grainSpread, &osc.grainLive,
                               &osc.uniMode, &osc.scale, &osc.scaleRoot, &osc.uniBlend, &osc.uniFrame, &osc.uniWarp, &osc.ampEnv,
                               &osc.warp2, &osc.warp2Amt,
                               &osc.tune, &osc.ratio, &osc.fixedHz, &osc.egOut, &osc.trim, &osc.feedback,
                               &osc.feedbackType }, enabled);

            // An amount whose stage or envelope is Off does nothing: dim it
            // (it keeps its place and shows a dash).
            if (enabled)
            {
                osc.warp2Amt.setAlpha (readChoice (prefix + "_warp2") > 0 ? 1.0f : IlanaTheme::dimmedAlpha);

                // A string or exciter knob the exciter in use ignores dims too.
                if (readChoice (prefix + "_mode") == 1)
                {
                    const auto excite = readChoice (prefix + "_excite");
                    const auto unison = juce::roundToInt (readFloat (prefix + "_unison"));
                    for (const auto& [suffix, component] : physicalLookup[(size_t) index])
                        if (suffix != "_excite")
                            component->setAlpha (physicalKnobApplies (excite, suffix, unison) ? 1.0f : IlanaTheme::dimmedAlpha);
                }
            }

            for (auto* knob : { &osc.warpAmt, &osc.warp2Amt, &osc.spectralAmt, &osc.uniBlend, &osc.uniFrame, &osc.uniWarp, &osc.spread })
                knob->refreshValueText();

            // A switched-off oscillator dims in place, the same size.
            const auto alpha = enabled ? 1.0f : 0.55f;
            auto& pieces = *aux[(size_t) index];
            waveDisplay (index).setAlpha (alpha);
            waveDisplay (index).setEnabled (enabled);
            loadButton (index).setEnabled (! chooserOpen && enabled);
            loadButton (index).setAlpha (alpha);
            editButtons[(size_t) index]->setEnabled (enabled);
            editButtons[(size_t) index]->setAlpha (alpha);
            bounceButtons[(size_t) index]->setEnabled (enabled && bouncingOsc < 0);
            bounceButtons[(size_t) index]->setAlpha (alpha);
            pieces.stringView.setAlpha (alpha);
            pieces.opEnvGraph.setAlpha (alpha);
            pieces.spreadView.setAlpha (alpha);
            pieces.partials.setAlpha (alpha);
            pieces.status.setAlpha (alpha);
            pieces.opEnvButton.setAlpha (alpha);
            pieces.stringButton.setAlpha (alpha);
            pieces.sampleLoadButton.setAlpha (alpha);
            pieces.sampleNameButton.setAlpha (alpha);
            pieces.sampleSliceButton.setAlpha (alpha);
            pieces.sampleExportButton.setAlpha (alpha);
            pieces.sampleZonesButton.setAlpha (alpha);
            pieces.sampleDetectButton.setAlpha (alpha);
            // The FILTER choice, the OUT bar and the remove cross dim too, so an
            // off card reads as off at a glance (Impeccable audit, P1).
            pieces.routeBar.setAlpha (alpha);
            pieces.outBar.setAlpha (alpha);
            pieces.removeButton.setAlpha (alpha);
        }

        effectRules.apply();
    }

    // The VOICE group's quiet state: the mode, so the tab says what lives in it.
    juce::String voiceModeName() const
    {
        static const char* const names[] { "POLY", "MONO", "LEGATO" };
        return names[juce::jlimit (0, 2, readChoice ("voice_mode"))];
    }

    int readChoice (const juce::String& id) const
    {
        const auto* value = processorRef.apvts.getRawParameterValue (id);
        return value != nullptr ? juce::roundToInt (value->load()) : 0;
    }

public:
    // M7.4: EDIT. A user table is edited in place; a factory table is first
    // copied into a free patch table, which the oscillator then plays.
    // M8.6: the BOUNCE menu. The choices stay set for the next bounce.
    void showBounceMenu (int index)
    {
        if (bouncingOsc >= 0)
            return;
        juce::PopupMenu menu;
        menu.addSectionHeader ("Resample the patch into OSC " + juce::String (index + 1));
        menu.addItem (1, "As a sample (tuned, one note)");
        menu.addItem (2, "As a wavetable (cut into single cycles)");
        menu.addItem (5, "As a multisample (" + juce::String (bounceRequest.numCaptures()) + " renders: notes x velocity layers)");
        menu.addSeparator();
        menu.addItem (3, "Include the effects", true, bounceRequest.withFx);
        menu.addItem (4, "Mute the other oscillators", true, bounceRequest.muteOthers);
        juce::PopupMenu notes, lengths;
        for (int note : { 36, 48, 60, 72 })
            notes.addItem (100 + note, juce::MidiMessage::getMidiNoteName (note, true, true, 4), true, bounceRequest.note == note);
        for (double hold : { 0.5, 1.0, 2.0, 4.0, 8.0 })
            lengths.addItem (300 + (int) (hold * 2.0), juce::String (hold, hold < 1.0 ? 1 : 0) + " s held",
                             true, std::abs (bounceRequest.holdSeconds - hold) < 1.0e-3);
        menu.addSubMenu ("Note (one sample)", notes);
        menu.addSubMenu ("Length", lengths);
        juce::PopupMenu tails, ranges, spacings, layers;
        for (double tail : { 0.0, 0.5, 1.0, 2.0, 4.0 })
            tails.addItem (700 + (int) (tail * 2.0), tail == 0.0 ? juce::String ("No tail") : juce::String (tail, tail < 1.0 ? 1 : 0) + " s tail",
                           true, std::abs (bounceRequest.tailSeconds - tail) < 1.0e-3);
        menu.addSubMenu ("Tail after release", tails);
        menu.addSectionHeader ("Multisample");
        const std::pair<int, int> rangeChoices[] { { 36, 84 }, { 24, 96 }, { 48, 72 }, { 36, 72 }, { 48, 96 }, { 24, 60 } };
        for (int r = 0; r < 6; ++r)
            ranges.addItem (800 + r, juce::MidiMessage::getMidiNoteName (rangeChoices[r].first, true, true, 4) + " to "
                                         + juce::MidiMessage::getMidiNoteName (rangeChoices[r].second, true, true, 4),
                            true, bounceRequest.noteLow == rangeChoices[r].first && bounceRequest.noteHigh == rangeChoices[r].second);
        for (int step : { 1, 2, 3, 4, 6, 12 })
            spacings.addItem (900 + step, step == 12 ? juce::String ("Every octave") : "Every " + juce::String (step) + (step == 1 ? " semitone" : " semitones"),
                              true, bounceRequest.spacing == step);
        for (int count = 1; count <= 4; ++count)
            layers.addItem (1000 + count, count == 1 ? juce::String ("1 velocity layer") : juce::String (count) + " velocity layers",
                            true, bounceRequest.velocityLayers == count);
        menu.addSubMenu ("Note range", ranges);
        menu.addSubMenu ("Note spacing", spacings);
        menu.addSubMenu ("Velocity layers", layers);

        juce::Component::SafePointer<OscPage> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (bounceButtons[(size_t) index].get()),
                            [safe, index] (int result)
                            {
                                if (safe == nullptr || result == 0)
                                    return;
                                auto& request = safe->bounceRequest;
                                if (result == 1 || result == 2 || result == 5)
                                {
                                    request.targetOsc = index;
                                    request.toTable = result == 2;
                                    request.multi = result == 5;
                                    if (safe->processorRef.startBounce (request))
                                    {
                                        safe->bouncingOsc = index;
                                        safe->updateBounce();
                                    }
                                    return;
                                }
                                if (result == 3) request.withFx = ! request.withFx;
                                if (result == 4) request.muteOthers = ! request.muteOthers;
                                if (result >= 100 && result < 300) request.note = result - 100;
                                if (result >= 300 && result < 700) request.holdSeconds = (result - 300) / 2.0;
                                if (result >= 700 && result < 800) request.tailSeconds = (result - 700) / 2.0;
                                if (result >= 800 && result < 900)
                                {
                                    const std::pair<int, int> choices[] { { 36, 84 }, { 24, 96 }, { 48, 72 }, { 36, 72 }, { 48, 96 }, { 24, 60 } };
                                    request.noteLow = choices[result - 800].first;
                                    request.noteHigh = choices[result - 800].second;
                                }
                                if (result >= 900 && result < 1000) request.spacing = result - 900;
                                if (result >= 1000 && result < 1100) request.velocityLayers = result - 1000;
                                safe->showBounceMenu (index); // keep choosing
                            });
    }

    // Step 44: EXPORT's choices, then where to save.
    void showExportMenu (int index)
    {
        const auto* sample = processorRef.getSampleForOsc (index);
        juce::PopupMenu menu;
        if (sample == nullptr || sample->getNumSamples() < 2)
        {
            menu.addItem (1, "Load a sample first", false);
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&aux[(size_t) index]->sampleExportButton));
            return;
        }
        menu.addItem (1, "Export Played Range as WAV...");
        menu.addItem (2, sample->zones.empty() ? juce::String ("Export Whole Sample as WAV...")
                                               : juce::String ("Export Whole Region as WAV (the one under middle C)..."));
        const auto name = sample->name.isNotEmpty() ? sample->name : juce::String ("Sample");
        juce::Component::SafePointer<OscPage> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&aux[(size_t) index]->sampleExportButton),
                            [safe, index, name] (int result)
                            {
                                if (safe == nullptr || result == 0)
                                    return;
                                const auto range = result == 1;
                                const auto suggested = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                                           .getChildFile (juce::File::createLegalFileName (name + (range ? " (range)" : "")) + ".wav");
                                safe->exportChooser = std::make_unique<juce::FileChooser> ("Export sample as WAV", suggested, "*.wav");
                                safe->exportChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                                                      | juce::FileBrowserComponent::warnAboutOverwriting,
                                                                  [safe, index, range] (const juce::FileChooser& chooser)
                                                                  {
                                                                      auto target = chooser.getResult();
                                                                      if (safe == nullptr || target == juce::File())
                                                                          return;
                                                                      target = target.withFileExtension ("wav");
                                                                      juce::String error;
                                                                      if (! safe->processorRef.exportSampleWav (index, target, range, error))
                                                                          juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                                                                  "Export sample", error);
                                                                  });
                            });
    }

    void updateBounce()
    {
        const auto state = processorRef.getBounceState();
        auto& button = *bounceButtons[(size_t) juce::jlimit (0, OscillatorIds::count - 1, bouncingOsc)];
        if (state == IlanaSynthAudioProcessor::BounceState::Rendering)
        {
            button.setButtonText ("RESAMPLING " + juce::String (juce::roundToInt (processorRef.getBounceProgress() * 100.0f)) + "%");
            if (bounceButtonWide != bouncingOsc)
            {
                bounceButtonWide = bouncingOsc;
                resized();
            }
            for (auto& other : bounceButtons)
                other->setEnabled (false);
            return;
        }
        button.setButtonText ("RESAMPLE");
        for (auto& other : bounceButtons)
            other->setEnabled (true);
        if (state == IlanaSynthAudioProcessor::BounceState::Done)
            selected = bouncingOsc; // the bounced oscillator stays shown
        bouncingOsc = -1;
        bounceButtonWide = -1;
        updateModeVisibility();
        const auto message = processorRef.getBounceMessage();
        if (state == IlanaSynthAudioProcessor::BounceState::Failed)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Resample", message);
        else
            button.setTooltip (message);
        updateModeVisibility();
        updateEnabled();
    }

    // The header's ‹ ›: the next or previous table in the oscillator's list, as one undo step.
    void stepTable (int index, int step)
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (processorRef.apvts.getParameter (juce::String (OscillatorIds::prefixes[(size_t) index]) + "_table")))
        {
            // Stepping skips the empty User slots (there are hundreds); the built-in tables follow them.
            const auto factoryCount = TableFactory::getNumFactoryTables();
            const auto builtInBase = factoryCount + IlanaSynthAudioProcessor::numUserSlots;
            const auto usable = [&] (int i) { return i < factoryCount || i >= builtInBase || processorRef.isUserSlotEdited (i - factoryCount); };
            auto next = choice->getIndex();
            do
                next += step;
            while (next >= 0 && next < choice->choices.size() && ! usable (next));
            next = juce::jlimit (0, choice->choices.size() - 1, next);
            if (! usable (next))
                next = choice->getIndex();
            if (next >= builtInBase)
                BuiltInTables::build (next - builtInBase); // ready before the first note
            if (next != choice->getIndex())
                processorRef.performEdit ("OSC " + juce::String (index + 1) + " table", [choice, next]
                {
                    choice->beginChangeGesture();
                    choice->setValueNotifyingHost (choice->convertTo0to1 ((float) next));
                    choice->endChangeGesture();
                });
        }
    }

    void openTableEditor (int index)
    {
        const auto id = juce::String (OscillatorIds::prefixes[(size_t) index]) + "_table";
        const auto factoryCount = TableFactory::getNumFactoryTables();
        const auto choice = readTableChoiceIndex (id);
        const auto builtInBase = factoryCount + IlanaSynthAudioProcessor::numUserSlots;
        auto slot = choice < builtInBase ? choice - factoryCount : -1;

        if (slot < 0)
        {
            slot = processorRef.findFreeUserSlot();
            if (slot < 0)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Wavetable editor",
                                                        "All " + juce::String (IlanaSynthAudioProcessor::numUserSlots) + " patch tables are in use. Pick a User table to edit it.");
                return;
            }
            auto doc = WavetableDoc::fromFactory (0);
            if (choice < factoryCount)
                doc = WavetableDoc::fromFactory (choice);
            else
                BuiltInTables::load (choice - builtInBase, doc);
            doc.name << " edit";
            // The copy and the switch to it are one undo step.
            processorRef.beginEdit ("Edit a copy of the table");
            processorRef.setUserTable (slot, doc);
            if (auto* parameter = processorRef.apvts.getParameter (id))
            {
                parameter->beginChangeGesture();
                parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) (factoryCount + slot)));
                parameter->endChangeGesture();
            }
            processorRef.endEdit();
        }

        if (auto* editor = findParentComponentOfClass<IlanaSynthAudioProcessorEditor>())
            editor->openWavetableEditor (slot, oscColour (index));
    }

private:
    int readTableChoiceIndex (const juce::String& tableId) const
    {
        if (auto* param = dynamic_cast<juce::AudioParameterChoice*> (processorRef.apvts.getParameter (tableId)))
            return param->getIndex();

        return 0;
    }

    IlanaSynthAudioProcessor& processorRef;
    EffectRules effectRules { processorRef }; // after processorRef, which it reads
    std::array<std::unique_ptr<WaveDisplay>, OscillatorIds::count> waveDisplays;
    std::array<FrameScrubber, OscillatorIds::count> scrubbers;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> scrubberAttachments;
    int lastRevealVersion = -1;
    int selected = 0;
    bool embedded = false; // on PLAY: the oscillator cards without the voice strip
    float laidOutScale = 0.0f;

    std::array<std::unique_ptr<juce::TextButton>, OscillatorIds::count> loadButtons, editButtons, bounceButtons;
    std::array<std::unique_ptr<CardAux>, OscillatorIds::count> aux;
    IlanaSynthAudioProcessor::BounceRequest bounceRequest;
    int bouncingOsc = -1, bounceButtonWide = -1;
    DashedAddButton addButton { "+  ADD OSC", "+  ADD OSC" };
    DashedAddButton addFmButton { "+  ADD FM / DX7", "+  ADD FM / DX7" };
    static constexpr int addFmWidth = 150;
    static constexpr int tileMinHeight = 56, tileButtonWidth = 104, tileButtonHeight = 26;
    OscDropTile dropTile;
    juce::TextButton tuneButton { juce::String::fromUTF8 ("\xe2\x80\xba") };
    OscPicker oscTabs;
    std::unique_ptr<juce::FileChooser> tableChooser, exportChooser;
    std::array<std::unique_ptr<PhysicalControls>, OscillatorIds::count> physical;
    // Each oscillator's controls by parameter suffix, for the shared
    // physical list.
    std::array<std::map<juce::String, juce::Component*>, OscillatorIds::count> physicalLookup;
    bool chooserOpen = false;

    // Voice-wide settings that shape how the oscillators stack and drift.
    std::unique_ptr<KnobControl> voiceSpread, unisonRandom, drift, tuneLoose;
    ToggleControl symOn, symManual, looseBoth;
    KnobControl symAmount, symDecay, symCount;

    // Acoustic keys (M4): soundboard, stretch tuning, pedal resonance and
    // the mechanism's noises, shared by every voice.
    ToggleControl sbOn;
    ComboControl sbModel;
    KnobControl sbMix, sbTone, sbSize, stretch, pedalRes, mechKey, mechDamper, mechPedal;
    std::array<std::unique_ptr<KnobControl>, 6> symNotes;

    // The dedicated sub and the noise.
    std::unique_ptr<ToggleControl> subOscOn;
    std::unique_ptr<KnobControl> subOscLevel, noiseStrip, noiseColourStrip;

    std::array<std::unique_ptr<OscControls>, OscillatorIds::count> controls;
    ComboControl subShape, subOctave, noiseType;
    TuningPopover tuningPopover;
    IlanaAnim::PopGrow tuningGrow { tuningPopover }; // (NOTES grows from its button)
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> laneLabels;
};

// The OSC page scrolls only when the card's rows cannot fit at their
// smallest.
class OscPageViewport : public juce::Viewport, private juce::Timer
{
public:
    explicit OscPageViewport (IlanaSynthAudioProcessor& processor)
    {
        setScrollBarsShown (true, false);
        auto* page = new OscPage (processor);
        page->onModeChanged = [this] { resized(); };
        setViewedComponent (page, true);
    }

    OscPage* getPage() const { return dynamic_cast<OscPage*> (getViewedComponent()); }

    // The page is laid out for 1060 design units of width, and its cards pack
    // a lot in; where the window is wider than the packing needs, the whole
    // page is drawn this much larger (the wave column takes the rest), so the
    // labels and dials read like the other tabs'. Where it would have to
    // scroll, it stays at 1.
    static constexpr float maximumContentScale = 1.3f, minimumVirtualWidth = 930.0f;
    // PLAY's copy keeps its cards at their drawn size (the bands under them are not scaled).
    void setMaximumScale (float scale) { maxScale = scale; }
    // PLAY draws its own thin scroll bar over the cards' side margin, so the columns stand where they do without one.
    void setOverlayScroll (bool overlay)
    {
        overlayScroll = overlay;
        setScrollBarsShown (! overlay, false);
    }

    void resized() override
    {
        juce::Viewport::resized();
        if (auto* page = getPage())
        {
            const auto width = (float) getWidth(), height = (float) getHeight();
            const auto widest = juce::jlimit (1.0f, maxScale, width / minimumVirtualWidth);

            auto chosen = 1.0f;
            for (auto scale = widest; scale > 1.005f; scale -= 0.02f)
            {
                page->setSize (juce::jmax (1, juce::roundToInt (width / scale)), juce::jmax (1, juce::roundToInt (height / scale)));
                if (page->getMinimumHeight() <= juce::roundToInt (height / scale))
                {
                    chosen = scale;
                    break;
                }
            }

            chosenScale = chosen;
            stopTimer();
            setOvershoot (0.0f);

            if (chosen > 1.0f)
            {
                page->setSize (juce::roundToInt (width / chosen), juce::roundToInt (height / chosen));
                return;
            }

            const auto needed = page->getMinimumHeight();
            const auto scrolls = needed > getHeight();
            page->setSize (juce::jmax (1, getWidth() - (scrolls && ! overlayScroll ? getScrollBarThickness() : 0)),
                           juce::jmax (getHeight(), needed));
        }
    }

    std::function<void()> onViewMoved;

    // The wheel glides: each notch adds to a target the view eases toward, and a hard scroll past either end
    // runs a little over and springs back (ilana 2026-10-22: "smooooth scroll ... bouncing back a bit").
    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        auto* page = getPage();
        const auto maxY = (float) maxScroll();
        if (page == nullptr || maxY <= 0.0f || wheel.deltaY == 0.0f)
        {
            juce::Viewport::mouseWheelMove (event, wheel);
            return;
        }

        if (! isTimerRunning())
            current = target = (float) getViewPositionY();

        target -= wheel.deltaY * wheelPixels;
        // (Past an end the target goes on at a third of the pace, and never more than overshootMax over.)
        if (target < 0.0f)
            target = juce::jmax (-overshootMax, target * 0.35f);
        else if (target > maxY)
            target = juce::jmin (maxY + overshootMax, maxY + (target - maxY) * 0.35f);
        lastWheel = juce::Time::getMillisecondCounter();
        startTimerHz (60);
    }

    // Moves the view straight to a place (the scroll bar, a card brought into view), stopping any glide.
    void jumpTo (int y)
    {
        stopTimer();
        setOvershoot (0.0f);
        current = target = (float) juce::jlimit (0, maxScroll(), y);
        setViewPosition (0, (int) current);
    }

    void visibleAreaChanged (const juce::Rectangle<int>& area) override
    {
        juce::Viewport::visibleAreaChanged (area);
        // (Moved by something else, its own scroll bar or a card brought into view: the glide starts from there.)
        if (! gliding && ! isTimerRunning())
            current = target = (float) area.getY();
        if (onViewMoved)
            onViewMoved();
    }

private:
    // (1: the cards keep one size on every tab and with any number of oscillators, ilana 2026-10-21; the page
    // scrolls instead of shrinking or growing.)
    float maxScale = 1.0f, chosenScale = 1.0f;
    bool overlayScroll = false;
    juce::uint32 lastWheel = 0;

    static constexpr float wheelPixels = 150.0f, overshootMax = 44.0f;
    float current = 0.0f, target = 0.0f, overshoot = 0.0f;
    bool gliding = false;

    int maxScroll() const
    {
        const auto* page = getPage();
        return page == nullptr ? 0 : juce::jmax (0, juce::roundToInt ((float) page->getHeight() * chosenScale) - getHeight());
    }

    // The part of the glide past an end: the page is drawn that far over, the view itself stays at its end.
    void setOvershoot (float pixels)
    {
        overshoot = pixels;
        if (auto* page = getPage())
            page->setTransform (juce::AffineTransform::scale (chosenScale).translated (0.0f, -overshoot));
    }

    void timerCallback() override
    {
        const auto maxY = (float) maxScroll();
        // (Once the wheel rests, a target past an end springs back to it.)
        if (juce::Time::getMillisecondCounter() - lastWheel > 90)
            target = juce::jlimit (0.0f, maxY, target);

        current += (target - current) * 0.22f;
        if (std::abs (target - current) < 0.5f)
        {
            current = target;
            stopTimer();
        }

        const auto inside = juce::jlimit (0.0f, maxY, current);
        const juce::ScopedValueSetter<bool> moving (gliding, true);
        setOvershoot (current - inside);
        setViewPosition (0, juce::roundToInt (inside));
    }
};
} // namespace
