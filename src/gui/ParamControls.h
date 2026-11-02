#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>

#include "../PluginProcessor.h"
#include "AnimationUtils.h"
#include "IlanaLookAndFeel.h"
#include "ModNames.h"
#include "ParamInfo.h"
#include "ModulePool.h"

struct ModRingConfig
{
    // How a route's amount moves the knob's value: as a share of the knob's
    // travel (plain parameters), in octaves (value x 2^(amount x scale)) or
    // in the knob's own unit (value + amount x scale).
    enum class Unit { travel, octaves, value };

    int destination = 0;
    float scale = 1.0f;
    Unit unit = Unit::travel;

    // The knob's value moved by `amount` (a route's depth x its source), as
    // the voices hear it, kept inside the knob's range.
    float modulated (const juce::NormalisableRange<float>& range, float plain, float amount) const
    {
        switch (unit)
        {
            case Unit::octaves: return juce::jlimit (range.start, range.end, plain * std::exp2 (amount * scale));
            case Unit::value:   return juce::jlimit (range.start, range.end, plain + amount * scale);
            case Unit::travel:  break;
        }
        return range.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, range.convertTo0to1 (plain) + amount * scale));
    }
};

// Which modulation destination a knob drives, and how a full-depth route
// moves it. Since 2026-10-23 every one spans the knob's whole range at
// 100 % (Mod::Range); the octave and own-unit ones say so, so the ring
// lands where the sound does on a skewed knob.
inline ModRingConfig modRingConfigFor (const juce::String& id)
{
    using D = Mod::Destination;
    using Unit = ModRingConfig::Unit;
    const auto explicitDest = [] (D d, float scale) { return ModRingConfig { (int) d, scale }; };
    const auto inOctaves = [] (D d, float octaves) { return ModRingConfig { (int) d, octaves, Unit::octaves }; };
    namespace R = Mod::Range;

    struct OscIds { const char* prefix; D pitch, frame, level, pan, detune, spread, warp, blend, start, end; };
    static const OscIds oscs[] {
        { "osc1", D::Osc1Pitch, D::Osc1Frame, D::Osc1Level, D::Osc1Pan, D::Osc1Detune, D::Osc1Spread, D::Osc1Warp,
          D::Osc1Blend, D::Osc1SampleStart, D::Osc1SampleEnd },
        { "osc2", D::Osc2Pitch, D::Osc2Frame, D::Osc2Level, D::Osc2Pan, D::Osc2Detune, D::Osc2Spread, D::Osc2Warp,
          D::Osc2Blend, D::Osc2SampleStart, D::Osc2SampleEnd },
        { "sub", D::SubPitch, D::SubFrame, D::SubLevel, D::SubPan, D::SubDetune, D::SubSpread, D::SubWarp,
          D::SubBlend, D::SubSampleStart, D::SubSampleEnd },
        { "osc4", D::Osc4Pitch, D::Osc4Frame, D::Osc4Level, D::Osc4Pan, D::Osc4Detune, D::Osc4Spread, D::Osc4Warp,
          D::Osc4Blend, D::Osc4SampleStart, D::Osc4SampleEnd },
        { "osc5", D::Osc5Pitch, D::Osc5Frame, D::Osc5Level, D::Osc5Pan, D::Osc5Detune, D::Osc5Spread, D::Osc5Warp,
          D::Osc5Blend, D::Osc5SampleStart, D::Osc5SampleEnd },
        { "osc6", D::Osc6Pitch, D::Osc6Frame, D::Osc6Level, D::Osc6Pan, D::Osc6Detune, D::Osc6Spread, D::Osc6Warp,
          D::Osc6Blend, D::Osc6SampleStart, D::Osc6SampleEnd },
    };

    for (const auto& osc : oscs)
    {
        const juce::String prefix (osc.prefix);

        if (! id.startsWith (prefix + "_"))
            continue;

        const auto field = id.fromFirstOccurrenceOf (prefix + "_", false, false);

        if (field == "semi" || field == "fine") return explicitDest (osc.pitch, 1.0f);
        if (field == "frame")        return explicitDest (osc.frame, 1.0f);
        if (field == "level")        return explicitDest (osc.level, 1.0f);
        if (field == "pan")          return explicitDest (osc.pan, 1.0f);
        if (field == "detune")       return { (int) osc.detune, R::detuneCents, Unit::value };
        if (field == "spread")       return explicitDest (osc.spread, 1.0f);
        if (field == "warp_amt")     return explicitDest (osc.warp, 1.0f);
        if (field == "uni_blend")    return explicitDest (osc.blend, 1.0f);
        if (field == "sample_start") return explicitDest (osc.start, 1.0f);
        if (field == "sample_end")   return explicitDest (osc.end, 1.0f);
    }

    struct OctaveEntry { const char* id; D destination; float octaves; };
    static const OctaveEntry octaveEntries[] {
        { "f1_cutoff", D::Filter1Cutoff, R::cutoffOctaves }, { "f2_cutoff", D::Filter2Cutoff, R::cutoffOctaves },
        { "lfo1_rate", D::Lfo1Rate, R::lfoRateOctaves }, { "lfo2_rate", D::Lfo2Rate, R::lfoRateOctaves },
        { "lfo3_rate", D::Lfo3Rate, R::lfoRateOctaves }, { "lfo4_rate", D::Lfo4Rate, R::lfoRateOctaves },
        { "mseg_rate", D::MsegRate, R::msegRateOctaves },
        { "amp_attack", D::AmpAttack, R::attackOctaves }, { "amp_decay", D::AmpDecay, R::decayOctaves },
        { "amp_release", D::AmpRelease, R::releaseOctaves },
        { "fe_attack", D::FeAttack, R::attackOctaves }, { "fe_decay", D::FeDecay, R::decayOctaves },
        { "fe_release", D::FeRelease, R::releaseOctaves },
        { "me_attack", D::MeAttack, R::attackOctaves }, { "me_decay", D::MeDecay, R::decayOctaves },
        { "me_release", D::MeRelease, R::releaseOctaves },
        { "f2e_attack", D::F2eAttack, R::attackOctaves }, { "f2e_decay", D::F2eDecay, R::decayOctaves },
        { "f2e_release", D::F2eRelease, R::releaseOctaves },
        { "e4_attack", D::E4Attack, R::attackOctaves }, { "e4_decay", D::E4Decay, R::decayOctaves },
        { "e4_release", D::E4Release, R::releaseOctaves },
        { "fx_comb_freq", D::FxCombFreq, R::combOctaves }, { "fx_phaser_rate", D::FxPhaserRate, R::phaserOctaves },
    };

    for (const auto& entry : octaveEntries)
        if (id == entry.id)
            return inOctaves (entry.destination, entry.octaves);

    struct Entry { const char* id; D destination; float scale; };
    static const Entry entries[] {
        { "noise_level", D::NoiseLevel, 1.0f },
        { "f1_reso", D::Filter1Reso, 1.0f },
        { "f1_drive", D::Filter1Drive, 1.0f }, { "f1_env", D::Filter1Env, 1.0f }, { "f1_fm", D::Filter1Fm, 1.0f },
        { "f1_morph", D::Filter1Morph, 1.0f },
        { "f2_reso", D::Filter2Reso, 1.0f },
        { "f2_drive", D::Filter2Drive, 1.0f }, { "f2_env", D::Filter2Env, 1.0f }, { "f2_fm", D::Filter2Fm, 1.0f },
        { "f2_morph", D::Filter2Morph, 1.0f },
        { "fm_amount", D::FmAmount, 1.0f }, { "fm_feedback", D::FmFeedback, 1.0f },
        { "fm_1to2", D::Fm1to2, 1.0f }, { "fm_1to3", D::Fm1to3, 1.0f }, { "fm_2to3", D::Fm2to3, 1.0f },
        { "fm_3to1", D::Fm3to1, 1.0f }, { "fm_3to2", D::Fm3to2, 1.0f },
        { "fm_fb2", D::Fm2Feedback, 1.0f }, { "fm_fb3", D::Fm3Feedback, 1.0f },
        { "ring_mod", D::RingMod, 1.0f }, { "drift", D::Drift, 1.0f },
        { "res_amount", D::ResAmount, 1.0f }, { "res_decay", D::ResDecay, 1.0f }, { "res_offset", D::ResOffset, 1.0f },
        { "amp_sustain", D::AmpSustain, 1.0f }, { "fe_sustain", D::FeSustain, 1.0f }, { "me_sustain", D::MeSustain, 1.0f },
        { "f2e_sustain", D::F2eSustain, 1.0f }, { "e4_sustain", D::E4Sustain, 1.0f },
        { "fx_drive_amount", D::FxDriveAmount, 1.0f }, { "fx_crush_mix", D::FxCrushMix, 1.0f },
        { "fx_chorus_depth", D::FxChorusDepth, 1.0f }, { "fx_delay_mix", D::FxDelayMix, 1.0f },
        { "fx_delay_feedback", D::FxDelayFeedback, 1.0f }, { "fx_smear_mix", D::FxSmearMix, 1.0f },
        { "fx_freeze_mix", D::FxFreezeMix, 1.0f }, { "fx_reverb_mix", D::FxReverbMix, 1.0f },
        { "fx_reverb_size", D::FxReverbSize, 1.0f },
    };

    for (const auto& entry : entries)
        if (id == entry.id)
            return explicitDest (entry.destination, entry.scale);

    for (int lfo = 4; lfo < Mod::numLfoSources; ++lfo)
        if (id == "lfo" + juce::String (lfo + 1) + "_rate")
            return inOctaves (Mod::lfoRateDestinationFor (lfo), R::lfoRateOctaves);

    // Everything else that can be modulated is a plain-parameter destination,
    // offset in the knob's own normalised range.
    return { Mod::destinationForParamId (id), 1.0f };
}

// What a mod source drives, for the envelope and LFO pool cards: "fixed"
// (built-in uses such as "Amp") first, then the matrix destinations, as
// "first target +N more". Empty when it drives nothing.
inline juce::String describeModTargets (const IlanaSynthAudioProcessor& processor, Mod::Source source,
                                        juce::StringArray targets = {})
{
    for (int slot = 0; slot < Mod::maxSlots; ++slot)
    {
        const auto routing = processor.readModSlot (slot);

        if (routing.isActive() && (routing.source == source || routing.aux == source))
            targets.addIfNotAlreadyThere (ModNames::destination (routing.destination));
    }

    if (targets.isEmpty())
        return {};

    // "+2 more", not "+2", which read as a depth beside the matrix's "+70%"
    // (UI review 8, S8-18).
    return targets[0] + (targets.size() > 1 ? "  +" + juce::String (targets.size() - 1) + " more" : juce::String());
}

// A small tag naming what a card drives, at the left of `area`'s bottom.
// A narrow card passes `below`, a row under its title: a tag that would be
// cut short on the title line ("Filte...") moves there whole.
inline void paintTargetTag (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, juce::Colour colour,
                            juce::Rectangle<float> below = {})
{
    const auto font = juce::Font (IlanaTheme::font (IlanaTheme::TextSize::tiny, true));
    const auto wanted = juce::GlyphArrangement::getStringWidth (font, text) + 12.0f;

    if (wanted > area.getWidth() && ! below.isEmpty())
        area = below;

    // A tag that still doesn't fit is left out rather than squeezed: the
    // card's tooltip names its targets (review 8, V8-29).
    if (text.isEmpty() || wanted > area.getWidth())
        return;

    const auto width = juce::jmin (area.getWidth(), wanted);
    const auto tag = juce::Rectangle<float> (area.getX(), area.getBottom() - 15.0f, width, 15.0f);

    // A rimmed tag, as the "OUT 2" tag is, so a card's tokens share one
    // look whether it is selected or not (V12-25).
    g.setColour (IlanaTheme::Ui::bg.withAlpha (0.85f));
    g.fillRoundedRectangle (tag, 4.0f);
    g.setColour (colour.withAlpha (0.6f));
    g.drawRoundedRectangle (tag.reduced (0.5f), 4.0f, 1.0f);
    g.setColour (colour.withAlpha (0.9f));
    g.setFont (font);
    IlanaTheme::drawFitted (g, text, tag.reduced (6.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1);
}

// The one "go to its full editor" idiom (UI review 9, I9-14): a raised
// button reading "EDIT <WHAT> ›" at the right of a card header or line,
// the same on every page (EDIT OP ENV, EDIT STRING, EDIT BODY...). Icons are
// kept for pop-outs only.
inline void styleJumpLink (juce::TextButton& button, const juce::String& what)
{
    button.setButtonText ("EDIT " + what + juce::String::fromUTF8 (" \xe2\x80\xba"));
    button.getProperties().remove ("pill");
    button.setColour (juce::TextButton::buttonColourId, IlanaTheme::Ui::raised);
    button.setColour (juce::TextButton::textColourOffId, IlanaTheme::Ui::text);
}

// The source currently hovered (chip, macro or LFO card), so knobs it
// modulates can light up. Message thread only.
inline int& highlightedModSource()
{
    static int source = 0;
    return source;
}

// The source pinned by clicking its chip in the bottom bar (0: none). Knobs
// it drives stay lit until the chip is clicked again. Message thread only.
inline int& pinnedModSource()
{
    static int source = 0;
    return source;
}

// The source a drag under way carries ("modsource:N" from a chip, a macro,
// an LFO or envelope card), 0 while none is dragged. Knobs poll it to show
// every drop target as soon as a drag starts (review 8, V8-10).
inline int modSourceBeingDragged (juce::Component& component)
{
    auto* container = juce::DragAndDropContainer::findParentDragContainerFor (&component);

    if (container == nullptr || ! container->isDragAndDropActive())
        return 0;

    const auto description = container->getCurrentDragDescription().toString();
    return description.startsWith ("modsource:") ? description.fromFirstOccurrenceOf (":", false, false).getIntValue() : 0;
}

// One colour per modulation source, used by its chip, its card (LFO and
// envelope pages), its tabs, the matrix and the rings on knobs it moves.
// Every source has its own: macros step from yellow to amber (and carry
// their number on the knob's dot), the performance sources each take a soft
// hue of their own, and ENV 6-16 come from a fixed list that keeps clear of
// the main envelopes' and LFO 1-4's hues.
inline juce::Colour modSourceColour (int sourceIndex)
{
    // Every LFO (A and B outputs) in the LFO palette.
    if (const auto lfo = Mod::lfoIndexFor ((Mod::Source) sourceIndex); lfo >= 0)
        return IlanaSynthAudioProcessor::lfoColour (lfo);

    if (sourceIndex >= (int) Mod::Source::Lfo1B && sourceIndex <= (int) Mod::Source::Lfo16B)
        return IlanaSynthAudioProcessor::lfoColour (sourceIndex - (int) Mod::Source::Lfo1B);

    if (const auto macro = Mod::macroIndexFor ((Mod::Source) sourceIndex); macro >= 0)
    {
        // Yellow (macro 1) to amber (macro 8), alternately lighter and
        // deeper so neighbours differ by more than the hue step.
        const auto t = (float) macro / (float) (Mod::numMacros - 1);
        return juce::Colour::fromHSV (0.155f - 0.07f * t, 0.70f + 0.12f * t + (macro % 2 == 0 ? 0.0f : 0.06f),
                                      macro % 2 == 0 ? 1.0f : 0.90f, 1.0f);
    }

    switch ((Mod::Source) sourceIndex)
    {
        case Mod::Source::AmpEnv:     return IlanaTheme::envIdentityColour (0); // not the accent: it would clash with FILT ENV or LFO 1
        case Mod::Source::FilterEnv:  return IlanaTheme::envIdentityColour (1);
        case Mod::Source::FilterEnv2: return IlanaTheme::envIdentityColour (2);
        case Mod::Source::ModEnv:     return IlanaTheme::envIdentityColour (3);
        case Mod::Source::Env4:       return IlanaTheme::envIdentityColour (4);
        // The performance sources: soft tints, each its own hue.
        case Mod::Source::Velocity:   return juce::Colour::fromHSV (0.03f, 0.34f, 0.97f, 1.0f); // peach
        case Mod::Source::KeyTrack:   return juce::Colour::fromHSV (0.31f, 0.34f, 0.92f, 1.0f); // sage
        case Mod::Source::Random:     return juce::Colour::fromHSV (0.88f, 0.34f, 0.95f, 1.0f); // orchid
        case Mod::Source::ClockSh:    return juce::Colour::fromHSV (0.70f, 0.30f, 0.97f, 1.0f); // lavender
        case Mod::Source::ModWheel:   return juce::Colour::fromHSV (0.60f, 0.34f, 0.97f, 1.0f); // pale sky (not LFO 2's cyan)
        case Mod::Source::Aftertouch: return juce::Colour::fromHSV (0.80f, 0.30f, 0.97f, 1.0f); // pale lilac (not LFO 1's rose)
        case Mod::Source::Expression: return juce::Colour::fromHSV (0.21f, 0.38f, 0.92f, 1.0f); // pale lime
        case Mod::Source::Mseg:       return juce::Colour::fromHSV (0.60f, 0.30f, 1.0f, 1.0f); // an LFO pastel, not white (I9-2)
        case Mod::Source::InputEnv:   return juce::Colour (0xffc9b79c); // sand
        case Mod::Source::VectorX:    return juce::Colour (0xff7fe0d8);
        case Mod::Source::VectorY:    return juce::Colour (0xff6fb8ff);
        // The Operator Env's: the FM page's amber, and a lighter one.
        case Mod::Source::OpLfo:      return juce::Colour (0xffe3a56f);
        case Mod::Source::OpPitchEnv: return juce::Colour (0xfff0c99a);
        default: break;
    }

    // ENV 6-16: a fixed list, deeper than the LFO pool's pastels, with hues
    // between AMP (red), MOD (lime), ENV 5 / FILT 2 (blues), FILT (violet)
    // and LFO 1-4 (rose, cyan, teal, yellow-green).
    if (sourceIndex >= (int) Mod::Source::Env6 && sourceIndex <= (int) Mod::Source::Env16)
    {
        static constexpr float hues[] { 0.05f, 0.22f, 0.32f, 0.37f, 0.42f, 0.50f, 0.70f, 0.74f, 0.84f, 0.89f, 0.975f };
        return juce::Colour::fromHSV (hues[sourceIndex - (int) Mod::Source::Env6], 0.80f, 0.86f, 1.0f);
    }

    return IlanaTheme::accent();
}

// How a knob opens and closes the editor's modulation card (ModHoverPopup,
// which sets these while it exists): the knob, its destination and title.
struct ModHoverHooks
{
    std::function<void (juce::Component&, int, const juce::String&)> show;
    std::function<void (const juce::Component&)> hide;
    // True while the mouse is on the knob's card (or dragging a row, or its
    // menu is open), so the card stays when the mouse moves onto it.
    std::function<bool (const juce::Component&)> engaged;
    // The same card for a source (a macro in the strip, a chip): where it
    // goes, with a warning for targets whose module is off.
    std::function<void (juce::Component&, int source)> showSource;
};

inline ModHoverHooks& modHoverHooks()
{
    static ModHoverHooks hooks;
    return hooks;
}

inline juce::String& knobClipboard()
{
    static juce::String value;
    return value;
}

// The badges beside a modulated knob: one per routing into it, a disc in
// the source's colour (a macro's carries its number). They are the legend
// for the knob's rings, shown while the knob is hovered (V7-27), and
// controls too:
// drag one up or down to set that routing's depth, double-click it to zero
// the depth (as knobs, rings and the source card do), right-click it for
// the routing's menu (polarity, stereo, amount, bypass, remove; the rings
// share it). More routings than fit show as a "+N"
// badge, which opens the knob's source card with all of them.
class ModDotStrip : public juce::Component,
                    public juce::SettableTooltipClient
{
public:
    struct Dot
    {
        int slot = -1;
        int source = 0;
        float depth = 0.0f;
        bool bypass = false;
        bool bipolar = false;
        int polarity = 0;           // Mod::Polarity
        bool stereo = false;
        bool stereoAllowed = false; // Mod::canBeStereoAnywhere (its destination)
    };

    std::function<void (int slot, float depth)> onDepthChange;
    // Double-click: the depth to zero, one undo step.
    std::function<void (int slot)> onZero;
    // The right-click menu's choices.
    std::function<void (int slot)> onRemove;
    std::function<void (int slot, bool bypass)> onBypass;
    std::function<void (int slot, int polarity)> onPolarity;
    std::function<void (int slot, bool stereo)> onStereo;
    std::function<void (int slot)> onTypeAmount;
    // A depth drag's start and end (one undo step).
    std::function<void (int source)> onDragStart;
    std::function<void()> onDragEnd;
    // The "+N" badge: show every routing (the knob's source card).
    std::function<void()> onShowAll;

    // A routing's right-click menu, on its ring or its badge (Vital's):
    // polarity, stereo, then the amount, bypass and remove.
    static void addRouteItems (juce::PopupMenu& menu, const Dot& dot)
    {
        menu.addSectionHeader (ModNames::source (dot.source) + "  (" + juce::String (Mod::shownPercent (dot.depth, dot.bipolar)) + "%)");

        const auto add = [&menu] (int id, const juce::String& text, const juce::String& hint, bool enabled, bool ticked)
        {
            juce::PopupMenu::Item item (text);
            item.itemID = id;
            item.shortcutKeyDescription = hint;
            item.isEnabled = enabled;
            item.isTicked = ticked;
            menu.addItem (std::move (item));
        };

        add (11, "Unipolar", "one way, up", true, dot.polarity == (int) Mod::Polarity::Unipolar);
        add (12, "Negative", "one way, down", true, dot.polarity == (int) Mod::Polarity::Negative);
        add (13, "Bipolar", "both sides", true, dot.polarity == (int) Mod::Polarity::Bipolar);
        add (14, "Auto", "source's range", true, dot.polarity == (int) Mod::Polarity::Natural);
        menu.addSeparator();
        add (15, "Stereo", dot.stereoAllowed ? "left up, right down" : "filter cutoffs only", dot.stereoAllowed, dot.stereo);
        menu.addSeparator();
        add (10, "Type an amount...", {}, true, false);
        add (1, "Bypass", {}, true, dot.bypass);
        add (2, "Remove", {}, true, false);
    }

    // Acts on a choice from addRouteItems' menu.
    void handleRouteChoice (int result, const Dot& dot)
    {
        if (result == 1 && onBypass != nullptr)
            onBypass (dot.slot, ! dot.bypass);
        else if (result == 2 && onRemove != nullptr)
            onRemove (dot.slot);
        else if (result == 10 && onTypeAmount != nullptr)
            onTypeAmount (dot.slot);
        else if (result >= 11 && result <= 14 && onPolarity != nullptr)
        {
            const Mod::Polarity choices[] { Mod::Polarity::Unipolar, Mod::Polarity::Negative, Mod::Polarity::Bipolar, Mod::Polarity::Natural };
            onPolarity (dot.slot, (int) choices[result - 11]);
        }
        else if (result == 15 && onStereo != nullptr)
            onStereo (dot.slot, ! dot.stereo);
    }

    void setDots (const std::vector<Dot>& newDots)
    {
        if (dragIndex >= 0)
            return; // keep the list stable mid-drag

        dots = newDots;
        updateTooltip();
        repaint();
    }

    // At least 14 px, with a 20 px target (UI review 5/6, S5-5).
    static constexpr int dotSize = 14;
    static constexpr int dotPitch = 16;
    static constexpr int stripWidth = 18;

    // The packed cards' knobs carry small badges (9 px discs): the full size sat over the dial's rings (ilana, review 23).
    void setMini (bool shouldBeMini)
    {
        if (mini != shouldBeMini)
        {
            mini = shouldBeMini;
            repaint();
        }
    }
    int diameter() const { return mini ? 11 : dotSize; }
    int pitchPx() const { return mini ? 13 : dotPitch; }
    int widthPx() const { return mini ? 18 : stripWidth; }

    // How many badges the strip has room for; past that, "+N".
    void setMaxVisible (int count)
    {
        maxVisible = juce::jmax (mini ? 0 : 1, count);
        repaint();
    }

    int getNumShown() const { return numShown(); }
    bool hasOverflow() const { return (int) dots.size() > maxVisible; }
    int getPreferredHeight() const { return juce::jmax (1, numShown() + (hasOverflow() ? 1 : 0)) * pitchPx(); }

    void paint (juce::Graphics& g) override
    {
        for (int i = 0; i < numShown(); ++i)
        {
            const auto& dot = dots[(size_t) i];
            const auto area = dotBounds (i);
            const auto colour = modSourceColour (dot.source);
            // (The hover's glow fades: the shared animator.)
            const auto active = IlanaTheme::fade (*this, i, i == dragIndex || i == hoverIndex ? 1.0f : 0.0f, IlanaTheme::FadeRate::hover);

            g.setColour (colour.withAlpha (0.16f + 0.19f * active));
            g.fillEllipse (area.expanded (1.0f + active));

            // A plain disc in the source's colour: the ring shows the depth,
            // the badge only which source it is (review 7, V7-27: no pie).
            g.setColour (IlanaTheme::Ui::bg);
            g.fillEllipse (area);
            g.setColour (colour.withAlpha (dot.bypass ? 0.3f : 0.9f));
            g.fillEllipse (area.reduced (1.0f));
            g.setColour (colour.withAlpha (dot.bypass ? 0.4f : 0.95f));
            g.drawEllipse (area.reduced (0.5f), 1.2f);

            // A macro's number, white with a dark outline so it reads on
            // its colour.
            if (const auto macro = Mod::macroIndexFor ((Mod::Source) dot.source); macro >= 0 && ! mini)
                paintOutlinedText (g, juce::String (macro + 1), area, IlanaTheme::TextSize::tiny);
        }

        if (hasOverflow())
        {
            const auto area = dotBounds (numShown());
            const auto active = IlanaTheme::fade (*this, 1000, hoverIndex == overflowIndex ? 1.0f : 0.0f, IlanaTheme::FadeRate::hover);
            // A pill, a little wider than a badge, so "+N" has air inside its outline.
            const auto pill = area.expanded (mini ? 3.0f : 2.0f, 0.0f);
            g.setColour (IlanaTheme::Ui::raised.interpolatedWith (juce::Colours::white, 0.15f * active));
            g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
            g.setColour (IlanaTheme::Ui::text2);
            g.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);
            g.setColour (IlanaTheme::Ui::text);
            g.setFont (IlanaTheme::font (IlanaTheme::TextSize::tiny, true));
            IlanaTheme::drawFitted (g, "+" + juce::String ((int) dots.size() - numShown()), pill.reduced (2.0f, 0.0f).translated (0.0f, 0.5f).toNearestInt(),
                              juce::Justification::centred, 1);
        }
    }

    void mouseMove (const juce::MouseEvent& event) override
    {
        const auto index = indexAt (event.position);

        if (index != hoverIndex)
        {
            hoverIndex = index;
            setMouseCursor (index >= 0 ? juce::MouseCursor::UpDownResizeCursor
                                       : index == overflowIndex ? juce::MouseCursor::PointingHandCursor
                                                                : juce::MouseCursor::NormalCursor);
            updateTooltip();
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverIndex = -1;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        const auto index = indexAt (event.position);

        if (index == overflowIndex)
        {
            if (onShowAll != nullptr)
                onShowAll();
            return;
        }

        if (event.mods.isPopupMenu())
        {
            showDotMenu (index);
            return;
        }

        dragIndex = index;

        if (dragIndex >= 0)
        {
            dragStartDepth = dots[(size_t) dragIndex].depth;

            if (onDragStart != nullptr)
                onDragStart (dots[(size_t) dragIndex].source);
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (dragIndex < 0)
            return;

        const auto fine = event.mods.isShiftDown() ? 0.2f : 1.0f;
        const auto depth = juce::jlimit (-1.0f, 1.0f, dragStartDepth - (float) event.getDistanceFromDragStartY() * 0.008f * fine);
        dots[(size_t) dragIndex].depth = depth;
        updateTooltip();
        repaint();

        if (onDepthChange != nullptr)
            onDepthChange (dots[(size_t) dragIndex].slot, depth);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragIndex >= 0 && onDragEnd != nullptr)
            onDragEnd();

        dragIndex = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        const auto index = indexAt (event.position);

        if (index >= 0 && ! event.mods.isPopupMenu() && onZero != nullptr)
            onZero (dots[(size_t) index].slot);
    }

    // Rings one routing's badge (a source dropped on a knob it already
    // drives points at the existing routing instead of adding a second).
    void flashSlot (int slot)
    {
        for (int i = 0; i < numShown(); ++i)
            if (dots[(size_t) i].slot == slot)
            {
                hoverIndex = i;
                updateTooltip();
                repaint();
            }
    }

    // White text with a dark outline (a macro's number on a yellow pie).
    static void paintOutlinedText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, float size)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addFittedText (juce::Font (IlanaTheme::font (size, true)), text, area.getX(), area.getY() + 0.5f,
                              area.getWidth(), area.getHeight(), juce::Justification::centred, 1);
        juce::Path glyph;
        glyphs.createPath (glyph);
        g.setColour (IlanaTheme::Ui::bg.withAlpha (0.85f));
        g.strokePath (glyph, juce::PathStrokeType (1.8f));
        g.setColour (juce::Colours::white);
        g.fillPath (glyph);
    }

