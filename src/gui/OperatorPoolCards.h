#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "../PluginProcessor.h"
#include "AnimationUtils.h"
#include "CardTabs.h"
#include "FmOperatorInfo.h"
#include "IlanaLookAndFeel.h"
#include "LfoShapeMenu.h"
#include "OperatorEnvDisplay.h"
#include "OscPicker.h"
#include "ParamControls.h"

// The Operator Env in the MOD page's pools (UI review 7, I7-7, V7-5, S7-6,
// S7-27): OP ENV and OP PITCH are cards in the envelope pool and OP LFO one
// in the LFO pool, the same size as the others, edited in place below them.
// OP ENV's editor is the one the FM page's operator card uses too (UI review
// 8, I8-6): one component, one knob order. While no oscillator plays the
// Operator Env they are greyed and say why. OP PITCH and OP LFO drag onto a
// knob like any pool source (the OP PITCH and OP LFO sources); OP ENV
// shapes its operators only, so it doesn't drag.
namespace OperatorPool
{
// The FM page's colour, which the Operator Env wears everywhere.
inline juce::Colour colour() { return juce::Colour (0xffe3a56f); }

// Why the Operator Env's cards are greyed.
inline const char* unusedReason() { return "no oscillator plays the Operator Env (an oscillator's ENVELOPE picks it)"; }

// The share of the row the graph takes (as the LFO and envelope panels).
inline constexpr int graphPercent = 47;

// The DX7 LFO's waves (opeg_lfo_wave, DX7 order) at a phase, -1..1.
inline float lfoWaveValue (int wave, double phase)
{
    phase -= std::floor (phase);
    const auto p = (float) phase;
    switch (wave)
    {
        case 0: return 1.0f - 2.0f * std::abs (2.0f * p - 1.0f);
        case 1: return 1.0f - 2.0f * p;
        case 2: return 2.0f * p - 1.0f;
        case 3: return p < 0.5f ? 1.0f : -1.0f;
        case 4: return std::sin (p * juce::MathConstants<float>::twoPi);
        default:
        {
            static const float held[] { 0.6f, -0.3f, 0.9f, -0.8f, 0.1f, -0.5f, 0.4f, -0.1f };
            return held[juce::jlimit (0, 7, (int) (p * 8.0f))];
        }
    }
}

// The oscillators that play the Operator Env now.
inline std::vector<int> operatorsOnEnv (const IlanaSynthAudioProcessor& p)
{
    std::vector<int> list;
    for (int osc = 0; osc < OscillatorIds::count; ++osc)
        if (FmOperatorInfo::isPlaying (p, osc) && FmOperatorInfo::usesOperatorEnv (p, osc))
            list.push_back (osc);
    return list;
}

// An envelope's shape for a card: samples 0..1 over its run (an operator's
// level, or the pitch with a little room around the note), rebuilt only when
// its settings change.
struct ShapeCache
{
    OperatorEnv::Settings settings {};
    bool valid = false;
    std::vector<float> values;
    bool moves = false; // the pitch envelope moves the pitch
};

inline const ShapeCache& envelopeShape (const IlanaSynthAudioProcessor& p, const juce::String& prefix, ShapeCache& cache)
{
    const auto settings = OperatorEnv::read (p, prefix);
    if (cache.valid && settings == cache.settings)
        return cache;

    cache.settings = settings;
    cache.valid = true;
    cache.values.clear();
    const auto run = OperatorEnv::run (settings);
    auto low = 1.0e9, high = -1.0e9;
    for (const auto value : run.values)
    {
        low = std::min (low, value);
        high = std::max (high, value);
    }
    cache.moves = high - low > 0.11;
    if (prefix.isEmpty())
        low = std::min (low, -0.05), high = std::max (high, 0.05);
    const auto step = std::max<size_t> (1, run.values.size() / 48);
    for (size_t i = 0; i < run.values.size(); i += step)
        cache.values.push_back ((float) ((run.values[i] - low) / std::max (1.0e-6, high - low)));
    return cache;
}

// A card's picture of an envelope shape, as the pool's other cards draw theirs.
inline void paintEnvelopeShape (juce::Graphics& g, juce::Rectangle<float> plot, const std::vector<float>& values, juce::Colour colour,
                                bool active)
{
    if (values.size() < 2)
        return;
    juce::Path path;
    for (size_t i = 0; i < values.size(); ++i)
    {
        const juce::Point<float> point (plot.getX() + plot.getWidth() * (float) i / (float) (values.size() - 1),
                                        plot.getBottom() - plot.getHeight() * values[i]);
        if (i == 0)
            path.startNewSubPath (point);
        else
            path.lineTo (point);
    }
    auto fill = path;
    fill.lineTo (plot.getRight(), plot.getBottom());
    fill.lineTo (plot.getX(), plot.getBottom());
    fill.closeSubPath();
    g.setColour (colour.withAlpha (active ? 0.16f : 0.08f));
    g.fillPath (fill);
    g.setColour (colour.withAlpha (active ? 0.95f : 0.6f));
    g.strokePath (path, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// One of the Operator Env's knobs. A rate reads as its stage's time (the rest
// of the envelope as set; a flat stage, the time a full sweep at that rate
// takes: I8-3), and "250 ms" or "1.5 s" typed sets that time; LOW and HIGH
// DEPTH read as the change in dB an octave from SCALE KEY along their curve
// (I7-12); the 0-99 is in the tooltip. Every Operator Env knob is made here,
// so the editor reads the same wherever it shows.
inline std::unique_ptr<KnobControl> makeStageKnob (IlanaSynthAudioProcessor& p, const juce::String& prefix, const juce::String& suffix,
                                                   const juce::String& label, juce::Colour colour)
{
    auto knob = std::make_unique<KnobControl> (p.apvts, prefix + suffix, label, colour, false);
    auto& slider = knob->getSlider();
    const auto rateStage = suffix.endsWith ("_r1") ? 0 : suffix.endsWith ("_r2") ? 1 : suffix.endsWith ("_r3") ? 2 : suffix.endsWith ("_r4") ? 3 : -1;
    if (rateStage >= 0)
    {
        // The pitch envelope's prefix is empty (its ids are whole).
        const auto envPrefix = prefix.startsWith ("osc") || prefix == "sub" ? prefix : juce::String();
        // Clockwise is longer, as on every envelope (review 7, I7-3).
        FmOperatorInfo::reverseRateKnob (slider);
        const auto original = slider.valueFromTextFunction;
        slider.textFromValueFunction = [&p, envPrefix, rateStage] (double value)
        {
            auto settings = OperatorEnv::read (p, envPrefix);
            settings.rates[(size_t) rateStage] = juce::jlimit (0, 99, juce::roundToInt (value));
            const auto time = OperatorEnv::stageTime (settings, rateStage);
            return OperatorEnv::formatSeconds (time.seconds, time.endless);
        };
        slider.valueFromTextFunction = [&p, envPrefix, rateStage, original] (const juce::String& text)
        {
            const auto trimmed = text.trim().toLowerCase();
            if (trimmed.endsWith ("ms") || trimmed.endsWith ("s"))
            {
                const auto seconds = trimmed.getDoubleValue() * (trimmed.endsWith ("ms") ? 0.001 : 1.0);
                return (double) OperatorEnv::rateForSeconds (OperatorEnv::read (p, envPrefix), rateStage, seconds);
            }
            return original != nullptr ? original (text) : trimmed.getDoubleValue();
        };
        slider.updateText();
    }
    else if (suffix == "_eg_ldepth" || suffix == "_eg_rdepth")
    {
        const auto high = suffix == "_eg_rdepth";
        slider.textFromValueFunction = [&p, prefix, high] (double value)
        {
            const auto depth = juce::jlimit (0, 99, juce::roundToInt (value));
            if (depth == 0)
                return juce::String ("0 dB");
            const auto breakPoint = juce::roundToInt (FmOperatorInfo::read (p, prefix + "_eg_break"));
            const auto curve = juce::roundToInt (FmOperatorInfo::read (p, prefix + (high ? "_eg_rcurve" : "_eg_lcurve")));
            const auto note = breakPoint + 21 + (high ? 12 : -12);
            const auto units = Dx7::scaleLevel (note, breakPoint, high ? 0 : depth, high ? depth : 0, curve, curve);
            const auto db = units * 6.0206 / 8.0;
            return (db > 0.05 ? "+" : "") + describeFixed ((float) db, 1) + " dB";
        };
        slider.updateText();
    }
    return knob;
}
} // namespace OperatorPool

// Rate knobs' times follow the levels around them (a flat stage's time is
// greyed and its tooltip says why), depths follow their key and curve, and
// every knob's tooltip carries its DX7 value: kept current on the editor's
// timer.
class OperatorKnobTexts
{
public:
    enum class Kind { plain, rate, depth };

    void add (KnobControl& knob, const juce::String& envPrefix, Kind kind, int stage = 0)
    {
        entries.push_back ({ &knob, envPrefix, knob.getTooltip(), -1, kind, stage, {}, {}, false,
                             knob.getSlider().findColour (juce::Slider::textBoxTextColourId) });
    }

    void refresh (const IlanaSynthAudioProcessor& p)
    {
        for (auto& entry : entries)
        {
            if (! IlanaAnim::showing (*entry.knob))
                continue;
            const auto value = juce::roundToInt (entry.knob->getSlider().getValue());
            auto tooltipChanged = value != entry.lastValue;
            entry.lastValue = value;

            if (entry.kind == Kind::rate)
            {
                const auto settings = OperatorEnv::read (p, entry.prefix);
                if (settings != entry.settings)
                {
                    entry.settings = settings;
                    const auto flat = OperatorEnv::stageTime (settings, entry.stage).flat;
                    if (flat != entry.flat)
                    {
                        entry.flat = flat;
                        tooltipChanged = true;
                        entry.knob->getSlider().setColour (juce::Slider::textBoxTextColourId,
                                                           flat ? IlanaTheme::Ui::text3 : entry.valueColour);
                    }
                    entry.knob->getSlider().updateText();
                }
            }
            else if (entry.kind == Kind::depth)
            {
                const std::array<int, 3> inputs { juce::roundToInt (FmOperatorInfo::read (p, entry.prefix + "_eg_break")),
                                                  juce::roundToInt (FmOperatorInfo::read (p, entry.prefix + "_eg_lcurve")),
                                                  juce::roundToInt (FmOperatorInfo::read (p, entry.prefix + "_eg_rcurve")) };
                if (inputs != entry.depthInputs)
                {
                    entry.depthInputs = inputs;
                    entry.knob->getSlider().updateText();
                }
            }

            if (tooltipChanged)
            {
                auto tooltip = entry.baseTooltip + "\nDX7 value: " + juce::String (value);
                if (entry.flat)
                    tooltip << "\nFlat: this stage's levels are the same at both ends, so it takes no time now; the time shown "
                               "(greyed) is a full sweep at this rate.";
                entry.knob->setTooltip (tooltip);
                entry.knob->getSlider().setTooltip (tooltip);
            }
        }
    }

private:
    struct Entry
    {
        KnobControl* knob;
        juce::String prefix, baseTooltip;
        int lastValue;
        Kind kind;
        int stage;
        OperatorEnv::Settings settings;
        std::array<int, 3> depthInputs;
        bool flat;
        juce::Colour valueColour;
    };
    std::vector<Entry> entries;
};

// The panel card beside a pool editor's graph: its title, a line about it
// and, at the right of the header, a small "FM ›" link.
inline void paintOperatorPanel (juce::Graphics& g, juce::Rectangle<int> panel, const juce::String& title, const juce::String& subtitle,
                                int reserveRight)
{
    IlanaTheme::paintCard (g, panel.toFloat(), 7.0f, OperatorPool::colour().withAlpha (0.35f));
    IlanaTheme::paintCardHeader (g, panel.reduced (12, 0).withHeight (26), title, subtitle, OperatorPool::colour(), reserveRight);
}

// (The jump links' one style since UI review 9, I9-14: "EDIT ON FM ›".)
inline void styleFmLink (juce::TextButton& button, const juce::String& what = "ON FM")
{
    styleJumpLink (button, what);
}

// TRANSPOSE (UI review 8, S8-19): a knob in semitones, as SCALE SHIFT beside
// it, not a one-off stepper. It holds no value of its own: it shows the first
// oscillator's SEMI (of those not at a fixed frequency) and turning it moves
// every such oscillator's SEMI by as much, one undo step per turn, as a DX7
// voice's TRANSPOSE did on import (I7-15).
class TransposeKnob : public juce::Component,
                      private IlanaAnim::FrameTimer
{
public:
    TransposeKnob (IlanaSynthAudioProcessor& p, juce::Colour colour) : processorRef (p)
    {
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 14);
        slider.setRange (-24.0, 24.0, 1.0);
        slider.setColour (juce::Slider::rotarySliderFillColourId, colour);
        slider.textFromValueFunction = [] (double value)
        {
            const auto semitones = juce::roundToInt (value);
            return (semitones > 0 ? "+" : "") + juce::String (semitones) + " st";
        };
        slider.valueFromTextFunction = [] (const juce::String& text)
        {
            return text.replace (juce::String::fromUTF8 ("\xe2\x88\x92"), "-").retainCharacters ("+-0123456789").getDoubleValue();
        };
        slider.onDragStart = [this]
        {
            processorRef.beginEdit ("Transpose");
            dragging = true;
        };
        slider.onDragEnd = [this]
        {
            dragging = false;
            processorRef.endEdit();
        };
        slider.onValueChange = [this]
        {
            if (! updating)
                shiftTo (juce::roundToInt (slider.getValue()));
        };
        const auto tip = juce::String ("Transpose the whole voice: every oscillator's SEMI moves by as much (those at a fixed "
                                       "frequency stay). It shows the first oscillator's SEMI.");
        slider.setTooltip (tip);
        addAndMakeVisible (slider);
        label.setText ("TRANSPOSE", juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (IlanaTheme::font (IlanaTheme::TextSize::body));
        label.setColour (juce::Label::textColourId, IlanaTheme::Ui::text2);
        label.setTooltip (tip);
        addAndMakeVisible (label);
        refresh();
        startTimerHz (8);
    }

    juce::Slider& getSlider() { return slider; }

    // The oscillator whose SEMI it shows, or -1 (none can be transposed).
    int referenceOscillator() const
    {
        for (int osc = 0; osc < OscillatorIds::count; ++osc)
            if (processorRef.isOscillatorShown (osc) && juce::roundToInt (FmOperatorInfo::read (processorRef, FmOperatorInfo::prefixOf (osc) + "_tune")) != OscTuning::Fixed)
                return osc;
        return -1;
    }

    void resized() override
    {
        auto area = getLocalBounds();
        label.setBounds (area.removeFromTop (13));
        slider.setBounds (area);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jmin (64, getWidth()), 16);
    }

private:
    void timerCallback() override
    {
        if (IlanaAnim::showing (*this) && ! dragging)
            refresh();
    }

    void refresh()
    {
        const auto osc = referenceOscillator();
        setEnabled (osc >= 0);
        const auto now = osc >= 0 ? juce::roundToInt (FmOperatorInfo::read (processorRef, FmOperatorInfo::prefixOf (osc) + "_semi")) : 0;
        if (juce::roundToInt (slider.getValue()) != now)
        {
            const juce::ScopedValueSetter<bool> guard (updating, true);
            slider.setValue (now, juce::dontSendNotification);
        }
    }

    void shiftTo (int target)
    {
        const auto osc = referenceOscillator();
        if (osc < 0)
            return;
        const auto delta = target - juce::roundToInt (FmOperatorInfo::read (processorRef, FmOperatorInfo::prefixOf (osc) + "_semi"));
        if (delta == 0)
            return;
        const auto apply = [this, delta]
        {
            for (int other = 0; other < OscillatorIds::count; ++other)
            {
                const auto prefix = FmOperatorInfo::prefixOf (other);
                if (! processorRef.isOscillatorShown (other) || juce::roundToInt (FmOperatorInfo::read (processorRef, prefix + "_tune")) == OscTuning::Fixed)
                    continue;
                if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (processorRef.apvts.getParameter (prefix + "_semi")))
                {
                    const auto now = parameter->convertFrom0to1 (parameter->getValue());
                    parameter->beginChangeGesture();
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (now + (float) delta));
                    parameter->endChangeGesture();
                }
            }
        };
        if (dragging)
            apply();
        else
            processorRef.performEdit (delta > 0 ? "Transpose up" : "Transpose down", apply);
    }

    IlanaSynthAudioProcessor& processorRef;
    KnobSlider slider;
    juce::Label label;
    bool dragging = false, updating = false;
};