private:
    static constexpr int overflowIndex = -2;

    int numShown() const
    {
        const auto count = (int) dots.size();
        // (A strip with room for two shows the "+N" alone, not a disc over a pill: one mark on a small knob, P-2.)
        return count > maxVisible ? (maxVisible <= 2 ? 0 : maxVisible - 1) : count;
    }

    void showDotMenu (int index)
    {
        if (! juce::isPositiveAndBelow (index, (int) dots.size()))
            return;

        const auto dot = dots[(size_t) index];
        juce::PopupMenu menu;
        addRouteItems (menu, dot);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                .withTargetScreenArea (localAreaToGlobal (dotBounds (index).expanded (2.0f).getSmallestIntegerContainer())),
                            [safeThis = juce::Component::SafePointer<ModDotStrip> (this), dot] (int result)
                            {
                                if (safeThis != nullptr)
                                    safeThis->handleRouteChoice (result, dot);
                            });
    }

    juce::Rectangle<float> dotBounds (int index) const
    {
        return juce::Rectangle<float> ((float) diameter(), (float) diameter())
            .withCentre ({ (float) getWidth() * 0.5f, (float) (index * pitchPx()) + (float) pitchPx() * 0.5f });
    }

    // A badge, overflowIndex for "+N", or -1. Each takes a 20 px target
    // (its own row's share of it where they touch).
    int indexAt (juce::Point<float> position) const
    {
        const auto rows = numShown() + (hasOverflow() ? 1 : 0);
        const auto row = (int) std::floor (position.y / (float) pitchPx());

        if (! juce::isPositiveAndBelow (row, rows) || std::abs (position.x - (float) getWidth() * 0.5f) > 10.0f)
            return -1;

        return row < numShown() ? row : overflowIndex;
    }

    void updateTooltip()
    {
        const auto index = hoverIndex >= 0 ? hoverIndex : dragIndex;

        if (hoverIndex == overflowIndex)
        {
            setTooltip ("Show all " + juce::String ((int) dots.size()) + " routings into this knob");
            return;
        }

        if (! juce::isPositiveAndBelow (index, (int) dots.size()))
        {
            setTooltip ("Modulation\nDrag a badge (or the knob's ring) to set its depth, double-click it to zero it, right-click for polarity, stereo, bypass or remove.");
            return;
        }

        const auto& dot = dots[(size_t) index];
        setTooltip (ModNames::source (dot.source) + "  " + (dot.depth >= 0.0f ? "+" : "") + juce::String (Mod::shownPercent (dot.depth, dot.bipolar))
                    + "%" + (dot.bipolar ? "  (swings both ways: 100% = the whole range)" : "") + (dot.bypass ? "  (bypassed)" : "")
                    + (dot.stereo ? "  (stereo: left up, right down)" : "")
                    + "\nDrag up or down to set the depth (Shift for fine), double-click to zero it, "
                      "right-click for unipolar, bipolar, stereo, bypass or remove.");
    }

    std::vector<Dot> dots;
    int dragIndex = -1;
    int hoverIndex = -1;
    int maxVisible = 3;
    bool mini = false;
    float dragStartDepth = 0.0f;
};

// The colour a knob draws a modulation arc in: the source's own, unless it
// is too close in hue to the knob's colour, then lifted towards white.
inline juce::Colour modArcColour (juce::Colour source, juce::Colour knob)
{
    auto hueGap = std::abs (source.getHue() - knob.getHue());
    hueGap = juce::jmin (hueGap, 1.0f - hueGap);
    const auto bothColoured = source.getSaturation() > 0.3f && knob.getSaturation() > 0.3f;
    return bothColoured && hueGap < 0.06f ? source.interpolatedWith (juce::Colours::white, 0.6f) : source;
}

// MIDI learn for any automatable control (macros keep their own CC slots):
// the next controller moved drives the parameter; saved with the patch.
// Menu item IDs 4001-4003 are taken by these.
inline void addMidiLearnItems (juce::PopupMenu& menu, const IlanaSynthAudioProcessor& processor, const juce::String& parameterId)
{
    const auto cc = processor.getParamCc (parameterId);

    if (processor.getParamLearnTarget() == parameterId)
        menu.addItem (4003, "Cancel MIDI Learn  (waiting for a controller)");
    else
        menu.addItem (4001, cc >= 0 ? "MIDI Learn  (CC " + juce::String (cc) + ")" : juce::String ("MIDI Learn"));

    if (cc >= 0)
        menu.addItem (4002, "Clear MIDI  (CC " + juce::String (cc) + ")");
}

// True when the menu result was one of addMidiLearnItems' (and is done).
inline bool handleMidiLearnResult (int result, IlanaSynthAudioProcessor& processor, const juce::String& parameterId)
{
    if (result == 4001)
        processor.startParamLearn (parameterId);
    else if (result == 4002)
        processor.clearParamCc (parameterId);
    else if (result == 4003)
        processor.cancelParamLearn();
    else
        return false;

    return true;
}

// A one-line note that floats over a control for a moment and fades (a
// source dropped on a knob that can't take it, I7-5). One at a time; it
// lives in the editor's scaled content and goes with it.
class FloatingNote : public juce::Component,
                     private juce::ComponentListener,
                     private IlanaAnim::FrameTimer
{
public:
    static void show (juce::Component& anchor, const juce::String& text)
    {
        // The anchor's ancestor just under the top level: the editor's
        // content, drawn at the zoom.
        juce::Component* host = &anchor;
        while (host->getParentComponent() != nullptr && host->getParentComponent()->getParentComponent() != nullptr)
            host = host->getParentComponent();
        if (host == &anchor)
            return;

        delete current().getComponent();
        auto* note = new FloatingNote (text, *host);
        current() = note;

        const auto font = juce::Font (IlanaTheme::font (IlanaTheme::TextSize::body, true));
        const auto width = juce::GlyphArrangement::getStringWidthInt (font, text) + 24;
        const auto at = host->getLocalArea (&anchor, anchor.getLocalBounds());
        auto bounds = juce::Rectangle<int> (width, 26).withCentre ({ at.getCentreX(), at.getY() - 15 });
        bounds = bounds.constrainedWithin (host->getLocalBounds().reduced (4));
        note->setBounds (bounds);
    }

    // The note on screen now, for the tests (empty when none).
    static juce::String shownText() { return current() != nullptr ? current()->text : juce::String(); }

    ~FloatingNote() override
    {
        if (host != nullptr)
            host->removeComponentListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (IlanaTheme::Ui::raised);
        g.fillRoundedRectangle (bounds, 6.0f);
        g.setColour (IlanaTheme::Ui::line);
        g.drawRoundedRectangle (bounds, 6.0f, 1.0f);
        g.setColour (IlanaTheme::Ui::text);
        g.setFont (IlanaTheme::font (IlanaTheme::TextSize::body, true));
        g.drawText (text, getLocalBounds(), juce::Justification::centred, false);
    }

private:
    FloatingNote (const juce::String& textIn, juce::Component& hostIn) : text (textIn), host (&hostIn)
    {
        setInterceptsMouseClicks (false, false);
        host->addAndMakeVisible (this);
        host->addComponentListener (this);
        toFront (false);
        startTimerHz (30);
    }

    static juce::Component::SafePointer<FloatingNote>& current()
    {
        static juce::Component::SafePointer<FloatingNote> note;
        return note;
    }

    void timerCallback() override
    {
        life -= frameSeconds();
        setAlpha (juce::jlimit (0.0f, 1.0f, life / 0.4f));
        if (life <= 0.0f)
        {
            stopTimer();
            setVisible (false);
            juce::MessageManager::callAsync ([note = juce::Component::SafePointer<FloatingNote> (this)] { delete note.getComponent(); });
        }
    }

    void componentBeingDeleted (juce::Component&) override
    {
        host->removeComponentListener (this);
        host = nullptr;
        stopTimer();
        delete this;
    }

    juce::String text;
    juce::Component* host = nullptr;
    float life = 2.2f;
};

// The knob's slider with Vital's drag feel: vertical only (the stock "rotary
// horizontal + vertical" style adds the sideways movement in, so a wobbling
// hand moves the value), 200 px for the whole range, linear in the
// parameter's normalised value (skewed knobs feel even), Shift (or Ctrl /
// Cmd) = 10x finer from where it is held, the cursor hidden and unbounded
// while turning so a drag never runs out of screen. Ctrl-click resets to
// the default (the double-click return value, set by KnobControl).
// A slider the keyboard can reach (Tab) and drive: Up / Right and Down / Left
// step the value by 1 % of its range (the interval, for a stepped one),
// Shift by 0.1 %, Page Up / Down by 10 %. A click does not take the keys
// from the editor, so the shortcuts and the note keys keep working; a focus
// ring is drawn by the look and feel while the keys are here (G-10).
// A page that scrolls (the OSC page in a short window) must scroll when the
// wheel or a touchpad sweeps over a knob or menu too, not change it (ilana:
// scrolling did nothing on a touchpad, the pointer being over controls
// nearly everywhere). Hold Ctrl / Cmd to turn the control instead. A page that
// does not scroll leaves the wheel to the control as before.
inline bool scrollPageInstead (juce::Component& control, const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (event.mods.isCommandDown() || event.mods.isCtrlDown())
        return false;

    auto* viewport = control.findParentComponentOfClass<juce::Viewport>();
    if (viewport == nullptr || viewport->getViewedComponent() == nullptr
        || viewport->getViewedComponent()->getHeight() <= viewport->getMaximumVisibleHeight())
        return false;

    viewport->mouseWheelMove (event.getEventRelativeTo (viewport), wheel);
    return true;
}

class KeyboardSlider : public juce::Slider
{
public:
    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        if (! scrollPageInstead (*this, event, wheel))
            juce::Slider::mouseWheelMove (event, wheel);
    }

    KeyboardSlider()
    {
        setWantsKeyboardFocus (true);
        setMouseClickGrabsKeyboardFocus (false);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto code = key.getKeyCode();
        const auto mods = key.getModifiers();
        const auto page = code == juce::KeyPress::pageUpKey || code == juce::KeyPress::pageDownKey;
        const auto up = page ? code == juce::KeyPress::pageUpKey : (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey);
        const auto down = page ? code == juce::KeyPress::pageDownKey : (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey);

        if (! isEnabled() || (! up && ! down) || mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
            return juce::Slider::keyPressed (key);

        const auto direction = up ? 1.0 : -1.0;

        if (getInterval() > 0.0)
        {
            setValue (getValue() + direction * getInterval() * (page ? 10.0 : 1.0), juce::sendNotificationSync);
        }
        else
        {
            const auto step = page ? 0.1 : (mods.isShiftDown() ? 0.001 : 0.01);
            setValue (proportionOfLengthToValue (juce::jlimit (0.0, 1.0, valueToProportionOfLength (getValue()) + direction * step)),
                      juce::sendNotificationSync);
        }

        return true;
    }

    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
};

class KnobSlider : public KeyboardSlider
{
public:
    static constexpr float fullRangePixels = 200.0f;
    static constexpr float fineDivisor = 10.0f;

    KnobSlider() { setSliderStyle (juce::Slider::RotaryVerticalDrag); }

    static bool wantsFine (const juce::ModifierKeys& mods) { return mods.isShiftDown() || mods.isCommandDown(); }

    void mouseDown (const juce::MouseEvent& event) override
    {
        juce::Slider::mouseDown (event);

        // (a popup-menu press or a ctrl-click reset is not the start of a drag)
        dragging = isEnabled() && ! event.mods.isPopupMenu() && ! (event.mods.withoutMouseButtons() == juce::ModifierKeys::ctrlModifier);
        lastY = event.position.y;
        if (auto* owner = getParentComponent()) // (the grab: the value grows, the name turns white)
            owner->repaint();
        proportion = valueToProportionOfLength (getValue());

        // Where the drag began, for the look-and-feel's ghost tick.
        if (dragging)
        {
            getProperties().set ("dragGhostPos", (float) proportion);
            getProperties().set ("dragGhostRelease", 0.0);
        }

        setFine (dragging && wantsFine (event.mods));
    }

    // A double-click reset glides back to the default like any value that
    // moves by itself (the look-and-feel glides while this is recent).
    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        getProperties().set ("resetGlideAt", juce::Time::getMillisecondCounterHiRes());
        juce::Slider::mouseDoubleClick (event);
    }

    // Shift or Ctrl pressed or let go mid-drag: the knob's name says FINE.
    void modifierKeysChanged (const juce::ModifierKeys& mods) override
    {
        juce::Slider::modifierKeysChanged (mods);
        setFine (dragging && wantsFine (mods));
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (! dragging)
            return;

        setFine (wantsFine (event.mods));
        const auto step = (lastY - event.position.y) / fullRangePixels / (wantsFine (event.mods) ? fineDivisor : 1.0f);
        lastY = event.position.y;
        proportion = juce::jlimit (0.0, 1.0, proportion + (double) step);
        setValue (proportionOfLengthToValue (proportion), juce::sendNotificationSync);

        if (event.source.canDoUnboundedMovement())
            event.source.enableUnboundedMouseMovement (true, false);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (dragging && getProperties().contains ("dragGhostPos"))
        {
            getProperties().set ("dragGhostRelease", juce::Time::getMillisecondCounterHiRes());
            repaint();
        }

        dragging = false;
        setFine (false);
        juce::Slider::mouseUp (event);
        event.source.enableUnboundedMouseMovement (false);
        if (auto* owner = getParentComponent())
            owner->repaint();
    }

    bool isGrabbed() const { return dragging; }
    // Dragging at a tenth of the speed (Shift or Ctrl held).
    bool isFine() const { return fine; }
    std::function<void()> onFineChanged;

private:
    void setFine (bool shouldBeFine)
    {
        if (shouldBeFine == fine)
            return;

        fine = shouldBeFine;
        if (onFineChanged != nullptr)
            onFineChanged();
    }

    bool dragging = false, fine = false;
    float lastY = 0.0f;
    double proportion = 0.0;
};

class KnobControl : public juce::Component,
                    public juce::DragAndDropTarget,
                    public juce::SettableTooltipClient,
                    private IlanaAnim::FrameTimer
{
public:
    KnobControl (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID,
                 const juce::String& labelText, juce::Colour accent = IlanaTheme::accent(),
                 bool followsThemeIn = true)
        : ringConfig (modRingConfigFor (parameterID)),
          parameterId (parameterID),
          knobAccent (accent),
          followsTheme (followsThemeIn)
    {
        processorRef = dynamic_cast<IlanaSynthAudioProcessor*> (&state.processor);

        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 14);
        slider.setPopupDisplayEnabled (true, true, nullptr);
        slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
        addAndMakeVisible (slider);
        slider.addMouseListener (this, false);

        label.setText (labelText, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setBorderSize ({ 0, 1, 0, 1 }); // (a name takes its whole cell: layoutRow fits it by that)
        // Bold, letter-spaced caps names as drawn in the mockup.
        // (The sheet's .kl: 700, tracked 0.08 em, in t3.)
        label.setFont (IlanaTheme::font (IlanaTheme::TextSize::label, true).withKerningFactor (0.08f));
        label.setColour (juce::Label::textColourId, IlanaTheme::Ui::text3);
        addAndMakeVisible (label);

        // Fine drag (Shift or Ctrl held): the name reads FINE in the accent
        // until the key or the mouse is let go, so the slow knob doesn't
        // feel stuck. (The label keeps its text; its ink hides and FINE is
        // drawn over it.)
        slider.onFineChanged = [this]
        {
            label.setColour (juce::Label::textColourId, slider.isFine() ? juce::Colours::transparentBlack : IlanaTheme::Ui::text3);
            repaint();
        };

        dotStrip.onDepthChange = [this] (int slot, float depth)
        {
            if (processorRef != nullptr)
                processorRef->setModSlotValue (slot, "amt", Mod::storedDepth (processorRef->readModSlot (slot), depth));
        };
        dotStrip.onZero = [this] (int slot)
        {
            if (processorRef != nullptr)
                processorRef->performEdit ("Zero " + ModNames::source ((int) processorRef->readModSlot (slot).source) + " depth",
                                           [this, slot] { processorRef->setModSlotValue (slot, "amt", 0.0f); });

            refreshRoutings();
        };
        dotStrip.onRemove = [this] (int slot)
        {
            if (processorRef != nullptr)
                processorRef->performEdit ("Remove modulation", [this, slot] { processorRef->clearModSlot (slot); });

            refreshRoutings();
        };
        dotStrip.onBypass = [this] (int slot, bool bypass)
        {
            if (processorRef != nullptr)
                processorRef->performEdit (bypass ? "Bypass modulation" : "Enable modulation",
                                           [this, slot, bypass] { processorRef->setModSlotValue (slot, "byp", bypass ? 1.0f : 0.0f); });

            refreshRoutings();
        };
        dotStrip.onPolarity = [this] (int slot, int polarity) { setRoutePolarity (slot, polarity); };
        dotStrip.onStereo = [this] (int slot, bool stereo)
        {
            if (processorRef != nullptr)
                processorRef->performEdit (stereo ? "Stereo modulation" : "Mono modulation",
                                           [this, slot, stereo] { processorRef->setModSlotValue (slot, "stereo", stereo ? 1.0f : 0.0f); });

            refreshRoutings();
        };
        dotStrip.onTypeAmount = [this] (int slot) { beginAmountEntry (slot); };
        dotStrip.onDragStart = [this] (int source)
        {
            if (processorRef != nullptr)
                processorRef->beginEdit (ModNames::source (source) + " depth");
        };
        dotStrip.onDragEnd = [this]
        {
            if (processorRef != nullptr)
                processorRef->endEdit();
        };
        dotStrip.onShowAll = [this] { openModCard (true); };
        addChildComponent (dotStrip);
        // Over the slider, so a press on a ring sets that routing's depth
        // (one inside it still turns the knob).
        addChildComponent (ringOverlay);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, parameterID, slider);

        parameter = state.getParameter (parameterID);

        if (parameter != nullptr)
        {
            // What a screen reader says for the dial.
            slider.setTitle (parameter->getName (64));
            slider.setDescription (describeParameter (parameterID));
            slider.setDoubleClickReturnValue (true, parameter->convertFrom0to1 (parameter->getDefaultValue()), juce::ModifierKeys::ctrlModifier);

            applyModulatableMark();
        }

        lastSliderValue = slider.getValue();
        refreshRoutings();
        startTimerHz (30);
    }

    void lookAndFeelChanged() override
    {
        if (followsTheme)
            slider.setColour (juce::Slider::rotarySliderFillColourId, IlanaTheme::accent());
    }

    // Gives the knob a fixed identity colour (an oscillator's, say) in place
    // of the theme accent.
    void setIdentityColour (juce::Colour colour)
    {
        knobAccent = colour;
        followsTheme = false;
        slider.setColour (juce::Slider::rotarySliderFillColourId, colour);
        repaint();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        closeModCard();

        if (event.mods.isPopupMenu())
        {
            startPulse (IlanaTheme::accent());
            showModMenu();
        }
    }

    juce::Slider& getSlider() { return slider; }
    void setLabelText (const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        resized(); // a knob with a label lays out differently
    }
    juce::String getLabelText() const { return label.getText(); }
    juce::Label& getNameLabel() { return label; }
    bool isCompact() const { return compact; }
    const juce::String& getParameterId() const { return parameterId; }
    // The mod destination its ring shows (0: none), so the tabs can count
    // a source's routes per page.
    int getModDestination() const { return ringConfig.destination; }
    // Undo or redo moved this knob: it flashes once, as a preset load does.
    void flashChanged() { startPulse (juce::Colours::white.withAlpha (0.9f)); }
    // The modulation destination whose depth the knob's ring shows (0: none).
    int getRingDestination() const { return ringConfig.destination; }
    int getNumRoutings() const { return (int) routings.size(); }
    const ModDotStrip::Dot& getRouting (int index) const { return routings[(size_t) index]; }
    // The depth rings (one per routing, up to three), for the tests: how
    // many, a point on one (0..1 along its sweep, local to the knob), the
    // component that takes presses on them, and the badge strip.
    int getNumRings() const { return numRings(); }
    juce::Point<float> getRingPoint (int ring, float proportion) const
    {
        const auto angle = ringStart + (ringEnd - ringStart) * proportion;
        return dialCentre().getPointOnCircumference (ringRadius (ring), angle);
    }
    juce::Component& getRingOverlay() { return ringOverlay; }
    // The drawn dial's radius and a ring's, about the dial's centre.
    float getDialRadius() const { return dialRadius(); }
    float getRingRadius (int ring) const { return ringRadius (ring); }
    juce::Point<float> getDialCentre() const { return dialCentre(); }
    // Where the name's line of letters draws (local; empty without a name),
    // and the box the drawn rings cover with their strokes: the cut-text
    // test keeps both inside the knob and its views, and apart.
    juce::Rectangle<float> getNameTextBounds() const
    {
        if (! label.isVisible() || label.getText().isEmpty() || label.getWidth() <= 0)
            return {};

        auto& lf = label.getLookAndFeel();
        const auto font = lf.getLabelFont (const_cast<juce::Label&> (label));
        const auto area = lf.getLabelBorderSize (const_cast<juce::Label&> (label)).subtractedFrom (label.getLocalBounds()).toFloat();
        juce::GlyphArrangement glyphs;
        glyphs.addFittedText (font, label.getText(), area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                              label.getJustificationType(), 1, 1.0f);
        return glyphs.getBoundingBox (0, -1, true).translated ((float) label.getX(), (float) label.getY());
    }
    // The ink of the name's letters (local; empty without a name): the
    // rings keep 1 px of air under it, not under the label's whole line.
    juce::Rectangle<float> getNameInkBounds() const
    {
        if (! label.isVisible() || label.getText().isEmpty() || label.getWidth() <= 0)
            return {};

        const auto key = label.getText() + label.getBounds().toString() + juce::String (label.getFont().getHeight());

        if (key != nameInkKey)
        {
            auto& lf = label.getLookAndFeel();
            const auto font = lf.getLabelFont (const_cast<juce::Label&> (label));
            const auto area = lf.getLabelBorderSize (const_cast<juce::Label&> (label)).subtractedFrom (label.getLocalBounds()).toFloat();
            juce::GlyphArrangement glyphs;
            glyphs.addFittedText (font, label.getText(), area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                                  label.getJustificationType(), 1, 1.0f);
            juce::Path ink;
            glyphs.createPath (ink);
            nameInk = ink.getBounds().translated ((float) label.getX(), (float) label.getY());
            nameInkKey = key;
        }

        return nameInk;
    }
    juce::Rectangle<float> getRingsBounds() const
    {
        if (numRings() == 0 || knobRadiusFor (knobBounds) < 8.0f)
            return {};

        // The sweep passes the top and both sides; its ends sit 0.81 r below the centre.
        const auto r = ringRadius (numRings() - 1) + 1.0f; // (the stroke's outer half)
        const auto centre = dialCentre();
        return juce::Rectangle<float>::leftTopRightBottom (centre.x - r, centre.y - r, centre.x + r, centre.y + r * 0.81f);
    }
    // (The timer skips knobs that aren't on screen, as in the offscreen tests.)
    void syncRoutings() { refreshRoutings(); }
    ModDotStrip& getDotStrip() { return dotStrip; }
    // Shows the badges as a hover does (the tests); false leaves them to the mouse.
    void setBadgesShown (bool shown)
    {
        badgesForced = shown;
        badgesShown = shown && ! routings.empty();
        layoutDots();
    }
    bool areBadgesShown() const { return dotStrip.isVisible(); }
    // Whether a mod slot routes this source into the knob, and whether the
    // source pinned by a chip click is one of them (the knob is lit).
    bool isDrivenBy (int source) const { return routesFrom (source); }
    // The source a drag under way carries, as the knob last saw it (V8-10;
    // the tests set it with showDragTarget).
    int getShownDragSource() const { return dragSource; }
    void showDragTarget (int source) { dragSource = source; repaint(); }
    bool isLitByPinnedSource() const { return pinnedModSource() != 0 && routesFrom (pinnedModSource()); }

    // Compact knobs (bottom strip) have no label or value box: the owner
    // draws those next to the knob.
    void setCompact (bool shouldBeCompact)
    {
        compact = shouldBeCompact;
        label.setVisible (! compact);
        slider.setTextBoxStyle (compact ? juce::Slider::NoTextBox : juce::Slider::TextBoxBelow, false, 64, 14);
        resized();
    }

    // An inline knob (the design's WEST / BODY row and the signal-flow strip's
    // BALANCE): the dial at the left, its name and value stacked beside it.
    // The dial is the compact knob (no text box under it); the name is the
    // label, the value is drawn here.
    void setInline (bool shouldInline)
    {
        inlineText = shouldInline;
        setCompact (shouldInline);
        if (shouldInline)
        {
            label.setVisible (true);
            label.setJustificationType (juce::Justification::centredLeft);
            label.setBorderSize ({ 0, 0, 0, 0 });
            slider.onValueChange = [this] { repaint(); };
        }
        resized();
    }

    bool isInline() const { return inlineText; }

    // The knob's role sets its largest dial (IlanaTheme::KnobSize: main,
    // small or mini); layoutRow and preferredControlHeight follow it.
    void setSizeRole (int largestDial)
    {
        maxDial = juce::jmax (IlanaTheme::KnobSize::minimum, largestDial);
        resized();
    }

    // Inline (the OSC cards and the voice strip): the dial at the left with
    // the name and the value stacked to its right, in a cell one dial high.
    // The layout is the look-and-feel's (getSliderLayout) for a slider with
    // the "inlineKnob" property.
    void setInlineKnob (bool shouldBeInline)
    {
        if (inlineLayout == shouldBeInline)
            return;

        inlineLayout = shouldBeInline;
        slider.getProperties().set ("inlineKnob", inlineLayout);
        slider.setTextBoxStyle (inlineLayout ? juce::Slider::TextBoxRight : juce::Slider::TextBoxBelow, false, 60, 14);
        label.setJustificationType (inlineLayout ? juce::Justification::centredLeft : juce::Justification::centred);
        // (Back from inline, the name is the sheet's .kl again, as the
        // constructor sets it: it used to come back in the body size,
        // unbolded, so one strip's names were bigger than the next's.)
        label.setFont (inlineLayout ? IlanaTheme::font (IlanaTheme::TextSize::label)
                                    : IlanaTheme::font (IlanaTheme::TextSize::label, true).withKerningFactor (0.08f));
        IlanaTheme::styleInlineValueBox (slider, inlineLayout);
        resized();
    }

    bool isInlineKnob() const { return inlineLayout; }

    // Packed (the OSC cards' panels, review 23): a dial of `dial` units with its
    // value written inside it and its name under it, in a cell `dial` wide plus
    // the room the knob needs round it (packedPadX each side, packedPadTop above,
    // for the modulation rings). 0 turns it off. Text is painted here through
    // font(), so it holds the 10 px screen floor at every zoom; the name Label
    // and the slider's text box stay empty and hidden.
    static constexpr int packedPadX = 3, packedPadTop = 6;
    static constexpr float kPackedValueScale = 0.37f; // number height / dial size

    void setPackedDial (float dial)
    {
        if (juce::approximatelyEqual (packedD, dial))
            return;

        packedD = dial;
        if (packedD > 0.0f)
            slider.getProperties().set ("packedDial", packedD);
        else
            slider.getProperties().remove ("packedDial");
        slider.setTextBoxStyle (packedD > 0.0f ? juce::Slider::NoTextBox : (inlineLayout ? juce::Slider::TextBoxRight : juce::Slider::TextBoxBelow), false, 60, 14);
        label.setVisible (packedD <= 0.0f);
        resized();
        repaint();
    }

    bool isPacked() const { return packedD > 0.0f; }
    float getPackedDial() const { return packedD; }
    // The packed name's size in units, and on screen in device pixels (the text floor test's measure).
    juce::Rectangle<int> getPackedNameArea() const { return packedNameArea; }
    float packedNameUnits() const { return packedNameSize > 0.0f ? packedNameSize : juce::jlimit (7.6f, 8.6f, 7.6f * packedD / 26.0f); }
    float packedNameDevicePx() const { return IlanaTheme::font (packedNameUnits(), true).getHeight() * IlanaTheme::uiScaleRef(); }
    // The packed number's height on screen in device pixels, and the dial's (a layout test's measure:
    // the number keeps its share of the dial at every zoom).
    float packedNumberDevicePx() const { return IlanaTheme::font (packedD * kPackedValueScale, true, true).getHeight() * IlanaTheme::uiScaleRef(); }
    float packedDialDevicePx() const { return packedD * IlanaTheme::uiScaleRef(); }
    // A packed name too wide for its cell breaks at its first space onto a second line ("KEY" over "TRACK"),
    // in full words; the cell the page gives the knob must leave the room (about 9 px under the name).
    void setPackedNameWrap (bool wrap, bool always = false)
    {
        packedNameWrap = wrap;
        packedNameAlwaysWraps = wrap && always; // (a row whose two-word names all break, as the OSC strip's KEYS)
        repaint();
    }
    // The packed name's size, when a row wants it bigger than the dial's own (the OSC strip's 10 px); 0: the dial's.
    void setPackedNameSize (float size)
    {
        packedNameSize = size;
        repaint();
    }
    // Pixels a packed value may run past the dial's width before its unit is dropped ("1.20 kHz").
    void setPackedValueRoom (int pixels)
    {
        packedValueRoom = pixels;
        repaint();
    }
    // The whole cell a packed knob takes for a dial (the page sizes it with this).
    // The cell a packed knob stands in, without the room round it for the rings (a layout test's measure).
    juce::Rectangle<int> packedCell() const { return isPacked() ? getLocalBounds().reduced (packedPadX, 0).withTrimmedTop (packedPadTop) : getLocalBounds(); }
    static juce::Rectangle<int> packedBoundsFor (juce::Rectangle<int> cell) { return cell.expanded (packedPadX, 0).withTrimmedTop (-packedPadTop); }
    // PLAY's ring knob in a page's cell (one knob style, UI review 26): the dial grows with the cell's
    // height up to `maxDial`, the value inside it, the name under it at the menus' label size.
    void placeAsRing (juce::Rectangle<int> area, int maxDial = 46)
    {
        // (The room round the ring for the modulation rings stays inside the area: a row's knobs never overlap the next's.)
        const auto inner = area.reduced (packedPadX, 0).withTrimmedTop (packedPadTop);
        const auto dial = (float) juce::jlimit (26, juce::jmax (26, maxDial), juce::roundToInt ((float) inner.getHeight() / 1.42f));
        setPackedDial (dial);
        setPackedValueRoom (12);
        setPackedNameSize (juce::jlimit (11.5f, 12.5f, dial * 0.28f));
        const auto width = juce::jmin (inner.getWidth(), juce::roundToInt (dial) + 40);
        const auto height = juce::jmin (inner.getHeight(), juce::roundToInt (dial * 1.42f));
        setBounds (packedBoundsFor (inner.withSizeKeepingCentre (width, height)));
    }
    // Opens the value for typing over the dial (double-click, as the text box did).
    void showValueEditor()
    {
        if (! isPacked() || valueEditor != nullptr || ! isEnabled())
            return;

        valueEditor = std::make_unique<juce::TextEditor>();
        auto& editor = *valueEditor;
        editor.setFont (IlanaTheme::font (IlanaTheme::TextSize::label, true, true));
        editor.setJustification (juce::Justification::centred);
        editor.setBorder ({ 0, 2, 0, 2 });
        editor.setIndents (0, 0);
        editor.setColour (juce::TextEditor::backgroundColourId, IlanaTheme::Ui::bg);
        editor.setColour (juce::TextEditor::textColourId, IlanaTheme::Ui::text);
        editor.setColour (juce::TextEditor::outlineColourId, knobAccent);
        editor.setColour (juce::TextEditor::focusedOutlineColourId, knobAccent);
        editor.setText (slider.getTextFromValue (slider.getValue()), false);
        const auto width = juce::jmax (knobBounds.getWidth() + 10, 54);
        editor.setBounds (juce::Rectangle<int> (width, 16).withCentre (knobBounds.getCentre()));
        editor.onReturnKey = [this]
        {
            if (valueEditor != nullptr)
                slider.setValue (slider.getValueFromText (valueEditor->getText()), juce::sendNotificationSync);
            closeValueEditor();
        };
        editor.onEscapeKey = [this] { closeValueEditor(); };
        editor.onFocusLost = [this] { closeValueEditor(); };
        addAndMakeVisible (editor);
        editor.grabKeyboardFocus();
        editor.selectAll();
    }

    void closeValueEditor()
    {
        if (valueEditor == nullptr)
            return;
        auto old = std::move (valueEditor);
        removeChildComponent (old.get());
        // (Deleted after the callback that closed it returns.)
        juce::MessageManager::callAsync ([keep = std::make_shared<std::unique_ptr<juce::TextEditor>> (std::move (old))] { keep->reset(); });
        repaint();
    }

    // A double-click puts the knob back to its default (the slider's own
    // double-click return, as in Vital and Serum 2; ilana 2026-10-28). A
    // typed value is the right-click menu's "Type a value...".
    void showTypedValueBox()
    {
        if (! isEnabled())
            return;

        if (isPacked())
            showValueEditor();
        else
            slider.showTextBox();
    }

    // A row mixing modulatable and fixed knobs: the fixed ones draw the same
    // smaller dial a ring-bearing inline knob does (S1-10, the VOICE strip).
    void setCapDialLikeRings (bool cap)
    {
        if (capDialLikeRings == cap)
            return;
        capDialLikeRings = cap;
        resized();
    }

    // The largest dial an inline knob draws (40 everywhere; the FX rack's rows
    // grow their dials with the row when the rack is short).
    void setInlineDial (int largest)
    {
        if (inlineDial == largest)
            return;
        inlineDial = largest;
        slider.getProperties().set ("inlineDial", largest); // (the look-and-feel's slider layout reads it)
        resized();
        slider.resized(); // (its bounds may not change, its layout does)
    }

    // A value drawn as a faded dash while `shown` says it does nothing (an
    // amount whose stage is Off): the knob keeps its place and its size.
    void setDashWhen (std::function<bool()> isIdle)
    {
        dashWhen = std::move (isIdle);
        const auto base = slider.textFromValueFunction;
        slider.textFromValueFunction = [this, base] (double value)
        {
            if (dashWhen != nullptr && dashWhen())
                return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94"));
            return base != nullptr ? base (value) : juce::String (value);
        };
        slider.updateText();
    }

    void refreshValueText() { slider.updateText(); }

    int getMaxDial() const { return maxDial; }
    // The label's line, and 3 px more under it for a knob that draws modulation
    // rings, so the rings clear the name (V10-18); 0 without a name.
    // (A knob that draws rings keeps 5 px under its name: room for three
    // rings over the dial without reaching the name, design sweep.)
    int labelBlockHeight() const { return label.getText().isEmpty() ? 0 : nameLineHeight() + (ringSpaced() ? 5 : 0); }
    // The name's line: 13, or the font's whole height when that is more (at
    // 75 % the screen floor makes the name taller than 13 units, and its
    // letters ran out of the top of the knob: ilana's PLAY strip).
    // (From the name's size step, not its font, whose height was snapped at
    // whatever zoom the knob was made in: every knob of a size gets one line.)
    int nameLineHeight() const
    {
        const auto size = label.getFont().getHeight() >= 13.8f ? IlanaTheme::TextSize::body : IlanaTheme::TextSize::label;
        return juce::jmax (13, (int) std::ceil (IlanaTheme::tallestAtAnyZoom (size) - 0.01f));
    }
    // The dial's drawn size right now (the UI test checks roles with it).
    int getDialSize() const { return knobBounds.getWidth() > 0 ? juce::jmin (knobBounds.getWidth(), (int) rotaryArea().getHeight()) : 0; }

    // A knob standing in for another parameter's modulation: a synced LFO's
    // division knob shows RATE's ring and dots, and a source dropped on it
    // (or picked from its menu) routes to RATE, which still scales the
    // synced rate.
    void setModulationTarget (const juce::String& targetParameterId)
    {
        ringConfig = modRingConfigFor (targetParameterId);
        ringOnOwnValue = targetParameterId == parameterId;
        applyModulatableMark();
        routings.clear();
        dotStrip.setDots (routings);
        refreshRoutings();
        layoutDots();
        repaint();
    }

    // Why the knob does nothing right now (EffectRules), added to its
    // tooltip; empty when it acts.
    void setInactiveNote (const juce::String& note)
    {
        if (note == inactiveNote || baseTooltip.isEmpty())
            return;

        inactiveNote = note;
        const auto tooltip = baseTooltip + (note.isNotEmpty() ? "\n(No effect now: " + note + ")" : juce::String());
        slider.setTooltip (tooltip);
        setTooltip (tooltip);
    }

    void mouseEnter (const juce::MouseEvent&) override
    {
        hover = true;
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hover = false;
        repaint();
    }

    ~KnobControl() override
    {
        closeModCard();
        // (A packed knob's "+N" may stand in the parent: take it back.)
        if (auto* holder = dotStrip.getParentComponent(); holder != nullptr && holder != this)
            holder->removeChildComponent (&dotStrip);
    }

    // The lifted "+N" follows the knob when it moves, hides or changes parent.
    void moved() override { if (dotStrip.getParentComponent() != this) layoutDots(); }
    void visibilityChanged() override { if (dotStrip.getParentComponent() != this) layoutDots(); }
    void parentHierarchyChanged() override
    {
        if (auto* holder = dotStrip.getParentComponent(); holder != nullptr && holder != this && holder != getParentComponent())
            addChildComponent (dotStrip);
    }

    // Opens the card listing this knob's sources (hover does it after a
    // short rest). Held: it stays until closeModCard (tests, snapshots).
    void openModCard (bool held = false)
    {
        modCardHeld = held;
        if (modHoverHooks().show != nullptr && ! routings.empty())
        {
            modHoverHooks().show (*this, ringConfig.destination, ModNames::destination (ringConfig.destination));
            modCardOpen = true;
        }
    }

    bool isModCardOpen() const { return modCardOpen; }

    void closeModCard()
    {
        if (modCardOpen && modHoverHooks().hide != nullptr)
            modHoverHooks().hide (*this);

        modCardOpen = false;
        modCardHeld = false;
        hoverRest = 0.0f;
        cardLeave = 0.0f;
    }



    void paint (juce::Graphics& g) override
    {
        const auto mod = processorRef != nullptr && ringConfig.destination != 0
                             ? processorRef->getModDisplay (ringConfig.destination)
                             : 0.0f;
        const auto modActive = std::abs (mod) > 0.001f;
        const auto glowColour = modActive ? modSourceColour (dominantSource) : IlanaTheme::accent();
        const auto glowIntensity = juce::jmax (glow * 0.09f, activity * 0.2f);
        const auto centre = rotaryArea().getCentre();
        const auto knobRadius = knobRadiusFor (knobBounds);

        if (glowIntensity > 0.005f)
        {
            for (int ring = 2; ring >= 1; --ring)
            {
                const auto radius = knobRadius + (float) ring * 3.5f;
                g.setColour (glowColour.withAlpha (glowIntensity * (ring == 2 ? 0.4f : 0.7f)));
                g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));
            }
        }

        // A hovered or pinned source lights up every knob it modulates: a
        // thin halo just outside the knob's rings, with no fill, so the
        // knob's label and value stay readable.
        const auto pinned = pinnedModSource();
        const auto highlighted = highlightedModSource() != 0 ? highlightedModSource() : pinned;

        if (highlighted != 0 && routesFrom (highlighted))
        {
            const auto radius = haloRadius();
            const juce::Graphics::ScopedSaveState clip (g);
            clipToHaloArea (g);
            g.setColour (modSourceColour (highlighted).withAlpha (highlighted == pinned ? 0.95f : 0.75f));
            g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (dialCentre()), highlighted == pinned ? 2.0f : 1.5f);
        }

        // While a source is dragged, every knob says whether it takes it:
        // a target ring in the source's colour, or dimmed (V8-10). The one
        // under the mouse lights up fully (below).
        if (dragSource != 0 && ! dragHover && ! sourceKnob)
        {
            if (isModulatable())
            {
                const auto radius = haloRadius();
                const juce::Graphics::ScopedSaveState clip (g);
                clipToHaloArea (g);
                g.setColour (modSourceColour (dragSource).withAlpha (0.5f));
                g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (dialCentre()), 1.5f);
            }
            else
            {
                g.setColour (IlanaTheme::Ui::bg.withAlpha (0.45f));
                g.fillRoundedRectangle (knobBounds.toFloat().reduced (2.0f), 6.0f);
            }
        }

        if (dragHover)
        {
            if (isModulatable() && dragSource != 0)
            {
                const auto radius = haloRadius();
                const juce::Graphics::ScopedSaveState clip (g);
                clipToHaloArea (g);
                g.setColour (modSourceColour (dragSource));
                g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (dialCentre()), 2.5f);
            }

            // A knob that can't take the source greys and is struck through.
            const auto area = knobBounds.toFloat().reduced (2.0f);
            g.setColour (isModulatable() ? juce::Colours::white.withAlpha (0.16f) : IlanaTheme::Ui::bg.withAlpha (0.55f));
            g.fillRoundedRectangle (area, 6.0f);
            if (! isModulatable())
            {
                const auto cross = juce::Rectangle<float> (16.0f, 16.0f).withCentre (rotaryArea().getCentre());
                g.setColour (IlanaTheme::Ui::text3);
                g.drawEllipse (cross, 1.6f);
                g.drawLine ({ cross.getBottomLeft() + juce::Point<float> (3.0f, -3.0f), cross.getTopRight() + juce::Point<float> (-3.0f, 3.0f) }, 1.6f);
            }
        }

        paintRings (g, highlighted);
        paintPulse (g);
    }

    // The knob's modulation rings (Vital and Serum 2 style): one concentric
    // ring per routing, outward from just outside the value arc, in the
    // source's colour. Each spans the range its routing can sweep from the
    // knob's value (both sides for a source that swings both ways), shows
    // what it adds right now as a brighter stretch with a white dot, and
    // ends in a handle at full depth. Drag a ring to set its depth.
    // The packed knob's value in its dial and name under it (the mockup's 8.5 px
    // and 7.5 px at 1.5x, held to the 10 px floor by font()). A value too wide
    // for the dial loses its unit ("-12 st" reads "-12"); the name says what it is.
    void paintPackedText (juce::Graphics& g) const
    {
        const auto on = isEnabled() ? 1.0f : 0.5f;
        auto& mutableSlider = const_cast<KnobSlider&> (slider);
        auto value = mutableSlider.getTextFromValue (mutableSlider.getValue());
        const auto inner = knobBounds.toFloat().reduced (packedD * 0.1f);
        // The number in one size for a dial of this size, as big as the ring holds (a unit or a % goes on a
        // second, smaller line under it: "300" over "ms", "80" over "%"; a value with no unit stays on one
        // line, four characters at most so it keeps clear of the ring: "x1.0", "+1.5"). The name under the dial
        // says what it is.
        auto unit = juce::String();
        if (value.endsWithChar ('%'))
        {
            unit = "%";
            value = value.dropLastCharacters (1);
        }
        else if (value.containsChar (' '))
        {
            unit = value.fromLastOccurrenceOf (" ", false, false);
            value = value.upToLastOccurrenceOf (" ", false, false);
            // ("20.0" kHz is "20": a whole number needs no decimal; a long unit word, "stages", is the name's job.)
            if (unit == "kHz" && value.endsWith (".0"))
                value = value.dropLastCharacters (2);
            if (unit.length() > 3)
                unit = {};
        }
        if (value.length() > 4 && value.containsChar ('.'))
            value = value.dropLastCharacters (value.length() - 4).trimCharactersAtEnd (".");
        // ("5.00" is "5.0": two decimals in one digit's dial came within 2 px of the ring, UI review 25.)
        if (value.length() == 4 && juce::CharacterFunctions::isDigit (value[0]) && value[1] == '.')
            value = juce::String (value.getDoubleValue(), 1);

        // Grabbed, the number grows a little and the name turns white; released, both settle back (~150 ms).
        const auto grab = IlanaTheme::fade (const_cast<KnobControl&> (*this), 77, slider.isGrabbed() ? 1.0f : 0.0f, 24.0f);
        g.setColour (IlanaTheme::Ui::text.withMultipliedAlpha (on));
        const auto drawn = IlanaTheme::font (packedD * kPackedValueScale * (1.0f + 0.07f * grab), true, true);
        g.setFont (drawn);
        if (unit.isNotEmpty())
        {
            const auto numberH = drawn.getHeight();
            const auto unitFont = drawn.withHeight (numberH * 0.78f);
            const auto total = numberH + unitFont.getHeight() - 1.0f;
            // (A pixel above the middle: the unit's foot keeps 3 px from the ring's end caps.)
            const auto top = inner.getCentreY() - total * 0.5f - 1.0f;
            const auto box = inner.toNearestInt().expanded (1, 0);
            IlanaTheme::drawFitted (g, value, juce::Rectangle<float> (box.getX(), top, box.getWidth(), numberH).toNearestInt(), juce::Justification::centred, 1);
            g.setFont (unitFont);
            g.setColour (IlanaTheme::Ui::text2.withMultipliedAlpha (on));
            IlanaTheme::drawFitted (g, unit, juce::Rectangle<float> (box.getX(), top + numberH - 1.0f, box.getWidth(), unitFont.getHeight()).toNearestInt(), juce::Justification::centred, 1);
        }
        else
            IlanaTheme::drawFitted (g, value, inner.toNearestInt().expanded (1, 0), juce::Justification::centred, 1);

        const auto fine = slider.isFine();
        const auto name = fine && label.getText().isNotEmpty() ? juce::String ("FINE") : label.getText();
        if (name.isNotEmpty() && ! packedNameArea.isEmpty())
        {
            g.setColour (fine ? IlanaTheme::accent()
                              : IlanaTheme::Ui::text3.interpolatedWith (IlanaTheme::Ui::text, grab).withMultipliedAlpha (on));
            // (Under the screen floor at a small zoom the same words are wider: no tracking, so neighbours' names keep apart.)
            const auto nameFont = IlanaTheme::font (packedNameUnits(), true);
            g.setFont (nameFont.withKerningFactor (nameFont.getHeight() > 6.2f ? 0.0f : 0.08f));
            const auto lineHeight = (int) std::ceil (nameFont.getHeight()) + 1;
            if (packedNameWrap && name.containsChar (' ')
                && (packedNameAlwaysWraps
                    || juce::GlyphArrangement::getStringWidth (juce::Font (nameFont), name) > (float) (packedNameArea.getWidth() - 2 * packedPadX - 8) - 2.0f))
            {
                auto area = packedNameArea;
                IlanaTheme::drawFitted (g, name.upToFirstOccurrenceOf (" ", false, false), area.removeFromTop (lineHeight), juce::Justification::centredTop, 1);
                IlanaTheme::drawFitted (g, name.fromFirstOccurrenceOf (" ", false, false), area.removeFromTop (lineHeight), juce::Justification::centredTop, 1);
            }
            else
                IlanaTheme::drawFitted (g, name, packedNameArea, juce::Justification::centredTop, 1);
        }
    }

    void paintRings (juce::Graphics& g, int highlighted)
    {
        const auto count = numRings();
        const auto knobRadius = knobRadiusFor (knobBounds);

        if (count == 0 || knobRadius < 8.0f)
            return;

        const auto centre = dialCentre();
        const auto knobColour = slider.findColour (juce::Slider::rotarySliderFillColourId);
        // (Where the knob's hand is: it glides to a value that changed by itself.)
        const auto baseNorm = IlanaTheme::fadePeek (&slider, 90, (float) juce::jlimit (0.0, 1.0, slider.valueToProportionOfLength (slider.getValue())));
        const auto angleOf = [] (float norm) { return ringStart + juce::jlimit (0.0f, 1.0f, norm) * (ringEnd - ringStart); };
        // Where the knob's value moved by `amount` sits on its travel (a
        // knob standing in for another parameter counts its travel as the
        // target's range).
        const auto normAt = [this, baseNorm] (float amount)
        {
            if (ringConfig.unit == ModRingConfig::Unit::travel || ! ringOnOwnValue)
                return baseNorm + amount * (ringConfig.unit == ModRingConfig::Unit::travel ? ringConfig.scale : 1.0f);
            const juce::NormalisableRange<float> range ((float) slider.getMinimum(), (float) slider.getMaximum());
            const auto base = (float) slider.proportionOfLengthToValue (baseNorm);
            return (float) slider.valueToProportionOfLength (ringConfig.modulated (range, base, amount));
        };

        // The outer rings pass behind the knob's name, not over it.
        const juce::Graphics::ScopedSaveState state (g);
        // (The rings stop 1 px under the name's letters (ringRoom), so this
        // only guards a grabbed ring's wider stroke; it used to cut the
        // rings flat 4 px under the label's line, V9-18, design sweep.)
        if (const auto ink = getNameInkBounds(); ! ink.isEmpty())
            g.excludeClipRegion (ink.expanded (2.0f, 0.5f).getSmallestIntegerContainer());

        for (int i = count - 1; i >= 0; --i)
        {
            const auto& dot = routings[(size_t) i];
            const auto radius = ringRadius (i);
            const auto hotNow = i == hoveredRing || i == draggedRing;
            const auto quietNow = dot.bypass || (highlighted != 0 && highlighted != dot.source);
            // (The grab and the quieting ease in and out: the shared animator.)
            const auto hotAmount = IlanaTheme::fade (*this, 300 + i, hotNow ? 1.0f : 0.0f, IlanaTheme::FadeRate::hover);
            const auto quietAmount = IlanaTheme::fade (*this, 320 + i, quietNow ? 1.0f : 0.0f);
            const auto hot = hotNow;
            const auto quiet = quietNow;
            const auto tight = count > 1 && ringRadius (1) - ringRadius (0) < 3.0f;
            const auto width = (tight ? 1.6f : 2.0f) + hotAmount;
            const auto colour = modArcColour (modSourceColour (dot.source), knobColour);
            const auto low = dot.bipolar ? normAt (-std::abs (dot.depth)) : juce::jmin (baseNorm, normAt (dot.depth));
            const auto high = dot.bipolar ? normAt (std::abs (dot.depth)) : juce::jmax (baseNorm, normAt (dot.depth));

            juce::Path range;
            const auto a = angleOf (low), b = angleOf (high);

            // A routing with no depth (or nothing left to sweep) still shows
            // as a short tick at the knob's value, so it can be grabbed.
            if (b - a < 0.06f)
                range.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, (a + b) * 0.5f - 0.03f, (a + b) * 0.5f + 0.03f, true);
            else
                range.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, a, b, true);

            // The component sheet's modulation ring: a 2 px arc in the
            // source's colour, solid; what it adds right now a brighter
            // stretch; the depth handle shows while the ring is grabbed.
            const auto rounded = juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
            g.setColour (colour.withAlpha (((0.95f + 0.05f * hotAmount) * (1.0f - quietAmount) + 0.3f * quietAmount) * ringIn));
            g.strokePath (range, rounded);

            // A stereo route moves the right channel the other way: that
            // half of the ring is dashed (a both-sides ring already covers it).
            if (dot.stereo && ! dot.bipolar)
            {
                const auto mirrored = angleOf (normAt (-dot.depth)), baseAt = angleOf (baseNorm);

                if (std::abs (mirrored - baseAt) >= 0.06f)
                {
                    juce::Path half, dashed;
                    half.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, juce::jmin (mirrored, baseAt), juce::jmax (mirrored, baseAt), true);
                    const float dashes[] { 3.0f, 3.0f };
                    juce::PathStrokeType (width).createDashedStroke (dashed, half, dashes, 2);
                    g.fillPath (dashed);
                }
            }

            if (dot.bypass)
                continue;

            // What it adds now: from the knob's value to the live position.
            const auto live = juce::isPositiveAndBelow (i, (int) liveValues.size()) ? liveValues[(size_t) i] : 0.0f;
            const auto liveAngle = angleOf (normAt (live));
            const auto baseAngle = angleOf (baseNorm);

            if (! quiet && std::abs (liveAngle - baseAngle) > 0.01f)
            {
                juce::Path now;
                now.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, juce::jmin (baseAngle, liveAngle),
                                   juce::jmax (baseAngle, liveAngle), true);
                g.setColour (colour.interpolatedWith (juce::Colours::white, 0.45f).withMultipliedAlpha (ringIn));
                g.strokePath (now, rounded);
            }

            // The white dot at the live position: where the source has the
            // knob right now (the stretch's leading end).
            if (! quiet)
            {
                const auto dotAt = centre.getPointOnCircumference (radius, liveAngle);
                g.setColour (juce::Colours::white.withAlpha (0.95f * ringIn));
                g.fillEllipse (juce::Rectangle<float> (width + 1.6f, width + 1.6f).withCentre (dotAt));
            }

            if (hot)
            {
                const auto handleAt = centre.getPointOnCircumference (radius, angleOf (normAt (dot.depth)));
                const auto handle = juce::Rectangle<float> (6.5f, 6.5f).withCentre (handleAt);
                g.setColour (IlanaTheme::Ui::bg);
                g.fillEllipse (handle.expanded (1.0f));
                g.setColour (colour);
                g.fillEllipse (handle);
            }
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (packedD > 0.0f)
        {
            paintPackedText (g);
            return;
        }

        if (slider.isFine() && label.isVisible() && ! label.getText().isEmpty())
        {
            g.setColour (IlanaTheme::accent());
            g.setFont (label.getFont());
            IlanaTheme::drawFitted (g, "FINE", label.getBounds(), label.getJustificationType(), 1);
        }

        if (! inlineText || inlineValueArea.isEmpty())
            return;

        g.setColour (IlanaTheme::Ui::text);
        g.setFont (IlanaTheme::font (IlanaTheme::TextSize::body, true));
        IlanaTheme::drawFitted (g, slider.getTextFromValue (slider.getValue()), inlineValueArea, juce::Justification::centredLeft, 1);
    }

    void resized() override
    {
        auto area = getLocalBounds();

        if (packedD > 0.0f)
        {
            // The cell inside the room for the rings: the dial at its top, the name under it.
            const auto cell = area.reduced (packedPadX, 0).withTrimmedTop (packedPadTop);
            const auto dial = juce::roundToInt (packedD);
            slider.getProperties().remove ("dialRadiusCap");
            knobBounds = juce::Rectangle<int> (dial, dial).withPosition (cell.getCentreX() - dial / 2, cell.getY());
            slider.setPaintingIsUnclipped (true);
            slider.setBounds (knobBounds);
            label.setBounds ({});
            packedNameArea = { cell.getX() - packedPadX - 4, knobBounds.getBottom(), cell.getWidth() + 2 * packedPadX + 8, juce::jmax (0, cell.getBottom() - knobBounds.getBottom()) };
            layoutDots();
            return;
        }

        // (Only an inline knob caps its dial: set below; a knob switched
        // back to stacked must not keep a small dial.)
        slider.getProperties().remove ("dialRadiusCap");
        // A small dial's arc is a little wider than its box (the dial never
        // draws under 28 px): it was cut flat at the top by the slider's
        // own edge. It may draw into the knob's cell (ilana, 2026-10-10).
        slider.setPaintingIsUnclipped (true);

        if (compact && inlineText)
        {
            const auto dial = juce::jmin (area.getHeight(), maxDial);

            // (As an inline knob: a modulatable dial a little smaller, so a ring fits the cell.)
            if (ringConfig.destination != 0)
                slider.getProperties().set ("dialRadiusCap", juce::jmax (9.0f, (float) dial * 0.5f - 4.0f));
            else
                slider.getProperties().remove ("dialRadiusCap");
            knobBounds = area.removeFromLeft (dial).withSizeKeepingCentre (dial, dial);

            slider.setBounds (knobBounds);
            area.removeFromLeft (6);
            const auto top = area.getY() + (area.getHeight() - 27) / 2;
            label.setBounds (area.getX(), top, area.getWidth(), 13);
            label.setFont (IlanaTheme::font (IlanaTheme::TextSize::label, true));
            inlineValueArea = { area.getX(), top + 13, area.getWidth(), 14 };
            layoutDots();
            return;
        }

        if (compact)
        {
            knobBounds = area;
            slider.setBounds (area);
            layoutDots();
            return;
        }

        if (inlineLayout)
        {
            // The dial is a square as high as the cell; the name above the
            // value, level with the dial's middle line, to its right.
            const auto dial = juce::jmin (area.getHeight(), inlineDial);
            // A modulatable dial in a low cell draws a little smaller, so at
            // least one ring fits round it inside the cell (the mockup's
            // inline knobs; none cut by the cell's edge, design sweep).
            if (ringConfig.destination != 0 || capDialLikeRings)
                slider.getProperties().set ("dialRadiusCap", juce::jmax (9.0f, (float) dial * 0.5f - 4.0f));
            else
                slider.getProperties().remove ("dialRadiusCap");
            knobBounds = area.withWidth (dial).withSizeKeepingCentre (dial, dial);
            slider.setBounds (area);
            const auto textX = knobBounds.getRight() + 3;
            label.setBounds (textX, area.getCentreY() - 13, juce::jmax (0, area.getRight() - textX), 13);
            layoutDots();
            return;
        }

        // Label, dial and value as one tight group, so the gaps between them
        // are the same whatever size cell a page gives the knob (a tall cell
        // used to push the label up and the value down). The group sits at
        // the top, where combo and toggle labels in the same row sit; a knob
        // without a label (a matrix cell) is centred instead.
        // (A knob that draws rings keeps 3 px more between its arcs and the value, V12-12.)
        // (A dial stacked between its name and value is never capped: the cap belongs to the inline layouts above, and a knob that was
        // inline a moment ago, in a card too low for its row, must not keep it, or its dial stays at half size.)
        slider.getProperties().remove ("dialRadiusCap");
        const auto valueHeight = 16 + (ringSpaced() ? 3 : 0);
        const auto hasLabel = label.getText().isNotEmpty();
        const auto labelHeight = labelBlockHeight();
        // The value's cell takes the cell's whole width up to 80 px (the
        // dial leaves it free each side), so "-30.9 dB" keeps its space
        // beside "0.0 dB" instead of respelling to fit (V10-8).
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jlimit (64, 80, area.getWidth()), 14);
        const auto dial = juce::jlimit (IlanaTheme::KnobSize::minimum, maxDial,
                                        juce::jmin (area.getWidth(), area.getHeight() - (hasLabel ? labelHeight : 0) - valueHeight));
        const auto groupHeight = juce::jmin (area.getHeight(), (hasLabel ? labelHeight : 0) + dial + valueHeight);
        auto group = hasLabel ? area.removeFromTop (groupHeight) : area.withSizeKeepingCentre (area.getWidth(), groupHeight);

        label.setBounds (hasLabel ? group.removeFromTop (nameLineHeight()) : juce::Rectangle<int>());
        if (hasLabel)
            group.removeFromTop (labelHeight - nameLineHeight());
        knobBounds = group;
        slider.setBounds (group);
        layoutDots();
    }

    // A knob that can't be modulated still takes the drag, to say so: it
    // greys while a source hovers it and a dropped source leaves a note.
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        return processorRef != nullptr && details.description.toString().startsWith ("modsource:");
    }

    // Whether a source can be routed here (else the knob is drawn with a
    // dashed track and refuses drops, I7-5).
    bool isModulatable() const { return ringConfig.destination != 0; }

    // A row mixing knobs that take a ring with ones that don't: the plain ones keep the
    // ring knobs' spacing, so every dial and value in the row sits on one line (P-10, SPRAY).
    void setMatchRingSpacing (bool match)
    {
        if (ringSpacing == match)
            return;
        ringSpacing = match;
        resized();
    }
    bool ringSpaced() const { return ringConfig.destination != 0 || ringSpacing; }

    // A knob that is itself a source (a macro): no dotted track.
    void setIsSourceKnob()
    {
        sourceKnob = true;
        applyModulatableMark();
    }

    // The note a refused drop leaves ("OSC 1 › Unison can't be modulated").
    juce::String refusalText() const { return displayName() + " can't be modulated"; }

    void itemDragEnter (const SourceDetails&) override
    {
        dragHover = true;
        repaint();
    }

    void itemDragExit (const SourceDetails&) override
    {
        dragHover = false;
        repaint();
    }

    void itemDropped (const SourceDetails& details) override
    {
        dragHover = false;

        if (processorRef == nullptr)
            return;

        if (! isModulatable())
        {
            FloatingNote::show (*this, refusalText());
            repaint();
            return;
        }

        const auto sourceIndex = details.description.toString()
                                     .fromFirstOccurrenceOf ("modsource:", false, false)
                                     .getIntValue();

        // One routing per source and knob: a source that already drives
        // this knob points at its routing (dot and card) instead of adding
        // a second one.
        if (! showExistingRouting (sourceIndex))
        {
            processorRef->performEdit ("Add modulation",
                                       [this, sourceIndex] { processorRef->assignModSlot (sourceIndex, ringConfig.destination, 0.35f); });
            startPulse (modSourceColour (sourceIndex));
        }

        refreshRoutings();
        repaint();
    }

    // The slot already routing this source into the knob, or -1.
    int findRoutingFrom (int source) const
    {
        for (const auto& dot : routings)
            if (dot.source == source)
                return dot.slot;

        return -1;
    }