// The Operator Env's one editor (UI review 8, I8-6, S8-4, V8-5): the graph
// and its knobs in one order that follows the graph left to right (each
// handle's time, then its level), with the key and velocity scaling on a
// second tab; or, for OP PITCH, the pitch envelope with the voice's
// TRANSPOSE and SCALE SHIFT on its second tab. MOD's pools show it with its
// own panel (the oscillators on the Operator Env picked in its header, "FM ›"
// beside them); the FM page's operator card uses the same component, its
// graph beside the card's tuning and the tabs and knobs along the bottom.
class OperatorEnvEditor : public juce::Component,
                          private IlanaAnim::FrameTimer
{
public:
    enum class Place { pool, fmCard };

    // The knobs' order on both tabs (an index into getControl).
    static constexpr int numStageControls = 8, numControls = 16;
    static const char* suffixAt (int index)
    {
        static const char* const suffixes[] { "_eg_r1", "_eg_l1", "_eg_r2", "_eg_l2", "_eg_r3", "_eg_l3", "_eg_r4", "_eg_l4",
                                              "_eg_rate_key", "_eg_break", "_eg_ldepth", "_eg_lcurve", "_eg_rdepth", "_eg_rcurve", "_eg_vel", "_eg_ams" };
        return suffixes[juce::jlimit (0, numControls - 1, index)];
    }
    static const char* labelAt (int index)
    {
        static const char* const labels[] { "ATTACK", "PEAK", "DECAY 1", "MID", "DECAY 2", "SUSTAIN", "RELEASE", "END",
                                            "KEY RATE", "SCALE KEY", "LOW DEPTH", "LOW CURVE", "HIGH DEPTH", "HIGH CURVE", "VEL", "AMP MOD" };
        return labels[juce::jlimit (0, numControls - 1, index)];
    }

    explicit OperatorEnvEditor (IlanaSynthAudioProcessor& p, Place placeIn = Place::pool)
        : processorRef (p), place (placeIn), graph (p), transpose (p, OperatorPool::colour()), effectRules (p)
    {
        addAndMakeVisible (graph);
        addAndMakeVisible (tabs);
        tabs.onSelect = [this] (int tab)
        {
            (pitch ? pitchTab : operatorTab) = tab;
            updateVisibility();
        };

        if (place == Place::pool)
        {
            styleFmLink (fmLink);
            fmLink.setTooltip ("Open this on the FM page, beside the algorithm.");
            fmLink.onClick = [this] { FmOperatorInfo::openOperator (selectedOperator); };
            addAndMakeVisible (fmLink);
            picker.onPick = [this] (int osc) { selectOperator (osc); };
            addChildComponent (picker);
            useButton.getProperties().set ("pill", true);
            useButton.setColour (juce::TextButton::buttonOnColourId, OperatorPool::colour());
            useButton.onClick = [this] { useOnFirstOscillator(); };
            addChildComponent (useButton);
        }
        else
        {
            // The card's own controls sit in the space left of the graph.
            setInterceptsMouseClicks (false, true);
        }

        const auto anyOperator = [this] { return FmOperatorInfo::anyOperatorEnv (processorRef); };

        for (int osc = 0; osc < OscillatorIds::count; ++osc)
        {
            const auto prefix = FmOperatorInfo::prefixOf (osc);
            for (int i = 0; i < numControls; ++i)
            {
                const juce::String suffix (suffixAt (i));
                std::unique_ptr<juce::Component> control;
                if (suffix.endsWith ("curve"))
                    control = std::make_unique<ComboControl> (p.apvts, prefix + suffix, labelAt (i));
                else
                {
                    auto knob = OperatorPool::makeStageKnob (p, prefix, suffix, labelAt (i), IlanaTheme::oscColour (osc));
                    texts.add (*knob, prefix, suffix.contains ("_eg_r") && ! suffix.contains ("rate") && ! suffix.contains ("rdepth")
                                                  ? OperatorKnobTexts::Kind::rate
                                                  : suffix.endsWith ("depth") ? OperatorKnobTexts::Kind::depth : OperatorKnobTexts::Kind::plain,
                               juce::jlimit (0, 3, suffix.getTrailingIntValue() - 1));
                    control = std::move (knob);
                }
                addChildComponent (*control);
                effectRules.add (*control, [this, osc] { return FmOperatorInfo::isPlaying (processorRef, osc) && FmOperatorInfo::usesOperatorEnv (processorRef, osc); },
                                 "OSC " + juce::String (osc + 1) + " doesn't play the Operator Env");
                controls[(size_t) osc][(size_t) i] = std::move (control);
            }
        }

        // OP PITCH: the same order, then the voice's tuning.
        for (int i = 0; i < numStageControls; ++i)
        {
            const auto suffix = juce::String ("opeg_pitch") + juce::String (suffixAt (i)).substring (3);
            pitchKnobs[(size_t) i] = OperatorPool::makeStageKnob (p, {}, suffix, labelAt (i), OperatorPool::colour());
            addChildComponent (*pitchKnobs[(size_t) i]);
            texts.add (*pitchKnobs[(size_t) i], {}, i % 2 == 0 ? OperatorKnobTexts::Kind::rate : OperatorKnobTexts::Kind::plain, i / 2);
            effectRules.add (*pitchKnobs[(size_t) i], anyOperator, OperatorPool::unusedReason());
        }
        scaleShift = std::make_unique<KnobControl> (p.apvts, OperatorEg::keyOffsetId, "SCALE SHIFT", OperatorPool::colour(), false);
        addChildComponent (*scaleShift);
        addChildComponent (transpose);
        effectRules.add (*scaleShift, anyOperator, OperatorPool::unusedReason());

        refreshOperators();
        updateVisibility();
        startTimerHz (8);
    }

    // OP ENV (false) or OP PITCH (true).
    void setPitch (bool shouldShowPitch)
    {
        if (pitch == shouldShowPitch)
            return;
        pitch = shouldShowPitch;
        refreshOperators();
        updateVisibility();
    }

    bool isPitch() const { return pitch; }
    int getSelectedOperator() const { return selectedOperator; }

    // The pop-up's layout (UI review 26): one title (the pop-up's), the graph at the left and a column
    // as the envelope pop-ups have, the tabs on its first line, then the knobs as PLAY's rings, a
    // graph handle's time over its level.
    void setCompact (bool shouldBeCompact)
    {
        compact = shouldBeCompact;
        for (auto& knob : pitchKnobs)
            knob->setPackedDial (compact ? compactDial : 0.0f);
        scaleShift->setPackedDial (compact ? compactDial : 0.0f);
        for (auto& row : controls)
            for (int i = 0; i < numStageControls; ++i)
                if (auto* knob = dynamic_cast<KnobControl*> (row[(size_t) i].get()))
                    knob->setPackedDial (compact ? compactDial : 0.0f);
        updateVisibility();
        resized();
        repaint();
    }

    static constexpr float compactDial = 28.0f;
    static constexpr int compactPitch = 46, compactRow = 39, compactGap = 10, compactLine = 20, compactLineGap = 4;
    static constexpr int compactColumn = 4 * compactPitch, compactHeight = compactLine + compactLineGap + 2 * compactRow;

    void selectOperator (int osc)
    {
        selectedOperator = juce::jlimit (0, OscillatorIds::count - 1, osc);
        picker.setSelectedOsc (selectedOperator);
        updateVisibility();
    }

    // The UI test reaches these.
    OperatorEnvDisplay& getGraph() { return graph; }
    CardTabs& getTabs() { return tabs; }
    OscPicker& getPicker() { return picker; }
    juce::TextButton& getUseButton() { return useButton; }
    juce::Component* getControl (int osc, int index) { return controls[(size_t) juce::jlimit (0, 5, osc)][(size_t) juce::jlimit (0, numControls - 1, index)].get(); }
    KnobControl* getKnob (int osc, int index) { return dynamic_cast<KnobControl*> (getControl (osc, index)); }
    KnobControl* getPitchKnob (int index) { return pitchKnobs[(size_t) juce::jlimit (0, numStageControls - 1, index)].get(); }
    KnobControl& getScaleShift() { return *scaleShift; }
    TransposeKnob& getTranspose() { return transpose; }

    // The FM card: the space left free for its own controls (local).
    juce::Rectangle<int> getHostArea() const { return hostArea; }

    void paint (juce::Graphics& g) override
    {
        if (compact)
            return;
        if (place == Place::fmCard)
        {
            if (! tabLine.isEmpty())
            {
                g.setColour (IlanaTheme::Ui::text3);
                g.setFont (IlanaTheme::font (IlanaTheme::TextSize::tiny));
                g.drawText (hintText(), tabLine.withTrimmedLeft (tabs.getRight() - tabLine.getX() + 12), juce::Justification::centredRight, true);
            }
            return;
        }

        const auto active = FmOperatorInfo::anyOperatorEnv (processorRef);
        const auto title = pitch ? juce::String ("OP PITCH") : juce::String ("OP ENV");
        const auto subtitle = ! active ? juce::String ("unused: no oscillator plays it")
                              : pitch  ? juce::String ("every oscillator on the Operator Env")
                                       : "OSC " + juce::String (selectedOperator + 1) + "'s level";
        paintOperatorPanel (g, panel, title, subtitle, panel.getRight() - headerLeft + 8);
        if (! tabLine.isEmpty())
        {
            g.setColour (IlanaTheme::Ui::text3);
            g.setFont (IlanaTheme::font (IlanaTheme::TextSize::tiny));
            g.drawText (hintText(), tabLine.withTrimmedLeft (tabs.getRight() - tabLine.getX() + 12), juce::Justification::centredRight, true);
        }
    }

    void resized() override
    {
        tabs.setNames (pitch ? juce::StringArray { "STAGES", "TRANSPOSE" } : juce::StringArray { "STAGES", "KEYS & VELOCITY" },
                       { OperatorPool::colour(), OperatorPool::colour() });
        tabs.setSelected (pitch ? pitchTab : operatorTab, false);
        const auto rows = shownRows();

        if (compact)
        {
            layoutCompact (rows);
            return;
        }

        if (place == Place::fmCard)
        {
            // Knobs along the bottom, the tabs over them, the graph at the
            // top right; the rest is the card's.
            auto area = getLocalBounds();
            auto row = area.removeFromBottom (juce::jlimit (56, 76, (area.getHeight() - 20) / 3));
            tabLine = area.removeFromBottom (22);
            tabs.setBounds (tabLine.withWidth (tabs.getIdealWidth()).withSizeKeepingCentre (tabs.getIdealWidth(), 20));
            area.removeFromBottom (4);
            graph.setBounds (area.removeFromRight (area.getWidth() / 3).withTrimmedLeft (8).reduced (0, 2));
            hostArea = area;
            layoutKnobs (row, rows, false);
            return;
        }

        auto area = getLocalBounds();
        graph.setBounds (area.removeFromLeft (area.getWidth() * OperatorPool::graphPercent / 100).reduced (2));
        area.removeFromLeft (8);
        panel = area;

        // The header: FM link at the right, the oscillator picker before it.
        // (OP PITCH lives here only, so it has no link.)
        auto header = panel.reduced (12, 0).withHeight (26).withSizeKeepingCentre (panel.getWidth() - 24, 20);
        headerLeft = header.getRight();
        if (fmLink.isVisible())
        {
            fmLink.setBounds (header.removeFromRight (84));
            header.removeFromRight (8);
            headerLeft = fmLink.getX();
        }
        if (picker.isVisible())
        {
            // Room for the title and a word of the line before it.
            const auto room = header.getWidth() - 90; // the title only (review 11, I11-2)
            const auto width = juce::jmin (header.getWidth(), picker.getQuietWidth() <= room ? picker.getQuietWidth() : picker.getShortWidth());
            picker.setBounds (header.removeFromRight (juce::jmax (0, width)).withSizeKeepingCentre (juce::jmax (0, width), 22));
            headerLeft = picker.getX();
        }

        auto inner = panel.reduced (10, 6);
        inner.removeFromTop (20);
        tabLine = inner.removeFromTop (24);
        tabs.setBounds (tabLine.withWidth (tabs.getIdealWidth()).withSizeKeepingCentre (tabs.getIdealWidth(), 20));
        if (useButton.isVisible())
        {
            const auto width = juce::GlyphArrangement::getStringWidthInt (juce::Font (IlanaTheme::pillFont()), useButton.getButtonText()) + 28;
            useButton.setBounds (tabLine.removeFromRight (width).withSizeKeepingCentre (width, 22));
            tabLine.removeFromRight (8);
        }
        inner.removeFromTop (2);
        layoutKnobs (inner, rows, true);
    }

    // Colour coding: each operator's stage knobs and the graph in its oscillator's colour.
    void refreshColours()
    {
        for (int osc = 0; osc < OscillatorIds::count; ++osc)
            for (auto& control : controls[(size_t) osc])
                if (auto* knob = dynamic_cast<KnobControl*> (control.get()))
                    knob->setIdentityColour (IlanaTheme::oscColour (osc));
        updateVisibility();
    }

private:
    void layoutCompact (const std::vector<juce::Component*>& items)
    {
        panel = tabLine = {};
        picker.setBounds ({}); // (the pop-up shows OP PITCH: every oscillator's, no picker)
        auto area = getLocalBounds();
        auto column = area.removeFromRight (compactColumn);
        area.removeFromRight (compactGap);
        graph.setBounds (area);
        if (useButton.isVisible())
        {
            const auto width = juce::GlyphArrangement::getStringWidthInt (juce::Font (IlanaTheme::pillFont()), useButton.getButtonText()) + 24;
            useButton.setBounds (area.getRight() - 6 - width, area.getBottom() - 6 - 22, width, 22);
        }
        tabs.setBounds (column.getX(), column.getY(), juce::jmin (column.getWidth(), tabs.getIdealWidth()), compactLine);
        const auto top = column.getY() + compactLine + compactLineGap;
        const auto cell = [&] (int slot, int row)
        {
            return juce::Rectangle<int> (column.getX() + slot * compactPitch, top + row * compactRow + (compactRow - (int) compactDial - 10) / 2,
                                         compactPitch, (int) compactDial + 10);
        };
        const auto stages = (pitch ? pitchTab : operatorTab) == 0;
        if (stages && items.size() == (size_t) numStageControls)
        {
            // Each column a graph handle: its time over its level.
            for (size_t i = 0; i < items.size(); ++i)
                if (items[i] != nullptr)
                    items[i]->setBounds (KnobControl::packedBoundsFor (cell ((int) i / 2, (int) i % 2)));
            return;
        }
        if (pitch)
        {
            // TRANSPOSE (its own steps knob, two cells over both rows) and SCALE SHIFT beside it.
            transpose.setBounds (juce::Rectangle<int> (column.getX(), top, 2 * compactPitch, 2 * compactRow).reduced (4, 2));
            scaleShift->setBounds (KnobControl::packedBoundsFor (cell (2, 0)));
            return;
        }
        // KEYS & VELOCITY: four to a row (the curve menus in their cells).
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i] != nullptr)
                items[i]->setBounds (dynamic_cast<KnobControl*> (items[i]) != nullptr ? KnobControl::packedBoundsFor (cell ((int) i % 4, (int) i / 4))
                                                                                      : cell ((int) i % 4, (int) i / 4).reduced (2, 6));
    }

    // The controls the selected tab shows, in order.
    std::vector<juce::Component*> shownRows() const
    {
        std::vector<juce::Component*> list;
        const auto stages = (pitch ? pitchTab : operatorTab) == 0;
        if (pitch)
        {
            if (stages)
                for (const auto& knob : pitchKnobs)
                    list.push_back (knob.get());
            else
                list = { (juce::Component*) &transpose, scaleShift.get() };
            return list;
        }
        for (int i = stages ? 0 : numStageControls; i < (stages ? numStageControls : numControls); ++i)
            list.push_back (controls[(size_t) selectedOperator][(size_t) i].get());
        return list;
    }

    // One row on the eight-column grid both tabs share (so a knob keeps its
    // size whichever tab is open); with room for two full rows in the pool,
    // a column per graph handle: its time over its level.
    void layoutKnobs (juce::Rectangle<int> area, std::vector<juce::Component*> items, bool mayStack)
    {
        constexpr int fullRow = 13 + 58 + 16 + 6;
        const auto stages = (pitch ? pitchTab : operatorTab) == 0;
        if (mayStack && stages && items.size() == (size_t) numStageControls && area.getHeight() >= fullRow * 2)
        {
            auto rows = area.withSizeKeepingCentre (area.getWidth(), fullRow * 2);
            auto top = rows.removeFromTop (fullRow);
            std::vector<juce::Component*> times, levels;
            for (size_t i = 0; i < items.size(); ++i)
                (i % 2 == 0 ? times : levels).push_back (items[i]);
            layoutRow (top, times);
            layoutRow (rows, levels);
            return;
        }
        items.resize ((size_t) numStageControls, nullptr);
        // The curve menus a little wider, so their words fit.
        layoutRow (area, items, true, 1.35f);
    }

    juce::String hintText() const
    {
        if ((pitch ? pitchTab : operatorTab) != 0)
            return pitch ? "TRANSPOSE moves every oscillator's SEMI; SCALE SHIFT only the keys the scaling follows."
                         : "How the key and velocity scale this operator";
        return "Drag the graph or the knobs";
    }

    void timerCallback() override
    {
        if (! IlanaAnim::showing (*this))
            return;
        refreshOperators();
        effectRules.apply();
        texts.refresh (processorRef);
        refreshUseButton();
        if (place == Place::pool && changeGate.check (processorRef.getUiEpoch()))
            repaint (panel.withHeight (26));
    }

    // Opened (a card picked, a page shown): greyed and offering USE ON OSC
    // at once, not a frame later.
    void visibilityChanged() override
    {
        if (! isVisible())
            return;
        effectRules.apply();
        refreshUseButton();
    }

    void refreshUseButton()
    {
        const auto unused = place == Place::pool && ! FmOperatorInfo::anyOperatorEnv (processorRef);
        const auto useText = "USE ON OSC " + juce::String (firstShownOscillator() + 1);
        if (unused != useButton.isVisible() || (unused && useButton.getButtonText() != useText))
        {
            useButton.setButtonText (useText);
            useButton.setTooltip ("Set OSC " + juce::String (firstShownOscillator() + 1) + "'s ENVELOPE to OP ENV, so these knobs shape it");
            useButton.setVisible (unused);
            resized();
        }
    }

    // Unused (I8-39): a button sets the first oscillator's ENVELOPE to OP
    // ENV, so the knobs say whose envelope they would change.
    int firstShownOscillator() const
    {
        auto osc = 0;
        while (osc < OscillatorIds::count - 1 && ! processorRef.isOscillatorShown (osc))
            ++osc;
        return osc;
    }

    void useOnFirstOscillator()
    {
        const auto osc = firstShownOscillator();
        if (auto* parameter = processorRef.apvts.getParameter (FmOperatorInfo::prefixOf (osc) + "_amp_env"))
            processorRef.performEdit ("OSC " + juce::String (osc + 1) + " plays OP ENV", [this, parameter, osc]
            {
                // (OP ENV is FM / DX7's: a Wavetable becomes one, the same sound otherwise.)
                if (juce::roundToInt (FmOperatorInfo::read (processorRef, FmOperatorInfo::prefixOf (osc) + "_mode")) == OscMode::wavetable)
                    processorRef.setOscillatorMode (osc, OscMode::fmOperator);
                parameter->beginChangeGesture();
                parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) OperatorEg::envelopeChoice));
                parameter->endChangeGesture();
            });
        selectOperator (osc);
    }

    // The picker follows the oscillators on the Operator Env; the selection
    // moves to one of them when its operator leaves (the pool's; the FM card
    // picks its operator itself).
    void refreshOperators()
    {
        if (place != Place::pool)
            return;
        const auto onEnv = OperatorPool::operatorsOnEnv (processorRef);
        auto changed = false;
        if (! onEnv.empty() && std::find (onEnv.begin(), onEnv.end(), selectedOperator) == onEnv.end())
        {
            selectedOperator = onEnv.front();
            changed = true;
        }
        if (onEnv != picker.getOscillators() || picker.getSelectedOsc() != selectedOperator)
        {
            picker.setOscillators (onEnv, {}, [] (int osc) { return "Edit OSC " + juce::String (osc + 1) + "'s OP ENV"; });
            picker.setSelectedOsc (selectedOperator);
            changed = true;
        }
        const auto wanted = ! pitch && ! onEnv.empty();
        if (picker.isVisible() != wanted)
        {
            picker.setVisible (wanted);
            changed = true;
        }
        useButton.setButtonText ("USE ON OSC " + juce::String ([this] { auto osc = 0; while (osc < OscillatorIds::count - 1 && ! processorRef.isOscillatorShown (osc)) ++osc; return osc + 1; }()));
        if (changed)
            updateVisibility();
    }

    void updateVisibility()
    {
        const auto shown = shownRows();
        const auto isShown = [&shown] (juce::Component* c) { return std::find (shown.begin(), shown.end(), c) != shown.end(); };
        for (auto& row : controls)
            for (auto& control : row)
                control->setVisible (isShown (control.get()));
        for (auto& knob : pitchKnobs)
            knob->setVisible (isShown (knob.get()));
        scaleShift->setVisible (isShown (scaleShift.get()));
        fmLink.setVisible (place == Place::pool && ! pitch && ! compact);
        transpose.setVisible (isShown (&transpose));
        graph.setSource (pitch ? juce::String() : FmOperatorInfo::prefixOf (selectedOperator),
                         pitch ? OperatorPool::colour() : IlanaTheme::oscColour (selectedOperator));
        resized();
        repaint();
    }

    IlanaSynthAudioProcessor& processorRef;
    Place place;
    OperatorEnvDisplay graph;
    CardTabs tabs { { "STAGES", "KEYS & VELOCITY" }, { OperatorPool::colour(), OperatorPool::colour() }, false };
    OscPicker picker;
    juce::TextButton fmLink, useButton;
    std::array<std::array<std::unique_ptr<juce::Component>, (size_t) numControls>, (size_t) OscillatorIds::count> controls;
    std::array<std::unique_ptr<KnobControl>, (size_t) numStageControls> pitchKnobs;
    std::unique_ptr<KnobControl> scaleShift;
    TransposeKnob transpose;
    OperatorKnobTexts texts;
    EffectRules effectRules;
    IlanaAnim::ChangeGate changeGate;
    juce::Rectangle<int> panel, tabLine, hostArea;
    int headerLeft = 0;
    int selectedOperator = 0, operatorTab = 0, pitchTab = 0;
    bool pitch = false, compact = false;
};