private:
    // The rings' geometry: the value arc's sweep, the first ring a little
    // outside the drawn dial (the look-and-feel's arc), the rest 3.5 px
    // apart. Up to three rings; past that the badges (and their "+N") list
    // every routing.
    static constexpr float ringStart = juce::MathConstants<float>::pi * 1.2f;
    static constexpr float ringEnd = juce::MathConstants<float>::pi * 2.8f;
    static constexpr float ringPitch = 3.5f, ringGap = 2.5f; // (the sheet: the first ring at the arc's radius + 3.5)
    static constexpr int maxRings = 3;

    // As many rings as the knob's cell holds at their tightest (2.5 px
    // apart, the first 1.5 px off the dial), up to three: a ring is never
    // cut flat by the cell's edge or drawn into the name above. The
    // routings past them get the badge ("+N"), as those past three do
    // (ilana's FRAME on OSC, design sweep).
    int ringCapacity() const
    {
        const auto spare = ringRoom() - (dialRadius() + 1.5f);
        return spare < 0.0f ? 0 : juce::jmin (maxRings, 1 + (int) std::floor (spare / 2.5f + 0.001f));
    }
    int numRings() const { return juce::jmin ((int) routings.size(), ringCapacity()); }

    // How far from the dial's centre a ring's middle may run: inside the
    // knob's sides with its stroke and outline, and under the name above
    // (1 px of air) or the knob's own top, with a grabbed ring's half stroke.
    float ringRoom() const
    {
        const auto centre = dialCentre();
        const auto dialTop = dialBounds().getY();
        const auto nameAbove = label.isVisible() && label.getText().isNotEmpty() && (float) label.getBottom() <= dialTop + 2.0f
                               && (float) label.getX() < centre.x && (float) label.getRight() > centre.x;
        const auto ink = nameAbove ? getNameInkBounds() : juce::Rectangle<float>();
        const auto ceiling = ink.isEmpty() ? 0.0f : ink.getBottom() + 1.0f;
        return juce::jmin (juce::jmin (centre.x, (float) getWidth() - centre.x) - 2.5f, centre.y - ceiling - 1.5f);
    }

    // Where the look-and-feel draws the dial: the slider's rotary bounds
    // (above its value box), less its 4 px inset, radius 14-30.
    juce::Rectangle<float> dialBounds() const
    {
        if (packedD > 0.0f)
            return knobBounds.toFloat();
        auto& s = const_cast<KnobSlider&> (slider);
        return s.getLookAndFeel().getSliderLayout (s).sliderBounds.toFloat().translated ((float) slider.getX(), (float) slider.getY()).reduced (4.0f);
    }
    juce::Point<float> dialCentre() const { return dialBounds().getCentre(); }
    float dialRadius() const
    {
        if (packedD > 0.0f)
            return packedD * 0.5f;
        const auto area = dialBounds();
        return juce::jmin ((float) slider.getProperties().getWithDefault ("dialRadiusCap", 30.0f),
                           juce::jlimit (14.0f, 30.0f, juce::jmin (area.getWidth(), area.getHeight()) * 0.5f));
    }

    // A knob in a narrow cell has little room either side of its dial: the
    // rings close up a little (never onto each other or the value arc)
    // rather than run off the knob's edge.
    float ringRadius (int index) const
    {
        const auto count = numRings();
        // (Nor past the cell's sides or into the name above: ringRoom.)
        const auto room = ringRoom();
        const auto dial = dialRadius();
        auto base = dial + ringGap;
        auto pitch = ringPitch;

        if (count > 1)
            pitch = juce::jlimit (2.5f, ringPitch, (room - base) / (float) (count - 1));

        base = juce::jmax (dial + 1.5f, juce::jmin (base, room - pitch * (float) (count - 1)));
        return base + (float) index * pitch;
    }
    // The halo and drop-target rings sit just outside the outer ring. They
    // used to be clipped to the dial's box (1 px of height spare), which cut
    // them flat at the top and bottom while a source was dragged; now they
    // may use the whole cell, kept inside it (and under the name above).
    float haloRadius() const
    {
        const auto wanted = outerRingRadius() + 2.5f;
        const auto room = ringRoom() + 1.0f;
        return juce::jmax (dialRadius() + 2.0f, juce::jmin (wanted, room));
    }
    void clipToHaloArea (juce::Graphics& g) const
    {
        g.reduceClipRegion (getLocalBounds());

        if (const auto ink = getNameInkBounds(); ! ink.isEmpty())
            g.excludeClipRegion (ink.expanded (2.0f, 0.5f).getSmallestIntegerContainer());
    }
    float outerRingRadius() const { return ringRadius (juce::jmax (0, numRings() - 1)) + 1.0f; }

    // The ring under a point (local to the knob), or -1: within the ring band
    // and the arc's sweep (the gap at the bottom stays the knob's).
    int ringAt (juce::Point<float> position) const
    {
        const auto count = numRings();

        if (count == 0 || knobRadiusFor (knobBounds) < 12.0f)
            return -1;

        const auto centre = dialCentre();
        const auto distance = position.getDistanceFrom (centre);

        // A source pinned (or hovered) in the chip bar owns the whole band,
        // at least 8 px, and with Alt held the whole knob (S7-33).
        const auto focus = highlightedModSource() != 0 ? highlightedModSource() : pinnedModSource();
        auto focusRing = -1;
        for (int i = 0; i < count && focus != 0; ++i)
            if (routings[(size_t) i].source == focus)
                focusRing = i;

        if (focusRing >= 0 && juce::ModifierKeys::currentModifiers.isAltDown() && distance <= ringRadius (count - 1) + 4.0f)
            return focusRing;

        const auto inner = ringRadius (0) - (focusRing >= 0 ? 4.0f : 2.5f);
        const auto outer = ringRadius (count - 1) + (focusRing >= 0 ? 4.0f : 3.0f);
        if (distance < inner || distance > outer)
            return -1;

        // JUCE's angles: 0 at 12 o'clock, clockwise.
        auto angle = std::atan2 (position.x - centre.x, centre.y - position.y);
        if (angle < 0.0f)
            angle += juce::MathConstants<float>::twoPi;
        if (angle > juce::MathConstants<float>::pi * 0.8f && angle < juce::MathConstants<float>::pi * 1.2f)
            return -1;

        if (focusRing >= 0)
            return focusRing;

        auto nearest = 0;
        for (int i = 1; i < count; ++i)
            if (std::abs (distance - ringRadius (i)) < std::abs (distance - ringRadius (nearest)))
                nearest = i;

        return nearest;
    }

    // Catches presses on the rings (over the slider); the rest of the knob
    // passes through to the slider. Drag a ring up or right to deepen its
    // routing (Shift for fine), double-click it to zero the depth,
    // right-click to bypass or remove it.
    class RingOverlay : public juce::Component,
                        public juce::SettableTooltipClient
    {
    public:
        explicit RingOverlay (KnobControl& ownerIn) : owner (ownerIn) { setRepaintsOnMouseActivity (false); }

        bool hitTest (int x, int y) override
        {
            return owner.ringAt (owner.getLocalPoint (this, juce::Point<int> (x, y)).toFloat()) >= 0;
        }

        void mouseMove (const juce::MouseEvent& event) override { setHovered (ringUnder (event)); }
        void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }

        void mouseDown (const juce::MouseEvent& event) override
        {
            owner.closeModCard();
            const auto ring = ringUnder (event);

            if (! juce::isPositiveAndBelow (ring, (int) owner.routings.size()))
                return;

            if (event.mods.isPopupMenu())
            {
                showMenu (ring);
                return;
            }

            owner.draggedRing = ring;
            dragSlot = owner.routings[(size_t) ring].slot;
            dragStartDepth = owner.routings[(size_t) ring].depth;

            if (owner.dotStrip.onDragStart != nullptr)
                owner.dotStrip.onDragStart (owner.routings[(size_t) ring].source);
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            if (owner.draggedRing < 0)
                return;

            const auto fine = event.mods.isShiftDown() ? 0.2f : 1.0f;
            const auto travel = (float) (event.getDistanceFromDragStartX() - event.getDistanceFromDragStartY());
            const auto depth = juce::jlimit (-1.0f, 1.0f, dragStartDepth + travel * 0.006f * fine);

            if (juce::isPositiveAndBelow (owner.draggedRing, (int) owner.routings.size()))
                owner.routings[(size_t) owner.draggedRing].depth = depth;

            if (owner.dotStrip.onDepthChange != nullptr)
                owner.dotStrip.onDepthChange (dragSlot, depth);

            updateTooltip (owner.draggedRing);
            owner.repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (owner.draggedRing >= 0 && owner.dotStrip.onDragEnd != nullptr)
                owner.dotStrip.onDragEnd();

            owner.draggedRing = -1;
            owner.repaint();
        }

        void mouseDoubleClick (const juce::MouseEvent& event) override
        {
            const auto ring = ringUnder (event);

            if (juce::isPositiveAndBelow (ring, (int) owner.routings.size()) && owner.dotStrip.onZero != nullptr)
                owner.dotStrip.onZero (owner.routings[(size_t) ring].slot);
        }

    private:
        int ringUnder (const juce::MouseEvent& event) const
        {
            return owner.ringAt (owner.getLocalPoint (this, event.position));
        }

        void setHovered (int ring)
        {
            if (ring == owner.hoveredRing)
                return;

            owner.hoveredRing = ring;
            setMouseCursor (ring >= 0 ? juce::MouseCursor::UpDownLeftRightResizeCursor : juce::MouseCursor::NormalCursor);
            updateTooltip (ring);
            owner.repaint();
        }

        void updateTooltip (int ring)
        {
            if (! juce::isPositiveAndBelow (ring, (int) owner.routings.size()))
                return;

            const auto& dot = owner.routings[(size_t) ring];
            setTooltip (ModNames::source (dot.source, owner.processorRef) + "  " + (dot.depth >= 0.0f ? "+" : "")
                        + juce::String (Mod::shownPercent (dot.depth, dot.bipolar)) + "%" + (dot.bipolar ? "  (swings both ways: 100% = the whole range)" : "")
                        + (dot.bypass ? "  (bypassed)" : "") + (dot.stereo ? "  (stereo: left up, right down)" : "")
                        + "\nDrag the ring up or right to deepen it (Shift for fine), double-click to zero it, "
                          "right-click for unipolar, bipolar, stereo, bypass or remove.");
        }

        void showMenu (int ring)
        {
            const auto dot = owner.routings[(size_t) ring];
            juce::PopupMenu menu;
            ModDotStrip::addRouteItems (menu, dot);
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                                [safeOwner = juce::Component::SafePointer<KnobControl> (&owner), dot] (int result)
                                {
                                    if (safeOwner != nullptr)
                                        safeOwner->dotStrip.handleRouteChoice (result, dot);
                                });
        }

        KnobControl& owner;
        int dragSlot = -1;
        float dragStartDepth = 0.0f;
    };

    float knobRadiusFor (juce::Rectangle<int>) const
    {
        if (packedD > 0.0f)
            return packedD * 0.5f;
        // Deliberately not reduced like the knob itself: the mod ring sits
        // just outside the value arc.
        const auto area = rotaryArea();
        return juce::jmin ((float) slider.getProperties().getWithDefault ("dialRadiusCap", 30.0f),
                           juce::jlimit (14.0f, 30.0f, juce::jmin (area.getWidth(), area.getHeight()) * 0.5f));
    }

    // The rotary is drawn above the value text box, so glow and mod ring must
    // use the same area or they appear off-centre.
    juce::Rectangle<float> rotaryArea() const
    {
        auto area = knobBounds.toFloat();

        if (packedD > 0.0f)
            return area;

        if (! compact && ! inlineLayout)
            area.setHeight (juce::jmax (8.0f, area.getHeight() - 16.0f));

        return area;
    }

    bool routesFrom (int source) const { return findRoutingFrom (source) >= 0; }

    // A source that already drives this knob: ring its dot and open the
    // knob's card so its depth can be set there. False when it doesn't.
    bool showExistingRouting (int source)
    {
        refreshRoutings();
        const auto slot = findRoutingFrom (source);

        if (slot < 0)
            return false;

        dotStrip.flashSlot (slot);
        openModCard();
        return true;
    }

    // The badges sit in a column right of the rings, as many as the dial's
    // height takes (then "+N"); the ring overlay covers the dial.
    void layoutDots()
    {
        const auto centre = dialCentre();
        const auto dial = dialRadius();
        dotStrip.setMini (packedD > 0.0f);
        const auto stripW = dotStrip.widthPx(), pitch = dotStrip.pitchPx();
        auto x = (int) std::ceil (centre.x + outerRingRadius() + 1.0f);

        // An inline knob's name and value stand right of its rings: the
        // badges go after the longer of them, not over them.
        if (inlineLayout && label.getText().isNotEmpty())
        {
            const auto value = slider.getTextFromValue (slider.getValue());
            const auto textWidth = juce::jmax (juce::GlyphArrangement::getStringWidthInt (label.getFont(), label.getText()),
                                               juce::GlyphArrangement::getStringWidthInt (juce::Font (IlanaTheme::font (IlanaTheme::TextSize::body, false, true)), value));
            x = juce::jmax (x, label.getX() + textWidth + 8);
        }

        // Beside the rings, a column of badges as tall as the dial, while
        // the knob is hovered (the rings are the legend the rest of the
        // time, V7-27). A knob too narrow for that leaves its routings to
        // the rings (and its card); only those past the three rings get a
        // badge, in its top-right corner: the routing's own, or "+N".
        const auto shownRings = numRings();
        const auto roomBeside = getWidth() - x >= stripW - 1 && (badgesShown || (int) routings.size() > shownRings);
        juce::Rectangle<int> bounds;

        if (roomBeside)
        {
            dotStrip.setDots (routings);
            dotStrip.setMaxVisible ((int) (dial * 2.0f + 6.0f) / pitch);
            const auto height = juce::jmax (pitch, dotStrip.getPreferredHeight());
            bounds = { x, (int) (centre.y - dial) - 2, stripW, height };
        }
        else
        {
            const auto extra = (int) routings.size() > shownRings ? std::vector<ModDotStrip::Dot> (routings.begin() + shownRings, routings.end())
                                                                   : std::vector<ModDotStrip::Dot>();
            dotStrip.setDots (extra);
            // (A packed knob's extra routings show as one quiet "+N", not a coloured disc over the rings.)
            dotStrip.setMaxVisible (packedD > 0.0f ? 0 : 1);
            auto y = (int) (centre.y - dial) - pitch + 1;

            if (label.isVisible() && label.getText().isNotEmpty())
            {
                const auto textWidth = juce::GlyphArrangement::getStringWidthInt (label.getFont(), label.getText());
                if (label.getBounds().getCentreX() + textWidth / 2 + 2 > getWidth() - stripW + 2)
                    y = juce::jmax (y, label.getBottom() - 1);
            }

            bounds = { getWidth() - stripW, packedD > 0.0f ? 0 : y, stripW, pitch };
        }

        // A packed knob's "+N" sat on its own rings (its cell has no room round them, UI review 26): it goes up into
        // the parent, just clear of the outer ring's top right, so it can stand outside the knob's box.
        const auto wantsVisible = ! compact && (roomBeside ? ! routings.empty() : (int) routings.size() > shownRings);
        auto* parent = getParentComponent();
        const auto lifted = packedD > 0.0f && ! roomBeside && parent != nullptr;
        auto* host = lifted ? parent : static_cast<juce::Component*> (this);
        if (dotStrip.getParentComponent() != host)
            host->addChildComponent (dotStrip);

        if (lifted)
        {
            const auto ring = outerRingRadius();
            bounds = juce::Rectangle<int> (stripW, pitch).withPosition ((int) std::round (centre.x + ring * 0.45f), (int) std::floor (centre.y - ring) - pitch - 1)
                     + getPosition();
            dotStrip.setBounds (bounds);
            dotStrip.setVisible (wantsVisible && isVisible());
        }
        else
        {
            dotStrip.setBounds (bounds.withY (juce::jmax (0, bounds.getY())));
            dotStrip.setVisible (wantsVisible);
        }

        ringOverlay.setBounds (getLocalBounds());
        ringOverlay.setVisible (! routings.empty());
        ringOverlay.toFront (false);
        dotStrip.toFront (false);
    }

    // Re-reads which mod slots route into this knob. Cheap (raw parameter
    // reads), so it runs on the timer.
    // Sets a route's polarity (one undo step), keeping what the ring shows.
    void setRoutePolarity (int slot, int choice)
    {
        if (processorRef == nullptr)
            return;

        processorRef->performEdit ("Modulation polarity", [this, slot, choice] { processorRef->setModSlotValue (slot, "pol", (float) choice); });
        refreshRoutings();
    }

    // Types an exact amount for a route: a small box over the knob; the
    // number is the share of the knob's range the route covers (bipolar:
    // 100 % is the whole range, either side of the value).
    void beginAmountEntry (int slot)
    {
        if (processorRef == nullptr)
            return;

        const auto data = processorRef->readModSlot (slot);
        const auto bipolar = Mod::isBipolarRoute (data);
        amountEditor = std::make_unique<juce::TextEditor>();
        auto& editor = *amountEditor;
        editor.setMultiLine (false);
        editor.setJustification (juce::Justification::centred);
        editor.setFont (IlanaTheme::font (IlanaTheme::TextSize::label, true));
        editor.setInputRestrictions (6, "-+0123456789.%");
        editor.setText (juce::String (Mod::shownPercent (Mod::visualDepth (data), bipolar)) + "%", false);
        editor.setBounds (juce::Rectangle<int> (64, 20).withCentre ({ getWidth() / 2, juce::jmin (getHeight() - 12, knobBounds.getBottom() - 8) }));
        addAndMakeVisible (editor);
        editor.selectAll();
        editor.grabKeyboardFocus();
        editor.onReturnKey = [this, slot, bipolar]
        {
            if (amountEditor != nullptr && processorRef != nullptr)
            {
                const auto typed = amountEditor->getText().retainCharacters ("-+0123456789.").getFloatValue();
                const auto depth = juce::jlimit (-1.0f, 1.0f, Mod::depthFromShownPercent (typed, bipolar));
                processorRef->performEdit ("Modulation amount", [this, slot, depth]
                                           { processorRef->setModSlotValue (slot, "amt", Mod::storedDepth (processorRef->readModSlot (slot), depth)); });
            }

            closeAmountEntry();
        };
        editor.onEscapeKey = [this] { closeAmountEntry(); };
        editor.onFocusLost = [this] { closeAmountEntry(); };
    }

    void closeAmountEntry()
    {
        if (amountEditor == nullptr)
            return;

        // (Removed after this call returns: it may be running inside the editor.)
        std::shared_ptr<juce::TextEditor> doomed (std::move (amountEditor));
        removeChildComponent (doomed.get());
        refreshRoutings();
        juce::MessageManager::callAsync ([doomed] {});
    }

    void refreshRoutings()
    {
        if (processorRef == nullptr || ringConfig.destination == 0)
            return;

        std::vector<ModDotStrip::Dot> found;
        auto strongest = 0.0f;
        dominantSource = 0;

        for (int i = 0; i < Mod::maxSlots; ++i)
        {
            const auto slot = processorRef->readModSlot (i);

            if (slot.destination != ringConfig.destination || slot.source == Mod::Source::None)
                continue;

            const auto bipolar = Mod::isBipolarRoute (slot);
            found.push_back ({ i, (int) slot.source, Mod::visualDepth (slot), slot.bypass, bipolar, (int) slot.polarity,
                               slot.stereo && Mod::canBeStereoAnywhere (slot.destination), Mod::canBeStereoAnywhere (slot.destination) });

            if (! slot.bypass && std::abs (slot.depth) > strongest)
            {
                strongest = std::abs (slot.depth);
                dominantSource = (int) slot.source;
            }
        }

        const auto changed = found.size() != routings.size()
                             || ! std::equal (found.begin(), found.end(), routings.begin(),
                                              [] (const auto& a, const auto& b)
                                              { return a.slot == b.slot && a.source == b.source && a.bypass == b.bypass
                                                       && a.bipolar == b.bipolar && a.polarity == b.polarity && a.stereo == b.stereo
                                                       && std::abs (a.depth - b.depth) < 1.0e-4f; });

        // Mid-drag the ring keeps its own depth (the parameter catches up).
        if (! changed || draggedRing >= 0)
            return;

        // A ring added to a knob on screen fades in.
        if (found.size() > routings.size() && IlanaAnim::showing (*this))
            ringIn = 0.0f;

        routings = std::move (found);
        layoutDots(); // (and the badges' list)
        repaint();
    }

public:
    // "Modulate with": LFOs, envelopes, macros and the rest, by their
    // ModNames names (the UI test reads it).
    juce::PopupMenu buildModulateWithMenu() const
    {
        juce::PopupMenu sourceMenu, lfoMenu, envMenu, macroMenu, otherMenu;

        // Only the LFOs and envelopes in the pools (a source that has no
        // card isn't offered); "New LFO" / "New envelope" adds the next.
        // The LFOs' B outputs file under LFOs.
        for (const auto i : ModNames::sourcesInMenuOrder())
        {
            const auto source = (Mod::Source) i;
            if (source == Mod::Source::InputEnv && ! IlanaSynthAudioProcessor::isEffectBuild)
                continue;
            if (! modSourceInPatch (*processorRef, source) && ! routesFrom (i))
                continue;
            const auto group = ModNames::groupOf (i);
            auto& target = group == ModNames::SourceGroup::lfo || group == ModNames::SourceGroup::lfoB ? lfoMenu
                         : group == ModNames::SourceGroup::envelope ? envMenu
                         : group == ModNames::SourceGroup::macro ? macroMenu : otherMenu;
            target.addItem (i + 1, ModNames::source (i, processorRef), true, routesFrom (i));
        }

        for (int lfo = 0; lfo < IlanaSynthAudioProcessor::numLfos; ++lfo)
            if (! processorRef->isLfoShown (lfo))
            {
                lfoMenu.addSeparator();
                lfoMenu.addItem (6000 + lfo, "New LFO  (LFO " + juce::String (lfo + 1) + ")");
                break;
            }

        for (int env = 0; env < 16; ++env)
            if (! envelopeShown (*processorRef, env))
            {
                envMenu.addSeparator();
                envMenu.addItem (6100 + env, "New envelope  (" + ModNames::source ((int) envelopeSource (env)) + ")");
                break;
            }

        sourceMenu.addSubMenu ("LFOs", lfoMenu);
        sourceMenu.addSubMenu ("Envelopes", envMenu);
        sourceMenu.addSubMenu ("Macros", macroMenu);
        sourceMenu.addSubMenu ("Performance and more", otherMenu);
        return sourceMenu;
    }

    // The right-click menu (the snapshot tool draws it offscreen: a real
    // menu window can't open under its X server).
    // Optional: items the knob's owner puts first in its right-click menu,
    // with their own actions (the PATCH tab's effect windows: assign to an LFO box).
    std::function<void (juce::PopupMenu&)> extraMenu;

    juce::PopupMenu buildModMenu() const
    {
        juce::PopupMenu menu;

        if (extraMenu)
        {
            extraMenu (menu);
            if (menu.getNumItems() > 0)
                menu.addSeparator();
        }

        if (processorRef == nullptr)
            return menu;

        if (ringConfig.destination != 0)
        {
            menu.addSubMenu ("Modulate with", buildModulateWithMenu());

            if (! routings.empty())
            {
                juce::PopupMenu removeMenu;

                for (const auto& dot : routings)
                    removeMenu.addItem (5000 + dot.slot, ModNames::source (dot.source, processorRef) + "  ("
                                                             + juce::String (Mod::shownPercent (dot.depth, dot.bipolar)) + "%)");

                menu.addSubMenu ("Remove modulation", removeMenu);
                menu.addItem (1000, "Clear all modulation to this knob");
            }

            menu.addSeparator();
        }

        if (parameter != nullptr)
            menu.addItem (2000, "Reset to default  (double-click)");

        menu.addItem (3500, juce::String (juce::CharPointer_UTF8 ("Type a value\xe2\x80\xa6")));
        menu.addSeparator();
        menu.addItem (3000, "Copy value");

        if (knobClipboard().isNotEmpty())
            menu.addItem (3001, "Paste value");

        const auto isMacro = parameterId.startsWith ("macro") && ! parameterId.containsChar ('_');

        if (isMacro)
        {
            const auto macroIndex = parameterId.getTrailingIntValue() - 1;

            menu.addSeparator();
            menu.addItem (4000, "MIDI Learn  (CC " + juce::String (processorRef->getMacroCc (macroIndex)) + ")");
        }
        else if (parameter != nullptr && parameter->isAutomatable())
        {
            menu.addSeparator();
            addMidiLearnItems (menu, *processorRef, parameterId);
        }

        return menu;
    }