// The Op LFO's picture: one cycle of its wave, as the pool LFOs' graphs
// draw theirs, with its live value at the right edge.
class OperatorLfoView : public juce::Component,
                        public juce::SettableTooltipClient,
                        private IlanaAnim::FrameTimer
{
public:
    explicit OperatorLfoView (IlanaSynthAudioProcessor& p) : processorRef (p)
    {
        setTooltip ("OP LFO\nOne cycle of its shape. Its DELAY fades it in after each note (with RETRIG on, it restarts too); "
                    "the tick at the right is where it is now.");
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        IlanaTheme::paintWell (g, bounds, 6.0f);
        const auto plot = bounds.reduced (12.0f, 16.0f);
        const auto centreY = plot.getCentreY();
        const auto half = plot.getHeight() * 0.42f;
        const auto active = FmOperatorInfo::anyOperatorEnv (processorRef);
        const auto colour = OperatorPool::colour().withAlpha (active ? 1.0f : 0.35f);

        g.setColour (juce::Colours::white.withAlpha (0.04f));
        for (const auto level : { -1.0f, -0.5f, 0.5f, 1.0f })
            g.fillRect (juce::Rectangle<float> (plot.getWidth(), 1.0f).withPosition (plot.getX(), centreY - level * half));
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (juce::Rectangle<float> (plot.getWidth(), 1.0f).withCentre ({ plot.getCentreX(), centreY }));

        const auto wave = juce::roundToInt (FmOperatorInfo::read (processorRef, "opeg_lfo_wave"));
        juce::Path path;
        const auto points = juce::jmax (2, (int) plot.getWidth());
        for (int i = 0; i <= points; ++i)
        {
            const auto phase = (double) i / (double) points;
            const auto x = plot.getX() + plot.getWidth() * (float) phase;
            const auto y = centreY - OperatorPool::lfoWaveValue (wave, juce::jmin (0.99999, phase)) * half;
            if (i == 0)
                path.startNewSubPath (x, y);
            else if (wave == 3 || wave == 5)
            {
                path.lineTo (x, path.getCurrentPosition().y);
                path.lineTo (x, y);
            }
            else
                path.lineTo (x, y);
        }
        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (1.6f));

        if (active)
        {
            const auto now = juce::jlimit (-1.0f, 1.0f, processorRef.getSourceDisplayValue ((int) Mod::Source::OpLfo));
            g.setColour (colour);
            g.fillRect (juce::Rectangle<float> (8.0f, 2.0f).withPosition (plot.getRight() + 2.0f, centreY - now * half - 1.0f));
        }

        g.setColour (IlanaTheme::Ui::text3);
        g.setFont (IlanaTheme::font (IlanaTheme::TextSize::tiny));
        g.drawText (active ? "one cycle at " + describeValue ("opeg_lfo_speed", FmOperatorInfo::read (processorRef, "opeg_lfo_speed"))
                           : juce::String ("unused: no oscillator plays the Operator Env"),
                    bounds.reduced (12.0f, 2.0f).removeFromBottom (13.0f), juce::Justification::centredLeft);
    }