private:
    void showModMenu()
    {
        if (processorRef == nullptr && ! extraMenu)
            return;

        auto menu = buildModMenu();
        juce::Component::SafePointer<KnobControl> safeThis (this);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safeThis] (int result)
                            {
                                if (safeThis == nullptr || result == 0)
                                    return;

                                auto& processor = *safeThis->processorRef;

                                // Each edit here is one named undo step.
                                const auto name = result == 1000   ? juce::String ("Clear modulation")
                                                  : result == 2000 ? "Reset " + safeThis->label.getText()
                                                  : result == 3001 ? "Paste " + safeThis->label.getText()
                                                  : result >= 6000 ? juce::String ("Add modulation")
                                                  : result >= 5000 ? juce::String ("Remove modulation")
                                                                   : juce::String ("Add modulation");

                                if (handleMidiLearnResult (result, processor, safeThis->parameterId))
                                    return;

                                if (result == 3500)
                                {
                                    safeThis->showTypedValueBox();
                                    return;
                                }

                                // "Modulate with" a source that already drives
                                // the knob edits that routing instead.
                                if (result > 0 && result < 1000 && safeThis->showExistingRouting (result - 1))
                                    return;

                                if (result != 3000 && result != 4000)
                                    processor.beginEdit (name);

                                if (result == 1000)
                                    processor.clearModSlotsForTarget (safeThis->ringConfig.destination);
                                else if (result == 2000)
                                {
                                    if (safeThis->parameter != nullptr)
                                        safeThis->parameter->setValueNotifyingHost (safeThis->parameter->getDefaultValue());
                                }
                                else if (result == 3000)
                                {
                                    knobClipboard() = juce::String (safeThis->slider.getValue(), 6);
                                }
                                else if (result == 3001)
                                {
                                    const auto value = knobClipboard().getDoubleValue();
                                    safeThis->slider.setValue (value, juce::sendNotificationSync);
                                }
                                else if (result == 4000)
                                {
                                    processor.startMacroLearn (safeThis->parameterId.getTrailingIntValue() - 1);
                                }
                                else if (result >= 6100)
                                {
                                    // A new envelope: into the pool, then routed.
                                    processor.setRevealed (IlanaSynthAudioProcessor::Module::Envelope, result - 6100, true);
                                    processor.assignModSlot ((int) envelopeSource (result - 6100), safeThis->ringConfig.destination, 0.35f);
                                }
                                else if (result >= 6000)
                                {
                                    processor.setRevealed (IlanaSynthAudioProcessor::Module::Lfo, result - 6000, true);
                                    processor.assignModSlot ((int) Mod::lfoSourceFor (result - 6000), safeThis->ringConfig.destination, 0.35f);
                                }
                                else if (result >= 5000)
                                {
                                    processor.clearModSlot (result - 5000);
                                }
                                else
                                {
                                    processor.assignModSlot (result - 1, safeThis->ringConfig.destination, 0.35f);
                                }

                                processor.endEdit();
                                safeThis->refreshRoutings();

                                // A source taking the knob swells its ring.
                                if (result >= 6100 || (result >= 6000) || (result > 0 && result < 1000))
                                    safeThis->startPulse (modSourceColour (result >= 6100 ? (int) envelopeSource (result - 6100)
                                                                           : result >= 6000 ? (int) Mod::lfoSourceFor (result - 6000)
                                                                                            : result - 1));
                            });
    }

    // A ring that swells off the dial and fades (about 0.35 s): the
    // right-click that opens the modulation menu, and a source taking the
    // knob (ilana, 2026-10-10).
    void startPulse (juce::Colour colour)
    {
        pulse = 1.0f;
        pulseColour = colour;
        repaint();
    }

    void paintPulse (juce::Graphics& g) const
    {
        if (pulse <= 0.0f || knobBounds.isEmpty())
            return;

        const auto eased = 1.0f - pulse * pulse;
        const auto radius = juce::jmin (dialRadius() + 2.0f + eased * 9.0f, juce::jmax (dialRadius() + 2.0f, ringRoom() + 1.0f));
        const juce::Graphics::ScopedSaveState state (g);
        clipToHaloArea (g);
        g.setColour (pulseColour.withAlpha (0.75f * pulse));
        g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (dialCentre()), 1.0f + 1.6f * pulse);
    }

    void timerCallback() override
    {
        // Knobs on hidden pages skip the frame (hundreds of them).
        if (! IlanaAnim::showing (*this))
            return;

        if (pulse > 0.0f)
        {
            pulse = juce::jmax (0.0f, pulse - frameSeconds() * 2.8f);
            repaint();
        }

        if (const auto dragged = modSourceBeingDragged (*this); dragged != dragSource)
        {
            dragSource = dragged;
            repaint();
        }

        const auto targetGlow = hover ? 1.0f : 0.0f;
        glow = IlanaAnim::approach (glow, targetGlow, 0.22f, frameTicks());

        const auto value = slider.getValue();

        // Only the user's own moves light the knob up, not a preset load,
        // undo or automation.
        if (std::abs (value - lastSliderValue) > 1.0e-6)
        {
            lastSliderValue = value;

            if (slider.isMouseOverOrDragging())
                activity = 1.0f;
            else if (IlanaAnim::presetJustLoaded() && pulse <= 0.0f)
                startPulse (juce::Colours::white.withAlpha (0.8f)); // a preset load moved it
        }

        activity = IlanaAnim::decay (activity, 0.88f, frameTicks());

        if (processorRef == nullptr || ringConfig.destination == 0)
        {
            if ((glow > 0.01f && glow < 0.99f) || activity > 0.01f)
                repaint();

            return;
        }

        if ((routingCheck += frameSeconds()) >= 0.2f)
        {
            routingCheck = 0.0f;
            refreshRoutings();
        }

        if (ringIn < 1.0f)
        {
            ringIn = ringIn > 0.97f ? 1.0f : IlanaAnim::approach (ringIn, 1.0f, 0.16f, frameTicks());
            repaint();
        }

        // Resting on a modulated knob (not turning it) opens its source card.
        // It stays while the mouse is on the card, after a moment's grace to
        // cross the gap between them.
        const auto overKnob = isMouseOver (true) && ! slider.isMouseButtonDown();

        if (const auto wantBadges = ! routings.empty() && (isMouseOverOrDragging (true) || badgesForced); wantBadges != badgesShown)
        {
            badgesShown = wantBadges;
            layoutDots();
        }

        const auto cardEngaged = modCardOpen && modHoverHooks().engaged != nullptr && modHoverHooks().engaged (*this);

        if (! routings.empty() && (overKnob || cardEngaged))
        {
            cardLeave = 0.0f;

            if (! modCardOpen && overKnob && (hoverRest += frameSeconds()) >= 0.35f)
                openModCard();
        }
        else if (modCardOpen && ! modCardHeld)
        {
            if ((cardLeave += frameSeconds()) >= 0.25f)
                closeModCard();
        }
        else if (hoverRest > 0.0f)
        {
            closeModCard();
        }

        const auto highlighted = highlightedModSource() * 1000 + pinnedModSource();
        auto modValue = processorRef->getModDisplay (ringConfig.destination);

        // Each ring's live stretch: its source now, shaped by the slot, times
        // the depth.
        liveValues.resize ((size_t) numRings());
        for (int i = 0; i < numRings(); ++i)
        {
            const auto slot = processorRef->readModSlot (routings[(size_t) i].slot);
            // (Eased a little, so a source swinging both ways at audio rate
            // reads as a stretch, not a flicker: N16 leftover.)
            const auto target = slot.bypass ? 0.0f
                                            : Mod::shape (slot, processorRef->getSourceDisplayValue ((int) slot.source)) * slot.depth;
            liveValues[(size_t) i] = IlanaAnim::approach (liveValues[(size_t) i], target, 0.5f, frameTicks());
            modValue += 3.0f * liveValues[(size_t) i] * (float) (i + 2);
        }

        if (std::abs (modValue - lastModValue) > 0.002f || highlighted != lastHighlighted)
        {
            lastModValue = modValue;
            lastHighlighted = highlighted;
            repaint();
        }
        else if ((glow > 0.01f && glow < 0.99f) || activity > 0.01f)
        {
            repaint();
        }
    }

    KnobSlider slider;
    juce::Label label;
    ModDotStrip dotStrip;
    RingOverlay ringOverlay { *this };
    std::vector<float> liveValues;
    float ringIn = 1.0f; // (a ring just added fades in)
    int hoveredRing = -1, draggedRing = -1;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    IlanaSynthAudioProcessor* processorRef = nullptr;
    juce::RangedAudioParameter* parameter = nullptr;
    // The knob's name as the matrix writes it ("OSC 1 › Frame", S7-20),
    // the first line of its tooltip and the hover line's title.
    juce::String displayName() const
    {
        if (ringConfig.destination != 0)
            return processorRef != nullptr ? ModNames::destination (ringConfig.destination, *processorRef)
                                           : ModNames::destination (ringConfig.destination);
        return parameter != nullptr ? ModNames::asLabelled (ModNames::detail::paramName (parameterId, parameter->getName (64))).full()
                                    : parameterId;
    }

    // Tooltip and track for whether the knob takes a source: a dotted
    // track and "Not modulatable" when it doesn't (I7-5).
    void applyModulatableMark()
    {
        slider.getProperties().set ("notModulatable", ! isModulatable() && ! sourceKnob);
        slider.repaint();

        if (parameter == nullptr)
            return;

        const auto description = describeParameter (parameterId);
        const auto modHint = isModulatable() ? juce::String ("  Drop a mod source here, or right-click to modulate.")
                                             : juce::String ("  Not modulatable.");
        baseTooltip = displayName() + "\n" + description + modHint;
        const auto tooltip = baseTooltip + (inactiveNote.isNotEmpty() ? "\n(No effect now: " + inactiveNote + ")" : juce::String());
        slider.setTooltip (tooltip);
        setTooltip (tooltip);
    }

    ModRingConfig ringConfig;
    bool ringOnOwnValue = true; // false while it shows another parameter's ring (setModulationTarget)
    juce::String parameterId;
    juce::Colour knobAccent;
    bool followsTheme = false;
    juce::Rectangle<int> knobBounds;
    std::vector<ModDotStrip::Dot> routings;
    int dominantSource = 0;
    float routingCheck = 0.0f;
    float hoverRest = 0.0f;
    float cardLeave = 0.0f;
    bool modCardOpen = false;
    bool modCardHeld = false;
    int lastHighlighted = 0;
    float lastModValue = 0.0f;
    float glow = 0.0f;
    float activity = 0.0f;
    double lastSliderValue = 0.0;
    bool dragHover = false;
    int dragSource = 0; // the source being dragged anywhere (V8-10)
    float pulse = 0.0f; // the right-click / assign ring, 1 to 0
    std::unique_ptr<juce::TextEditor> amountEditor;
    juce::Colour pulseColour;
    bool hover = false;
    bool compact = false, inlineText = false;
    float packedD = 0.0f;
    int packedValueRoom = 0;
    bool packedNameWrap = false, packedNameAlwaysWraps = false;
    float packedNameSize = 0.0f;
    juce::Rectangle<int> packedNameArea;
    std::unique_ptr<juce::TextEditor> valueEditor;
    juce::Rectangle<int> inlineValueArea;
    bool inlineLayout = false;
    bool capDialLikeRings = false;
    bool ringSpacing = false;
    int inlineDial = 40;
    std::function<bool()> dashWhen;
    bool sourceKnob = false;
    bool badgesShown = false; // the mouse is on the knob (or its badges)
    mutable juce::String nameInkKey; // (getNameInkBounds's cache)
    mutable juce::Rectangle<float> nameInk;
    bool badgesForced = false; // (the tests and snapshots)
    int maxDial = IlanaTheme::KnobSize::main;
    juce::String baseTooltip, inactiveNote;
};

class ComboControl : public juce::Component,
                     public juce::SettableTooltipClient,
                     private IlanaAnim::FrameTimer
{
public:
    ComboControl (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID,
                  const juce::String& labelText)
        : parameterId (parameterID)
    {
        label.setText (labelText, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centredLeft);
        label.setBorderSize ({ 0, 4, 0, 1 }); // (layoutRow fits a menu's name by these 5 px)
        // (The sheet's field name, .lbl: bold tracked caps in t3, as a knob's.)
        label.setFont (IlanaTheme::font (IlanaTheme::TextSize::label, true).withKerningFactor (0.07f));
        label.setColour (juce::Label::textColourId, IlanaTheme::Ui::text3);
        addAndMakeVisible (label);

        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (parameterID)))
            combo.addItemList (choice->getAllValueStrings(), 1);

        addAndMakeVisible (combo);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, parameterID, combo);

        if (auto* parameter = state.getParameter (parameterID))
        {
            const auto description = describeParameter (parameterID);
            const auto tooltip = parameter->getName (64) + (description.isNotEmpty() ? "\n" + description : "");
            combo.setTitle (parameter->getName (64));
            combo.setDescription (description);
            combo.setTooltip (tooltip);
            setTooltip (tooltip);
        }

        startTimerHz (30);
    }

    juce::ComboBox& getComboBox() { return combo; }
    juce::Label& getNameLabel() { return label; }
    const juce::String& getParameterId() const { return parameterId; }

    // A family-coloured menu as the mockup draws the VECTOR corners: a tinted
    // outline and fill, the name in the family colour.
    void setTint (juce::Colour tint)
    {
        if (tint == currentTint)
            return;

        currentTint = tint;
        // (The sheet's .sel.c: the colour at 55 % into the hairline, 10 % into ink 3.)
        combo.setColour (juce::ComboBox::outlineColourId, IlanaTheme::Ui::line.interpolatedWith (tint, 0.55f));
        combo.setColour (juce::ComboBox::backgroundColourId, IlanaTheme::Ui::raised.interpolatedWith (tint, 0.1f));
        // (Only the box takes the colour: the name stays the sheet's grey .lbl.)
        label.setFont (IlanaTheme::font (IlanaTheme::TextSize::label, true).withKerningFactor (0.07f));
        combo.repaint();
        label.repaint();
    }

    // Replace the drop-down list with something else when clicked.
    void setPopupOverride (std::function<void()> override) { combo.popupOverride = std::move (override); }
    // The arrow keys' step to the next or previous choice (+1 / -1), when a
    // choice must go through more than its parameter (the oscillator type).
    void setNudgeOverride (std::function<void (int)> override) { combo.nudgeOverride = std::move (override); }

    // Called with the new item's index when the person picks from the list
    // (not when a preset load, undo or automation moves the choice).
    void setOnPick (std::function<void (int)> onPick)
    {
        combo.onChange = [this, onPick = std::move (onPick)]
        {
            if (combo.pickedFromList() && ! IlanaAnim::presetJustLoaded())
                onPick (combo.getSelectedItemIndex());
        };
    }

    juce::String getLabelText() const { return label.getText(); }
    void setLabelText (const juce::String& text)
    {
        if (label.getText() != text)
        {
            label.setText (text, juce::dontSendNotification);
            if (packedMenu)
                combo.getProperties().set ("textPrefix", text);
        }
    }

    // Packed (the OSC cards' panels, review 23): one box, its name inside before
    // the value; the box and its text are the look-and-feel's ("packedMenu").
    void setPackedMenu (bool packed)
    {
        if (packedMenu == packed)
            return;

        packedMenu = packed;
        combo.getProperties().set ("packedMenu", packed);
        combo.getProperties().set ("textPrefix", packed ? label.getText() : juce::String());
        label.setVisible (! packed);
        resized();
        combo.resized();
        combo.repaint();
    }
    bool isPackedMenu() const { return packedMenu; }

    // A menu 18 px high under a 9 px name (the OSC cards and the voice strip).
    void setCompactLayout (bool shouldBeCompact)
    {
        if (compactLayout == shouldBeCompact)
            return;

        compactLayout = shouldBeCompact;
        label.setFont (IlanaTheme::font (IlanaTheme::TextSize::label, true).withKerningFactor (0.07f));
        combo.getProperties().set ("compactMenu", compactLayout);
        resized();
    }

    void resized() override
    {
        auto area = getLocalBounds();

        if (packedMenu)
        {
            label.setBounds ({});
            combo.setBounds (area);
            return;
        }

        if (compactLayout && label.getText().isNotEmpty())
        {
            label.setBounds (area.removeFromTop (10));
            combo.setBounds (area.removeFromTop (18));
            return;
        }

        if (compactLayout)
        {
            label.setBounds ({});
            combo.setBounds (area.withSizeKeepingCentre (area.getWidth(), juce::jmin (20, area.getHeight())));
            return;
        }

        // A menu without a name (PLAY's strips: its value says what it is)
        // is just the box.
        if (label.getText().isEmpty())
        {
            label.setBounds ({});
            combo.setBounds (area.withSizeKeepingCentre (area.getWidth(), juce::jmin (24, area.getHeight())));
            return;
        }

        label.setBounds (area.removeFromTop (13));
        combo.setBounds (area.removeFromTop (24));
    }

    void paint (juce::Graphics& g) override
    {
        // (Hover lights the select's own rim: drawComboBox.)
        juce::ignoreUnused (g);
    }



private:
    void timerCallback() override
    {
        if (! IlanaAnim::showing (*this))
            return;

        const auto target = isMouseOver() ? 1.0f : 0.0f;

        if (std::abs (hover - target) < 0.005f)
            return;

        hover = std::abs (hover - target) < 0.01f ? target : IlanaAnim::approach (hover, target, 0.22f, frameTicks());
        repaint();
    }

public:
    // A ComboBox whose popup can be replaced (e.g. by the wavetable browser).
    // (Public for the dropdown UI test.)
    struct PopupCombo : public juce::ComboBox,
                        private juce::ComboBox::Listener
    {
        PopupCombo() { addListener (this); }
        ~PopupCombo() override { removeListener (this); }

        // A list opening pictures the shown displays; the choice landing
        // fades the old pictures out over the new shapes (IlanaAnim::MorphSource).
        void comboBoxChanged (juce::ComboBox*) override { IlanaAnim::playMorphs(); }

        std::function<void()> popupOverride;
        double lastOverrideOpen = 0.0;
        double lastPopupOpen = -1.0e9; // (when the person last opened this list)
        std::function<void (int)> nudgeOverride;

        // True when a change comes within a moment of the person's pick from the list.
        bool pickedFromList() const { return juce::Time::getMillisecondCounterHiRes() - lastPopupOpen < 20000.0; }

        void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
        {
            if (! scrollPageInstead (*this, event, wheel))
                juce::ComboBox::mouseWheelMove (event, wheel);
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (nudgeOverride != nullptr)
            {
                if (key == juce::KeyPress::upKey || key == juce::KeyPress::leftKey)
                {
                    nudgeOverride (-1);
                    return true;
                }
                if (key == juce::KeyPress::downKey || key == juce::KeyPress::rightKey)
                {
                    nudgeOverride (1);
                    return true;
                }
            }
            if (popupOverride != nullptr && key == juce::KeyPress::returnKey)
            {
                openOverride();
                return true;
            }

            return juce::ComboBox::keyPressed (key);
        }

        // A select with its own menu opens it on the press only, and never
        // goes through ComboBox's "menu active" flag. The base class sets that
        // flag on the press, again on the release, and asks showPopup() to
        // open on a drag and on key repeats; the old override cleared it by
        // hidePopup() but, on a quick click, a second showPopup() landed
        // inside the 150 ms debounce and returned with the flag still set, so
        // every later click did nothing (the list never showed, though the
        // arrow keys still worked: ilana's PC report, PLAY and OSC). Taking
        // the press, release, drag and Return key here leaves the flag
        // alone for good.
        void mouseDown (const juce::MouseEvent& event) override
        {
            if (popupOverride == nullptr)
            {
                juce::ComboBox::mouseDown (event);
                return;
            }

            // (Asynchronously, as ComboBox does: the press that dismissed
            // another open menu gets to finish first.)
            if (isEnabled() && ! event.mods.isPopupMenu())
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PopupCombo> (this)]
                                                 { if (safe != nullptr) safe->openOverride(); });
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            if (popupOverride == nullptr)
                juce::ComboBox::mouseDrag (event);
        }

        void mouseUp (const juce::MouseEvent& event) override
        {
            if (popupOverride == nullptr)
                juce::ComboBox::mouseUp (event);
        }

        void showPopup() override
        {
            if (popupOverride == nullptr)
            {
                lastPopupOpen = juce::Time::getMillisecondCounterHiRes();
                IlanaAnim::armMorphs();
                juce::ComboBox::showPopup();
                return;
            }

            // (Reached by accessibility's "press" action: clear the flag it
            // set before opening our own list.)
            hidePopup();
            openOverride();
        }

        // Opens the replacement list. A press can arrive twice in a few
        // milliseconds (a touchpad tap, a double event from the host); the
        // second would close the first's menu as it opened.
        void openOverride()
        {
            const auto now = juce::Time::getMillisecondCounterHiRes();

            if (now - lastOverrideOpen < 150.0)
                return;

            lastOverrideOpen = now;
            IlanaAnim::armMorphs();
            popupOverride();
        }
    };

private:
    PopupCombo combo;
    juce::Label label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
    float hover = 0.0f;
    juce::Colour currentTint;
    bool compactLayout = false;
    bool packedMenu = false;
    juce::String parameterId;
};

// A row of choices as one segmented box (the OSC header's type, a filter route;
// review 23): the chosen segment filled in the family colour. The page says what
// is chosen and what a click does; nothing here owns a parameter.
class SegmentBar : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    std::function<int()> getChosen;
    std::function<void (int)> onChoose;

    void setItems (juce::StringArray names, std::vector<juce::String> tips = {})
    {
        items = std::move (names);
        itemTips = std::move (tips);
        repaint();
    }
    void setColour (juce::Colour newColour) { colour = newColour; repaint(); }
    const juce::StringArray& getItems() const { return items; }
    // The segments' widths: each its text and a little air (or, with `equal`, the same).
    void setEqual (bool equalWidths) { equal = equalWidths; repaint(); }

    static juce::Font segmentFont() { return IlanaTheme::font (7.8f, true).withKerningFactor (0.03f); }
    // Widths measured at a fixed size (the 10 px floor at the design zoom), so the layout
    // does not move with the zoom the page was laid out at.
    static int measure (const juce::String& text) { return juce::GlyphArrangement::getStringWidthInt (segmentFont(), text); }

    int preferredWidth() const
    {
        auto width = 6;
        for (const auto& name : items)
            width += measure (name) + 14;
        return width;
    }

    void refresh() { repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (IlanaTheme::Ui::raised);
        g.fillRoundedRectangle (bounds, 5.0f);
        g.setColour (IlanaTheme::Ui::line);
        g.drawRoundedRectangle (bounds, 5.0f, 1.0f);

        const auto chosen = getChosen != nullptr ? getChosen() : -1;
        const auto on = isEnabled() ? 1.0f : 0.5f;
        for (int i = 0; i < items.size(); ++i)
        {
            const auto cell = segmentRect (i);
            if (i == chosen)
            {
                g.setColour (colour.withMultipliedAlpha (on));
                g.fillRoundedRectangle (cell, 3.5f);
            }
            else if (i == hovered)
            {
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.fillRoundedRectangle (cell, 3.5f);
            }
            g.setColour ((i == chosen ? juce::Colour (0xff14110a) : IlanaTheme::Ui::text3).withMultipliedAlpha (on));
            g.setFont (segmentFont());
            IlanaTheme::drawFitted (g, items[i], cell.toNearestInt(), juce::Justification::centred, 1);
        }
    }

    void mouseMove (const juce::MouseEvent& event) override { setHovered (segmentAt (event.position)); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }
    void mouseDown (const juce::MouseEvent& event) override
    {
        if (! isEnabled())
            return;
        if (const auto index = segmentAt (event.position); index >= 0 && onChoose != nullptr)
        {
            IlanaAnim::armMorphs(); // (the old shapes fade out over the new: MorphSource)
            onChoose (index);
            IlanaAnim::playMorphs();
        }
        repaint();
    }

    juce::Rectangle<float> segmentRect (int index) const
    {
        const auto area = getLocalBounds().toFloat().reduced (2.5f);
        if (items.isEmpty())
            return {};
        auto total = 0.0f;
        std::vector<float> widths;
        for (const auto& name : items)
        {
            widths.push_back (equal ? 1.0f : (float) measure (name) + 14.0f);
            total += widths.back();
        }
        auto x = area.getX();
        for (int i = 0; i < index; ++i)
            x += area.getWidth() * widths[(size_t) i] / total;
        return { x, area.getY(), area.getWidth() * widths[(size_t) index] / total, area.getHeight() };
    }

    int segmentAt (juce::Point<float> position) const
    {
        for (int i = 0; i < items.size(); ++i)
            if (segmentRect (i).expanded (1.0f, 3.0f).contains (position))
                return i;
        return -1;
    }

private:
    void setHovered (int index)
    {
        if (index == hovered)
            return;
        hovered = index;
        setTooltip (juce::isPositiveAndBelow (index, (int) itemTips.size()) ? itemTips[(size_t) index] : juce::String());
        repaint();
    }

    juce::StringArray items;
    std::vector<juce::String> itemTips;
    juce::Colour colour = IlanaTheme::accent();
    bool equal = false;
    int hovered = -1;
};

// A wrapping row of small pills, one chosen (the Physical card's exciters; review 23):
// the chosen pill filled in the family colour, the rest a quiet field.
class PillWrap : public juce::Component,
                 public juce::SettableTooltipClient
{
public:
    std::function<int()> getChosen;
    std::function<void (int)> onChoose;

    static constexpr int pillH = 13, hGap = 3, vGap = 3;

    void setItems (juce::StringArray names) { items = std::move (names); repaint(); }
    void setColour (juce::Colour newColour) { colour = newColour; repaint(); }
    void refresh() { repaint(); }

    static juce::Font pillFont() { return IlanaTheme::font (7.7f, true); }
    static int measure (const juce::String& text) { return juce::GlyphArrangement::getStringWidthInt (pillFont(), text) + 9; }

    // The rectangles the pills take in a row of `width`, wrapped.
    std::vector<juce::Rectangle<int>> flow (int width) const
    {
        std::vector<juce::Rectangle<int>> rects;
        auto x = 0, y = 0;
        for (const auto& name : items)
        {
            const auto w = juce::jmin (width, measure (name));
            if (x > 0 && x + w > width)
            {
                x = 0;
                y += pillH + vGap;
            }
            rects.push_back ({ x, y, w, pillH });
            x += w + hGap;
        }
        return rects;
    }

    int heightFor (int width) const
    {
        const auto rects = flow (width);
        return rects.empty() ? 0 : rects.back().getBottom();
    }

    void paint (juce::Graphics& g) override
    {
        const auto chosen = getChosen != nullptr ? getChosen() : -1;
        const auto rects = flow (getWidth());
        const auto on = isEnabled() ? 1.0f : 0.5f;
        for (size_t i = 0; i < rects.size(); ++i)
        {
            const auto r = rects[i].toFloat().reduced (0.5f);
            const auto isChosen = (int) i == chosen;
            if (isChosen)
            {
                g.setColour (colour.withMultipliedAlpha (on));
                g.fillRoundedRectangle (r, 4.0f);
            }
            else
            {
                g.setColour ((juce::Component::isMouseOver() && (int) i == hovered ? IlanaTheme::Ui::hover : IlanaTheme::Ui::raised).withMultipliedAlpha (on));
                g.fillRoundedRectangle (r, 4.0f);
                g.setColour (IlanaTheme::Ui::line);
                g.drawRoundedRectangle (r, 4.0f, 1.0f);
            }
            g.setColour ((isChosen ? juce::Colour (0xff14110a) : IlanaTheme::Ui::text3).withMultipliedAlpha (on));
            g.setFont (pillFont());
            IlanaTheme::drawFitted (g, items[(int) i], rects[i], juce::Justification::centred, 1);
        }
    }

    void mouseMove (const juce::MouseEvent& event) override { setHovered (pillAt (event.getPosition())); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }
    void mouseDown (const juce::MouseEvent& event) override
    {
        if (const auto index = pillAt (event.getPosition()); isEnabled() && index >= 0 && onChoose != nullptr)
        {
            IlanaAnim::armMorphs();
            onChoose (index);
            IlanaAnim::playMorphs();
            repaint();
        }
    }

private:
    int pillAt (juce::Point<int> point) const
    {
        const auto rects = flow (getWidth());
        for (size_t i = 0; i < rects.size(); ++i)
            if (rects[i].expanded (1, 1).contains (point))
                return (int) i;
        return -1;
    }
    void setHovered (int index)
    {
        if (index != hovered)
        {
            hovered = index;
            repaint();
        }
    }

    juce::StringArray items;
    juce::Colour colour = IlanaTheme::accent();
    int hovered = -1;
};

// A row of small square icons, one per choice (the exciter: ilana, 2026-10-21, "like the
// Serum sub picker"): the chosen tile lit in the accent, the others dim; the hovered
// tile's name (and what it does) is its tooltip.
class IconPicker : public juce::Component,
                   public juce::TooltipClient
{
public:
    struct Item
    {
        juce::String name, tip;
        // The glyph in an 18 x 16 box: SVG path data, each with its stroke width (0 fills).
        std::vector<std::pair<juce::String, float>> paths;
    };

    std::function<int()> getChosen;
    std::function<void (int)> onChoose;

    static constexpr float tileW = 22.0f, tileH = 18.0f;

    void setItems (std::vector<Item> newItems)
    {
        items = std::move (newItems);
        glyphs.clear();
        for (const auto& item : items)
        {
            std::vector<std::pair<juce::Path, float>> parsed;
            for (const auto& [data, stroke] : item.paths)
                parsed.emplace_back (juce::Drawable::parseSVGPath (data), stroke);
            glyphs.push_back (std::move (parsed));
        }
        repaint();
    }
    void setColour (juce::Colour newColour) { colour = newColour; repaint(); }
    void refresh() { repaint(); }
    int size() const { return (int) items.size(); }

    // Each tile's rectangle: the tiles spread over the width, the gaps even.
    // When the row is too narrow for full tiles (12 exciters in a short
    // panel), every tile shrinks evenly so the last one is never cut off.
    juce::Rectangle<float> tile (int index) const
    {
        const auto n = juce::jmax (1, (int) items.size());
        const auto w = tileScale() * tileW, h = tileScale() * tileH;
        const auto gap = n > 1 ? juce::jmax (0.0f, ((float) getWidth() - (float) n * w) / (float) (n - 1)) : 0.0f;
        return { (float) index * (w + gap), ((float) getHeight() - h) * 0.5f, w, h };
    }
    float tileScale() const
    {
        const auto n = juce::jmax (1, (int) items.size());
        const auto minGap = 2.0f;
        return juce::jlimit (0.5f, 1.0f, ((float) getWidth() - (float) (n - 1) * minGap) / ((float) n * tileW));
    }

    void paint (juce::Graphics& g) override
    {
        const auto chosen = getChosen != nullptr ? getChosen() : -1;
        const auto on = isEnabled() ? 1.0f : 0.5f;
        for (int i = 0; i < (int) items.size(); ++i)
        {
            const auto r = tile (i).reduced (0.5f);
            const auto lit = i == chosen;
            const auto hover = i == hovered && isMouseOver();
            g.setColour ((lit ? IlanaTheme::Ui::well.interpolatedWith (colour, 0.2f) : IlanaTheme::Ui::well).withMultipliedAlpha (on));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour ((lit ? colour : hover ? IlanaTheme::Ui::text3 : IlanaTheme::Ui::line).withMultipliedAlpha (on));
            g.drawRoundedRectangle (r, 3.0f, 1.0f);

            const auto ink = (lit ? colour : hover ? IlanaTheme::Ui::text : IlanaTheme::Ui::text2).withMultipliedAlpha (on);
            const auto transform = juce::AffineTransform::scale (tileScale())
                                       .translated (r.getX() + 1.5f * tileScale(), r.getY() + 0.5f * tileScale());
            g.setColour (ink);
            for (const auto& [path, stroke] : glyphs[(size_t) i])
            {
                if (stroke > 0.0f)
                    g.strokePath (path, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), transform);
                else
                    g.fillPath (path, transform);
            }
        }
    }

    juce::String getTooltip() override
    {
        if (hovered >= 0 && hovered < (int) items.size())
            return items[(size_t) hovered].name + (items[(size_t) hovered].tip.isNotEmpty() ? ": " + items[(size_t) hovered].tip : juce::String());
        return {};
    }

    void mouseMove (const juce::MouseEvent& event) override { setHovered (tileAt (event.position)); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }
    void mouseDown (const juce::MouseEvent& event) override
    {
        if (const auto index = tileAt (event.position); isEnabled() && index >= 0 && onChoose != nullptr)
        {
            IlanaAnim::armMorphs();
            onChoose (index);
            IlanaAnim::playMorphs();
            repaint();
        }
    }

private:
    int tileAt (juce::Point<float> point) const
    {
        for (int i = 0; i < (int) items.size(); ++i)
            if (tile (i).expanded (1.5f, 2.0f).contains (point))
                return i;
        return -1;
    }
    void setHovered (int index)
    {
        if (index != hovered)
        {
            hovered = index;
            repaint();
        }
    }

    std::vector<Item> items;
    std::vector<std::vector<std::pair<juce::Path, float>>> glyphs;
    juce::Colour colour = IlanaTheme::accent();
    int hovered = -1;
};