private:
    void timerCallback() override
    {
        if (IlanaAnim::showing (*this) && changeGate.check (processorRef.getUiEpoch()
                                             ^ IlanaAnim::phaseSignature (processorRef.getSourceDisplayValue ((int) Mod::Source::OpLfo), 41)))
            repaint();
    }

    IlanaSynthAudioProcessor& processorRef;
    IlanaAnim::ChangeGate changeGate;
};

// OP LFO, edited in the LFO pool's place, laid out as the pool LFOs' panel:
// SHAPE and RETRIG on the top row, RATE and DELAY below, then what it moves
// on the operators (the DX7's PITCH DEPTH, PITCH SENS and AMP DEPTH).
class OperatorLfoEditor : public juce::Component,
                          private IlanaAnim::FrameTimer
{
public:
    explicit OperatorLfoEditor (IlanaSynthAudioProcessor& p)
        : processorRef (p),
          view (p),
          shape (p.apvts, "opeg_lfo_wave", "SHAPE"),
          retrig (p.apvts, "opeg_lfo_sync", "RETRIG"),
          rate (p.apvts, "opeg_lfo_speed", "RATE", OperatorPool::colour(), false),
          delay (p.apvts, "opeg_lfo_delay", "DELAY", OperatorPool::colour(), false),
          pitchDepth (p.apvts, "opeg_lfo_pmd", "PITCH DEPTH", OperatorPool::colour(), false),
          pitchSens (p.apvts, "opeg_lfo_pms", "PITCH SENS", OperatorPool::colour(), false),
          ampDepth (p.apvts, "opeg_lfo_amd", "AMP DEPTH", OperatorPool::colour(), false),
          effectRules (p)
    {
        retrig.showAsSwitch();
        LfoShapeMenu::applyOperatorLfo (shape);
        for (auto* child : std::initializer_list<juce::Component*> { &view, &shape, &retrig, &rate, &delay, &pitchDepth, &pitchSens, &ampDepth })
            addAndMakeVisible (child);
        const auto anyOperator = [this] { return FmOperatorInfo::anyOperatorEnv (processorRef); };
        for (auto* knob : { &rate, &delay, &pitchDepth, &pitchSens, &ampDepth })
            effectRules.add (*knob, anyOperator, OperatorPool::unusedReason());
        startTimerHz (8);
    }

    // The UI test reaches these.
    ComboControl& getShape() { return shape; }
    juce::Rectangle<int> getPanelBounds() const { return panel; }

    // The pop-up's layout (UI review 26): one title (the pop-up's), the picture at the left and a
    // column as the LFO pop-ups have: SHAPE on its first line, RATE DELAY and the RETRIG switch over
    // what it moves on the operators, PLAY's ring knobs three to a row (their names whole).
    void setCompact (bool shouldBeCompact)
    {
        compact = shouldBeCompact;
        shape.setPackedMenu (compact);
        for (auto* knob : { &rate, &delay, &pitchDepth, &pitchSens, &ampDepth })
            knob->setPackedDial (compact ? compactDial : 0.0f);
        retrig.setPackedName (compact ? ToggleControl::NamePlace::below : ToggleControl::NamePlace::none);
        resized();
        repaint();
    }

    static constexpr float compactDial = 28.0f;
    static constexpr int compactPitch = 61, compactRow = 39, compactGap = 10, compactLine = 16, compactLineGap = 4;
    static constexpr int compactColumn = 3 * compactPitch, compactHeight = compactLine + compactLineGap + 2 * compactRow;

    void paint (juce::Graphics& g) override
    {
        if (compact)
            return;
        const auto active = FmOperatorInfo::anyOperatorEnv (processorRef);
        paintOperatorPanel (g, panel, "OP LFO",
                            active ? "the DX7 LFO: every oscillator on the Operator Env follows it"
                                   : juce::String ("unused: ") + OperatorPool::unusedReason(),
                            0);
        // The operator half of the controls, under its own small heading.
        if (! operatorHeading.isEmpty())
        {
            g.setColour (IlanaTheme::Ui::text3);
            g.setFont (IlanaTheme::font (IlanaTheme::TextSize::tiny, true));
            g.drawText ("ON THE OPERATORS", operatorHeading, juce::Justification::centredLeft);
        }
        if (! hintArea.isEmpty())
        {
            g.setColour (IlanaTheme::Ui::text3);
            g.setFont (IlanaTheme::font (IlanaTheme::TextSize::body));
            IlanaTheme::drawFitted (g, "Pitch moves only while PITCH DEPTH and PITCH SENS are both above 0. AMP DEPTH moves the operators whose AMP MOD is above 0.",
                                    hintArea, juce::Justification::topLeft, 2);
        }
    }

    void resized() override
    {
        if (compact)
        {
            panel = operatorHeading = hintArea = {};
            auto area = getLocalBounds();
            auto column = area.removeFromRight (compactColumn);
            area.removeFromRight (compactGap);
            view.setBounds (area);
            shape.setBounds (column.getX(), column.getY(), column.getWidth(), compactLine);
            const auto top = column.getY() + compactLine + compactLineGap;
            const auto cell = [&] (int slot, int row)
            {
                return juce::Rectangle<int> (column.getX() + slot * compactPitch, top + row * compactRow + (compactRow - (int) compactDial - 10) / 2,
                                             compactPitch, (int) compactDial + 10);
            };
            rate.setBounds (KnobControl::packedBoundsFor (cell (0, 0)));
            delay.setBounds (KnobControl::packedBoundsFor (cell (1, 0)));
            retrig.setBounds (cell (2, 0).withTrimmedTop (12)); // (its name on the knob names' line)
            pitchDepth.setBounds (KnobControl::packedBoundsFor (cell (0, 1)));
            pitchSens.setBounds (KnobControl::packedBoundsFor (cell (1, 1)));
            ampDepth.setBounds (KnobControl::packedBoundsFor (cell (2, 1)));
            return;
        }

        auto area = getLocalBounds();
        view.setBounds (area.removeFromLeft (area.getWidth() * OperatorPool::graphPercent / 100).reduced (2));
        area.removeFromLeft (8);
        panel = area;

        auto inner = panel.reduced (10, 6);
        inner.removeFromTop (20);
        // As the LFO panel: SHAPE and the switch on one row, the knobs
        // filling the rest.
        auto options = inner.removeFromTop (13 + 24 + 14);
        shape.setBounds (options.removeFromLeft (options.getWidth() / 2).reduced (3, 1));
        retrig.setBounds (options.removeFromLeft (options.getWidth() / 3).reduced (3, 1));
        inner.removeFromTop (6);
        // The heading sits on its knob row and the row right under the options, a
        // line of what the knobs do under it, so no blank band is left between
        // them (N16-11).
        operatorHeading = inner.withLeft (inner.getX() + inner.getWidth() * 2 / 5 + 3).withHeight (12);
        inner.removeFromTop (14);
        const auto knobHeight = juce::jmin (inner.getHeight() - 30, 96);
        layoutRow (inner.removeFromTop (juce::jmax (60, knobHeight)), { &rate, &delay, &pitchDepth, &pitchSens, &ampDepth });
        hintArea = inner.getHeight() >= 24 ? inner.withTrimmedTop (4) : juce::Rectangle<int>();
    }

private:
    void timerCallback() override
    {
        if (! IlanaAnim::showing (*this))
            return;
        effectRules.apply();
        if (changeGate.check (processorRef.getUiEpoch()))
            repaint (panel.withHeight (26));
    }

    IlanaSynthAudioProcessor& processorRef;
    OperatorLfoView view;
    ComboControl shape;
    ToggleControl retrig;
    KnobControl rate, delay, pitchDepth, pitchSens, ampDepth;
    EffectRules effectRules;
    IlanaAnim::ChangeGate changeGate;
    juce::Rectangle<int> panel, operatorHeading, hintArea;
    bool compact = false;
};