class ToggleControl : public juce::Component,
                      public juce::SettableTooltipClient,
                      private IlanaAnim::FrameTimer
{
public:
    ToggleControl (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID,
                   const juce::String& labelText)
        : parameterId (parameterID)
    {
        processorRef = dynamic_cast<IlanaSynthAudioProcessor*> (&state.processor);
        button.onPopupMenu = [this] { showMenu(); };
        button.setButtonText (labelText);
        button.setClickingTogglesState (true);
        button.setColour (juce::TextButton::buttonOnColourId, IlanaTheme::accent().withAlpha (0.85f));
        addAndMakeVisible (button);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, parameterID, button);

        // Every on/off is drawn as a sliding switch (a lit and a dark button
        // were easy to misread): a card's bare "ON" switch, or a named one
        // with its name centred above it, like a knob's.
        switchAmount = button.getToggleState() ? 1.0f : 0.0f;
        button.getProperties().set ("switch", true);
        button.getProperties().set ("switchAmount", switchAmount);

        if (auto* parameter = state.getParameter (parameterID))
        {
            const auto description = describeParameter (parameterID);
            const auto tooltip = parameter->getName (64) + (description.isNotEmpty() ? "\n" + description : "");
            button.setTitle (parameter->getName (64));
            button.setDescription (description);
            button.setTooltip (tooltip);
            setTooltip (tooltip);
        }

        startTimerHz (30);
    }

    juce::TextButton& getButton() { return button; }

    // The switch in an owner's colour (an oscillator's) instead of the accent.
    void setSwitchColour (juce::Colour colour)
    {
        button.getProperties().set ("switchColour", (int) colour.getARGB());
        button.repaint();
    }

    // The name to the right of the switch, on one line (a row of knobs and
    // switches in a card: "TUNED", "LOOP").
    void setInlineLabel (bool shouldBeInline)
    {
        inlineLabel = shouldBeInline;
        resized();
        repaint();
    }

    // The name over the switch in the small size (the voice strip's LEGATO).
    void setSmallName (bool shrunk)
    {
        smallName = shrunk;
        repaint();
    }

    // The packed panels' switch (review 23): a 28 x 15 switch with its name under it, to its
    // left (small, dim) or to its right (plain text).
    enum class NamePlace { none, below, left, leftFlush, right }; // leftFlush: the name on the box's left edge, the switch on its right
    void setPackedName (NamePlace place)
    {
        if (packedPlace == place)
            return;
        packedPlace = place;
        resized();
        repaint();
    }
    NamePlace getPackedName() const { return packedPlace; }
    // The size of a name to the switch's right (7.3 px in the cards; the OSC strip's Manual tuning and the dock's LEGATO are bigger).
    void setRightNameSize (float size)
    {
        rightNameSize = size;
        repaint();
    }

    // Just the pill, filling the area (a card header's power switch).
    void setBareSwitch (bool shouldBeBare)
    {
        bareSwitch = shouldBeBare;
        resized();
    }

    // A group's name that is its own switch (the voice strip's SUB, STRINGS,
    // SOUNDBOARD): a lit dot while on, the name in the family colour, all of
    // it dimmed while off. The whole area takes the click.
    void setGroupName (const juce::String& name, juce::Colour colour)
    {
        groupColour = colour;
        groupText = name;
        groupStyle = true;
        button.getProperties().set ("groupName", true);
        button.getProperties().set ("groupColour", (int) colour.getARGB());
        button.getProperties().set ("groupText", name);
        resized();
        repaint();
    }

    bool isGroupName() const { return groupStyle; }
    juce::String getGroupText() const { return groupText; }

    bool isSwitch() const { return button.getProperties().contains ("switch"); }
    const juce::String& getParameterId() const { return parameterId; }

    // (Every toggle is a switch now; kept for the call sites that ask.)
    void showAsSwitch()
    {
        switchAmount = button.getToggleState() ? 1.0f : 0.0f;
        button.getProperties().set ("switch", true);
        button.getProperties().set ("switchAmount", switchAmount);
        repaint();
    }

    // A button that lights up flares, breathes once or twice, then holds a
    // steady glow (a glow that never settles keeps the whole UI busy).
    void paint (juce::Graphics& g) override
    {
        if (groupStyle)
            return; // (the switch draws its own dot and name: IlanaLookAndFeel)

        if (packedPlace != NamePlace::none && isSwitch())
        {
            const auto on = isEnabled() ? 1.0f : 0.5f;
            const auto b = button.getBounds();
            if (packedPlace == NamePlace::below)
            {
                g.setColour (IlanaTheme::Ui::text3.withMultipliedAlpha (on));
                g.setFont (belowNameSize > 0.0f ? IlanaTheme::font (belowNameSize, true)
                                                : IlanaTheme::font (IlanaTheme::TextSize::packedLabel, true).withKerningFactor (0.08f));
                IlanaTheme::drawFitted (g, button.getButtonText(), juce::Rectangle<int> { 0, b.getBottom() + 1 + nameDrop, getWidth(), juce::jmax (0, getHeight() - b.getBottom() - 1 - nameDrop) }, juce::Justification::centredTop, 1);
            }
            else if (packedPlace == NamePlace::left || packedPlace == NamePlace::leftFlush)
            {
                g.setColour (IlanaTheme::Ui::text3.withMultipliedAlpha (on));
                g.setFont (IlanaTheme::font (IlanaTheme::TextSize::packedLabel, true).withKerningFactor (0.08f));
                IlanaTheme::drawFitted (g, button.getButtonText(), juce::Rectangle<int> { 0, b.getY(), juce::jmax (0, b.getX() - 3), b.getHeight() }, packedPlace == NamePlace::leftFlush ? juce::Justification::centredLeft : juce::Justification::centredRight, 1);
            }
            else
            {
                g.setColour (IlanaTheme::Ui::text.withMultipliedAlpha (on));
                g.setFont (IlanaTheme::font (rightNameSize));
                IlanaTheme::drawFitted (g, button.getButtonText(), juce::Rectangle<int> { b.getRight() + 5, b.getY(), juce::jmax (0, getWidth() - b.getRight() - 5), b.getHeight() }, juce::Justification::centredLeft, 1);
            }
            return;
        }

        if (inlineLabel && isSwitch())
        {
            g.setColour (IlanaTheme::Ui::text3);
            g.setFont (IlanaTheme::font (IlanaTheme::TextSize::tiny, true).withKerningFactor (0.07f));
            IlanaTheme::drawFitted (g, button.getButtonText(), getLocalBounds().withTrimmedLeft (inlineDialColumn() + 3),
                                    juce::Justification::centredLeft, 1);
            return;
        }

        // A named switch shows its name where other controls show a label.
        if (isSwitch() && button.getButtonText() != "ON")
        {
            // (Named as a knob is: the sheet's .lbl, bold tracked caps in t3.)
            g.setColour (IlanaTheme::Ui::text3);
            g.setFont (IlanaTheme::font (smallName ? IlanaTheme::TextSize::tiny : IlanaTheme::TextSize::label, true).withKerningFactor (0.07f));
            g.drawText (button.getButtonText(), getLocalBounds().withHeight (smallName ? 11 : 13), juce::Justification::centred, true);
            return;
        }

        if (isSwitch() || ! button.getToggleState())
            return;

        const auto settle = juce::jlimit (0.0f, 1.0f, litSeconds / breathSeconds);
        const auto breath = 0.5f + 0.5f * std::cos (litSeconds * 4.2f);
        const auto pulse = juce::jmap (settle, breath, 0.6f);
        IlanaTheme::paintGlow (g, button.getBounds().toFloat().reduced (0.5f), 5.0f,
                               button.findColour (juce::TextButton::buttonOnColourId).withAlpha (1.0f), 0.4f + 0.6f * pulse);
    }

    void resized() override
    {
        auto area = getLocalBounds();

        if (groupStyle || bareSwitch)
        {
            button.setBounds (area);
            return;
        }

        if (packedPlace != NamePlace::none)
        {
            const auto w = packedPlace == NamePlace::leftFlush ? 22 : 28, h = packedPlace == NamePlace::leftFlush ? 13 : 15; // (leftFlush: the mock's slimmer switch)
            button.setBounds (packedPlace == NamePlace::below ? juce::Rectangle<int> (w, h).withPosition (area.getCentreX() - w / 2, area.getY())
                              : packedPlace == NamePlace::left || packedPlace == NamePlace::leftFlush ? juce::Rectangle<int> (w, h).withPosition (area.getRight() - w, area.getCentreY() - h / 2)
                                                               : juce::Rectangle<int> (w, h).withPosition (area.getX(), area.getCentreY() - h / 2));
            return;
        }

        if (inlineLabel)
        {
            // (The switch centres on the dial column of the knobs beside it, its name on their names' line.)
            button.setBounds (area.removeFromLeft (inlineDialColumn()).withSizeKeepingCentre (inlineSwitchWidth, juce::jmin (24, area.getHeight())));
            return;
        }

        area.removeFromTop (13 + switchDrop);
        button.setBounds (area.removeFromTop (juce::jmin (24, juce::jmax (16, area.getHeight()))));
    }

    // The packed switch's name lower under it, level with a knob row's names.
    void setNameDrop (int pixels) { nameDrop = pixels; repaint(); }
    // The name under the switch at a ring knob's name size, so it reads as one row with them (0: the packed label).
    void setBelowNameSize (float size) { belowNameSize = size; repaint(); }

    // Lowers the switch under its name, to sit level with the dials of the
    // knobs in its row (S13-1); 0 is the usual place.
    void setSwitchDrop (int pixels)
    {
        if (switchDrop == pixels)
            return;
        switchDrop = juce::jmax (0, pixels);
        resized();
    }



private:
    int switchDrop = 0, nameDrop = 0;
    bool inlineLabel = false, groupStyle = false, bareSwitch = false, smallName = false;
    NamePlace packedPlace = NamePlace::none;
    float rightNameSize = 7.3f, belowNameSize = 0.0f;
    juce::Colour groupColour;
    juce::String groupText;
    static constexpr int inlineSwitchWidth = 34;
    int inlineDialColumn() const { return juce::jmax (inlineSwitchWidth, juce::jmin (getHeight(), 40)); }

    void timerCallback() override
    {
        // Hidden: no animation, but a switch keeps its position, so it isn't
        // shown in a stale state (or slides) when its page opens.
        if (! IlanaAnim::showing (*this))
        {
            if (isSwitch())
            {
                const auto target = button.getToggleState() ? 1.0f : 0.0f;

                if (switchAmount != target)
                {
                    switchAmount = target;
                    button.getProperties().set ("switchAmount", switchAmount);
                }
            }

            return;
        }

        hover = IlanaAnim::approach (hover, isMouseOver() ? 1.0f : 0.0f, 0.22f, frameTicks());

        const auto on = button.getToggleState();
        auto switchMoving = false;
        const auto breathing = on && ! isSwitch() && litSeconds < breathSeconds;

        // Only a button the user switches on breathes, not one lit by a
        // preset load, undo or automation.
        if (on && ! lastOn && ticked && button.isMouseOverOrDragging())
            litSeconds = 0.0f;
        else if (breathing)
            litSeconds += frameSeconds();

        if (isSwitch())
        {
            const auto target = on ? 1.0f : 0.0f;
            switchMoving = std::abs (switchAmount - target) > 0.001f;
            switchAmount = switchMoving && IlanaAnim::fullMotion() ? IlanaAnim::approach (switchAmount, target, 0.3f, frameTicks()) : target;
            button.getProperties().set ("switchAmount", switchAmount);

            if (switchMoving)
                button.repaint();
        }

        if (breathing || on != lastOn || switchMoving || (hover > 0.01f && hover < 0.99f))
            repaint();

        lastOn = on;
        ticked = true;
    }

    // Right-click opens the switch's menu (MIDI learn) instead of flipping it.
    struct SwitchButton : public juce::TextButton
    {
        std::function<void()> onPopupMenu;

        void mouseDown (const juce::MouseEvent& event) override
        {
            if (event.mods.isPopupMenu() && onPopupMenu != nullptr)
                onPopupMenu();
            else
                juce::TextButton::mouseDown (event);
        }

        void mouseUp (const juce::MouseEvent& event) override
        {
            if (! (event.mods.isPopupMenu() && onPopupMenu != nullptr))
                juce::TextButton::mouseUp (event);
        }
    };

    void showMenu()
    {
        auto* parameter = processorRef != nullptr ? processorRef->apvts.getParameter (parameterId) : nullptr;

        if (parameter == nullptr || ! parameter->isAutomatable())
            return;

        juce::PopupMenu menu;
        addMidiLearnItems (menu, *processorRef, parameterId);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&button),
                            [safeThis = juce::Component::SafePointer<ToggleControl> (this)] (int result)
                            {
                                if (safeThis != nullptr && safeThis->processorRef != nullptr)
                                    handleMidiLearnResult (result, *safeThis->processorRef, safeThis->parameterId);
                            });
    }

    SwitchButton button;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    IlanaSynthAudioProcessor* processorRef = nullptr;
    juce::String parameterId;
    static constexpr float breathSeconds = 2.2f;
    float litSeconds = breathSeconds;
    float hover = 0.0f;
    float switchAmount = 0.0f;
    bool lastOn = false, ticked = false;
};


// A compact bar-slider row for a card's list of switches (the sample's CROSSFADE): a rounded
// box 16 units high, the share filled in the card's colour, the name on its left edge and the
// value on its right, one drag or a double-click for the default.
class BarSliderControl : public KeyboardSlider
{
public:
    BarSliderControl (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID, const juce::String& nameText)
        : name (nameText)
    {
        setSliderStyle (juce::Slider::LinearBar);
        setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, parameterID, *this);

        if (auto* parameter = state.getParameter (parameterID))
        {
            setDoubleClickReturnValue (true, parameter->convertFrom0to1 (parameter->getDefaultValue()));
            const auto description = describeParameter (parameterID);
            setTitle (parameter->getName (64));
            setDescription (description);
            setTooltip (parameter->getName (64) + (description.isNotEmpty() ? "\n" + description : ""));
        }

    }

    void setAccent (juce::Colour c) { accent = c; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto box = getLocalBounds().toFloat().reduced (0.5f);
        const auto alpha = isEnabled() ? 1.0f : 0.5f;
        const auto share = (float) juce::jlimit (0.0, 1.0, valueToProportionOfLength (getValue()));
        g.setColour (IlanaTheme::Ui::raised);
        g.fillRoundedRectangle (box, 4.0f);

        if (share > 0.001f)
        {
            g.saveState();
            g.reduceClipRegion (juce::Rectangle<float> (box.getX(), box.getY(), box.getWidth() * share, box.getHeight()).getSmallestIntegerContainer());
            g.setColour (accent.withAlpha (0.38f * alpha));
            g.fillRoundedRectangle (box, 4.0f);
            g.restoreState();
        }

        g.setColour (IlanaTheme::Ui::line);
        g.drawRoundedRectangle (box, 4.0f, 1.0f);

        // The name (or, while the pointer is on the bar or dragging, the value) centred in the bar
        // by its ink: the caps' middle on the bar's middle.
        const auto text = isMouseOverOrDragging() ? juce::String (juce::roundToInt (getValue() * 100.0)) + "%" : name;
        const auto font = isMouseOverOrDragging() ? IlanaTheme::font (IlanaTheme::TextSize::packedLabel) : IlanaTheme::font (IlanaTheme::TextSize::packedLabel, true).withKerningFactor (0.08f);
        g.setFont (font);
        g.setColour ((isMouseOverOrDragging() ? IlanaTheme::Ui::text : IlanaTheme::Ui::text3.interpolatedWith (IlanaTheme::Ui::text, share * 0.6f)).withMultipliedAlpha (alpha));
        const juce::Font f (font);
        const auto capMiddle = f.getAscent() - f.getHeight() * 0.35f; // (the caps' middle below the box top)
        const auto top = (float) getHeight() * 0.5f - capMiddle + inkNudge;
        g.drawText (text, juce::Rectangle<float> (0.4f, top, (float) getWidth(), f.getHeight()), juce::Justification::centred, false);
    }

private:
    static constexpr float inkNudge = -0.75f; // (the font box sits the caps a little low)
    void mouseEnter (const juce::MouseEvent& e) override { KeyboardSlider::mouseEnter (e); repaint(); }
    void mouseExit (const juce::MouseEvent& e) override { KeyboardSlider::mouseExit (e); repaint(); }

    juce::String name;
    juce::Colour accent { juce::Colours::orange };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

class ValueSliderControl : public juce::Component
{
public:
    ValueSliderControl (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID) : parameterId (parameterID)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 16);
        slider.setColour (juce::Slider::rotarySliderFillColourId, IlanaTheme::accent());
        addAndMakeVisible (slider);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, parameterID, slider);

        if (auto* parameter = state.getParameter (parameterID))
        {
            slider.setDoubleClickReturnValue (true, parameter->convertFrom0to1 (parameter->getDefaultValue()));

            const auto description = describeParameter (parameterID);
            slider.setTitle (parameter->getName (64));
            slider.setDescription (description);
            slider.setTooltip (parameter->getName (64) + (description.isNotEmpty() ? "\n" + description : ""));
        }
    }

    void resized() override { slider.setBounds (getLocalBounds()); }

    juce::Slider& getSlider() { return slider; }
    const juce::String& getParameterId() const { return parameterId; }

private:
    juce::String parameterId;
    KeyboardSlider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

// One dimming rule for every page (UI review 4, V12 and S10). A control
// declares when it has an effect (SPEC AMT while SPECTRAL is on, DETUNE
// with more than one unison voice...), and the page calls apply() on its
// timer: a control with no effect is drawn at IlanaTheme::dimmedAlpha (its
// arc goes grey) and says why in its tooltip, but stays usable, so it can be
// set up before it is switched in. A control the page has disabled (its
// section is off) is left as the page set it.
class EffectRules
{
public:
    explicit EffectRules (const IlanaSynthAudioProcessor& p) : processor (p) {}

    using Condition = std::function<bool()>;

    // `hasEffect` decides; `why` (shown in a knob's tooltip while it doesn't
    // act) names what to change, e.g. "SPECTRAL is Off".
    void add (juce::Component& control, Condition hasEffect, const juce::String& why = {})
    {
        rules.push_back ({ &control, std::move (hasEffect), why, {} });
    }

    // The same, for a control whose whole module is switched off: drawn at
    // `offAlpha()` (e.g. FilterColours::offAlpha, lighter than one dimmed
    // control) instead of IlanaTheme::dimmedAlpha while it doesn't act.
    void add (juce::Component& control, Condition hasEffect, const juce::String& why, std::function<float()> offAlpha)
    {
        rules.push_back ({ &control, std::move (hasEffect), why, std::move (offAlpha) });
    }

    // Conditions on a parameter's plain value.
    Condition isOn (const juce::String& id) const { return [this, id] { return read (id) > 0.5f; }; }
    Condition isOff (const juce::String& id) const { return [this, id] { return read (id) < 0.5f; }; }
    Condition isAbove (const juce::String& id, float threshold) const { return [this, id, threshold] { return read (id) > threshold; }; }
    Condition choiceIsNot (const juce::String& id, int index) const
    {
        return [this, id, index] { return juce::roundToInt (read (id)) != index; };
    }

    void apply()
    {
        for (auto& rule : rules)
        {
            if (! rule.control->isEnabled())
                continue;

            const auto acts = rule.hasEffect();
            const auto alpha = acts ? 1.0f : rule.offAlpha != nullptr ? rule.offAlpha() : IlanaTheme::dimmedAlpha;

            // (A card switched on or off fades its controls over 180 ms.)
            IlanaAnim::fadeAlpha (*rule.control, alpha);

            if (auto* knob = dynamic_cast<KnobControl*> (rule.control))
                knob->setInactiveNote (acts ? juce::String() : rule.why);
        }
    }

    // How many controls are dimmed by a rule right now (the UI test).
    int numInactive() const
    {
        auto count = 0;
        for (const auto& rule : rules)
            count += rule.control->isEnabled() && ! rule.hasEffect() ? 1 : 0;
        return count;
    }

private:
    float read (const juce::String& id) const
    {
        const auto* value = processor.apvts.getRawParameterValue (id);
        return value != nullptr ? value->load() : 0.0f;
    }

    struct Rule
    {
        juce::Component* control;
        Condition hasEffect;
        juce::String why;
        std::function<float()> offAlpha;
    };

    const IlanaSynthAudioProcessor& processor;
    std::vector<Rule> rules;
};

// The height a control needs at a given width (label, then dial and value,
// or the box/button), or -1 when it takes whatever it's given.
inline int preferredControlHeight (juce::Component* item, int width)
{
    if (auto* knob = dynamic_cast<KnobControl*> (item))
    {
        if (knob->isCompact())
            return -1;

        const auto labelHeight = knob->labelBlockHeight();
        return labelHeight + juce::jlimit (IlanaTheme::KnobSize::minimum, knob->getMaxDial(), width) + 16;
    }

    if (dynamic_cast<ComboControl*> (item) != nullptr || dynamic_cast<ToggleControl*> (item) != nullptr)
        return 13 + 24;

    return -1;
}

// Controls side by side in equal columns. When the row is taller than its
// controls need, they sit as one band centred in it (labels on one line)
// rather than hugging the top with the spare space below. oneLabelLine keeps
// menus' and switches' names on the knobs' label line too (their boxes right
// under), for a grid read by its labels (SEQ's GENERATE). menuWeight > 1
// gives each menu that much of a column against the others' one, for rows
// whose menus hold long names beside short knob labels (FX algorithms).
inline void layoutRow (juce::Rectangle<int> area, const std::vector<juce::Component*>& items, bool oneLabelLine = false,
                       float menuWeight = 1.0f)
{
    if (items.empty())
        return;

    auto menus = 0;
    for (auto* item : items)
        menus += dynamic_cast<ComboControl*> (item) != nullptr ? 1 : 0;
    if (menus == (int) items.size())
        menuWeight = 1.0f;

    // A menu whose text won't fit its column at the interactive size takes
    // more of the row (up to 1.6 columns), so the row's menus read at one
    // size instead of one being squeezed (UI review 9, V9-11).
    if (menus > 0 && menuWeight < 1.6f)
    {
        const auto baseShares = (float) ((int) items.size() - menus) + (float) menus * menuWeight;
        const auto baseWidth = (float) area.getWidth() / baseShares;
        const juce::Font font (IlanaTheme::font (IlanaTheme::TextSize::minInteractive));
        auto needed = menuWeight;

        for (auto* item : items)
            if (auto* combo = dynamic_cast<ComboControl*> (item))
            {
                // (Sized for its longest item, so picking another doesn't
                // squeeze the text; a long list is judged by its shown text.)
                auto widest = (float) juce::GlyphArrangement::getStringWidth (font, combo->getComboBox().getText());
                const auto& box = combo->getComboBox();
                if (box.getNumItems() <= 40)
                    for (int i = 0; i < box.getNumItems(); ++i)
                        widest = juce::jmax (widest, (float) juce::GlyphArrangement::getStringWidth (font, box.getItemText (i)));
                if (widest > 0.0f)
                    needed = juce::jmax (needed, (widest + 6.0f + 30.0f) / juce::jmax (1.0f, baseWidth));
            }

        menuWeight = juce::jlimit (menuWeight, 1.6f, needed);
    }

    const auto shares = (float) ((int) items.size() - menus) + (float) menus * menuWeight;
    const auto width = (int) ((float) area.getWidth() / shares);
    const auto menuWidth = (int) ((float) width * menuWeight);
    auto band = 0;

    // One label size for the row (V9-6): the smallest any of its names needs
    // in its cell, so no label is shrunk on its own beside bigger ones.
    {
        std::vector<std::pair<juce::Label*, float>> needs;
        auto smallest = 1000.0f;

        for (auto* item : items)
        {
            juce::Label* name = nullptr;

            if (auto* knob = dynamic_cast<KnobControl*> (item))
                name = knob->isCompact() ? nullptr : &knob->getNameLabel();
            else if (auto* combo = dynamic_cast<ComboControl*> (item))
                name = &combo->getNameLabel();

            if (name == nullptr || name->getText().isEmpty())
                continue;

            // (The room the label draws in: its cell less the cell's 3 px a side
            // and the label's own 1 px a side, so no label is cut down again
            // below the row's size when drawn.)
            const auto cell = (float) (dynamic_cast<ComboControl*> (item) != nullptr ? menuWidth - 5 : width - 2) - 6.0f;
            const auto need = IlanaTheme::fittedFont (juce::Font (IlanaTheme::font (IlanaTheme::TextSize::body)), name->getText().trim(), cell,
                                                      IlanaTheme::TextSize::minPassive).getHeight();
            needs.push_back ({ name, need });
            smallest = juce::jmin (smallest, need);
        }

        for (auto& entry : needs)
            entry.first->getProperties().set ("fitCap", smallest);
    }

    // (A null item is an empty column, so rows can share one grid.)
    for (auto* item : items)
    {
        if (item == nullptr)
            continue;

        const auto height = preferredControlHeight (item, width - 6);

        if (height < 0)
        {
            band = -1;
            break;
        }

        band = juce::jmax (band, height);
    }

    if (band > 0 && band + 6 < area.getHeight())
        area = area.withSizeKeepingCentre (area.getWidth(), band + 6);

    // Menus and switches beside full-size knobs sit with their box level
    // with the dials' centres (their names drop with them).
    auto hasKnob = false;
    auto largestDial = IlanaTheme::KnobSize::minimum;
    for (auto* item : items)
        if (auto* knob = dynamic_cast<KnobControl*> (item))
        {
            hasKnob = hasKnob || (! knob->isCompact() && knob->getLabelText().isNotEmpty());
            largestDial = juce::jmax (largestDial, knob->getMaxDial());
        }

    // (The dial is sized as KnobControl sizes it: by the cell's width or
    // height, whichever is tighter, up to its role's size; a small dial
    // barely drops anything.)
    const auto dialSize = juce::jlimit (IlanaTheme::KnobSize::minimum, largestDial, juce::jmin (width - 6, area.getHeight() - 6 - 13 - 16));
    const auto dialDrop = hasKnob && ! oneLabelLine ? juce::jmax (0, dialSize / 2 - 12) : 0;

    for (auto* item : items)
    {
        auto cell = area.removeFromLeft (dynamic_cast<ComboControl*> (item) != nullptr ? menuWidth : width).reduced (3);

        if (item == nullptr)
            continue;

        if (dialDrop > 0 && (dynamic_cast<ToggleControl*> (item) != nullptr || dynamic_cast<ComboControl*> (item) != nullptr))
            cell = cell.withTrimmedTop (dialDrop);

        item->setBounds (cell);
    }
}
