// Dev tool: renders the plugin editor offscreen and writes one PNG per tab,
// so the UI can be reviewed without a DAW or a display session.
//   ilanaSnapshot <output dir> [factory preset index]
//   ilanaSnapshot --uitest     (drives the editor and checks the wiring)
//   ilanaSnapshot --idlecpu    (UI cost of each page while nothing moves)

#if __linux__
 #include <dlfcn.h>
#endif
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <iostream>
#include <functional>
#include <typeinfo>
#if defined (__GNUC__)
 #include <cxxabi.h>
#endif
#include <map>
#include <set>
#include <thread>

#include "PluginProcessor.h"
#include "gui/HeaderWidgets.h"
#include "gui/CardTabs.h"
#include "gui/FilterWidgets.h"
#include "gui/VectorPad.h"
#include "gui/PhysicalView.h"
#include "gui/EnvThumbs.h"
#include "gui/TableBrowser.h"
#include "gui/WavetableEditor.h"
#include "PluginEditor.h"
#include "gui/SectionColourPicker.h"
#include "gui/EnvelopeDisplay.h"
#include "gui/FmWidgets.h"
#include "gui/FmDiagram.h"
#include "gui/OperatorEnvDisplay.h"
#include "gui/OperatorPoolCards.h"
#include "gui/LfoThumbs.h"
#include "gui/ModulationCardView.h"
#include "gui/MatrixWidgets.h"
#include "gui/FilterDisplay.h"
#include "gui/OutputView.h"
#include "gui/ParamControls.h"
#include "gui/SubTabBar.h"
#include "gui/TutorialOverlay.h"
#include "gui/WaveDisplay.h"
#include "gui/ModHoverPopup.h"
#include "gui/StateTabs.h"
#include "gui/ClipEditor.h"
#include "gui/ConfirmOverlay.h"
#include "Dx7Banks.h"
#include "gui/LfoSimView.h"
#include "gui/LogoComponent.h"
#include "gui/FxCarousel.h"
#include "gui/PatchCableView.h"
#include "gui/FxLibrary.h"
#include "processor/PatchGraphState.h"
#include "gui/LfoDisplay.h"
#include "gui/LfoShapeMenu.h"
#include "gui/SequencerEditors.h"
#include "gui/RemapEditor.h"
#include "gui/FxDisplays.h"
#include "gui/FxLibrary.h"

namespace
{
template <typename Type>
void findAll (juce::Component& parent, std::vector<Type*>& found)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* match = dynamic_cast<Type*> (child))
            found.push_back (match);

        findAll<Type> (*child, found);
    }
}

template <typename Type>
Type* findChild (juce::Component& parent)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* match = dynamic_cast<Type*> (child))
            return match;

        if (auto* nested = findChild<Type> (*child))
            return nested;
    }

    return nullptr;
}

void settle (int milliseconds)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
}

// Snapshot mode plays a held note so the live views (scope, modulation
// markers) have something to show; this runs the audio before each capture.
std::function<void()>& beforeSave()
{
    static std::function<void()> callback;
    return callback;
}

void save (juce::Component& editor, const juce::File& file)
{
    if (beforeSave() != nullptr)
        beforeSave()();

    // ILANA_SNAPSHOT_SCALE: the pixel scale of the PNGs (1.5 by default; 3 for close-ups).
    static const auto scale = (float) juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_SCALE", "1.5").getDoubleValue();
    const auto pixels = scale > 0.1f ? scale : 1.5f;
    // The text is sized for the zoom it draws at, as in the host (a 1.5x PNG of a 100 % editor showed the 100 %
    // floors and drew the small OSC card text bigger than ilana's 175 % window, 2026-10-23).
    const auto zoom = IlanaTheme::uiScaleRef();
    IlanaTheme::uiScaleRef() = zoom * pixels;
    IlanaTheme::notifyUiScaleListeners();
    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, pixels);
    IlanaTheme::uiScaleRef() = zoom;
    IlanaTheme::notifyUiScaleListeners();
    file.deleteFile();
    juce::FileOutputStream stream (file);
    juce::PNGImageFormat().writeImageToStream (image, stream);
    std::cout << file.getFullPathName() << std::endl;
}
int uiFailures = 0;

// isShowing() needs a window on screen; the editor here is offscreen, so
// walk the visibility chain instead.
bool visibleInTree (const juce::Component* component)
{
    // The editor itself is never put on screen here, so stop below it.
    for (; component != nullptr && component->getParentComponent() != nullptr; component = component->getParentComponent())
        if (! component->isVisible())
            return false;

    return true;
}

void expect (bool condition, const juce::String& message)
{
    std::cout << (condition ? "PASS: " : "FAIL: ") << message << std::endl;

    if (! condition)
        ++uiFailures;
}

#include "ModulationUiTests.h"
#include "FilterFxUiTests.h"
#include "ModulationReview8Tests.h"
#include "LayoutUiTests8.h"
#include "LayoutUiTests9.h"
#include "GlobalUiTests.h"
#include "DropdownUiTests.h"
#include "PatchCableUiTests.h"
#include "OscRoutingUiTests.h"
#include "OscSlotUiTests.h"
#include "WorkflowUiTests.h"
#include "FxLfoUiTests.h"
#include "FxCarouselUiTests.h"
#include "LayoutUiTests10.h"
#include "OperatorUiTests.h"
#include "OscTypeUiTests.h"
#include "PlayPageUiTests.h"
#include <set>
#include "HitReachUiTests.h"
#include "OscCardOverlapTests.h"
#include "Review9T2Tests.h"

// UI review 4, batch H: the tour, text sizes, the scope and meters, spelled-out
// labels and SEQ GENERATE's grid.
// UI review 7, package Q4: PLAY's strips, the OSC card of an operator, SF2
// zones, the exciters, BODY's names, VECTOR and PHYSICAL.
void runPlayOscReview7Tests (IlanaSynthAudioProcessor& processor, IlanaSynthAudioProcessorEditor& editor)
{
    const auto loadNamed = [&processor] (const juce::String& name)
    {
        processor.loadFactoryPreset (processor.getFactoryPresetNames().indexOf (name));
        settle (500);
    };
    const auto setParam = [&processor] (const juce::String& id, float plain)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
    };
    const auto knobFor = [&editor] (const juce::String& id, const juce::String& label = {}) -> KnobControl*
    {
        std::vector<KnobControl*> knobs;
        findAll<KnobControl> (editor, knobs);
        for (auto* knob : knobs)
            if (knob->getParameterId() == id && visibleInTree (knob) && knob->getWidth() > 0
                && (label.isEmpty() || knob->getLabelText() == label))
                return knob;
        return nullptr;
    };
    const auto centreX = [&editor] (juce::Component* c) { return c == nullptr ? -1 : editor.getLocalArea (c, c->getLocalBounds()).getCentreX(); };
    const auto buttonNamed = [&editor] (const juce::String& text) -> juce::Button*
    {
        std::vector<juce::Button*> buttons;
        findAll<juce::Button> (editor, buttons);
        for (auto* button : buttons)
            if (visibleInTree (button) && button->getButtonText() == text)
                return button;
        return nullptr;
    };
    const auto selectOscTab = [&editor] (int osc)
    {
        std::vector<StateTabs*> rows;
        findAll<StateTabs> (editor, rows);
        for (auto* tabs : rows)
            for (int i = 0; i < tabs->getNumItems(); ++i)
                if (tabs->getItem (i).name == "OSC " + juce::String (osc + 1) && tabs->onSelect != nullptr
                    && (dynamic_cast<OscPicker*> (tabs) == nullptr || tabs->getName() == "OSC tabs"))
                {
                    tabs->setSelected (i);
                    tabs->onSelect (i);
                    settle (200);
                }
    };
    const auto oscTabState = [&editor] (int osc)
    {
        std::vector<StateTabs*> rows;
        findAll<StateTabs> (editor, rows);
        for (auto* tabs : rows)
            for (int i = 0; i < tabs->getNumItems(); ++i)
                if (tabs->getItem (i).name == "OSC " + juce::String (osc + 1) && (dynamic_cast<OscPicker*> (tabs) == nullptr || tabs->getName() == "OSC tabs"))
                    return tabs->getItem (i).state;
        return juce::String();
    };
    const auto oscWave = [&editor] (int osc) -> WaveDisplay*
    {
        std::vector<WaveDisplay*> waves;
        findAll<WaveDisplay> (editor, waves);
        for (auto* wave : waves)
            if (visibleInTree (wave) && wave->getOscIndex() == osc && ! wave->isCompact())
                return wave;
        return nullptr;
    };

    // PLAY: no empty slots, one slim ADD OSC row, the knobs in one order
    // whatever the mode or FM role (V7-4, V7-26, S7-2, S7-3).
    loadNamed ("Neuro Wobble");
    editor.showPage ("MAIN");
    settle (400);
    {
        // (OSC 3's parameters are "sub_".)
        const juce::String osc3 (OscillatorIds::prefixes[2]);
        // (OSC 3 is off, so folded: UI review 8, V8-17; OSC 2 is the plain one.)
        auto* add = buttonNamed ("+  ADD OSC 4");
        auto* level1 = knobFor ("osc1_level");
        auto* level3 = knobFor ("osc2_level");
        const auto addArea = add != nullptr ? editor.getLocalArea (add, add->getLocalBounds()) : juce::Rectangle<int>();
        const auto level3Area = level3 != nullptr ? editor.getLocalArea (level3, level3->getLocalBounds()) : juce::Rectangle<int>();
        // (Past three cards the add row sits under them and PLAY's oscillator area scrolls to it, ilana 2026-10-23.)
        expect (add != nullptr && level3 != nullptr && knobFor (osc3 + "_level") != nullptr,
                "PLAY: three oscillator cards (OSC 3 shown, dimmed while off) and the + ADD OSC 4 row under them");
        // (ilana 2026-10-23, "i cant scroll to add another osc": scrolled to the end, the whole row shows.)
        if (auto* view = add != nullptr ? add->findParentComponentOfClass<juce::Viewport>() : nullptr)
        {
            view->setViewPosition (0, 1 << 20);
            settle (100);
            const auto shownRow = editor.getLocalArea (add, add->getLocalBounds());
            const auto shownView = editor.getLocalArea (view, view->getLocalBounds());
            expect (shownView.contains (shownRow), "PLAY: scrolled to the end, the + ADD OSC 4 row is in view (" + shownRow.toString() + " in " + shownView.toString() + ")");
            view->setViewPosition (0, 0);
            settle (100);
        }
        expect (level1 != nullptr && level3 != nullptr && centreX (level1) == centreX (level3)
                    && centreX (knobFor ("osc1_semi")) == centreX (knobFor ("osc2_semi")),
                "PLAY: SEMI, LEVEL in the same columns on every wavetable card");
        // SUB and NOISE: two cards, each with its own ON switch, VOLUME knob
        // and controls (SUB: wave menu, octave, COLOUR; NOISE: six types).
        {
            std::vector<ToggleControl*> toggles;
            findAll<ToggleControl> (editor, toggles);
            auto subSwitch = false, noiseSwitch = false;
            for (auto* toggle : toggles)
                if (visibleInTree (toggle))
                {
                    subSwitch = subSwitch || toggle->getTooltip().startsWith (processor.apvts.getParameter ("subosc_on")->getName (64));
                    noiseSwitch = noiseSwitch || toggle->getTooltip().startsWith (processor.apvts.getParameter ("noise_on")->getName (64));
                }
            auto* subVolume = knobFor ("subosc_level");
            auto* subColour = knobFor ("sub_colour", "COLOUR");
            auto* noise = knobFor ("noise_level");
            // (ilana's arranger pass, 2026-10-29: SUB's LEVEL and COLOUR in one row, the NOISE card under SUB.)
            expect (subSwitch && noiseSwitch && subVolume != nullptr && subColour != nullptr && noise != nullptr && centreX (subColour) > centreX (subVolume)
                        && editor.getLocalArea (noise, noise->getLocalBounds()).getY() > editor.getLocalArea (subVolume, subVolume->getLocalBounds()).getBottom()
                        && std::abs (editor.getLocalArea (subColour, subColour->getLocalBounds()).getCentreY()
                                     - editor.getLocalArea (subVolume, subVolume->getLocalBounds()).getCentreY()) < 2,
                    "PLAY: SUB and NOISE each have a switch and a LEVEL knob, SUB's COLOUR beside its LEVEL, NOISE under SUB");
        }

        // The same controls at three window sizes: no two overlap, none is cut off.
        for (const auto width : { 795, 1060, 1400 })
        {
            editor.setSize (width, width * 720 / 1060);
            settle (300);
            std::vector<juce::Rectangle<int>> areas;
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (editor, knobs);
            for (auto* knob : knobs)
                if (visibleInTree (knob) && (knob->getParameterId() == "subosc_level" || knob->getParameterId() == "sub_colour" || knob->getParameterId() == "noise_level"))
                    areas.push_back (knob->isPacked() ? editor.getLocalArea (knob, knob->packedCell()) : editor.getLocalArea (knob, knob->getLocalBounds()));
            std::vector<ToggleControl*> toggles;
            findAll<ToggleControl> (editor, toggles);
            for (auto* toggle : toggles)
                if (visibleInTree (toggle) && (toggle->getTooltip().startsWith (processor.apvts.getParameter ("subosc_on")->getName (64))
                                               || toggle->getTooltip().startsWith (processor.apvts.getParameter ("noise_on")->getName (64))))
                    areas.push_back (editor.getLocalArea (toggle, toggle->getLocalBounds()));
            auto overlap = false;
            juce::String pairs;
            for (size_t i = 0; i < areas.size(); ++i)
                for (size_t j = i + 1; j < areas.size(); ++j)
                    if (areas[i].reduced (2).intersects (areas[j].reduced (2)))
                    {
                        overlap = true;
                        pairs << areas[i].toString() << " x " << areas[j].toString() << "; ";
                    }
            expect (areas.size() == 5 && ! overlap, "PLAY at " + juce::String (width) + " px: SUB and NOISE's switches and knobs are all there and none overlaps ("
                                                       + juce::String ((int) areas.size()) + " found " + pairs + ")");
        }
        editor.setSize (1060, 720);
        settle (300);

        // A wavetable in an FM route reads its part in the FM diagram's
        // words, operator or not (I8-19).
        editor.showPage ("OSC");
        settle (300);
        // (Only the chosen tab carries its role: one rule on every page, I12-2.)
        selectOscTab (0);
        const auto firstState = oscTabState (0), otherState = oscTabState (1);
        selectOscTab (1);
        // (The tag is the engine, never the role: I13-2.)
        expect (firstState == "WAVETABLE" && otherState.isEmpty() && oscTabState (1) == "WAVETABLE",
                "OSC: the chosen tab's tag is its engine on every patch (" + firstState + ", " + oscTabState (1) + ")");
        selectOscTab (0);
        // A DX7 operator is OSC n and its kind is OPERATOR (I13-1, I13-2).
        loadNamed ("E.PIANO 1 (ROM1A)");
        editor.showPage ("OSC");
        settle (400);
        selectOscTab (0);
        expect (oscTabState (0) == "FM / DX7", "OSC: a DX7 operator's tab tag is its type, FM / DX7 (" + oscTabState (0) + ")");
        loadNamed ("Neuro Wobble");
        editor.showPage ("OSC");
        settle (300);
    }

    // A DX7 voice: each strip shows the operator's OUTPUT in dB (the FM
    // card's), LEVEL and FINE, its Operator Env instead of a sine, no FRAME
    // (I7-2, I7-19).
    loadNamed ("E.PIANO 1 (ROM1A)");
    editor.showPage ("MAIN");
    settle (400);
    {
        auto* level = knobFor ("osc2_eg_out", "OUTPUT"); // (a modulator too: never DEPTH, I15-1)
        auto* trim = knobFor ("osc2_level"); // (none on PLAY: one level, OUTPUT, I10-1)
        const auto levelText = level != nullptr ? level->getSlider().getTextFromValue (level->getSlider().getValue()) : juce::String();
        std::vector<WaveDisplay*> waves;
        findAll<WaveDisplay> (editor, waves);
        auto compactWaves = 0;
        for (auto* wave : waves)
            compactWaves += visibleInTree (wave) && wave->isCompact() ? 1 : 0;
        expect (level != nullptr && levelText.endsWith ("dB") && trim == nullptr && knobFor ("osc2_frame") == nullptr
                    && centreX (knobFor ("osc2_fine")) < centreX (knobFor ("osc2_ratio")) && centreX (knobFor ("osc2_ratio")) < centreX (level)
                    && compactWaves > 0, // (the FM card's compact picture is the Operator Env itself, #133)
                "PLAY: an operator card is FINE, RATIO, OUTPUT (" + levelText + ") as on FM (V8-5, I10-1), its Operator Env "
                "pictured, no FRAME (Operator Env pictures " + juce::String (compactWaves) + ", frame " + juce::String (knobFor ("osc2_frame") != nullptr ? 1 : 0)
                + ", trim " + juce::String (trim != nullptr ? 1 : 0) + ")");
    }

    // Review 14 (I14-3): a DX7 voice with both filters open has no filter;
    // PLAY says so, dims everything but CUTOFF, which brings one in.
    {
        auto* reso = knobFor ("f1_reso");
        auto* cutoff = knobFor ("f1_cutoff");
        expect (reso != nullptr && cutoff != nullptr && reso->getAlpha() < 0.99f && cutoff->getAlpha() > 0.99f,
                "PLAY: a DX7 voice's filter knobs dim but CUTOFF, which brings a filter in (I14-3)");
    }

    // OSC: the operator's card is its Operator Env (the FM graph), LEVEL,
    // pitch with LEVEL and one WAVE row, no warp, spectral or unison spread;
    // the one-frame sine shows as WAVE (I7-20, V7-16, V7-31, S7-12).
    editor.showPage ("OSC");
    settle (300);
    selectOscTab (0);
    {
        std::vector<OperatorEnvDisplay*> graphs;
        findAll<OperatorEnvDisplay> (editor, graphs);
        auto graph = false;
        for (auto* g : graphs)
            graph = graph || (visibleInTree (g) && g->getPrefix() == "osc1" && g->getWidth() > 120); // (the approved card gives it its 150 px well)
        auto* wave = oscWave (0);
        const auto editOpEnv = juce::String::fromUTF8 ("EDIT OP ENV \xe2\x80\xba");
        expect (graph && knobFor ("osc1_eg_out", "OUTPUT") != nullptr && knobFor ("osc1_level", "VOICE LEVEL") == nullptr
                    && knobFor ("osc1_warp_amt") == nullptr && knobFor ("osc1_spectral_amt") == nullptr && knobFor ("osc1_detune") == nullptr
                    && knobFor ("osc1_frame") == nullptr && buttonNamed (editOpEnv) == nullptr,
                "OSC: an operator's card shows its Operator Env graph and OUTPUT alone (no VOICE LEVEL, I12-1), no wavetable warp or unison spread ("
                    + juce::String ((int) graph) + juce::String ((int) (knobFor ("osc1_eg_out", "OUTPUT") != nullptr)) + juce::String ((int) (knobFor ("osc1_level", "VOICE LEVEL") == nullptr))
                    + juce::String ((int) (knobFor ("osc1_warp_amt") == nullptr)) + juce::String ((int) (knobFor ("osc1_spectral_amt") == nullptr))
                    + juce::String ((int) (knobFor ("osc1_detune") == nullptr)) + juce::String ((int) (knobFor ("osc1_frame") == nullptr))
                    + juce::String ((int) (buttonNamed (editOpEnv) == nullptr)) + ")");
        // No full-height sine beside the envelope (review 11, V11-7): the WAVE menu in the rows
        // is the one place the wave is chosen, and VOICE LEVEL is not drawn on the page (I12-1).
        expect (wave == nullptr && knobFor ("osc1_level", "VOICE LEVEL") == nullptr && knobFor ("osc1_level") == nullptr,
                "OSC: an Operator Env operator has the envelope and one WAVE menu, no sine picture (V11-7)");

        // (The mock has no EDIT OP ENV button on the card: a click on the envelope picture opens the editor.)
        setParam ("osc1_mode", 1.0f);
        settle (400);
        expect (buttonNamed (editOpEnv) == nullptr, "OSC: an operator's card has no EDIT OP ENV button (its picture opens the editor)");
        setParam ("osc1_mode", 0.0f);
        settle (300);
    }

    // OSC rows packed from the top (S7-25): Init's three rows sit within
    // about 3 x 110 px.
    loadNamed ("Init");
    editor.showPage ("OSC");
    settle (400);
    {
        auto* frame = knobFor ("osc1_semi");
        auto* unison = knobFor ("osc1_unison");
        const auto spread = frame != nullptr && unison != nullptr
                                ? editor.getLocalArea (unison, unison->getLocalBounds()).getY() - editor.getLocalArea (frame, frame->getLocalBounds()).getY()
                                : 999;
        expect (spread <= 2 * 134, "OSC: the card's rows are packed (SHAPE to UNISON " + juce::String (spread) + " px)");
        expect (buttonNamed ("RESAMPLE") != nullptr && buttonNamed ("BOUNCE") == nullptr, "OSC: the resampler's button is RESAMPLE (I7-25)");

        // Sample mode: a LOAD button, and an SF2 / SFZ's zones under the
        // wave (I7-24).
        auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ilana-q4-sfz");
        folder.createDirectory();
        {
            juce::AudioBuffer<float> tone (1, 4410);
            for (int i = 0; i < tone.getNumSamples(); ++i)
                tone.setSample (0, i, 0.5f * std::sin ((float) i * 0.06f));
            juce::WavAudioFormat wav;
            auto file = folder.getChildFile ("tone.wav");
            file.deleteFile();
            if (auto stream = std::unique_ptr<juce::OutputStream> (file.createOutputStream()))
                if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (stream.get(), 44100.0, 1, 16, {}, 0)))
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer (tone, 0, tone.getNumSamples());
                }
            folder.getChildFile ("test.sfz").replaceWithText ("<region> sample=tone.wav lokey=36 hikey=59 pitch_keycenter=48\n"
                                                              "<region> sample=tone.wav lokey=60 hikey=84 pitch_keycenter=72\n");
        }
        setParam ("osc1_mode", 2.0f);
        settle (300);
        const auto loaded = processor.loadUserSample (0, folder.getChildFile ("test.sfz"));
        settle (400);
        auto* wave = oscWave (0);
        expect (buttonNamed ("LOAD...") != nullptr, "OSC: Sample mode has a LOAD... button (I10-11)");
        expect (loaded && wave != nullptr && wave->getZoneCount() == 2, "OSC: an SFZ's two zones show under the sample ("
                                                                            + juce::String (wave != nullptr ? wave->getZoneCount() : -1) + ")");
        folder.deleteRecursively();

        // The exciters: one name table, in groups; a hammer has no pick
        // controls (I7-27).
        // (On the oscillator's card, the one string editor since the
        // PHYSICAL tab folded in.)
        setParam ("osc1_mode", 1.0f);
        setParam ("osc1_excite", 5.0f);
        editor.showPage ("OSC");
        settle (400);
        // (Review 23: the exciter is a row of pills; the menu behind them holds the choice.)
        const auto hammerName = processor.apvts.getParameter ("osc1_excite")->getCurrentValueAsText();
        expect (hammerName == "Bright Hammer" && knobFor ("osc1_string_pick_hardness") == nullptr && knobFor ("osc1_string_pick_pos") == nullptr
                    && knobFor ("osc1_couple", "COUPLE") != nullptr,
                "OSC: the M4 hammer reads Bright Hammer, without the pick's HARDNESS / PICK POS ('" + hammerName + "')");
        setParam ("osc1_excite", 0.0f);
        settle (300);
        expect (knobFor ("osc1_string_pick_hardness") != nullptr, "OSC: a plucked burst has HARDNESS");
        setParam ("osc1_mode", 0.0f);
        settle (300);
    }

    // BODY by one name: the comb filter type is COMB BODY (I7-26).
    // (Its case is the FILTER page's own name, I7-40.)
    expect (FilterTypeGrid::shortNames()[FilterType::CombBody].equalsIgnoreCase ("COMB BODY"), "the comb body filter's short name is COMB BODY");

    // VECTOR: off, every control dims and says why; on, VEC X / Y are in
    // the chip bar (V7-21, I7-22).
    loadNamed ("Init");
    editor.showPage ("VECTOR");
    settle (300);
    {
        setParam ("vec_on", 0.0f);
        settle (300);
        // The corners named as the pad names them, and showing it.
        juce::StringArray corners;
        std::vector<ComboControl*> combos;
        findAll<ComboControl> (editor, combos);
        for (auto* combo : combos)
            if (visibleInTree (combo) && combo->getComboBox().getNumItems() > 0 && combo->getComboBox().getItemText (0) == "OSC 1")
                corners.add (combo->getComboBox().getText());
        corners.sort (false);
        const auto cornerTexts = corners.joinIntoString ("|");
        auto* x = knobFor ("vec_x");
        const auto dim = x != nullptr && x->getAlpha() < 0.9f && x->getSlider().getTooltip().contains ("VECTOR is off");
        setParam ("vec_on", 1.0f);
        settle (300);
        const auto lit = x != nullptr && x->getAlpha() > 0.99f;
        editor.showPage ("MAIN");
        settle (300);
        std::vector<ModSourceChip*> chips;
        findAll<ModSourceChip> (editor, chips);
        // In the bar as chips of their own: played sources fold last (S8-20).
        const auto inBar = [&] (Mod::Source source)
        {
            for (auto* chip : chips)
                if (visibleInTree (chip) && chip->getSourceIndex() == (int) source)
                    return 1;
            return 0;
        };
        const auto barChips = inBar (Mod::Source::VectorX) + inBar (Mod::Source::VectorY);
        setParam ("vec_on", 0.0f);
        settle (300);
        const auto offChips = inBar (Mod::Source::VectorX);
        expect (dim && lit, "VECTOR: off, X dims and says VECTOR is off; on, it lights");
        expect (cornerTexts.replace (": none", "").replace (": off", "") == "OSC 1|OSC 2|OSC 3|OSC 4", "VECTOR: the corners read OSC 1..4 (" + cornerTexts + ")");
        expect (barChips == 2 && offChips == 0, "VEC X / VEC Y are in the chip bar while VECTOR is on (" + juce::String (barChips) + ")");
    }

    // A Physical oscillator's card: the moving string in its well, STRING and
    // EXCITER rows of the card's one grid (the PHYSICAL tab folded in), the
    // body a link to FILTER.
    loadNamed ("Felt Hammer Board");
    editor.showPage ("OSC");
    settle (400);
    {
        auto* decay = knobFor ("osc1_string_decay");
        auto* hammer = knobFor ("osc1_hammer_hard");
        std::vector<PhysicalView*> views;
        findAll<PhysicalView> (editor, views);
        auto stringShown = false;
        for (auto* view : views)
            stringShown = stringShown || (visibleInTree (view) && view->getOscillator() == "osc1");
        // (STRING and EXCITER are two rows of one grid: the hammer sits under the decay.)
        // (Review 23: TONE and EXCITER are two panels of one card, side by side.)
        const auto sameGrid = decay != nullptr && hammer != nullptr && visibleInTree (decay) && visibleInTree (hammer)
                              && decay->getParentComponent() == hammer->getParentComponent();
        expect (stringShown && sameGrid && buttonNamed (juce::CharPointer_UTF8 ("EDIT BODY \xe2\x80\xba")) != nullptr,
                "OSC: a Physical card shows its moving string, STRING and EXCITER on one grid, EDIT BODY > links to FILTER");
        const auto exciteText = processor.apvts.getParameter ("osc1_excite")->getCurrentValueAsText();
        expect (exciteText.startsWith ("Piano"), "OSC: EXCITE reads Piano ('" + exciteText + "')");
        expect (editor.getPageIds().indexOf ("PHYSICAL") < 0, "the PHYSICAL tab is gone: the card holds the whole string");
    }
}

void runSmallThingsTests (IlanaSynthAudioProcessor& processor, IlanaSynthAudioProcessorEditor& editor)
{
    // The tour's "new in" list: the header's version, real pages, current
    // names, every chip shown, and a click opens the chip's page.
    {
        const auto& features = TutorialOverlay::whatsNew();
        const auto pageIds = editor.getPageIds();
        auto* logo = findChild<LogoComponent> (editor);
        juce::StringArray labels, badPages;
        for (const auto& feature : features)
        {
            labels.add (feature.label);
            if (! pageIds.contains (feature.page))
                badPages.add (feature.label + " -> " + feature.page);
        }
        expect (logo != nullptr && logo->version == "v" + juce::String (TutorialOverlay::whatsNewVersion),
                "the tour's NEW IN version is the header's (" + juce::String (TutorialOverlay::whatsNewVersion) + ")");
        expect (badPages.isEmpty(), "every tour chip opens a page that exists" + (badPages.isEmpty() ? juce::String() : ": " + badPages.joinIntoString (", ")));
        // Names retired since: the piano preset was renamed Felt Hammer Board.
        expect (! labels.joinIntoString ("|").containsIgnoreCase ("GRAND PIANO") && labels.contains ("FELT HAMMER BOARD"),
                "the tour names the Felt Hammer Board, not the retired GRAND PIANO");
        expect (labels.contains (juce::String (FilterType::Count) + " FILTERS"), "the tour's filter count is the code's ("
                                                                                     + juce::String (FilterType::Count) + ")");
        for (const auto* name : { "DX7 BANKS + OPERATOR ENV", "AIRWINDOWS", "SF2 / SFZ", "VOCODER", "CLIP SEQUENCER" })
            expect (labels.contains (name), juce::String ("the tour lists ") + name);

        // UI review 7 (I7-35): header captions are lower-case fragments.
        expect (IlanaTheme::captionFragment ("Drag the graph's points.") == "drag the graph's points"
                    && IlanaTheme::captionFragment ("OSC 1 carrier") == "OSC 1 carrier"
                    && IlanaTheme::captionFragment ("rows modulate columns") == "rows modulate columns",
                "card captions read as lower-case fragments");

        auto* tutorial = findChild<TutorialOverlay> (editor);
        if (tutorial != nullptr)
        {
            // UI review 6: three tips, the box unticked, no "DX7 MODE".
            expect (tutorial->getTips().size() == 3 && ! tutorial->isDontShowTicked()
                        && ! (labels.joinIntoString ("|") + tutorial->getTips().joinIntoString ("|")).contains ("DX7 MODE"),
                    "the tour has three tips, \"Don't show this again\" unticked and no DX7 MODE");
            tutorial->setVisible (true);
            settle (600);
            // UI review 7 (S7-30): the tips are as tall as their lines, no
            // fixed allowance leaving a gap above NEW IN.
            expect (tutorial->panelBounds().getHeight() < 431, "the tour's panel is as tall as its tips ("
                                                                   + juce::String (tutorial->panelBounds().getHeight()) + " px)");
            // Review 9, S9-20: the pills are behind WHAT'S NEW; closed, the
            // tour is the tips and one line.
            expect (! tutorial->isWhatsNewOpen() && tutorial->chipBounds().empty(), "the tour opens with the NEW IN pills folded away");
            const auto closedHeight = tutorial->panelBounds().getHeight();
            tutorial->setWhatsNewOpen (true);
            settle (100);
            expect (tutorial->panelBounds().getHeight() > closedHeight && tutorial->panelBounds().getHeight() < 470, // (the 720 px column wraps the pills to three rows: S11-16)
                    "opening WHAT'S NEW grows the panel (" + juce::String (closedHeight) + " to " + juce::String (tutorial->panelBounds().getHeight()) + " px)");
            expect (tutorial->chipBounds().size() == features.size(), "every tour chip fits on the panel ("
                                                                          + juce::String ((int) tutorial->chipBounds().size()) + " of "
                                                                          + juce::String ((int) features.size()) + ")");
            // Impeccable re-score (H7): MOUSE MOVES lists the hidden moves,
            // folded until clicked; one of it and WHAT'S NEW opens at a time,
            // so the panel is never cut by a 720 px window.
            tutorial->setMovesOpen (true);
            settle (100);
            expect (tutorial->isMovesOpen() && ! tutorial->isWhatsNewOpen() && TutorialOverlay::moves().size() == 8
                        && tutorial->panelBounds().getHeight() < tutorial->getHeight() - 140,
                    "MOUSE MOVES opens its eight moves, folds WHAT'S NEW and fits ("
                        + juce::String (tutorial->panelBounds().getHeight()) + " px)");
            tutorial->setWhatsNewOpen (true);
            settle (100);
            expect (! tutorial->isMovesOpen() && tutorial->isWhatsNewOpen(), "opening WHAT'S NEW folds MOUSE MOVES");
            tutorial->openChip (labels.indexOf ("CLIP SEQUENCER"));
            settle (300);
            expect (! tutorial->isVisible() && editor.getCurrentPageId() == "ARP/SEQ",
                    "clicking the tour's CLIP SEQUENCER chip closes the tour and opens SEQ");
        }
        else
        {
            expect (false, "the tour exists");
        }
    }

    // Impeccable re-score (H9): a step that throws something away shows the
    // undo note with its name; UNDO takes it back; a plain edit shows none.
    if (auto* toast = findChild<UndoToast> (editor); toast != nullptr)
    {
        auto* cutoff = processor.apvts.getParameter ("f1_cutoff");
        const auto before = cutoff->getValue();
        processor.performEdit ("Change cutoff", [cutoff, before] { cutoff->setValueNotifyingHost (before > 0.5f ? 0.2f : 0.8f); });
        settle (200);
        expect (! toast->isVisible(), "a plain edit shows no undo note");
        processor.performEdit ("Remove test step", [cutoff] { cutoff->setValueNotifyingHost (0.5f); });
        settle (200);
        expect (toast->isVisible() && toast->getText() == "Remove test step" && toast->getBottom() <= editor.getHeight(),
                "a Remove step shows the undo note with its name ('" + toast->getText() + "')");
        toast->getUndoButton().triggerClick();
        settle (200);
        expect (processor.getUndoManager().getUndoDescription() == "Change cutoff",
                "UNDO on the note takes the step back ('" + processor.getUndoManager().getUndoDescription() + "')");
        settle (200);
        // (Usability flair pass 1: the undo then says what it changed. CUTOFF
        // isn't on the open page here, so the key is SHOW, which opens it.)
        const auto key = toast->getUndoButton().getButtonText();
        expect (toast->isVisible() && toast->getText().startsWith ("Undid CUTOFF") && (key == "REDO" || key == "SHOW"),
                "the undo says what it changed ('" + toast->getText() + "', " + key + ")");
        if (key == "SHOW")
        {
            toast->getUndoButton().triggerClick();
            settle (400);
            expect (editor.getCurrentPageId() == "MAIN", "SHOW opens the page the undone knob is on (" + editor.getCurrentPageId() + ")");
        }
        processor.getUndoManager().undo();
        settle (100);
    }
    else
    {
        expect (false, "the undo note exists");
    }

    // Text floors (S25): every visible label and button on every page, at
    // 100 %, against the theme's floors. Interactive: buttons, menus,
    // editable fields and knob value boxes; the rest is passive.
    {
        juce::StringArray small;
        std::set<juce::String> seen;
        auto checked = 0;
        const auto check = [&] (const juce::String& page)
        {
            std::vector<juce::Label*> labels;
            std::vector<juce::TextButton*> buttons;
            findAll<juce::Label> (editor, labels);
            findAll<juce::TextButton> (editor, buttons);

            for (auto* label : labels)
            {
                if (! visibleInTree (label) || label->getWidth() <= 0 || label->getText().trim().isEmpty())
                    continue;
                const auto interactive = label->isEditable() || dynamic_cast<juce::Slider*> (label->getParentComponent()) != nullptr
                                         || dynamic_cast<juce::ComboBox*> (label->getParentComponent()) != nullptr;
                const auto height = label->getLookAndFeel().getLabelFont (*label).getHeight();
                ++checked;
                const auto floor = interactive ? IlanaTheme::TextSize::minInteractive : IlanaTheme::TextSize::minPassive;
                const auto key = page + ": '" + label->getText() + "' " + juce::String (height, 1);
                if (height < floor - 0.01f && seen.insert (key).second)
                    small.add (key);
            }

            // PLAY's ring knobs paint their names themselves (FX, SEQ, VECTOR and FM too since review 26):
            // held to the 10 px screen floor, as the packed buttons.
            std::vector<KnobControl*> rings;
            findAll<KnobControl> (editor, rings);
            for (auto* knob : rings)
            {
                if (! knob->isPacked() || ! visibleInTree (knob) || knob->getWidth() <= 0)
                    continue;
                ++checked;
                if (knob->packedNameDevicePx() < IlanaTheme::TextSize::screenFloorPx - 0.6f)
                    small.add (page + ": (" + knob->getLabelText() + ") ring " + juce::String (knob->packedNameDevicePx(), 1) + " px");
            }

            for (auto* button : buttons)
            {
                if (! visibleInTree (button) || button->getWidth() <= 0 || button->getButtonText().trim().isEmpty()
                    || button->getProperties().contains ("switch"))
                    continue;
                // (The OSC cards' packed header buttons keep the 10 px screen floor instead: review 23.)
                if (button->getProperties().contains ("packedButton"))
                {
                    const auto device = button->getLookAndFeel().getTextButtonFont (*button, button->getHeight()).getHeight() * IlanaTheme::uiScaleRef();
                    ++checked;
                    if (device < IlanaTheme::TextSize::screenFloorPx - 0.6f)
                        small.add (page + ": [" + button->getButtonText() + "] packed " + juce::String (device, 1) + " px");
                    continue;
                }
                const auto height = button->getProperties().contains ("pill")
                                        ? juce::Font (IlanaTheme::pillFont()).getHeight()
                                        : button->getLookAndFeel().getTextButtonFont (*button, button->getHeight()).getHeight();
                const auto key = page + ": [" + button->getButtonText() + "] " + juce::String (height, 1);
                ++checked;
                if (height < IlanaTheme::TextSize::minInteractive - 0.01f && seen.insert (key).second)
                    small.add (key);
            }
        };

        for (const auto& page : editor.getPageIds())
        {
            editor.showPage (page);
            settle (250);
            check (page);
        }
        editor.setScopeOpen (true);
        settle (300);
        check ("SCOPE");
        editor.setScopeOpen (false);
        settle (100);

        expect (small.isEmpty() && checked > 300, "no text under the floors (" + juce::String (checked) + " checked; "
                                     + juce::String (IlanaTheme::TextSize::minInteractive) + " interactive, "
                                     + juce::String (IlanaTheme::TextSize::minPassive) + " passive)"
                                     + (small.isEmpty() ? juce::String() : ": " + small.joinIntoString (", ")));
    }

    // The 75 % floor (UI review 5 #31, 6 #46): every page (and the scope)
    // painted at the smallest zoom, with the theme's font helper watching:
    // no text drawn under 10 screen pixels.
    {
        auto* top = editor.getTopLevelComponent();
        const auto before = top->getBounds();
        top->setSize (795, 540);
        settle (300);
        auto& probe = IlanaTheme::fontProbe();
        juce::StringArray under;
        auto smallest = 1.0e6f;
        const auto paintPage = [&] (const juce::String& page)
        {
            probe = {};
            probe.armed = true;
            probe.limit = IlanaTheme::TextSize::screenFloorPx;
            editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f);
            probe.armed = false;
            smallest = juce::jmin (smallest, probe.smallest);
            if (probe.under > 0)
                under.add (page + " (" + juce::String (probe.smallest, 2) + " px)");
        };
        for (const auto& page : editor.getPageIds())
        {
            editor.showPage (page);
            settle (250);
            paintPage (page);
        }
        editor.setScopeOpen (true);
        settle (300);
        paintPage ("SCOPE");
        editor.setScopeOpen (false);
        top->setBounds (before);
        settle (300);
        expect (under.isEmpty() && smallest < 1.0e5f, "at 75 % no text is drawn under " + juce::String (IlanaTheme::TextSize::screenFloorPx, 0)
                                                          + " px (smallest " + juce::String (smallest, 2) + ")"
                                                          + (under.isEmpty() ? juce::String() : ": " + under.joinIntoString (", ")));
    }

    // The scope's meters (S23, V27): held peak numbers after sound, reset by
    // a click; the clip light starts dark. The header meter has its own.
    {
        editor.showPage ("MAIN");
        editor.setScopeOpen (true);
        settle (300);
        auto* scope = findChild<ScopeDisplay> (editor);
        expect (scope != nullptr, "the scope panel has its display");

        if (scope != nullptr)
        {
            // UI review 7 (I7-31): the scope docks over the page area by
            // default, with the browser's FLOAT / × pair; FLOAT makes it a
            // small panel and DOCK puts it back. The engine quality settings
            // live in the settings menu only (review 6).
            std::vector<juce::ComboBox*> boxes;
            findAll<juce::ComboBox> (*scope, boxes);
            auto* panel = scope->getParentComponent();
            juce::TextButton* floatButton = nullptr;
            auto hasCross = false;
            if (panel != nullptr)
                for (auto* child : panel->getChildren())
                    if (auto* button = dynamic_cast<juce::TextButton*> (child))
                    {
                        if (button->getButtonText() == "FLOAT")
                            floatButton = button;
                        hasCross = hasCross || button->getButtonText() == juce::String (juce::CharPointer_UTF8 ("\xc3\x97"));
                    }
            const auto dockedBounds = panel != nullptr ? panel->getBounds() : juce::Rectangle<int>();
            expect (boxes.empty() && panel != nullptr && dockedBounds.getWidth() > 900 && dockedBounds.getY() > 56 && floatButton != nullptr && hasCross,
                    "the scope docks over the page with FLOAT and a cross, without quality or oversampling controls ("
                        + dockedBounds.toString() + ")");
            if (floatButton != nullptr)
            {
                floatButton->triggerClick();
                settle (100);
                const auto floating = panel->getBounds();
                const auto dockText = floatButton->getButtonText();
                floatButton->triggerClick();
                settle (100);
                expect (floating.getWidth() < 600 && floating.getY() > 56 && dockText == "DOCK" && panel->getBounds() == dockedBounds,
                        "FLOAT makes the scope a small panel, DOCK puts it back (" + floating.toString() + ")");
            }
            scope->resetPeaks();
            juce::AudioBuffer<float> audio (2, 512);
            for (int block = 0; block < 24; ++block)
            {
                juce::MidiBuffer midi;
                if (block == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
                audio.clear();
                processor.processBlock (audio, midi);
            }
            // (A paint reads the output into the meters.)
            const auto image = scope->createComponentSnapshot (scope->getLocalBounds());
            juce::ignoreUnused (image);
            const auto held = juce::jmax (scope->getHeldPeakDb (0), scope->getHeldPeakDb (1));
            expect (held > -60.0f && held < 12.0f, "the scope's meters hold the peak in dB after a note (" + juce::String (held, 1) + " dB)");
            scope->resetPeaks();
            expect (scope->getHeldPeakDb (0) < -90.0f && ! scope->isClipLit(), "resetting the scope's meters clears the peaks and the clip light");

            juce::AudioBuffer<float> silence (2, 512);
            juce::MidiBuffer off;
            off.addEvent (juce::MidiMessage::allNotesOff (1), 0);
            for (int block = 0; block < 200; ++block)
            {
                silence.clear();
                processor.processBlock (silence, off);
                off.clear();
            }
        }
        editor.setScopeOpen (false);
        settle (100);

        // WINDOW pops the scope out beside the synth (step 14, S23), so it
        // covers no page. (A real top-level window can't open under xvfb,
        // which has no window manager: unit-level checks, as for menus.)
        {
            editor.setScopeOpen (true);
            settle (200);
            juce::TextButton* windowButton = nullptr;
            if (auto* shown = findChild<ScopeDisplay> (editor); shown != nullptr && shown->getParentComponent() != nullptr)
                for (auto* child : shown->getParentComponent()->getChildren())
                    if (auto* button = dynamic_cast<juce::TextButton*> (child); button != nullptr && button->getButtonText() == "WINDOW")
                        windowButton = button;
            expect (windowButton != nullptr && visibleInTree (windowButton) && windowButton->getTooltip().contains ("own window"),
                    "the scope panel has WINDOW, which opens it in its own window");
            expect (! editor.isScopeWindowOpen(), "the scope opens in the page until WINDOW is clicked");
            editor.setScopeOpen (false);
            settle (100);
            expect (! editor.isScopeOpen(), "the scope closes");
        }

        auto* meter = findChild<OutputMeter> (editor);
        expect (meter != nullptr && meter->getTooltip().contains ("dB") && ! meter->isClipLit(),
                "the OUT meter has a clip light and a tooltip in dB");
    }

    // The voice dots' tooltip counts them (V28).
    {
        settle (400);
        juce::String voices;
        std::vector<juce::Component*> all;
        findAll<juce::Component> (editor, all);
        for (auto* component : all)
            if (auto* client = dynamic_cast<juce::SettableTooltipClient*> (component))
                if (client->getTooltip().startsWith ("Voices: "))
                    voices = client->getTooltip();
        expect (voices.contains (" of ") && voices.contains ("playing"), "the voice dots' tooltip counts them (" + voices.upToFirstOccurrenceOf ("\n", false, false) + ")");
    }

    // Chaos LFO outputs are spelled out, following OUTPUT A's axis (V28).
    {
        const auto& lorenz = LfoSimInfo::get (LfoSimShapes::RandomHold + 4);
        const auto x = LfoSimPreview::outputNames (lorenz, 0);
        const auto z = LfoSimPreview::outputNames (lorenz, 2);
        const auto mix = LfoSimPreview::outputNames (lorenz, 3);
        expect (juce::String (lorenz.name) == "Lorenz" && x.first == "X axis" && x.second == "Y axis" && z.first == "Z axis"
                    && z.second == "X axis" && mix.first.contains ("mix") && mix.second == "Y axis",
                "a chaos LFO's outputs read 'X axis' / 'Y axis' and follow OUTPUT A's axis");
        const auto bounce = LfoSimPreview::outputNames (LfoSimInfo::get (LfoSimShapes::RandomHold + 10), 0);
        expect (bounce.first == "height" && bounce.second == "impacts", "the bounce LFO's outputs read height / impacts");
    }

    // SEQ GENERATE (review 6: V5-21, S5-17, S6-24, I6-30): three boxes,
    // SNAP TO KEY, STRUM and SPRAY, each with its own switch on its title
    // line and its controls inside, every name on one line; SNAP TO KEY's
    // and STRUM's switches turn their choice Off and back, one undo step.
    {
        editor.showPage ("ARP/SEQ");
        settle (400);
        const auto controlFor = [&editor, &processor] (const juce::String& id) -> juce::Component*
        {
            auto* parameter = processor.apvts.getParameter (id);
            std::vector<juce::Component*> all;
            findAll<juce::Component> (editor, all);
            for (auto* component : all)
            {
                if (! visibleInTree (component) || component->getWidth() <= 0)
                    continue;
                if (auto* knob = dynamic_cast<KnobControl*> (component); knob != nullptr && knob->getParameterId() == id)
                    return knob;
                if (auto* combo = dynamic_cast<ComboControl*> (component); combo != nullptr && parameter != nullptr
                    && combo->getTooltip().startsWith (parameter->getName (64)))
                    return combo;
                if (auto* toggle = dynamic_cast<ToggleControl*> (component); toggle != nullptr && parameter != nullptr
                    && toggle->getButton().getTooltip().startsWith (parameter->getName (64)))
                    return toggle;
            }
            return nullptr;
        };
        const auto switchFor = [&editor] (const juce::String& what) -> ChoiceSwitch*
        {
            std::vector<ChoiceSwitch*> switches;
            findAll<ChoiceSwitch> (editor, switches);
            for (auto* candidate : switches)
                if (visibleInTree (candidate) && candidate->getTooltip().startsWith (what))
                    return candidate;
            return nullptr;
        };
        const auto boundsOf = [&editor] (juce::Component* component)
        {
            return component != nullptr ? editor.getLocalArea (component->getParentComponent(), component->getBounds()) : juce::Rectangle<int>();
        };
        const auto topOf = [&boundsOf] (juce::Component* component) { return component != nullptr ? boundsOf (component).getY() : -1; };
        const auto xOf = [&boundsOf] (juce::Component* component) { return component != nullptr ? boundsOf (component).getCentreX() : -1; };
        auto* scale = controlFor ("gen_scale");
        auto* root = controlFor ("gen_root");
        auto* snap = controlFor ("gen_snap");
        auto* strum = controlFor ("spray_strum");
        auto* strumTime = controlFor ("spray_strum_time");
        auto* pitch = controlFor ("spray_direction");
        auto* count = controlFor ("spray_count");
        auto* spread = controlFor ("spray_spread");
        auto* velocity = controlFor ("spray_velocity");
        auto* sprayOn = controlFor ("spray_on");
        auto* scaleSwitch = switchFor ("SNAP TO KEY");
        auto* strumSwitch = switchFor ("STRUM");
        const auto found = scale != nullptr && root != nullptr && snap != nullptr && strum != nullptr && strumTime != nullptr
                        && pitch != nullptr && count != nullptr && spread != nullptr && velocity != nullptr && sprayOn != nullptr
                        && scaleSwitch != nullptr && strumSwitch != nullptr;
        expect (found, "GENERATE shows SNAP TO KEY, STRUM and SPRAY with a switch each");

        if (found)
        {
            const auto row = topOf (count);
            // (Menu-only boxes centre their row, S10-12, so a menu sits a few px lower than a knob.)
            const auto sameRow = [&] (juce::Component* c) { return std::abs (topOf (c) - row) <= 24; };
            expect (sameRow (scale) && sameRow (root) && sameRow (snap) && sameRow (strum)
                        && sameRow (strumTime) && sameRow (pitch) && sameRow (spread),
                    "GENERATE's controls are one row, every name on one line");
            expect (xOf (root) < xOf (scale) && xOf (scale) < xOf (snap) && xOf (snap) < xOf (strum) && xOf (strum) < xOf (strumTime)
                        && xOf (strumTime) < xOf (pitch) && xOf (pitch) < xOf (count) && xOf (spread) < xOf (velocity),
                    "SNAP TO KEY's, STRUM's and SPRAY's controls sit together, in that order");
            expect (topOf (scaleSwitch) < row && topOf (strumSwitch) < row && topOf (sprayOn) < row
                        && xOf (scaleSwitch) > xOf (snap) - 60 && xOf (scaleSwitch) < xOf (strum)
                        && xOf (strumSwitch) > xOf (strum) && xOf (strumSwitch) < xOf (pitch) && xOf (sprayOn) > xOf (spread),
                    "each box's switch closes its own title line, over its controls");

            // SNAP TO KEY's switch: Off, then back to the key it had.
            if (auto* parameter = processor.apvts.getParameter ("gen_scale"))
            {
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (3.0f));
                settle (150);
                processor.apvts.copyState();
                processor.getUndoManager().clearUndoHistory();
                const auto onBefore = scaleSwitch->isOn();
                scaleSwitch->getButton().triggerClick();
                settle (50);
                const auto off = juce::roundToInt (processor.apvts.getRawParameterValue ("gen_scale")->load()) == 0 && ! scaleSwitch->isOn();
                processor.apvts.copyState();
                const auto steps = processor.getUndoManager().getUndoDescriptions();
                scaleSwitch->getButton().triggerClick();
                settle (50);
                const auto back = juce::roundToInt (processor.apvts.getRawParameterValue ("gen_scale")->load());
                expect (onBefore && off && back == 3 && steps.size() == 1 && steps[0] == "SNAP TO KEY off",
                        "SNAP TO KEY's switch sets the key Off in one undo step ('" + steps.joinIntoString ("', '")
                            + "') and on again brings back Dorian (" + juce::String (back) + ")");
                parameter->setValueNotifyingHost (parameter->getDefaultValue());
            }

            // STRUM's switch: on from Off is Up.
            if (auto* parameter = processor.apvts.getParameter ("spray_strum"))
            {
                parameter->setValueNotifyingHost (parameter->getDefaultValue());
                settle (150);
                strumSwitch->getButton().triggerClick();
                settle (50);
                const auto up = juce::roundToInt (processor.apvts.getRawParameterValue ("spray_strum")->load());
                strumSwitch->getButton().triggerClick();
                settle (50);
                expect (up == 1 && juce::roundToInt (processor.apvts.getRawParameterValue ("spray_strum")->load()) == 0,
                        "STRUM's switch turns the strum on (Up) and off");
            }
        }
    }
}

// Drives the real editor the way a user would and checks the processor
// state, for the wiring that unit tests can't see.
int runUiTests()
{
    // The FILTER page's SIGNAL FLOW opens on hover or click: the tests hold it open.
    filterFlowForcedOpen() = true;
    IlanaSynthAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    // ILANA_UITEST_ONLY=R6 runs just review 8's R6 checks (a quick loop).
    const auto only = juce::SystemStats::getEnvironmentVariable ("ILANA_UITEST_ONLY", {});

    const auto names = processor.getFactoryPresetNames();
    const auto neuroWobble = names.indexOf ("Neuro Wobble");
    processor.loadFactoryPreset (neuroWobble);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setSize (1060, 720);
    settle (400);

    if (auto* tutorial = findChild<TutorialOverlay> (*editor))
        tutorial->setVisible (false);

    auto* pages = dynamic_cast<IlanaSynthAudioProcessorEditor*> (editor.get());
    expect (pages != nullptr && pages->getPageIds()[0] == "MAIN", "MAIN is the first page");

    // The animation clock's cheap "is it on screen" (IlanaAnim::showing)
    // answers as Component::isShowing does, without asking the window system
    // on every frame: a hidden branch or an editor with no window is not shown.
    {
        juce::Component parent, child;
        parent.addAndMakeVisible (child);
        parent.setVisible (false);
        expect (! IlanaAnim::showing (child) && ! child.isShowing(), "a child of a hidden component is not showing");
        parent.setVisible (true);
        expect (IlanaAnim::showing (child) == child.isShowing(), "showing() agrees with isShowing() for a component with no window");
    }

    // Loads don't ask over edited patches except in the test that checks it
    // (the user's own choice is put back at the end).
    const auto askedBefore = pages->asksBeforeReplacingEdits();
    pages->setAsksBeforeReplacingEdits (false);

    if (only == "T1" || only == "T2" || only == "U1")
    {
        if (only == "U1")
            runLayoutReview10Tests (processor, *pages);
        else if (only == "T1")
            runLayoutReview9Tests (processor, *pages);
        else
            runReview9T2Tests (processor, *pages);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    if (only == "PLAY")
    {
        runPlayPageTests (processor, *pages);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    if (only == "OSC")
    {
        runOscTypeTests (processor, *pages);
        runLayoutReview8Tests (processor, *pages);
        runLayoutReview9Tests (processor, *pages);
        runLayoutReview10Tests (processor, *pages);
        runOscCardOverlapTests (processor, *pages);
        runGlobalReview8Tests (processor, *pages);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    if (only == "R6")
    {
        runGlobalReview8Tests (processor, *pages);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    if (only == "PG")
    {
        runPatchCableTests (processor);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    if (only == "HR")
    {
        runHitReachTests (processor, *pages);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    if (only == "WF")
    {
        runWorkflowUiTests (processor, *pages);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    if (only == "OS")
    {
        runOscSlotUiTests (processor, *pages);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    if (only == "OR")
    {
        runOscRoutingUiTests (processor, *pages);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    if (only == "FC")
    {
        runFxCarouselUiTests (processor, *pages);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    if (only == "FL")
    {
        runFxLfoUiTests (processor, *pages);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    if (only == "FF")
    {
        runFilterFxTests (processor, *pages);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    if (only == "DD")
    {
        runDropdownTests (processor, *pages);
        pages->setAsksBeforeReplacingEdits (askedBefore);
        editor.reset();
        std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
        return uiFailures == 0 ? 0 : 1;
    }

    // Undo checks start from an empty history, with every parameter change
    // already in the tree (it otherwise catches up on a timer).
    const auto clearHistory = [&processor]
    {
        processor.apvts.copyState();
        processor.getUndoManager().clearUndoHistory();
    };
    const auto undoSteps = [&processor]
    {
        processor.apvts.copyState();
        return processor.getUndoManager().getUndoDescriptions();
    };
    expect (findChild<juce::TabbedComponent> (*editor) != nullptr
                && findChild<juce::TabbedComponent> (*editor)->getNumTabs() == (IlanaSynthAudioProcessor::isEffectBuild ? 10 : 9),
            "nine tabs");

    // An oscillator card's switch on the page shown (its tooltip names the
    // parameter), and a click on that card's title line: a card folded to
    // fit opens (UI review 4, V13).
    const auto toggleFor = [&processor, &editor] (const juce::String& id) -> ToggleControl*
    {
        auto* parameter = processor.apvts.getParameter (id);
        std::vector<ToggleControl*> toggles;
        findAll<ToggleControl> (*editor, toggles);
        for (auto* toggle : toggles)
            if (parameter != nullptr && visibleInTree (toggle) && toggle->getWidth() > 0
                && toggle->getButton().getTooltip().startsWith (parameter->getName (64)))
                return toggle;
        return nullptr;
    };
    const auto clickCardTitle = [&toggleFor] (const juce::String& onId, bool rightClick = false)
    {
        auto* toggle = toggleFor (onId);
        auto* card = toggle != nullptr ? toggle->getParentComponent() : nullptr;
        if (card == nullptr)
            return false;
        const juce::Point<float> position (40.0f, (float) toggle->getBounds().getCentreY());
        const juce::MouseEvent event (juce::Desktop::getInstance().getMainMouseSource(), position,
                                      rightClick ? juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier | juce::ModifierKeys::popupMenuClickModifier)
                                                 : juce::ModifierKeys(),
                                      1.0f, 0.0f, 0.0f, 0.0f, 0.0f, card, card, juce::Time::getCurrentTime(),
                                      position, juce::Time::getCurrentTime(), 1, false);
        card->mouseUp (event);
        settle (200);
        return true;
    };

    // OSC's tab for an oscillator, clicked (UI review 6: one oscillator at
    // a time on OSC).
    const auto selectOscTab = [&editor] (int osc)
    {
        std::vector<StateTabs*> rows;
        findAll<StateTabs> (*editor, rows);
        for (auto* tabs : rows)
            for (int i = 0; i < tabs->getNumItems(); ++i)
                if (tabs->getItem (i).name == "OSC " + juce::String (osc + 1) && tabs->onSelect != nullptr
                    && (dynamic_cast<OscPicker*> (tabs) == nullptr || tabs->getName() == "OSC tabs"))
                {
                    tabs->setSelected (i);
                    tabs->onSelect (i);
                    settle (200);
                    return true;
                }
        return false;
    };

    // Pages inside a tab: MATRIX is its own tab, and the scope opens over any page.
    pages->showPage ("MATRIX");
    settle (200);
    expect (pages->getCurrentPageId() == "MATRIX" && pages->getCurrentPage() != nullptr && visibleInTree (pages->getCurrentPage()),
            "MATRIX opens in its tab");
    // STEPS & MSEG went into ENV / LFO (UI review 6, V5-7): the old id opens
    // ENV / LFO.
    pages->showPage ("STEPS");
    settle (200);
    expect (pages->getCurrentPageId() == "ENV/LFO" && ! pages->getPageIds().contains ("STEPS"),
            "MOD is ENV / LFO; the old STEPS id opens ENV / LFO");
    {
        std::vector<juce::TextButton*> buttons;
        findAll<juce::TextButton> (*editor, buttons);
        juce::TextButton* scopeButton = nullptr;
        for (auto* b : buttons)
            if (b->getButtonText() == "SCOPE" && b->getTooltip().startsWith ("Scope"))
                scopeButton = scopeButton == nullptr ? b : scopeButton;
        std::vector<SectionSwitcher*> switchers;
        findAll<SectionSwitcher> (*editor, switchers);
        auto shownSwitchers = 0;
        for (auto* sw : switchers)
            shownSwitchers += visibleInTree (sw) && sw->getWidth() > 40 ? 1 : 0;
        expect (scopeButton != nullptr && visibleInTree (scopeButton) && scopeButton->getWidth() > 20,
                "the SCOPE button is in the tab row (" + (scopeButton != nullptr ? scopeButton->getBounds().toString() : juce::String ("none")) + ")");
        juce::String where;
        for (auto* sw : switchers)
            if (sw->isVisible())
                where << sw->getBounds().toString() << " z" << sw->getParentComponent()->getIndexOfChildComponent (sw)
                      << "/" << sw->getParentComponent()->getNumChildComponents() << " alpha " << sw->getAlpha();
        expect (shownSwitchers == 0, "MOD has one page, so no page switch shows (" + where + ")");
    }
    pages->setScopeOpen (true);
    settle (250);
    expect (pages->isScopeOpen(), "the scope panel opens");
    pages->setScopeOpen (false);
    expect (! pages->isScopeOpen(), "the scope panel closes");

    // KEYS opens the keyboard and gives the page less room; COMPARE flips
    // to B and lights; the page switch changes the page.
    {
        std::vector<juce::TextButton*> buttons;
        findAll<juce::TextButton> (*editor, buttons);
        juce::TextButton* keys = nullptr;
        juce::TextButton* compare = nullptr;
        for (auto* b : buttons)
        {
            keys = b->getButtonText() == "KEYBOARD" ? b : keys;
            compare = dynamic_cast<ABButton*> (b) != nullptr ? b : compare;
        }

        auto* tabsComponent = findChild<juce::TabbedComponent> (*editor);
        auto* keyboard = findChild<KeyboardStrip> (*editor);
        expect (keys != nullptr && keyboard != nullptr && ! visibleInTree (keyboard), "the keyboard starts hidden");

        if (keys != nullptr && keyboard != nullptr && tabsComponent != nullptr)
        {
            // The keyboard only adds height below the synth: the window grows
            // by the strip and every existing panel keeps its size and place.
            const auto windowBefore = editor->getBounds();
            const auto pageBounds = tabsComponent->getBounds();
            std::vector<juce::Rectangle<int>> panelsBefore;
            std::vector<juce::Component*> panels;
            for (auto* b : buttons)
            {
                if (! b->isShowing())
                    continue; // (the hidden dialogs centre in the window, so they would move)
                panels.push_back (b);
                panelsBefore.push_back (b->getScreenBounds() - editor->getScreenPosition());
            }

            keys->triggerClick();
            settle (150);
            expect (visibleInTree (keyboard), "KEYS shows the keyboard");
            expect (editor->getWidth() == windowBefore.getWidth() && editor->getHeight() > windowBefore.getHeight(),
                    "KEYS grows the window (" + juce::String (windowBefore.getHeight()) + " to " + juce::String (editor->getHeight()) + " px)");
            expect (tabsComponent->getBounds() == pageBounds, "KEYS leaves the page area where it was and the same size");
            auto panelsStay = true;
            for (size_t i = 0; i < panels.size(); ++i)
                panelsStay = panelsStay && (panels[i]->getScreenBounds() - editor->getScreenPosition()) == panelsBefore[i];
            expect (panelsStay, "KEYS leaves the header and tab buttons where they were");
            expect (keyboard->getScreenBounds().getY() >= tabsComponent->getScreenBounds().getBottom()
                        && keyboard->getScreenBounds().getBottom() <= editor->getScreenBounds().getBottom(),
                    "the keyboard sits below the pages inside the taller window");

            keys->triggerClick();
            settle (150);
            expect (! visibleInTree (keyboard) && editor->getBounds() == windowBefore && tabsComponent->getBounds() == pageBounds,
                    "KEYS hides it again and the window returns to its size");
        }

        // Flair 2026-11-01: the keyboard lights for every note the synth
        // plays (the host's too), as hard as it was hit, and lets go with it.
        {
            juce::AudioBuffer<float> buffer (2, 256);
            juce::MidiBuffer midi;
            const auto epoch = processor.getUiNoteEpoch();
            midi.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
            processor.processBlock (buffer, midi);
            expect (processor.getUiNoteVelocity (64) == 100 && processor.getUiNoteEpoch() != epoch,
                    "a host note shows on the keyboard with its velocity (" + juce::String (processor.getUiNoteVelocity (64)) + ")");
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
            buffer.clear();
            processor.processBlock (buffer, midi);
            expect (processor.getUiNoteVelocity (64) == 0, "the key goes dark when the host lets the note go");
        }

        expect (compare != nullptr && compare->getWidth() <= 64, "the header's compare is a compact A|B");

        if (compare != nullptr)
        {
            compare->triggerClick();
            settle (150);
            expect (compare->getButtonText() == "B" && compare->getToggleState(), "A|B flips to B and lights");
            compare->triggerClick();
            settle (150);
            expect (compare->getButtonText() == "A" && ! compare->getToggleState(), "A|B flips back to A");
        }

        // UI review 6, header and global: VOICES as "3/32", a tuning
        // indicator while a scale retunes, MASTER at 0 dB on the factory
        // presets (their level in the hidden trim), the hover line.
        {
            expect (pages->getVoicesText().matchesWildcard ("*/*", true)
                        && pages->getVoicesText().fromFirstOccurrenceOf ("/", false, false).getIntValue() >= 1,
                    "VOICES reads as a count of the most (" + pages->getVoicesText() + ")");

            settle (400); // (the A|B swaps above land first: they restore the tuning too)
            expect (pages->getTuningIndicatorText().isEmpty(), "no tuning indicator in 12-TET");
            juce::String error;
            const auto loaded = processor.loadTuningScale ("! test.scl\nQuarter tones\n 2\n!\n 150.0\n 2/1\n", error);
            settle (600); // (the header refreshes on a 250 ms timer)
            expect (loaded && pages->getTuningIndicatorText().startsWith ("TUNING: "),
                    "a Scala scale shows the tuning indicator (" + pages->getTuningIndicatorText() + error + ", on "
                        + juce::String (processor.apvts.getRawParameterValue ("tuning_on")->load()) + ", scale "
                        + processor.getTuningState().getDescription() + ")");
            if (auto* tuningOn = processor.apvts.getParameter ("tuning_on"))
                tuningOn->setValueNotifyingHost (0.0f);
            settle (600);
            expect (pages->getTuningIndicatorText().isEmpty(), "switching the tuning off hides the indicator ("
                                                                   + pages->getTuningIndicatorText() + ", on "
                                                                   + juce::String (processor.apvts.getRawParameterValue ("tuning_on")->load()) + ")");

            const auto factoryNames = processor.getFactoryPresetNames();
            const auto presetBefore = factoryNames.indexOf (processor.getCurrentPresetName());
            auto atZero = 0, checked = 0;
            juce::String offender;
            for (int index = 0; index < factoryNames.size(); index += 7)
            {
                processor.loadFactoryPreset (index);
                const auto master = processor.apvts.getRawParameterValue ("master")->load();
                ++checked;
                if (std::abs (master) < 0.01f)
                    ++atZero;
                else if (offender.isEmpty())
                    offender = factoryNames[index] + " " + juce::String (master, 2);
            }
            processor.loadFactoryPreset (0);
            const auto initTrim = processor.apvts.getRawParameterValue ("output_trim")->load();
            processor.loadFactoryPreset (juce::jmax (0, presetBefore));
            settle (100);
            expect (atZero == checked && std::abs (initTrim + 5.1f) < 0.05f,
                    "MASTER reads 0 dB on the factory presets, the level in the hidden trim (" + juce::String (atZero) + "/"
                        + juce::String (checked) + offender + ", Init trim " + juce::String (initTrim, 1) + " dB)");
            const auto* trimParam = dynamic_cast<juce::AudioProcessorParameterWithID*> (processor.apvts.getParameter ("output_trim"));
            expect (trimParam != nullptr && ! trimParam->isAutomatable(), "the preset trim is hidden from automation");

            // UI review 7 (I7-23): MASTER's hover names the preset's own
            // level, and the preset menu resets it (one undo step).
            {
                processor.loadFactoryPreset (0);
                settle (150);
                const auto tip = pages->getMasterTooltip();
                const auto shown = pages->presetLevelText();
                pages->resetPresetLevel();
                settle (150);
                const auto reset = processor.apvts.getRawParameterValue ("output_trim")->load();
                const auto tipAfter = pages->getMasterTooltip();
                processor.getUndoManager().undo();
                const auto undone = processor.apvts.getRawParameterValue ("output_trim")->load();
                expect (shown == "-5.1 dB" && tip.contains ("preset's own level: -5.1 dB") && std::abs (reset) < 0.01f
                            && tipAfter.contains ("level: 0.0 dB") && std::abs (undone + 5.1f) < 0.05f,
                        "MASTER's hover names the preset level, and its reset is one undo step (" + shown + ", reset " + juce::String (reset, 2)
                            + ", undone " + juce::String (undone, 2) + ", " + tipAfter.fromLastOccurrenceOf ("  ", false, false) + ")");
                processor.loadFactoryPreset (juce::jmax (0, presetBefore));
                settle (100);
            }

            auto& hover = pages->getHoverLine();
            hover.restOn (compare);
            expect (compare == nullptr || (hover.isShowingLine() && hover.getShownTitle().isNotEmpty()),
                    "the hover line shows while the mouse rests on a control (" + hover.getShownTitle() + ")");
            hover.restOn (nullptr);
            bool clicksHere = true, clicksBelow = true;
            hover.getInterceptsMouseClicks (clicksHere, clicksBelow);
            expect (! hover.isShowingLine() && ! clicksHere && ! clicksBelow,
                    "the hover line is gone when the mouse leaves, and never takes a click");
        }

        // MATRIX is a tab of its own (2026-10-10): no switcher shows on MOD.
        pages->showPage ("ENV/LFO");
        settle (150);
        std::vector<SectionSwitcher*> switchers;
        findAll<SectionSwitcher> (*editor, switchers);
        auto anySwitcher = false;
        for (auto* sw : switchers)
            anySwitcher = anySwitcher || visibleInTree (sw);
        expect (! anySwitcher, "MOD has one page, so no page switch shows");
        pages->showPage ("MATRIX");
        settle (200);
        expect (pages->getCurrentPageId() == "MATRIX", "MATRIX opens in its own tab");
    }

    // Matrix shows the preset's routing with the right destination text.
    pages->showPage ("MATRIX");
    settle (300);

    std::vector<MatrixRow*> rows;
    findAll<MatrixRow> (*editor, rows);
    MatrixRow* firstRow = nullptr;

    for (auto* row : rows)
        if (row->isVisible() && row->getSlotIndex() == 0)
            firstRow = row;

    expect (firstRow != nullptr, "matrix shows slot 1 for a preset that uses it");

    if (firstRow != nullptr)
    {
        std::vector<juce::ComboBox*> combos;
        findAll<juce::ComboBox> (*firstRow, combos);

        auto showsCutoff = false;

        for (auto* combo : combos)
            showsCutoff = showsCutoff || combo->getText() == ModNames::destination ((int) Mod::Destination::Filter1Cutoff);

        expect (showsCutoff, "matrix row shows the preset's destination as Filter 1 > Cutoff");

        for (auto* combo : combos)
        {
            if (combo->getText() == ModNames::destination ((int) Mod::Destination::Filter1Cutoff))
            {
                combo->setSelectedId ((int) Mod::Destination::Osc2Warp + 1, juce::sendNotificationSync);
                settle (50);
                expect (processor.readModSlot (0).destination == (int) Mod::Destination::Osc2Warp,
                        "picking a grouped destination writes the right index");
            }
        }
    }

    // Dropping a source on a knob routes it, and the knob grows a dot.
    // (BODY folds while its coupling is off: V9-4, so it is switched on.)
    if (auto* resOn = processor.apvts.getParameter ("res_on"))
        resOn->setValueNotifyingHost (1.0f);
    pages->showPage ("FILTER");
    settle (300);

    std::vector<KnobControl*> knobs;
    findAll<KnobControl> (*editor, knobs);
    KnobControl* reso = nullptr;

    for (auto* knob : knobs)
        if (knob->getParameterId() == "f1_reso" && visibleInTree (knob))
            reso = knob;

    expect (reso != nullptr, "filter page has a resonance knob");
    for (const auto* id : { "body_material", "body_size", "body_coupling" })
    {
        const auto found = std::any_of (knobs.begin(), knobs.end(), [id] (const KnobControl* knob)
        {
            return knob->getParameterId() == id && visibleInTree (knob) && knob->getWidth() > 30;
        });
        expect (found, juce::String ("BODY card shows ") + id);
    }

    if (reso != nullptr)
    {
        juce::DragAndDropTarget::SourceDetails details ("modsource:" + juce::String ((int) Mod::Source::Lfo2), nullptr, {});
        reso->itemDropped (details);
        settle (100);

        auto routed = -1;

        for (int i = 0; i < Mod::maxSlots; ++i)
        {
            const auto slot = processor.readModSlot (i);

            if (slot.source == Mod::Source::Lfo2 && slot.destination == (int) Mod::Destination::Filter1Reso)
                routed = i;
        }

        expect (routed >= 0 && reso->getNumRoutings() == 1, "dropping LFO 2 on RESO routes it and shows one dot");

        std::vector<ModDotStrip*> strips;
        findAll<ModDotStrip> (*reso, strips);

        if (! strips.empty() && strips[0]->onDepthChange != nullptr && routed >= 0)
        {
            strips[0]->onDepthChange (routed, -0.6f);
            expect (std::abs (processor.readModSlot (routed).depth + 0.6f) < 1.0e-3f, "dragging the dot sets the depth");

            // UI review 4 (V2): a double-click zeroes the depth (as knobs and
            // the source card do), keeps the routing, is one named undo
            // step, and undo brings the depth back.
            // (A knob too narrow for badges beside its rings leaves them
            // to the rings, which take the same double-click.)
            reso->syncRoutings();
            const auto onRing = strips[0]->getNumShown() == 0;
            auto& strip = onRing ? reso->getRingOverlay() : static_cast<juce::Component&> (*strips[0]);
            const auto at = onRing ? strip.getLocalPoint (reso, reso->getRingPoint (0, 0.5f))
                                   : juce::Point<float> ((float) strip.getWidth() * 0.5f, (float) ModDotStrip::dotPitch * 0.5f);
            const juce::MouseEvent click (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(), 1.0f, 0.0f,
                                          0.0f, 0.0f, 0.0f, &strip, &strip, juce::Time::getCurrentTime(), at,
                                          juce::Time::getCurrentTime(), 2, false);
            clearHistory();
            strip.mouseDoubleClick (click);
            settle (100);
            const auto zeroed = processor.readModSlot (routed).source == Mod::Source::Lfo2
                                && std::abs (processor.readModSlot (routed).depth) < 1.0e-4f;
            const auto steps = undoSteps();
            processor.getUndoManager().undo();
            settle (100);
            expect (zeroed && steps.size() == 1 && steps[0] == "Zero LFO 2 depth"
                        && processor.readModSlot (routed).source == Mod::Source::Lfo2
                        && std::abs (processor.readModSlot (routed).depth + 0.6f) < 1.0e-3f,
                    "double-clicking a depth dot zeroes it, keeps the routing, is one undo step ('" + steps.joinIntoString ("', '")
                        + "') and undo restores the depth");

            // Removal and bypass are on the dot's right-click menu.
            expect (strips[0]->onRemove != nullptr && strips[0]->onBypass != nullptr, "a depth dot offers Remove and Bypass");
            if (strips[0]->onBypass != nullptr)
            {
                strips[0]->onBypass (routed, true);
                const auto bypassed = processor.readModSlot (routed).bypass;
                strips[0]->onBypass (routed, false);
                expect (bypassed && ! processor.readModSlot (routed).bypass, "the dot's Bypass switches the routing off and on");
            }

            // Dropping the same source again is refused: it points at the
            // existing routing instead of adding a second.
            reso->itemDropped (details);
            settle (100);
            auto copies = 0;
            for (int i = 0; i < Mod::maxSlots; ++i)
            {
                const auto slot = processor.readModSlot (i);
                copies += slot.source == Mod::Source::Lfo2 && slot.destination == (int) Mod::Destination::Filter1Reso ? 1 : 0;
            }
            expect (copies == 1 && reso->getNumRoutings() == 1,
                    "dropping a source on a knob it already drives adds no second routing (" + juce::String (copies) + ")");
            reso->closeModCard();
        }
        else
        {
            expect (false, "knob has a dot strip");
        }

        // An effect knob is a drop target too (plain-parameter destination).
        pages->showPage ("FX");
        processor.assignFxSlot (1, 20); // OTT
        settle (400);
        knobs.clear();
        findAll<KnobControl> (*editor, knobs);
        KnobControl* ottAmount = nullptr;

        for (auto* knob : knobs)
            if (knob->getParameterId() == "fx_ott_amount" && visibleInTree (knob))
                ottAmount = knob;

        expect (ottAmount != nullptr && ottAmount->isInterestedInDragSource (details), "OTT amount accepts modulation");
    }

    // (The old MODULATION card tests are retired: PLAY now carries its envelope and LFO carousels, covered in PlayPageUiTests.)

    // Legacy presets get named default macros, and the matrix shows the names.
    {
        auto showsMacroName = false;

        for (auto* row : rows)
        {
            std::vector<juce::ComboBox*> combos;
            findAll<juce::ComboBox> (*row, combos);

            for (auto* combo : combos)
                showsMacroName = showsMacroName || combo->getText() == "TONE";
        }

        expect (showsMacroName, "matrix source shows the macro's name alone ('TONE', review 9 I9-8)");
    }

    // ENV/LFO: every envelope has a card, and picking one shows its controls.
    pages->showPage ("ENV/LFO");
    settle (300);

    if (auto* page = pages->getCurrentPage())
    {
        if (auto* envCards = findChild<EnvThumbBar> (*page); envCards != nullptr && envCards->onSelect != nullptr)
        {
            envCards->onSelect (3);
            settle (100);

            knobs.clear();
            findAll<KnobControl> (*page, knobs);
            auto modAttackShown = false, ampAttackShown = false;

            for (auto* knob : knobs)
            {
                modAttackShown = modAttackShown || (knob->getParameterId() == "me_attack" && visibleInTree (knob));
                ampAttackShown = ampAttackShown || (knob->getParameterId() == "amp_attack" && visibleInTree (knob));
            }

            expect (modAttackShown && ! ampAttackShown, "clicking the MOD envelope card shows the mod envelope");
            envCards->onSelect (0);
        }
        else
        {
            expect (false, "ENV/LFO page has envelope cards");
        }
    }

    // Envelope graph: a dragged handle lands where the mouse is.
    pages->showPage ("ENV/LFO");
    settle (200);

    if (auto* page = pages->getCurrentPage())
    {
        std::vector<EnvelopeDisplay*> displays;
        findAll<EnvelopeDisplay> (*page, displays);
        EnvelopeDisplay* amp = nullptr;

        for (auto* display : displays)
            if (visibleInTree (display))
                amp = display;

        const auto set = [&processor] (const char* id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };

        const auto get = [&processor] (const char* id) { return processor.apvts.getRawParameterValue (id)->load(); };

        if (amp != nullptr)
        {
            set ("amp_attack", 0.25f);
            set ("amp_decay", 0.3f);
            set ("amp_sustain", 0.5f);
            set ("amp_release", 0.5f);

            // Mirror of the display's geometry.
            const auto plot = amp->getPlotArea();
            const auto total = std::sqrt (0.25f) + std::sqrt (0.3f) + 0.55f + std::sqrt (0.5f);
            const auto scale = plot.getWidth() / juce::jmax (1.6f, total * 1.15f);
            const auto xA = plot.getX() + scale * std::sqrt (0.25f);
            const auto xS = xA + scale * std::sqrt (0.3f) + scale * 0.55f;
            const auto xR = xS + scale * std::sqrt (0.5f);

            auto source = juce::Desktop::getInstance().getMainMouseSource();
            const auto drag = [&] (juce::Point<float> from, juce::Point<float> to)
            {
                const auto now = juce::Time::getCurrentTime();
                const auto make = [&] (juce::Point<float> at, bool dragged)
                {
                    return juce::MouseEvent (source, at, juce::ModifierKeys::leftButtonModifier, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                             amp, amp, now, from, now, 1, dragged);
                };

                auto& component = static_cast<juce::Component&> (*amp);
                component.mouseDown (make (from, false));
                component.mouseDrag (make (to, true));
                component.mouseUp (make (to, true));
            };

            drag ({ xA, plot.getY() }, { xA + 30.0f, plot.getY() });
            const auto expectedAttack = std::pow ((xA + 30.0f - plot.getX()) / scale, 2.0f);
            expect (std::abs (get ("amp_attack") - expectedAttack) < expectedAttack * 0.03f,
                    "attack handle follows the mouse (" + juce::String (get ("amp_attack"), 3) + " s, expected "
                        + juce::String (expectedAttack, 3) + ")");

            // Re-measure with the new attack, then drag the release end.
            const auto total2 = std::sqrt (get ("amp_attack")) + std::sqrt (0.3f) + 0.55f + std::sqrt (0.5f);
            const auto scale2 = plot.getWidth() / juce::jmax (1.6f, total2 * 1.15f);
            const auto xS2 = plot.getX() + scale2 * (std::sqrt (get ("amp_attack")) + std::sqrt (0.3f) + 0.55f);
            const auto xR2 = xS2 + scale2 * std::sqrt (0.5f);
            drag ({ xR2, plot.getBottom() }, { xR2 - 25.0f, plot.getBottom() });
            const auto expectedRelease = std::pow ((xR2 - 25.0f - xS2) / scale2, 2.0f);
            expect (std::abs (get ("amp_release") - expectedRelease) < expectedRelease * 0.03f,
                    "release handle follows the mouse (" + juce::String (get ("amp_release"), 3) + " s, expected "
                        + juce::String (expectedRelease, 3) + ")");
            juce::ignoreUnused (xR);

            // Drag feel (ilana: "adjusting adsrs via the graph feels weird"): the first click does not
            // jump the handle to the pointer, Shift is 10x finer, dragging left past the stage's start
            // does not turn into a long time, a double-click resets.
            {
                set ("amp_attack", 0.25f);
                set ("amp_decay", 0.3f);
                set ("amp_sustain", 0.5f);
                set ("amp_release", 0.5f);
                const auto gesture = [&] (juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods, int clicks = 1)
                {
                    const auto now = juce::Time::getCurrentTime();
                    const auto make = [&] (juce::Point<float> at, bool dragged)
                    {
                        return juce::MouseEvent (source, at, mods.withFlags (juce::ModifierKeys::leftButtonModifier), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                 amp, amp, now, from, now, clicks, dragged);
                    };
                    auto& component = static_cast<juce::Component&> (*amp);
                    component.mouseDown (make (from, false));
                    component.mouseDrag (make (to, true));
                    component.mouseUp (make (to, true));
                };
                const auto attackHandle = amp->getHandlePosition (0);

                gesture (attackHandle + juce::Point<float> (6.0f, 0.0f), attackHandle + juce::Point<float> (6.0f, 0.0f), {});
                expect (std::abs (get ("amp_attack") - 0.25f) < 0.003f,
                        "a press 6 px off the attack handle does not move it (" + juce::String (get ("amp_attack"), 4) + " s)");

                const auto before = get ("amp_attack");
                gesture (attackHandle, attackHandle + juce::Point<float> (20.0f, 0.0f), {});
                const auto coarse = get ("amp_attack") - before;
                set ("amp_attack", before);
                gesture (attackHandle, attackHandle + juce::Point<float> (20.0f, 0.0f), juce::ModifierKeys::shiftModifier);
                const auto fine = get ("amp_attack") - before;
                expect (coarse > 0.0f && fine > 0.0f && fine < coarse * 0.2f,
                        "Shift drags the handle about 10x finer (" + juce::String (fine, 4) + " vs " + juce::String (coarse, 4) + ")");

                set ("amp_attack", 0.25f);
                gesture (attackHandle, attackHandle - juce::Point<float> (400.0f, 0.0f), {});
                expect (get ("amp_attack") < 0.01f,
                        "dragging left of the stage's start gives the shortest time, not a long one (" + juce::String (get ("amp_attack"), 3) + " s)");

                set ("amp_attack", 0.25f);
                set ("amp_attack", 0.6f);
                gesture (amp->getHandlePosition (0), amp->getHandlePosition (0), {}, 2);
                static_cast<juce::Component&> (*amp).mouseDoubleClick (juce::MouseEvent (source, amp->getHandlePosition (0), {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                                                         amp, amp, juce::Time::getCurrentTime(), amp->getHandlePosition (0), juce::Time::getCurrentTime(), 2, false));
                if (auto* parameter = processor.apvts.getParameter ("amp_attack"))
                    expect (std::abs (get ("amp_attack") - parameter->convertFrom0to1 (parameter->getDefaultValue())) < 0.002f,
                            "double-clicking the attack handle resets it");

                // A knob: 20 px up moves it 10% of its range, Shift 1%, sideways nothing, Ctrl-click resets.
                std::vector<KnobControl*> knobs;
                findAll<KnobControl> (*page, knobs);
                KnobControl* knob = nullptr;
                for (auto* candidate : knobs)
                    if (visibleInTree (candidate) && candidate->getSlider().getInterval() == 0.0 && candidate->getSlider().isEnabled())
                    {
                        knob = candidate;
                        break;
                    }
                if (knob != nullptr)
                {
                    auto& slider = knob->getSlider();
                    slider.setPopupDisplayEnabled (false, false, nullptr); // never open a real popup window in a test
                    const auto centre = juce::Point<float> (slider.getWidth() * 0.5f, slider.getHeight() * 0.5f);
                    const auto press = [&] (juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods)
                    {
                        const auto now = juce::Time::getCurrentTime();
                        const auto make = [&] (juce::Point<float> at, bool dragged)
                        {
                            return juce::MouseEvent (source, at, mods.withFlags (juce::ModifierKeys::leftButtonModifier), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                     &slider, &slider, now, from, now, 1, dragged);
                        };
                        slider.mouseDown (make (from, false));
                        slider.mouseDrag (make (to, true));
                        slider.mouseUp (make (to, true));
                    };
                    const auto proportion = [&slider] { return slider.valueToProportionOfLength (slider.getValue()); };
                    slider.setValue (slider.proportionOfLengthToValue (0.5), juce::sendNotificationSync);
                    press (centre, centre - juce::Point<float> (0.0f, 20.0f), {});
                    expect (std::abs (proportion() - 0.6) < 0.002, "a knob moves 10% for a 20 px drag up (" + juce::String (proportion(), 4) + ")");
                    slider.setValue (slider.proportionOfLengthToValue (0.5), juce::sendNotificationSync);
                    press (centre, centre + juce::Point<float> (30.0f, 0.0f), {});
                    expect (std::abs (proportion() - 0.5) < 0.002, "a sideways drag does not turn a knob (" + juce::String (proportion(), 4) + ")");
                    press (centre, centre - juce::Point<float> (0.0f, 20.0f), juce::ModifierKeys::shiftModifier);
                    expect (std::abs (proportion() - 0.51) < 0.002, "Shift turns a knob 10x finer (" + juce::String (proportion(), 4) + ")");
                    press (centre, centre, juce::ModifierKeys::ctrlModifier);
                    expect (std::abs (slider.getValue() - slider.getDoubleClickReturnValue()) < 1.0e-6, "Ctrl-click resets a knob to its default");
                }
                else
                {
                    expect (false, "ENV/LFO page has a continuous knob to drag");
                }
            }
        }
        else
        {
            expect (false, "ENV/LFO shows the amp envelope display");
        }
    }

    // MAIN shows OSC 1-3 by default, and only those; added ones get the same
    // full card, with the mode's own knobs.
    {
        pages->showPage ("MAIN");
        settle (300);

        const auto shownOnMain = [&] (const juce::String& id)
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
                if (visibleInTree (knob) && knob->getParameterId() == id)
                    return true;
            return false;
        };

        const auto mainKnob = [&] (const juce::String& id) -> KnobControl*
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
                if (visibleInTree (knob) && knob->getParameterId() == id)
                    return knob;
            return nullptr;
        };

        // A switched-off oscillator keeps its place, folded to a slim strip
        // with its switch (UI review 6, V19, S8; review 8, V8-17).
        const juce::String osc3 (OscillatorIds::prefixes[2]);
        if (auto* on = processor.apvts.getParameter (osc3 + "_on"))
            on->setValueNotifyingHost (0.0f);
        settle (300);
        expect (mainKnob (osc3 + "_level") != nullptr && toggleFor (osc3 + "_on") != nullptr,
                "MAIN keeps a switched-off OSC 3's whole card, with its switch");

        if (auto* on = processor.apvts.getParameter (osc3 + "_on"))
            on->setValueNotifyingHost (1.0f);
        settle (300);

        expect (shownOnMain (osc3 + "_level") && ! shownOnMain ("osc4_level") && toggleFor ("osc4_on") == nullptr,
                "MAIN shows OSC 3 and hides OSC 4 by default");

        // Every strip is the same height, and the next one to add is one
        // button in the first empty slot (UI review 6, S33).
        {
            std::vector<juce::Button*> buttons;
            findAll<juce::Button> (*editor, buttons);
            auto adds = 0;
            for (auto* button : buttons)
                if (visibleInTree (button) && button->getButtonText().contains ("ADD OSC"))
                    ++adds;
            expect (adds <= 1, "PLAY offers at most one ADD OSC button (" + juce::String (adds) + ")");
            auto* level1 = mainKnob ("osc1_level");
            auto* level3 = mainKnob (osc3 + "_level");
            expect (level1 != nullptr && level3 != nullptr && level1->getHeight() == level3->getHeight(),
                    "PLAY's oscillator strips share one size"
                        + (level1 != nullptr && level3 != nullptr ? " (" + juce::String (level1->getHeight()) + ", " + juce::String (level3->getHeight()) + "; "
                                                                 + level1->getBoundsInParent().toString() + " / " + level3->getBoundsInParent().toString() + "; "
                                                                 + juce::String (level1->labelBlockHeight()) + " " + juce::String (level3->labelBlockHeight()) + " "
                                                                 + juce::String (level1->getNameLabel().getFont().getHeight()) + " " + juce::String (level3->getNameLabel().getFont().getHeight()) + ")" : juce::String()));
            if (only == "STRIPS") return uiFailures == 0 ? 0 : 1; // (a quick loop for this check)
        }

        for (int osc = 3; osc < OscillatorIds::count; ++osc)
            processor.addOscillator (osc);

        for (int osc = 0; osc < OscillatorIds::count; ++osc)
        {
            const juce::String prefix (OscillatorIds::prefixes[(size_t) osc]);
            if (auto* mode = processor.apvts.getParameter (prefix + "_mode"))
                mode->setValueNotifyingHost (mode->convertTo0to1 (3.0f));
            if (auto* on = processor.apvts.getParameter (prefix + "_on"))
                on->setValueNotifyingHost (1.0f);

            settle (400);
            std::vector<KnobControl*> mainKnobs;
            findAll<KnobControl> (*editor, mainKnobs);
            auto level = false, grainSize = false;

            for (auto* knob : mainKnobs)
            {
                if (! visibleInTree (knob))
                    continue;

                level = level || knob->getParameterId() == prefix + "_level";
                grainSize = grainSize || knob->getParameterId() == prefix + "_grain_size";
            }

            expect (level && grainSize,
                    "MAIN's OSC " + juce::String (osc + 1)
                        + " card shows level and grain size in granular mode");

            if (auto* mode = processor.apvts.getParameter (prefix + "_mode"))
                mode->setValueNotifyingHost (mode->convertTo0to1 (0.0f));
        }

        for (int osc = 3; osc < OscillatorIds::count; ++osc)
            processor.removeOscillator (osc);

        settle (300);
        expect (! shownOnMain ("osc4_level"), "removing OSC 4 takes its card off MAIN");
    }

    // An empty FX rack offers one-click effects.
    {
        for (int slot = 1; slot <= IlanaSynthAudioProcessor::numFxSlots; ++slot)
            processor.assignFxSlot (slot, 0);

        pages->showPage ("FX");
        settle (500);
        std::vector<juce::TextButton*> textButtons;
        findAll<juce::TextButton> (*editor, textButtons);
        juce::TextButton* reverb = nullptr;

        juce::TextButton* delay = nullptr;

        for (auto* button : textButtons)
        {
            if (button->getButtonText() == "REVERB" && visibleInTree (button))
                reverb = button;
            if (button->getButtonText() == "DELAY" && visibleInTree (button))
                delay = button;
        }

        expect (reverb != nullptr && delay != nullptr, "the empty rack shows the effect library");

        if (reverb != nullptr)
        {
            reverb->triggerClick();
            settle (400);
            const auto* slot1 = processor.apvts.getRawParameterValue ("fx_slot1");
            expect (slot1 != nullptr && (int) slot1->load() == 13, "the library's REVERB puts a reverb in slot 1");
            // One view of the chain (S5-18, S6-25): with an effect loaded the
            // library leaves the page; + ADD EFFECT opens it in a call-out.
            std::vector<FxLibraryButton*> libraryButtons;
            findAll<FxLibraryButton> (*editor, libraryButtons);
            expect (std::none_of (libraryButtons.begin(), libraryButtons.end(), [] (FxLibraryButton* b) { return visibleInTree (b); }),
                    "no library or chain list beside the cards once the rack has an effect");

            juce::Button* addEffect = nullptr;
            std::vector<juce::Button*> allButtons;
            findAll<juce::Button> (*editor, allButtons);
            for (auto* button : allButtons)
                if (button->getButtonText() == "+ ADD EFFECT" && visibleInTree (button))
                    addEffect = button;
            expect (addEffect != nullptr, "the rack's top bar has + ADD");

            if (addEffect != nullptr)
            {
                addEffect->triggerClick();
                // (Briefly: a call-out closes itself within 200 ms while the
                // process isn't in front, as under xvfb.)
                settle (80);
                FxLibraryView* callout = nullptr;
                std::vector<FxLibraryView*> views;
                findAll<FxLibraryView> (*editor, views);
                for (auto* view : views)
                    if (view->getName() == "FX LIBRARY" && visibleInTree (view))
                        callout = view;
                expect (callout != nullptr, "+ ADD EFFECT opens the library in a call-out");

                if (callout != nullptr)
                {
                    auto* inRack = callout->findButton (13);
                    expect (inRack != nullptr && inRack->isEnabled() && inRack->getInRackSlot() == 0
                                && inRack->getTooltip().contains ("slot 1"),
                            "a type already in the rack has an in-rack dot and says where, not greyed out (V6-25)");
                    // (UI review 7, I7-28: Spaces is Reverb's Airwindows model, beside it.)
                    auto* spaces = callout->findButton (34);
                    expect (spaces != nullptr && spaces->getKind() == FxLibraryButton::Kind::airwindowsModel && spaces->getTooltip().contains ("Spaces")
                                && spaces->getTooltip().contains ("Airwindows"),
                            "Airwindows effects are named by their job with an Airwindows note (S6-27)");
                    if (auto* addDelay = callout->findButton (9))
                        addDelay->triggerClick();
                    settle (400);
                    const auto* slot2 = processor.apvts.getRawParameterValue ("fx_slot2");
                    expect (slot2 != nullptr && (int) slot2->load() == 9, "a pick from the call-out goes into the next empty slot");
                    views.clear();
                    findAll<FxLibraryView> (*editor, views);
                    expect (std::none_of (views.begin(), views.end(), [] (FxLibraryView* v) { return v->getName() == "FX LIBRARY" && visibleInTree (v); }),
                            "the call-out closes after a pick");
                }
            }
        }

        // The names: by job, the catch-all "Airwindows (all)", no "AW" prefix.
        auto awPrefix = false;
        for (int type = 1; type <= IlanaSynthAudioProcessor::numFxTypes; ++type)
            awPrefix = awPrefix || fxTypeName (type).startsWith ("AW ") || fxTypeName (type) == "-";
        expect (! awPrefix && fxTypeName (30) == "Airwindows (all)" && fxTypeName (35) == "Echoes",
                "every effect has a library name, none starts with AW, the catch-all is \"Airwindows (all)\"");
    }

    // FX cards (UI review 4, batch D): sized to their controls, a display
    // per family, one MIX, the header's solo / band / SLOT BLEND.
    {
        const auto loadFx = [&] (std::initializer_list<int> types)
        {
            auto slot = 1;
            for (auto type : types)
                processor.assignFxSlot (slot++, type);
            for (; slot <= IlanaSynthAudioProcessor::numFxSlots; ++slot)
                processor.assignFxSlot (slot, 0);
            settle (400);
        };
        // The stack's viewport: the one whose content holds the FX knobs.
        const auto stackViewport = [&]() -> juce::Viewport*
        {
            std::vector<juce::Viewport*> viewports;
            findAll<juce::Viewport> (*editor, viewports);
            for (auto* viewport : viewports)
            {
                if (! visibleInTree (viewport) || viewport->getViewedComponent() == nullptr)
                    continue;
                std::vector<KnobControl*> knobs;
                findAll<KnobControl> (*viewport->getViewedComponent(), knobs);
                for (auto* knob : knobs)
                    if (knob->getParameterId().startsWith ("fx_"))
                        return viewport;
            }
            return nullptr;
        };
        // Every shown control of the cards (and the displays), in content coordinates.
        const auto cardControls = [&] (juce::Viewport& viewport)
        {
            std::vector<juce::Component*> found;
            for (auto* child : viewport.getViewedComponent()->getChildren())
                if (child->isVisible() && ! child->getBounds().isEmpty())
                    found.push_back (child);
            return found;
        };
        const auto checkCards = [&] (const juce::String& what, bool allInView)
        {
            auto* viewport = stackViewport();
            expect (viewport != nullptr, what + ": the FX stack is on screen");
            if (viewport == nullptr)
                return;
            viewport->setViewPosition (0, 0);
            settle (100);
            const auto controls = cardControls (*viewport);
            const auto view = viewport->getViewArea();
            // (A packed knob's bounds reach packedPadX past its cell for the ring's room, as on PLAY:
            // neighbours are judged by their cells. A stacked choice in a knob's cell says so.)
            const auto cellOf = [] (juce::Component* control)
            {
                const auto* knob = dynamic_cast<KnobControl*> (control);
                if ((knob != nullptr && knob->isPacked()) || (bool) control->getProperties().getWithDefault ("packedCell", false))
                    return control->getBounds().reduced (KnobControl::packedPadX, 0).withTrimmedTop (KnobControl::packedPadTop);
                return control->getBounds();
            };
            juce::String clipped, overlapping;
            for (size_t i = 0; i < controls.size(); ++i)
            {
                const auto bounds = controls[i]->getBounds();
                if (! viewport->getViewedComponent()->getLocalBounds().contains (bounds) || (allInView && ! view.contains (bounds)))
                    clipped << controls[i]->getName() << typeid (*controls[i]).name() << bounds.toString() << " ";
                for (size_t j = i + 1; j < controls.size(); ++j)
                    if (cellOf (controls[i]).intersects (cellOf (controls[j])))
                        overlapping << bounds.toString() << "/" << controls[j]->getBounds().toString() << " ";
            }
            expect (clipped.isEmpty(), what + ": no FX card control is clipped " + clipped);
            expect (overlapping.isEmpty(), what + ": no two FX card controls overlap " + overlapping);
        };
        const auto shownKnob = [&] (const juce::String& id)
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
                if (knob->getParameterId() == id && visibleInTree (knob) && ! knob->getBounds().isEmpty())
                    return true;
            return false;
        };
        const auto shownDisplays = [&]
        {
            std::vector<FxDisplay*> displays;
            findAll<FxDisplay> (*editor, displays);
            return (int) std::count_if (displays.begin(), displays.end(), [] (FxDisplay* d) { return visibleInTree (d); });
        };
        const auto findButtons = [&] (const juce::String& text)
        {
            std::vector<juce::TextButton*> buttons, matching;
            findAll<juce::TextButton> (*editor, buttons);
            for (auto* button : buttons)
                if (button->getButtonText() == text && visibleInTree (button))
                    matching.push_back (button);
            return matching;
        };

        pages->showPage ("FX");
        loadFx ({ 27, 2, 20 }); // Neuro Wobble's rack: Vowel, Drive, OTT
        checkCards ("3 effects", true);
        expect (shownDisplays() == 3, "Vowel, Drive and OTT cards each show a display");
        expect (findButtons ("S").size() == 3, "each card header has a SOLO button");
        loadFx ({ 27, 2, 20, 13 });
        checkCards ("4 effects", false);
        loadFx ({ 9, 4, 13, 21 });
        checkCards ("delay, comp, reverb, limiter", false);
        expect (shownDisplays() == 4, "delay, comp, reverb and limiter each show a display");

        // The delay's custom tap grid only while TAPS is on.
        const auto tapGridShown = [&]
        {
            std::vector<juce::Component*> all;
            findAll<juce::Component> (*editor, all);
            for (auto* component : all)
                if (component->getName() == "CUSTOM TAP GRID")
                    return visibleInTree (component);
            return false;
        };
        auto* taps = processor.apvts.getParameter ("fx_taps_on");
        taps->setValueNotifyingHost (0.0f);
        settle (300);
        const auto hiddenWhileOff = ! tapGridShown();
        taps->setValueNotifyingHost (1.0f);
        settle (300);
        expect (hiddenWhileOff && tapGridShown(), "the delay's tap grid shows only while TAPS is on");
        checkCards ("delay with taps", false);
        taps->setValueNotifyingHost (0.0f);

        // Solo is a visible button in the card header.
        if (auto soloButtons = findButtons ("S"); ! soloButtons.empty())
        {
            soloButtons.front()->triggerClick();
            settle (200);
            expect (processor.apvts.getRawParameterValue ("fx_slot1_solo")->load() > 0.5f, "the header's SOLO button solos its slot");
            soloButtons.front()->triggerClick();
            settle (200);
        }

        // The slot's dry / wet, but only where the effect has no MIX of its
        // own (UI review 7, V7-7): here the limiter's, as a MIX knob in its
        // row (UI review 8, S8-6).
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            auto inCards = 0;
            for (auto* knob : knobs)
                if (knob->getParameterId().startsWith ("fx_slot") && knob->getParameterId().endsWith ("_mix") && visibleInTree (knob)
                    && ! knob->getBounds().isEmpty())
                    ++inCards;
            expect (inCards == 1, "only the limiter's card (no MIX of its own) has the slot's MIX (" + juce::String (inCards) + " of 4)");
        }

        // One MIX: the Airwindows algorithms' own Dry/Wet is hidden.
        loadFx ({ 33 }); // AW Saturation; its first effect, Density3, has Dry/Wet as knob 4
        processor.apvts.getParameter ("fx_awsat_algo")->setValueNotifyingHost (0.0f);
        settle (300);
        expect (shownKnob ("fx_awsat_p1") && ! shownKnob ("fx_awsat_p4") && shownKnob ("fx_awsat_mix"),
                "AW Saturation shows DENSITY and MIX but not the algorithm's Dry/Wet");
        {
            // The rack's words and units (I9-15 / S9-5): Density3 reads
            // AMOUNT, LOW CUT and OUTPUT in dB, as the built-in twin would.
            std::vector<KnobControl*> awKnobs;
            findAll<KnobControl> (*editor, awKnobs);
            juce::String labels, outputText;
            for (const auto* id : { "fx_awsat_p1", "fx_awsat_p2", "fx_awsat_p3" })
                for (auto* knob : awKnobs)
                    if (knob->getParameterId() == id && visibleInTree (knob))
                    {
                        labels << knob->getLabelText() << "/";
                        if (juce::String (id) == "fx_awsat_p3")
                            outputText = knob->getSlider().getTextFromValue (knob->getSlider().getValue());
                    }
            expect (labels == "AMOUNT/LOW CUT/OUTPUT/" && outputText.endsWith (" dB"),
                    "an Airwindows saturation reads AMOUNT, LOW CUT, OUTPUT in dB (" + labels + " " + outputText + ")");
        }
        expect (shownDisplays() == 1, "AW Saturation shows its transfer curve");
        loadFx ({ 30 });
        if (auto* algo = processor.apvts.getParameter ("fx_aw_algo"))
            algo->setValueNotifyingHost (algo->convertTo0to1 (3.0f)); // Density: Dry/Wet is knob 4
        settle (300);
        expect (shownKnob ("fx_aw_p1") && ! shownKnob ("fx_aw_p4") && shownKnob ("fx_aw_mix"),
                "the all-in-one Airwindows module hides the algorithm's Dry/Wet");
        {
            processor.apvts.getParameter ("fx_aw_algo")->setValueNotifyingHost (0.0f); // ToTape6
            settle (300);
            std::vector<juce::ComboBox*> boxes;
            findAll<juce::ComboBox> (*editor, boxes);
            juce::String shown;
            for (auto* box : boxes)
                if (visibleInTree (box) && box->getTooltip().startsWith ("Airwindows ToTape6"))
                    shown = box->getText();
            expect (shown == "To Tape 6", "the Airwindows menu shows \"To Tape 6\" for ToTape6 (" + shown + ")");
        }
        checkCards ("Airwindows", true);

        // DICE starts its own undo step, and undo brings the chain back.
        loadFx ({ 13, 9 });
        std::vector<DiceFxButton*> dice;
        findAll<DiceFxButton> (*editor, dice);
        if (! dice.empty())
        {
            processor.getUndoManager().beginNewTransaction();
            dice.front()->triggerClick();
            settle (300);
            const auto described = processor.getUndoManager().getUndoDescription();
            processor.getUndoManager().undo();
            settle (300);
            expect (described == "Dice FX chain" && (int) processor.apvts.getRawParameterValue ("fx_slot1")->load() == 13
                        && (int) processor.apvts.getRawParameterValue ("fx_slot2")->load() == 9,
                    "DICE FX is one undo step (" + described + ")");
        }
        else
            expect (false, "the FX toolbar has the FX dice");

        // UI review 6: small cards two to a row (S6-26), a split group with
        // its crossovers inline (S5-18, I6-27), a duplicate that says what it
        // is with REMOVE, the new displays, reorder by dragging a header.
        {
            const auto setParam = [&processor] (const juce::String& id, float plain)
            {
                if (auto* parameter = processor.apvts.getParameter (id))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
            };
            const auto readParam = [&processor] (const juce::String& id)
            {
                const auto* value = processor.apvts.getRawParameterValue (id);
                return value != nullptr ? value->load() : 0.0f;
            };
            const auto typeButtons = [&]
            {
                std::vector<FxTypeButton*> found, shown;
                findAll<FxTypeButton> (*editor, found);
                for (auto* button : found)
                    if (visibleInTree (button))
                        shown.push_back (button);
                std::sort (shown.begin(), shown.end(), [] (FxTypeButton* a, FxTypeButton* b)
                           { return a->getY() != b->getY() ? a->getY() < b->getY() : a->getX() < b->getX(); });
                return shown;
            };

            loadFx ({ 27, 2, 20 });
            auto titles = typeButtons();
            auto* viewport = stackViewport();
            // The design's rack: one row per effect, one under the other (10 px
            // apart); a short rack's rows grow from 80 (up to 120) and a dashed
            // + ADD EFFECT row takes the rest, so no band is left empty.
            {
                const auto step1 = titles.size() == 3 ? titles[1]->getY() - titles[0]->getY() : 0;
                const auto step2 = titles.size() == 3 ? titles[2]->getY() - titles[1]->getY() : 0;
                expect (titles.size() == 3 && viewport != nullptr && titles[0]->getX() == titles[1]->getX() && step1 == step2
                            && step1 > 90 && step1 <= 260,
                        "Vowel, Drive and OTT are equal rows one under the other, taller than 80 px on a short rack ("
                            + juce::String (step1) + ", " + juce::String (step2) + ")");
                std::vector<DashedAddButton*> tiles;
                findAll<DashedAddButton> (*editor, tiles);
                DashedAddButton* tile = nullptr;
                for (auto* candidate : tiles)
                    if (candidate->getButtonText().contains ("ADD EFFECT") && visibleInTree (candidate))
                        tile = candidate;
                const auto tileBox = tile != nullptr ? editor->getLocalArea (tile, tile->getLocalBounds()) : juce::Rectangle<int>();
                const auto viewBox = viewport != nullptr ? editor->getLocalArea (viewport, viewport->getLocalBounds()) : juce::Rectangle<int>();
                expect (tile != nullptr && tileBox.getY() >= viewBox.getBottom() - 4 && tileBox.getY() <= viewBox.getBottom() + 2
                            && tileBox.getHeight() >= 32,
                        "a short rack ends in a dashed + ADD EFFECT row right under its last effect");
                if (tile != nullptr)
                {
                    tile->triggerClick();
                    settle (80); // (a call-out closes itself within 200 ms under xvfb)
                    FxLibraryView* shown = nullptr;
                    std::vector<FxLibraryView*> views;
                    findAll<FxLibraryView> (*editor, views);
                    for (auto* view : views)
                        if (view->getName() == "FX LIBRARY" && visibleInTree (view))
                            shown = view;
                    expect (shown != nullptr, "the dashed + ADD EFFECT row opens the library in a call-out");
                    if (shown != nullptr)
                        if (auto* box = shown->findParentComponentOfClass<juce::CallOutBox>())
                            box->dismiss();
                    settle (300);
                }
            }
            // The knobs sit at the row's right end (no knobs stranded mid-row).
            if (titles.size() == 3 && viewport != nullptr)
            {
                std::vector<KnobControl*> knobs;
                findAll<KnobControl> (*editor, knobs);
                auto ottMixRight = 0;
                for (auto* knob : knobs)
                    if (knob->getParameterId() == "fx_ott_mix" && visibleInTree (knob))
                        ottMixRight = knob->getRight();
                expect (ottMixRight > viewport->getWidth() - 40, "a row's knobs end at its right edge (V9-3)");
            }

            loadFx ({ 7, 2, 13, 20 });
            setParam ("fx_slot2_band", 1.0f);
            setParam ("fx_slot3_band", 3.0f);
            settle (300);
            std::vector<CrossoverStrip*> strips;
            findAll<CrossoverStrip> (*editor, strips);
            const auto shownStrips = std::count_if (strips.begin(), strips.end(), [] (CrossoverStrip* s) { return visibleInTree (s); });
            expect (shownStrips == 1, "two banded slots in a row form one split group with its crossovers (" + juce::String ((int) shownStrips) + ")");
            checkCards ("a split group", false);
            setParam ("fx_slot2_band", 0.0f);
            setParam ("fx_slot3_band", 0.0f);

            loadFx ({ 20, 2, 20 });
            auto removes = findButtons ("REMOVE");
            expect (removes.size() == 1, "a duplicate card offers REMOVE");
            if (! removes.empty())
            {
                processor.getUndoManager().beginNewTransaction();
                removes.front()->triggerClick();
                settle (300);
                expect ((int) readParam ("fx_slot3") == 0 && (int) readParam ("fx_slot1") == 20
                            && processor.getUndoManager().getUndoDescription() == "Remove OTT",
                        "REMOVE takes the duplicate out as one undo step (" + processor.getUndoManager().getUndoDescription() + ")");
            }

            loadFx ({ 27, 5, 7, 6 });
            expect (shownDisplays() == 4, "vowel, comb, chorus and phaser each show a display");
            loadFx ({ 31, 35, 34 });
            expect (shownDisplays() == 3, "the vocoder and the Airwindows echoes and spaces show displays");

            // Drag Vowel's header onto OTT's card: Drive, OTT, Vowel.
            loadFx ({ 27, 2, 20 });
            titles = typeButtons();
            auto* content = viewport != nullptr ? viewport->getViewedComponent() : nullptr;
            if (titles.size() == 3 && content != nullptr)
            {
                auto source = juce::Desktop::getInstance().getMainMouseSource();
                const auto now = juce::Time::getCurrentTime();
                const auto from = juce::Point<float> ((float) titles[0]->getRight() + 12.0f, (float) titles[0]->getBounds().getCentreY());
                const auto to = juce::Point<float> ((float) titles[2]->getRight() + 40.0f, (float) titles[2]->getBounds().getCentreY() + 40.0f);
                const auto make = [&] (juce::Point<float> at, bool dragged)
                {
                    return juce::MouseEvent (source, at, juce::ModifierKeys::leftButtonModifier, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                             content, content, now, from, now, 1, dragged);
                };
                processor.getUndoManager().beginNewTransaction();
                content->mouseDown (make (from, false));
                content->mouseDrag (make (to, true));
                content->mouseUp (make (to, true));
                settle (300);
                expect ((int) readParam ("fx_slot1") == 2 && (int) readParam ("fx_slot2") == 20 && (int) readParam ("fx_slot3") == 27
                            && processor.getUndoManager().getUndoDescription() == "Move Vowel",
                        "dragging a card's header reorders the chain as one undo step (" + juce::String ((int) readParam ("fx_slot1")) + "," + juce::String ((int) readParam ("fx_slot2")) + "," + juce::String ((int) readParam ("fx_slot3")) + " " + processor.getUndoManager().getUndoDescription() + ")");
            }
            else
                expect (false, "three FX cards to drag");
        }

        // The live meters: OTT and the limiter report gain while audio runs.
        {
            loadFx ({ 20, 21 });
            processor.apvts.getParameter ("fx_limit_ceiling")->setValueNotifyingHost (0.0f); // -24 dB
            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 48, 1.0f), 0);
            for (int block = 0; block < 20; ++block)
            {
                audio.clear();
                processor.processBlock (audio, midi);
                midi.clear();
            }
            expect (processor.getLimiterGainReduction() < 0.99f && std::abs (processor.getOttBandGain (1) - 1.0f) > 0.01f,
                    "the limiter and OTT report their gain for the cards' meters");
            juce::MidiBuffer off;
            off.addEvent (juce::MidiMessage::allNotesOff (1), 0);
            processor.processBlock (audio, off);
        }

        // The drive's transfer curve shows the live input as a dot (step 14);
        // a curve whose effect isn't in the rack shows none.
        {
            loadFx ({ 2 });
            FxDisplay display (processor), absent (processor);
            display.setSize (200, 80);
            absent.setSize (200, 80);
            display.setType (2, IlanaTheme::accent());
            absent.setType (3, IlanaTheme::accent());
            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 48, 1.0f), 0);
            for (int block = 0; block < 20; ++block)
            {
                audio.clear();
                processor.processBlock (audio, midi);
                midi.clear();
            }
            display.refreshNow();
            absent.refreshNow();
            expect (absent.getInputDotLevel() == 0.0f && display.getInputDotLevel() > 0.004f,
                    "the drive's transfer curve marks the live input level (" + juce::String (display.getInputDotLevel())
                        + "), an effect not in the rack none (" + juce::String (absent.getInputDotLevel()) + ")");
            juce::MidiBuffer off;
            off.addEvent (juce::MidiMessage::allNotesOff (1), 0);
            processor.processBlock (audio, off);
        }

        processor.loadFactoryPreset (neuroWobble);
        settle (300);
    }

    // M5/M6/M6b pages.
    {
        const auto visibleKnob = [&] (const juce::String& id)
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
                if (visibleInTree (knob) && knob->getParameterId() == id && ! knob->getBounds().isEmpty())
                    return true;
            return false;
        };
        const auto set = [&processor] (const juce::String& id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };

        // FM: clicking an algorithm routes the operators.
        pages->showPage ("FM");
        settle (300);

        if (auto* strip = findChild<FmAlgorithmStrip> (*editor))
        {
            // An earlier edit in its own undo step, so undoing the algorithm
            // must leave it alone.
            processor.getUndoManager().beginNewTransaction();
            set ("osc2_level", 0.33f);
            settle (200);

            // The DX7 page: cell 5 is DX7 algorithm 5, under its own number.
            strip->setPage (FmAlgorithmStrip::dx7Low);
            settle (100);
            auto source = juce::Desktop::getInstance().getMainMouseSource();
            const auto at = strip->getCellCentre (4).toFloat();
            const auto now = juce::Time::getCurrentTime();
            const juce::MouseEvent click (source, at, juce::ModifierKeys::leftButtonModifier, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                          strip, strip, now, at, now, 1, false);
            static_cast<juce::Component&> (*strip).mouseDown (click);
            static_cast<juce::Component&> (*strip).mouseUp (click);
            settle (400);
            expect (processor.findMatchingDx7Algorithm() == 5 && processor.isOscillatorShown (5),
                    "clicking DX7 algorithm 5 routes six operators and is found as 5");
            expect (visibleKnob ("fm_6to5"), "the FM matrix grows to six operators (its noise row follows the EXTRAS line, I11-5)");

            processor.getUndoManager().undo();
            settle (300);
            const auto afterUndo = processor.findMatchingDx7Algorithm();
            const auto level = processor.apvts.getRawParameterValue ("osc2_level")->load();
            processor.getUndoManager().redo();
            settle (300);
            expect (afterUndo != 5 && std::abs (level - 0.33f) < 0.01f && processor.findMatchingDx7Algorithm() == 5,
                    "undo reverts an algorithm as one step and keeps the edit before it (level "
                        + juce::String (level, 2) + ")");
            expect (strip->getNumCells() == 16 && strip->dx7NumberAt (15) == 16,
                    "the DX7 1-16 page holds sixteen DX7 algorithms by number");
            strip->setPage (FmAlgorithmStrip::dx7High);
            expect (strip->getNumCells() == 16 && strip->dx7NumberAt (0) == 17 && strip->dx7NumberAt (15) == 32,
                    "the DX7 17-32 page holds the other sixteen");
            strip->setPage (FmAlgorithmStrip::basic);
        }
        else
        {
            expect (false, "the FM page has the algorithm strip");
        }

        // The operator panel follows the selected operator and its tuning.
        std::vector<juce::TextButton*> buttons;
        findAll<juce::TextButton> (*editor, buttons);
        // The operators on the one picker (UI review 8, I8-10).
        std::vector<OscPicker*> pickers;
        findAll<OscPicker> (*editor, pickers);
        for (auto* picker : pickers)
            if (visibleInTree (picker))
                picker->pick (1);
        set ("osc2_tune", (float) OscTuning::Ratio);
        settle (400);
        expect (visibleKnob ("osc2_ratio") && ! visibleKnob ("osc1_ratio") && ! visibleKnob ("osc2_fixed_hz"),
                "selecting OSC 2 in Ratio tuning shows its RATIO knob");
        set ("osc2_tune", (float) OscTuning::Fixed);
        settle (400);
        expect (visibleKnob ("osc2_fixed_hz") && ! visibleKnob ("osc2_ratio"), "Fixed tuning swaps RATIO for FIXED");

        // UI review 6: DX7 voices on the FM page. Every eighth factory DX7
        // voice: its algorithm lit under its number, the diagram's nodes
        // apart and inside it with their captions, at least its minimum
        // height; the Amp Env shown as unused; the wheel and pressure routed.
        {
            auto voices = 0, lit = 0, laidOut = 0, ampUnused = 0, wheel = 0;
            juce::String firstBad;
            for (int index = 0; index < names.size(); ++index)
            {
                if (! names[index].contains ("(ROM") || index % 8 != 0)
                    continue;
                processor.loadFactoryPreset (index);
                pages->showPage ("FM");
                settle (150);
                ++voices;
                auto* strip = findChild<FmAlgorithmStrip> (*editor);
                auto* diagram = findChild<FmDiagram> (*editor);
                if (strip == nullptr || diagram == nullptr)
                    continue;
                strip->refreshMatch();
                const auto stored = juce::roundToInt (processor.apvts.getRawParameterValue (OperatorEg::dx7AlgorithmId)->load());
                lit += stored > 0 && strip->getMatchingDx7() == stored ? 1 : 0;

                const auto nodes = diagram->getNodeBounds();
                const auto captions = diagram->getCaptionBoundsList();
                const auto area = diagram->getLocalBounds().toFloat();
                auto apart = diagram->getHeight() >= diagram->getMinimumHeight();
                for (size_t i = 0; i < nodes.size(); ++i)
                {
                    apart = apart && area.contains (nodes[i]) && area.contains (captions[i]);
                    for (size_t j = 0; j < nodes.size(); ++j)
                        apart = apart && (i == j || (! nodes[i].intersects (nodes[j]) && ! captions[i].intersects (nodes[j])
                                                     && ! captions[i].intersects (captions[j])));
                }
                laidOut += apart ? 1 : 0;
                if (! apart && firstBad.isEmpty())
                {
                    firstBad = names[index] + " in " + diagram->getLocalBounds().toString() + " (minimum height " + juce::String (diagram->getMinimumHeight()) + "):";
                    for (size_t i = 0; i < nodes.size(); ++i)
                        firstBad << " [" << nodes[i].toString() << " / " << captions[i].toString() << "]";
                }
                ampUnused += FmOperatorInfo::ampEnvelopeInUse (processor) ? 0 : 1;

                auto routes = 0;
                for (int slot = 0; slot < Mod::maxSlots; ++slot)
                {
                    const auto routing = processor.readModSlot (slot);
                    routes += (routing.source == Mod::Source::ModWheel || routing.source == Mod::Source::Aftertouch)
                                      && Mod::getDestinationNames()[routing.destination] == "OP LFO Pitch Depth"
                                  ? 1 : 0;
                }
                wheel += routes == 2 ? 1 : 0;
            }
            expect (voices >= 30 && lit == voices, "every DX7 voice lights its algorithm by number ("
                                                       + juce::String (lit) + " of " + juce::String (voices) + ")");
            expect (laidOut == voices, "the FM diagram never overlaps or clips a DX7 voice's operators ("
                                           + juce::String (laidOut) + " of " + juce::String (voices)
                                           + (firstBad.isNotEmpty() ? ", first " + firstBad : juce::String()) + ")");
            expect (ampUnused == voices, "a DX7 voice's Amp Env reads as unused (its operators play the Operator Env)");
            expect (wheel == voices, "a DX7 voice routes the wheel and pressure to the Op LFO's pitch depth");

            // The tallest DX7 stacks fit too, with every depth label clear of
            // the nodes, captions and other labels, and the operator card
            // does not move between algorithms (review 7, V7-3 and I7-10).
            processor.loadFactoryPreset (names.indexOf ("E.PIANO 1 (ROM1A)"));
            juce::Rectangle<int> firstDiagramBounds;
            auto cardFixed = true;
            for (int number = 1; number <= 32; ++number)
            {
                processor.applyDx7Algorithm (number);
                settle (150);
                auto* diagram = findChild<FmDiagram> (*editor);
                auto apart = diagram != nullptr && diagram->getHeight() >= diagram->getMinimumHeight();
                auto labelsClear = diagram != nullptr;
                juce::String badLabels;
                if (diagram != nullptr)
                {
                    const auto nodes = diagram->getNodeBounds();
                    const auto captions = diagram->getCaptionBoundsList();
                    const auto labels = diagram->getAmountLabelBoundsList();
                    for (size_t i = 0; i < nodes.size(); ++i)
                        for (size_t j = 0; j < nodes.size(); ++j)
                            apart = apart && diagram->getLocalBounds().toFloat().contains (nodes[i]) && (i == j || ! nodes[i].intersects (nodes[j]));
                    for (size_t i = 0; i < labels.size(); ++i)
                    {
                        auto clear = diagram->getLocalBounds().toFloat().contains (labels[i]);
                        for (size_t j = 0; j < nodes.size(); ++j)
                            clear = clear && ! labels[i].intersects (nodes[j].reduced (nodes[j].getWidth() * 0.06f))
                                    && ! labels[i].intersects (captions[j]);
                        for (size_t j = 0; j < labels.size(); ++j)
                            clear = clear && (i == j || ! labels[i].intersects (labels[j]));
                        if (! clear)
                            badLabels << " [" << labels[i].toString() << "]";
                        labelsClear = labelsClear && clear;
                    }
                    if (firstDiagramBounds.isEmpty())
                        firstDiagramBounds = diagram->getBounds();
                    cardFixed = cardFixed && diagram->getBounds() == firstDiagramBounds;
                }
                // Review 8 (V8-7): each stack stands straight, as a DX7
                // chart (an operator driving only one that it alone drives
                // sits right over it), halos never meet, and no depth sits
                // on an arrowhead.
                auto straight = diagram != nullptr, halosApart = diagram != nullptr, headsClear = diagram != nullptr;
                auto columnsClose = diagram != nullptr;
                if (diagram != nullptr)
                {
                    const auto nodes = diagram->getNodeBounds();
                    // Neighbouring columns no further apart than a node, its
                    // caption and a gap: algorithm 1's two stacks read as one
                    // chart, not islands at the box's edges.
                    std::vector<float> xs;
                    for (const auto& node : nodes)
                        xs.push_back (node.getCentreX());
                    std::sort (xs.begin(), xs.end());
                    for (size_t i = 1; i < xs.size(); ++i)
                        columnsClose = columnsClose && xs[i] - xs[i - 1] <= nodes.front().getWidth() + 58.0f + 101.0f;
                    const auto routed = [&] (int a, int b)
                    {
                        return a != b && processor.apvts.getRawParameterValue (FmDiagram::routeId (a, b))->load() > 0.001f;
                    };
                    for (int m = 0; m < 6 && nodes.size() == 6; ++m)
                        for (int t = 0; t < 6; ++t)
                        {
                            if (! routed (m, t))
                                continue;
                            auto drivers = 0, driven = 0;
                            for (int other = 0; other < 6; ++other)
                            {
                                drivers += routed (other, t) ? 1 : 0;
                                driven += routed (m, other) ? 1 : 0;
                            }
                            if (drivers == 1 && driven == 1)
                                straight = straight && std::abs (nodes[(size_t) m].getCentreX() - nodes[(size_t) t].getCentreX()) < 1.0f
                                           && nodes[(size_t) m].getBottom() + 12.0f < nodes[(size_t) t].getY();
                        }
                    const auto halo = diagram->getHaloRoom();
                    for (size_t i = 0; i < nodes.size(); ++i)
                        for (size_t j = i + 1; j < nodes.size(); ++j)
                            halosApart = halosApart && ! nodes[i].expanded (halo).intersects (nodes[j].expanded (halo));
                    for (const auto& label : diagram->getAmountLabelBoundsList())
                        for (const auto& path : diagram->getRoutePathsList())
                            for (size_t k = path.size() - 2; k < path.size(); ++k)
                                headsClear = headsClear && ! label.intersects (juce::Line<float> (path[k - 1], path[k]));
                }
                expect (straight, "DX7 algorithm " + juce::String (number) + "'s stacks stand straight over what they drive");
                expect (columnsClose, "DX7 algorithm " + juce::String (number) + "'s columns sit close, as one chart");
                expect (halosApart, "DX7 algorithm " + juce::String (number) + "'s node halos never meet");
                expect (headsClear, "DX7 algorithm " + juce::String (number) + "'s depth labels sit off the arrowheads");
                expect (apart, "DX7 algorithm " + juce::String (number) + "'s stack fits the diagram without overlaps");
                expect (labelsClear, "DX7 algorithm " + juce::String (number) + "'s depth labels sit clear of nodes, captions and each other" + badLabels);
            }
            expect (cardFixed, "the operator card keeps one height across DX7 algorithms 1-32");

            // One node style in every layout: "OSC n" inside the circle,
            // also for a three-oscillator patch (V7-13).
            processor.loadFactoryPreset (names.indexOf ("Neuro Wobble"));
            pages->showPage ("FM");
            settle (200);
            if (auto* diagram = findChild<FmDiagram> (*editor))
                expect (diagram->getOperatorRadius() >= 21.0f && diagram->getNodeBounds().size() >= 2,
                        "a basic FM patch draws full-size OSC n nodes");
            expect (FmAlgorithmStrip::nearestBasic (processor) == 0,
                    "a one-modulator routing that no tile matches is named after the nearest tile (B1)");

            // Review 8. One operator card height on every patch and for
            // every kind of operator, panel and tab (I8-11): the diagram
            // above it never moves. An off oscillator's node shows no tuning
            // or level (S8-36). One matrix cell size on every patch, and the
            // card no taller than its rows (V8-6, V8-35). The ring mod, sync
            // and noise rows fold away on a DX7 voice (S8-21). No OP PITCH
            // link on a patch without the Operator Env.
            {
                const auto diagramBounds = [&]
                {
                    auto* diagram = findChild<FmDiagram> (*editor);
                    return diagram != nullptr ? diagram->getBounds() : juce::Rectangle<int>();
                };
                const auto knobBounds = [&] (const juce::String& id)
                {
                    std::vector<KnobControl*> all;
                    findAll<KnobControl> (*editor, all);
                    for (auto* knob : all)
                        if (visibleInTree (knob) && knob->getParameterId() == id)
                            return knob->getBounds();
                    return juce::Rectangle<int>();
                };
                const auto clickButton = [&] (const juce::String& text)
                {
                    std::vector<juce::TextButton*> all;
                    findAll<juce::TextButton> (*editor, all);
                    for (auto* button : all)
                        if (button->getButtonText() == text && visibleInTree (button))
                        {
                            button->triggerClick();
                            settle (250);
                            return true;
                        }
                    return false;
                };
                const auto pickOsc = [&] (int osc)
                {
                    std::vector<OscPicker*> pickers;
                    findAll<OscPicker> (*editor, pickers);
                    for (auto* picker : pickers)
                        if (visibleInTree (picker))
                            picker->pick (osc);
                    settle (250);
                };
                const auto pitchLfoLinkShown = [&]
                {
                    std::vector<juce::TextButton*> all;
                    findAll<juce::TextButton> (*editor, all);
                    return std::any_of (all.begin(), all.end(), [] (juce::TextButton* button)
                                        { return button->getButtonText().contains ("OP PITCH") && visibleInTree (button); });
                };
                const auto heightOnNeuro = diagramBounds().getHeight();
                auto* neuroDiagram = findChild<FmDiagram> (*editor);
                const auto offCaption = neuroDiagram != nullptr && neuroDiagram->getCaptionTexts().size() == 3
                                        && neuroDiagram->getCaptionTexts()[2].isEmpty() && neuroDiagram->getCaptionTexts()[0].isNotEmpty();
                expect (offCaption, "FM: an off oscillator's node shows no tuning or level captions (S8-36)");
                const auto neuroCell = knobBounds ("fm_amount"), neuroNext = knobBounds ("fm_fb2");
                // (ilana's FM option A, 2026-10-29: the extras always stand in the right column under FM MODE.)
                expect (visibleKnob ("ring_mod") && visibleKnob ("fm_noise_color"),
                        "FM: a basic patch shows RING MOD, SYNC 2 TO 1 and the noise's COLOUR in the EXTRAS row");
                expect (! pitchLfoLinkShown(), "FM: no OP PITCH link on a patch without the Operator Env");
                std::vector<EnvelopeDisplay*> graphs;
                findAll<EnvelopeDisplay> (*editor, graphs);
                expect (std::any_of (graphs.begin(), graphs.end(), [] (EnvelopeDisplay* graph) { return visibleInTree (graph); }),
                        "FM: an Amp Env operator's card shows its envelope where an Operator Env one shows its own");
                pickOsc (1);
                const auto sameOnOsc2 = diagramBounds().getHeight() == heightOnNeuro;

                processor.loadFactoryPreset (names.indexOf ("E.PIANO 1 (ROM1A)"));
                settle (300);
                pickOsc (0);
                // (2026-10-29: a patch without the Operator Env gets the one-row card, a DX7 voice the taller one; within a
                // patch the height never changes.)
                const auto heightOnDx = diagramBounds().getHeight();
                auto sameKinds = sameOnOsc2;
                std::vector<CardTabs*> tabs;
                findAll<CardTabs> (*editor, tabs);
                for (auto* tab : tabs)
                    if (visibleInTree (tab) && tab->getNames().contains ("KEYS & VELOCITY"))
                    {
                        tab->setSelected (1, true);
                        settle (250);
                        sameKinds = sameKinds && diagramBounds().getHeight() == heightOnDx;
                        tab->setSelected (0, true);
                    }
                expect (pitchLfoLinkShown(), "FM: a DX7 voice's operator card links to OP PITCH and OP LFO");
                pickOsc (1);
                sameKinds = sameKinds && diagramBounds().getHeight() == heightOnDx;
                expect (sameKinds, "FM: the operator card keeps one height across a patch's operators and tabs (I8-11)");

                // Each route in use: a row in ROUTES and a knob on its cable, both the same parameter.
                {
                    std::vector<KnobControl*> all;
                    findAll<KnobControl> (*editor, all);
                    const auto shownFor = [&] (const juce::String& id)
                    {
                        return (int) std::count_if (all.begin(), all.end(), [&] (KnobControl* knob) { return visibleInTree (knob) && knob->getParameterId() == id; });
                    };
                    expect (shownFor ("fm_amount") == 2 && shownFor ("fm_fb6") == 2 && shownFor ("fm_1to2") == 0,
                            "FM: a route in use has its ROUTES row and its knob on the cable; one not in use has neither ("
                                + juce::String (shownFor ("fm_amount")) + ", " + juce::String (shownFor ("fm_fb6")) + ", " + juce::String (shownFor ("fm_1to2")) + ")");
                }
                expect (visibleKnob ("ring_mod"), "FM: a DX7 voice shows the EXTRAS too (one layout)");
            }

            // The operator card speaks the synth's words: a rate as a time, a
            // level as dB, the output as OUTPUT and the oscillator level as LEVEL.
            processor.loadFactoryPreset (names.indexOf ("E.PIANO 1 (ROM1A)"));
            pages->showPage ("FM");
            settle (300);
            {
                std::vector<OscPicker*> pickers;
                findAll<OscPicker> (*editor, pickers);
                for (auto* picker : pickers)
                    if (visibleInTree (picker))
                        picker->pick (0);
            }
            settle (300);
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            juce::String attackText, peakText, levelLabel, trimLabel;
            for (auto* knob : knobs)
            {
                const auto id = knob->getParameterId();
                if (id == "osc1_eg_r1")
                    attackText = knob->getLabelText() + " " + knob->getSlider().getTextFromValue (knob->getSlider().getValue());
                if (id == "osc1_eg_l1")
                    peakText = knob->getLabelText() + " " + knob->getSlider().getTextFromValue (knob->getSlider().getValue());
                if (id == "osc1_eg_out" && visibleInTree (knob))
                    levelLabel = knob->getLabelText();
                if (id == "osc1_level")
                    trimLabel = knob->getLabelText();
            }
            expect (attackText.startsWith ("ATTACK ") && (attackText.endsWith (" ms") || attackText.endsWith (" s"))
                        && peakText.startsWith ("PEAK ") && peakText.endsWith (" dB") && levelLabel == "OUTPUT" && (trimLabel == "LEVEL" || trimLabel == "VOICE LEVEL"),
                    "the Operator Env reads in the synth's words and units (" + attackText + ", " + peakText + ")");

            // Review 7: the time knobs turn the normal way (clockwise is
            // longer), a ratio reads "×1.00", a silent level "-inf dB", and
            // no visible text or help names the "Operator EG" or "Op EG".
            for (auto* knob : knobs)
                if (knob->getParameterId() == "osc1_eg_r1")
                    expect (knob->getSlider().valueToProportionOfLength (10.0) > knob->getSlider().valueToProportionOfLength (90.0),
                            "an Operator Env time knob turns clockwise for a longer stage (I7-3)");
            expect (describeValue ("osc2_ratio", 1.0f) == juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) + "1.00"
                        && describeValue ("osc2_eg_l2", 0.0f) == silentDecibels() && describeValue ("osc2_eg_out", 0.0f) == silentDecibels(),
                    "a ratio reads x1.00 and a silent Operator Env level " + silentDecibels());
            {
                juce::StringArray stale;
                for (auto* parameter : processor.getParameters())
                    if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
                        for (const auto& text : { describeParameter (withId->paramID), withId->getName (100) })
                            if (text.contains ("Operator EG") || text.contains ("Op EG") || text.contains ("OP EG"))
                                stale.add (withId->paramID);
                std::vector<juce::Component*> all;
                findAll<juce::Component> (*editor, all);
                for (auto* component : all)
                {
                    juce::String text;
                    if (auto* label = dynamic_cast<juce::Label*> (component))
                        text = label->getText();
                    if (auto* button = dynamic_cast<juce::Button*> (component))
                        text = button->getButtonText() + " " + button->getTooltip();
                    if (auto* tip = dynamic_cast<juce::SettableTooltipClient*> (component))
                        text << " " << tip->getTooltip();
                    if (text.contains ("Operator EG") || text.contains ("Op EG") || text.contains ("OP EG"))
                        stale.add (text.substring (0, 40));
                }
                for (const auto& name : Mod::getDestinationNames())
                    if (name.contains ("Op EG") || name.contains ("Op Env") || name.contains ("Op LFO") || name.contains ("Op Pitch"))
                        stale.add (name);
                expect (stale.isEmpty(), "the Operator Env has one name everywhere (I7-6): " + stale.joinIntoString (", "));
            }
            // SPACE is live on a DX7 voice: the reverb is on, its dry kept.
            expect (processor.apvts.getRawParameterValue ("fx_reverb_on")->load() > 0.5f
                        && processor.apvts.getRawParameterValue ("fx_reverb_keep_dry")->load() > 0.5f,
                    "a DX7 voice's SPACE macro drives a reverb that is on (I7-4)");

            // Its graph is an editor: four handles inside the plot.
            OperatorEnvDisplay* graph = nullptr;
            std::vector<OperatorEnvDisplay*> graphs;
            findAll<OperatorEnvDisplay> (*editor, graphs);
            for (auto* candidate : graphs)
                if (visibleInTree (candidate))
                    graph = candidate;
            auto handlesInside = graph != nullptr;
            if (graph != nullptr)
                for (const auto point : graph->getHandlePositions())
                    handlesInside = handlesInside && graph->getLocalBounds().toFloat().contains (point);
            expect (handlesInside, "the Operator Env graph shows its four stage handles");

            // FB TYPE dims without a feedback route (OSC 1 on E.PIANO 1);
            // empty matrix cells are dots (their knobs hidden until hovered).
            std::vector<ComboControl*> combos;
            findAll<ComboControl> (*editor, combos);
            auto fbDimmed = false;
            for (auto* combo : combos)
                if (visibleInTree (combo) && combo->getComboBox().getText() == "DX7")
                    fbDimmed = fbDimmed || combo->getAlpha() < 1.0f;
            expect (fbDimmed, "FB TYPE dims on an operator with no feedback route");
            auto emptyHidden = true, usedShown = false;
            for (auto* knob : knobs)
            {
                if (! visibleInTree (knob))
                    continue;
                if (knob->getParameterId() == FmDiagram::routeId (0, 1))
                    emptyHidden = false;
                if (knob->getParameterId() == FmDiagram::routeId (1, 0))
                    usedShown = usedShown || knob->getAlpha() > 0.99f;
            }
            expect (emptyHidden && usedShown, "an unused FM route shows no knob; a route in use shows its knob");

            // A switched-off oscillator's routes stay listed but can't be edited.
            set ("osc2_on", 0.0f);
            settle (300);
            auto offRow = true;
            for (auto* knob : knobs)
                if (visibleInTree (knob) && knob->getParameterId() == FmDiagram::routeId (1, 0))
                    offRow = offRow && ! knob->isEnabled();
            expect (offRow, "an off oscillator's FM routes can't be edited");
            set ("osc2_on", 1.0f);

            // PITCH & LFO opens the voice's pitch envelope.
            findAll<juce::TextButton> (*editor, buttons);
            for (auto* button : buttons)
                if (button->getButtonText().contains ("OP PITCH") && visibleInTree (button))
                    button->triggerClick();
            settle (300);
            graphs.clear();
            findAll<OperatorEnvDisplay> (*editor, graphs);
            auto pitchShown = false;
            for (auto* candidate : graphs)
                pitchShown = pitchShown || (visibleInTree (candidate) && candidate->isPitch());
            expect (pitchShown && pages->getCurrentPageId() == "MAIN", "OP PITCH · OP LFO opens the pitch envelope's graph in its pop-up on PLAY");
            // (Closed again: its cross, not an FM route's remove button.)
            findAll<juce::TextButton> (*editor, buttons);
            for (auto* button : buttons)
                if (button->getButtonText() == juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) && button->getName() != "removeRoute" && visibleInTree (button))
                {
                    button->triggerClick();
                    break;
                }
            settle (200);

            // The MOD pools carry OP ENV, OP PITCH and OP LFO as pool cards,
            // edited in place (UI review 7, I7-7); a DX7 voice opens on OP
            // ENV rather than the unused AMP ENV (I7-8).
            pages->showPage ("ENV/LFO");
            settle (600);
            {
                auto* modPage = pages->getCurrentPage();
                auto* lfoCards = modPage != nullptr ? findChild<LfoThumbBar> (*modPage) : nullptr;
                auto* envCards = modPage != nullptr ? findChild<EnvThumbBar> (*modPage) : nullptr;
                const auto opLfoCard = IlanaSynthAudioProcessor::numLfos + 2;
                const auto inPools = lfoCards != nullptr && envCards != nullptr && envCards->isCardInPool (16) && envCards->isCardInPool (17)
                                     && lfoCards->isCardInPool (opLfoCard);
                std::vector<OperatorEnvEditor*> envEditors;
                findAll<OperatorEnvEditor> (*editor, envEditors);
                auto opensOnOpEnv = false;
                for (auto* candidate : envEditors)
                    opensOnOpEnv = opensOnOpEnv || (visibleInTree (candidate) && ! candidate->isPitch() && visibleInTree (&candidate->getGraph()));
                expect (inPools && opensOnOpEnv, "a DX7 voice has OP ENV, OP PITCH and OP LFO as MOD pool cards and opens on OP ENV");

                // UI review 8, I8-5: there they come first in their rows, and
                // stay in view when the pools fill up (cards fold from the
                // right).
                if (inPools)
                {
                    const auto firstIn = [] (auto& bar, int card, int other)
                    {
                        const auto cardBounds = bar.boundsOfCard (card), otherBounds = bar.boundsOfCard (other);
                        return ! cardBounds.isEmpty() && (otherBounds.isEmpty() || cardBounds.getX() < otherBounds.getX());
                    };
                    expect (firstIn (*envCards, 16, 0) && firstIn (*envCards, 17, 0) && firstIn (*lfoCards, opLfoCard, 0),
                            "on a DX7 voice OP ENV, OP PITCH and OP LFO come first in their pools");

                    using M = IlanaSynthAudioProcessor::Module;
                    std::vector<bool> lfosBefore, envsBefore;
                    for (int lfo = 0; lfo < IlanaSynthAudioProcessor::numLfos; ++lfo)
                        lfosBefore.push_back (processor.isRevealed (M::Lfo, lfo));
                    for (int env = 0; env < 16; ++env)
                        envsBefore.push_back (processor.isRevealed (M::Envelope, env));
                    for (int lfo = 0; lfo < IlanaSynthAudioProcessor::numLfos; ++lfo)
                        processor.setRevealed (M::Lfo, lfo, true);
                    for (int env = 0; env < 16; ++env)
                        processor.setRevealed (M::Envelope, env, true);
                    lfoCards->refreshLayout();
                    envCards->refreshLayout();
                    settle (200);
                    const auto lfoFolded = lfoCards->getFoldedCards(), envFolded = envCards->getFoldedCards();
                    const auto folded = [] (const std::vector<int>& list, int card) { return std::find (list.begin(), list.end(), card) != list.end(); };
                    expect (! envFolded.empty() && ! lfoFolded.empty() && ! folded (envFolded, 16) && ! folded (envFolded, 17)
                                && ! folded (lfoFolded, opLfoCard) && envCards->isCardShown (16) && lfoCards->isCardShown (opLfoCard),
                            "full pools on a DX7 voice keep OP ENV, OP PITCH and OP LFO in view (the others fold)");
                    for (int lfo = 0; lfo < IlanaSynthAudioProcessor::numLfos; ++lfo)
                        processor.setRevealed (M::Lfo, lfo, lfosBefore[(size_t) lfo]);
                    for (int env = 0; env < 16; ++env)
                        processor.setRevealed (M::Envelope, env, envsBefore[(size_t) env]);
                    lfoCards->refreshLayout();
                    envCards->refreshLayout();
                    settle (100);
                }

                pages->showPage ("ENV/LFO");
                settle (200);

                pages->showPage ("ENV/LFO");
                settle (200);

                // OP PITCH opens the pitch envelope's graph, OP LFO its panel
                // with the shape list in the pool LFOs' order.
                if (inPools)
                {
                    envCards->onSelect (17);
                    lfoCards->onSelect (opLfoCard);
                    settle (300);
                    auto pitchShown = false;
                    for (auto* candidate : envEditors)
                        pitchShown = pitchShown || (visibleInTree (candidate) && candidate->isPitch() && candidate->getGraph().isPitch());
                    std::vector<OperatorLfoEditor*> lfoEditors;
                    findAll<OperatorLfoEditor> (*editor, lfoEditors);
                    auto lfoShown = false, names = false;
                    for (auto* candidate : lfoEditors)
                        if (visibleInTree (candidate))
                        {
                            lfoShown = true;
                            auto& box = candidate->getShape().getComboBox();
                            names = box.getItemText (4) == "Sine" && box.getItemText (5) == "S&H";
                        }
                    expect (pitchShown && lfoShown && names, "OP PITCH and OP LFO open their editors in the MOD pools, the LFO's "
                                                             "shapes named as the pool LFOs' are");
                    envCards->onSelect (0);
                    lfoCards->onSelect (0);
                    settle (100);
                }

                processor.loadFactoryPreset (neuroWobble);
                settle (300);
                // UI review 8, I8-5 / S8-1 / V8-1: elsewhere they are absent
                // (not greyed), and so is the old MSEG module (I8-4), from the
                // pools and the source menus.
                const auto gone = lfoCards != nullptr && envCards != nullptr && ! envCards->isCardInPool (16) && ! envCards->isCardInPool (17)
                                  && ! lfoCards->isCardInPool (opLfoCard) && ! lfoCards->isCardInPool (IlanaSynthAudioProcessor::numLfos);
                expect (gone && ! FmOperatorInfo::anyOperatorEnv (processor) && ! modSourceInPatch (processor, Mod::Source::OpLfo)
                            && ! modSourceInPatch (processor, Mod::Source::OpPitchEnv) && ! modSourceInPatch (processor, Mod::Source::Mseg),
                        "a patch without the Operator Env has no OP ENV / OP PITCH / OP LFO cards, nor an MSEG card it doesn't use");
                {
                    pages->showPage ("MAIN");
                    settle (300);
                    std::vector<CardTabs*> tabs;
                    if (auto* page = pages->getCurrentPage())
                        findAll<CardTabs> (*page, tabs);
                    auto noOpTab = true;
                    for (auto* candidate : tabs)
                        if (candidate->getNames().contains ("AMP ENV"))
                            noOpTab = noOpTab && ! candidate->getNames().contains ("OP ENV") && candidate->getNames()[0] == "AMP ENV";
                    expect (noOpTab, "PLAY's ENVELOPE card has no OP ENV tab on a patch without the Operator Env");
                    pages->showPage ("ENV/LFO");
                    settle (100);
                }
                // S8-1 / V8-1: there the "+" menu offers the Operator Env,
                // and picking it puts OSC 1 on it and opens OP ENV.
                if (envCards != nullptr && envCards->plusOffer != nullptr && envCards->onPlusOffer != nullptr)
                {
                    const auto offered = envCards->plusOffer().startsWith ("OP ENV (DX7)");
                    envCards->onPlusOffer();
                    settle (300);
                    const auto added = FmOperatorInfo::anyOperatorEnv (processor) && envCards->isCardInPool (16)
                                       && envCards->plusOffer().isEmpty();
                    expect (offered && added, "\"+\" offers OP ENV (DX7) on a patch without it, and picking it adds the Operator Env");
                    processor.loadFactoryPreset (neuroWobble);
                    settle (300);
                    expect (! envCards->isCardInPool (16), "a patch loaded without the Operator Env takes its card away again");
                    // (The page's timer, which leaves a card that went away,
                    // only runs on screen.)
                    envCards->onSelect (0);
                    settle (100);
                }
                else
                    expect (false, "the envelope pool's \"+\" has an Operator Env item");
            }
            pages->showPage ("FM");
            settle (200);
        }

        // OSC: the PD chain row is always there (V14-6); its amounts dim while their stage is Off.
        pages->showPage ("OSC");
        settle (300);
        set ("osc1_mode", 0.0f);
        set ("osc1_warp", 0.0f);
        set ("osc1_warp2", 0.0f);
        settle (300);
        const auto drawnBefore = visibleKnob ("osc1_warp2_amt");
        const auto boundsOf = [&] (const juce::String& id)
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
                if (visibleInTree (knob) && knob->getParameterId() == id)
                    return knob->getScreenBounds();
            return juce::Rectangle<int>();
        };
        const auto ringBefore = boundsOf ("osc1_warp2_amt");
        set ("osc1_warp", (float) Warp::PdSaw);
        settle (300);
        set ("osc1_warp2", 1.0f);
        settle (300);
        // (2026-10-22: WARP 2 always has its row, so picking it never changes the card's height.)
        expect (drawnBefore && visibleKnob ("osc1_warp2_amt")
                    && boundsOf ("osc1_warp2_amt") == ringBefore,
                "OSC 1 shows its WARP 2 row while Off, and picking a stage keeps the card's height");

        // M7.3: Tine and Reed swap the string controls for the pickup (on
        // the oscillator's card).
        set ("osc1_mode", 1.0f);
        set ("osc1_excite", 7.0f);
        settle (300);
        expect (visibleKnob ("osc1_ep_distance") && visibleKnob ("osc1_ep_position") && visibleKnob ("osc1_hammer_hard")
                    && ! visibleKnob ("osc1_string_stiffness") && ! visibleKnob ("osc1_string_sustain"),
                "Tine shows DISTANCE, OFFSET and HAMMER instead of the string controls");
        set ("osc1_excite", 0.0f);
        settle (300);
        expect (! visibleKnob ("osc1_ep_distance") && visibleKnob ("osc1_string_stiffness"),
                "a plucked string hides the pickup controls again");
        set ("osc1_mode", 0.0f);
        settle (300);

        // M7.4: EDIT copies a factory table into a free patch table and
        // opens the editor on it; edits reach the table the oscillator plays.
        {
            const auto factoryCount = TableFactory::getNumFactoryTables();
            set ("osc1_table", 3.0f);
            settle (200);
            std::vector<juce::TextButton*> buttons;
            findAll<juce::TextButton> (*editor, buttons);
            juce::TextButton* editButton = nullptr;
            for (auto* button : buttons)
                if (button->getButtonText().startsWith ("EDIT TABLE") && visibleInTree (button) && editButton == nullptr)
                    editButton = button;
            expect (editButton != nullptr, "the OSC page has an EDIT button for the wavetable");
            if (editButton != nullptr)
            {
                editButton->triggerClick();
                settle (300);
                auto* host = dynamic_cast<IlanaSynthAudioProcessorEditor*> (editor.get());
                auto* tableEditor = host != nullptr ? host->getWavetableEditor() : nullptr;
                const auto choice = juce::roundToInt (processor.apvts.getRawParameterValue ("osc1_table")->load());
                expect (tableEditor != nullptr && tableEditor->isVisible() && choice >= factoryCount
                            && processor.isUserSlotEdited (choice - factoryCount),
                        "EDIT on a factory table opens the editor on a patch table the oscillator now plays");
                if (tableEditor != nullptr)
                {
                    const auto slot = tableEditor->getSlot();
                    const auto before = processor.getUserTableDoc (slot).frames[0];
                    tableEditor->selectFrame (0);
                    tableEditor->drawLine (0.0f, 1.0f, 0.5f, -1.0f);
                    auto doc = processor.getUserTableDoc (slot);
                    expect (doc.recipes[0].kind == FrameRecipe::Kind::Draw && doc.frames[0] != before,
                            "drawing in the editor changes the table's first frame");
                    tableEditor->setHarmonic (3, 0.8f);
                    doc = processor.getUserTableDoc (slot);
                    expect (doc.recipes[0].kind == FrameRecipe::Kind::Harmonics && doc.recipes[0].magnitudes.size() >= 3
                                && std::abs (doc.recipes[0].magnitudes[2] - 0.8f) < 1.0e-6f,
                            "a SPECTRUM bar sets that harmonic");
                    expect (tableEditor->applyFormulaText ("sin(2*pi*x) * (1 - f)", true)
                                && processor.getUserTableDoc (slot).recipes.back().kind == FrameRecipe::Kind::Formula,
                            "a formula applies to every frame");
                    expect (! tableEditor->applyFormulaText ("sin(", false), "a broken formula is refused");
                    tableEditor->morphTable (16, true);
                    expect (processor.getUserTableDoc (slot).getNumFrames() == 16
                                && processor.getWavetable (factoryCount + slot)->getNumFrames() == 16,
                            "MORPH rebuilds the table at 16 frames and the oscillator plays it");
                    // SPECTRUM keeps its scale through a drag: holding the
                    // loudest (here the only) bar at -30 dB leaves it at -30 dB,
                    // where it used to sink further on every mouse move; a fast
                    // sweep sets every bar it passes.
                    tableEditor->applyFormulaText ("sin(2*pi*x)", false);
                    std::vector<juce::TextButton*> modeButtons;
                    findAll<juce::TextButton> (*tableEditor, modeButtons);
                    for (auto* button : modeButtons)
                        if (button->getButtonText() == "SPECTRUM")
                            button->triggerClick();
                    settle (50);
                    {
                        auto& canvas = tableEditor->getCanvas();
                        auto bars = canvas.getLocalBounds().toFloat().reduced (10.0f);
                        bars.removeFromBottom (bars.getHeight() * 0.22f);
                        bars.removeFromBottom (22.0f);
                        const auto barAt = [&] (int harmonic, float t)
                        {
                            return juce::Point<float> (bars.getX() + ((float) harmonic - 0.5f) * bars.getWidth() / 64.0f,
                                                       bars.getBottom() - t * bars.getHeight());
                        };
                        const auto press = barAt (1, 0.5f);
                        canvas.mouseDown (OscSlotUi::mouse (canvas, press, true, press));
                        for (int i = 0; i < 6; ++i)
                            canvas.mouseDrag (OscSlotUi::mouse (canvas, press, true, press));
                        canvas.mouseUp (OscSlotUi::mouse (canvas, press, false, press));
                        tableEditor->commitNow();
                        auto held = processor.getUserTableDoc (slot).recipes[0];
                        const auto level = held.magnitudes.empty() ? 0.0f : held.magnitudes[0];
                        expect (held.kind == FrameRecipe::Kind::Harmonics
                                    && std::abs (juce::Decibels::gainToDecibels (level) + 30.0f) < 1.5f,
                                "a SPECTRUM bar held at half height stays at -30 dB through the drag (was "
                                    + juce::String (juce::Decibels::gainToDecibels (level), 1) + " dB)");

                        const auto from = barAt (2, 0.98f), to = barAt (12, 0.98f);
                        canvas.mouseDown (OscSlotUi::mouse (canvas, from, true, from));
                        canvas.mouseDrag (OscSlotUi::mouse (canvas, to, true, from));
                        canvas.mouseUp (OscSlotUi::mouse (canvas, to, false, from));
                        tableEditor->commitNow();
                        held = processor.getUserTableDoc (slot).recipes[0];
                        auto filled = held.magnitudes.size() >= 12;
                        for (int k = 1; filled && k < 12; ++k)
                            filled = held.magnitudes[(size_t) k] > 0.5f;
                        expect (filled, "a fast SPECTRUM sweep sets every bar between its two ends");
                    }
                    tableEditor->applyShape (1);
                    save (*tableEditor, juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ilana-wt-editor.png"));
                    host->closeWavetableEditor();
                    settle (100);
                    expect (host->getWavetableEditor() == nullptr, "CLOSE takes the editor away");
                }
            }
            set ("osc1_table", 7.0f);
            settle (200);
        }

        // ENV: DAHDSR and key-rate knobs.
        pages->showPage ("ENV/LFO");
        settle (300);
        expect (visibleKnob ("amp_delay") && visibleKnob ("amp_hold") && visibleKnob ("amp_keyrate"),
                "the amp envelope shows DELAY, HOLD and KEY RATE");

        // The destination menu files the M5/M6 targets with their oscillator
        // and under FM, not under Physical & Keys.
        {
            juce::ComboBox combo;
            MatrixMenus::fillDestinations (combo);
            const auto submenuHolding = [&combo] (const juce::String& itemText)
            {
                for (juce::PopupMenu::MenuItemIterator top (*combo.getRootMenu()); top.next();)
                    if (auto* sub = top.getItem().subMenu.get())
                        for (juce::PopupMenu::MenuItemIterator inner (*sub); inner.next();)
                            if (inner.getItem().text == itemText)
                                return top.getItem().text;
                return juce::String();
            };
            // (The menu shows display names; look them up from the saved ones.)
            const auto shown = [] (const juce::String& savedName)
            {
                return ModNames::destination (Mod::getDestinationNames().indexOf (savedName));
            };
            expect (submenuHolding (shown ("Osc2 Warp 2")) == "OSC 2" && submenuHolding (shown ("FM Noise > Osc3")) == "FM"
                        && submenuHolding (shown ("FM Osc4 > Osc1")) == "FM" && submenuHolding (shown ("Osc1 Hammer")) == "Physical & Keys",
                    "the destination menu files new targets by oscillator and FM ("
                        + shown ("Osc2 Warp 2") + ": " + submenuHolding (shown ("Osc2 Warp 2")) + ", "
                        + shown ("FM Noise > Osc3") + ": " + submenuHolding (shown ("FM Noise > Osc3")) + ", "
                        + shown ("Osc1 Hammer") + ": " + submenuHolding (shown ("Osc1 Hammer")) + ")");
        }

        // MATRIX: a routing in slot 60 shows up as a row.
        set ("mod60_src", (float) Mod::Source::Lfo3);
        set ("mod60_dst", (float) Mod::Destination::Filter1Cutoff);
        set ("mod60_amt", 0.3f);
        pages->showPage ("MATRIX");
        settle (400);
        std::vector<MatrixRow*> matrixRows;
        findAll<MatrixRow> (*editor, matrixRows);
        auto slot60 = false;
        for (auto* row : matrixRows)
            slot60 = slot60 || (row->isVisible() && row->getSlotIndex() == 59);
        expect (matrixRows.size() == (size_t) Mod::maxSlots && slot60, "the matrix page has 64 rows and shows slot 60");
    }

    // Header: next steps to the following preset and the display follows.
    std::vector<IconButton*> buttons;
    findAll<IconButton> (*editor, buttons);

    for (auto* button : buttons)
    {
        if (button->getName() == "next")
        {
            button->triggerClick();
            settle (400);
            // (the arrows walk the browser's list, which the click made)
            juce::String following;
            if (auto* panel = findChild<PresetPanel> (*editor))
            {
                const auto listed = panel->getListedNames();
                following = listed[(listed.indexOf (names[neuroWobble]) + 1) % juce::jmax (1, listed.size())];
            }
            expect (following.isNotEmpty() && processor.getCurrentPresetName() == following,
                    "next-preset button loads the browser's following preset ('" + processor.getCurrentPresetName() + "', expected '"
                        + following + "')");
        }
    }

    // UI review 4 (V1, S1): drawn data (LFO curves, remaps, clips) is undone
    // one gesture at a time, EDITED sees it, and a load over an edited patch
    // asks once.
    {
        const auto event = [] (juce::Component& component, juce::Point<float> position, bool dragged, int clicks = 1,
                               juce::Point<float> down = { -1.0e9f, -1.0e9f })
        {
            if (down.x < -1.0e8f)
                down = position;
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys(),
                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component, juce::Time::getCurrentTime(),
                                     down, juce::Time::getCurrentTime(), clicks, dragged);
        };
        const auto gesture = [&event] (juce::Component& component, juce::Point<float> from, juce::Point<float> to, int clicks = 1)
        {
            component.mouseDown (event (component, from, false, clicks));
            component.mouseDrag (event (component, to, true, clicks, from));
            component.mouseUp (event (component, to, true, clicks, from));
        };
        const auto sameCurve = [] (const LfoCurve& a, const LfoCurve& b) { return a.toString() == b.toString(); };

        // An LFO curve stroke.
        {
            if (auto* shape = processor.apvts.getParameter ("lfo1_shape"))
                shape->setValueNotifyingHost (shape->convertTo0to1 ((float) IlanaSynthAudioProcessor::curveShape));

            LfoDisplay display (processor, 0);
            display.setSize (300, 150);
            const auto before = processor.getLfoCurve (0);
            clearHistory();
            gesture (display, { 100.0f, 50.0f }, { 120.0f, 40.0f });
            settle (50);
            const auto drawn = processor.getLfoCurve (0);
            const auto steps = undoSteps();
            processor.getUndoManager().undo();
            const auto undone = processor.getLfoCurve (0);
            processor.getUndoManager().redo();
            expect (! sameCurve (before, drawn) && steps.size() == 1 && steps[0] == "LFO 1 curve" && sameCurve (undone, before)
                        && sameCurve (processor.getLfoCurve (0), drawn),
                    "an LFO curve stroke is one undo step ('" + steps.joinIntoString ("', '") + "'); undo and redo restore the curve");
        }

        // A remap curve drag.
        {
            RemapEditor remap (processor, 5, juce::Colours::orange);
            clearHistory();
            gesture (remap, { 150.0f, 60.0f }, { 160.0f, 50.0f });
            const auto drawn = processor.isModRemapOn (5);
            const auto steps = undoSteps();
            processor.getUndoManager().undo();
            expect (drawn && steps.size() == 1 && steps[0] == "Remap curve 6" && ! processor.isModRemapOn (5),
                    "a remap curve drag is one undo step and undo straightens it again");
        }

        // A clip note: one step, and EDITED follows it (no parameter changes).
        auto* confirm = findChild<ConfirmOverlay> (*editor);
        IconButton* nextButton = nullptr;
        for (auto* button : buttons)
            if (button->getName() == "next")
                nextButton = button;

        if (confirm != nullptr && nextButton != nullptr)
        {
            // Start from a freshly loaded preset.
            nextButton->triggerClick();
            settle (300);
            const auto loadedName = processor.getCurrentPresetName();
            expect (! pages->isPatchEdited() && ! processor.getClipState().hasAny(), "a freshly loaded preset is not EDITED and has no clips");

            ClipEditor clips (processor, juce::Colours::orange);
            clips.setSize (400, 200);
            clips.setDrawMode (false); // (DRAW, on by default, places a note with one click)
            clearHistory();
            gesture (clips, { 80.0f, 100.0f }, { 80.0f, 100.0f }, 2); // a double-click adds a note
            settle (50);
            const auto steps = undoSteps();
            const auto edited = pages->isPatchEdited();
            processor.getUndoManager().undo();
            const auto undone = ! processor.getClipState().hasAny() && ! pages->isPatchEdited();
            processor.getUndoManager().redo();
            expect (steps.size() == 1 && steps[0] == "Add clip note" && undone && processor.getClipState().hasAny(),
                    "adding a clip note is one undo step ('" + steps.joinIntoString ("', '") + "'); undo and redo bring it back");
            expect (edited && pages->isPatchEdited(), "a clip edit lights EDITED, and undoing it clears it");

            // A load over the edit asks; Cancel keeps the patch.
            pages->setAsksBeforeReplacingEdits (true);
            nextButton->triggerClick();
            settle (100);
            expect (confirm->isAsking() && processor.getCurrentPresetName() == loadedName,
                    "the next-preset button asks before replacing an edited patch");
            // UI review 6: one dirty state. If the dialog asks, the header
            // says EDITED; and it offers to save first.
            expect (pages->isEditedBadgeShown(), "when the dialog asks, the header's EDITED badge shows");
            expect (confirm->hasAlternative() && confirm->getAlternativeText() == "SAVE AS... AND LOAD",
                    "the dialog offers Save as and load on a factory preset (" + confirm->getAlternativeText() + ")");

            // Save and load on a factory preset opens Save As; cancelling
            // it keeps the edited patch.
            if (auto* saveOverlay = findChild<SavePresetOverlay> (*editor))
            {
                confirm->chooseAlternative();
                settle (200);
                expect (saveOverlay->isShowing() && processor.getCurrentPresetName() == loadedName,
                        "Save and load opens SAVE AS first");
                saveOverlay->cancel();
                settle (200);
                expect (! saveOverlay->isShowing() && processor.getCurrentPresetName() == loadedName && pages->isPatchEdited(),
                        "cancelling that SAVE AS keeps the edited patch");
                nextButton->triggerClick();
                settle (100);
            }

            confirm->finish (false);
            settle (100);
            expect (processor.getCurrentPresetName() == loadedName && pages->isPatchEdited() && processor.getClipState().hasAny(),
                    "Cancel keeps the edited patch");

            // Load anyway loads it, and EDITED clears.
            nextButton->triggerClick();
            settle (100);
            confirm->finish (true);
            settle (300);
            const auto landed = processor.getCurrentPresetName();
            expect (landed != loadedName && ! pages->isPatchEdited() && ! processor.getClipState().hasAny(),
                    "Load anyway loads the next preset and EDITED clears ('" + landed + "')");

            // Stepping on from an unedited patch doesn't ask again.
            nextButton->triggerClick();
            settle (300);
            expect (! confirm->isAsking() && processor.getCurrentPresetName() != landed,
                    "stepping on from the loaded preset doesn't ask again");

            // Undoing the load brings the clip back with the old patch.
            processor.getUndoManager().undo();
            processor.getUndoManager().undo();
            settle (100);
            expect (processor.getClipState().hasAny(), "undoing a load restores the clips it replaced");

            // UI review 7 (S7-32): the dialog is as tall as what it says,
            // and while the browser is open one "Load anyway" covers the
            // rest of that browsing session.
            if (auto* display = findChild<PresetDisplay> (*editor); display != nullptr && display->onClick != nullptr)
            {
                auto* cutoff = processor.apvts.getParameter ("f1_cutoff");
                const auto edit = [&] { cutoff->setValueNotifyingHost (cutoff->getValue() > 0.5f ? 0.3f : 0.7f); settle (50); };
                display->onClick();
                settle (400);
                auto* panel = findChild<PresetPanel> (*editor);
                edit();
                nextButton->triggerClick();
                settle (100);
                const auto asked = confirm->isAsking();
                const auto tight = confirm->panelBounds().getHeight();
                confirm->finish (true);
                settle (200);
                const auto first = processor.getCurrentPresetName();
                edit();
                nextButton->triggerClick();
                settle (200);
                const auto askedAgain = confirm->isAsking();
                expect (asked && ! askedAgain && processor.getCurrentPresetName() != first,
                        "while browsing, one Load anyway covers the session (asked " + juce::String (asked ? "once" : "never") + ")");
                expect (tight > 100 && tight <= 160, "the confirm is as tall as its text (" + juce::String (tight) + " px)");
                if (askedAgain)
                    confirm->finish (false);
                if (panel != nullptr)
                    panel->close();
                settle (400);
                edit();
                nextButton->triggerClick();
                settle (100);
                expect (confirm->isAsking(), "after the browser closes, a load over an edit asks again");
                confirm->finish (false);
                settle (100);
            }

            pages->setAsksBeforeReplacingEdits (false);
            processor.loadFactoryPreset (0);
            settle (100);
        }
        else
        {
            expect (false, "the editor has the confirm overlay and the next button");
        }
    }

    // UI review 4 (S9): the clip piano roll's grid menu, velocity lane,
    // selection and copy / paste / duplicate / nudge keys, each edit one
    // undo step. A 400 x 200 roll on a 2-bar clip, reached through its own
    // geometry (beat to x, note to y, velocity to y).
    {
        const auto event = [] (juce::Component& component, juce::Point<float> position, bool dragged, int clicks,
                               juce::ModifierKeys mods)
        {
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), position, mods,
                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component, juce::Time::getCurrentTime(),
                                     position, juce::Time::getCurrentTime(), clicks, dragged);
        };
        const auto gesture = [&event] (juce::Component& component, juce::Point<float> from, juce::Point<float> to,
                                       int clicks = 1, juce::ModifierKeys mods = {})
        {
            component.mouseDown (event (component, from, false, clicks, mods));
            component.mouseDrag (event (component, to, true, clicks, mods));
            component.mouseUp (event (component, to, true, clicks, mods));
        };
        const auto command = [] (char letter) { return juce::KeyPress (letter, juce::ModifierKeys::commandModifier, 0); };
        const auto starts = [] (const Clip& clip)
        {
            juce::StringArray list;
            for (const auto& n : clip.notes)
                list.add (juce::String (n.start, 3) + "/" + juce::String (n.note) + "/" + juce::String (n.velocity));
            return list.joinIntoString (" ");
        };

        processor.loadFactoryPreset (0);
        settle (100);
        ClipEditor roll (processor, juce::Colours::orange);
        roll.setSize (400, 200);
        // Review 8 (S8 speed table): a new roll opens in DRAW. The selection
        // tests below run with it off.
        expect (roll.isDrawMode(), "the clip roll opens with DRAW on");
        roll.setDrawMode (false);
        const auto x = [&roll] (float beat) { return roll.xForBeat (beat); };
        const auto gridY = [&roll] (float fraction) { return roll.getGridArea().getY() + roll.getGridArea().getHeight() * fraction; };

        // GRID: 1/32 puts a double-clicked note on 32nds (1/16 would give
        // 2.5 here); the double-click has no drag, so the note keeps its
        // length (the drag-to-lengthen gesture is tested in the step 22 block).
        roll.setGrid (3);
        clearHistory();
        gesture (roll, { x (2.64f), 100.0f }, { x (2.64f), 100.0f }, 2);
        auto steps = undoSteps();
        auto shown = processor.getClipState().getClip (0);
        expect (roll.getGrid() == 3 && shown.notes.size() == 1 && std::abs (shown.notes[0].start - 2.625f) < 1.0e-4f
                    && shown.notes[0].velocity == 100 && steps.size() == 1,
                "the clip grid set to 1/32 snaps a double-clicked note to 32nds, in one undo step (" + starts (shown) + ")");
        expect (std::abs (ClipEditor::gridBeats (5) - 1.0f / 3.0f) < 1.0e-5f && std::abs (ClipEditor::gridBeats (3) - 0.125f) < 1.0e-6f
                    && ClipEditor::numGrids == 8 && ClipEditor::defaultGrid == 2,
                "the clip grid has 1/4 to 1/32 straight and triplet, 1/16 by default");

        // Velocity: a drag in the lane sets the bar under it; new notes take it.
        clearHistory();
        gesture (roll, { x (2.625f) + 1.0f, roll.yForVelocity (30) }, { x (2.625f) + 1.0f, roll.yForVelocity (118) });
        steps = undoSteps();
        const auto velocity = processor.getClipState().getClip (0).notes[0].velocity;
        processor.getUndoManager().undo();
        const auto undoneVelocity = processor.getClipState().getClip (0).notes[0].velocity;
        processor.getUndoManager().redo();
        roll.reload (false);
        expect (velocity > 110 && velocity < 127 && steps.size() == 1 && steps[0] == "Clip velocity" && undoneVelocity == 100,
                "dragging a clip note's velocity bar sets it (" + juce::String (velocity) + ") in one undo step; undo puts back 100");
        gesture (roll, { x (5.1f), 100.0f }, { x (5.1f), 100.0f }, 2);
        shown = processor.getClipState().getClip (0);
        expect (shown.notes.size() == 2 && shown.notes[1].velocity == velocity && std::abs (shown.notes[1].start - 5.0f) < 1.0e-4f,
                "a note added after a velocity drag takes that velocity (" + starts (shown) + ")");

        // A single click on empty space no longer writes a note.
        clearHistory();
        gesture (roll, { x (6.6f), 60.0f }, { x (6.6f), 60.0f });
        expect (processor.getClipState().getClip (0).notes.size() == 2 && undoSteps().isEmpty() && roll.getNumSelected() == 0,
                "a single click on an empty clip row writes nothing and clears the selection");

        // Rubber band: notes on beats 0-3 (60, 62, 64, 65); a band over
        // beats 1.2-2.5 picks the second and third; Delete removes them.
        {
            Clip four;
            four.bars = 2;
            for (const auto& [start, note] : { std::pair<float, int> { 0.0f, 60 }, { 1.0f, 62 }, { 2.0f, 64 }, { 3.0f, 65 } })
                four.notes.push_back ({ start, 1.0f, note, 100 });
            processor.getClipState().setClip (0, four);
            processor.clipsEdited();
        }
        roll.setGrid (ClipEditor::defaultGrid);
        roll.reload (true);
        clearHistory();
        gesture (roll, { x (1.2f), gridY (0.05f) }, { x (2.5f), gridY (0.95f) });
        const auto banded = roll.getNumSelected();
        const auto bandSteps = undoSteps().size();
        roll.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
        steps = undoSteps();
        shown = processor.getClipState().getClip (0);
        expect (banded == 2 && bandSteps == 0 && shown.notes.size() == 2 && shown.notes[0].note == 60 && shown.notes[1].note == 65
                    && steps.size() == 1 && steps[0] == "Delete clip notes",
                "a rubber band over two clip notes selects them (no undo step) and Delete removes both in one step (" + starts (shown) + ")");
        processor.getUndoManager().undo();
        expect (processor.getClipState().getClip (0).notes.size() == 4, "undoing the delete brings both notes back");

        // A click picks one note, shift-click adds one; neither edits.
        roll.reload (false);
        const auto noteY = [&roll] (int note) { return roll.yForNote (note); };
        clearHistory();
        gesture (roll, { x (0.5f), noteY (60) }, { x (0.5f), noteY (60) });
        gesture (roll, { x (3.5f), noteY (65) }, { x (3.5f), noteY (65) }, 1, juce::ModifierKeys::shiftModifier);
        const auto shiftPicked = roll.getNumSelected();
        gesture (roll, { x (1.5f), noteY (62) }, { x (1.5f), noteY (62) });
        expect (shiftPicked == 2 && roll.getNumSelected() == 1 && undoSteps().isEmpty(),
                "clicking a clip note selects it, shift-click adds one, neither changes the clip");

        // Copy and paste after the selection; duplicate; each one step.
        clearHistory();
        roll.keyPressed (command ('a'));
        const auto all = roll.getNumSelected();
        roll.keyPressed (command ('c'));
        roll.keyPressed (command ('v'));
        steps = undoSteps();
        shown = processor.getClipState().getClip (0);
        const auto pasted = shown.notes.size() == 8 && std::abs (shown.notes[4].start - 4.0f) < 1.0e-4f
                         && std::abs (shown.notes[7].start - 7.0f) < 1.0e-4f;
        expect (all == 4 && pasted && steps.size() == 1 && steps[0] == "Paste clip notes" && roll.getNumSelected() == 4,
                "Ctrl+A, Ctrl+C, Ctrl+V pastes the four notes after them in one undo step, selected (" + starts (shown) + ")");
        processor.getUndoManager().undo();
        roll.reload (false);
        clearHistory();
        roll.keyPressed (command ('a'));
        roll.keyPressed (juce::KeyPress (juce::KeyPress::leftKey)); // already at the start: stays
        roll.keyPressed (command ('d'));
        steps = undoSteps();
        shown = processor.getClipState().getClip (0);
        expect (shown.notes.size() == 8 && std::abs (shown.notes[4].start - 4.0f) < 1.0e-4f && steps.size() == 1
                    && steps[0] == "Duplicate clip notes",
                "Ctrl+D duplicates the selection after itself in one undo step ('" + steps.joinIntoString ("', '") + "')");
        processor.getUndoManager().undo();
        expect (processor.getClipState().getClip (0).notes.size() == 4, "undoing the duplicate leaves the four notes");

        // Arrows nudge: up a semitone, shift+right a bar.
        roll.reload (false);
        clearHistory();
        roll.keyPressed (command ('a'));
        roll.keyPressed (juce::KeyPress (juce::KeyPress::upKey));
        roll.keyPressed (juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0));
        steps = undoSteps();
        shown = processor.getClipState().getClip (0);
        expect (shown.notes.size() == 4 && shown.notes[0].note == 61 && std::abs (shown.notes[0].start - 4.0f) < 1.0e-4f
                    && steps.size() == 2 && steps[0] == "Nudge clip notes",
                "up nudges the selected notes a semitone and shift+right a bar, a step each (" + starts (shown) + ")");

        // Ctrl+wheel zooms in time; the plain wheel still scrolls pitch.
        juce::MouseWheelDetails wheel {};
        wheel.deltaY = 0.5f;
        roll.mouseWheelMove (event (roll, { x (2.0f), 80.0f }, false, 0, juce::ModifierKeys::commandModifier), wheel);
        const auto zoomed = roll.getZoom();
        roll.mouseWheelMove (event (roll, { x (2.0f), 80.0f }, false, 0, {}), wheel);
        expect (zoomed > 1.1f && std::abs (roll.getZoom() - zoomed) < 1.0e-6f,
                "Ctrl+wheel zooms the clip roll in time, the wheel alone doesn't");

        // A touchpad's stream of small deltas scrolls as far as one mouse notch.
        {
            const auto before = roll.getLowNote();
            roll.mouseWheelMove (event (roll, { x (2.0f), 80.0f }, false, 0, {}), wheel);
            const auto notch = roll.getLowNote() - before;
            const auto start = roll.getLowNote();
            juce::MouseWheelDetails tiny {};
            tiny.deltaY = -0.025f;
            tiny.isSmooth = true;
            for (int i = 0; i < 20; ++i)
                roll.mouseWheelMove (event (roll, { x (2.0f), 80.0f }, false, 0, {}), tiny);
            const auto moved = roll.getLowNote() - start;
            expect (notch >= 2 && notch <= 4 && std::abs (moved + notch) <= 1,
                    "twenty tiny touchpad deltas scroll the clip roll as far as one mouse notch ("
                        + juce::String (moved) + " vs " + juce::String (notch) + ")");
        }
        // Review 6 (V5-22, S6-10): a keyboard column left of the grid and a
        // bar ruler over it; zoom buttons' calls; QUANTISE snaps starts to
        // the grid in one undo step (every note with none selected).
        roll.zoomToFit();
        roll.zoomBy (2.0f);
        const auto zoomedIn = roll.getZoom();
        roll.zoomToFit();
        expect (roll.getKeysArea().getWidth() >= 40.0f && roll.getKeysArea().getRight() <= roll.getGridArea().getX()
                    && roll.getRulerArea().getBottom() <= roll.getGridArea().getY() && roll.getRulerArea().getHeight() >= 16.0f,
                "the clip roll has a keyboard column left of the grid and a ruler over it");
        expect (std::abs (zoomedIn - 2.0f) < 1.0e-4f && roll.getZoom() == 1.0f,
                "the roll's zoom in doubles the zoom and FIT shows the whole clip again");
        {
            Clip loose;
            loose.bars = 2;
            loose.notes.push_back ({ 0.1f, 0.5f, 60, 100 });
            loose.notes.push_back ({ 1.37f, 0.5f, 62, 100 });
            processor.getClipState().setClip (0, loose);
            processor.clipsEdited();
        }
        roll.reload (true);
        clearHistory();
        const auto moved = roll.quantise();
        steps = undoSteps();
        shown = processor.getClipState().getClip (0);
        expect (moved && shown.notes.size() == 2 && std::abs (shown.notes[0].start) < 1.0e-4f && std::abs (shown.notes[1].start - 1.25f) < 1.0e-4f
                    && steps.size() == 1 && steps[0] == "Quantise clip notes" && ! roll.quantise(),
                "QUANTISE puts every note's start on the 1/16 grid in one undo step, and then has nothing to do (" + starts (shown) + ")");

        // Review 7 (S7-18): DRAW. A click on empty space places a note (no
        // double-click), a drag paints a run along the grid; each one undo
        // step. D toggles it.
        {
            Clip empty;
            empty.bars = 2;
            processor.getClipState().setClip (0, empty);
            processor.clipsEdited();
        }
        roll.reload (true);
        roll.setGrid (ClipEditor::defaultGrid);
        roll.keyPressed (juce::KeyPress ('d'));
        const auto drawOn = roll.isDrawMode();
        clearHistory();
        gesture (roll, { x (0.1f), roll.yForNote (60) }, { x (0.1f), roll.yForNote (60) });
        steps = undoSteps();
        shown = processor.getClipState().getClip (0);
        expect (drawOn && shown.notes.size() == 1 && std::abs (shown.notes[0].start) < 1.0e-4f && shown.notes[0].note == 60
                    && steps.size() == 1,
                "DRAW (the D key) places a note with a single click, in one undo step (" + starts (shown) + ")");
        clearHistory();
        gesture (roll, { x (2.1f), roll.yForNote (62) }, { x (5.5f), roll.yForNote (62) });
        steps = undoSteps();
        shown = processor.getClipState().getClip (0);
        expect (shown.notes.size() == 5 && steps.size() == 1 && steps[0] == "Draw clip notes",
                "a drag in DRAW paints a run of notes on beats 2-5 in one undo step (" + starts (shown) + ")");
        // Step 24: the spray lands where the pointer went. A single jump
        // across the grid gives each cell the pitch the pointer had where it
        // crossed it (not all the last one); a pointer above the roll keeps
        // the top row; a spray across notes that are already there doesn't
        // stack a second note on them.
        {
            const auto kept = processor.getClipState().getClip (0);
            Clip blank;
            blank.bars = 2;
            const auto freshRoll = [&] (const Clip& clip)
            {
                processor.getClipState().setClip (0, clip);
                processor.clipsEdited();
                roll.reload (true);
            };
            const auto sorted = [&] (Clip clip)
            {
                std::sort (clip.notes.begin(), clip.notes.end(), [] (const ClipNote& a, const ClipNote& b) { return a.start < b.start; });
                return clip;
            };

            freshRoll (blank);
            gesture (roll, { x (2.1f), roll.yForNote (58) }, { x (5.5f), roll.yForNote (64) });
            auto sprayed = sorted (processor.getClipState().getClip (0));
            auto rising = sprayed.notes.size() == 4 && sprayed.notes.front().note == 58;
            for (size_t i = 1; i < sprayed.notes.size(); ++i)
                rising = rising && sprayed.notes[i].note > sprayed.notes[i - 1].note && sprayed.notes[i].note <= 64;
            expect (rising && sprayed.notes.back().note >= 62,
                    "a spray dragged in one jump from note 58 up to 64 puts each cell on the pitch the pointer crossed it at (" + starts (sprayed) + ")");

            freshRoll (blank);
            gesture (roll, { x (1.1f), roll.getGridArea().getY() + 0.5f }, { x (3.5f), roll.getGridArea().getY() + 0.5f });
            const auto top = processor.getClipState().getClip (0).notes.front().note;
            freshRoll (blank);
            gesture (roll, { x (1.1f), roll.getGridArea().getY() + 0.5f }, { x (3.5f), roll.getGridArea().getY() - 80.0f });
            sprayed = processor.getClipState().getClip (0);
            auto kept_in_view = ! sprayed.notes.empty();
            for (const auto& n : sprayed.notes)
                kept_in_view = kept_in_view && n.note <= top;
            expect (kept_in_view, "a spray whose pointer leaves the roll at the top stays on the top row (" + starts (sprayed) + ")");

            Clip busy = blank;
            busy.notes.push_back ({ 3.0f, 0.25f, 60, 100 });
            busy.notes.push_back ({ 4.0f, 0.25f, 60, 100 });
            freshRoll (busy);
            gesture (roll, { x (1.1f), roll.yForNote (60) }, { x (6.5f), roll.yForNote (60) });
            sprayed = processor.getClipState().getClip (0);
            auto stacked = false;
            for (size_t i = 0; i < sprayed.notes.size(); ++i)
                for (size_t j = i + 1; j < sprayed.notes.size(); ++j)
                    stacked = stacked || (sprayed.notes[i].note == sprayed.notes[j].note
                                          && std::abs (sprayed.notes[i].start - sprayed.notes[j].start) < 1.0e-3f);
            expect (! stacked && sprayed.notes.size() > 2, "a spray across notes that are already there doesn't stack a second one on them (" + starts (sprayed) + ")");

            freshRoll (kept);
        }

        roll.keyPressed (juce::KeyPress ('d'));
        clearHistory();
        gesture (roll, { x (7.1f), roll.yForNote (64) }, { x (7.1f), roll.yForNote (64) });
        expect (! roll.isDrawMode() && processor.getClipState().getClip (0).notes.size() == 5 && undoSteps().isEmpty(),
                "with DRAW off a single click on empty space writes nothing again");

        // Step 22: a double-click on empty space adds a note; holding the
        // second click and dragging lengthens it; a plain double-click keeps
        // the length last used. DRAW still places one note per click. The
        // clip is restored afterwards, so the tests below see the same notes.
        {
            roll.reload (false);
            clearHistory();
            const auto saved = processor.getClipState().getClip (0);
            Clip blank;
            blank.bars = saved.bars;
            processor.getClipState().setClip (0, blank);
            processor.clipsEdited();
            roll.reload (true);
            clearHistory();
            gesture (roll, { x (6.0f), noteY (62) }, { x (7.5f), noteY (62) }, 2);
            const auto lengthened = processor.getClipState().getClip (0);
            const auto added = lengthened.notes.empty() ? ClipNote {} : lengthened.notes.back();
            const auto addedSteps = undoSteps();
            gesture (roll, { x (5.0f), noteY (62) }, { x (5.0f), noteY (62) }, 2);
            const auto plain = processor.getClipState().getClip (0);
            ClipNote plainNote {};
            for (const auto& n : plain.notes)
                if (std::abs (n.start - 5.0f) < 1.0e-4f)
                    plainNote = n;
            roll.setDrawMode (true);
            gesture (roll, { x (7.0f), noteY (60) }, { x (7.0f), noteY (60) });
            roll.setDrawMode (false);
            const auto drawn = processor.getClipState().getClip (0);
            ClipNote drawnNote {};
            for (const auto& n : drawn.notes)
                if (std::abs (n.start - 7.0f) < 1.0e-4f)
                    drawnNote = n;
            const auto allSteps = undoSteps();
            expect (lengthened.notes.size() == 1 && added.note == 62 && std::abs (added.start - 6.0f) < 1.0e-4f
                        && added.length > 1.5f && added.length < 2.5f && addedSteps.size() == 1,
                    "a double-click then drag adds a note at the click and lengthens it by the drag (" + starts (lengthened) + ")");
            expect (plain.notes.size() == 2 && plainNote.note == 62 && std::abs (plainNote.start - 5.0f) < 1.0e-4f
                        && std::abs (plainNote.length - added.length) < 1.0e-6f,
                    "a plain double-click adds a note at the length last used (" + starts (plain) + " lengthened " + juce::String (added.length) + ")");
            expect (drawn.notes.size() == 3 && drawnNote.note == 60 && std::abs (drawnNote.start - 7.0f) < 1.0e-4f
                        && allSteps.size() == 3 && allSteps[0] == "Draw clip notes" && allSteps[1] == "Add clip note" && allSteps[2] == "Add clip note",
                    "DRAW places one note per click, one undo step, and the two double-click edits stay separate steps (" + starts (drawn) + ")");
            processor.getClipState().setClip (0, saved);
            processor.clipsEdited();
            roll.reload (false);
            clearHistory();
        }

        // Review 7 (S7-17): the roll opens fitted to the clip's notes, counts
        // the notes scrolled out of view at its edges, and a click on a
        // count brings them back.
        {
            Clip wide;
            wide.bars = 2;
            for (const auto note : { 55, 57, 60, 62, 64, 67 })
                wide.notes.push_back ({ (float) (note % 8), 0.5f, note, 100 });
            processor.getClipState().setClip (0, wide);
            processor.clipsEdited();
        }
        roll.reload (true);
        const auto fitted = roll.notesOutOfView();
        juce::MouseWheelDetails scroll {};
        scroll.deltaY = 1.0f;
        for (int i = 0; i < 12; ++i)
            roll.mouseWheelMove (event (roll, { x (2.0f), 80.0f }, false, 0, {}), scroll);
        const auto scrolled = roll.notesOutOfView();
        const auto low = roll.getLowestShownNote();
        gesture (roll, { roll.getGridArea().getRight() - 26.0f, roll.getGridArea().getBottom() - 12.0f },
                 { roll.getGridArea().getRight() - 26.0f, roll.getGridArea().getBottom() - 12.0f });
        expect (fitted.first == 0 && fitted.second == 0, "the clip roll opens with every note of the clip in view");
        expect (scrolled.second > 0 && roll.getLowestShownNote() < low && roll.notesOutOfView().second < scrolled.second,
                "scrolled up, the roll counts the notes below the view (" + juce::String (scrolled.second)
                    + "), and a click on that count scrolls down to them");

        processor.loadFactoryPreset (0);
        settle (100);
    }

    // Init puts the modules back to two oscillators and three envelopes and LFOs, and forgets the old patch's
    // macro CCs and drawn LFO shapes.
    {
        using M = IlanaSynthAudioProcessor::Module;
        processor.addOscillator (4);
        processor.setRevealed (M::Lfo, 8, true);
        processor.setRevealed (M::Envelope, 9, true);
        processor.setLfoCustomPoint (0, 3, 0.9f);
        processor.loadFactoryPreset (0);
        settle (100);
        expect (! processor.isOscillatorShown (4) && ! processor.isLfoShown (8)
                    && ! processor.isRevealed (M::Envelope, 9) && processor.isOscillatorShown (1) && ! processor.isOscillatorShown (2),
                "Init shows two oscillators and three LFOs and envelopes again");
        expect (std::abs (processor.getLfoCustomPoint (0, 3) - 0.9f) > 0.1f && processor.getMacroCc (0) == 20,
                "Init resets drawn LFO shapes and macro CCs");
    }

    // M7.0: on Init, a matrix starter adds its routing and an FX quick-add
    // fills the first slot.
    {
        processor.loadFactoryPreset (0);
        pages->showPage ("MAIN");
        settle (100);
        // (Init carries the two cutoff routes that replaced its ENV AMOUNT knob; the starters want an empty matrix.)
        for (int slot = 0; slot < Mod::maxSlots; ++slot)
            processor.clearModSlot (slot);
        pages->showPage ("MATRIX");
        settle (300);

        const auto findButton = [&editor] (const juce::String& text) -> juce::TextButton*
        {
            std::vector<juce::TextButton*> buttons;
            findAll<juce::TextButton> (*editor, buttons);
            for (auto* button : buttons)
                if (button->getButtonText() == text && button->isVisible())
                    return button;
            return nullptr;
        };

        auto* starter = findButton ("MOD WHEEL  >  Vibrato");
        expect (starter != nullptr, "an empty matrix shows the starter routings");

        if (starter != nullptr)
        {
            starter->triggerClick();
            settle (300);
            const auto slot = processor.readModSlot (0);
            expect (slot.source == Mod::Source::Lfo2 && slot.aux == Mod::Source::ModWheel
                        && slot.destination == (int) Mod::Destination::Osc1Pitch
                        && processor.readModSlot (2).destination == (int) Mod::Destination::SubPitch,
                    "WHEEL > VIBRATO routes LFO 2 via the wheel to OSC 1-3 pitch");
            expect (findButton ("MOD WHEEL  >  Vibrato") == nullptr, "the starters hide once something is routed");
        }

        pages->showPage ("FX");
        settle (300);
        auto* tapeStop = findButton ("TAPE STOP");
        expect (tapeStop != nullptr, "the empty rack offers every effect, grouped");

        if (tapeStop != nullptr)
        {
            tapeStop->triggerClick();
            settle (300);
            expect ((int) processor.apvts.getRawParameterValue ("fx_slot1")->load() == 17, "a quick-add button fills slot 1");
        }

        processor.loadFactoryPreset (0);
        settle (200);
    }

    // M7.1: the Generative card switches between ARP, EUCLID and PROB SEQ.
    {
        pages->showPage ("ARP/SEQ");
        settle (100);
        std::vector<CardTabs*> cardTabs;
        findAll<CardTabs> (*editor, cardTabs);
        CardTabs* engineTabs = nullptr;
        for (auto* candidate : cardTabs)
            if (candidate->getNames().contains ("PROB SEQ"))
                engineTabs = candidate;

        expect (engineTabs != nullptr, "the ARP/SEQ page has a Generative card with ARP, EUCLID and PROB SEQ");

        if (engineTabs != nullptr)
        {
            pages->showPage ("ARP/SEQ");
            engineTabs->setSelected (1, true);
            settle (200);

            const auto visibleWith = [&editor] (const juce::String& id)
            {
                std::vector<KnobControl*> knobs;
                findAll<KnobControl> (*editor, knobs);
                for (auto* knob : knobs)
                    if (knob->getParameterId() == id)
                        return knob->isVisible();
                return false;
            };

            expect (visibleWith ("euc_hits") && ! visibleWith ("arp_gate") && ! visibleWith ("pseq_length"),
                    "the EUCLID tab shows only Euclid's controls");
            engineTabs->setSelected (2, true);
            settle (200);
            expect (visibleWith ("pseq_length") && ! visibleWith ("euc_hits"), "the PROB SEQ tab shows the sequencer");
            expect (visibleWith ("spray_strum_time"), "the GENERATE card has STRUM TIME");
            engineTabs->setSelected (0, true);

            // Review 6 (I6-15): a tab lights while its engine is on, and the
            // note path says PROB SEQ plays instead of the ARP.
            const auto set = [&processor] (const juce::String& id, float value)
            {
                if (auto* parameter = processor.apvts.getParameter (id))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
            };
            set ("arp_on", 1.0f);
            set ("pseq_on", 1.0f);
            settle (400);
            auto* chain = findChild<NoteChainView> (*editor);
            juce::String note;
            const auto stages = chain != nullptr ? chain->stages (note) : std::vector<NoteChainView::Stage> {};
            auto arpWaits = false, seqPlays = false;
            for (const auto& stage : stages)
            {
                arpWaits = arpWaits || (stage.name == "ARP" && stage.waiting);
                seqPlays = seqPlays || (stage.name == "PROB SEQ" && ! stage.waiting);
            }
            expect (engineTabs->isTabOn (0) && engineTabs->isTabOn (2) && ! engineTabs->isTabOn (1) && ! engineTabs->isTabOn (3),
                    "the ARP and PROB SEQ tabs light while they are on, EUCLID's and CLIP's don't");
            expect (chain != nullptr && visibleInTree (chain) && arpWaits && seqPlays && note == "PROB SEQ replaces the ARP.",
                    "the note path shows PROB SEQ playing and the ARP waiting ('" + note + "')");

            // S12-4: the chain lists the engines in the tab order, and each opens its tab.
            {
                set ("euc_on", 1.0f);
                set ("euc_target", 0.0f);
                set ("clip_on", 1.0f);
                settle (300);
                juce::String orderNote;
                auto last = -1, opens = 0;
                auto ordered = true;
                for (const auto& stage : chain->stages (orderNote))
                    if (stage.engine >= 0)
                    {
                        ordered = ordered && stage.engine >= last;
                        last = stage.engine;
                        ++opens;
                    }
                expect (ordered && opens >= 3, "the note path lists ARP, EUCLID, PROB SEQ, CLIP in the tab order, each opening its tab (S12-4)");
                set ("euc_on", 0.0f);
                set ("clip_on", 0.0f);
                settle (200);
            }

            // The arp's settings step back while PROB SEQ plays instead.
            KnobControl* arpGate = nullptr;
            {
                std::vector<KnobControl*> knobs;
                findAll<KnobControl> (*editor, knobs);
                for (auto* knob : knobs)
                    if (knob->getParameterId() == "arp_gate")
                        arpGate = knob;
            }
            expect (arpGate != nullptr && arpGate->getAlpha() < 0.99f, "the ARP's GATE dims while PROB SEQ plays instead");
            set ("pseq_on", 0.0f);
            settle (300);
            expect (arpGate != nullptr && arpGate->getAlpha() > 0.99f && ! engineTabs->isTabOn (2), "and lights again once PROB SEQ is off");

            // Review 7 (V7-11, I7-36): each engine's power is the switch in
            // its tab; the engines' rows have no switch of their own. A click
            // on a tab's switch toggles that engine (one undo step) without
            // changing the tab shown; a click on its name shows it.
            {
                std::vector<ToggleControl*> toggles;
                findAll<ToggleControl> (*editor, toggles);
                juce::StringArray rowSwitches;
                for (auto* toggle : toggles)
                    for (const auto* id : { "arp_on", "euc_on", "pseq_on", "clip_on" })
                        if (visibleInTree (toggle) && toggle->getButton().getTooltip().startsWith (processor.apvts.getParameter (id)->getName (64)))
                            rowSwitches.add (id);
                expect (rowSwitches.isEmpty(), "the PATTERN engines have no switch in their rows (" + rowSwitches.joinIntoString (" ") + ")");

                const auto click = [] (juce::Component& component, juce::Point<float> position)
                {
                    const juce::MouseEvent down (juce::Desktop::getInstance().getMainMouseSource(), position, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                 &component, &component, juce::Time::getCurrentTime(), position, juce::Time::getCurrentTime(),
                                                 1, false);
                    component.mouseDown (down);
                    component.mouseUp (down);
                };
                engineTabs->setSelected (0, true);
                processor.getUndoManager().clearUndoHistory();
                click (*engineTabs, engineTabs->switchBounds (1).getCentre());
                settle (100);
                const auto steps = processor.getUndoManager().getUndoDescriptions();
                const auto euclidOn = processor.apvts.getRawParameterValue ("euc_on")->load() > 0.5f;
                expect (euclidOn && engineTabs->isTabOn (1) && engineTabs->getSelected() == 0 && steps.size() == 1
                            && steps[0] == "EUCLID on",
                        "the switch in EUCLID's tab turns EUCLID on in one undo step and leaves ARP shown ('"
                            + steps.joinIntoString ("', '") + "')");
                click (*engineTabs, engineTabs->switchBounds (1).getCentre());
                settle (100);
                expect (processor.apvts.getRawParameterValue ("euc_on")->load() < 0.5f && ! engineTabs->isTabOn (1),
                        "a second click on the switch turns EUCLID off");
                click (*engineTabs, { engineTabs->switchBounds (2).getRight() + 20.0f, engineTabs->switchBounds (2).getCentreY() });
                settle (100);
                expect (engineTabs->getSelected() == 2 && processor.apvts.getRawParameterValue ("pseq_on")->load() < 0.5f,
                        "a click on PROB SEQ's name shows it without switching it on");
                {
                    std::vector<KnobControl*> knobs;
                    findAll<KnobControl> (*editor, knobs);
                    juce::String label;
                    for (auto* knob : knobs)
                        if (knob->getParameterId() == "pseq_length")
                            label = knob->getLabelText();
                    expect (label == "STEPS", "PROB SEQ's step count is STEPS, as the arp's (I7-29: '" + label + "')");
                }

                // An engine shown while off says so in the header (no plate
                // over its display: S7-19).
                auto* chain = findChild<NoteChainView> (*editor);
                juce::String offNote;
                if (chain != nullptr)
                    chain->stages (offNote);
                expect (offNote.startsWith ("PROB SEQ is off"), "the PATTERN header says the shown engine is off ('" + offNote + "')");

                // S7-37: the note follows the chain when EUCLID drives the clip.
                set ("arp_on", 0.0f);
                set ("euc_on", 1.0f);
                set ("clip_on", 1.0f);
                engineTabs->setSelected (3, true);
                settle (200);
                juce::String clipNote;
                if (chain != nullptr)
                    chain->stages (clipNote);
                expect (clipNote == "EUCLID's hits trigger the clip.",
                        "with EUCLID and CLIP on the header says how they combine ('" + clipNote + "')");
                set ("euc_on", 0.0f);
                set ("clip_on", 0.0f);
                set ("arp_on", 1.0f);
                engineTabs->setSelected (0, true);
                settle (200);
            }

            // The arp's lanes (S6-23): a drag down VELOCITY over steps 3-5
            // draws them in one undo step; the defaults leave every step as
            // it was.
            if (auto* lanes = findChild<ArpLanesEditor> (*editor); lanes != nullptr && visibleInTree (lanes))
            {
                const auto event = [] (juce::Component& component, juce::Point<float> position, bool dragged)
                {
                    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), position, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                             &component, &component, juce::Time::getCurrentTime(), position, juce::Time::getCurrentTime(),
                                             1, dragged);
                };
                const auto lane = lanes->laneArea (ArpLanesEditor::velocity);
                const auto low = lane.getBottom() - lane.getHeight() * 0.25f;
                processor.apvts.copyState();
                processor.getUndoManager().clearUndoHistory();
                lanes->mouseDown (event (*lanes, { lanes->cellCentre (0, 2).x, low }, false));
                lanes->mouseDrag (event (*lanes, { lanes->cellCentre (0, 4).x, low }, true));
                lanes->mouseUp (event (*lanes, { lanes->cellCentre (0, 4).x, low }, true));
                processor.apvts.copyState();
                const auto steps = processor.getUndoManager().getUndoDescriptions();
                const auto read = [&processor] (const char* id) { return juce::roundToInt (processor.apvts.getRawParameterValue (id)->load()); };
                expect (read ("arp_vel3") < 50 && read ("arp_vel4") < 50 && read ("arp_vel5") < 50 && read ("arp_vel6") == 100
                            && read ("arp_vel2") == 100 && steps.size() == 1 && steps[0] == "Arp velocity",
                        "dragging across the arp's VELOCITY lane draws steps 3-5 (" + juce::String (read ("arp_vel4"))
                            + ") in one undo step ('" + steps.joinIntoString ("', '") + "')");
                lanes->mouseDoubleClick (event (*lanes, lanes->cellCentre (0, 3), false));
                expect (read ("arp_vel4") == 100, "a double-click puts an arp step back to its defaults");

                // A click on a step number sets how many steps loop.
                lanes->mouseDown (event (*lanes, lanes->cellCentre (-1, 11), false));
                lanes->mouseUp (event (*lanes, lanes->cellCentre (-1, 11), false));
                expect (read ("arp_steps") == 12, "clicking step 12's number loops 12 arp steps");
                // S7-19: a colour per lane.
                expect (lanes->laneColour (0) != lanes->laneColour (1) && lanes->laneColour (1) != lanes->laneColour (2)
                            && lanes->laneColour (0) != lanes->laneColour (2),
                        "the arp's VELOCITY, GATE and PITCH lanes each have their own colour");
                for (const auto* id : { "arp_steps", "arp_vel3", "arp_vel5" })
                    set (id, processor.apvts.getParameter (id)->convertFrom0to1 (processor.apvts.getParameter (id)->getDefaultValue()));
            }
            else
            {
                expect (false, "the ARP tab shows the arp's step lanes");
            }
            set ("arp_on", 0.0f);

            // EXPAND (S6-10): the clip roll takes the page, GENERATE folds to
            // its title line; COLLAPSE brings it back.
            engineTabs->setSelected (3, true);
            settle (200);
            juce::TextButton* expandButton = nullptr;
            {
                std::vector<juce::TextButton*> buttons;
                findAll<juce::TextButton> (*editor, buttons);
                for (auto* button : buttons)
                    if (button->getButtonText() == "EXPAND" && visibleInTree (button))
                        expandButton = button;
            }
            auto* roll = findChild<ClipEditor> (*editor);
            expect (expandButton != nullptr && roll != nullptr, "the CLIP tab has the roll and an EXPAND button");

            if (expandButton != nullptr && roll != nullptr)
            {
                const auto before = roll->getHeight();
                expandButton->triggerClick();
                settle (200);
                const auto expanded = roll->getHeight();
                const auto generateHidden = ! visibleWith ("spray_count") && ! visibleWith ("spray_strum_time");
                const auto collapseText = expandButton->getButtonText();
                expandButton->triggerClick();
                settle (200);
                expect (expanded > before + 80 && generateHidden && collapseText == "COLLAPSE",
                        "EXPAND gives the clip roll the page (" + juce::String (before) + " -> " + juce::String (expanded)
                            + " px) and folds GENERATE away");
                expect (roll->getHeight() == before && visibleWith ("spray_count") && expandButton->getButtonText() == "EXPAND",
                        "COLLAPSE brings GENERATE back");
            }

            engineTabs->setSelected (0, true);
        }
    }

    // M8.1: the LFO card for a simulated shape.
    {
        const auto knobFor = [&editor] (const juce::String& id) -> KnobControl*
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
                if (knob->getParameterId() == id)
                    return knob;
            return nullptr;
        };
        const auto setShape = [&processor] (int shape)
        {
            if (auto* parameter = processor.apvts.getParameter ("lfo1_shape"))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) shape));
        };
        pages->showPage ("ENV/LFO");
        if (auto* page = pages->getCurrentPage())
            if (auto* thumbs = findChild<LfoThumbBar> (*page); thumbs != nullptr && thumbs->onSelect != nullptr)
                thumbs->onSelect (0);
        setShape (LfoSimShapes::Lorenz);
        settle (300);
        // The LFO cards (PLAY and MOD) draw a simulated shape from the live
        // simulation, not one fixed picture: the card's pixels move between
        // two moments, and its dot rides the trace's right-hand end.
        if (auto* page = pages->getCurrentPage())
            if (auto* cards = findChild<LfoThumbBar> (*page); cards != nullptr && cards->isCardShown (0))
            {
                const auto area = cards->boundsOfCard (0).toNearestInt();
                const auto grab = [&] { return cards->createComponentSnapshot (area, false, 1.0f); };
                const auto before = grab();
                settle (500);
                const auto after = grab();
                auto changed = 0;
                for (int y = 0; y < before.getHeight() && y < after.getHeight(); ++y)
                    for (int x = 0; x < before.getWidth() && x < after.getWidth(); ++x)
                        changed += before.getPixelAt (x, y) != after.getPixelAt (x, y) ? 1 : 0;
                expect (changed > 20, "a simulated LFO's thumbnail moves with the live simulation (" + juce::String (changed) + " pixels changed in 0.5 s)");
            }
        auto* p1 = knobFor ("lfo1_p1");
        auto* p4 = knobFor ("lfo1_p4");
        auto* start = knobFor ("lfo1_phase");
        auto* smooth = knobFor ("lfo1_smooth");
        expect (p1 != nullptr && p1->isVisible() && p1->getLabelText() == "SIGMA", "Lorenz shows a SIGMA knob");
        expect (p1 != nullptr && p1->getSlider().getTextFromValue (0.5) == "10.00", "SIGMA reads 10.00 at the middle");
        expect (p4 != nullptr && ! p4->isVisible(), "Lorenz hides the knobs it doesn't use");
        expect (start != nullptr && ! start->isVisible() && smooth != nullptr && smooth->isVisible(),
                "simulated shapes show SMOOTH instead of START");
        std::vector<juce::TextButton*> buttons;
        findAll<juce::TextButton> (*editor, buttons);
        auto fireShown = false;
        for (auto* button : buttons)
            fireShown = fireShown || (button->getButtonText() == "FIRE" && button->isVisible());
        expect (fireShown, "the card has a FIRE button");

        setShape (LfoSimShapes::Bounce);
        settle (300);
        expect (p1 != nullptr && p1->getLabelText() == "GRAVITY" && p4 != nullptr && p4->isVisible() && p4->getLabelText() == "DRAG",
                "Bounce names its knobs GRAVITY ... DRAG");
        setShape (0);
        settle (300);
        expect (p1 != nullptr && ! p1->isVisible() && smooth != nullptr && smooth->isVisible() && start != nullptr && start->isVisible(),
                "classic shapes keep START and gain SMOOTH");
        expect (Mod::getSourceNames().contains ("LFO 16 B"), "every LFO's output B is a mod source");

        // UI review 8, I8-17 / S8-7 / V8-6: one panel grid for every shape.
        // SHAPE, SYNC, RETRIG, KEY, RATE and SMOOTH keep their places when
        // SHAPE changes; nothing in the panel overlaps or leaves it.
        {
            // (Combos and switches carry their parameter's name first in
            // their tooltips.)
            const auto findToggle = [&editor, &processor] (const juce::String& id) -> juce::Component*
            {
                auto* parameter = processor.apvts.getParameter (id);
                std::vector<ToggleControl*> toggles;
                findAll<ToggleControl> (*editor, toggles);
                for (auto* toggle : toggles)
                    if (parameter != nullptr && visibleInTree (toggle) && toggle->getWidth() > 0
                        && toggle->getButton().getTooltip().startsWith (parameter->getName (64)))
                        return toggle;
                return nullptr;
            };
            const auto findCombo = [&editor, &processor] (const juce::String& id) -> juce::Component*
            {
                auto* parameter = processor.apvts.getParameter (id);
                std::vector<ComboControl*> combos;
                findAll<ComboControl> (*editor, combos);
                for (auto* combo : combos)
                    if (parameter != nullptr && visibleInTree (combo) && combo->getWidth() > 0
                        && combo->getTooltip().startsWith (parameter->getName (64)))
                        return combo;
                return nullptr;
            };
            const auto placesOf = [&]
            {
                std::vector<juce::Rectangle<int>> places;
                for (auto* component : { findCombo ("lfo1_shape"), findToggle ("lfo1_sync"), findToggle ("lfo1_retrig"),
                                         findToggle ("lfo1_key"), static_cast<juce::Component*> (knobFor ("lfo1_rate")),
                                         static_cast<juce::Component*> (knobFor ("lfo1_smooth")) })
                    places.push_back (component != nullptr && visibleInTree (component) ? component->getBounds() : juce::Rectangle<int>());
                return places;
            };
            // The visible controls of LFO 1's panel: none overlap another.
            const auto overlaps = [&]
            {
                juce::String bad;
                std::vector<std::pair<juce::String, juce::Rectangle<int>>> boxes;
                for (const juce::String suffix : { "_shape", "_sync", "_retrig", "_key", "_loop", "_kick", "_trigger", "_axis", "_rate", "_div",
                                                   "_smooth", "_phase", "_phys_a", "_phys_b", "_stereo", "_seed", "_p1", "_p2", "_p3", "_p4", "_p5", "_p6" })
                {
                    const auto id = "lfo1" + suffix;
                    juce::Component* component = knobFor (id);
                    if (component == nullptr || ! visibleInTree (component))
                        component = findToggle (id);
                    if (component == nullptr)
                        component = findCombo (id);
                    if (component != nullptr && visibleInTree (component) && component->getWidth() > 0)
                        boxes.push_back ({ id, component->getBounds() });
                }
                for (size_t a = 0; a < boxes.size(); ++a)
                    for (size_t b = a + 1; b < boxes.size(); ++b)
                        if (boxes[a].second.intersects (boxes[b].second))
                            bad << boxes[a].first << " / " << boxes[b].first << " ";
                return bad;
            };
            juce::String moved, overlapping;
            setShape (0);
            settle (300);
            const auto reference = placesOf();
            auto allFound = true;
            for (const auto& place : reference)
                allFound = allFound && ! place.isEmpty();
            for (const auto shape : { (int) LfoShapes::Triangle, (int) LfoShapes::Draw, (int) LfoShapes::Curve, (int) LfoShapes::Pendulum,
                                      (int) LfoSimShapes::Lorenz, (int) LfoSimShapes::Pendulum, (int) LfoSimShapes::Bounce,
                                      (int) LfoSimShapes::DoublePendulum, (int) LfoSimShapes::Perlin, 0 })
            {
                setShape (shape);
                settle (250);
                if (placesOf() != reference)
                    moved << shape << " ";
                const auto bad = overlaps();
                if (bad.isNotEmpty())
                    overlapping << shape << ": " << bad;
            }
            expect (allFound && moved.isEmpty(), "SHAPE, SYNC, RETRIG, KEY, RATE and SMOOTH keep their places for every shape (moved on: "
                                                     + moved + ")");
            expect (overlapping.isEmpty(), "no two controls of the LFO panel overlap, for any shape (" + overlapping + ")");

            // I8-16: a simulated shape's caption follows TRIGGER.
            const auto setParam = [&processor] (const juce::String& id, float value)
            {
                if (auto* parameter = processor.apvts.getParameter (id))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
            };
            // I9-1 / I9-19: one trigger model. The switch says where it runs
            // (RETRIG on a plain shape, PER VOICE on a simulated one, whose
            // TRIGGER says when it restarts), and the caption says both in
            // the same words for every shape.
            const auto runSwitchText = [&]
            {
                auto* toggle = dynamic_cast<ToggleControl*> (findToggle ("lfo1_retrig"));
                return toggle != nullptr ? toggle->getButton().getButtonText() : juce::String();
            };
            const auto plainSwitch = runSwitchText();
            const auto plainCaption = LfoShapeMenu::runCaption (processor, 0);
            setShape (LfoSimShapes::Bounce);
            settle (150);
            const auto simSwitch = runSwitchText();
            setParam ("lfo1_trigger", 0.0f);
            const auto onNote = LfoShapeMenu::runCaption (processor, 0);
            setParam ("lfo1_trigger", 1.0f);
            const auto free = LfoShapeMenu::runCaption (processor, 0);
            setParam ("lfo1_trigger", 0.0f);
            setShape (0);
            expect (onNote.contains ("restarts on note") && free.contains ("runs free")
                        && onNote.startsWith ("shared") && plainCaption == juce::String::fromUTF8 ("shared \xc2\xb7 runs free")
                        && onNote.contains ("OUT 2") && onNote.length() < 48,
                    "an LFO's caption says where it runs and when it restarts, the same words for plain and simulated shapes ('"
                        + plainCaption + "' / '" + onNote + "' / '" + free + "')");
            expect (plainSwitch == "RETRIG" && simSwitch == "VOICE",
                    "the run switch reads RETRIG on a plain shape and VOICE on a simulated one, beside its TRIGGER ('" + plainSwitch
                        + "' / '" + simSwitch + "')");
            settle (200);
        }

        // I8-26: physics units read as units; I8-36: no two shapes share a
        // name in the SHAPE list.
        {
            const auto gravity = LfoSimInfo::text (LfoSimShapes::Bounce, 0, 0.5f), drag = LfoSimInfo::text (LfoSimShapes::Bounce, 3, 0.0f);
            expect (gravity.endsWith (juce::String::fromUTF8 ("m/s\xc2\xb2")) && drag.endsWith ("per s"),
                    "physics units read m/s squared and 'per s' (" + gravity + ", " + drag + ")");
            juce::StringArray shown;
            juce::String clash;
            auto* shapeParameter = dynamic_cast<juce::AudioParameterChoice*> (processor.apvts.getParameter ("lfo1_shape"));
            if (shapeParameter != nullptr)
                for (int shape = 0; shape < shapeParameter->choices.size(); ++shape)
                {
                    const auto name = LfoShapeMenu::displayName (shape, shapeParameter->choices[shape]);
                    if (shown.contains (name))
                        clash << name << " ";
                    shown.add (name);
                }
            expect (shapeParameter != nullptr && clash.isEmpty(), "every LFO shape has its own name in the SHAPE list (" + clash + ")");
        }
    }

    // UI review 8, I8-4 / S8-5 / V8-2: one MSEG. UI review 9, I9-2: an old
    // patch that routes a looping MSEG module loads with it drawn on an LFO
    // (SHAPE > MSEG, the same points and RATE) and its routes moved there, so
    // no MSEG card or chip shows. A route made to it later (or a one-shot
    // one) shows its card, in an LFO pastel, and MOVE TO LFO still moves it;
    // undo brings it back.
    {
        const auto msegSweep = names.indexOf ("MSEG Sweep");
        processor.loadFactoryPreset (msegSweep);
        pages->showPage ("ENV/LFO");
        settle (400);
        auto* page = pages->getCurrentPage();
        auto* lfoCards = page != nullptr ? findChild<LfoThumbBar> (*page) : nullptr;
        const auto msegCard = IlanaSynthAudioProcessor::numLfos;
        {
            auto target = -1, fromLfo = 0;
            for (int slot = 0; slot < Mod::maxSlots; ++slot)
            {
                const auto routing = processor.readModSlot (slot);
                if (routing.destination != 0 && Mod::lfoIndexFor (routing.source) >= 0 && target < 0
                    && juce::roundToInt (processor.apvts.getRawParameterValue ("lfo" + juce::String (Mod::lfoIndexFor (routing.source) + 1) + "_shape")->load())
                           == IlanaSynthAudioProcessor::curveShape)
                    target = Mod::lfoIndexFor (routing.source);
            }
            for (int slot = 0; target >= 0 && slot < Mod::maxSlots; ++slot)
            {
                const auto routing = processor.readModSlot (slot);
                fromLfo += routing.destination != 0 && routing.source == Mod::lfoSourceFor (target) ? 1 : 0;
            }
            const auto curve = target >= 0 ? processor.getLfoCurve (target) : LfoCurve();
            auto pointsMatch = curve.points.size() == 5;
            for (int i = 0; pointsMatch && i < 4; ++i)
                pointsMatch = std::abs (curve.points[(size_t) i].y - processor.apvts.getRawParameterValue ("mseg_level" + juce::String (i + 1))->load()) < 1.0e-4f;
            const auto rate = target >= 0 ? processor.apvts.getRawParameterValue ("lfo" + juce::String (target + 1) + "_rate")->load() : 0.0f;
            expect (msegSweep >= 0 && lfoCards != nullptr && ! lfoCards->isCardInPool (msegCard) && ! modSourceInPatch (processor, Mod::Source::Mseg)
                        && target >= 0 && fromLfo == 2 && pointsMatch && processor.isLfoShown (target)
                        && std::abs (rate - processor.apvts.getRawParameterValue ("mseg_rate")->load()) < 1.0e-3f,
                    "MSEG Sweep loads with its MSEG drawn on LFO " + juce::String (target + 1)
                        + " (SHAPE > MSEG, its points and RATE) and both routes there; no MSEG card or chip");
            expect (modSourceColour ((int) Mod::Source::Mseg) != juce::Colour (0xffe0e6f0) && modSourceColour ((int) Mod::Source::Mseg).getSaturation() > 0.1f,
                    "the MSEG source has an LFO pastel, not the white of a source without a family");
        }

        // A route made to the module afterwards: its card, and MOVE TO LFO.
        processor.setModSlotValue (40, "src", (float) (int) Mod::Source::Mseg);
        processor.setModSlotValue (40, "dst", 2.0f);
        processor.setModSlotValue (40, "amt", 0.5f);
        lfoCards = page != nullptr ? findChild<LfoThumbBar> (*page) : nullptr;
        if (lfoCards != nullptr)
            lfoCards->refreshLayout();
        settle (300);
        expect (lfoCards != nullptr && lfoCards->isCardInPool (msegCard) && modSourceInPatch (processor, Mod::Source::Mseg),
                "a patch that routes the MSEG module shows its card");
        if (lfoCards != nullptr && lfoCards->isCardInPool (msegCard))
        {
            lfoCards->onSelect (msegCard);
            settle (300);
            std::vector<juce::TextButton*> buttons;
            findAll<juce::TextButton> (*editor, buttons);
            juce::TextButton* move = nullptr;
            for (auto* button : buttons)
                if (button->getButtonText().startsWith ("MOVE TO LFO") && visibleInTree (button))
                    move = button;
            expect (move != nullptr, "the MSEG panel offers MOVE TO LFO");
            if (move != nullptr)
            {
                const auto target = move->getButtonText().getTrailingIntValue() - 1;
                move->triggerClick();
                settle (300);
                auto routesMoved = true;
                for (int slot = 0; slot < Mod::maxSlots; ++slot)
                    routesMoved = routesMoved && processor.readModSlot (slot).source != Mod::Source::Mseg;
                const auto shape = juce::roundToInt (processor.apvts.getRawParameterValue ("lfo" + juce::String (target + 1) + "_shape")->load());
                expect (target >= 0 && routesMoved && processor.readModSlot (40).source == Mod::lfoSourceFor (target)
                            && shape == IlanaSynthAudioProcessor::curveShape && ! lfoCards->isCardInPool (msegCard) && processor.isLfoShown (target),
                        "MOVE TO LFO draws the MSEG on LFO " + juce::String (target + 1) + " (SHAPE > MSEG) with its routes, and its card goes");
                processor.getUndoManager().undo();
                settle (300);
                expect (modSourceRouted (processor, Mod::Source::Mseg), "undo puts the MSEG's routes back");
            }
        }

        // A one-shot MSEG (LOOP off) stays a module: an LFO always cycles.
        if (auto* loop = processor.apvts.getParameter ("mseg_loop"))
            loop->setValueNotifyingHost (0.0f);
        settle (100);
        expect (processor.legacyMsegTargetLfo() < 0 && ! processor.moveLegacyMsegToLfo() && modSourceRouted (processor, Mod::Source::Mseg),
                "a one-shot MSEG stays the module (no LFO can play it once)");
        processor.loadFactoryPreset (neuroWobble);
        settle (300);
    }

    // M8.5: the VECTOR page has the pad (EVOLVE moved onto the macro card,
    // and VECTOR X / Y live in the source bar only: review 8, I8-13/14).
    {
        pages->showPage ("VECTOR");
        settle (200);
        auto* page = pages->getCurrentPage();
        expect (page != nullptr && findChild<VectorPadDisplay> (*page) != nullptr, "the VECTOR page shows the vector pad");
        std::vector<KnobControl*> knobs;
        std::vector<ModSourceChip*> pageChips;
        if (page != nullptr)
        {
            findAll<KnobControl> (*page, knobs);
            findAll<ModSourceChip> (*page, pageChips);
        }
        auto evolveKnobs = 0;
        for (auto* knob : knobs)
            evolveKnobs += knob->getParameterId().contains ("_evolve") ? 1 : 0;
        expect (evolveKnobs == 0 && pageChips.empty(), "VECTOR has no EVOLVE pane and no VECTOR X / Y chips of its own (I8-13, I8-14)");

        // EVOLVE on the macro's card: AMOUNT and RATE sliders for that
        // macro, and FREEZE; a macro that evolves wears a mark in the strip.
        pages->showPage ("MAIN");
        settle (200);
        std::vector<StripKnob*> macros;
        findAll<StripKnob> (*editor, macros);
        StripKnob* macro3 = nullptr;
        for (auto* macro : macros)
            if (macro->getMacroIndex() == 2)
                macro3 = macro;
        auto* card = ModHoverPopup::instance();
        if (macro3 != nullptr && card != nullptr)
        {
            macro3->openCard();
            card->holdOpen (true);
            settle (100);
            auto* amount = card->getEvolveAmount();
            auto* rate = card->getEvolveRate();
            const auto ids = amount != nullptr && rate != nullptr
                             && amount->getSlider().getTooltip().startsWith (processor.apvts.getParameter ("macro3_evolve")->getName (64))
                             && rate->getSlider().getTooltip().startsWith (processor.apvts.getParameter ("macro3_evolve_rate")->getName (64));
            const auto inside = amount != nullptr && card->getLocalBounds().contains (amount->getBounds())
                                && rate != nullptr && card->getLocalBounds().contains (rate->getBounds())
                                && ! amount->getBounds().intersects (rate->getBounds())
                                && card->getLocalBounds().contains (card->getFreezeButton().getBounds())
                                && card->getFreezeButton().isVisible();
            expect (ids && inside, "a macro's card carries its EVOLVE (AMOUNT, RATE) and FREEZE, inside the card, not overlapping");
            auto* evolve = processor.apvts.getParameter ("macro3_evolve");
            const auto evolveWas = evolve->getValue();
            evolve->setValueNotifyingHost (0.5f);
            macro3->refreshTargets();
            const auto marked = macro3->isEvolving() && macro3->isAssigned();
            evolve->setValueNotifyingHost (evolveWas);
            macro3->refreshTargets();
            expect (marked, "an evolving macro is marked in the strip and counts as assigned");
            card->holdOpen (false);
            card->close();
            // A knob's card has no EVOLVE.
            expect (card->getEvolveAmount() == nullptr || ! card->isVisible(), "only a macro's card has EVOLVE");
        }
        else
        {
            expect (false, "the macro strip and its card exist");
        }
    }

    // M8.6: BOUNCE on every oscillator card; a bounce turns the card to
    // Sample mode.
    {
        pages->showPage ("OSC");
        settle (200);
        std::vector<juce::TextButton*> buttons;
        findAll<juce::TextButton> (*editor, buttons);
        auto bounces = 0;
        for (auto* button : buttons)
            bounces += button->getButtonText() == "RESAMPLE" && visibleInTree (button) ? 1 : 0;
        expect (bounces >= 1, "the switched-on oscillator cards have RESAMPLE buttons (" + juce::String (bounces) + ")");

        IlanaSynthAudioProcessor::BounceRequest request;
        request.targetOsc = 1;
        request.holdSeconds = 0.5;
        request.tailSeconds = 0.5;
        request.muteOthers = false;
        processor.startBounce (request);
        for (int i = 0; i < 300 && processor.getBounceState() == IlanaSynthAudioProcessor::BounceState::Rendering; ++i)
            settle (50);
        settle (300);
        selectOscTab (1);
        std::vector<KnobControl*> knobs;
        findAll<KnobControl> (*editor, knobs);
        auto sampleShown = false;
        for (auto* knob : knobs)
            sampleShown = sampleShown || (knob->getParameterId() == "osc2_sample_start" && visibleInTree (knob));
        expect (processor.getBounceState() == IlanaSynthAudioProcessor::BounceState::Done && sampleShown,
                "a bounce puts OSC 2 in Sample mode with the sample controls showing");
    }

    // Step 30: the sample display zooms, and a typed position sets START / END.
    {
        std::vector<WaveDisplay*> waves;
        findAll<WaveDisplay> (*editor, waves);
        WaveDisplay* wave = nullptr;
        for (auto* candidate : waves)
            if (visibleInTree (candidate) && candidate->getOscIndex() == 1)
                wave = candidate;
        expect (wave != nullptr, "OSC 2's sample display is shown");

        if (wave != nullptr)
        {
            const auto read = [&processor] (const char* id) { return processor.apvts.getRawParameterValue (id)->load(); };
            expect (wave->getSampleView().isFull(), "a sample display starts fitted");
            // ILANA_UITEST_SHOTS=<dir>: pictures of the zoom, for review.
            const auto shotsDir = juce::SystemStats::getEnvironmentVariable ("ILANA_UITEST_SHOTS", "");
            const auto shot = [&] (const char* name)
            {
                if (shotsDir.isNotEmpty())
                {
                    settle (150);
                    save (*editor, juce::File (shotsDir).getChildFile (name));
                }
            };
            shot ("sample-fitted.png");
            wave->zoomSample (0.5, 4.0);
            expect (std::abs (wave->getSampleView().span() - 0.25) < 1.0e-6, "zooming in 4x shows a quarter of the sample");
            wave->setSamplePosition (1, 0.9f);
            const auto start = read ("osc2_sample_start"), end = read ("osc2_sample_end");
            expect (start < end && start > 0.8f, "a typed START lands, and stays under END (" + juce::String (start, 3) + " < " + juce::String (end, 3) + ")");
            const auto view = wave->getSampleView();
            expect ((double) start >= view.lo - 1.0e-6 && (double) start <= view.hi + 1.0e-6, "the zoom window follows a typed position");
            shot ("sample-zoomed.png");
            wave->showPositionEditor (1);
            shot ("sample-typed-start.png");
            wave->zoomSample (0.5, 0.001);
            expect (wave->getSampleView().isFull(), "zooming out fits the sample");
            wave->setSamplePosition (1, 0.0f);
            expect (read ("osc2_sample_start") < 0.001f, "a typed START of zero goes back to the head");

            // Step 27: LOOP PTS gives the loop its own handles.
            const auto set = [&processor] (const char* id, float value)
            {
                if (auto* parameter = processor.apvts.getParameter (id))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
            };
            const auto plot = wave->samplePlotForTest();
            const auto midY = plot.getCentreY();
            set ("osc2_sample_loop", 1.0f);
            set ("osc2_sample_loop_own", 0.0f);
            wave->setSamplePosition (2, 1.0f);
            settle (150);
            expect (wave->handleAtForTest (wave->sampleXForTest (0.25), midY) == 0, "LOOP PTS off: no loop handles on the wave");
            shot ("sample-loop-whole.png");
            set ("osc2_sample_loop_own", 1.0f);
            set ("osc2_sample_loop_start", 0.25f);
            set ("osc2_sample_loop_end", 0.75f);
            settle (150);
            expect (wave->handleAtForTest (wave->sampleXForTest (0.25), midY) == 3, "LOOP PTS on: the loop's start handle is on the wave");
            expect (wave->handleAtForTest (wave->sampleXForTest (0.75), midY) == 4, "LOOP PTS on: the loop's end handle is on the wave");
            expect (wave->handleAtForTest (plot.getX() + 1.0f, midY) == 1 && wave->handleAtForTest (plot.getRight() - 1.0f, midY) == 2,
                    "START and END keep their handles");
            shot ("sample-loop-points.png");

            // Step 29: ROOT and ROOT FINE on the card, and DETECT in the header.
            set ("osc2_sample_root", 45.0f);
            settle (150);
            expect (! processor.detectSampleRoot (1).isEmpty(), "DETECT always says what it found");
            shot ("sample-root.png");
            wave->setSamplePosition (3, 0.4f);
            expect (std::abs (read ("osc2_sample_loop_start") - 0.4f) < 0.002f, "a typed LOOP START lands");
            wave->setSamplePosition (4, 0.3f);
            expect (read ("osc2_sample_loop_end") > read ("osc2_sample_loop_start"), "LOOP END stays after LOOP START");
            wave->setSamplePosition (3, 1.0f);
            expect (read ("osc2_sample_loop_start") < read ("osc2_sample_loop_end"), "LOOP START stays before LOOP END");
            wave->setSamplePosition (4, 0.6f);
            wave->setSamplePosition (3, 0.0f);
            wave->setSamplePosition (1, 0.5f);
            expect (wave->handleAtForTest (wave->sampleXForTest (0.5), plot.getY() + 4.0f) == 1,
                    "a loop edge sitting on START: a press on START's flag takes START");
            expect (wave->handleAtForTest (wave->sampleXForTest (0.5), midY) == 3,
                    "a loop edge sitting on START: a press lower down takes the loop's edge");
            shot ("sample-loop-attack-cut.png");
            wave->setSamplePosition (1, 0.0f);
            set ("osc2_sample_loop_own", 0.0f);
            set ("osc2_sample_loop", 0.0f);

            // Step 28: SNAP puts a typed marker on a zero crossing of the wave.
            if (const auto* bounced = processor.getSampleForOsc (1); bounced != nullptr && bounced->getNumSamples() > 4000)
            {
                const auto frames = bounced->getNumSamples();
                const auto* left = bounced->buffer.getReadPointer (0);
                const auto* right = bounced->getNumChannels() > 1 ? bounced->buffer.getReadPointer (1) : nullptr;
                const auto mix = [&] (int i) { return right != nullptr ? (left[i] + right[i]) * 0.5f : left[i]; };
                const auto crossesAt = [&] (float fraction)
                {
                    const auto i = juce::jlimit (0, frames - 2, juce::roundToInt (fraction * (float) frames));
                    for (int k = -2; k <= 2; ++k)
                        if (i + k >= 0 && i + k + 1 < frames && ((mix (i + k) <= 0.0f) != (mix (i + k + 1) <= 0.0f)))
                            return true;
                    return false;
                };
                set ("osc2_sample_snap", 0.0f);
                wave->setSamplePosition (1, 0.31337f);
                expect (std::abs (read ("osc2_sample_start") - 0.31337f) < 0.0001f, "SNAP off: a typed START stays where typed");
                set ("osc2_sample_snap", 1.0f);
                wave->setSamplePosition (1, 0.31337f);
                expect (crossesAt (read ("osc2_sample_start")) && std::abs (read ("osc2_sample_start") - 0.31337f) < 0.02f,
                        "SNAP on: a typed START lands on a zero crossing of the wave");
                set ("osc2_sample_snap", 0.0f);
                wave->setSamplePosition (1, 0.0f);
            }

            // The crossfade knob and SNAP sit on the sample card's third row.
            std::vector<KnobControl*> loopKnobs;
            findAll<KnobControl> (*editor, loopKnobs);
            auto xfadeShown = false;
            for (auto* knob : loopKnobs)
                xfadeShown = xfadeShown || (knob->getParameterId() == "osc2_sample_loop_xfade" && visibleInTree (knob));
            expect (xfadeShown, "the XFADE knob shows on a sample card");
            set ("osc2_sample_loop", 1.0f);
            set ("osc2_sample_loop_xfade", 0.5f);
            shot ("sample-loop-xfade.png");
            set ("osc2_sample_loop_xfade", 0.0f);
            set ("osc2_sample_loop", 0.0f);
        }
    }

    // The string has one editor, its oscillator's card (the PHYSICAL tab
    // folded in, 2026-10-10): every control of the physical list is on the
    // card, in its order, with the moving string in the well; the body is a
    // link (EDIT BODY > opens FILTER), the soundboard is the strip's.
    {
        processor.loadFactoryPreset (names.indexOf ("Felt Hammer Board"));
        pages->showPage ("OSC");
        settle (400);
        juce::String physicalPrefix;
        for (const auto* prefix : OscillatorIds::prefixes)
            if (physicalPrefix.isEmpty() && juce::roundToInt (processor.apvts.getRawParameterValue (juce::String (prefix) + "_mode")->load()) == 1)
                physicalPrefix = prefix;
        selectOscTab (physicalPrefix.getTrailingIntValue() - 1);

        for (const auto excite : { 9, 0, 4, 10, 7 })
        {
            if (auto* parameter = processor.apvts.getParameter (physicalPrefix + "_excite"))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) excite));
            settle (300);
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            juce::StringArray listIds, missing;
            for (const auto& row : physicalControlRows (excite))
                for (const auto& spec : row.second)
                    if (dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter (physicalPrefix + spec.suffix)) != nullptr)
                        listIds.add (physicalPrefix + spec.suffix);
            std::vector<juce::Rectangle<int>> boxes;
            for (const auto& id : listIds)
            {
                auto shown = false;
                for (auto* knob : knobs)
                    if (knob->getParameterId() == id && visibleInTree (knob) && ! knob->getBounds().isEmpty())
                    {
                        shown = true;
                        boxes.push_back (editor->getLocalArea (knob, knob->packedCell().reduced (1, 3)));
                    }
                if (! shown)
                    missing.add (id);
            }
            auto overlap = false;
            for (size_t i = 0; i < boxes.size(); ++i)
                for (size_t j = i + 1; j < boxes.size(); ++j)
                    overlap = overlap || boxes[i].intersects (boxes[j]);
            expect (! listIds.isEmpty() && missing.isEmpty() && ! overlap,
                    "OSC: excite " + juce::String (excite) + ", the card shows every string and exciter control, none overlapping"
                        + (missing.isEmpty() ? juce::String() : " (missing " + missing.joinIntoString (", ") + ")"));
        }
        if (auto* parameter = processor.apvts.getParameter (physicalPrefix + "_excite"))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (9.0f));
        settle (300);

        std::vector<PhysicalView*> views;
        findAll<PhysicalView> (*editor, views);
        auto stringShown = false;
        for (auto* view : views)
            stringShown = stringShown || (visibleInTree (view) && view->getOscillator() == physicalPrefix);
        expect (stringShown, "the OSC card shows the moving string");
        {
            std::vector<juce::TextButton*> buttons;
            findAll<juce::TextButton> (*editor, buttons);
            juce::TextButton* editBody = nullptr;
            for (auto* button : buttons)
                if (visibleInTree (button) && button->getButtonText() == juce::String::fromUTF8 ("EDIT BODY \xe2\x80\xba"))
                    editBody = button;
            if (editBody != nullptr)
            {
                editBody->triggerClick();
                settle (300);
            }
            expect (editBody != nullptr && pages->getCurrentPageId() == "MAIN", "OSC's EDIT BODY > opens PLAY (its BODY / BOARD card, one-tab PLAY 2026-10-25)");
            pages->showPage ("OSC");
            settle (200);
        }
    }

    // (Plain-value parameter access for the review-6 FILTER checks below.)
    const auto setParam = [&processor] (const juce::String& id, float plain)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
    };
    const auto readParam = [&processor] (const juce::String& id)
    {
        const auto* value = processor.apvts.getRawParameterValue (id);
        return value != nullptr ? value->load() : 0.0f;
    };

    // UI review 6 (V5-17, S5-12, S6-20): the filter type is one compact
    // picker per filter (no 12-button grids), every model once in its menu,
    // the analogue models under ANALOG, and arrows that step through it as
    // one undo step each.
    {
        processor.loadFactoryPreset (0);
        pages->showPage ("FILTER");
        settle (300);
        std::vector<FilterTypePicker*> pickers;
        findAll<FilterTypePicker> (*editor, pickers);
        auto shownPickers = 0;
        for (auto* picker : pickers)
            shownPickers += visibleInTree (picker) && picker->getHeight() <= 30 ? 1 : 0;
        expect (shownPickers == 2, "FILTER shows one compact type picker per filter (" + juce::String (shownPickers) + ")");

        auto order = FilterTypes::order();
        std::sort (order.begin(), order.end());
        expect ((int) order.size() == FilterType::Count && std::adjacent_find (order.begin(), order.end()) == order.end()
                    && order.front() == 0 && order.back() == FilterType::Count - 1,
                "the type menu lists every filter model exactly once");
        expect (FilterTypes::shortNames().size() == FilterType::Count, "every filter type has a short name");
        auto analogue = true;
        for (const auto type : { FilterType::LadderLow, FilterType::LadderHigh, FilterType::DiodeLow, FilterType::Ms20Low,
                                 FilterType::MoogDrive, FilterType::Acid303, FilterType::Sem })
            analogue = analogue && FilterTypes::getPageName (FilterTypes::pageOf (type)) == "ANALOG";
        expect (analogue, "Ladder, Diode, MS-20, Moog, 303 and SEM are under ANALOG");
        expect (FilterTypes::displayName (FilterType::AwZLow).startsWith ("Smooth") && FilterTypes::shortNames()[FilterType::AwYNotLow] == "Reso LP",
                "the Airwindows filters are named by what they do");

        if (! pickers.empty())
        {
            auto* picker = pickers.front();
            const auto before = juce::roundToInt (readParam ("f1_type"));
            clearHistory();
            const juce::Point<float> nextArrow ((float) picker->getWidth() - 6.0f, (float) picker->getHeight() * 0.5f);
            static_cast<juce::Component*> (picker)->mouseDown (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), nextArrow,
                                                                                 juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, picker, picker,
                                                                                 juce::Time::getCurrentTime(), nextArrow, juce::Time::getCurrentTime(), 1, false));
            settle (100);
            const auto after = juce::roundToInt (readParam ("f1_type"));
            const auto steps = undoSteps();
            expect (after == FilterTypes::stepFrom (before, 1) && after != before && steps.size() == 1 && steps[0] == "Filter 1 type",
                    "the picker's next arrow steps Filter 1 to the next model, one undo step (" + juce::String (before) + " -> "
                        + juce::String (after) + ")");
            processor.getUndoManager().undo();
            settle (100);
            expect (juce::roundToInt (readParam ("f1_type")) == before, "undo brings the filter type back");
        }
    }

    // UI review 6 (I6-15, I6-16): WEST is its own card, shown beside Filter 2
    // (not a hidden tab); with PLACE Replace Filter 2, Filter 2's card dims
    // and the graph and flow drop Filter 2.
    {
        if (auto* westOn = processor.apvts.getParameter ("west_on"))
            westOn->setValueNotifyingHost (1.0f); // (it folds while off: V9-4)
        pages->showPage ("FILTER");
        settle (200);
        const auto visibleKnob = [&editor] (const juce::String& id)
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
                if (knob->getParameterId() == id && visibleInTree (knob))
                    return knob;
            return (KnobControl*) nullptr;
        };
        std::vector<CardTabs*> cardTabs;
        findAll<CardTabs> (*editor, cardTabs);
        auto westTabs = false;
        for (auto* bar : cardTabs)
            westTabs = westTabs || (visibleInTree (bar) && bar->getNames().contains ("WEST"));
        expect (! westTabs && visibleKnob ("west_fold") != nullptr && visibleKnob ("west_decay") != nullptr && visibleKnob ("f2_cutoff") != nullptr,
                "FILTER shows WEST as its own card beside Filter 2, no FILTER 2 / WEST tabs");

        auto* f2Cutoff = visibleKnob ("f2_cutoff");
        const auto litBefore = f2Cutoff != nullptr && f2Cutoff->getAlpha() > 0.99f;
        setParam ("west_on", 1.0f);
        setParam ("west_pos", 1.0f);
        settle (400);
        const auto dimmed = f2Cutoff != nullptr && f2Cutoff->getAlpha() < 0.6f;
        auto markerFor2 = 1;
        if (auto* display = findChild<FilterDisplay> (*editor); display != nullptr)
            markerFor2 = display->filterAt (display->getMarkerCentres()[1]);
        setParam ("west_pos", 0.0f);
        setParam ("west_on", 0.0f);
        settle (400);
        expect (litBefore && dimmed && markerFor2 == 0 && f2Cutoff->getAlpha() > 0.99f,
                "WEST in Filter 2's place dims Filter 2's card and the graph stops offering Filter 2's marker");
    }

    // Review 14 (I14-5, V14-12): WEST and BODY open together (WEST on shows
    // BODY's knobs too, dimmed, so no hole sits beside it), and Filter 2 has
    // a switch in its header that parks CUTOFF at the top and brings it back.
    {
        processor.loadFactoryPreset (0);
        pages->showPage ("FILTER");
        settle (300);
        const auto visibleKnob = [&editor] (const juce::String& id)
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
                if (knob->getParameterId() == id && visibleInTree (knob))
                    return knob;
            return (KnobControl*) nullptr;
        };
        // (The design keeps them open, dimmed while off: controls dim in place.)
        const auto foldedTogether = visibleKnob ("west_fold") != nullptr && visibleKnob ("west_fold")->getAlpha() < 0.99f
                                    && visibleKnob ("body_material") != nullptr && visibleKnob ("body_material")->getAlpha() < 0.99f;
        setParam ("west_on", 1.0f);
        settle (400);
        const auto openTogether = visibleKnob ("west_fold") != nullptr && visibleKnob ("body_material") != nullptr;
        setParam ("west_on", 0.0f);
        settle (400);
        expect (foldedTogether && openTogether, "WEST and BODY dim together while off and light together when on (I14-5)");

        std::vector<juce::TextButton*> buttons;
        findAll<juce::TextButton> (*editor, buttons);
        juce::TextButton* f2Switch = nullptr;
        for (auto* button : buttons)
            if (visibleInTree (button) && button->getButtonText().isEmpty() && button->getProperties().contains ("switch"))
                f2Switch = button;
        expect (f2Switch != nullptr && f2Switch->getToggleState() == false, "FILTER 2 has a switch in its header, off while it is open at 20 kHz (V14-12)");
        if (f2Switch != nullptr)
        {
            f2Switch->triggerClick();
            settle (300);
            const auto cutoffOn = readParam ("f2_cutoff");
            f2Switch->triggerClick();
            settle (300);
            expect (cutoffOn < 19000.0f && readParam ("f2_cutoff") >= 19999.0f, "the F2 switch brings a cutoff in and parks it at the top again (V14-12)");
        }
        setParam ("f2_cutoff", 20000.0f);
        settle (200);
    }

    // UI review 6 (V6-18, S6-21): the graph's markers sit on their filter's
    // response, 8 px inside the plot: Init's two filters, open at 20 kHz,
    // are high on the right (not in the bottom corner) and fanned apart.
    {
        processor.loadFactoryPreset (0);
        pages->showPage ("FILTER");
        settle (300);
        if (auto* display = findChild<FilterDisplay> (*editor); display != nullptr && visibleInTree (display))
        {
            const auto markers = display->getMarkerCentres();
            const auto plot = display->getLocalBounds().toFloat().reduced (10.0f, 12.0f);
            const auto inset = plot.reduced (8.0f);
            expect (inset.contains (markers[0]) && inset.contains (markers[1]) && markers[0].getDistanceFrom (markers[1]) >= 10.0f
                        && markers[0].y < plot.getCentreY() && markers[1].y < plot.getCentreY(),
                    "Init: the open filters' markers sit high on the response, inside the plot, apart ("
                        + markers[0].toString() + " / " + markers[1].toString() + ")");
        }
        else
            expect (false, "FILTER shows the response graph");

        // BALANCE does nothing in serial: dimmed and disabled (V6-18).
        setParam ("filters_parallel", 0.0f);
        settle (300);
        std::vector<KnobControl*> knobs;
        findAll<KnobControl> (*editor, knobs);
        KnobControl* balance = nullptr;
        for (auto* knob : knobs)
            if (knob->getParameterId() == "filter_balance" && visibleInTree (knob))
                balance = knob;
        const auto serialOff = balance != nullptr && ! balance->isEnabled() && balance->getAlpha() < 0.99f;
        setParam ("filters_parallel", 1.0f);
        settle (300);
        expect (serialOff && balance->isEnabled(), "BALANCE is disabled in serial and enabled in parallel");
        setParam ("filters_parallel", 0.0f);
        settle (200);
    }

    // UI review 6 (S6-16): in SIGNAL FLOW an oscillator with OUT off is an
    // FM modulator, not a source into the filters (checked through its
    // tooltip, which names what it modulates).
    {
        setParam ("osc2_on", 1.0f);
        setParam ("osc2_out", 0.0f);
        setParam ("fm_amount", 0.5f); // OSC 2 > OSC 1
        pages->showPage ("FILTER");
        settle (400);
        std::vector<SignalFlow*> flows;
        findAll<SignalFlow> (*editor, flows);
        juce::String tip;
        for (auto* flow : flows)
        {
            if (! visibleInTree (flow))
                continue;
            for (int y = 2; y < flow->getHeight() && tip.isEmpty(); y += 2)
            {
                const juce::Point<float> at (16.0f, (float) y);
                flow->mouseMove (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f,
                                                   0.0f, 0.0f, flow, flow, juce::Time::getCurrentTime(), at, juce::Time::getCurrentTime(), 1, false));
                if (flow->getTooltip().startsWith ("OSC 2"))
                    tip = flow->getTooltip();
            }
        }
        expect (tip.contains ("isn't heard") && tip.contains ("OSC 1"), "SIGNAL FLOW shows OSC 2 (OUT off) as OSC 1's FM modulator ('" + tip + "')");
        processor.loadFactoryPreset (0);
        settle (200);
    }

    // Knobs, chips, the filter graph, the browser and SUB + NOISE (UI review 1 fixes).
    {
        const auto makeEvent = [] (juce::Component& component, juce::Point<float> position, bool dragged)
        {
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys(),
                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component, juce::Time::getCurrentTime(),
                                     position, juce::Time::getCurrentTime(), 1, dragged);
        };

        // A click on a source chip keeps every knob it drives lit; another click clears it.
        processor.loadFactoryPreset (neuroWobble);
        pages->showPage ("MAIN");
        settle (400);
        std::vector<ModSourceChip*> chipsFound;
        std::vector<KnobControl*> knobsFound;
        findAll<ModSourceChip> (*editor, chipsFound);
        findAll<KnobControl> (*editor, knobsFound);
        ModSourceChip* chosen = nullptr;
        auto drivenKnobs = 0;

        for (auto* chip : chipsFound)
        {
            if (! visibleInTree (chip))
                continue;

            auto count = 0;

            for (auto* knob : knobsFound)
                if (visibleInTree (knob) && knob->isDrivenBy (chip->getSourceIndex()))
                    ++count;

            if (count > 0)
            {
                chosen = chip;
                drivenKnobs = count;
                break;
            }
        }

        expect (chosen != nullptr, "Neuro Wobble has a source chip that drives a visible knob");

        if (chosen != nullptr)
        {
            auto lit = [&]
            {
                auto count = 0, wrong = 0;
                for (auto* knob : knobsFound)
                    if (visibleInTree (knob))
                    {
                        count += knob->isLitByPinnedSource() ? 1 : 0;
                        wrong += knob->isLitByPinnedSource() != knob->isDrivenBy (chosen->getSourceIndex()) ? 1 : 0;
                    }
                return std::pair<int, int> { count, wrong };
            };

            expect (pinnedModSource() == 0 && lit().first == 0, "no source is pinned to start with");
            chosen->mouseUp (makeEvent (*chosen, { 8.0f, 8.0f }, false));
            settle (100);
            expect (pinnedModSource() == chosen->getSourceIndex() && chosen->isPinned(), "clicking a chip pins its source");
            expect (lit().first == drivenKnobs && lit().second == 0,
                    "the pinned chip lights exactly the knobs it drives (" + juce::String (drivenKnobs) + ")");
            chosen->mouseUp (makeEvent (*chosen, { 8.0f, 8.0f }, true)); // a drag is not a click
            expect (pinnedModSource() == chosen->getSourceIndex(), "dragging a chip does not unpin it");
            chosen->mouseUp (makeEvent (*chosen, { 8.0f, 8.0f }, false));
            expect (pinnedModSource() == 0 && lit().first == 0, "clicking the chip again clears the highlight");
        }

        // UI review 4, batch B (V8, V9, V17, V18, V24, S7, S12): modulation.
        {
            using Module = IlanaSynthAudioProcessor::Module;
            const auto usedSource = [&processor] (int source)
            {
                for (int i = 0; i < Mod::maxSlots; ++i)
                {
                    const auto slot = processor.readModSlot (i);
                    if (slot.destination != 0 && ((int) slot.source == source || (int) slot.aux == source))
                        return true;
                }
                return false;
            };
            const auto poolKindOf = [] (int source) -> std::pair<int, int>
            {
                if (const auto lfo = Mod::lfoIndexFor ((Mod::Source) source); lfo >= 0)
                    return { (int) Module::Lfo, lfo };
                using S = Mod::Source;
                const S fixedEnvs[] { S::AmpEnv, S::FilterEnv, S::FilterEnv2, S::ModEnv, S::Env4 };
                for (int i = 0; i < 5; ++i)
                    if ((S) source == fixedEnvs[i])
                        return { (int) Module::Envelope, i };
                if (source >= (int) S::Env6 && source <= (int) S::Env16)
                    return { (int) Module::Envelope, 5 + source - (int) S::Env6 };
                return { -1, 0 };
            };
            // (Chips in a group's tray are counted through their group chip.)
            const auto chipState = [&]
            {
                std::vector<ModSourceChip*> found;
                findAll<ModSourceChip> (*editor, found);
                auto shown = 0, compact = 0, strays = 0;
                juce::StringArray strayNames;
                for (auto* chip : found)
                {
                    if (! chip->isVisible() || chip->getParentComponent() == nullptr
                        || dynamic_cast<ModSourceTray*> (chip->getParentComponent()) != nullptr)
                        continue;
                    ++shown;
                    // "Compact" now means a chip not showing its source's
                    // one name (a code such as "E6" or "AT").
                    compact += chip->getSourceName() != ModNames::sourceUpper (chip->getSourceIndex()) ? 1 : 0;
                    const auto [kind, index] = poolKindOf (chip->getSourceIndex());
                    if (kind >= 0 && ! processor.isRevealed ((Module) kind, index) && ! usedSource (chip->getSourceIndex()))
                    {
                        ++strays;
                        strayNames.add (chip->getSourceName());
                    }
                }
                return std::make_tuple (shown, compact, strays, strayNames.joinIntoString (", "));
            };

            // The chip row follows the pool, and every chip shows its
            // source's one name, never a code (UI review 6, V6-9 / S6-19).
            {
                const auto [shown, compact, strays, strayNames] = chipState();
                expect (shown > 0 && strays == 0, "the chip row shows only sources in the pool or in use (" + juce::String (shown)
                                                      + " shown" + (strays > 0 ? "; not in the pool: " + strayNames : juce::String()) + ")");
                expect (compact == 0, "every chip shows its source's full name (" + juce::String (compact) + " codes)");
            }

            // "+" adds a source to the pool: LFO 9 (chip index 9, after OP
            // LFO) gets a chip.
            const auto lfoMask = processor.isRevealed (Module::Lfo, 8);
            pages->addPoolSource (9);
            settle (100);
            {
                std::vector<ModSourceChip*> found;
                findAll<ModSourceChip> (*editor, found);
                auto lfo9 = false;
                for (auto* chip : found)
                    lfo9 = lfo9 || (chip->isVisible() && chip->getSourceIndex() == (int) Mod::lfoSourceFor (8));
                // (Or folded into its region's "+N", the LFO region being full.)
                std::vector<ModSourceGroupChip*> groups;
                findAll<ModSourceGroupChip> (*editor, groups);
                for (auto* group : groups)
                    lfo9 = lfo9 || (group->isVisible() && std::find (group->getSources().begin(), group->getSources().end(),
                                                                     (int) Mod::lfoSourceFor (8)) != group->getSources().end());
                juce::StringArray seen;
                for (auto* chip : found)
                    if (chip->isVisible())
                        seen.add (chip->getSourceName());
                for (auto* group : groups)
                    if (group->isVisible())
                        seen.add (group->getLabel());
                expect (processor.isRevealed (Module::Lfo, 8) && lfo9, "the chip picker adds LFO 9 to the pool and the row ("
                                                                           + seen.joinIntoString (" | ") + ")");
            }
            processor.setRevealed (Module::Lfo, 8, lfoMask);

            // A full pool: every chip shown, all shortened alike.
            std::vector<bool> lfoBefore, envBefore;
            for (int i = 0; i < 16; ++i)
            {
                lfoBefore.push_back (processor.isRevealed (Module::Lfo, i));
                envBefore.push_back (processor.isRevealed (Module::Envelope, i));
                processor.setRevealed (Module::Lfo, i, true);
                processor.setRevealed (Module::Envelope, i, true);
            }
            // The row follows the pool on the editor's 250 ms timer, so a
            // fixed 200 ms settle sometimes checked it before the timer had
            // run (the flake): wait for that tick, up to two seconds.
            // A full pool: the LFOs and envelopes fold their last chips into
            // group chips ("ENV +11"), whose tray shows them; no chip is
            // shortened, none is lost, and the chips keep their full width.
            const auto groupState = [&]
            {
                std::vector<ModSourceGroupChip*> groups;
                findAll<ModSourceGroupChip> (*editor, groups);
                auto folded = 0, visibleGroups = 0;
                ModSourceGroupChip* biggest = nullptr;
                for (auto* group : groups)
                    if (group->isVisible())
                    {
                        ++visibleGroups;
                        folded += (int) group->getSources().size();
                        if (biggest == nullptr || group->getSources().size() > biggest->getSources().size())
                            biggest = group;
                    }
                return std::make_tuple (folded, visibleGroups, biggest);
            };
            // (32 pool chips and the five performance sources; the MSEG chip
            // only shows while the patch uses the old module: I8-4.)
            for (int wait = 0; wait < 40 && std::get<0> (chipState()) + std::get<0> (groupState()) < 32 + 5; ++wait)
                settle (50);
            {
                const auto [shown, compact, strays, strayNames] = chipState();
                const auto [folded, visibleGroups, biggest] = groupState();
                expect (shown + folded >= 32 + 5 && compact == 0 && visibleGroups > 0,
                        "a full pool keeps every source (" + juce::String (shown) + " chips, " + juce::String (folded) + " in "
                            + juce::String (visibleGroups) + " group chips), none shortened (" + juce::String (compact) + ")");

                std::vector<ModSourceChip*> found;
                findAll<ModSourceChip> (*editor, found);
                auto narrow = 0;
                for (auto* chip : found)
                    if (chip->isVisible() && (float) chip->getWidth() < chip->getNaturalWidth() * 0.9f - 4.0f)
                        ++narrow;
                expect (narrow == 0, "no chip is squeezed below its name's width (" + juce::String (narrow) + ")");

                // The tray: its chips are the folded sources, draggable.
                if (biggest != nullptr && biggest->onOpen != nullptr)
                {
                    biggest->onOpen (*biggest);
                    settle (100);
                    auto* tray = findChild<ModSourceTray> (*editor);
                    const auto trayChips = tray != nullptr ? (int) tray->getChips().size() : 0;
                    auto inside = tray != nullptr && tray->isVisible();
                    if (tray != nullptr)
                        for (auto& chip : tray->getChips())
                            inside = inside && tray->getLocalBounds().contains (chip->getBounds());
                    expect (trayChips == (int) biggest->getSources().size() && inside,
                            "a group chip opens a tray with its " + juce::String ((int) biggest->getSources().size())
                                + " chips (" + juce::String (trayChips) + ")");
                    if (tray != nullptr)
                        tray->close();
                }
            }
            for (int i = 0; i < 16; ++i)
            {
                processor.setRevealed (Module::Lfo, i, lfoBefore[(size_t) i]);
                processor.setRevealed (Module::Envelope, i, envBefore[(size_t) i]);
            }
            settle (200);

            // One colour per source: eight macros, the performance sources,
            // and LFO 1 off the accent.
            {
                using S = Mod::Source;
                std::vector<int> sources;
                for (int m = 0; m < Mod::numMacros; ++m)
                    sources.push_back ((int) Mod::macroSourceFor (m));
                for (const auto s : { S::Velocity, S::KeyTrack, S::Random, S::ClockSh, S::ModWheel, S::Aftertouch, S::Expression,
                                      S::Lfo1, S::Lfo2, S::Lfo3, S::Lfo4, S::AmpEnv, S::FilterEnv, S::FilterEnv2, S::ModEnv, S::Env4 })
                    sources.push_back ((int) s);
                for (int s = (int) S::Env6; s <= (int) S::Env16; ++s)
                    sources.push_back (s);
                for (int s = (int) S::Lfo5; s <= (int) S::Lfo16; ++s)
                    sources.push_back (s);

                auto clashes = 0;
                juce::String example;
                for (size_t a = 0; a < sources.size(); ++a)
                    for (size_t b = a + 1; b < sources.size(); ++b)
                    {
                        const auto ca = modSourceColour (sources[a]), cb = modSourceColour (sources[b]);
                        const auto distance = std::abs (ca.getFloatRed() - cb.getFloatRed()) + std::abs (ca.getFloatGreen() - cb.getFloatGreen())
                                              + std::abs (ca.getFloatBlue() - cb.getFloatBlue());
                        if (distance < 0.06f)
                        {
                            ++clashes;
                            example = Mod::getSourceNames()[sources[a]] + " / " + Mod::getSourceNames()[sources[b]];
                        }
                    }
                expect (clashes == 0, "every source has its own colour (" + juce::String (clashes) + " clashes"
                                          + (example.isNotEmpty() ? ", e.g. " + example : juce::String()) + ")");

                std::set<juce::uint32> macroColours;
                for (int m = 0; m < Mod::numMacros; ++m)
                    macroColours.insert (modSourceColour ((int) Mod::macroSourceFor (m)).getARGB());
                expect (macroColours.size() == (size_t) Mod::numMacros, "the eight macros have eight colours");

                auto hueGap = std::abs (modSourceColour ((int) S::Lfo1).getHue() - IlanaTheme::accent().getHue());
                hueGap = juce::jmin (hueGap, 1.0f - hueGap);
                expect (hueGap > 0.08f, "LFO 1 is not the accent's orange");
            }

            // MATRIX: rows numbered 1..n as shown, repeats flagged, sortable,
            // the remap editor docked under its row.
            pages->showPage ("MATRIX");
            settle (400);
            const auto shownRows = [&]
            {
                std::vector<MatrixRow*> found, visible;
                findAll<MatrixRow> (*editor, found);
                for (auto* row : found)
                    if (row->isVisible())
                        visible.push_back (row);
                std::sort (visible.begin(), visible.end(), [] (auto* a, auto* b) { return a->getY() < b->getY(); });
                return visible;
            };
            {
                const auto visible = shownRows();
                auto numbered = ! visible.empty();
                auto duplicates = 0;
                for (size_t i = 0; i < visible.size(); ++i)
                {
                    numbered = numbered && visible[i]->getDisplayNumber() == (int) i + 1;
                    duplicates += visible[i]->isDuplicate() ? 1 : 0;
                }
                auto lastSlot = 0;
                for (auto* row : visible)
                    lastSlot = juce::jmax (lastSlot, row->getSlotIndex() + 1);
                expect (numbered && lastSlot > (int) visible.size(),
                        "matrix rows are numbered 1.." + juce::String (visible.size()) + " as shown (the last slot used is " + juce::String (lastSlot) + ")");
                // Factory presets load with their repeated routings merged
                // (review 6, S5-9): Neuro Wobble's two LFO 1 > Filter 1
                // Cutoff rows are one row now.
                expect (duplicates == 0, "a factory preset loads with no repeated routing (" + juce::String (duplicates) + " flagged)");
                // Rows grow to fill a page the dock's note doesn't fit on
                // (V8-20), up to 38 px, their controls at their own size.
                if (! visible.empty())
                    expect (visible[0]->getHeight() <= 34, "matrix rows stay compact (" + juce::String (visible[0]->getHeight()) + " px)");
            }

            if (auto* page = pages->getCurrentPage())
            {
                // With routings, "+ ADD MODULATION" is the row after the last
                // one (review 7, S7-35); the header's button is for an empty matrix.
                std::vector<juce::Button*> buttons;
                findAll<juce::Button> (*page, buttons);
                juce::Button* add = nullptr;
                auto headerAdd = false;
                for (auto* button : buttons)
                    if (button->getButtonText().contains ("ADD MODULATION"))
                    {
                        if (dynamic_cast<DashedAddButton*> (button) != nullptr)
                            add = button;
                        else
                            headerAdd = headerAdd || button->isVisible();
                    }
                auto lastRowBottom = 0;
                for (auto* row : shownRows())
                    lastRowBottom = juce::jmax (lastRowBottom, row->getBottom());
                expect (add != nullptr && add->isVisible() && ! headerAdd && add->getY() >= lastRowBottom
                            && add->getY() < lastRowBottom + 10,
                        "+ ADD MODULATION is the row after the last routing, not a header button");

                // The rows open grouped by source (S8-25), in the bar's order;
                // a click on SOURCE reverses that, on # goes back to slot order.
                const auto clickAt = [&] (juce::Point<float> at)
                {
                    page->mouseUp (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(), 1.0f,
                                                     0.0f, 0.0f, 0.0f, 0.0f, page, page, juce::Time::getCurrentTime(), at,
                                                     juce::Time::getCurrentTime(), 1, false));
                    settle (100);
                };
                const auto sourceRank = [&] (MatrixRow* row)
                {
                    const auto& order = ModNames::sourcesInMenuOrder();
                    return (int) std::distance (order.begin(), std::find (order.begin(), order.end(),
                                                                          (int) processor.readModSlot (row->getSlotIndex()).source));
                };
                const auto sortedBySource = [&] (bool reversed)
                {
                    const auto visible = shownRows();
                    auto sorted = ! visible.empty() && visible[0]->getDisplayNumber() == 1;
                    for (size_t i = 1; i < visible.size(); ++i)
                        sorted = sorted && (reversed ? sourceRank (visible[i - 1]) >= sourceRank (visible[i])
                                                     : sourceRank (visible[i - 1]) <= sourceRank (visible[i]));
                    return sorted;
                };
                using C = MatrixRow::Columns;
                const auto headingY = 6.0f + 32.0f + 9.0f;
                const auto grouped = sortedBySource (false);
                clickAt ({ 12.0f + (float) (C::number + C::bypass + C::gap * 2) + 20.0f, headingY });
                const auto bySource = grouped && sortedBySource (true);
                clickAt ({ 20.0f, headingY });
                auto bySlot = true;
                {
                    const auto visible = shownRows();
                    for (size_t i = 1; i < visible.size(); ++i)
                        bySlot = bySlot && visible[i - 1]->getSlotIndex() < visible[i]->getSlotIndex();
                }
                expect (bySource && bySlot, "the matrix opens grouped by source, SOURCE reverses it, # goes back to slot order");
                // (Back to the opening order for the tests after.)
                clickAt ({ 12.0f + (float) (C::number + C::bypass + C::gap * 2) + 20.0f, headingY });

                // The remap editor opens in the dock under the rows: no row
                // moves, none is covered, and its X closes it (V6-22).
                const auto visible = shownRows();
                if (visible.size() >= 3)
                {
                    auto* row = visible[1];
                    std::vector<int> rowsY;
                    for (auto* other : visible)
                        rowsY.push_back (other->getY());
                    row->getCurve().openRemapEditor();
                    settle (200);
                    auto* remap = findChild<RemapEditor> (*page);
                    auto covers = 0, moved = 0;
                    if (remap != nullptr)
                        for (auto* other : shownRows())
                            if (other->isShowing() || true)
                            {
                                const auto rowArea = page->getLocalArea (other, other->getLocalBounds());
                                const auto visibleArea = rowArea.getIntersection (page->getLocalArea (other->getParentComponent()->getParentComponent(),
                                                                                                      other->getParentComponent()->getParentComponent()->getLocalBounds()));
                                if (! visibleArea.isEmpty() && visibleArea.intersects (page->getLocalArea (remap, remap->getLocalBounds())))
                                    ++covers;
                            }
                    const auto after = shownRows();
                    for (size_t i = 0; i < after.size() && i < rowsY.size(); ++i)
                        moved += after[i]->getY() != rowsY[i] ? 1 : 0;
                    const auto docked = remap != nullptr && remap->getParentComponent() == page
                                        && page->getLocalArea (remap, remap->getLocalBounds()).getBottom() >= page->getHeight() - 20;
                    expect (remap != nullptr && covers == 0 && moved == 0 && docked,
                            "the remap editor opens in the dock under the rows, covering none (" + juce::String (covers)
                                + ") and moving none (" + juce::String (moved) + ")");
                    if (remap != nullptr)
                    {
                        remap->createComponentSnapshot (remap->getLocalBounds());
                        expect (remap->getLiveInput() >= 0.0f, "the remap editor shows the live input on the curve");
                        std::vector<juce::TextButton*> tools;
                        findAll<juce::TextButton> (*remap, tools);
                        juce::TextButton* close = nullptr;
                        auto shapes = false;
                        for (auto* button : tools)
                        {
                            close = button->getButtonText() == juce::String::fromUTF8 ("\xc3\x97") && button->isVisible() ? button : close;
                            shapes = shapes || (button->getButtonText().startsWith ("SHAPES") && button->isVisible());
                        }
                        expect (close != nullptr && shapes, "the remap editor has a close button and a shapes button");
                        if (close != nullptr)
                        {
                            close->triggerClick();
                            settle (200);
                            expect (findChild<RemapEditor> (*page) == nullptr, "the remap editor's X closes it");
                        }
                    }
                }
            }

            // MIDI learn on any automatable knob: the next CC drives it, and
            // the map is saved with the patch.
            {
                processor.startParamLearn ("f1_cutoff");
                const auto learning = processor.getParamLearnTarget() == "f1_cutoff";
                juce::AudioBuffer<float> buffer (2, 512);
                juce::MidiBuffer midi;
                midi.addEvent (juce::MidiMessage::controllerEvent (1, 74, 100), 0);
                processor.processBlock (buffer, midi);
                settle (100);
                auto* cutoff = processor.apvts.getParameter ("f1_cutoff");
                const auto mapped = processor.getParamCc ("f1_cutoff") == 74 && processor.getParamLearnTarget().isEmpty();
                const auto moved = cutoff != nullptr && std::abs (cutoff->getValue() - 100.0f / 127.0f) < 0.01f;
                expect (learning && mapped && moved, "MIDI learn on a knob maps the next CC (74) and the CC moves it");

                juce::MemoryBlock saved;
                processor.getStateInformation (saved);
                processor.clearParamCc ("f1_cutoff");
                const auto cleared = processor.getParamCc ("f1_cutoff") < 0;
                processor.setStateInformation (saved.getData(), (int) saved.getSize());
                settle (100);
                expect (cleared && processor.getParamCc ("f1_cutoff") == 74, "a learned CC is saved and restored with the patch");
                processor.clearParamCc ("f1_cutoff");
                processor.loadFactoryPreset (neuroWobble);
                settle (200);
            }
        }

        // The macro names the browser lists match what the preset loads with.
        {
            const auto listed = processor.getFactoryMacroNames (neuroWobble);
            auto matches = true;
            for (const auto& name : listed)
            {
                auto found = false;
                for (int macro = 0; macro < 4; ++macro)
                    found = found || processor.getMacroName (macro) == name;
                matches = matches && found;
            }
            expect (! listed.isEmpty() && matches, "the browser's macro names for Neuro Wobble are the ones it loads ("
                                                       + listed.joinIntoString (", ") + ")");
        }

        // Worked out without loading, the list's macro names equal what every factory preset loads with.
        {
            juce::StringArray mismatches;
            for (int index = 0; index < names.size(); ++index)
            {
                processor.loadFactoryPreset (index);
                const auto listed = processor.getFactoryMacroNames (index);
                for (int macro = 0; macro < 4; ++macro)
                {
                    const auto loaded = processor.apvts.state.getProperty ("macroName" + juce::String (macro + 1)).toString();
                    if (loaded != listed[macro])
                        mismatches.add (names[index] + " #" + juce::String (macro + 1) + " listed '" + listed[macro] + "' loads '" + loaded + "'");
                }
            }
            expect (mismatches.isEmpty(), "the browser lists every factory preset's macro names as loaded ("
                                              + juce::String (names.size()) + " presets"
                                              + (mismatches.isEmpty() ? juce::String() : "; " + juce::String (mismatches.size()) + " differ, e.g. " + mismatches[0]) + ")");
            for (const auto& line : mismatches)
                std::cout << "  " << line << std::endl;
            processor.loadFactoryPreset (neuroWobble);
        }

        // Up / down in the open browser load the next / previous preset and leave it open.
        if (auto* display = findChild<PresetDisplay> (*editor); display != nullptr && display->onClick != nullptr)
        {
            display->onClick();
            settle (500);
            auto* panel = findChild<PresetPanel> (*editor);
            expect (panel != nullptr && panel->isOpen(), "the preset browser opens");

            if (panel != nullptr)
            {
                const auto start = processor.getCurrentPresetName();
                panel->keyPressed (juce::KeyPress (juce::KeyPress::downKey));
                settle (150);
                const auto next = processor.getCurrentPresetName();
                expect (next.isNotEmpty() && next != start && panel->isOpen(), "Down loads the next preset and keeps the browser open");
                panel->keyPressed (juce::KeyPress (juce::KeyPress::upKey));
                settle (150);
                expect (processor.getCurrentPresetName() == start && panel->isOpen(), "Up steps back to the preset before");
            }

            display->onClick();
            settle (500);
        }

        // SUB + NOISE keeps its slot whatever is on (nothing folds: UI
        // review 6, V7, V19); the sub's controls follow its switch.
        {
            pages->showPage ("MAIN");
            const auto set = [&] (const char* id, float value)
            {
                if (auto* parameter = processor.apvts.getParameter (id))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
            };
            const auto shown = [&] (const juce::String& id) -> KnobControl*
            {
                std::vector<KnobControl*> knobs;
                findAll<KnobControl> (*editor, knobs);
                for (auto* knob : knobs)
                    if (visibleInTree (knob) && knob->getParameterId() == id)
                        return knob;
                return nullptr;
            };

            processor.loadFactoryPreset (0);
            set ("subosc_on", 0.0f);
            set ("noise_level", 0.0f);
            settle (600);
            auto* subLevel = shown ("subosc_level");
            expect (shown ("noise_level") != nullptr && subLevel != nullptr && subLevel->getAlpha() < 0.99f,
                    "SUB and NOISE cards stay in place while both are off, the sub's controls dimmed");
            set ("subosc_on", 1.0f);
            settle (600);
            subLevel = shown ("subosc_level");
            expect (subLevel != nullptr && subLevel->getAlpha() > 0.99f, "switching the sub on lights its controls");
            set ("subosc_on", 0.0f);
            settle (300);
        }

        // Dragging the filter graph: across for cutoff, up and down for resonance, one gesture each.
        if (auto* display = findChild<FilterDisplay> (*editor); display != nullptr)
        {
            struct GestureCounter : juce::AudioProcessorParameter::Listener
            {
                int begins = 0, ends = 0;
                void parameterValueChanged (int, float) override {}
                void parameterGestureChanged (int, bool starting) override { (starting ? begins : ends)++; }
            } cutoffGestures, resoGestures;

            processor.loadFactoryPreset (0);
            pages->showPage ("MAIN");
            settle (400);
            auto* cutoff = processor.apvts.getParameter ("f1_cutoff");
            auto* reso = processor.apvts.getParameter ("f1_reso");
            cutoff->addListener (&cutoffGestures);
            reso->addListener (&resoGestures);
            const auto before = processor.apvts.getRawParameterValue ("f1_cutoff")->load();
            const auto resoBefore = processor.apvts.getRawParameterValue ("f1_reso")->load();
            const auto w = (float) display->getWidth();
            const auto h = (float) display->getHeight();
            auto* component = static_cast<juce::Component*> (display);
            clearHistory();
            component->mouseDown (makeEvent (*component, { 10.0f + 0.5f * (w - 20.0f), 0.5f * h }, false));
            component->mouseDrag (makeEvent (*component, { 10.0f + 0.3f * (w - 20.0f), 0.2f * h }, true));
            component->mouseUp (makeEvent (*component, { 10.0f + 0.3f * (w - 20.0f), 0.2f * h }, true));
            const auto after = processor.apvts.getRawParameterValue ("f1_cutoff")->load();
            const auto resoAfter = processor.apvts.getRawParameterValue ("f1_reso")->load();
            {
                settle (100);
                const auto steps = undoSteps();
                processor.getUndoManager().undo();
                const auto undone = processor.apvts.getRawParameterValue ("f1_cutoff")->load();
                processor.getUndoManager().redo();
                expect (steps.size() == 1 && steps[0] == "Filter 1 graph" && std::abs (undone / before - 1.0f) < 0.01f
                            && std::abs (processor.apvts.getRawParameterValue ("f1_cutoff")->load() / after - 1.0f) < 0.01f,
                        "the filter graph drag is one undo step ('" + steps.joinIntoString ("', '") + "'); undo and redo move the cutoff");
            }
            expect (before > 5000.0f && std::abs (after / 159.0f - 1.0f) < 0.15f,
                    "dragging across the filter graph sets the cutoff (" + juce::String (before, 0) + " -> " + juce::String (after, 0) + " Hz)");
            expect (resoAfter > resoBefore + 0.1f, "dragging up on the filter graph raises the resonance");
            expect (cutoffGestures.begins == 1 && cutoffGestures.ends == 1 && resoGestures.begins == 1 && resoGestures.ends == 1,
                    "the filter drag is one gesture per parameter");
            cutoff->removeListener (&cutoffGestures);
            reso->removeListener (&resoGestures);
            processor.loadFactoryPreset (0);
            settle (200);
        }

        // The oscillator display: across scrubs the frame, up raises the warp, one gesture each; the corner key cycles WAVE / 3D / SPEC.
        {
            processor.loadFactoryPreset (0);
            pages->showPage ("MAIN");
            const auto set = [&] (const char* id, float value)
            {
                if (auto* parameter = processor.apvts.getParameter (id))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
            };
            set ("osc1_warp", 2.0f); // Bend +
            set ("osc1_warp_amt", 0.2f);
            settle (400);

            std::vector<WaveDisplay*> waves;
            findAll<WaveDisplay> (*editor, waves);
            WaveDisplay* wave = nullptr;
            for (auto* candidate : waves)
                if (visibleInTree (candidate) && candidate->getOscIndex() == 0)
                    wave = candidate;
            expect (wave != nullptr, "PLAY shows OSC 1's display");

            if (wave != nullptr)
            {
                struct GestureCounter : juce::AudioProcessorParameter::Listener
                {
                    int begins = 0, ends = 0;
                    void parameterValueChanged (int, float) override {}
                    void parameterGestureChanged (int, bool starting) override { (starting ? begins : ends)++; }
                } frameGestures, warpGestures;

                auto* frame = processor.apvts.getParameter ("osc1_frame");
                auto* warp = processor.apvts.getParameter ("osc1_warp_amt");
                frame->addListener (&frameGestures);
                warp->addListener (&warpGestures);
                const auto w = (float) wave->getWidth();
                const auto h = (float) wave->getHeight();
                auto* component = static_cast<juce::Component*> (wave);
                component->mouseDown (makeEvent (*component, { 10.0f + 0.25f * (w - 20.0f), 0.6f * h }, false));
                component->mouseDrag (makeEvent (*component, { 10.0f + 0.75f * (w - 20.0f), 0.2f * h }, true));
                const auto frameAfter = frame->getValue();
                const auto warpAfter = processor.apvts.getRawParameterValue ("osc1_warp_amt")->load();
                expect (wave->isDraggingWarp(), "a drag on a warped table moves the warp");
                component->mouseUp (makeEvent (*component, { 10.0f + 0.75f * (w - 20.0f), 0.2f * h }, true));
                expect (std::abs (frameAfter - 0.75f) < 0.03f, "dragging across the oscillator display scrubs the frame (" + juce::String (frameAfter, 2) + ")");
                expect (warpAfter > 0.3f, "dragging up on the oscillator display raises the warp amount (0.20 -> " + juce::String (warpAfter, 2) + ")");
                expect (frameGestures.begins == 1 && frameGestures.ends == 1 && warpGestures.begins == 1 && warpGestures.ends == 1,
                        "the oscillator drag is one gesture per parameter");
                component->mouseDoubleClick (makeEvent (*component, { 20.0f, 20.0f }, false));
                expect (processor.apvts.getRawParameterValue ("osc1_warp_amt")->load() < 0.001f, "a double-click sets the warp back to zero");
                frame->removeListener (&frameGestures);
                warp->removeListener (&warpGestures);

                // With no warp chosen a vertical drag leaves the amount alone.
                set ("osc1_warp", 0.0f);
                set ("osc1_warp_amt", 0.4f);
                settle (100);
                component->mouseDown (makeEvent (*component, { 30.0f, 0.8f * h }, false));
                component->mouseDrag (makeEvent (*component, { 30.0f, 0.1f * h }, true));
                component->mouseUp (makeEvent (*component, { 30.0f, 0.1f * h }, true));
                expect (std::abs (processor.apvts.getRawParameterValue ("osc1_warp_amt")->load() - 0.4f) < 0.001f,
                        "without a warp the vertical drag changes nothing");

            }

            // On OSC the display opens on the cycle (the design's 2D well), its views a WAVE | 3D | SPEC
            // control, its table named with arrows on it (UI review 5, V6,
            // V27; review 6, V21, V40).
            pages->showPage ("OSC");
            settle (300);
            waves.clear();
            findAll<WaveDisplay> (*editor, waves);
            wave = nullptr;
            for (auto* candidate : waves)
                if (visibleInTree (candidate) && candidate->getOscIndex() == 0)
                    wave = candidate;
            expect (wave != nullptr && wave->getViewMode() == 0, "OSC opens OSC 1's display on the cycle (WAVE), as the design draws it");

            if (wave != nullptr)
            {
                std::vector<juce::TextButton*> keys;
                findAll<juce::TextButton> (*wave, keys);
                const auto key = [&keys] (const juce::String& text) -> juce::TextButton*
                {
                    for (auto* button : keys)
                        if (button->getButtonText() == text && visibleInTree (button))
                            return button;
                    return nullptr;
                };
                // (OSC's card shows the display slim: its view chip and table arrows
                // are drawn on its top line, so the test clicks them there.)
                const auto clickSlim = [&] (int which)
                {
                    const auto at = wave->getSlimTarget (which);
                    const auto now = juce::Time::getCurrentTime();
                    const auto make = [&] (bool up)
                    {
                        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier,
                                                 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, wave, wave, now, at, now, 1, false);
                    };
                    static_cast<juce::Component*> (wave)->mouseDown (make (false));
                    static_cast<juce::Component*> (wave)->mouseUp (make (true));
                    settle (60);
                };

                juce::StringArray seen;
                auto spec = juce::String();
                if (wave->isSlim())
                {
                    // The chip cycles WAVE > 3D > SPEC > WAVE; it starts on WAVE.
                    for (int i = 0; i < 3; ++i)
                    {
                        clickSlim (0);
                        seen.add (juce::String (wave->getViewMode()));
                        if (wave->getViewMode() == 2)
                            spec = wave->getFrameReadout();
                    }
                    juce::StringArray sorted (seen);
                    sorted.sort (false);
                    expect (sorted.joinIntoString (",") == "0,1,2", "WAVE, 3D and SPEC each pick their view (" + seen.joinIntoString (",") + ")");
                    expect (spec.isEmpty(), "SPEC shows no frame readout");
                    while (wave->getViewMode() != 1)
                        clickSlim (0);
                }
                else
                {
                    for (const auto* name : { "WAVE", "3D", "SPEC" })
                        if (auto* button = key (name))
                        {
                            button->triggerClick();
                            settle (60);
                            seen.add (juce::String (wave->getViewMode()));
                        }
                    expect (seen.joinIntoString (",") == "0,1,2", "WAVE, 3D and SPEC each pick their view (" + seen.joinIntoString (",") + ")");
                    expect (wave->getFrameReadout().isEmpty(), "SPEC shows no frame readout");
                    if (auto* button = key ("3D"))
                        button->triggerClick();
                }
                settle (60);
                expect (wave->getFrameReadout().startsWith ("frame "), "3D reads its frame under the plot (" + wave->getFrameReadout() + ")");

                const auto table = [&processor] { return juce::roundToInt (processor.apvts.getRawParameterValue ("osc1_table")->load()); };
                const auto before = table();
                const auto nameBefore = wave->getTableName();
                if (wave->isSlim())
                    clickSlim (2);
                else if (auto* next = key (">"))
                    next->triggerClick();
                settle (100);
                const auto after = table();
                if (wave->isSlim())
                    clickSlim (1);
                else if (auto* previous = key ("<"))
                    previous->triggerClick();
                settle (100);
                expect (after != before && table() == before && wave->getTableName() == nameBefore && nameBefore.isNotEmpty(),
                        "the display's arrows step the table and back (" + nameBefore + ")");
            }

            processor.loadFactoryPreset (0);
            settle (200);
        }

        // A macro's arc on an OSC 1 knob (yellow on gold) is drawn paler; an unrelated hue is left alone.
        {
            const auto macro = modSourceColour ((int) Mod::Source::Macro1);
            const auto onGold = modArcColour (macro, IlanaTheme::oscColour (0));
            const auto onBlue = modArcColour (macro, IlanaTheme::oscColour (1));
            expect (onGold.getBrightness() > macro.getBrightness() + 0.02f || onGold.getSaturation() < macro.getSaturation() - 0.2f,
                    "a macro arc on an OSC 1 knob stands apart from its gold");
            expect (onBlue == macro, "a macro arc on an OSC 2 knob keeps the macro colour");
        }

        // Resting on a modulated knob opens a card listing its sources with live bars.
        {
            processor.loadFactoryPreset (neuroWobble);
            pages->showPage ("MAIN");
            settle (400);
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            KnobControl* modulated = nullptr;
            for (auto* knob : knobs)
                if (visibleInTree (knob) && knob->getNumRoutings() > 0)
                {
                    modulated = knob;
                    break;
                }
            expect (modulated != nullptr, "Neuro Wobble has a modulated knob on PLAY");
            auto* card = findChild<ModHoverPopup> (*editor);
            expect (card != nullptr, "the editor has the modulation card");

            if (modulated != nullptr && card != nullptr)
            {
                modulated->openModCard(); // (hover opens it after a short rest; the test has no mouse)
                const auto inside = card->getParentComponent() != nullptr
                                    && card->getParentComponent()->getLocalBounds().contains (card->getBounds());
                expect (card->isShowingFor (*modulated) && card->getNumRows() >= modulated->getNumRoutings() && inside,
                        "the card lists the knob's " + juce::String (modulated->getNumRoutings()) + " source(s) inside the window ("
                            + modulated->getParameterId() + ")");
                modulated->closeModCard();
                settle (60);
                expect (! card->isVisible(), "leaving the knob closes the card");

                // The card's rows are controls: drag for depth (one gesture), double-click for zero, right-click to bypass or remove.
                modulated->openModCard (true);
                const auto slot = card->getRowSlot (0);
                auto* depth = slot >= 0 ? processor.apvts.getParameter (processor.getModSlotParamId (slot, "amt")) : nullptr;
                expect (depth != nullptr, "the card's first row names its routing");

                if (depth != nullptr)
                {
                    struct GestureCounter : juce::AudioProcessorParameter::Listener
                    {
                        int begins = 0, ends = 0;
                        void parameterValueChanged (int, float) override {}
                        void parameterGestureChanged (int, bool starting) override { (starting ? begins : ends)++; }
                    } gestures;
                    depth->addListener (&gestures);

                    const auto row = card->getRowBounds (0).toFloat();
                    const auto from = row.getCentre();
                    const auto to = from.translated (25.0f, 0.0f);
                    const auto rowEvent = [&] (juce::Point<float> at, bool dragged, juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier)
                    {
                        const auto now = juce::Time::getCurrentTime();
                        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                 card, card, now, from, now, 1, dragged);
                    };
                    const auto read = [&] { return processor.readModSlot (slot); };
                    const auto start = read().depth;
                    // Away from the clamp, so the move shows.
                    processor.setModSlotValue (slot, "amt", 0.2f);
                    settle (80); // the card re-reads its rows each frame
                    auto& component = static_cast<juce::Component&> (*card);
                    clearHistory();
                    component.mouseDown (rowEvent (from, false));
                    component.mouseDrag (rowEvent (to, true));
                    component.mouseUp (rowEvent (to, true));
                    settle (50);
                    const auto dragSteps = undoSteps();
                    expect (dragSteps.size() == 1 && dragSteps[0].endsWith (" depth"),
                            "a card row drag is one undo step ('" + dragSteps.joinIntoString ("', '") + "')");
                    expect (std::abs (read().depth - 0.35f) < 0.02f && gestures.begins == 1 && gestures.ends == 1,
                            "dragging a card row sets its depth in one gesture (0.20 -> " + juce::String (read().depth, 2) + ")");
                    component.mouseDoubleClick (rowEvent (from, false));
                    expect (std::abs (read().depth) < 0.001f, "double-clicking a card row zeroes its depth");
                    depth->removeListener (&gestures);
                    processor.setModSlotValue (slot, "amt", start);

                    card->applyRowAction (0, ModHoverPopup::RowAction::toggleBypass);
                    expect (read().bypass, "the row menu bypasses the routing");
                    card->applyRowAction (0, ModHoverPopup::RowAction::toggleBypass);
                    expect (! read().bypass, "and turns it back on");

                    const auto rowsBefore = card->getNumRows();
                    const auto sourceBefore = read().source;
                    clearHistory();
                    card->applyRowAction (0, ModHoverPopup::RowAction::remove);
                    expect (read().source == Mod::Source::None && (card->getNumRows() == rowsBefore - 1 || ! card->isVisible()),
                            "the row menu removes the routing and the card follows");
                    const auto removeSteps = undoSteps();
                    processor.getUndoManager().undo();
                    expect (removeSteps.size() == 1 && removeSteps[0] == "Remove modulation" && read().source == sourceBefore,
                            "the card's Remove is one undo step and undo restores the routing");
                }

                modulated->closeModCard();
                processor.loadFactoryPreset (neuroWobble);
                settle (200);
            }
        }

        // DOCK opens the browser over the page (UI review 6: inside the
        // window, which keeps its size), loading doesn't close it, the name
        // toggles it, FLOAT returns it.
        if (auto* display = findChild<PresetDisplay> (*editor); display != nullptr && display->onClick != nullptr)
        {
            processor.loadFactoryPreset (0);
            const auto baseWidth = editor->getWidth();
            display->onClick();
            settle (400);
            auto* panel = findChild<PresetPanel> (*editor);
            expect (panel != nullptr && panel->onDockRequest != nullptr, "the browser has a DOCK key");

            if (panel != nullptr && panel->onDockRequest != nullptr)
            {
                panel->onDockRequest (true);
                settle (300);
                expect (panel->isDocked() && panel->isVisible() && editor->getWidth() == baseWidth && editor->getHeight() == 720
                            && panel->getWidth() >= 1000 && panel->getY() > 60 && panel->getBottom() <= 720,
                        "docking opens the browser over the page, the window keeps its size (" + juce::String (baseWidth) + " -> "
                            + juce::String (editor->getWidth()) + "x" + juce::String (editor->getHeight()) + ", panel " + panel->getBounds().toString()
                            + (panel->isDocked() ? " docked" : " floating") + (panel->isVisible() ? " shown)" : " hidden)"));
                const auto start = processor.getCurrentPresetName();
                panel->keyPressed (juce::KeyPress (juce::KeyPress::downKey));
                settle (150);
                panel->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
                settle (150);
                expect (processor.getCurrentPresetName() != start && panel->isVisible(),
                        "the docked browser loads with the arrows and stays open on Enter");
                display->onClick();
                settle (300);
                expect (! panel->isVisible() && editor->getWidth() == baseWidth, "the preset name closes the docked browser");
                display->onClick();
                settle (300);
                expect (panel->isVisible() && panel->isDocked() && editor->getWidth() == baseWidth, "the preset name reopens it docked");
                panel->onDockRequest (false);
                settle (400);
                expect (! panel->isDocked() && panel->isOpen() && editor->getWidth() == baseWidth, "FLOAT returns it to the drop-down");
                panel->close();
                settle (400);
            }
        }

        // UI review 4 (V3, V15, V25, V31, S11, S28, S29): SAVE / SAVE AS, the
        // browser's sort, DX7 banks, stars and tags, the keys and the table
        // search. Saves go to a temporary folder, never the user's presets.
        {
            const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile ("ilanaUiTestPresets-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
            tempDir.createDirectory();
            IlanaSynthAudioProcessor::userPresetDirectoryOverride = tempDir;
            const auto countFiles = [&tempDir] { return tempDir.findChildFiles (juce::File::findFiles, true, "*.ilanapreset").size(); };
            const auto setCutoff = [&processor] (float hz)
            {
                if (auto* parameter = processor.apvts.getParameter ("f1_cutoff"))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (hz));
            };
            const auto cutoffIn = [&processor] (const juce::File& file)
            {
                IlanaSynthAudioProcessor other;
                other.loadPresetFromFile (file);
                return other.apvts.getRawParameterValue ("f1_cutoff")->load();
            };

            processor.loadFactoryPreset (neuroWobble);
            settle (200);
            auto& saveOverlay = pages->getSaveOverlay();
            pages->savePreset();
            settle (100);
            expect (saveOverlay.isShowing(), "SAVE on a factory preset opens the SAVE AS panel");

            saveOverlay.getNameField().setText (juce::String::fromUTF8 ("Rock'n (Roll): \xc3\xb1/1?"), true);
            settle (100);
            // V12-20: nothing warns after the fact; text that did not come through typing is cleaned in place.
            expect (! saveOverlay.getNameField().getText().containsAnyOf (":/?") && saveOverlay.getNote().isEmpty(),
                    "SAVE AS cleans a name that holds characters a file name can't, quietly (" + saveOverlay.getNameField().getText() + ")");
            saveOverlay.getNameField().clear();
            saveOverlay.getNameField().insertTextAtCaret ("A:B");
            settle (60);
            expect (saveOverlay.getNameField().getText() == "AB", "a character a file name can't hold never gets into the name field as typed");
            saveOverlay.getNameField().setText (juce::String::fromUTF8 ("Rock'n (Roll): \xc3\xb1/1?"), true);
            settle (60);
            const auto suggested = saveOverlay.getSuggestedTags();
            expect (suggested.size() >= 3, "SAVE AS suggests tags to tick (" + suggested.joinIntoString (", ") + ")");
            if (! suggested.isEmpty())
            {
                saveOverlay.toggleSuggestedTag (suggested[0]);
                expect (saveOverlay.getTagsField().getText().contains (suggested[0]), "a suggested tag goes into TAGS when clicked");
                saveOverlay.toggleSuggestedTag (suggested[0]);
                expect (! saveOverlay.getTagsField().getText().contains (suggested[0]), "and comes out when clicked again");
            }
            saveOverlay.getTagsField().setText ("dark, wide", false);
            saveOverlay.getAuthorField().setText ("Test Author", false);
            saveOverlay.getCommentField().setText ("A note about the sound.", false);
            setCutoff (900.0f);
            saveOverlay.save();
            settle (100);
            const auto savedName = juce::String::fromUTF8 ("Rock'n (Roll) \xc3\xb1" "1");
            const auto savedFile = tempDir.getChildFile (savedName + ".ilanapreset");
            expect (! saveOverlay.isShowing() && savedFile.existsAsFile() && processor.getCurrentPresetName() == savedName,
                    "SAVE AS keeps apostrophes, brackets and accents, dropping only : / ? (" + processor.getCurrentPresetName() + ")");
            {
                const auto info = processor.getPresetInfo (processor.getAllPresetNames().indexOf (savedName));
                expect (info.author == "Test Author" && info.comment == "A note about the sound.",
                        "SAVE AS stores the author and the comment (" + info.author + ", " + info.comment + ")");
            }
            expect (! pages->isPatchEdited(), "a saved patch is no longer EDITED");

            setCutoff (2400.0f);
            pages->savePreset();
            settle (100);
            auto* display = findChild<PresetDisplay> (*editor);
            expect (! saveOverlay.isShowing() && countFiles() == 1 && std::abs (cutoffIn (savedFile) - 2400.0f) < 5.0f,
                    "SAVE overwrites the loaded user preset in place, with no dialog");
            expect (display != nullptr && display->getNotice() == "SAVED", "SAVE says SAVED by the preset name");

            setCutoff (3100.0f);
            editor->keyPressed (juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0));
            settle (100);
            expect (! saveOverlay.isShowing() && countFiles() == 1 && std::abs (cutoffIn (savedFile) - 3100.0f) < 5.0f, "Ctrl+S saves in place");

            editor->keyPressed (juce::KeyPress ('s', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0));
            settle (100);
            expect (saveOverlay.isShowing(), "Ctrl+Shift+S opens SAVE AS");
            saveOverlay.getNameField().setText (savedName, true);
            setCutoff (500.0f);
            saveOverlay.save();
            expect (saveOverlay.isAskingOverwrite() && countFiles() == 1, "SAVE AS asks before overwriting a name in use");
            saveOverlay.finishOverwrite (false);
            expect (saveOverlay.isShowing() && ! saveOverlay.isAskingOverwrite() && std::abs (cutoffIn (savedFile) - 3100.0f) < 5.0f,
                    "Cancel keeps the old preset and goes back to the name");
            saveOverlay.save();
            saveOverlay.finishOverwrite (true);
            settle (100);
            expect (! saveOverlay.isShowing() && countFiles() == 1 && std::abs (cutoffIn (savedFile) - 500.0f) < 5.0f,
                    "Overwrite replaces it, with no \"Name 2\" copy");

            pages->savePresetAs();
            saveOverlay.getNameField().setText ("neuro wobble", true);
            saveOverlay.save();
            expect (saveOverlay.isShowing() && ! saveOverlay.isAskingOverwrite() && saveOverlay.getNote().contains ("factory") && countFiles() == 1,
                    "a factory preset's name can't be taken");
            editor->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
            expect (! saveOverlay.isShowing(), "Esc closes the SAVE AS panel");

            // Ctrl+Right / Left step through the presets, through the
            // confirm over an edited patch.
            processor.loadFactoryPreset (neuroWobble);
            settle (200);
            editor->keyPressed (juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::commandModifier, 0));
            settle (150);
            const auto afterRight = processor.getCurrentPresetName();
            editor->keyPressed (juce::KeyPress (juce::KeyPress::leftKey, juce::ModifierKeys::commandModifier, 0));
            settle (150);
            expect (afterRight != "Neuro Wobble" && processor.getCurrentPresetName() == "Neuro Wobble",
                    "Ctrl+Right / Ctrl+Left load the next and previous preset (" + afterRight + ")");
            pages->setAsksBeforeReplacingEdits (true);
            setCutoff (700.0f);
            settle (100);
            editor->keyPressed (juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::commandModifier, 0));
            settle (100);
            auto* confirm = findChild<ConfirmOverlay> (*editor);
            expect (confirm != nullptr && confirm->isAsking() && processor.getCurrentPresetName() == "Neuro Wobble",
                    "Ctrl+Right over an edited patch asks first");
            if (confirm != nullptr && confirm->isAsking())
                confirm->finish (false);
            pages->setAsksBeforeReplacingEdits (false);
            processor.loadFactoryPreset (neuroWobble);
            settle (150);

            // Ctrl/Cmd + 1-9 pick tabs (UI review 6: bare digits are left
            // to the host and the keyboard); there is no tenth tab for 0.
            if (auto* tabbed = findChild<juce::TabbedComponent> (*editor))
            {
                const auto before = tabbed->getCurrentTabIndex();
                tabbed->setCurrentTabIndex (0);
                const auto bareTaken = editor->keyPressed (juce::KeyPress ('2'));
                const auto onBare = tabbed->getCurrentTabIndex();
                editor->keyPressed (juce::KeyPress ('2', juce::ModifierKeys::commandModifier, 0));
                const auto onTwo = tabbed->getCurrentTabIndex();
                expect (! bareTaken && onBare == 0 && onTwo == 1
                            && ! editor->keyPressed (juce::KeyPress ('0', juce::ModifierKeys::commandModifier, 0))
                            && tabbed->getCurrentTabIndex() == 1,
                        "Ctrl+2 picks the second tab, a bare 2 and Ctrl+0 do nothing");
                tabbed->setCurrentTabIndex (before);
                settle (150);
            }

            // Esc closes the drop-down browser.
            if (display != nullptr && display->onClick != nullptr)
            {
                display->onClick();
                settle (400);
                auto* panel = findChild<PresetPanel> (*editor);
                expect (panel != nullptr && panel->isOpen(), "the browser opens for the browser checks");

                if (panel != nullptr)
                {
                    const auto sortBefore = panel->getSortMode();
                    panel->setSortMode (PresetPanel::sortByName);
                    panel->selectFilter ("");
                    auto listed = panel->getListedNames();
                    auto sorted = true;
                    const auto listedShown = panel->getListedDisplayNames();
                    for (int i = 1; i < listedShown.size(); ++i)
                        sorted = sorted && listedShown[i - 1].compareNatural (listedShown[i], false) <= 0;
                    auto dx7Shown = 0;
                    for (const auto& name : listed)
                        dx7Shown += name.endsWith ("(ROM1A)") || name.endsWith ("(DEXED01)") ? 1 : 0;
                    expect (sorted && listed.size() > 100, "the browser sorts by the names it shows (" + listedShown[0] + ", " + listedShown[1] + ", "
                                                              + listedShown[2] + "...)");
                    expect (dx7Shown >= 64 && listed.contains (savedName) && ! panel->getSidebarKeys().contains ("DX7"),
                            "All means all: the DX7 voices are in it, with no DX7 folder (" + juce::String (listed.size()) + " listed)");

                    // UI review 6: the DX7 chip narrows to the voices, a chip
                    // per bank; names show in Title Case without the bank.
                    auto chipKeys = panel->getChipKeys();
                    expect (chipKeys.contains ("pack:dx7") && ! chipKeys.contains ("bank:ROM1A") && ! chipKeys.contains ("tag:DX7"),
                            "the DX7 chip leads the chip row, banks folded away (" + chipKeys.joinIntoString (" ").substring (0, 120) + ")");
                    panel->clickChip ("pack:dx7");
                    chipKeys = panel->getChipKeys();
                    expect (panel->getListedNames().size() >= 270 && chipKeys.contains ("bank:ROM1A") && chipKeys.contains ("bank:DEXED01"),
                            "the DX7 chip lists the voices with a chip per bank (" + juce::String (panel->getListedNames().size()) + ")");

                    // UI review 7 (S7-10, I7-41, V7-25): a voice a later
                    // cartridge repeats is listed once, with "also in", and
                    // under its own bank's chip; DX7 rows drop the FM tag and
                    // their bank tag while a bank chip is on; the tags get a
                    // line of their own.
                    {
                        const auto all = panel->getListedNames();
                        const auto original = all.indexOf ("ORCHESTRA (ROM1A)");
                        const auto tip = panel->getRowTooltip (original);
                        expect (original >= 0 && ! all.contains ("ORCHESTRA (ROM4B)") && tip.contains ("Also in ROM4B")
                                    && panel->getRowTooltip (all.indexOf ("BRASS 2 (ROM1A)")).contains ("ROM4A (as Synth Brass)"),
                                "a repeated DX7 voice is listed once, with the bank that repeats it (" + tip.fromLastOccurrenceOf ("\n", false, false) + ")");
                        auto fmTagged = 0;
                        for (int row = 0; row < juce::jmin (40, all.size()); ++row)
                            fmTagged += panel->getRowTags (row).contains ("FM") ? 1 : 0;
                        expect (fmTagged == 0, "DX7 rows carry no FM tag (" + juce::String (fmTagged) + " of 40 do)");
                        panel->clickChip ("bank:ROM4B");
                        const auto rom4b = panel->getListedNames();
                        expect (rom4b.size() == 32 && rom4b.contains ("ORCHESTRA (ROM4B)"), "the ROM4B chip lists all its 32 voices, repeats included");
                        panel->clickChip ("bank:ROM4B");
                        expect (Presets::dx7DisplayName ("HARPSICH 1 (ROM1A)") == "Harpsichord 1"
                                    && Presets::dx7DisplayName ("JAZZ GUIT1 (ROM1B)") == "Jazz Guitar 1"
                                    && Presets::dx7DisplayName ("STRG ENS 1 (ROM2A)") == "Strings Ensemble 1"
                                    && Presets::dx7DisplayName ("E.P-BRS BC (ROM3B)") == "E.Piano-Brass BC"
                                    && Presets::dx7DisplayName ("PIANO 5THS (ROM3A)") == "Piano 5ths",
                                "DX7 names spell out the cartridge's cuts (" + Presets::dx7DisplayName ("HARPSICH 1 (ROM1A)") + ", "
                                    + Presets::dx7DisplayName ("JAZZ GUIT1 (ROM1B)") + ", " + Presets::dx7DisplayName ("E.P-BRS BC (ROM3B)") + ")");
                        const auto categories = processor.getAllPresetCategories();
                        const auto allNames = processor.getAllPresetNames();
                        const auto factoryNames = processor.getFactoryPresetNames();
                        const auto macros = [&] (const juce::String& name) { return processor.getFactoryMacroNames (factoryNames.indexOf (name)).joinIntoString (" "); };
                        expect (categories[allNames.indexOf ("BRASS 1 (ROM1A)")] == "Brass" && categories[allNames.indexOf ("FLUTE 1 (ROM1A)")] == "Wind"
                                    && panel->getSidebarKeys().contains ("Brass") && panel->getSidebarKeys().contains ("Wind"),
                                "brass and wind DX7 voices have their own categories");
                        expect (macros ("E.PIANO 1 (ROM1A)") == "BARK DARKEN WOBBLE ROOM" && macros ("BASS 1 (ROM1A)") == "GROWL DARKEN DETUNE ROOM",
                                "DX7 voices name their macros for their kind of sound (" + macros ("E.PIANO 1 (ROM1A)") + " / "
                                    + macros ("BASS 1 (ROM1A)") + ")");
                    }
                    panel->clickChip ("bank:ROM1B");
                    listed = panel->getListedNames();
                    const auto shown = panel->getListedDisplayNames();
                    auto allRom1B = listed.size() == 32;
                    for (const auto& name : listed)
                        allRom1B = allRom1B && name.endsWith ("(ROM1B)");
                    expect (allRom1B, "the ROM1B chip lists that bank's 32 voices");
                    const auto piano = listed.indexOf ("E.PIANO 1 (ROM1B)") >= 0 ? listed.indexOf ("E.PIANO 1 (ROM1B)") : 0;
                    expect (shown.joinIntoString ("|").indexOf ("(ROM1B)") < 0 && shown[piano] == Presets::dx7DisplayName (listed[piano])
                                && Presets::dx7DisplayName ("E.PIANO 1 (ROM1A)") == "E.Piano 1"
                                && Presets::dx7DisplayName ("SYN-LEAD 1 (ROM1A)") == "Syn-Lead 1",
                            "DX7 names show in Title Case without the bank (" + shown[0] + ", " + shown[1] + ")");
                    panel->clickChip ("bank:ROM1B");
                    panel->clickChip ("pack:dx7");
                    expect (panel->getListedNames().size() > 600, "the DX7 chip clicked again shows everything");

                    // DX7 voices are filed by sound, beside the factory sounds.
                    panel->selectFilter ("Keys");
                    listed = panel->getListedNames();
                    expect (listed.contains ("E.PIANO 1 (ROM1A)") && listed.contains ("Init") == false,
                            "the DX7 voices are filed by sound (E.Piano 1 under Keys)");
                    panel->selectFilter ("Bass");
                    expect (panel->getListedNames().contains ("PLUCK BASS (ROM3B)"), "PLUCK BASS is filed under Bass");
                    panel->selectFilter ("FX");
                    expect (panel->getListedNames().contains ("GRAND PRIX (ROM3A)") || ! processor.getAllPresetNames().contains ("GRAND PRIX (ROM3A)"),
                            "GRAND PRIX is filed under FX");

                    // Every factory preset carries 3-6 descriptive tags, shown
                    // on its row and as chips; macro names only on hover.
                    {
                        const auto tags = processor.getAllPresetTags();
                        const auto factoryCount = processor.getFactoryPresetNames().size();
                        auto tagged = 0;
                        juce::String untagged;
                        for (int i = 0; i < factoryCount; ++i)
                        {
                            const auto count = juce::StringArray::fromTokens (tags[i], ",", "").size();
                            if (count >= 3 && count <= 6)
                                ++tagged;
                            else if (untagged.isEmpty())
                                untagged = processor.getAllPresetNames()[i] + " (" + tags[i] + ")";
                        }
                        expect (tagged == factoryCount, "every factory preset has 3-6 tags (" + juce::String (tagged) + "/"
                                                            + juce::String (factoryCount) + " " + untagged + ")");

                        panel->selectFilter ("");
                        chipKeys = panel->getChipKeys();
                        expect (chipKeys.contains ("tag:Bright") && chipKeys.contains ("tag:FM"), "the tags show as chips");
                        panel->clickChip ("tag:Bell");
                        listed = panel->getListedNames();
                        auto allBells = listed.size() > 10;
                        for (const auto& name : listed)
                            allBells = allBells && tags[processor.getAllPresetNames().indexOf (name)].contains ("Bell");
                        expect (allBells, "a tag chip lists the presets with that tag (" + juce::String (listed.size()) + " bells)");
                        expect (panel->getRowTags (0).contains ("Bell") || panel->getRowTags (0).size() > 0,
                                "a row shows its tags (" + panel->getRowTags (0).joinIntoString (", ") + ")");
                        panel->clickChip ("tag:Bell");
                        const auto rowTip = panel->getRowTooltip (panel->getListedNames().indexOf ("Rip Bass"));
                        expect (rowTip.contains ("Macros:") && rowTip.contains ("TONE"), "the macro names show on hover: " + rowTip.replace ("\n", " / "));
                    }

                    // The list shows whole rows only: no clipped last row.
                    expect (panel->getListBounds().getHeight() % panel->getListRowHeight() == 0,
                            "the list ends on a whole row (" + juce::String (panel->getListBounds().getHeight()) + " px, rows of "
                                + juce::String (panel->getListRowHeight()) + ")");

                    // DELETE says why it can't delete a factory preset.
                    panel->selectFilter ("");
                    expect (! panel->isDeleteEnabled() && panel->getDeleteTooltip().containsIgnoreCase ("factory"),
                            "DELETE explains itself on a factory preset: " + panel->getDeleteTooltip());

                    // S15-10: the caption says why too, and DOCK is as tall as the bottom buttons.
                    expect (panel->getAuditionCaption().containsIgnoreCase ("factory"), "DELETE's reason is written in the details: " + panel->getAuditionCaption());
                    expect (panel->getDockBounds().getHeight() == panel->getFooterButtonBounds().getHeight(),
                            "DOCK is the height of the bottom buttons (" + juce::String (panel->getDockBounds().getHeight()) + " vs "
                                + juce::String (panel->getFooterButtonBounds().getHeight()) + ")");

                    // S15-1: the audition sounds C3 and stops by itself.
                    if (! panel->getPlayBounds().isEmpty())
                    {
                        panel->startAudition();
                        const auto sounding = panel->isAuditioning();
                        const auto lit = panel->isPlayLit();
                        settle (1300);
                        expect (sounding && ! panel->isAuditioning(), "PLAY sounds C3 and lets go after about a second");
                        expect (lit && ! panel->isPlayLit(), "PLAY is lit while its note sounds and fades out after it");

                        // (No audio device runs here: the preview events wait in the FIFO; hand them to the processor.)
                        juce::AudioBuffer<float> drain (2, 512);
                        juce::MidiBuffer noMidi;
                        processor.processBlock (drain, noMidi);
                        processor.processBlock (drain, noMidi);
                    }

                    panel->setSortMode (PresetPanel::sortByCategory);
                    listed = panel->getListedNames();
                    const auto categories = processor.getAllPresetCategories();
                    const auto allNames = processor.getAllPresetNames();
                    expect (categories[allNames.indexOf (listed[1])] == "Bass", "SORT: CATEGORY puts the basses first after Init (" + listed[1] + ")");
                    panel->setSortMode (PresetPanel::sortByName);

                    // The star on a row toggles the favourite without loading it.
                    listed = panel->getListedNames();
                    const auto row = listed.indexOf ("Metal Pad");
                    panel->clickRow (row, 12);
                    panel->selectFilter ("*fav");
                    const auto starred = panel->getListedNames().contains ("Metal Pad");
                    panel->selectFilter ("");
                    panel->clickRow (panel->getListedNames().indexOf ("Metal Pad"), 12);
                    panel->selectFilter ("*fav");
                    const auto unstarred = ! panel->getListedNames().contains ("Metal Pad");
                    expect (row >= 0 && starred && unstarred && processor.getCurrentPresetName() == "Neuro Wobble",
                            "a row's star toggles the favourite (and doesn't load the preset)");

                    // Tags saved with user presets become chips.
                    panel->selectFilter ("*user");
                    expect (panel->getChipKeys().contains ("tag:dark") && panel->getChipKeys().contains ("tag:wide"),
                            "user preset tags show as chips (" + panel->getChipKeys().joinIntoString (" ") + ")");
                    panel->clickChip ("tag:dark");
                    expect (panel->getListedNames().size() == 1, "a tag chip filters the list");
                    panel->clickChip ("tag:dark");

                    panel->selectFilter ("");
                    panel->setSortMode (sortBefore);
                    editor->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
                    settle (400);
                    expect (! panel->isOpen(), "Esc closes the browser");
                }
            }

            // The wavetable browser: a search field, spaced Title Case names.
            {
                TableBrowser browser (processor, "osc1_table", IlanaTheme::accent());
                browser.setLookAndFeel (&editor->getLookAndFeel());
                browser.setSize (740, 520);
                const auto all = browser.getShownNames();
                browser.setSearchText ("saw");
                settle (100);
                const auto saws = browser.getShownNames();
                auto allSaws = saws.size() > 2 && saws.size() < all.size();
                for (const auto& name : saws)
                    allSaws = allSaws && name.containsIgnoreCase ("saw");
                expect (allSaws && saws.contains ("Drive Saw") && saws.contains ("Analog Saw"),
                        "the table search filters (" + saws.joinIntoString (", ") + ")");

                // The built-in packs: listed after the factory tables, found by the search,
                // and a pick selects it like a factory table.
                expect (BuiltInTables::count() > 300, "the synth carries its built-in wavetables");
                const auto builtIn = BuiltInTables::count() / 2;
                expect (all.contains (BuiltInTables::name (builtIn)), "the browser lists the built-in tables");
                browser.setSearchText (BuiltInTables::name (builtIn));
                settle (100);
                expect (browser.getShownNames().contains (BuiltInTables::name (builtIn)), "the search finds a built-in table");
                browser.setSearchText ({});
                const auto slotBefore = processor.findFreeUserSlot();
                browser.pickBuiltInTable (builtIn);
                const auto table = juce::roundToInt (processor.apvts.getRawParameterValue ("osc1_table")->load());
                expect (table == TableFactory::getNumFactoryTables() + IlanaSynthAudioProcessor::numUserSlots + builtIn
                            && processor.findFreeUserSlot() == slotBefore && BuiltInTables::table (builtIn) != nullptr
                            && processor.getWavetable (table) == BuiltInTables::table (builtIn),
                        "picking a built-in table selects it as a factory table (no User slot used) and it is ready to play");
                expect (processor.getWavetable (table)->getName() == BuiltInTables::name (builtIn)
                            && processor.apvts.getParameter ("osc1_table") != nullptr
                            && dynamic_cast<juce::AudioParameterChoice*> (processor.apvts.getParameter ("osc1_table"))->getCurrentChoiceName() == BuiltInTables::name (builtIn),
                        "the choice reads as the table's name");
                expect (all.contains ("Hard Sync") && ! all.contains ("DriveSaw") && ! all.contains ("Basic"),
                        "table names show as spaced Title Case, ANALOG's Basic as Analog Saw");
            }

            // UI review 6 (I6-35, I6-36): a .syx dropped on the window imports
            // the bank, filed by sound, and "Show in browser" opens it there.
            if (auto* confirm = findChild<ConfirmOverlay> (*editor); confirm != nullptr)
            {
                juce::MemoryOutputStream syx;
                for (const auto byte : { 0xF0, 0x43, 0x00, 0x09, 0x20, 0x00 })
                    syx.writeByte ((char) byte);
                syx.write (Dx7Banks::banks[0].data, 4096);
                auto sum = 0;
                for (int i = 0; i < 4096; ++i)
                    sum += Dx7Banks::banks[0].data[i];
                syx.writeByte ((char) ((128 - (sum & 127)) & 127));
                syx.writeByte ((char) 0xF7);
                const auto syxFile = tempDir.getChildFile ("TESTBANK.syx");
                syxFile.replaceWithData (syx.getData(), syx.getDataSize());

                expect (pages->isInterestedInFileDrag ({ syxFile.getFullPathName() })
                            && ! pages->isInterestedInFileDrag ({ tempDir.getChildFile ("x.wav").getFullPathName() }),
                        "the window takes .syx drops (and leaves other files to the sample zones)");
                pages->filesDropped ({ syxFile.getFullPathName() }, 10, 10);
                settle (200);
                expect (confirm->isAsking() && confirm->getTitle() == "Imported TESTBANK",
                        "a dropped bank imports and says so (" + confirm->getTitle() + ")");
                confirm->finish (true);
                settle (500);
                auto* panel = findChild<PresetPanel> (*editor);
                const auto listed = panel != nullptr ? panel->getListedNames() : juce::StringArray();
                const auto categories = processor.getAllPresetCategories();
                const auto names = processor.getAllPresetNames();
                const auto firstCategory = listed.isEmpty() ? juce::String() : categories[names.indexOf (listed[0])];
                expect (panel != nullptr && panel->isOpen() && listed.size() == 32 && panel->getChipKeys().contains ("bank:TESTBANK")
                            && firstCategory.isNotEmpty() && firstCategory != "DX7",
                        "Show in browser opens the new bank's 32 voices, filed by sound (" + juce::String (listed.size()) + ", "
                            + firstCategory + ")");
                if (panel != nullptr)
                {
                    panel->close();
                    settle (400);
                }
            }

            IlanaSynthAudioProcessor::userPresetDirectoryOverride = juce::File();
            tempDir.deleteRecursively();
            processor.loadFactoryPreset (neuroWobble);
            settle (200);
        }

        // The header's live waveform and the VOICES hotspot.
        {
            auto* scope = findChild<OutputView> (*editor);
            std::vector<OutputView*> views;
            findAll<OutputView> (*editor, views);
            auto strips = 0;
            for (auto* view : views)
                strips += view->isStrip() && visibleInTree (view) && view->getWidth() > 100 ? 1 : 0;
            juce::ignoreUnused (scope);
            expect (strips == 1, "the header shows one live waveform strip");
        }
    }

    // Modulation rings: every knob on every page that drives a destination
    // shows that destination's live depth. Each destination is routed from a
    // macro at full travel, a note is played, and the depth the ring reads
    // must move.
    if (pages != nullptr)
    {
        std::map<int, juce::String> destinations;
        juce::StringArray ringless;
        for (const auto& id : pages->getPageIds())
        {
            pages->showPage (id);
            settle (60);
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
            {
                if (knob->getRingDestination() > 0)
                    destinations.emplace (knob->getRingDestination(), knob->getParameterId());
                else if (knob->getParameterId().isNotEmpty())
                    ringless.addIfNotAlreadyThere (knob->getParameterId());
            }
        }
        pages->showPage ("MAIN");

        juce::StringArray silent;
        for (const auto& [destination, id] : destinations)
        {
            IlanaSynthAudioProcessor probe;
            probe.prepareToPlay (48000.0, 512);
            if (auto* macro = probe.apvts.getParameter ("macro1"))
                macro->setValueNotifyingHost (1.0f);
            probe.assignModSlot ((int) Mod::Source::Macro1, destination, 0.5f);
            juce::AudioBuffer<float> buffer (2, 512);
            for (int block = 0; block < 3; ++block)
            {
                juce::MidiBuffer midi;
                if (block == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
                buffer.clear();
                probe.processBlock (buffer, midi);
            }
            if (std::abs (probe.getModDisplay (destination)) < 0.001f)
                silent.add (id);
        }
        expect (silent.isEmpty(), "every modulatable knob's ring shows its depth (" + juce::String ((int) destinations.size())
                                      + " destinations" + (silent.isEmpty() ? juce::String() : "; silent: " + silent.joinIntoString (", ")) + ")");
        if (juce::SystemStats::getEnvironmentVariable ("ILANA_RING_REPORT", "").isNotEmpty())
            std::cout << "knobs without a ring: " << ringless.joinIntoString (", ") << std::endl;
    }

    // UI review 4, batch C (V4, V10, V12, V29, S5, S7, S19, S21, S22):
    // LFOs and envelopes.
    {
        processor.loadFactoryPreset (neuroWobble);
        settle (200);

        const auto event = [] (juce::Component& component, juce::Point<float> position, bool dragged)
        {
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys(),
                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component, juce::Time::getCurrentTime(),
                                     position, juce::Time::getCurrentTime(), 1, dragged);
        };
        const auto readShape = [&processor] (int lfo)
        {
            return juce::roundToInt (processor.apvts.getRawParameterValue ("lfo" + juce::String (lfo + 1) + "_shape")->load());
        };
        const auto setShape = [&processor] (int lfo, int shape)
        {
            auto* parameter = processor.apvts.getParameter ("lfo" + juce::String (lfo + 1) + "_shape");
            parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) shape));
        };
        const auto clearHistory = [&processor]
        {
            processor.apvts.copyState();
            processor.getUndoManager().clearUndoHistory();
        };
        const auto undoSteps = [&processor]
        {
            processor.apvts.copyState();
            return processor.getUndoManager().getUndoDescriptions();
        };

        // The first drag on a preset wave makes it a Curve with the wave's
        // points, as one undo step; a plain click changes nothing.
        {
            setShape (0, LfoShapes::Triangle);
            const auto curveBefore = processor.getLfoCurve (0).toString();
            LfoDisplay lfoDisplay (processor, 0);
            juce::Component& display = lfoDisplay;
            display.setSize (300, 150);
            clearHistory();
            display.mouseDown (event (display, { 80.0f, 110.0f }, false));
            display.mouseUp (event (display, { 80.0f, 110.0f }, false));
            const auto clickChanged = readShape (0) != LfoShapes::Triangle || ! undoSteps().isEmpty();

            display.mouseDown (event (display, { 80.0f, 110.0f }, false));
            display.mouseDrag (event (display, { 90.0f, 70.0f }, true));
            display.mouseUp (event (display, { 90.0f, 70.0f }, true));
            const auto converted = processor.getLfoCurve (0);
            const auto steps = undoSteps();
            const auto shapeAfter = readShape (0);
            processor.getUndoManager().undo();
            const auto undoneShape = readShape (0);
            const auto undoneCurve = processor.getLfoCurve (0).toString();
            processor.getUndoManager().redo();

            expect (! clickChanged, "a click on a Triangle LFO changes nothing");
            expect (shapeAfter == IlanaSynthAudioProcessor::curveShape && steps.size() == 1 && steps[0] == "LFO 1 curve"
                        && std::abs (converted.valueAt (0.5) - 1.0f) < 0.02f && std::abs (converted.valueAt (0.75)) < 0.05f
                        && std::abs (converted.valueAt (0.0) + 1.0f) < 0.02f,
                    "dragging a Triangle LFO turns it into a Curve with the triangle's points in one undo step ('"
                        + steps.joinIntoString ("', '") + "', peak " + juce::String (converted.valueAt (0.5), 2) + ")");
            expect (undoneShape == LfoShapes::Triangle && undoneCurve == curveBefore && readShape (0) == IlanaSynthAudioProcessor::curveShape,
                    "undo puts the Triangle and the old curve back, redo the converted curve");
            expect (lfoDisplay.getGridDivisions() == 8, "the LFO graph shows its snap grid control (GRID 8)");
            setShape (0, LfoShapes::Triangle);
        }

        // Steps are an LFO shape edited in that LFO's own graph (UI review 6,
        // V6-20 / I6-20): a drag on a bar sets that step, one undo step.
        {
            pages->showPage ("ENV/LFO");
            settle (200);
            auto* lfoPage = pages->getCurrentPage();
            auto* thumbs = lfoPage != nullptr ? findChild<LfoThumbBar> (*lfoPage) : nullptr;
            const auto shapeBefore = readShape (0);
            setShape (0, LfoShapes::Steps);
            if (thumbs != nullptr && thumbs->onSelect != nullptr)
                thumbs->onSelect (0);
            settle (200);

            std::vector<LfoDisplay*> lfoDisplays;
            if (lfoPage != nullptr)
                findAll<LfoDisplay> (*lfoPage, lfoDisplays);
            LfoDisplay* shown = nullptr;
            for (auto* display : lfoDisplays)
                if (visibleInTree (display))
                    shown = display;

            if (shown != nullptr)
            {
                auto* step5 = processor.apvts.getParameter ("lfo1_step5");
                const auto stepBefore = step5->getValue();
                // The time ruler sits under the well (rulerHeight + 2), outside the plot.
                const auto plot = shown->getLocalBounds().toFloat().withTrimmedBottom (16.0f).reduced (10.0f, 14.0f);
                const juce::Point<float> at (plot.getX() + plot.getWidth() * 4.5f / 16.0f, plot.getCentreY() - plot.getHeight() * 0.42f * 0.5f);
                clearHistory();
                auto& component = static_cast<juce::Component&> (*shown);
                component.mouseDown (event (component, at, false));
                component.mouseDrag (event (component, at, true));
                component.mouseUp (event (component, at, true));
                settle (50);
                const auto value = step5->convertFrom0to1 (step5->getValue());
                const auto steps = undoSteps();
                processor.getUndoManager().undo();
                expect (std::abs (value - 0.5f) < 0.05f && steps.size() == 1 && std::abs (step5->getValue() - stepBefore) < 0.001f,
                        "a Steps LFO's graph edits its steps: a drag on bar 5 sets it to " + juce::String (value, 2)
                            + " (want 0.50) in one undo step, undo puts it back");
            }
            else
            {
                expect (false, "ENV/LFO shows LFO 1's graph");
            }

            setShape (0, shapeBefore);
            settle (100);
        }

        // One RATE knob: while synced it shows the division, on MOD and PLAY.
        {
            const auto findKnob = [&editor] (const juce::String& id) -> KnobControl*
            {
                std::vector<KnobControl*> knobs;
                findAll<KnobControl> (*editor, knobs);
                for (auto* knob : knobs)
                    if (knob->getParameterId() == id && visibleInTree (knob) && knob->getWidth() > 0)
                        return knob;
                return nullptr;
            };

            // PLAY's MODULATION card: SYNC swaps RATE for the division in place.
            {
                pages->showPage ("MAIN");
                settle (300);
                if (auto* page = pages->getCurrentPage())
                    if (auto* card = findChild<ModulationCardView> (*page))
                        card->selectSource ((int) Mod::lfoSourceFor (0));
                settle (100);
                auto* sync = processor.apvts.getParameter ("lfo1_sync");
                auto* divParam = processor.apvts.getParameter ("lfo1_div");
                if (sync != nullptr && divParam != nullptr)
                {
                    const auto syncBefore = sync->getValue();
                    sync->setValueNotifyingHost (1.0f);
                    settle (300);
                    auto* division = findKnob ("lfo1_div");
                    const auto divText = divParam->getText (divParam->getValue(), 0);
                    const auto shownText = division != nullptr ? division->getSlider().getTextFromValue (division->getSlider().getValue()) : juce::String();
                    expect (division != nullptr && shownText == divText && findKnob ("lfo1_rate") == nullptr,
                            "MAIN: a synced LFO's RATE knob shows the division ('" + shownText + "', want '" + divText + "')");
                    sync->setValueNotifyingHost (0.0f);
                    settle (300);
                    expect (findKnob ("lfo1_rate") != nullptr && findKnob ("lfo1_div") == nullptr, "MAIN: free-running, RATE is the Hz knob again");
                    sync->setValueNotifyingHost (syncBefore);
                    settle (100);
                }
                else
                    expect (false, "MAIN: an LFO RATE knob is on the page");
            }

            for (const auto* page : { "ENV/LFO" })
            {
                pages->showPage (page);
                settle (300);

                // Whichever LFO the page shows.
                auto lfo = -1;
                for (int index = 0; index < IlanaSynthAudioProcessor::numLfos && lfo < 0; ++index)
                    if (findKnob ("lfo" + juce::String (index + 1) + "_rate") != nullptr || findKnob ("lfo" + juce::String (index + 1) + "_div") != nullptr)
                        lfo = index;

                const auto prefix = "lfo" + juce::String (lfo + 1);
                auto* sync = processor.apvts.getParameter (prefix + "_sync");
                auto* divParam = processor.apvts.getParameter (prefix + "_div");

                if (lfo < 0 || sync == nullptr || divParam == nullptr)
                {
                    expect (false, juce::String (page) + ": an LFO RATE knob is on the page");
                    continue;
                }

                const auto syncBefore = sync->getValue();
                sync->setValueNotifyingHost (1.0f);
                settle (300);
                const auto divText = divParam->getText (divParam->getValue(), 0);
                auto* division = findKnob (prefix + "_div");
                const auto shownText = division != nullptr ? division->getSlider().getTextFromValue (division->getSlider().getValue()) : juce::String();
                expect (division != nullptr && division->getLabelText() == "RATE" && shownText == divText && findKnob (prefix + "_rate") == nullptr,
                        juce::String (page) + ": a synced LFO's RATE knob shows the division ('" + shownText + "', want '" + divText + "')");

                sync->setValueNotifyingHost (0.0f);
                settle (300);
                expect (findKnob (prefix + "_rate") != nullptr && findKnob (prefix + "_div") == nullptr,
                        juce::String (page) + ": free-running, RATE is the Hz knob again");
                sync->setValueNotifyingHost (syncBefore);
                settle (100);
            }
        }

        // UI review 6, P4: the MOD page's pools, LFO panels, MSEG and
        // envelope graphs.
        {
            using M = IlanaSynthAudioProcessor::Module;
            constexpr auto numLfos = IlanaSynthAudioProcessor::numLfos;
            pages->showPage ("ENV/LFO");
            settle (300);
            auto* modPage = pages->getCurrentPage();
            auto* thumbs = modPage != nullptr ? findChild<LfoThumbBar> (*modPage) : nullptr;
            auto* envCards = modPage != nullptr ? findChild<EnvThumbBar> (*modPage) : nullptr;
            expect (thumbs != nullptr && envCards != nullptr, "ENV / LFO has its LFO and envelope pools");

            if (thumbs != nullptr && envCards != nullptr)
            {
                const auto click = [&event] (juce::Component& component, juce::Point<float> at)
                {
                    component.mouseDown (event (component, at, false));
                    component.mouseUp (event (component, at, false));
                };
                const auto visibleOf = [&modPage] (auto* dummy)
                {
                    using Type = std::remove_pointer_t<decltype (dummy)>;
                    std::vector<Type*> all, shown;
                    findAll<Type> (*modPage, all);
                    for (auto* component : all)
                        if (visibleInTree (component) && component->getWidth() > 0)
                            shown.push_back (component);
                    return shown;
                };

                // One card per LFO / envelope in the patch, then the extra
                // cards, then "+" last (no 1-16 ruler: V6-8; I7-30).
                auto cardsMatch = thumbs->isCardShown (LfoThumbBar::plusId)
                                  && thumbs->boundsOfCard (LfoThumbBar::plusId).getX() > thumbs->boundsOfCard (numLfos + 2).getX();
                for (int lfo = 0; lfo < numLfos; ++lfo)
                    cardsMatch = cardsMatch && thumbs->isCardInPool (lfo) == processor.isLfoShown (lfo);
                auto envCardsMatch = envCards->isCardShown (EnvThumbBar::plusId);
                for (int env = 0; env < 16; ++env)
                    envCardsMatch = envCardsMatch && envCards->isCardInPool (env) == envelopeShown (processor, env);
                expect (cardsMatch && envCardsMatch,
                        "the pools show one card per LFO / envelope in the patch, and a '+' last");

                // A full pool never scrolls sideways: cards past what fits
                // fold into a "N MORE" card, the selected one keeps a place
                // (UI review 7, V7-17 / S7-40).
                {
                    std::vector<bool> lfosBefore, envsBefore;
                    for (int lfo = 0; lfo < numLfos; ++lfo)
                        lfosBefore.push_back (processor.isRevealed (M::Lfo, lfo));
                    for (int env = 0; env < 16; ++env)
                        envsBefore.push_back (processor.isRevealed (M::Envelope, env));
                    for (int lfo = 0; lfo < numLfos; ++lfo)
                        processor.setRevealed (M::Lfo, lfo, true);
                    for (int env = 0; env < 16; ++env)
                        processor.setRevealed (M::Envelope, env, true);
                    thumbs->refreshLayout();
                    envCards->refreshLayout();
                    thumbs->onSelect (13);
                    envCards->onSelect (14);
                    settle (200);
                    const auto inside = [] (juce::Component& bar, juce::Rectangle<int> card)
                    {
                        return ! card.isEmpty() && bar.getLocalBounds().contains (card);
                    };
                    const auto lfoFolded = thumbs->getFoldedCards(), envFolded = envCards->getFoldedCards();
                    const auto fits = thumbs->getWidth() <= thumbs->getParentWidth() && envCards->getWidth() <= envCards->getParentWidth();
                    expect (fits && ! lfoFolded.empty() && ! envFolded.empty() && inside (*thumbs, thumbs->boundsOfCard (13))
                                && inside (*envCards, envCards->boundsOfCard (14)) && inside (*thumbs, thumbs->boundsOfCard (PoolCards::overflowId))
                                && inside (*envCards, envCards->boundsOfCard (PoolCards::overflowId)),
                            "full pools fold into a MORE card instead of scrolling, the selected card in view ("
                                + juce::String ((int) lfoFolded.size()) + " LFO cards and " + juce::String ((int) envFolded.size()) + " envelope cards folded)");
                    for (int lfo = 0; lfo < numLfos; ++lfo)
                        processor.setRevealed (M::Lfo, lfo, lfosBefore[(size_t) lfo]);
                    for (int env = 0; env < 16; ++env)
                        processor.setRevealed (M::Envelope, env, envsBefore[(size_t) env]);
                    thumbs->onSelect (0);
                    envCards->onSelect (0);
                    thumbs->refreshLayout();
                    envCards->refreshLayout();
                    settle (200);
                }

                // Times of 0 read "0 ms": "Off" is for switches (S7-29).
                expect (describeValue ("amp_delay", 0.0f) == "0 ms" && describeValue ("fe_hold", 0.0f) == "0 ms"
                            && describeValue ("opeg_lfo_delay", 0.0f) == "0 ms",
                        "an envelope's DELAY / HOLD and the Op LFO's DELAY at 0 read 0 ms, not Off");
                // A word in a number's place is a value, not a dim label that forgot one
                // (I9-25, review 11 S11-17): SEED reads Random, EXCITE POS Auto, in the value colour.
                expect (describeValue ("lfo1_seed", 0.0f) == "Random" && describeValue ("osc1_string_excite_pos", 0.0f) == "Auto"
                            && ! IlanaLookAndFeel::isPlaceholderValue (describeValue ("lfo1_seed", 0.0f))
                            && ! IlanaLookAndFeel::isPlaceholderValue (describeValue ("osc1_string_excite_pos", 0.0f))
                            && ! IlanaLookAndFeel::isPlaceholderValue (describeValue ("lfo1_seed", 3.0f)),
                        "SEED reads Random and EXCITE POS Auto in the value colour, as the numbers do");

                // "+" adds the next LFO and opens it; its x takes it away
                // again at once while nothing routes it.
                auto hidden = -1;
                for (int lfo = numLfos - 1; lfo >= 0; --lfo)
                    if (! processor.isLfoShown (lfo))
                        hidden = lfo;
                if (hidden >= 0)
                {
                    click (*thumbs, thumbs->boundsOfCard (LfoThumbBar::plusId).getCentre().toFloat());
                    settle (100);
                    const auto added = processor.isLfoShown (hidden) && thumbs->isCardShown (hidden);
                    std::vector<LfoDisplay*> lfoDisplays;
                    findAll<LfoDisplay> (*modPage, lfoDisplays);
                    auto opened = 0;
                    for (int lfo = 0; lfo < (int) lfoDisplays.size(); ++lfo)
                        opened += visibleInTree (lfoDisplays[(size_t) lfo]) ? 1 : 0;
                    expect (added && opened == 1, "'+' adds LFO " + juce::String (hidden + 1) + " and opens it");

                    clearHistory();
                    const auto removeAt = thumbs->removeButtonOf (hidden).getCentre().toFloat();
                    thumbs->mouseMove (event (*thumbs, removeAt, false));
                    click (*thumbs, removeAt);
                    settle (100);
                    expect (! processor.isLfoShown (hidden) && ! thumbs->isCardShown (hidden) && undoSteps().isEmpty(),
                            "the hover x on an unrouted LFO removes its card at once (nothing to undo)");

                    // A routed one asks first, naming what it drives, and
                    // takes its routes with it in one undo step.
                    processor.setRevealed (M::Lfo, hidden, true);
                    auto slot = -1;
                    for (int index = 0; index < Mod::maxSlots && slot < 0; ++index)
                        if (processor.readModSlot (index).source == Mod::Source::None)
                            slot = index;
                    processor.setModSlotValue (slot, "src", (float) (int) Mod::lfoSourceFor (hidden));
                    processor.setModSlotValue (slot, "dst", 1.0f);
                    processor.setModSlotValue (slot, "amt", 0.3f);
                    settle (100);
                    juce::String heading, action;
                    poolRemovalHook() = [&heading, &action] (const juce::String& h, const juce::String& a, std::function<void()> remove)
                    {
                        heading = h;
                        action = a;
                        remove();
                    };
                    clearHistory();
                    thumbs->requestRemove (hidden);
                    settle (100);
                    poolRemovalHook() = nullptr;
                    const auto name = "LFO " + juce::String (hidden + 1);
                    const auto steps = undoSteps();
                    const auto cleared = processor.readModSlot (slot).source == Mod::Source::None;
                    processor.getUndoManager().undo();
                    settle (50);
                    const auto restored = processor.readModSlot (slot).source == Mod::lfoSourceFor (hidden);
                    expect (heading.startsWith (name + " drives ") && action == "Remove " + name + " and its route" && cleared
                                && steps.size() == 1 && steps[0] == "Remove " + name && restored,
                            "the x on a routed LFO asks ('" + heading + "' / '" + action + "'), removes it with its route as '"
                                + steps.joinIntoString ("', '") + "', undo brings the route back");
                    processor.clearModSlot (slot);
                    processor.setRevealed (M::Lfo, hidden, false);
                    settle (100);
                }

                // The envelope pool: the x on a routed envelope asks too.
                {
                    processor.setRevealed (M::Envelope, 6, true);
                    envCards->refreshLayout();
                    auto slot = -1;
                    for (int index = 0; index < Mod::maxSlots && slot < 0; ++index)
                        if (processor.readModSlot (index).source == Mod::Source::None)
                            slot = index;
                    processor.setModSlotValue (slot, "src", (float) (int) envelopeSource (6));
                    processor.setModSlotValue (slot, "dst", 1.0f);
                    processor.setModSlotValue (slot, "amt", 0.3f);
                    settle (100);
                    juce::String heading;
                    poolRemovalHook() = [&heading] (const juce::String& h, const juce::String&, std::function<void()> remove)
                    {
                        heading = h;
                        remove();
                    };
                    clearHistory();
                    envCards->requestRemove (6);
                    settle (100);
                    poolRemovalHook() = nullptr;
                    const auto gone = ! envelopeShown (processor, 6) && processor.readModSlot (slot).source == Mod::Source::None;
                    processor.getUndoManager().undo();
                    settle (50);
                    expect (heading.startsWith ("ENV 7 ") && gone && processor.readModSlot (slot).source == envelopeSource (6),
                            "the x on a routed envelope asks ('" + heading + "'), removes it and its route, undo restores the route");
                    processor.clearModSlot (slot);
                    processor.setRevealed (M::Envelope, 6, false);
                    envCards->refreshLayout();
                    settle (100);
                }

                // Every simulated shape's panel: names, dials and values
                // clear of each other (V6-11 / S6-12), all inside the LFO
                // section, and output B a draggable tag on the graph (I6-24).
                {
                    const auto shapeBefore = readShape (0);
                    const auto envTop = envCards->getScreenY();
                    juce::StringArray problems;
                    for (int shape = LfoSimShapes::RandomHold; shape <= LfoSimShapes::Friction; ++shape)
                    {
                        setShape (0, shape);
                        thumbs->onSelect (0);
                        settle (150);

                        std::vector<juce::Component*> controls;
                        for (auto* knob : visibleOf ((KnobControl*) nullptr))
                        {
                            if (knob->getScreenY() >= envTop)
                                continue;
                            controls.push_back (knob);
                            if (knob->getDialSize() < IlanaTheme::KnobSize::minimum || knob->getHeight() < 13 + IlanaTheme::KnobSize::minimum + 16)
                                problems.add (juce::String (shape) + ":" + knob->getLabelText() + " dial " + juce::String (knob->getDialSize())
                                              + " in " + juce::String (knob->getHeight()) + " px");
                        }
                        for (auto* combo : visibleOf ((ComboControl*) nullptr))
                            if (combo->getScreenY() < envTop)
                                controls.push_back (combo);
                        for (auto* toggle : visibleOf ((ToggleControl*) nullptr))
                            if (toggle->getScreenY() < envTop)
                                controls.push_back (toggle);

                        for (size_t a = 0; a < controls.size(); ++a)
                        {
                            if (controls[a]->getScreenBounds().getBottom() > envTop)
                                problems.add (juce::String (shape) + ": a control runs into the envelopes");
                            for (size_t b = a + 1; b < controls.size(); ++b)
                                if (controls[a]->getScreenBounds().intersects (controls[b]->getScreenBounds()))
                                    problems.add (juce::String (shape) + ": two controls overlap");
                        }

                        LfoDisplay* display = nullptr;
                        for (auto* candidate : visibleOf ((LfoDisplay*) nullptr))
                            display = candidate;
                        const auto tagB = display != nullptr ? display->getOutputTagBounds (1) : juce::Rectangle<float>();
                        if (tagB.isEmpty() || ! display->getLocalBounds().toFloat().contains (tagB))
                            problems.add (juce::String (shape) + ": no output B tag");
                    }
                    // The LFO's source chip carries a "B" for output B too.
                    {
                        std::vector<ModSourceChip*> chips;
                        findAll<ModSourceChip> (*editor, chips);
                        for (auto* chip : chips)
                            if (chip->getSourceIndex() == (int) Mod::Source::Lfo1)
                            {
                                settle (100);
                                if ((chip->getSecondOutputBounds().isEmpty() || chip->secondIndex != (int) Mod::Source::Lfo1B))
                                    problems.add ("LFO 1's chip has no OUT 2");
                                // S8-16 / V8-28 / V9-20: over the sub-chip, the
                                // tooltip names the second output, spelt out.
                                else if (! chip->tooltipAt (chip->getSecondOutputBounds().getCentre()).contains ("OUT 2")
                                         || chip->tooltipAt ({ 4.0f, 4.0f }).contains ("OUT 2"))
                                    problems.add ("LFO 1's OUT 2 has no tooltip naming it");
                            }
                    }
                    expect (problems.isEmpty(), "every simulated LFO shape's knobs show name, dial and value apart, no control overlaps, "
                                                "and output B has its tag" + (problems.isEmpty() ? juce::String() : " (" + problems.joinIntoString ("; ") + ")"));
                    setShape (0, shapeBefore);
                    settle (100);
                }

                // The SHAPE list is grouped for display only: every shape in
                // one group, the box's items still in parameter order.
                {
                    std::vector<int> counts (30, 0);
                    for (const auto& group : LfoShapeMenu::groups())
                        for (const auto shape : group.shapes)
                            if (juce::isPositiveAndBelow (shape, 30))
                                ++counts[(size_t) shape];
                    auto eachOnce = std::all_of (counts.begin(), counts.end(), [] (int count) { return count == 1; });
                    ComboControl* shapeBox = nullptr;
                    for (auto* combo : visibleOf ((ComboControl*) nullptr))
                        if (combo->getComboBox().getNumItems() == 30)
                            shapeBox = combo;
                    auto inOrder = shapeBox != nullptr;
                    for (int item = 0; shapeBox != nullptr && item < 30; ++item)
                        inOrder = inOrder && shapeBox->getComboBox().getItemId (item) == item + 1;
                    expect (eachOnce && inOrder && shapeBox->getComboBox().getItemText (LfoSimShapes::Rossler).contains ("ssler"),
                            "the LFO SHAPE list groups every shape once and keeps the saved indices");
                }

                // The MSEG is a card in the LFO pool, edited in the LFO's
                // place; a dragged point follows the mouse, one undo step.
                // (The old module has a card only while the patch uses it, UI
                // review 8, I8-4: here OSC 1's warp envelope plays it.)
                auto* warpEnvelope = processor.apvts.getParameter ("osc1_pd_env");
                const auto warpBefore = warpEnvelope != nullptr ? warpEnvelope->getValue() : 0.0f;
                if (warpEnvelope != nullptr)
                    warpEnvelope->setValueNotifyingHost (warpEnvelope->convertTo0to1 (17.0f));
                thumbs->refreshLayout();
                settle (200);
                {
                    thumbs->onSelect (numLfos);
                    settle (200);
                    MsegEditor* mseg = nullptr;
                    for (auto* candidate : visibleOf ((MsegEditor*) nullptr))
                        mseg = candidate;
                    if (mseg != nullptr)
                    {
                        // GRID snaps the time (UI review 7, I7-30): point 2
                        // lands on a column; then free, it follows the mouse.
                        {
                            const auto plotArea = mseg->getPlotArea();
                            const auto from = mseg->getPointPosition (1);
                            const auto to = juce::Point<float> (plotArea.getX() + plotArea.getWidth() * 0.3f, from.y);
                            auto& component = static_cast<juce::Component&> (*mseg);
                            component.mouseDown (event (component, from, false));
                            component.mouseDrag (event (component, to, true));
                            const auto landed = mseg->getPointPosition (1);
                            component.mouseUp (event (component, to, true));
                            settle (50);
                            const auto column = (landed.x - plotArea.getX()) / plotArea.getWidth() * (float) mseg->getGridDivisions();
                            processor.getUndoManager().undo();
                            settle (50);
                            expect (mseg->getGridDivisions() == 8 && std::abs (column - std::round (column)) < 0.05f,
                                    "the MSEG's GRID 8 snaps a dragged point to a column (" + juce::String (column, 2) + ")");
                        }
                        mseg->setGridDivisions (0);
                        const auto from = mseg->getPointPosition (1);
                        // Up or down to +0.3 (clear of the level's limits), a
                        // little sideways.
                        const auto plotArea = mseg->getPlotArea();
                        const auto to = juce::Point<float> (from.x + 8.0f, plotArea.getCentreY() - 0.3f * plotArea.getHeight() * 0.5f);
                        clearHistory();
                        auto& component = static_cast<juce::Component&> (*mseg);
                        component.mouseDown (event (component, from, false));
                        component.mouseDrag (event (component, to, true));
                        const auto landed = mseg->getPointPosition (1);
                        component.mouseUp (event (component, to, true));
                        settle (50);
                        const auto steps = undoSteps();
                        processor.getUndoManager().undo();
                        settle (50);
                        expect (landed.getDistanceFrom (to) < 3.0f && steps.size() == 1 && mseg->getPointPosition (1).getDistanceFrom (from) < 1.0f,
                                "the MSEG card opens its editor; point 2 follows the mouse (" + juce::String (landed.getDistanceFrom (to), 1)
                                    + " px off) in one undo step");
                        mseg->setGridDivisions (8);
                    }
                    else
                    {
                        expect (false, "the MSEG card opens the MSEG editor");
                    }
                    thumbs->onSelect (0);
                    settle (100);
                }
                if (warpEnvelope != nullptr)
                    warpEnvelope->setValueNotifyingHost (warpBefore);
                settle (100);

                // Envelope graphs: the time ruler sits below the plot
                // (V6-17), and each segment's dot curves that segment alone,
                // with its value beside it while dragged (V5-18).
                {
                    envCards->onSelect (0);
                    settle (150);
                    EnvelopeDisplay* amp = nullptr;
                    for (auto* candidate : visibleOf ((EnvelopeDisplay*) nullptr))
                        amp = candidate;
                    if (amp != nullptr)
                    {
                        const auto plot = amp->getPlotArea();
                        const auto read = [&processor] (const char* id) { return processor.apvts.getRawParameterValue (id)->load(); };
                        const auto curvesBefore = std::array<float, 3> { read ("amp_curve"), read ("amp_dcurve"), read ("amp_rcurve") };
                        const auto from = amp->getHandlePosition (EnvelopeDisplay::firstCurveHandle);
                        const auto to = from + juce::Point<float> (0.0f, -12.0f);
                        clearHistory();
                        auto& component = static_cast<juce::Component&> (*amp);
                        component.mouseDown (event (component, from, false));
                        component.mouseDrag (event (component, to, true));
                        const auto readout = amp->getReadout();
                        const auto landed = amp->getHandlePosition (EnvelopeDisplay::firstCurveHandle);
                        component.mouseUp (event (component, to, true));
                        settle (50);
                        const auto changed = read ("amp_acurve");
                        const auto others = std::array<float, 3> { read ("amp_curve"), read ("amp_dcurve"), read ("amp_rcurve") };
                        const auto steps = undoSteps();
                        processor.getUndoManager().undo();
                        settle (50);
                        expect (plot.getBottom() + EnvelopeDisplay::rulerHeight <= (float) amp->getHeight() && plot.getY() > 0.0f,
                                "the envelope's time ruler is below its plot");
                        expect (changed > 0.05f && others == curvesBefore && readout.startsWith ("ATTACK CURVE")
                                    && landed.getDistanceFrom (to) < 3.0f && steps.size() == 1 && std::abs (read ("amp_acurve")) < 0.001f,
                                "the attack's curve dot bends the attack alone (" + juce::String (changed, 2) + ", '" + readout
                                    + "', the dot " + juce::String (landed.getDistanceFrom (to), 1) + " px from the mouse), one undo step");
                        // Impeccable re-score toward 35: a handle says its value on hover too.
                        const auto hoverText = amp->getReadoutFor (0);
                        expect (hoverText.startsWith ("ATTACK") && amp->getReadoutFor (1).contains ("SUSTAIN"),
                                "an envelope handle names its value for the hover readout ('" + hoverText + "')");
                    }
                    else
                    {
                        expect (false, "ENV / LFO shows the amp envelope's graph");
                    }
                }

                // (PLAY's envelope carousel following the pool is covered in PlayPageUiTests.h.)
                {
                    pages->showPage ("ENV/LFO");
                    settle (200);
                }

                // No routing to an LFO or envelope that isn't in its pool:
                // a knob's "Modulate with" lists the pool's (and offers the
                // next as "New LFO"), the matrix greys the rest out.
                {
                    KnobControl* knob = nullptr;
                    for (auto* candidate : visibleOf ((KnobControl*) nullptr))
                        if (knob == nullptr && candidate->getRingDestination() != 0)
                            knob = candidate;
                    auto hiddenLfo = -1;
                    for (int lfo = numLfos - 1; lfo >= 0; --lfo)
                        if (! processor.isLfoShown (lfo))
                            hiddenLfo = lfo;
                    auto hiddenEnv = -1;
                    for (int env = 15; env >= 0; --env)
                        if (! envelopeShown (processor, env))
                            hiddenEnv = env;
                    juce::StringArray offered;
                    if (knob != nullptr)
                    {
                        const auto menu = knob->buildModulateWithMenu();
                        for (juce::PopupMenu::MenuItemIterator item (menu, true); item.next();)
                            if (item.getItem().itemID != 0)
                                offered.add (item.getItem().text);
                    }
                    const auto names = Mod::getSourceNames();
                    auto onlyPool = knob != nullptr && hiddenLfo >= 0 && hiddenEnv >= 0;
                    for (int source = 1; source < names.size() && onlyPool; ++source)
                        if (offered.contains (names[source]) && ! modSourceInPatch (processor, (Mod::Source) source))
                            onlyPool = false;
                    onlyPool = onlyPool && offered.contains ("LFO 1") && offered.contains (juce::String::fromUTF8 ("LFO 1 \xc2\xb7 OUT 2"))
                               && offered.contains ("AMP ENV")
                               && ! offered.contains (names[(int) Mod::lfoSourceFor (hiddenLfo)])
                               && offered.contains ("New LFO  (LFO " + juce::String (hiddenLfo + 1) + ")");

                    MatrixRow row (processor, 0);
                    std::vector<bool> inPatch ((size_t) names.size(), true);
                    for (int source = 1; source < names.size(); ++source)
                        inPatch[(size_t) source] = modSourceInPatch (processor, (Mod::Source) source);
                    row.setSourcesInPatch (inPatch);
                    const auto lfoItem = (int) Mod::lfoSourceFor (hiddenLfo) + 1;
                    const auto greyed = hiddenLfo >= 0 && ! row.isSourceItemEnabled (lfoItem)
                                        && row.isSourceItemEnabled ((int) Mod::Source::Lfo1 + 1)
                                        && row.isSourceItemEnabled ((int) Mod::Source::ModWheel + 1);
                    expect (onlyPool && greyed, "knob menus offer only the pools' LFOs and envelopes (" + juce::String (offered.size())
                                                    + " items) and a New LFO; the matrix greys out LFO " + juce::String (hiddenLfo + 1));
                }
            }
        }

        // The envelope graph's playhead rides the curve for the playing note
        // and goes when it ends.
        {
            EnvelopeDisplay display (processor, "amp");
            display.setSize (400, 160);
            juce::AudioBuffer<float> buffer (2, 512);
            const auto run = [&processor, &buffer] (int blocks, bool noteOn, bool noteOff)
            {
                for (int block = 0; block < blocks; ++block)
                {
                    juce::MidiBuffer midi;
                    if (block == 0 && noteOn)
                        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
                    if (block == 0 && noteOff)
                        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                    buffer.clear();
                    processor.processBlock (buffer, midi);
                }
            };
            run (40, true, false); // 0.4 s: past Neuro Wobble's 5 ms attack and 300 ms decay
            const auto held = display.playheadPoint();
            const auto stage = (int) processor.getEnvMonitorPosition (0);
            run (60, false, true); // past the 50 ms release
            const auto after = display.playheadPoint();
            expect (held.has_value() && stage == 4 && ! after.has_value(),
                    "the envelope's playhead sits on the sustain while a note is held (stage " + juce::String (stage)
                        + ") and goes once it has released");
        }

        // The dot follows the newest note, not the loudest voice, and a stage
        // shorter than a block is still crossed.
        {
            juce::AudioBuffer<float> buffer (2, 512);
            const auto run = [&processor, &buffer] (int blocks, int note, bool on)
            {
                for (int block = 0; block < blocks; ++block)
                {
                    juce::MidiBuffer midi;
                    if (block == 0 && note >= 0)
                        midi.addEvent (on ? juce::MidiMessage::noteOn (1, note, 0.8f) : juce::MidiMessage::noteOff (1, note), 0);
                    buffer.clear();
                    processor.processBlock (buffer, midi);
                }
            };
            run (40, 60, true);
            const auto held = processor.getEnvMonitorPosition (0);
            const auto serial = processor.getNoteSerial();
            if (auto* parameter = processor.apvts.getParameter ("amp_attack"))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (0.3f));
            run (2, 64, true); // a second note, slowly rising
            const auto second = processor.getEnvMonitorPosition (0);
            expect ((int) held == 4 && (int) second == 1 && processor.getNoteSerial() == serial + 1,
                    "the playhead follows the newest note (held " + juce::String (held) + ", then " + juce::String (second)
                        + ") and the note count goes up");
            run (1, 60, false);
            run (1, 64, false);
            run (80, -1, false);

            IlanaAnim::EnvTracker tracker;
            const std::array<float, 5> seconds { 0.0f, 0.002f, 0.0f, 0.3f, 0.05f };
            tracker.get (4.0f, seconds, 0);
            const auto atNote = tracker.get (3.2f, seconds, 1); // the block already past a 2 ms attack
            juce::Thread::sleep (20);
            const auto later = tracker.get (3.3f, seconds, 1);
            expect ((int) atNote == 1 && (int) later == 3,
                    "a new note starts the dot at the attack and runs it on (" + juce::String (atNote) + ", then " + juce::String (later) + ")");
        }

        // The dot rides the drawn line, curved or not, through a whole note,
        // an early key up and a retrigger; it leaves a trail while it moves
        // and no repaints are asked for once it is gone.
        for (const auto curveValue : { 0.0f, 0.8f, -0.8f })
        {
            IlanaSynthAudioProcessor local;
            local.prepareToPlay (48000.0, 512);
            const auto setLocal = [&local] (const juce::String& id, float plain)
            {
                if (auto* parameter = local.apvts.getParameter (id))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
            };
            setLocal ("amp_attack", 0.15f);
            setLocal ("amp_decay", 0.3f);
            setLocal ("amp_sustain", 0.5f);
            setLocal ("amp_release", 0.4f);
            setLocal ("amp_curve", curveValue);
            EnvelopeDisplay display (local, "amp");
            display.setSize (400, 160);
            juce::AudioBuffer<float> buffer (2, 512);
            auto worst = 0.0f;
            auto seen = 0;
            const auto frame = [&] (int note, bool on)
            {
                for (int block = 0; block < 3; ++block)
                {
                    juce::MidiBuffer midi;
                    if (block == 0 && note >= 0)
                        midi.addEvent (on ? juce::MidiMessage::noteOn (1, note, 0.8f) : juce::MidiMessage::noteOff (1, note), 0);
                    buffer.clear();
                    local.processBlock (buffer, midi);
                }
                juce::Thread::sleep (20);
                display.createComponentSnapshot (display.getLocalBounds()); // paints: the trail follows the dot
                const auto off = display.playheadOffLine();
                if (off >= 0.0f)
                {
                    worst = juce::jmax (worst, off);
                    ++seen;
                }
            };
            frame (60, true);
            for (int i = 0; i < 25; ++i) frame (-1, false);   // attack, decay, sustain
            frame (60, false);
            for (int i = 0; i < 8; ++i) frame (-1, false);    // part of the release
            frame (64, true);                                  // a new note, on the way down
            for (int i = 0; i < 3; ++i) frame (-1, false);
            frame (64, false);                                 // let go in the attack
            for (int i = 0; i < 30; ++i) frame (-1, false);
            expect (seen > 20 && worst < 1.0f,
                    "the envelope dot rides the drawn line at curve " + juce::String (curveValue, 1) + " (worst " + juce::String (worst, 2)
                        + " px over " + juce::String (seen) + " frames)");
        }

        {
            IlanaSynthAudioProcessor local;
            local.prepareToPlay (48000.0, 512);
            EnvelopeDisplay display (local, "amp");
            display.setSize (400, 160);
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
            local.processBlock (buffer, midi);
            display.createComponentSnapshot (display.getLocalBounds());
            const auto lit = display.trailIsLit();
            juce::MidiBuffer off;
            off.addEvent (juce::MidiMessage::allNotesOff (1), 0);
            local.processBlock (buffer, off);
            for (int i = 0; i < 400; ++i)
            {
                buffer.clear();
                juce::MidiBuffer none;
                local.processBlock (buffer, none);
            }
            for (int i = 0; i < 60; ++i) // the display's timer, 30 frames a second
            {
                juce::Thread::sleep (30);
                display.createComponentSnapshot (display.getLocalBounds());
            }
            expect (lit && ! display.trailIsLit(), "the envelope's trail lights with a note and has died away (no repaints asked) once it is over");
        }
    }

    // UI review 4, batch E: oscillators, filter, PLAY.
    {
        const auto setParam = [&processor] (const juce::String& id, float plain)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
        };
        const auto readParam = [&processor] (const juce::String& id)
        {
            const auto* value = processor.apvts.getRawParameterValue (id);
            return value != nullptr ? value->load() : 0.0f;
        };
        const auto findKnob = [&editor] (const juce::String& id) -> KnobControl*
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
                if (knob->getParameterId() == id && visibleInTree (knob) && knob->getWidth() > 0)
                    return knob;
            return nullptr;
        };
        const auto loadNamed = [&processor] (const juce::String& name)
        {
            processor.loadFactoryPreset (processor.getFactoryPresetNames().indexOf (name));
            settle (500);
        };
        const juce::StringArray presets { "Init", "Neuro Wobble", "E.PIANO 1 (ROM1A)" };

        // No card is cut mid-knob: every visible knob on PLAY and OSC is
        // wholly inside each of its ancestors (the scrolling viewports
        // included) or wholly out of view, at 100 % and 75 % (V13, S8).
        const auto clippedKnobs = [&editor] (juce::StringArray& names)
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
            {
                if (! visibleInTree (knob) || knob->getWidth() <= 0 || knob->getHeight() <= 0)
                    continue;
                // The dial and its value: the label above may tuck under a header.
                const auto area = editor->getLocalArea (knob, knob->getLocalBounds().withTrimmedTop (13));
                // A knob scrolled wholly out of its view is hidden, not cut
                // (the scrolled column may still reach past the page).
                auto hidden = false;
                for (auto* parent = knob->getParentComponent(); parent != nullptr && parent != editor.get(); parent = parent->getParentComponent())
                    hidden = hidden || ! editor->getLocalArea (parent, parent->getLocalBounds()).intersects (area);
                if (hidden)
                    continue;
                for (auto* parent = knob->getParentComponent(); parent != nullptr && parent != editor.get(); parent = parent->getParentComponent())
                {
                    // (A scrolling column, PLAY's operators past the third, is cut by its viewport on purpose.)
                    if (dynamic_cast<juce::Viewport*> (parent) != nullptr || dynamic_cast<juce::Viewport*> (parent->getParentComponent()) != nullptr)
                        break;
                    const auto view = editor->getLocalArea (parent, parent->getLocalBounds());
                    if (view.intersects (area) && ! view.contains (area))
                    {
                        names.add (knob->getParameterId());
                        break;
                    }
                }
            }
        };

        for (const auto width : { 1060, 795 })
        {
            editor->setSize (width, width * 720 / 1060);
            settle (300);
            for (const auto& preset : presets)
            {
                loadNamed (preset);
                for (const auto* page : { "MAIN", "OSC" })
                {
                    pages->showPage (page);
                    settle (400);
                    juce::StringArray clipped;
                    clippedKnobs (clipped);
                    expect (clipped.isEmpty() && processor.getFactoryPresetNames().contains (preset),
                            juce::String (page) + " at " + juce::String (width) + " px, " + preset + ": no knob is cut by its card or the page"
                                + (clipped.isEmpty() ? juce::String() : " (cut: " + clipped.joinIntoString (", ") + ")"));
                }
            }
        }
        editor->setSize (1060, 720);
        settle (300);

        // Knob names and modulation rings are never cut (ilana's Windows
        // screenshots, design sweep): on every page, at 75 %, 100 % and
        // 150 %, each visible knob's name and the box its rings draw in sit
        // inside the knob and inside every view above it, and the rings stay
        // under the name. Neuro Wobble's FRAME has three rings.
        const auto cutKnobText = [&editor] (juce::StringArray& names)
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
            {
                if (! visibleInTree (knob) || knob->getWidth() <= 0 || knob->getHeight() <= 0)
                    continue;
                const auto own = knob->getLocalBounds().toFloat().expanded (0.5f);
                const auto name = knob->getNameTextBounds();
                const auto rings = knob->getRingsBounds();
                const auto whole = editor->getLocalArea (knob, knob->getLocalBounds());
                // A knob scrolled out of view, wholly or in part, is hidden, not cut (the scroll
                // decides that, not the layout: the OSC tab's one-row strip can sit at a viewport's edge).
                auto hidden = false;
                for (auto* parent = knob->getParentComponent(); parent != nullptr && parent != editor.get(); parent = parent->getParentComponent())
                    hidden = hidden || ! editor->getLocalArea (parent, parent->getLocalBounds()).intersects (whole)
                             || (dynamic_cast<juce::Viewport*> (parent) != nullptr && ! editor->getLocalArea (parent, parent->getLocalBounds()).contains (whole));
                if (hidden)
                    continue;
                juce::StringArray why;
                if (! name.isEmpty() && ! own.contains (name))
                    why.add ("name outside the knob");
                if (! rings.isEmpty() && ! own.contains (rings))
                    why.add ("rings outside the knob");
                // (The rings may pass under the name's line, never into its letters.)
                const auto ink = knob->getNameInkBounds();
                if (! ink.isEmpty() && ! rings.isEmpty() && rings.getY() < ink.getBottom() + 0.5f
                    && rings.getX() < ink.getRight() && rings.getRight() > ink.getX())
                    why.add ("rings reach the name");
                const std::pair<const char*, juce::Rectangle<float>> parts[] { { "name", name }, { "rings", rings } };
                for (const auto& [what, part] : parts)
                {
                    if (part.isEmpty())
                        continue;
                    const auto area = editor->getLocalArea (knob, part.getSmallestIntegerContainer().reduced (1));
                    for (auto* parent = knob->getParentComponent(); parent != nullptr && parent != editor.get(); parent = parent->getParentComponent())
                        if (! editor->getLocalArea (parent, parent->getLocalBounds()).contains (area))
                        {
                            why.add (juce::String (what) + " cut by " + parent->getName() + " " + juce::String (typeid (*parent).name()));
                            break;
                        }
                }
                if (! why.isEmpty())
                    names.add (knob->getParameterId() + " (" + why.joinIntoString ("; ") + "; knob " + knob->getLocalBounds().toString()
                               + ", name " + name.toString() + ", rings " + rings.toString() + ")");
            }
        };

        loadNamed ("Neuro Wobble");
        for (const auto width : { 795, 1060, 1590 })
        {
            editor->setSize (width, width * 720 / 1060);
            settle (300);
            for (const auto& page : pages->getPageIds())
            {
                pages->showPage (page);
                settle (300);
                juce::StringArray cut;
                cutKnobText (cut);
                expect (cut.isEmpty(), page + " at " + juce::String (width) + " px: no knob's name or rings are cut"
                                           + (cut.isEmpty() ? juce::String() : " (" + cut.joinIntoString (", ") + ")"));
            }
        }
        editor->setSize (1060, 720);
        settle (300);

        // The DX7 voice's six operators: six strips on PLAY, each showing
        // its RATIO (it is an operator: UI review 6, V3, I6-5), nothing
        // cut; on OSC the OSC 4 tab shows OSC 4 alone.
        loadNamed ("E.PIANO 1 (ROM1A)");
        {
            pages->showPage ("MAIN");
            settle (400);
            juce::StringArray clipped;
            clippedKnobs (clipped);
            auto ratios = 0;
            for (const auto* prefix : OscillatorIds::prefixes)
                ratios += findKnob (juce::String (prefix) + "_ratio") != nullptr ? 1 : 0;
            expect (ratios == OscillatorIds::count && clipped.isEmpty(),
                    "MAIN: the DX7 voice's six operators each show their RATIO (" + juce::String (ratios) + "), nothing cut ("
                        + clipped.joinIntoString (", ") + ")");

            // (PLAY's envelope carousel on a DX7 voice is covered in PlayPageUiTests.h.)

            pages->showPage ("OSC");
            settle (400);
            selectOscTab (3);
            clipped.clear();
            clippedKnobs (clipped);
            expect (findKnob ("osc4_ratio") != nullptr && findKnob ("osc1_level") == nullptr && clipped.isEmpty(),
                    "OSC: the OSC 4 tab shows OSC 4's RATIO alone, nothing cut (" + clipped.joinIntoString (", ") + ")");
            selectOscTab (0);
        }

        // No remove button on an oscillator's title (S16): it is on the
        // title's right-click menu.
        loadNamed ("Neuro Wobble");
        for (const auto* page : { "MAIN", "OSC" })
        {
            pages->showPage (page);
            settle (300);
            std::vector<juce::Button*> buttons;
            findAll<juce::Button> (*editor, buttons);
            auto crosses = 0;
            for (auto* button : buttons)
                crosses += visibleInTree (button) && (button->getButtonText() == juce::String (juce::CharPointer_UTF8 ("\xc3\x97"))
                                                      || button->getButtonText() == "x") ? 1 : 0;
            expect (crosses == 0, juce::String (page) + ": no remove button on the oscillator titles");

            // One way to add an oscillator (S33); on OSC one oscillator at a
            // time, behind its tab (V19).
            auto adds = 0;
            for (auto* button : buttons)
                adds += visibleInTree (button) && button->getButtonText().contains ("ADD OSC") ? 1 : 0;
            auto switches = 0;
            for (const auto* prefix : OscillatorIds::prefixes)
                switches += toggleFor (juce::String (prefix) + "_on") != nullptr ? 1 : 0;
            auto shown = 0;
            for (int osc = 0; osc < OscillatorIds::count; ++osc)
                shown += processor.isOscillatorShown (osc) ? 1 : 0;
            // (The approved OSC design stacks every shown oscillator as a card,
            // each with its own switch, so OSC counts like PLAY now.)
            expect ((juce::String (page) == "MAIN" ? adds <= 1 : adds == 1) && switches == shown,
                    juce::String (page) + ": one ADD OSC button (" + juce::String (adds) + "), "
                        + "a switch per oscillator" + " ("
                        + juce::String (switches) + ")");
        }

        // PLAY: one FILTER card, one graph drawing both filters (one-tab PLAY, 2026-10-25).
        loadNamed ("Init");
        pages->showPage ("MAIN");
        settle (300);
        {
            std::vector<FilterDisplay*> displays;
            findAll<FilterDisplay> (*editor, displays);
            auto shownGraphs = 0;
            for (auto* display : displays)
                shownGraphs += visibleInTree (display) ? 1 : 0;
            expect (shownGraphs == 1, "PLAY shows one filter graph for both filters (" + juce::String (shownGraphs) + ")");
        }

        // PLAY's filter TYPE reads like FILTER's (its short names), SLOPE is
        // FILTER's 12 / 24 dB switch (S10).
        {
            std::vector<ComboControl*> combos;
            findAll<ComboControl> (*editor, combos);
            auto typeText = juce::String();
            for (auto* combo : combos)
                if (visibleInTree (combo) && combo->getComboBox().getNumItems() == FilterTypeGrid::shortNames().size()
                    && combo->getComboBox().getItemText (0) == FilterTypeGrid::shortNames()[0])
                    typeText = combo->getComboBox().getText();
            expect (typeText.isNotEmpty() && typeText == FilterTypeGrid::shortNames()[juce::roundToInt (readParam ("f1_type"))],
                    "PLAY's filter TYPE shows FILTER's name ('" + typeText + "')");
        }

        // The WAVE view: the frame as a readout and scrubber (V12).
        pages->showPage ("OSC");
        settle (300);
        {
            std::vector<WaveDisplay*> waves;
            findAll<WaveDisplay> (*editor, waves);
            auto readout = juce::String();
            for (auto* wave : waves)
                if (visibleInTree (wave) && readout.isEmpty())
                    readout = wave->getFrameReadout();
            expect (readout.startsWith ("frame ") && readout.contains (" of "), "the WAVE view reads out the frame ('" + readout + "')");
        }

        // One dimming rule (V26): a control that does nothing now dims, says
        // why on hover, and still takes edits.
        {
            auto* spectralOff = findKnob ("osc1_spectral_amt");
            const auto dimmed = spectralOff != nullptr && spectralOff->getAlpha() < 0.99f; // dim while Off (S13-2)
            setParam ("osc1_spectral", 1.0f);
            settle (500);
            auto* spectral = findKnob ("osc1_spectral_amt");
            const auto lit = spectral != nullptr && spectral->getAlpha() > 0.99f && ! spectral->getSlider().getTooltip().contains ("No effect now");
            setParam ("osc1_spectral", 0.0f);
            settle (300);
            auto* detune = findKnob ("osc1_detune");
            expect (dimmed && lit && detune != nullptr && detune->getAlpha() < 0.99f,
                    "OSC: SPEC AMT is dim while SPECTRAL is Off, lights when it is on; DETUNE dims at UNISON 1");

            pages->showPage ("FILTER");
            settle (400);
            auto* balance = findKnob ("filter_balance");
            const auto serialDim = balance != nullptr && balance->getAlpha() < 0.99f;
            setParam ("filters_parallel", 1.0f);
            settle (400);
            expect (serialDim && balance != nullptr && balance->getAlpha() > 0.99f, "FILTER: BALANCE dims in serial, not in parallel");
            setParam ("filters_parallel", 0.0f);
            settle (200);
        }

        // Three knob sizes by role (V22): FM sends are mini, OSC's core
        // knobs as large as PLAY's.
        {
            pages->showPage ("FM");
            settle (400);
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            // (2026-10-29: the sends are the ROUTES list's and the cables' small rings; a route makes them show.)
            processor.apvts.getParameter ("fm_amount")->setValueNotifyingHost (0.5f);
            settle (400);
            knobs.clear();
            findAll<KnobControl> (*editor, knobs);
            auto sends = 0, miniSends = 0;
            for (auto* knob : knobs)
                if (visibleInTree (knob) && knob->getParameterId().startsWith ("fm_") && knob->getLabelText().isEmpty())
                {
                    ++sends;
                    miniSends += knob->getWidth() <= 60 ? 1 : 0;
                }
            expect (sends > 0 && sends == miniSends, "FM: the route knobs are small (" + juce::String (miniSends) + " of " + juce::String (sends) + ")");
            processor.apvts.getParameter ("fm_amount")->setValueNotifyingHost (0.0f);

            pages->showPage ("MAIN");
            settle (300);
            auto* playLevel = findKnob ("osc1_level");
            const auto playDial = playLevel != nullptr ? playLevel->getDialSize() : 0;
            pages->showPage ("OSC");
            settle (300);
            auto* oscLevel = findKnob ("osc1_level");
            // (The approved OSC design draws every oscillator as a compact card with
            // a 26 px inline dial, so OSC's LEVEL is deliberately smaller than PLAY's.
            // The check is now that it exists and is still a usable size.)
            expect (playDial > 0 && oscLevel != nullptr && oscLevel->getDialSize() >= 24,
                    "OSC's LEVEL dial (" + juce::String (oscLevel != nullptr ? oscLevel->getDialSize() : 0) + " px) is a usable inline dial (PLAY's is "
                        + juce::String (playDial) + " px)");
        }

        // The physical card: LOAD .WAV only for a wavetable (V30).
        {
            setParam ("osc1_mode", 1.0f);
            settle (500);
            std::vector<juce::TextButton*> buttons;
            findAll<juce::TextButton> (*editor, buttons);
            auto loadShown = false;
            // (Every oscillator is a card on OSC now: look inside OSC 1's.)
            for (auto* button : buttons)
                if (visibleInTree (button) && button->getButtonText().startsWith ("LOAD")
                    && pages->getOscCardBounds (0).contains (editor->getLocalArea (button, button->getLocalBounds()).getCentre()))
                    loadShown = true;
            expect (! loadShown && findKnob ("osc1_string_decay") != nullptr && findKnob ("osc1_unison") != nullptr,
                    "a Physical card has no LOAD .WAV, and shows its string and voice rows (load "
                        + juce::String ((int) loadShown) + ", decay " + juce::String ((int) (findKnob ("osc1_string_decay") != nullptr))
                        + ", unison " + juce::String ((int) (findKnob ("osc1_unison") != nullptr)) + ")");
            setParam ("osc1_mode", 0.0f);
            settle (300);
        }

        // VECTOR: a corner whose oscillator is off says so (S27).
        {
            pages->showPage ("VECTOR");
            settle (300);
            auto* pad = findChild<VectorPadDisplay> (*editor);
            auto offCorner = -1;
            for (int corner = 0; corner < 4 && pad != nullptr; ++corner)
                if (! pad->isCornerSounding (corner))
                    offCorner = corner;
            expect (pad != nullptr && offCorner >= 0 && (pad->getCornerLabel (offCorner, 0.5f).endsWith (": off") || pad->getCornerLabel (offCorner, 0.5f).endsWith (": none"))
                        && pad->isCornerSounding (0) && ! pad->getCornerLabel (0, 0.5f).contains (":"),
                    "VECTOR: Init's corners on switched-off oscillators say so, OSC 1's does not");

            // An oscillator the patch doesn't have reads "(none)"; no share
            // shows while the vector is off (UI review 6, V29).
            auto none = false;
            for (int corner = 0; corner < 4 && pad != nullptr; ++corner)
                if (! processor.isOscillatorShown (processor.getVectorCorner (corner)))
                    none = pad->getCornerLabel (corner, 0.5f).endsWith (": none");
            auto* vecOn = processor.apvts.getParameter ("vec_on");
            const auto wasOn = vecOn->getValue();
            vecOn->setValueNotifyingHost (0.0f);
            settle (100);
            expect (none && pad != nullptr && ! pad->getCornerLabel (0, 0.5f).contains ("%"),
                    "VECTOR: a corner without its oscillator reads ': none'; no shares while the vector is off");
            vecOn->setValueNotifyingHost (wasOn);
        }

        // The synced RATE knob takes modulation for RATE (V14).
        {
            pages->showPage ("ENV/LFO");
            settle (300);
            setParam ("lfo1_sync", 1.0f);
            settle (400);
            auto* division = findKnob ("lfo1_div");
            const juce::DragAndDropTarget::SourceDetails drag (juce::var ("modsource:0"), nullptr, {});
            expect (division != nullptr && division->getRingDestination() == (int) Mod::Destination::Lfo1Rate
                        && division->isInterestedInDragSource (drag),
                    "a synced LFO's RATE knob shows RATE's modulation and takes a dropped source");
            setParam ("lfo1_sync", 0.0f);
            settle (200);
        }
        loadNamed ("Neuro Wobble");
    }

    // UI review 6, P1: rings, badges, the macro strip and the matrix.
    runModulationTests (processor, *pages);

    // Batch H (UI review 4: V19, V27, V28, S17, S20, S23, S25).
    runSmallThingsTests (processor, *pages);

    // UI review 9, T2: voice, SEQ, matrix, browser, macros.
    runReview9T2Tests (processor, *pages);

    // UI review 7, Q4: PLAY, OSC, PHYSICAL, VECTOR.
    runPlayOscReview7Tests (processor, *pages);

    // UI review 7, FILTER and FX.
    runFilterFxTests (processor, *pages);

    // UI review 8, R4: modulation.
    runModulationReview8Tests (processor, *pages);
    // UI review 8, R5: PLAY / OSC / PHYSICAL / VECTOR / FILTER / FX layout.
    runLayoutReview8Tests (processor, *pages);
    runLayoutReview9Tests (processor, *pages);
    runLayoutReview10Tests (processor, *pages);
    runOscCardOverlapTests (processor, *pages);
    runPlayPageTests (processor, *pages);
    // UI review 8, R6: text fitting, header, browser, SEQ, dialogs.
    runGlobalReview8Tests (processor, *pages);
    // Step 18: every drop-down select opens its list, click after click.
    runDropdownTests (processor, *pages);
    // Step A: the cable view.
    runPatchCableTests (processor);
    // Step 16: the oscillators' ROUTING cables.
    runOscRoutingUiTests (processor, *pages);
    // Duplicate, drag to reorder, x to remove (2026-10-26).
    runOscSlotUiTests (processor, *pages);
    // Bug hunt 2026-10-31: every visible control takes a click at its centre, at 100 % and 175 %.
    runHitReachTests (processor, *pages);
    // The RC 1.0 workflow fixes (2026-10-28).
    runWorkflowUiTests (processor, *pages);
    // The PATCH tab's LFO boxes.
    runFxLfoUiTests (processor, *pages);
    runFxCarouselUiTests (processor, *pages);
    // UI review 8, R1: operator editors and names.
    runOperatorReview8Tests (processor, *pages);
    runOscTypeTests (processor, *pages);

    // Design sweep: every pop-up draws in the design's look. A menu shown on
    // the desktop never asks its target for a look, nor does an AlertWindow
    // with no owner or JUCE's file browser: they use the process default,
    // which is the editor's own look while it is open. A knob's right-click
    // menu and its "Modulate with" submenu are windows of that look.
    {
        expect (&juce::LookAndFeel::getDefaultLookAndFeel() == &editor->getLookAndFeel()
                    && dynamic_cast<IlanaLookAndFeel*> (&editor->getLookAndFeel()) != nullptr,
                "sweep: while the editor is open its look is the default one (desktop menus, dialogs)");

        // (Real menu windows can't open under xvfb: unit-level checks.) A
        // second editor opening and closing leaves the default in place;
        // only the last one to close hands it back.
        {
            DefaultLookAndFeelScope second;
            expect (&second.get() == &editor->getLookAndFeel(), "sweep: every open editor shares one look");
        }
        expect (&juce::LookAndFeel::getDefaultLookAndFeel() == &editor->getLookAndFeel(),
                "sweep: a second editor closing keeps the default look while one is still open");

        // A menu with no look of its own (a knob's right-click menu, the
        // header's and the dock's) takes the default; its submenus take
        // their parent window's. The default's menu rows are the sheet's.
        auto& lf = juce::LookAndFeel::getDefaultLookAndFeel();
        int w = 0, h = 0;
        lf.getIdealPopupMenuItemSize ("Modulate with", false, 18, w, h);
        expect (h == IlanaLookAndFeel::menuRowHeight && lf.getPopupMenuBorderSize() == IlanaLookAndFeel::menuBorder,
                "sweep: the default look's menu rows are the design's (26 px, 5 px border)");
        const auto tip = lf.getTooltipBounds ("Frame\nWhere in the wavetable the oscillator plays.", { 100, 100 }, { 0, 0, 1000, 1000 });
        expect (tip.getWidth() <= 362 && tip.getHeight() >= 30, "sweep: a tooltip is the design's small card");
        // The one animator: a state change eases (it does not jump) and settles.
        {
            static const int key = 0;
            auto moving = false;
            IlanaTheme::fadeValue (&key, 0, 0.0f, IlanaTheme::FadeRate::slide, true, moving);
            juce::Thread::sleep (30);
            const auto early = IlanaTheme::fadeValue (&key, 0, 1.0f, IlanaTheme::FadeRate::slide, true, moving);
            const auto wasMoving = moving;
            juce::Thread::sleep (100);
            const auto settled = IlanaTheme::fadeValue (&key, 0, 1.0f, IlanaTheme::FadeRate::slide, true, moving);
            expect (early > 0.05f && early < 0.95f && wasMoving && settled > early,
                    "sweep: a switch or a choice eases to its new state (" + juce::String (early, 2) + " then " + juce::String (settled, 2) + ")");
        }
        juce::Slider probe;
        expect (dynamic_cast<IlanaLookAndFeel*> (&probe.getLookAndFeel()) != nullptr,
                "sweep: a component outside the editor (a dialog's, a file browser's) draws in the design's look");
    }

    // A dock chip's click opens its source's editor over the page (pinned,
    // the knobs it drives lit); a second click closes it.
    {
        pages->showPage ("FILTER");
        settle (200);
        std::vector<ModSourceChip*> chips;
        findAll<ModSourceChip> (*editor, chips);
        ModSourceChip* chip = nullptr;
        for (auto* candidate : chips)
            if (candidate->isVisible() && candidate->onClick != nullptr && chip == nullptr)
                chip = candidate;
        if (chip != nullptr)
        {
            chip->onClick (*chip);
            settle (200);
            auto* popover = pages->getSourcePopover();
            const auto opened = popover != nullptr && popover->isVisible() && pages->getSourcePopoverSource() == chip->getSourceIndex()
                                && pinnedModSource() == chip->getSourceIndex();
            chip->onClick (*chip);
            settle (200);
            expect (opened && ! popover->isVisible() && pinnedModSource() != chip->getSourceIndex(),
                    "a dock chip's click opens its editor over the page, pinned; a second click closes it");
        }
        else
            expect (false, "the dock's chips open their source's editor");
    }

    // An LFO's pop-up: its SHAPE sits in the title row, but the cross and the
    // rest of that row (the drag strip) still take the mouse (ilana, 2026-10-30:
    // "i cant close the lfo thing", "cant drag it either").
    {
        pages->showPage ("FILTER");
        settle (200);
        std::vector<ModSourceChip*> chips;
        findAll<ModSourceChip> (*editor, chips);
        ModSourceChip* chip = nullptr;
        for (auto* candidate : chips)
            if (candidate->isVisible() && candidate->onClick != nullptr && chip == nullptr
                && Mod::lfoIndexFor ((Mod::Source) candidate->getSourceIndex()) >= 0)
                chip = candidate;
        if (chip != nullptr)
        {
            chip->onClick (*chip);
            settle (200);
            auto* popover = pages->getSourcePopover();
            juce::TextButton* cross = nullptr;
            if (popover != nullptr)
                for (auto* child : popover->getChildren())
                    if (auto* button = dynamic_cast<juce::TextButton*> (child); button != nullptr && button->isVisible())
                        cross = button;
            const auto onCross = cross != nullptr && popover->getComponentAt (cross->getBounds().getCentre()) == cross;
            // (18, 8): the title strip, left of the source name (SourcePopover::padding is 4).
            const auto onStrip = popover != nullptr && popover->getComponentAt (juce::Point<int> (18, 8)) == popover;
            expect (onCross && onStrip, "an LFO's pop-up: the cross and the title strip take the mouse (close, hover, drag)");
            if (cross != nullptr)
                cross->triggerClick();
            settle (200);
            expect (popover != nullptr && ! popover->isVisible(), "an LFO's pop-up: the cross closes it");
        }
        else
            expect (false, "the dock has an LFO chip");
    }

    pages->setAsksBeforeReplacingEdits (askedBefore);
    editor.reset();
    expect (dynamic_cast<IlanaLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel()) == nullptr,
            "sweep: with no editor open the default look is JUCE's own again");
    std::cout << (uiFailures == 0 ? "UI TESTS PASSED" : "UI TESTS FAILED") << " (" << uiFailures << " failures)" << std::endl;
    return uiFailures == 0 ? 0 : 1;
}
} // namespace

// The editor on screen, idle, on every page: how much of a core the UI
// spends repainting when nothing moves (animations must settle).
#include <ctime>
#if JUCE_WINDOWS
 #define NOMINMAX
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif

// Process CPU time in seconds (std::clock is wall time on Windows).
double cpuSeconds()
{
   #if JUCE_WINDOWS
    FILETIME created, exited, kernel, user;
    GetProcessTimes (GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto toSeconds = [] (const FILETIME& t) { return (double) (((unsigned long long) t.dwHighDateTime << 32) | t.dwLowDateTime) * 1.0e-7; };
    return toSeconds (kernel) + toSeconds (user);
   #else
    return (double) std::clock() / CLOCKS_PER_SEC;
   #endif
}

// An opaque window holding the editor, as hosts do: a non-opaque editor put
// on the desktop directly gets a layered window that redraws in full.
struct HostWindow : juce::Component
{
    explicit HostWindow (juce::Component& editor)
    {
        setOpaque (true);
        addAndMakeVisible (editor);
        setSize (editor.getWidth(), editor.getHeight());
#if JUCE_LINUX
        // (A bare Xvfb has no window manager: the title bar's property names a missing atom.)
        if (auto* setHandler = (void* (*) (int (*) (void*, void*))) dlsym (RTLD_DEFAULT, "XSetErrorHandler"))
            setHandler ([] (void*, void*) { return 0; });
#endif
        addToDesktop (juce::ComponentPeer::windowHasTitleBar);
        setVisible (true);
        toFront (true); // a background window may get fewer vblanks
    }
    ~HostWindow() override { removeFromDesktop(); }
    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }
};

// Records what gets repainted: a transparent component over the editor
// sees the clip of every paint.
struct RegionProbe : juce::Component
{
    RegionProbe() { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override
    {
        ++frames;
        // Which 40-px cells the clip region touches (the clip's bounds
        // alone can be the whole window).
        for (int y = 0; y < getHeight(); y += 40)
            for (int x = 0; x < getWidth(); x += 40)
                if (g.clipRegionIntersects ({ x, y, 40, 40 }))
                    ++counts[juce::Rectangle<int> (x, y, 40, 40).toString()];
    }
    int frames = 0;
    std::map<juce::String, int> counts;
};

int runIdleCpu()
{
    IlanaSynthAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setSize (1060, 720);
    HostWindow window (*editor);
    settle (800);

    if (auto* tutorial = findChild<TutorialOverlay> (*editor))
        tutorial->setVisible (false);

    // Listing regions needs exact clips: Direct2D's clip tests are coarse.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_IDLE_REGIONS", "").isNotEmpty())
        if (auto* peer = window.getPeer())
            peer->setCurrentRenderingEngine (0);

    auto* pages = dynamic_cast<IlanaSynthAudioProcessorEditor*> (editor.get());
    auto worst = 0.0;
    auto idleFailures = 0;

    // ILANA_IDLE_HIDE=LogoComponent,ModSourceChip,... hides every component
    // of those classes (by their type name) to find what keeps repainting.
    const auto hide = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_IDLE_HIDE", ""), ",", "");
    if (! hide.isEmpty())
    {
        std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
        {
            for (auto* child : c.getChildren())
            {
                const juce::String type (typeid (*child).name());
                for (const auto& h : hide)
                    if (h.isNotEmpty() && type.contains (h))
                        child->setVisible (false);
                walk (*child);
            }
        };
        walk (*editor);
    }

    const auto only = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_FPS_PAGES", ""), ",", "");

    for (const auto& id : pages->getPageIds())
    {
        if (! only.isEmpty() && ! only.contains (id))
            continue;
        pages->showPage (id);
        // Let entrance animations finish (ILANA_IDLE_SETTLE=<ms>: the MATRIX
        // cable plays for a few seconds).
        settle (juce::jmax (id == "MATRIX" ? 8000 : 1200,
                            juce::SystemStats::getEnvironmentVariable ("ILANA_IDLE_SETTLE", "").getIntValue()));
        RegionProbe regions;
        const auto listRegions = juce::SystemStats::getEnvironmentVariable ("ILANA_IDLE_REGIONS", "").isNotEmpty();
        // (Always attached: the paints in the window are the idle check below.)
        editor->addAndMakeVisible (regions);
        regions.setBounds (editor->getLocalBounds());
        const auto start = cpuSeconds();
        settle (3000);
        const auto percent = 100.0 * (cpuSeconds() - start) / 3.0;
        worst = juce::jmax (worst, percent);
        std::cout << "idle " << id << ": " << juce::String (percent, 1) << "% of a core, " << regions.frames << " paints in 3 s" << std::endl;
        // An idle page redraws a few times a second at most (a clock
        // readout, the 2 s safety repaint). A page that keeps animating when
        // nothing sounds or moves is the bug this catches.
        if (regions.frames > 45)
        {
            std::cout << "FAIL: " << id << " keeps repainting while idle (" << regions.frames << " paints in 3 s, at most 45)" << std::endl;
            idleFailures++;
        }
        editor->removeChildComponent (&regions);
        if (listRegions)
        {
            std::vector<std::pair<int, juce::String>> sorted;
            for (auto& [rect, n] : regions.counts)
                sorted.push_back ({ n, rect });
            std::sort (sorted.rbegin(), sorted.rend());
            std::cout << "  " << regions.frames << " paints in 3 s" << std::endl;
            for (auto* top : editor->getChildren())
                for (auto* child : top->getChildren())
                    if (child->isVisible())
                        std::cout << "    child " << typeid (*child).name() << " " << child->getBounds().toString() << std::endl;
            for (size_t i = 0; i < juce::jmin<size_t> (40, sorted.size()); ++i)
            {
                const auto r = juce::Rectangle<int>::fromString (sorted[i].second);
                auto* at = editor->getComponentAt (r.getCentre());
                juce::String path;
                for (auto* c = at; c != nullptr && c != editor.get(); c = c->getParentComponent())
                    path = juce::String (typeid (*c).name()).fromLastOccurrenceOf (" ", false, false) + (path.isEmpty() ? "" : " > " + path);
                std::cout << "  " << sorted[i].first << "x " << sorted[i].second << "  " << path.substring (juce::jmax (0, path.length() - 160)) << std::endl;
            }
        }
    }

    pages->setScopeOpen (true);
    settle (1200);
    const auto start = cpuSeconds();
    settle (3000);
    std::cout << "idle SCOPE panel: " << juce::String (100.0 * (cpuSeconds() - start) / 3.0, 1) << "% of a core" << std::endl;
    std::cout << "worst page: " << juce::String (worst, 1) << "%" << std::endl;
    window.removeChildComponent (editor.get());
    return idleFailures == 0 ? 0 : 1;
}

// Frames the editor actually paints while it animates: a page switch, then a
// held note (scopes, meters and modulation move). A transparent component
// over the whole editor is painted whenever anything under it is, so its
// paints are the frames drawn. 60 fps means gaps of at most ~17 ms.
struct FrameProbe : juce::Component
{
    FrameProbe() { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics&) override { times.push_back (juce::Time::getMillisecondCounterHiRes()); }
    std::vector<double> times;
};

juce::String frameStats (const std::vector<double>& times, double windowMs)
{
    if (times.size() < 2)
        return juce::String ((int) times.size()) + " frames";
    std::vector<double> gaps;
    for (size_t i = 1; i < times.size(); ++i)
        gaps.push_back (times[i] - times[i - 1]);
    std::sort (gaps.begin(), gaps.end());
    const auto p95 = gaps[(size_t) ((double) (gaps.size() - 1) * 0.95)];
    return juce::String (1000.0 * (double) times.size() / windowMs, 0) + " fps, median gap "
         + juce::String (gaps[gaps.size() / 2], 1) + " ms, p95 " + juce::String (p95, 1) + " ms, worst " + juce::String (gaps.back(), 1) + " ms";
}

int runFps()
{
    IlanaSynthAudioProcessor processor;
    const auto blockSize = juce::jmax (32, juce::SystemStats::getEnvironmentVariable ("ILANA_FPS_BLOCK", "256").getIntValue());
    processor.prepareToPlay (48000.0, blockSize);
    // ILANA_FPS_PRESET=<factory preset> and ILANA_FPS_SET="id=value;..."
    // (real values) set the patch the chord plays.
    if (const auto preset = juce::SystemStats::getEnvironmentVariable ("ILANA_FPS_PRESET", ""); preset.isNotEmpty())
        processor.loadFactoryPreset (processor.getFactoryPresetNames().indexOf (preset));
    for (const auto& pair : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_FPS_SET", ""), ";", ""))
        if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (processor.apvts.getParameter (pair.upToFirstOccurrenceOf ("=", false, false))))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (pair.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setSize (1060, 720);
    HostWindow window (*editor);
    settle (800);

    if (auto* peer = window.getPeer())
    {
        const auto engines = peer->getAvailableRenderingEngines();
        std::cout << "renderer: " << engines[peer->getCurrentRenderingEngine()] << " (of " << engines.joinIntoString (", ") << ")" << std::endl;
    }

    if (auto* ilanaEditor = dynamic_cast<IlanaSynthAudioProcessorEditor*> (editor.get()))
        std::cout << "GPU (OpenGL) UI: " << (ilanaEditor->isGpuRendering() ? "on" : "off") << std::endl;

    if (auto* tutorial = findChild<TutorialOverlay> (*editor))
        tutorial->setVisible (false);

    // The display's refresh rate.
    {
        int vblanks = 0;
        juce::VBlankAttachment counter (editor.get(), [&vblanks] { ++vblanks; });
        settle (1000);
        std::cout << "display: " << vblanks << " vblanks per second" << std::endl;
    }

    FrameProbe probe;
    editor->addAndMakeVisible (probe);
    int vblanks = 0;
    juce::VBlankAttachment vblankCounter (editor.get(), [&vblanks] { ++vblanks; });
    probe.setBounds (editor->getLocalBounds());

    // Audio in real time on another thread, a chord held while "playing".
    std::atomic<bool> running { true }, playing { false };
    std::thread audio ([&]
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        auto wasPlaying = false;
        auto next = juce::Time::getMillisecondCounterHiRes();
        while (running)
        {
            juce::MidiBuffer midi;
            const auto now = playing.load();
            if (now != wasPlaying)
                for (auto note : { 48, 55, 60, 64 })
                    midi.addEvent (now ? juce::MidiMessage::noteOn (1, note, 0.8f) : juce::MidiMessage::noteOff (1, note), 0);
            wasPlaying = now;
            buffer.clear();
            processor.processBlock (buffer, midi);
            next += (double) blockSize / 48.0;
            const auto wait = next - juce::Time::getMillisecondCounterHiRes();
            if (wait > 0.0)
                std::this_thread::sleep_for (std::chrono::microseconds ((int) (wait * 1000.0)));
        }
    });

    auto* pages = dynamic_cast<IlanaSynthAudioProcessorEditor*> (editor.get());
    const auto filter = juce::SystemStats::getEnvironmentVariable ("ILANA_FPS_PAGES", "");

    for (const auto& id : pages->getPageIds())
    {
        if (filter.isNotEmpty() && ! juce::StringArray::fromTokens (filter, ",", "").contains (id))
            continue;
        playing = false;
        settle (400);
        probe.times.clear();
        const auto cpuStart = cpuSeconds();
        pages->showPage (id);
        probe.toFront (false);
        settle (400);
        const auto switchCpu = 100.0 * (cpuSeconds() - cpuStart) / 0.4;
        const auto switchStats = frameStats (probe.times, 400.0);

        playing = true;
        settle (300);
        probe.times.clear();
        vblanks = 0;
        IlanaAnim::paintStats().clear();
        IlanaAnim::paintStatsOn() = true;
        const auto playStart = cpuSeconds();
        settle (1500);
        std::cout << id << "\n  switch:  " << switchStats << " (" << juce::String (switchCpu, 0) << "% cpu)"
                  << "\n  playing: " << frameStats (probe.times, 1500.0) << " (" << juce::String (100.0 * (cpuSeconds() - playStart) / 1.5, 0) << "% cpu, "
                  << juce::String (vblanks / 1.5, 0) << " vblanks/s)" << std::endl;
        IlanaAnim::paintStatsOn() = false;
        for (const auto& entry : IlanaAnim::paintStats())
            std::cout << "    paints/s " << entry.first << ": " << juce::String ((double) entry.second / 1.5, 0) << std::endl;
    }

    running = false;
    audio.join();
    editor->removeChildComponent (&probe);
    window.removeChildComponent (editor.get());
    return 0;
}

// Times what a host does when it opens a saved project: construct, prepare,
// restore the state, open the editor, and restore again with it open.
static int runLoadTime()
{
    const auto now = [] { return juce::Time::getMillisecondCounterHiRes(); };
    const auto pump = []
    {
        for (int i = 0; i < 20; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
    };

    auto start = now();
    auto processor = std::make_unique<IlanaSynthAudioProcessor>();
    std::cout << "construct:        " << juce::String (now() - start, 0) << " ms" << std::endl;

    start = now();
    processor->prepareToPlay (48000.0, 512);
    std::cout << "prepareToPlay:    " << juce::String (now() - start, 0) << " ms" << std::endl;

    for (const auto* name : { "Swarm", "Init" })
    {
        const auto index = processor->getFactoryPresetNames().indexOf (name);
        start = now();
        processor->loadFactoryPreset (index);
        std::cout << "load preset " << name << ": " << juce::String (now() - start, 0) << " ms" << std::endl;
    }

    processor->loadFactoryPreset (processor->getFactoryPresetNames().indexOf ("Swarm"));
    juce::MemoryBlock state;
    start = now();
    processor->getStateInformation (state);
    std::cout << "getState:         " << juce::String (now() - start, 0) << " ms (" << (int) state.getSize() << " bytes)" << std::endl;

    start = now();
    processor->setStateInformation (state.getData(), (int) state.getSize());
    std::cout << "setState:         " << juce::String (now() - start, 0) << " ms" << std::endl;

    start = now();
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor->createEditorIfNeeded());
    std::cout << "createEditor:     " << juce::String (now() - start, 0) << " ms" << std::endl;
    start = now();
    pump();
    std::cout << "editor settle:    " << juce::String (now() - start - 100.0, 0) << " ms (beyond the 100 ms pump)" << std::endl;

    start = now();
    processor->setStateInformation (state.getData(), (int) state.getSize());
    std::cout << "setState+editor:  " << juce::String (now() - start, 0) << " ms" << std::endl;

    // What a host hears while presets are clicked through with the editor open.
    struct Counter : juce::AudioProcessorListener
    {
        int changes = 0;
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { ++changes; }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    } counter;
    processor->addListener (&counter);

    for (const auto* name : { "Init", "Swarm", "Rip Bass", "Grain Choir", "Scream Lead" })
    {
        counter.changes = 0;
        start = now();
        processor->loadFactoryPreset (processor->getFactoryPresetNames().indexOf (name));
        const auto loadMs = now() - start;
        start = now();
        pump();
        std::cout << "preset " << name << " with editor: " << juce::String (loadMs, 0) << " ms, settle "
                  << juce::String (now() - start - 100.0, 0) << " ms, " << counter.changes << " host notifications" << std::endl;
    }

    processor->removeListener (&counter);
    start = now();
    pump();
    std::cout << "after settle:     " << juce::String (now() - start - 100.0, 0) << " ms (beyond the 100 ms pump)" << std::endl;

    editor.reset();
    return 0;
}

// A 1-bar clip looped in a DAW: four notes held for the whole bar, the
// note-offs and the next loop's note-ons on the same sample. Prints each
// loop's render time and the live voice count.
static int runLoopTest (const juce::String& presetName, bool allNotesOffAtLoop)
{
    IlanaSynthAudioProcessor processor;
    const auto rate = 48000.0;
    const auto blockSize = 512;
    processor.prepareToPlay (rate, blockSize);
    if (juce::File::isAbsolutePath (presetName))
        processor.loadPresetFromFile (juce::File (presetName));
    else
        processor.loadFactoryPreset (processor.getFactoryPresetNames().indexOf (presetName));

    // ILANA_LOOP_SET="id=value;id=value" sets parameters (real values) after the load.
    for (const auto& pair : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_LOOP_SET", ""), ";", ""))
        if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (processor.apvts.getParameter (pair.upToFirstOccurrenceOf ("=", false, false))))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (pair.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));

    const int notes[] { 48, 55, 60, 64 };
    const auto barSamples = (int) (rate * 2.0); // 120 BPM, 4/4
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::int64 position = 0;

    for (int loop = 0; loop < 12; ++loop)
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        auto maxVoices = 0;

        for (int offset = 0; offset < barSamples; offset += blockSize)
        {
            juce::MidiBuffer midi;
            const auto n = juce::jmin (blockSize, barSamples - offset);

            if (offset == 0)
            {
                if (loop > 0)
                {
                    if (allNotesOffAtLoop)
                        midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
                    else
                        for (auto note : notes)
                            midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
                }

                for (auto note : notes)
                    midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
            }

            buffer.setSize (2, n, false, false, true);
            buffer.clear();
            processor.processBlock (buffer, midi);
            maxVoices = juce::jmax (maxVoices, processor.getActiveVoiceCount());
            position += n;
        }

        const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
        std::cout << "loop " << loop + 1 << ": " << juce::String (100.0 * ms / 2000.0, 1) << "% of real time, up to "
                  << maxVoices << " voices" << std::endl;
    }

    // Release everything and see how long the voices take to free up.
    juce::MidiBuffer offs;
    for (auto note : notes)
        offs.addEvent (juce::MidiMessage::noteOff (1, note), 0);

    buffer.setSize (2, blockSize, false, false, true);
    for (int block = 0; block < (int) (rate * 60.0) / blockSize; ++block)
    {
        buffer.clear();
        juce::MidiBuffer none;
        processor.processBlock (buffer, block == 0 ? offs : none);

        if (processor.getActiveVoiceCount() == 0)
        {
            std::cout << "all voices free " << juce::String (block * blockSize / rate, 2) << " s after the last note-off" << std::endl;
            return 0;
        }
    }

    std::cout << processor.getActiveVoiceCount() << " voices still live 60 s after the last note-off" << std::endl;
    return 0;
}

// CPU of a preset with twelve notes held (C2 to G5, struck at once, velocity
// 100): percent of real time per second of audio, for 8 s
// (ILANA_HELD_SECONDS). The preset is a
// factory name or a file; ILANA_LOOP_SET applies as in --looptest.
static int runHeldCpu (const juce::String& presetName)
{
    IlanaSynthAudioProcessor processor;
    const auto rate = 48000.0;
    const auto blockSize = 256;
    processor.prepareToPlay (rate, blockSize);
    if (juce::File::isAbsolutePath (presetName))
        processor.loadPresetFromFile (juce::File (presetName));
    else
        processor.loadFactoryPreset (processor.getFactoryPresetNames().indexOf (presetName));
    // ILANA_HELD_CABLES=1: the preset's chain played by the ROUTING cables
    // (roadmap step 16; the same sound, the cables' code path), before
    // ILANA_LOOP_SET, so that can then change cable_* masks.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_HELD_CABLES", "").isNotEmpty())
        processor.setRouteCables (processor.getChainAsCables(), true);
    for (const auto& pair : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_LOOP_SET", ""), ";", ""))
        if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (processor.apvts.getParameter (pair.upToFirstOccurrenceOf ("=", false, false))))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (pair.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
    // ILANA_HELD_PRINT="id,id,...": print those parameters first.
    for (const auto& id : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_HELD_PRINT", ""), ",", ""))
        if (auto* parameter = processor.apvts.getParameter (id))
            std::cout << id << " = " << parameter->getCurrentValueAsText() << std::endl;

    const int notes[] { 36, 43, 48, 52, 55, 60, 64, 67, 72, 76, 79, 84 };
    // ILANA_HELD_DUMP=file: the raw output (each block's left then right
    // floats), to compare two builds sample for sample.
    std::unique_ptr<juce::FileOutputStream> dump;
    if (const auto dumpPath = juce::SystemStats::getEnvironmentVariable ("ILANA_HELD_DUMP", ""); dumpPath.isNotEmpty())
    {
        juce::File (dumpPath).deleteFile();
        dump = std::make_unique<juce::FileOutputStream> (juce::File (dumpPath));
    }
    // ILANA_HELD_NOTES=n: only the first n of them.
    const auto heldNotes = juce::jlimit (1, 12, juce::SystemStats::getEnvironmentVariable ("ILANA_HELD_NOTES", "12").getIntValue());
    juce::AudioBuffer<float> buffer (2, blockSize);
    const auto blocksPerSecond = (int) (rate / blockSize);
    double total = 0.0, sumSquares = 0.0, worstBlock = 0.0;
    const auto seconds = juce::jmax (1, juce::SystemStats::getEnvironmentVariable ("ILANA_HELD_SECONDS", "8").getIntValue());
    const auto releaseAfter = juce::SystemStats::getEnvironmentVariable ("ILANA_HELD_RELEASE", "0").getIntValue();
    const auto arp = juce::SystemStats::getEnvironmentVariable ("ILANA_HELD_ARP", "").isNotEmpty();
    // ILANA_NO_RELEASE_SILENCE=1: released voices run their whole release.
    Voice::disableReleaseSilence = juce::SystemStats::getEnvironmentVariable ("ILANA_NO_RELEASE_SILENCE", "").isNotEmpty();
    Voice::disableSlowEnvelopes = juce::SystemStats::getEnvironmentVariable ("ILANA_NO_SLOW_ENVELOPES", "").isNotEmpty();
    IlanaSynthAudioProcessor::forceVoiceThreads = juce::SystemStats::getEnvironmentVariable ("ILANA_THREADS", "-1").getIntValue();
    IlanaSynthAudioProcessor::disableFxSleep = juce::SystemStats::getEnvironmentVariable ("ILANA_NO_FX_SLEEP", "").isNotEmpty();
    for (int second = 0; second < seconds; ++second)
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        for (int block = 0; block < blocksPerSecond; ++block)
        {
            juce::MidiBuffer midi;
            // (ILANA_HELD_RESTRIKE=1: the chord again every 2 s, as in playing.)
            // (ILANA_HELD_ARP=1: instead of the chord, the sustain pedal down
            // and a new note every 0.1 s across four octaves, each let go
            // after 0.05 s: the pedal holds them all, as in playing a piano.)
            if (arp)
            {
                const auto blockIndex = second * blocksPerSecond + block;
                const auto every = juce::jmax (1, (int) (0.1 * rate / blockSize));
                if (blockIndex == 0)
                    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
                if (blockIndex % every == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 36 + (blockIndex / every * 7) % 48, (juce::uint8) 90), 0);
                if (blockIndex % every == every / 2)
                    midi.addEvent (juce::MidiMessage::noteOff (1, 36 + (blockIndex / every * 7) % 48), 0);
            }
            else
            // (ILANA_HELD_RELEASE=n: let go of the chord after n seconds.)
            if (block == 0 && second > 0 && second == releaseAfter)
                for (int n = 0; n < heldNotes; ++n)
                    midi.addEvent (juce::MidiMessage::noteOff (1, notes[n]), 0);
            if (! arp && block == 0 && (second == 0 || (second % 2 == 0 && juce::SystemStats::getEnvironmentVariable ("ILANA_HELD_RESTRIKE", "").isNotEmpty())))
                for (int n = 0; n < heldNotes; ++n)
                {
                    const auto note = notes[n];
                    if (second > 0)
                        midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
                    midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
                }
            buffer.clear();
            const auto blockStart = juce::Time::getMillisecondCounterHiRes();
            processor.processBlock (buffer, midi);
            const auto blockMs = juce::Time::getMillisecondCounterHiRes() - blockStart;
            worstBlock = juce::jmax (worstBlock, blockMs);
            if (blockMs > 1000.0 * blockSize / rate && juce::SystemStats::getEnvironmentVariable ("ILANA_HELD_SPIKES", "").isNotEmpty())
                std::cout << "  block " << second * blocksPerSecond + block << ": " << juce::String (100.0 * blockMs / (1000.0 * blockSize / rate), 0) << "%" << std::endl;
            for (int i = 0; i < blockSize; ++i)
                sumSquares += (double) buffer.getSample (0, i) * buffer.getSample (0, i);
            if (dump != nullptr)
                for (int channel = 0; channel < 2; ++channel)
                    dump->write (buffer.getReadPointer (channel), (size_t) blockSize * sizeof (float));
        }
        const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
        total += ms;
        std::cout << "second " << second + 1 << ": " << juce::String (100.0 * ms / (1000.0 * blocksPerSecond * blockSize / rate), 1)
                  << "% of real time, " << processor.getActiveVoiceCount() << " voices, "
                  << processor.getRenderingVoiceCount() << " rendering" << std::endl;
    }
    std::cout << "mean " << juce::String (100.0 * total / (1000.0 * seconds * blocksPerSecond * blockSize / rate), 1) << "%, rms "
              << juce::String (juce::Decibels::gainToDecibels ((float) std::sqrt (sumSquares / ((double) seconds * blocksPerSecond * blockSize)), -120.0f), 2)
              << " dB, worst block " << juce::String (100.0 * worstBlock / (1000.0 * blockSize / rate), 0) << "% of its time" << std::endl;
    return 0;
}

// CPU of every factory preset, then every preset file in a folder, with the
// twelve notes of --heldcpu held for 2 s: one CSV row each (index, name,
// percent of real time over the second second, voices, what it uses).
//   ilanaSnapshot --cpurank [folder] > cpu.csv   (ILANA_RANK_FACTORY=0: the folder only)
static int runCpuRank (const juce::File& folder)
{
    const auto rate = 48000.0;
    const auto blockSize = 256;
    const int notes[] { 36, 43, 48, 52, 55, 60, 64, 67, 72, 76, 79, 84 };
    const auto blocksPerSecond = (int) (rate / blockSize);

    juce::String lastTags;
    const auto measure = [&] (const std::function<void (IlanaSynthAudioProcessor&)>& load) -> std::pair<double, int>
    {
        auto processor = std::make_unique<IlanaSynthAudioProcessor>();
        processor->prepareToPlay (rate, blockSize);
        load (*processor);
        juce::AudioBuffer<float> buffer (2, blockSize);
        double ms = 0.0;
        for (int block = 0; block < 2 * blocksPerSecond; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                for (auto note : notes)
                    midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
            buffer.clear();
            const auto start = juce::Time::getMillisecondCounterHiRes();
            processor->processBlock (buffer, midi);
            if (block >= blocksPerSecond)
                ms += juce::Time::getMillisecondCounterHiRes() - start;
        }
        // What the patch uses, for finding what heavy presets share.
        juce::StringArray tags;
        const auto text = [&processor] (const juce::String& id)
        {
            auto* parameter = processor->apvts.getParameter (id);
            return parameter != nullptr ? parameter->getCurrentValueAsText() : juce::String();
        };
        const auto on = [&processor] (const juce::String& id)
        {
            auto* parameter = processor->apvts.getParameter (id);
            return parameter != nullptr && parameter->getValue() > 0.5f;
        };
        for (const auto& osc : { "osc1", "osc2", "sub", "osc4", "osc5", "osc6" })
            if (on (juce::String (osc) + "_on"))
                tags.add (juce::String (osc) + ":" + text (juce::String (osc) + "_mode") + "x" + text (juce::String (osc) + "_unison"));
        if (on ("west_on")) tags.add ("west");
        if (on ("res_on")) tags.add ("body:" + text ("body_type"));
        if (on ("sym_on")) tags.add ("sym");
        if (on ("oversampling")) tags.add ("oversample");
        tags.add ("q:" + text ("quality"));
        tags.add ("f1:" + text ("f1_type"));
        for (int slot = 1; slot <= 10; ++slot)
            if (auto* fx = processor->apvts.getParameter ("fx_slot" + juce::String (slot)); fx != nullptr && fx->getValue() > 0.0f)
                tags.add ("fx:" + fx->getCurrentValueAsText());
        lastTags = tags.joinIntoString (" ");
        return { 100.0 * ms / (1000.0 * blocksPerSecond * blockSize / rate), processor->getActiveVoiceCount() };
    };

    std::cout << "index,name,cpu_percent,voices,uses" << std::endl;
    const auto names = IlanaSynthAudioProcessor().getFactoryPresetNames();
    const auto factory = juce::SystemStats::getEnvironmentVariable ("ILANA_RANK_FACTORY", "1") != "0";
    for (int i = 0; factory && i < names.size(); ++i)
    {
        const auto [cpu, voices] = measure ([i] (IlanaSynthAudioProcessor& p) { p.loadFactoryPreset (i); });
        std::cout << i << ",\"" << names[i] << "\"," << juce::String (cpu, 2) << "," << voices << ",\"" << lastTags << "\"" << std::endl;
    }
    if (folder.isDirectory())
        for (const auto& file : folder.findChildFiles (juce::File::findFiles, true, "*.ilanapreset"))
        {
            const auto [cpu, voices] = measure ([file] (IlanaSynthAudioProcessor& p) { p.loadPresetFromFile (file); });
            std::cout << "user,\"" << file.getFileNameWithoutExtension() << "\"," << juce::String (cpu, 2) << "," << voices << ",\"" << lastTags << "\"" << std::endl;
        }
    return 0;
}

// Loudness of each physical exciter on Init with OSC 1 switched to Physical:
// one note held 1 s, RMS and peak in dBFS. ILANA_LOOP_SET applies as above.
static int runExciterLevels()
{
    const juce::StringArray names { "Burst", "Noise", "Saw", "Pulse", "Bow", "Hammer", "Osc In", "Tine", "Reed", "Piano", "Feedback" };
    const auto rate = 48000.0;
    const auto blockSize = 512;
    // ILANA_EXCITER_VELOCITY: the note-on velocity (100 by default).
    const auto velocity = juce::jlimit (1, 127, juce::SystemStats::getEnvironmentVariable ("ILANA_EXCITER_VELOCITY", "100").getIntValue());

    for (const auto note : { 48, 60, 72 })
    {
        std::cout << "note " << note << ":" << std::endl;

        for (int excite = 0; excite < names.size(); ++excite)
        {
            IlanaSynthAudioProcessor processor;
            processor.prepareToPlay (rate, blockSize);
            // ILANA_EXCITER_PRESET: a factory preset by name instead of Init
            // (its osc1 exciter is switched; the mode is left as it is).
            const auto presetName = juce::SystemStats::getEnvironmentVariable ("ILANA_EXCITER_PRESET", "");
            processor.loadFactoryPreset (juce::jmax (0, processor.getFactoryPresetNames().indexOf (presetName)));
            const auto set = [&processor] (const juce::String& id, float value)
            {
                if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (processor.apvts.getParameter (id)))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
            };
            if (presetName.isEmpty())
                set ("osc1_mode", 1.0f);
            set ("osc1_excite", (float) excite);
            for (const auto& pair : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_LOOP_SET", ""), ";", ""))
                set (pair.upToFirstOccurrenceOf ("=", false, false), pair.fromFirstOccurrenceOf ("=", false, false).getFloatValue());

            juce::AudioBuffer<float> buffer (2, blockSize);
            double sum = 0.0;
            float peak = 0.0f;
            int count = 0;
            // The loudest 400 ms (what a percussive sound is heard at).
            std::vector<double> squares, weighted;
            // ITU-R BS.1770 K-weighting at 48 kHz (what loudness meters hear).
            struct Biquad
            {
                double b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
                double process (double x)
                {
                    const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                    x2 = x1; x1 = x; y2 = y1; y1 = y;
                    return y;
                }
            };
            Biquad shelf { 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585 };
            Biquad highPass { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 };

            for (int block = 0; block < (int) rate / blockSize; ++block)
            {
                juce::MidiBuffer midi;
                if (block == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) velocity), 0);
                buffer.clear();
                processor.processBlock (buffer, midi);

                for (int i = 0; i < blockSize; ++i)
                {
                    const auto s = buffer.getSample (0, i);
                    sum += (double) s * s;
                    squares.push_back ((double) s * s);
                    const auto k = highPass.process (shelf.process ((double) s));
                    weighted.push_back (k * k);
                    peak = juce::jmax (peak, std::abs (s));
                    ++count;
                }
            }

            const auto loudestOver = [rate] (const std::vector<double>& squares, double seconds)
            {
                const auto window = (size_t) (seconds * rate);
                double windowSum = 0.0, loudest = 0.0;
                for (size_t i = 0; i < squares.size(); ++i)
                {
                    windowSum += squares[i] - (i >= window ? squares[i - window] : 0.0);
                    if (i + 1 >= window)
                        loudest = juce::jmax (loudest, windowSum / (double) window);
                }
                return juce::Decibels::gainToDecibels ((float) std::sqrt (loudest), -120.0f);
            };

            std::cout << "  " << names[excite].paddedRight (' ', 9) << " rms " << juce::String (juce::Decibels::gainToDecibels ((float) std::sqrt (sum / count), -120.0f), 1)
                      << " dB, loudest 400 ms " << juce::String (loudestOver (squares, 0.4), 1)
                      << " dB, 50 ms " << juce::String (loudestOver (squares, 0.05), 1)
                      << " dB, K 400 ms " << juce::String (loudestOver (weighted, 0.4), 1)
                      << " dB, peak " << juce::String (juce::Decibels::gainToDecibels (peak, -120.0f), 1) << " dB" << std::endl;
        }
    }

    return 0;
}

// Loads a saved state (as a host would hand it back) and plays a note: for
// chasing a crash the state fuzz test finds. ILANA_STRIP="Samples;Wavetables"
// removes those children first.
static int runLoadState (const juce::File& file)
{
    juce::MemoryBlock data;
    file.loadFileAsData (data);

    if (const auto strip = juce::SystemStats::getEnvironmentVariable ("ILANA_STRIP", ""); strip.isNotEmpty())
        if (auto xml = juce::AudioProcessor::getXmlFromBinary (data.getData(), (int) data.getSize()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            for (const auto& name : juce::StringArray::fromTokens (strip, ";", ""))
                tree.removeChild (tree.getChildWithName (name), nullptr);
            data.reset();
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), data);
        }

    auto target = std::make_unique<IlanaSynthAudioProcessor>();
    std::cout << "setState" << std::endl;
    target->setStateInformation (data.getData(), (int) data.getSize());
    std::cout << "flush" << std::endl;
    target->flushAsyncUpdates();
    std::cout << "play" << std::endl;
    target->prepareToPlay (48000.0, 256);
    juce::AudioBuffer<float> buffer (2, 256);
    for (int b = 0; b < 30; ++b)
    {
        buffer.clear();
        juce::MidiBuffer midi;
        if (b == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        target->processBlock (buffer, midi);
    }
    std::cout << "save" << std::endl;
    juce::MemoryBlock again;
    target->getStateInformation (again);
    target.reset();
    std::cout << "ok" << std::endl;
    return 0;
}

// A menu drawn offscreen row by row as its window draws it (border,
// section headers, separators, ticks, submenu chevrons), `lit` the row
// highlighted; several menus side by side on the page's ink.
static juce::Image renderMenuSheet (juce::LookAndFeel& lf, std::vector<std::pair<juce::PopupMenu, int>> menus, float scale, int minWidth = 0)
{
    struct Row { juce::PopupMenu::Item item; int height; };
    std::vector<std::vector<Row>> rows;
    std::vector<juce::Point<int>> sizes;
    const auto border = lf.getPopupMenuBorderSize();

    for (auto& [menu, lit] : menus)
    {
        std::vector<Row> list;
        auto width = minWidth, height = 2 * border;
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        {
            const auto& item = it.getItem();
            int w = 0, h = 0;
            if (item.isSectionHeader)
                lf.getIdealPopupMenuSectionHeaderSizeWithOptions (item.text, -1, w, h, juce::PopupMenu::Options());
            else
                lf.getIdealPopupMenuItemSize (item.text, item.isSeparator, -1, w, h);
            list.push_back ({ item, h });
            width = juce::jmax (width, w + 2 * border);
            height += h;
        }
        rows.push_back (std::move (list));
        sizes.push_back ({ width, height });
    }

    auto total = 20, tallest = 0;
    for (const auto& size : sizes)
    {
        total += size.x + 20;
        tallest = juce::jmax (tallest, size.y);
    }

    juce::Image image (juce::Image::ARGB, juce::roundToInt ((float) total * scale), juce::roundToInt ((float) (tallest + 40) * scale), true);
    juce::Graphics g (image);
    g.addTransform (juce::AffineTransform::scale (scale));
    g.fillAll (IlanaTheme::Ui::bg);
    auto x = 20;

    for (size_t m = 0; m < rows.size(); ++m)
    {
        juce::Graphics::ScopedSaveState state (g);
        g.setOrigin (x, 20);
        g.reduceClipRegion (0, 0, sizes[m].x, sizes[m].y); // (a menu background may fill its whole clip)
        lf.drawPopupMenuBackground (g, sizes[m].x, sizes[m].y);
        auto y = border;
        for (size_t i = 0; i < rows[m].size(); ++i)
        {
            const auto& [item, h] = rows[m][i];
            const juce::Rectangle<int> row (border, y, sizes[m].x - 2 * border, h);
            if (item.isSectionHeader)
                lf.drawPopupMenuSectionHeader (g, row, item.text);
            else
                lf.drawPopupMenuItem (g, row, item.isSeparator, item.isEnabled, (int) i == menus[m].second, item.isTicked,
                                      item.subMenu != nullptr, item.text, item.shortcutKeyDescription, item.image.get(),
                                      item.colour != juce::Colour() ? &item.colour : nullptr);
            y += h;
        }
        x += sizes[m].x + 20;
    }

    return image;
}

// --envdot <dir> [curve]: frames of the amp envelope graph while a note is
// held and let go, for judging the playhead's motion and trail (ffmpeg joins
// them into a GIF). `curve` is the CURVE knob, -1 to 1.
int runEnvDot (const juce::File& outDir, float curve)
{
    outDir.createDirectory();
    IlanaSynthAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    const auto setParam = [&processor] (const juce::String& id, float plain)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
    };
    setParam ("amp_attack", 0.3f);
    setParam ("amp_decay", 0.7f);
    setParam ("amp_sustain", 0.55f);
    setParam ("amp_release", 0.9f);
    setParam ("amp_curve", curve);

    EnvelopeDisplay display (processor, "amp");
    display.setSize (480, 190);
    juce::AudioBuffer<float> buffer (2, 512);
    auto frame = 0;
    const auto step = [&] (bool noteOn, bool noteOff)
    {
        // About 33 ms of audio, then the picture.
        for (int block = 0; block < 3; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0 && noteOn)
                midi.addEvent (juce::MidiMessage::noteOn (1, 57, 0.8f), 0);
            if (block == 0 && noteOff)
                midi.addEvent (juce::MidiMessage::noteOff (1, 57), 0);
            buffer.clear();
            processor.processBlock (buffer, midi);
        }
        juce::Thread::sleep (33);
        const auto image = display.createComponentSnapshot (display.getLocalBounds(), true, 2.0f);
        juce::FileOutputStream stream (outDir.getChildFile (juce::String::formatted ("f%03d.png", frame++)));
        if (stream.openedOk())
        {
            stream.setPosition (0);
            stream.truncate();
            juce::PNGImageFormat().writeImageToStream (image, stream);
        }
    };
    step (false, false);
    for (int i = 0; i < 60; ++i)
        step (i == 0, false); // 2 s held
    for (int i = 0; i < 45; ++i)
        step (false, i == 0); // 1.5 s released
    for (int i = 0; i < 15; ++i)
        step (false, false);  // and quiet
    std::cout << frame << " frames in " << outDir.getFullPathName() << std::endl;
    return 0;
}

// --motion <dir>: frames of the review-26 flair (Motion: Full), 30 a
// second, one folder per scene, for the PR's recordings.
juce::Button* findButtonWithText (juce::Component& parent, const juce::String& text)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* button = dynamic_cast<juce::Button*> (child); button != nullptr && (button->getButtonText() == text || button->getName() == text) && IlanaAnim::showing (*button))
            return button;
        if (auto* nested = findButtonWithText (*child, text))
            return nested;
    }
    return nullptr;
}

int runMotion (const juce::File& outDir)
{
    IlanaAnim::fullMotion() = true;
    // A frame here takes about 150 ms to render: the timed motions (pop-ups,
    // tabs, cards, shapes) run 10 times slower and a frame is taken every
    // 330 ms, so the GIFs at 30 fps play them at their real speed.
    IlanaAnim::motionClockRate() = 0.1;
    outDir.createDirectory();
    IlanaSynthAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setSize (1060, 720);
    // A bare window, so the editor counts as on screen and animates (the
    // missing window manager's atom is ignored, as for ILANA_SNAPSHOT_FLASH).
#if JUCE_LINUX
    if (auto* setHandler = (void* (*) (int (*) (void*, void*))) dlsym (RTLD_DEFAULT, "XSetErrorHandler"))
        setHandler ([] (void*, void*) { return 0; });
#endif
    editor->addToDesktop (0);
    editor->setVisible (true);
    settle (800);
    if (auto* tutorial = findChild<TutorialOverlay> (*editor))
        tutorial->setVisible (false);
    auto* shell = dynamic_cast<IlanaSynthAudioProcessorEditor*> (editor.get());

    std::atomic<bool> running { true }, playing { false };
    std::thread audio ([&]
    {
        juce::AudioBuffer<float> buffer (2, 256);
        auto wasPlaying = false;
        auto next = juce::Time::getMillisecondCounterHiRes();
        while (running)
        {
            juce::MidiBuffer midi;
            const auto now = playing.load();
            if (now != wasPlaying)
                for (auto note : { 48, 55, 60, 64 })
                    midi.addEvent (now ? juce::MidiMessage::noteOn (1, note, 0.9f) : juce::MidiMessage::noteOff (1, note), 0);
            wasPlaying = now;
            buffer.clear();
            processor.processBlock (buffer, midi);
            next += 256.0 / 48.0;
            const auto wait = next - juce::Time::getMillisecondCounterHiRes();
            if (wait > 0.0)
                std::this_thread::sleep_for (std::chrono::microseconds ((int) (wait * 1000.0)));
        }
    });

    const auto record = [&] (const juce::String& scene, int frames, std::function<void (int)> atFrame)
    {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        auto dir = outDir.getChildFile (scene);
        dir.deleteRecursively();
        dir.createDirectory();
        // (Kept in memory and written afterwards: a PNG per frame took longer than the motion.)
        std::vector<juce::Image> images;
        auto lastFrame = juce::Time::getMillisecondCounterHiRes();
        for (int frame = 0; frame < frames; ++frame)
        {
            atFrame (frame);
            settle (juce::jmax (1, 330 - (int) (juce::Time::getMillisecondCounterHiRes() - lastFrame)));
            lastFrame = juce::Time::getMillisecondCounterHiRes();
            images.push_back (editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f));
        }
        for (size_t frame = 0; frame < images.size(); ++frame)
        {
            juce::FileOutputStream stream (dir.getChildFile (juce::String::formatted ("f%03d.png", (int) frame)));
            if (stream.openedOk())
            {
                stream.setPosition (0);
                stream.truncate();
                juce::PNGImageFormat().writeImageToStream (images[frame], stream);
            }
        }
        std::cout << scene << ": " << frames << " frames in " << (int) (juce::Time::getMillisecondCounterHiRes() - started) << " ms" << std::endl;
    };
    const auto set = [&processor] (const char* id, float plain)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
    };

    shell->showPage ("MAIN");
    settle (600);
    set ("west_on", 0.0f);
    set ("subosc_on", 0.0f);
    settle (600);
    record ("switch", 30, [&] (int f) { if (f == 4) set ("west_on", 1.0f); if (f == 6) set ("subosc_on", 1.0f); if (f == 20) set ("west_on", 0.0f); });
    record ("breathe", 45, [&] (int f) { if (f == 2) playing = true; if (f == 30) playing = false; });
    record ("stereo", 30, [&] (int f)
    {
        if (f == 3 || f == 18)
            if (auto* button = findButtonWithText (*editor, "STEREO"))
                button->triggerClick();
    });
    record ("tabs", 36, [&] (int f) { if (f == 3) shell->showPage ("FX"); if (f == 15) shell->showPage ("MATRIX"); if (f == 27) shell->showPage ("MAIN"); });
    shell->showPage ("MAIN");
    settle (600);
    record ("morph", 24, [&] (int f)
    {
        if (f == 4 || f == 14)
        {
            IlanaAnim::armMorphs();
            processor.loadFactoryPreset (juce::jmin (f == 4 ? 30 : 60, (int) processor.getFactoryPresetNames().size() - 1));
            IlanaAnim::playMorphs();
        }
    });
    shell->showPage ("ENV/LFO");
    playing = true;
    settle (500);
    record ("lfo-trail", 30, [&] (int f) { if (f == 29) playing = false; });
    shell->showPage ("OSC");
    settle (500);
    record ("cards", 36, [&] (int f)
    {
        if (f == 3)
        {
            auto* add = findButtonWithText (*editor, "+  ADD OSC");
            if (add != nullptr)
                add->triggerClick();
        }
        if (f == 22)
        {
            // The last card's header x.
            juce::Button* last = nullptr;
            std::function<void (juce::Component&)> scan = [&] (juce::Component& parent)
            {
                for (auto* child : parent.getChildren())
                {
                    if (auto* button = dynamic_cast<juce::Button*> (child); button != nullptr && IlanaAnim::showing (*button)
                        && button->getTooltip().startsWith ("Remove this oscillator"))
                        last = button;
                    scan (*child);
                }
            };
            scan (*editor);
            if (last != nullptr)
                last->triggerClick();
        }
    });

    running = false;
    audio.join();
    std::cout << "motion frames in " << outDir.getFullPathName() << std::endl;
    return 0;
}

// --flair <dir>: frames of the 2026-11-01 flair at 175 % (Motion: Full),
// one folder per scene, cropped to what moves: the keyboard lighting up
// for host notes, the logo breathing, a knob's start tick, a preset name
// decoding, the scope's afterglow and the clip light's bloom.
int runFlair (const juce::File& outDir)
{
    IlanaAnim::fullMotion() = true;
    IlanaAnim::motionClockRate() = 0.1; // (as --motion: frames 330 ms apart play at real speed at 30 fps)
    outDir.createDirectory();
    IlanaSynthAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setSize (1060, 720);
#if JUCE_LINUX
    if (auto* setHandler = (void* (*) (int (*) (void*, void*))) dlsym (RTLD_DEFAULT, "XSetErrorHandler"))
        setHandler ([] (void*, void*) { return 0; });
#endif
    editor->addToDesktop (0);
    editor->setVisible (true);
    settle (800);
    if (auto* tutorial = findChild<TutorialOverlay> (*editor))
        tutorial->setVisible (false);
    auto* shell = dynamic_cast<IlanaSynthAudioProcessorEditor*> (editor.get());
    const auto set = [&processor] (const char* id, float plain)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
    };

    // Host notes: a chord, harder on the bass; `gain` turns the output up for the clip scene.
    std::atomic<bool> running { true }, playing { false }, single { false };
    std::atomic<float> gain { 1.0f };
    std::thread audio ([&]
    {
        juce::AudioBuffer<float> buffer (2, 256);
        auto wasPlaying = false;
        auto next = juce::Time::getMillisecondCounterHiRes();
        while (running)
        {
            juce::MidiBuffer midi;
            const auto now = playing.load();
            if (now != wasPlaying)
                for (auto [note, velocity] : { std::pair { 36, 1.0f }, { 60, 0.9f }, { 64, 0.6f }, { 67, 0.35f } })
                    if (! single || note == 60)
                        midi.addEvent (now ? juce::MidiMessage::noteOn (1, note, velocity) : juce::MidiMessage::noteOff (1, note), 0);
            wasPlaying = now;
            buffer.clear();
            processor.processBlock (buffer, midi);
            if (gain.load() != 1.0f)
            {
                buffer.applyGain (gain.load());
                processor.setOutputPeakForTest (buffer); // (the meter reads what the host would hear)
            }
            next += 256.0 / 48.0;
            const auto wait = next - juce::Time::getMillisecondCounterHiRes();
            if (wait > 0.0)
                std::this_thread::sleep_for (std::chrono::microseconds ((int) (wait * 1000.0)));
        }
    });

    const auto record = [&] (const juce::String& scene, juce::Rectangle<int> crop, int frames, std::function<void (int)> atFrame)
    {
        auto dir = outDir.getChildFile (scene);
        dir.deleteRecursively();
        dir.createDirectory();
        std::vector<juce::Image> images;
        auto lastFrame = juce::Time::getMillisecondCounterHiRes();
        for (int frame = 0; frame < frames; ++frame)
        {
            atFrame (frame);
            settle (juce::jmax (1, 330 - (int) (juce::Time::getMillisecondCounterHiRes() - lastFrame)));
            lastFrame = juce::Time::getMillisecondCounterHiRes();
            images.push_back (editor->createComponentSnapshot (crop.isEmpty() ? editor->getLocalBounds() : crop, true, 1.75f));
        }
        for (size_t frame = 0; frame < images.size(); ++frame)
        {
            juce::FileOutputStream stream (dir.getChildFile (juce::String::formatted ("f%03d.png", (int) frame)));
            if (stream.openedOk())
            {
                stream.setPosition (0);
                stream.truncate();
                juce::PNGImageFormat().writeImageToStream (images[frame], stream);
            }
        }
        std::cout << scene << ": " << frames << " frames" << std::endl;
    };
    const auto boundsOf = [&] (juce::Component* c, int margin) { return c == nullptr ? juce::Rectangle<int>() : editor->getLocalArea (c->getParentComponent(), c->getBounds()).expanded (margin).getIntersection (editor->getLocalBounds()); };

    IlanaTheme::uiScaleRef() = IlanaTheme::uiScaleRef() * 1.75f;
    IlanaTheme::notifyUiScaleListeners();
    shell->showPage ("MAIN");
    settle (600);

    // 1 The keyboard, open, lighting up for the host's chord and fading.
    if (auto* keys = findButtonWithText (*editor, "KEYBOARD"))
        keys->triggerClick();
    settle (800);
    record ("keyboard", boundsOf (findChild<KeyboardStrip> (*editor), 4), 24, [&] (int f) { if (f == 3) playing = true; if (f == 12) playing = false; });

    // 2 The logo breathing with the same chord.
    record ("logo", boundsOf (findChild<LogoComponent> (*editor), 6), 30, [&] (int f) { if (f == 3) playing = true; if (f == 20) playing = false; });
    if (auto* keys = findButtonWithText (*editor, "KEYBOARD"))
        keys->triggerClick();
    settle (600);

    // 3 A knob dragged from 30 % to 70 % and let go: the start tick, then its fade.
    std::vector<KnobControl*> knobs;
    findAll<KnobControl> (*editor, knobs);
    KnobControl* cutoff = nullptr;
    for (auto* knob : knobs)
        if (knob->getParameterId() == "f1_cutoff" && visibleInTree (knob))
            cutoff = knob;
    if (cutoff != nullptr)
    {
        auto& slider = cutoff->getSlider();
        slider.setValue (slider.proportionOfLengthToValue (0.3), juce::sendNotificationSync);
        settle (300);
        record ("knob", boundsOf (cutoff, 6), 22, [&] (int f)
        {
            if (f == 2)
            {
                slider.getProperties().set ("dragGhostPos", 0.3f);
                slider.getProperties().set ("dragGhostRelease", 0.0);
            }
            if (f >= 2 && f <= 8)
                slider.setValue (slider.proportionOfLengthToValue (0.3 + 0.4 * (f - 2) / 6.0), juce::sendNotificationSync);
            if (f == 10)
                slider.getProperties().set ("dragGhostRelease", juce::Time::getMillisecondCounterHiRes());
            slider.repaint();
        });
    }

    // 4 Two preset loads: the name decodes in.
    juce::Component* presetField = nullptr;
    {
        std::vector<PresetDisplay*> displays;
        findAll<PresetDisplay> (*editor, displays);
        if (! displays.empty())
            presetField = displays.front();
    }
    record ("decode", boundsOf (presetField, 4), 24, [&] (int f)
    {
        if (f == 3 || f == 14)
            processor.loadFactoryPreset (juce::jmin (f == 3 ? 30 : 60, (int) processor.getFactoryPresetNames().size() - 1));
    });

    // 5 The scope while the cutoff sweeps: the afterglow trails the change.
    processor.loadFactoryPreset (0);
    shell->showPage ("MAIN");
    settle (400);
    if (auto* scope = findButtonWithText (*editor, "SCOPE"))
        scope->triggerClick();
    settle (600);
    single = true; // (one note: a chord's wave jitters at the trigger)
    playing = true;
    record ("scope", {}, 26, [&] (int f) { set ("f1_cutoff", 300.0f * std::pow (2.0f, (float) (f % 13) * 0.45f)); });
    playing = false;
    single = false;
    if (auto* scope = findButtonWithText (*editor, "SCOPE"))
        scope->triggerClick();
    settle (600);

    // 6 The clip light: the output turned up past 0 dB, once.
    OutputMeter* meter = nullptr;
    {
        std::vector<OutputMeter*> meters;
        findAll<OutputMeter> (*editor, meters);
        if (! meters.empty())
            meter = meters.front();
    }
    record ("clip", boundsOf (meter, 6), 18, [&] (int f) { if (f == 3) { gain = 8.0f; playing = true; } if (f == 5) { playing = false; gain = 1.0f; } });

    running = false;
    audio.join();
    std::cout << "flair frames in " << outDir.getFullPathName() << std::endl;
    return 0;
}

int runUsability2 (const juce::File& outDir)
{
    IlanaAnim::fullMotion() = true;
    outDir.createDirectory();
    IlanaSynthAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setSize (1060, 720);
#if JUCE_LINUX
    if (auto* setHandler = (void* (*) (int (*) (void*, void*))) dlsym (RTLD_DEFAULT, "XSetErrorHandler"))
        setHandler ([] (void*, void*) { return 0; });
#endif
    editor->addToDesktop (0);
    editor->setVisible (true);
    settle (800);
    if (auto* tutorial = findChild<TutorialOverlay> (*editor))
        tutorial->setVisible (false);
    auto* shell = dynamic_cast<IlanaSynthAudioProcessorEditor*> (editor.get());
    IlanaTheme::uiScaleRef() = IlanaTheme::uiScaleRef() * 1.75f;
    IlanaTheme::notifyUiScaleListeners();

    const auto shoot = [&] (const juce::String& name)
    {
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.75f);
        juce::FileOutputStream stream (outDir.getChildFile (name + ".png"));
        if (stream.openedOk())
        {
            stream.setPosition (0);
            stream.truncate();
            juce::PNGImageFormat().writeImageToStream (image, stream);
        }
        std::cout << name << std::endl;
    };
    const auto mouse = [&] (juce::Point<float> at, juce::ModifierKeys mods)
    {
        // (The real pointer too: isMouseOver and getMouseXYRelative ask X where it is.)
        juce::Desktop::setMousePosition (editor->localPointToGlobal (at.roundToInt()));
        if (auto* peer = editor->getPeer())
            peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, at, mods, juce::MouseInputSource::defaultPressure,
                                    juce::MouseInputSource::defaultOrientation, juce::Time::currentTimeMillis());
    };
    const auto turn = [&] (const char* id, float plain)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
        {
            processor.getUndoManager().beginNewTransaction();
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
            parameter->endChangeGesture();
            processor.getUndoManager().beginNewTransaction();
        }
    };
    const auto knobFor = [&] (const juce::String& id) -> KnobControl*
    {
        std::vector<KnobControl*> knobs;
        findAll<KnobControl> (*editor, knobs);
        for (auto* knob : knobs)
            if (knob->getParameterId() == id && visibleInTree (knob))
                return knob;
        return nullptr;
    };

    // A preset with LFO 1 routes on more than one page.
    for (int index = 0; index < processor.getFactoryPresetNames().size(); ++index)
    {
        processor.loadFactoryPreset (index);
        auto routes = 0;
        for (int i = 0; i < Mod::maxSlots; ++i)
            routes += processor.readModSlot (i).source == Mod::Source::Lfo1 ? 1 : 0;
        if (routes >= 3)
        {
            std::cout << "preset: " << processor.getFactoryPresetNames()[index] << std::endl;
            break;
        }
    }
    shell->showPage ("MAIN");
    settle (800);

    // 1 Edited since load: RESO (PLAY) and a reverb knob (FX) turned.
    {
        shell->showPage ("FX");
        settle (500);
        std::vector<KnobControl*> knobs;
        findAll<KnobControl> (*editor, knobs);
        juce::String fxId;
        for (auto* knob : knobs)
            if (visibleInTree (knob) && fxId.isEmpty() && knob->getParameterId().isNotEmpty())
                fxId = knob->getParameterId();
        shell->showPage ("MAIN");
        settle (500);
        turn ("f1_reso", 0.62f);
        if (fxId.isNotEmpty())
            if (auto* parameter = processor.apvts.getParameter (fxId))
                parameter->setValueNotifyingHost (parameter->getValue() > 0.5f ? 0.2f : 0.8f);
        std::cout << "fx knob: " << fxId << std::endl;
    }
    settle (600);
    shoot ("1-edited-dots");

    // 2 A ring hovered: CUTOFF shows its depth and source.
    if (auto* knob = knobFor ("f1_cutoff"))
    {
        auto& slider = knob->getSlider();
        const auto centre = editor->getLocalPoint (&slider, slider.getLocalBounds().getCentre()).toFloat();
        auto found = false;
        for (float r = 10.0f; r < 60.0f && ! found; r += 1.0f)
            for (float a = -2.4f; a < 2.4f && ! found; a += 0.1f)
            {
                const auto at = centre + juce::Point<float> (std::sin (a), -std::cos (a)) * r;
                mouse (at, {});
                if (knob->hotRouting() != nullptr)
                    found = true;
            }
        std::cout << "ring hovered: " << (found ? "yes" : "no") << std::endl;
        settle (300);
        shoot ("2-ring-depth");
        mouse ({ 2.0f, 700.0f }, {});
        settle (300);
    }

    // 3 A|B hovered after B's RESO and DRIVE moved.
    if (auto* ab = findChild<ABButton> (*editor))
    {
        ab->triggerClick(); // (B starts as a copy of A)
        settle (300);
        turn ("f1_reso", 0.3f);
        turn ("f1_drive", 0.7f);
        settle (300);
        mouse (editor->getLocalPoint (ab, ab->getLocalBounds().getCentre()).toFloat(), {});
        settle (600);
        shoot ("3-ab-diff");
        std::cout << "A|B tooltip: " << ab->getTooltip() << std::endl;
        mouse ({ 2.0f, 700.0f }, {});
        settle (300);
    }

    // 4 OSC 3 (prefix "sub") switched off: its greyed TABLE hovered.
    {
        std::vector<ComboControl*> combos;
        findAll<ComboControl> (*editor, combos);
        for (auto* combo : combos)
            if (combo->getParameterId() == "sub_table" && visibleInTree (combo))
            {
                mouse (editor->getLocalPoint (combo, combo->getLocalBounds().getCentre()).toFloat(), {});
                break;
            }
        settle (600);
        shoot ("4-off-switch");
    }

    std::cout << "usability pass 2 frames in " << outDir.getFullPathName() << std::endl;
    return 0;
}

// --usability <dir>: still frames of the usability flair at 175 % (pass 1:
// the tabs' route badges, undo's note and flash, the default notch, FINE),
// full window, for the PR's pictures.
int runUsability (const juce::File& outDir)
{
    IlanaAnim::fullMotion() = true;
    outDir.createDirectory();
    IlanaSynthAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setSize (1060, 720);
#if JUCE_LINUX
    if (auto* setHandler = (void* (*) (int (*) (void*, void*))) dlsym (RTLD_DEFAULT, "XSetErrorHandler"))
        setHandler ([] (void*, void*) { return 0; });
#endif
    editor->addToDesktop (0);
    editor->setVisible (true);
    settle (800);
    if (auto* tutorial = findChild<TutorialOverlay> (*editor))
        tutorial->setVisible (false);
    auto* shell = dynamic_cast<IlanaSynthAudioProcessorEditor*> (editor.get());
    IlanaTheme::uiScaleRef() = IlanaTheme::uiScaleRef() * 1.75f;
    IlanaTheme::notifyUiScaleListeners();

    const auto shoot = [&] (const juce::String& name)
    {
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.75f);
        juce::FileOutputStream stream (outDir.getChildFile (name + ".png"));
        if (stream.openedOk())
        {
            stream.setPosition (0);
            stream.truncate();
            juce::PNGImageFormat().writeImageToStream (image, stream);
        }
        std::cout << name << std::endl;
    };
    const auto mouse = [&] (juce::Point<float> at, juce::ModifierKeys mods)
    {
        if (auto* peer = editor->getPeer())
            peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, at, mods, juce::MouseInputSource::defaultPressure,
                                    juce::MouseInputSource::defaultOrientation, juce::Time::currentTimeMillis());
    };
    const auto turn = [&] (const char* id, float plain)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
        {
            processor.getUndoManager().beginNewTransaction();
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
            parameter->endChangeGesture();
            processor.getUndoManager().beginNewTransaction();
        }
    };
    const auto knobFor = [&] (const juce::String& id) -> KnobControl*
    {
        std::vector<KnobControl*> knobs;
        findAll<KnobControl> (*editor, knobs);
        for (auto* knob : knobs)
            if (knob->getParameterId() == id && visibleInTree (knob))
                return knob;
        return nullptr;
    };

    // A preset with LFO 1 routes on more than one page.
    for (int index = 0; index < processor.getFactoryPresetNames().size(); ++index)
    {
        processor.loadFactoryPreset (index);
        auto routes = 0;
        for (int i = 0; i < Mod::maxSlots; ++i)
            routes += processor.readModSlot (i).source == Mod::Source::Lfo1 ? 1 : 0;
        if (routes >= 3)
        {
            std::cout << "preset: " << processor.getFactoryPresetNames()[index] << std::endl;
            break;
        }
    }
    shell->showPage ("MAIN");
    settle (800);

    // 1 The route badges while LFO 1 is hovered.
    highlightedModSource() = (int) Mod::Source::Lfo1;
    settle (500);
    shoot ("1-tab-badges");
    highlightedModSource() = 0;
    settle (500);

    // 2 Undo on this page: RESO turned, then undone.
    turn ("f1_reso", 0.6f);
    settle (300);
    if (auto* undo = findButtonWithText (*editor, "undo"))
        undo->triggerClick();
    settle (120);
    shoot ("2-undo-here");
    settle (2600);

    // 2b Undo of a knob on another page.
    shell->showPage ("FX");
    settle (500);
    turn ("f1_reso", 0.7f);
    settle (300);
    if (auto* undo = findButtonWithText (*editor, "undo"))
        undo->triggerClick();
    settle (200);
    shoot ("2-undo-elsewhere");
    settle (2600);
    shell->showPage ("MAIN");
    settle (600);

    // 3 Hovering CUTOFF (turned away from its 12 kHz default): the notch.
    if (auto* parameter = processor.apvts.getParameter ("f1_cutoff"))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (2000.0f));
    settle (400);
    if (auto* knob = knobFor ("f1_cutoff"))
    {
        auto& slider = knob->getSlider();
        const auto centre = editor->getLocalPoint (&slider, slider.getLocalBounds().getCentre()).toFloat();
        mouse (centre, {});
        for (int i = 0; i < 12; ++i) // (an offscreen window isn't painted, so step the hover fade with throwaway pictures)
        {
            editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.75f);
            settle (40);
        }
        shoot ("3-default-notch");

        // 4 Shift-drag: FINE.
        const auto drag = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
        juce::ModifierKeys::currentModifiers = juce::ModifierKeys (juce::ModifierKeys::shiftModifier); // (events read the held keys from here)
        mouse (centre, drag);
        settle (60);
        mouse (centre.translated (0.0f, -6.0f), drag);
        settle (60);
        mouse (centre.translated (0.0f, -12.0f), drag);
        settle (200);
        shoot ("4-fine");
        juce::ModifierKeys::currentModifiers = {};
        mouse (centre.translated (0.0f, -12.0f), juce::ModifierKeys());
        settle (200);
        mouse ({ 2.0f, 700.0f }, {});
        settle (200);
    }

    std::cout << "usability frames in " << outDir.getFullPathName() << std::endl;
    return 0;
}

// The layout dump's card finder: paints one component on its own and keeps
// every big, solid, dark filled shape (a card's body) in the component's own
// coordinates, with its colour.
struct FillRecorder : public juce::LowLevelGraphicsSoftwareRenderer
{
    struct Fill { juce::Rectangle<float> area; juce::Colour colour; bool rounded; };

    explicit FillRecorder (const juce::Image& image) : juce::LowLevelGraphicsSoftwareRenderer (image) {}

    void setOrigin (juce::Point<int> o) override { transforms.getReference (transforms.size() - 1) = juce::AffineTransform::translation ((float) o.x, (float) o.y).followedBy (transforms.getLast()); LowLevelGraphicsSoftwareRenderer::setOrigin (o); }
    void addTransform (const juce::AffineTransform& t) override { transforms.getReference (transforms.size() - 1) = t.followedBy (transforms.getLast()); LowLevelGraphicsSoftwareRenderer::addTransform (t); }
    void saveState() override { transforms.add (transforms.getLast()); colours.add (colours.getLast()); LowLevelGraphicsSoftwareRenderer::saveState(); }
    void restoreState() override { if (transforms.size() > 1) { transforms.removeLast(); colours.removeLast(); } LowLevelGraphicsSoftwareRenderer::restoreState(); }
    void setFill (const juce::FillType& fill) override { colours.getReference (colours.size() - 1) = fill.isColour() ? fill.colour : juce::Colours::transparentBlack; LowLevelGraphicsSoftwareRenderer::setFill (fill); }
    void fillRoundedRectangle (const juce::Rectangle<float>& r, float corner) override { keep (r, true); LowLevelGraphicsSoftwareRenderer::fillRoundedRectangle (r, corner); }
    void fillRect (const juce::Rectangle<float>& r) override { keep (r, false); LowLevelGraphicsSoftwareRenderer::fillRect (r); }
    void fillRect (const juce::Rectangle<int>& r, bool replace) override { keep (r.toFloat(), false); LowLevelGraphicsSoftwareRenderer::fillRect (r, replace); }
    void fillPath (const juce::Path& path, const juce::AffineTransform& t) override
    {
        // A rounded rectangle path fills its bounding box almost fully.
        const auto box = path.getBounds();
        if (! box.isEmpty())
            keep (box.transformedBy (t), true);
        LowLevelGraphicsSoftwareRenderer::fillPath (path, t);
    }

    void keep (juce::Rectangle<float> r, bool rounded)
    {
        const auto colour = colours.getLast();
        if (colour.getAlpha() < 250 || r.getWidth() < 40.0f || r.getHeight() < 20.0f)
            return;
        if (colour.getRed() + colour.getGreen() + colour.getBlue() > 150)
            return;
        fills.push_back ({ r.transformedBy (transforms.getLast()), colour, rounded });
    }

    juce::Array<juce::AffineTransform> transforms { juce::AffineTransform() };
    juce::Array<juce::Colour> colours { juce::Colours::transparentBlack };
    std::vector<Fill> fills;
};

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    if (argc < 2)
    {
        std::cout << "usage: ilanaSnapshot <output dir> [factory preset index]\n"
                     "       ilanaSnapshot --uitest" << std::endl;
        return 1;
    }

    if (juce::String (argv[1]) == "--uitest")
        return runUiTests();

    if (juce::String (argv[1]) == "--usability2" && argc > 2)
        return runUsability2 (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    if (juce::String (argv[1]) == "--usability" && argc > 2)
        return runUsability (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));

    if (juce::String (argv[1]) == "--flair" && argc > 2)
        return runFlair (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));

    if (juce::String (argv[1]) == "--motion" && argc > 2)
        return runMotion (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));

    if (juce::String (argv[1]) == "--envdot" && argc > 2)
        return runEnvDot (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]), argc > 3 ? juce::String (argv[3]).getFloatValue() : 0.0f);

    // (ILANA_MOTION_LESS=1: --idlecpu and --fps measured with Motion: Less, to weigh the flair.)
    if (std::getenv ("ILANA_MOTION_LESS") != nullptr)
        IlanaAnim::fullMotion() = false;

    if (juce::String (argv[1]) == "--idlecpu")
        return runIdleCpu();

    if (juce::String (argv[1]) == "--fps")
        return runFps();

    // Still pictures are taken with Motion: Less, so a pop-up or a card is
    // drawn where it ends, not partway through growing or sliding in
    // (ILANA_SNAPSHOT_MOTION=1 keeps the motion, for recordings).
    IlanaAnim::fullMotion() = std::getenv ("ILANA_SNAPSHOT_MOTION") != nullptr;

    if (juce::String (argv[1]) == "--loadtime")
        return runLoadTime();

    if (juce::String (argv[1]) == "--loadstate" && argc > 2)
        return runLoadState (juce::File (juce::String (argv[2])));

    if (juce::String (argv[1]) == "--exciters")
        return runExciterLevels();

    if (juce::String (argv[1]) == "--cpurank")
        return runCpuRank (argc > 2 ? juce::File (juce::String (argv[2])) : juce::File());

    if (juce::String (argv[1]) == "--heldcpu" && argc > 2)
        return runHeldCpu (juce::String (argv[2]));

    if (juce::String (argv[1]) == "--looptest")
        return runLoopTest (argc > 2 ? juce::String (argv[2]) : juce::String ("Swarm"), argc > 3);

    const juce::File outDir (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    outDir.createDirectory();

    IlanaSynthAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    // A factory preset by index or by name.
    if (argc > 2)
    {
        const juce::String preset (argv[2]);
        const auto byName = processor.getFactoryPresetNames().indexOf (preset);
        processor.loadFactoryPreset (byName >= 0 ? byName : preset.getIntValue());
    }

    // ILANA_SNAPSHOT_SET="osc1_mode=1,osc1_excite=9": parameters set (in their own units) after the preset loads.
    for (const auto& assignment : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_SET", ""), ",", ""))
        if (const auto id = assignment.upToFirstOccurrenceOf ("=", false, false).trim(); id.isNotEmpty())
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (assignment.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));

    // ILANA_SNAPSHOT_COLOURS="0:ffff6fa8,14:ff3fc7e8": section colours picked
    // (keys as in SectionColours.h) after the preset loads.
    for (const auto& pair : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_COLOURS", ""), ",", ""))
        if (pair.containsChar (':'))
            processor.setSectionColour (pair.upToFirstOccurrenceOf (":", false, false).getIntValue(),
                                        (juce::uint32) pair.fromFirstOccurrenceOf (":", false, false).getHexValue64());

    // Shows the SEQ tab, which is hidden until a step LFO is in use.
    if (auto* shape = processor.apvts.getParameter ("lfo4_shape"))
        shape->setValueNotifyingHost (shape->convertTo0to1 (7.0f));

    // A held note (unless ILANA_SNAPSHOT_SILENT is set), so the scope and the
    // modulation markers are live in the pictures.
    juce::AudioBuffer<float> audio (2, 512);
    bool noteSent = false;
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_SILENT", "").isEmpty())
        beforeSave() = [&]
        {
            for (int block = 0; block < 24; ++block)
            {
                juce::MidiBuffer midi;
                if (! noteSent)
                {
                    midi.addEvent (juce::MidiMessage::noteOn (1, 48, 0.8f), 0);
                    noteSent = true;
                }
                audio.clear();
                processor.processBlock (audio, midi);
            }
            settle (60);
        };

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    // ILANA_SNAPSHOT_WIDTH renders at another zoom (795 is 75%, 2120 is 200%).
    const auto width = juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_WIDTH", "1060").getIntValue();
    editor->setSize (width, width * 720 / 1060);
    settle (400);

    // The intro only shows until it has been seen once; capture it anyway.
    if (auto* tutorial = findChild<TutorialOverlay> (*editor))
    {
        tutorial->setVisible (true);
        tutorial->toFront (false);
        settle (200);
        save (*editor, outDir.getChildFile ("00-tutorial.png"));
        tutorial->setVisible (false);
    }

    // ILANA_SNAPSHOT_CONFIRM: the question a load over an edited patch asks.
    if (auto* confirm = findChild<ConfirmOverlay> (*editor);
        confirm != nullptr && juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_CONFIRM", "").isNotEmpty())
    {
        ConfirmOverlay::Choices choices;
        choices.confirmText = "Load anyway";
        choices.alternativeText = "Save and load";
        choices.onAlternative = [] {};
        confirm->ask ("Replace your edits?",
                      "'" + processor.getCurrentPresetName() + "' has changes that aren't saved. Loading 'Init' replaces them.",
                      choices, [] (bool, bool) {});
        settle (100);
        save (*editor, outDir.getChildFile ("00-confirm.png"));
        confirm->finish (false);
    }

    auto* pages = dynamic_cast<IlanaSynthAudioProcessorEditor*> (editor.get());

    if (pages == nullptr)
        return 1;

    // ILANA_SNAPSHOT_LAYOUT: every page (and the ENV / LFO pop-ups) as
    // layout-<page>.png plus layout-<page>.json, each visible component's
    // class, words and bounds in editor pixels, clipped to what shows. The
    // layout arranger page is built from these; then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_LAYOUT", "").isNotEmpty())
    {
        const auto dump = [&] (const juce::String& name)
        {
            save (*editor, outDir.getChildFile ("layout-" + name + ".png"));
            juce::Array<juce::var> items;
            std::function<void (juce::Component&, juce::Rectangle<int>, int)> walk = [&] (juce::Component& c, juce::Rectangle<int> clip, int depth)
            {
                for (auto* child : c.getChildren())
                {
                    if (! child->isVisible() || child->getWidth() <= 0 || child->getHeight() <= 0)
                        continue;
                    const auto area = editor->getLocalArea (child, child->getLocalBounds());
                    const auto shown = area.getIntersection (clip);
                    if (shown.isEmpty())
                        continue;
                    juce::DynamicObject::Ptr item = new juce::DynamicObject();
                   #if defined (__GNUC__)
                    int status = 0;
                    auto* demangled = abi::__cxa_demangle (typeid (*child).name(), nullptr, nullptr, &status);
                    item->setProperty ("cls", juce::String (status == 0 && demangled != nullptr ? demangled : typeid (*child).name()));
                    std::free (demangled);
                   #else
                    item->setProperty ("cls", juce::String (typeid (*child).name()));
                   #endif
                    item->setProperty ("d", depth);
                    item->setProperty ("x", area.getX());
                    item->setProperty ("y", area.getY());
                    item->setProperty ("w", area.getWidth());
                    item->setProperty ("h", area.getHeight());
                    if (shown != area)
                        item->setProperty ("clip", juce::Array<juce::var> { shown.getX(), shown.getY(), shown.getWidth(), shown.getHeight() });
                    if (child->getName().isNotEmpty())
                        item->setProperty ("name", child->getName());
                    if (auto* knob = dynamic_cast<KnobControl*> (child))
                        item->setProperty ("text", knob->getLabelText()), item->setProperty ("param", knob->getParameterId());
                    else if (auto* combo = dynamic_cast<ComboControl*> (child))
                        item->setProperty ("text", combo->getLabelText()), item->setProperty ("param", combo->getParameterId());
                    else if (auto* toggle = dynamic_cast<ToggleControl*> (child))
                        item->setProperty ("param", toggle->getParameterId());
                    else if (auto* label = dynamic_cast<juce::Label*> (child))
                        item->setProperty ("text", label->getText());
                    else if (auto* button = dynamic_cast<juce::Button*> (child))
                        item->setProperty ("text", button->getButtonText());
                    if (const auto cards = child->getProperties()["playCards"].toString(); cards.isNotEmpty())
                        item->setProperty ("cards", cards);
                    {
                        // The card bodies this component paints (not its children's).
                        juce::Image scratch (juce::Image::ARGB, juce::jmax (1, child->getWidth()), juce::jmax (1, child->getHeight()), true);
                        FillRecorder recorder (scratch);
                        juce::Graphics g (recorder);
                        child->paint (g);
                        juce::Array<juce::var> fills;
                        for (const auto& f : recorder.fills)
                        {
                            const auto r = editor->getLocalArea (child, f.area.getSmallestIntegerContainer());
                            fills.add (juce::Array<juce::var> { r.getX(), r.getY(), r.getWidth(), r.getHeight(), f.colour.toDisplayString (false), f.rounded });
                        }
                        if (! fills.isEmpty())
                            item->setProperty ("fills", fills);
                    }
                    items.add (juce::var (item.get()));
                    walk (*child, shown, depth + 1);
                }
            };
            walk (*editor, editor->getLocalBounds(), 0);
            juce::DynamicObject::Ptr root = new juce::DynamicObject();
            root->setProperty ("page", name);
            root->setProperty ("w", editor->getWidth());
            root->setProperty ("h", editor->getHeight());
            root->setProperty ("items", items);
            outDir.getChildFile ("layout-" + name + ".json").replaceWithText (juce::JSON::toString (juce::var (root.get()), true));
        };
        for (const auto& page : pages->getPageIds())
        {
            pages->showPage (page);
            settle (500);
            dump (page.replaceCharacter ('/', '-'));
        }
        // PLAY's ENVELOPE and LFO EDIT pop-ups.
        pages->showPage ("MAIN");
        settle (400);
        // (PLAY with FILTER's WEST view, then back to F1.)
        if (auto* shell = dynamic_cast<IlanaSynthAudioProcessorEditor*> (editor.get()); shell != nullptr && shell->showPlayViews != nullptr)
        {
            shell->showPlayViews (2, -1, -1);
            settle (400);
            dump ("MAIN-WEST");
            shell->showPlayViews (0, -1, -1);
            settle (300);
        }
        for (const auto* popup : { "PLAY envelope EDIT", "PLAY LFO EDIT" })
        {
            std::vector<juce::Button*> buttons;
            findAll<juce::Button> (*editor, buttons);
            for (auto* button : buttons)
                if (button->getName() == popup && visibleInTree (button))
                {
                    button->triggerClick();
                    settle (500);
                    dump (juce::String (popup).containsIgnoreCase ("envelope") ? "POPUP-ENV" : "POPUP-LFO");
                    button->triggerClick();
                    settle (300);
                    break;
                }
        }
        return 0;
    }

    // ILANA_SNAPSHOT_PICKER: the section colour picker (OSC 1's, on the
    // pop-up's card) into colour-picker.png at the snapshot scale.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_PICKER", "").isNotEmpty())
    {
        struct Card : juce::Component
        {
            void paint (juce::Graphics& g) override
            {
                g.fillAll (IlanaTheme::Ui::bg);
                g.setColour (IlanaTheme::Ui::raised);
                g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (8.0f), 8.0f);
            }
        } card;
        SectionColourPicker picker (processor, SectionColours::oscKey (0), "OSC 1");
        card.setSize (picker.getWidth() + 16, picker.getHeight() + 16);
        card.addAndMakeVisible (picker);
        picker.setTopLeftPosition (8, 8);
        save (card, outDir.getChildFile ("colour-picker.png"));
    }

    // ILANA_SNAPSHOT_PRESET: every page of one factory preset, into
    // preset-<PAGE>.png; then stop (a failing layout test's patch, seen).
    if (const auto only = juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_PRESET", ""); only.isNotEmpty())
    {
        const auto program = processor.getFactoryPresetNames().indexOf (only);
        if (program < 0)
            return 1;
        processor.loadFactoryPreset (program);
        settle (500);
        for (const auto& page : pages->getPageIds())
        {
            pages->showPage (page);
            settle (400);
            save (*editor, outDir.getChildFile ("preset-" + page + ".png"));
        }
        return 0;
    }

    // ILANA_SNAPSHOT_MENU: a menu drawn by the look-and-feel as the component
    // sheet's MENU (a section name, a hovered item, a tick, a separator, a
    // sub-menu), into menu.png at the snapshot scale; then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_MENU", "").isNotEmpty())
    {
        auto& lf = juce::LookAndFeel::getDefaultLookAndFeel(); // (what a desktop menu draws with)
        const auto scale = (float) juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_SCALE", "1.5").getDoubleValue();
        struct Item { juce::String text; bool header, separator, highlighted, ticked, sub; };
        const Item items[] { { "ADD EFFECT", true, false, false, false, false }, { "Vowel", false, false, true, false, false },
                             { "Drive", false, false, false, true, false }, { "OTT", false, false, false, false, false },
                             { {}, false, true, false, false, false }, { "Airwindows", false, false, false, false, true } };
        const auto width = 200;
        const auto border = lf.getPopupMenuBorderSize();
        auto height = border;
        std::vector<int> heights;
        for (const auto& item : items)
        {
            int w = 0, h = 0;
            if (item.header)
                lf.getIdealPopupMenuSectionHeaderSizeWithOptions (item.text, -1, w, h, juce::PopupMenu::Options());
            else
                lf.getIdealPopupMenuItemSize (item.text, item.separator, -1, w, h);
            heights.push_back (h);
            height += heights.back();
        }
        height += border;
        juce::Image image (juce::Image::ARGB, juce::roundToInt ((float) (width + 40) * scale), juce::roundToInt ((float) (height + 40) * scale), true);
        {
            juce::Graphics g (image);
            g.addTransform (juce::AffineTransform::scale (scale));
            g.fillAll (IlanaTheme::Ui::bg);
            g.setOrigin (20, 20);
            lf.drawPopupMenuBackground (g, width, height);
            auto y = border;
            for (size_t i = 0; i < std::size (items); ++i)
            {
                const auto& item = items[i];
                const juce::Rectangle<int> row (border, y, width - 2 * border, heights[i]);
                if (item.header)
                    lf.drawPopupMenuSectionHeader (g, row, item.text);
                else
                    lf.drawPopupMenuItem (g, row, item.separator, true, item.highlighted, item.ticked, item.sub, item.text, {}, nullptr, nullptr);
                y += heights[i];
            }
        }
        const auto file = outDir.getChildFile ("menu.png");
        file.deleteFile();
        juce::FileOutputStream stream (file);
        juce::PNGImageFormat().writeImageToStream (image, stream);
        std::cout << file.getFullPathName() << std::endl;
        return 0;
    }

    // ILANA_SNAPSHOT_FLASH: a preset load while the editor is open, as it
    // plays out (the knobs a load moved flash once): flash-0.png before,
    // flash-1.png 60 ms after, flash-2.png 250 ms after; then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_FLASH", "").isNotEmpty())
    {
#if JUCE_LINUX
        // (A bare Xvfb has no window manager, so one window property names an atom that
        // does not exist; the window works without it.)
        // (xlib's own header clashes with JUCE's names, so find the call at run time.)
        if (auto* setHandler = (void* (*) (int (*) (void*, void*))) dlsym (RTLD_DEFAULT, "XSetErrorHandler"))
            setHandler ([] (void*, void*) { return 0; });
#endif
        editor->addToDesktop (0); // (the knobs only animate in a window that is showing)
        editor->setVisible (true);
        settle (400);
        processor.loadPresetByIndex (0);
        settle (400);
        save (*editor, outDir.getChildFile ("flash-0.png"));
        processor.loadPresetByIndex (juce::jmin (5, processor.getNumAllPresets() - 1));
        IlanaAnim::notePresetLoad();
        settle (60);
        save (*editor, outDir.getChildFile ("flash-1.png"));
        settle (190);
        save (*editor, outDir.getChildFile ("flash-2.png"));
        return 0;
    }

    // ILANA_SNAPSHOT_STRIP: the OSC tab's one-row strip with the strings' tuning open
    // (strip-tuning.png), then the macros sliding to page 2 (strip-slide-0..5.png, 50 ms
    // apart, and strip-page2.png), then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_STRIP", "").isNotEmpty())
    {
#if JUCE_LINUX
        if (auto* setHandler = (void* (*) (int (*) (void*, void*))) dlsym (RTLD_DEFAULT, "XSetErrorHandler"))
            setHandler ([] (void*, void*) { return 0; });
#endif
        for (const auto* id : { "sym_on", "sym_manual", "subosc_on" })
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (1.0f);
        editor->addToDesktop (0);
        editor->setVisible (true);
        pages->showPage ("OSC");
        settle (400);
        {
            // (The strings' chevron: the one TextButton that reads a lone ">".)
            std::vector<juce::TextButton*> buttons;
            findAll<juce::TextButton> (*editor, buttons);
            for (auto* button : buttons)
                if (button->getButtonText() == juce::String::fromUTF8 ("\xe2\x80\xba") && button->isShowing() && button->getTooltip().contains ("six notes"))
                    button->triggerClick();
        }
        settle (300);
        save (*editor, outDir.getChildFile ("strip-tuning.png"));
        if (auto* pager = findChild<MacroPageSwitch> (*editor); pager != nullptr && pager->onPick != nullptr)
        {
            pager->onPick (1);
            for (int i = 0; i < 6; ++i)
            {
                settle (50);
                save (*editor, outDir.getChildFile ("strip-slide-" + juce::String (i) + ".png"));
            }
            settle (300);
            save (*editor, outDir.getChildFile ("strip-page2.png"));
        }
        return 0;
    }

    // ILANA_SNAPSHOT_FMCABLES: the FM matrix with three routes (OSC 2 into OSC 1, OSC 1 into
    // OSC 2, noise into OSC 2 and OSC 1): the cables under the frosted cells, three frames
    // apart (the pulses run); fmcables-0/1/2.png, then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_FMCABLES", "").isNotEmpty())
    {
#if JUCE_LINUX
        if (auto* setHandler = (void* (*) (int (*) (void*, void*))) dlsym (RTLD_DEFAULT, "XSetErrorHandler"))
            setHandler ([] (void*, void*) { return 0; });
#endif
        const auto setParam = [&] (const char* id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };
        setParam ("fm_amount", 0.30f);   // OSC 2 into OSC 1
        setParam ("fm_1to2", 0.15f);     // OSC 1 into OSC 2
        setParam ("fm_noise2", 0.45f);   // noise into OSC 2
        setParam ("fm_noise1", 0.20f);   // noise into OSC 1
        editor->addToDesktop (0);
        editor->setVisible (true);
        pages->showPage ("FM");
        settle (500);
        for (int i = 0; i < 3; ++i)
        {
            save (*editor, outDir.getChildFile ("fmcables-" + juce::String (i) + ".png"));
            settle (250);
        }
        return 0;
    }

    // ILANA_SNAPSHOT_SAMPLER: the sampler steps. OSC 1 plays a three-tone
    // sample cut into slices (the PLAYBACK row, slice lines on the wave),
    // then the zone map and the sample browser; sampler-card.png,
    // sampler-zones.png, sampler-browser.png, then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_SAMPLER", "").isNotEmpty())
    {
#if JUCE_LINUX
        if (auto* setHandler = (void* (*) (int (*) (void*, void*))) dlsym (RTLD_DEFAULT, "XSetErrorHandler"))
            setHandler ([] (void*, void*) { return 0; });
#endif
        const auto setParam = [&] (const char* id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };
        auto tones = std::make_shared<SampleData>();
        tones->sampleRate = 48000.0;
        tones->name = "Three Tones";
        tones->buffer.setSize (2, 96000);
        for (int i = 0; i < 96000; ++i)
        {
            const auto part = i / 12000, local = i % 12000;
            const auto value = (float) (0.7 * std::exp (-4.0 * local / 12000.0)
                                        * std::sin (juce::MathConstants<double>::twoPi * (110.0 * (1 + part % 4)) * local / 48000.0));
            tones->buffer.setSample (0, i, value);
            tones->buffer.setSample (1, i, value);
        }
        setParam ("osc1_mode", 2.0f);
        processor.setUserSample (0, tones, {});
        SampleSlicer::Settings slicing;
        slicing.mode = SampleSlicer::transients;
        processor.setSampleSlices (0, slicing, "Slice sample");
        setParam ("osc1_sample_playback", 1.0f);
        setParam ("osc1_sample_quality", 2.0f);
        setParam ("osc1_sample_start_rand", 0.2f);
        editor->addToDesktop (0);
        editor->setVisible (true);
        pages->showPage ("OSC");
        settle (500);
        save (*editor, outDir.getChildFile ("sampler-card.png"));
        pages->openZoneEditor (0, IlanaTheme::accent());
        settle (400);
        save (*editor, outDir.getChildFile ("sampler-zones.png"));
        pages->closeZoneEditor();
        settle (100);
        pages->openSampleBrowser (0);
        settle (400);
        save (*editor, outDir.getChildFile ("sampler-browser.png"));
        pages->closeSampleBrowser();
        return 0;
    }

    // ILANA_SNAPSHOT_WTEDITOR: the wavetable editor over OSC 1's table, in
    // each of its three modes (wteditor-draw.png, -spectrum.png,
    // -formula.png), then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_WTEDITOR", "").isNotEmpty())
    {
#if JUCE_LINUX
        if (auto* setHandler = (void* (*) (int (*) (void*, void*))) dlsym (RTLD_DEFAULT, "XSetErrorHandler"))
            setHandler ([] (void*, void*) { return 0; });
#endif
        editor->addToDesktop (0);
        editor->setVisible (true);
        if (auto* parameter = processor.apvts.getParameter ("osc1_table"))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (3.0f));
        pages->showPage ("OSC");
        settle (300);
        const auto clickButton = [&] (juce::Component& root, const juce::String& prefix)
        {
            std::vector<juce::TextButton*> buttons;
            findAll<juce::TextButton> (root, buttons);
            for (auto* button : buttons)
                if (button->getButtonText().startsWith (prefix) && visibleInTree (button))
                {
                    button->triggerClick();
                    return true;
                }
            return false;
        };
        clickButton (*editor, "EDIT TABLE");
        settle (400);
        if (auto* tableEditor = pages->getWavetableEditor())
        {
            for (const auto& [button, name] : { std::pair<const char*, const char*> { "DRAW", "draw" },
                                                { "SPECTRUM", "spectrum" }, { "FORMULA", "formula" } })
            {
                clickButton (*tableEditor, button);
                settle (300);
                save (*editor, outDir.getChildFile (juce::String ("wteditor-") + name + ".png"));
            }
        }
        return 0;
    }

    // ILANA_SNAPSHOT_SWEEP: a held chord with LFO 1 on filter 1's cutoff, the
    // filter graph with its sweep band: sweep-0.png before any audio, then
    // sweep-1.png and sweep-2.png after about 0.6 s and 1.3 s of playing; stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_SWEEP", "").isNotEmpty())
    {
       #if JUCE_LINUX
        if (auto* setHandler = (void* (*) (int (*) (void*, void*))) dlsym (RTLD_DEFAULT, "XSetErrorHandler"))
            setHandler ([] (void*, void*) { return 0; });
       #endif
        editor->addToDesktop (0);
        editor->setVisible (true);
        const auto set = [&processor] (const char* id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };
        processor.loadPresetByIndex (0);
        set ("f1_cutoff", 900.0f);
        set ("f1_reso", 0.45f);
        set ("mod1_src", (float) Mod::Source::Lfo1);
        set ("mod1_dst", (float) Mod::Destination::Filter1Cutoff);
        set ("mod1_amt", 0.8f);
        settle (400);
        save (*editor, outDir.getChildFile ("sweep-0.png"));
        processor.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        const auto play = [&] (int blocks, bool chord)
        {
            for (int b = 0; b < blocks; ++b)
            {
                juce::MidiBuffer midi;
                if (chord && b == 0)
                    for (const auto note : { 48, 55, 60, 64 })
                        midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);
                buffer.clear();
                processor.processBlock (buffer, midi);
                if (b % 8 == 0)
                    settle (20);
            }
        };
        play (56, true);
        save (*editor, outDir.getChildFile ("sweep-1.png"));
        play (64, false);
        save (*editor, outDir.getChildFile ("sweep-2.png"));
        return 0;
    }

    // ILANA_SNAPSHOT_WIDGETS: the real pop-ups, drawn by the look the user
    // gets (the process default while the editor is open), then stop:
    //   widgets-knob-menu.png   FRAME's right-click menu with "Modulate with"
    //                           opened by the keys (the keyboard highlight)
    //   widgets-combo-menu.png  a type box's list
    //   widgets-sheet.png       tooltip, text fields (typing, with a selection,
    //                           and empty), scroll bar, progress, call-out box
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_WIDGETS", "").isNotEmpty())
    {
        const auto scale = (float) juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_SCALE", "1.5").getDoubleValue();
        auto& lf = juce::LookAndFeel::getDefaultLookAndFeel();
        const auto writePng = [&outDir] (const juce::Image& image, const juce::String& name)
        {
            const auto file = outDir.getChildFile (name);
            file.deleteFile();
            juce::FileOutputStream stream (file);
            juce::PNGImageFormat().writeImageToStream (image, stream);
            std::cout << file.getFullPathName() << std::endl;
        };

        // Offscreen, as the menu windows draw them (a real menu window
        // can't open under xvfb): the look is the process default, which
        // is what a desktop menu with no look of its own takes.
        pages->showPage ("MAIN");
        settle (400);

        std::vector<KnobControl*> knobs;
        findAll<KnobControl> (*editor, knobs);
        KnobControl* knob = nullptr;
        for (auto* candidate : knobs)
            if (knob == nullptr && visibleInTree (candidate) && candidate->getNumRings() > 0)
                knob = candidate;

        if (knob != nullptr)
        {
            // The menu with "Modulate with" lit (keys), its submenu beside it.
            const auto menu = knob->buildModMenu();
            juce::PopupMenu sub;
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                if (it.getItem().subMenu != nullptr && sub.getNumItems() == 0)
                    sub = *it.getItem().subMenu;
            writePng (renderMenuSheet (lf, { { menu, 0 }, { sub, 1 } }, scale), "widgets-knob-menu.png");
        }

        std::vector<juce::ComboBox*> combos;
        findAll<juce::ComboBox> (*editor, combos);
        for (auto* combo : combos)
            if (visibleInTree (combo) && combo->getNumItems() > 3)
            {
                writePng (renderMenuSheet (combo->getLookAndFeel(), { { *combo->getRootMenu(), 2 } }, scale, combo->getWidth()),
                          "widgets-combo-menu.png");
                break;
            }

        // (No AlertWindow here: it is a top-level window, and a real window
        // can't open under xvfb. It draws through drawAlertBox in the
        // default look, which the uitest checks is the design's.)

        {
            struct Sheet : public juce::Component
            {
                void paint (juce::Graphics& g) override { g.fillAll (IlanaTheme::Ui::bg); }
            } sheet;
            sheet.setSize (640, 330);

            // A tooltip as the editor's TooltipWindow draws it.
            struct Tip : public juce::Component
            {
                juce::String text;
                void paint (juce::Graphics& g) override { getLookAndFeel().drawTooltip (g, text, getWidth(), getHeight()); }
            } tip;
            tip.text = "Frame\nWhere in the wavetable the oscillator plays. Drag a modulation source here to sweep it.";
            tip.setBounds (lf.getTooltipBounds (tip.text, { 0, 0 }, { 0, 0, 1000, 1000 }).withPosition (16, 16));
            sheet.addAndMakeVisible (tip);

            juce::TextEditor typing, empty;
            typing.setText ("Neuro Wobble");
            typing.setBounds (16, 120, 260, 30);
            empty.setTextToShowWhenEmpty ("Optional", IlanaTheme::Ui::text3);
            empty.setBounds (16, 160, 260, 30);
            for (auto* field : { &typing, &empty })
            {
                field->setFont (juce::Font (IlanaTheme::font (IlanaTheme::TextSize::body, true)));
                field->setIndents (10, 7);
                sheet.addAndMakeVisible (*field);
            }

            juce::ScrollBar scroll (true);
            scroll.setRangeLimits (0.0, 100.0);
            scroll.setCurrentRange (20.0, 35.0);
            scroll.setBounds (300, 120, 10, 160);
            sheet.addAndMakeVisible (scroll);

            double progress = 0.62;
            juce::ProgressBar bar (progress);
            bar.setBounds (16, 210, 260, 12);
            sheet.addAndMakeVisible (bar);

            juce::Label callOutText ({}, "A call-out box");
            callOutText.setJustificationType (juce::Justification::centred);
            callOutText.setColour (juce::Label::textColourId, IlanaTheme::Ui::text);
            callOutText.setSize (180, 60);

            // (A child of the sheet, not a desktop window.)
            juce::CallOutBox callOut (callOutText, { 420, 40, 20, 20 }, &sheet);
            typing.setHighlightedRegion ({ 6, 12 });
            settle (300);
            writePng (sheet.createComponentSnapshot (sheet.getLocalBounds(), true, scale), "widgets-sheet.png");
        }

        return 0;
    }

    // ILANA_SNAPSHOT_PLAY: just PLAY (UI review 12): the patch as loaded, then
    // with oscillators 4 and 5 added, then all six; then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_PLAY", "").isNotEmpty())
    {
        pages->showPage ("MAIN");
        settle (500);
        save (*editor, outDir.getChildFile ("play.png"));
        // The one-tab PLAY's other views (mockup v3, 2026-10-25): FILTER 2 in parallel on a morphing type,
        // KEYS, BOARD, the strings' tuning, ENVELOPE's EDIT pop-up and the dock's STEREO pop-up.
        {
            auto* shell = dynamic_cast<IlanaSynthAudioProcessorEditor*> (editor.get());
            const auto set = [&processor] (const char* id, float value)
            {
                if (auto* parameter = processor.apvts.getParameter (id))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
            };
            const auto click = [&] (const juce::String& name)
            {
                std::vector<juce::Button*> buttons;
                findAll<juce::Button> (*editor, buttons);
                for (auto* button : buttons)
                    if (button->getName() == name && visibleInTree (button))
                    {
                        button->triggerClick();
                        return;
                    }
            };
            if (shell != nullptr && shell->showPlayViews != nullptr)
            {
                set ("filters_parallel", 1.0f);
                set ("f2_type", (float) FilterType::VowelMorph);
                set ("sym_on", 1.0f);
                set ("sym_manual", 1.0f);
                shell->showPlayViews (1, 1, 1);
                settle (600);
                save (*editor, outDir.getChildFile ("play-views.png"));
                shell->showPlayViews (-1, 0, -1);
                click ("Tuning");
                settle (400);
                save (*editor, outDir.getChildFile ("play-tuning.png"));
                click ("Tuning");
                click ("PLAY envelope EDIT");
                settle (500);
                save (*editor, outDir.getChildFile ("play-edit-popup.png"));
                // The decay handle hovered: its value beside it (Impeccable re-score toward 35).
                {
                    std::vector<EnvelopeDisplay*> displays;
                    findAll<EnvelopeDisplay> (*editor, displays);
                    EnvelopeDisplay* biggest = nullptr;
                    for (auto* display : displays)
                        if (visibleInTree (display) && ! display->showsHandlesOnHoverOnly())
                            biggest = display;
                    if (biggest != nullptr)
                    {
                        const auto at = biggest->getHandlePosition (1);
                        auto& component = static_cast<juce::Component&> (*biggest);
                        component.mouseMove (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(),
                                                               1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component, juce::Time::getCurrentTime(),
                                                               at, juce::Time::getCurrentTime(), 1, false));
                        settle (200);
                        save (*editor, outDir.getChildFile ("play-edit-popup-hover.png"));
                    }
                }
                click ("PLAY envelope EDIT");
                settle (300);
                click ("PLAY LFO EDIT");
                settle (500);
                save (*editor, outDir.getChildFile ("play-edit-lfo-popup.png"));
                // A simulated shape: TRIGGER, FIRE and every knob in the compact pop-up.
                set ("lfo1_shape", (float) LfoSimShapes::Pendulum);
                settle (500);
                save (*editor, outDir.getChildFile ("play-edit-lfo-sim-popup.png"));
                set ("lfo1_shape", 0.0f);
                settle (300);
                click ("PLAY LFO EDIT");
                settle (300);
                click ("STEREO");
                settle (400);
                save (*editor, outDir.getChildFile ("play-stereo-popup.png"));
                click ("STEREO");
                // OP PITCH (FM's link, UI review 25): its own editor in the pop-up on PLAY, by its dock chip.
                set ("osc1_amp_env", (float) OperatorEg::envelopeChoice);
                settle (500);
                FmOperatorInfo::openPitchAndLfo();
                settle (500);
                save (*editor, outDir.getChildFile ("play-op-pitch-popup.png"));
                {
                    std::vector<ModSourceChip*> dockChips;
                    findAll<ModSourceChip> (*editor, dockChips);
                    for (auto* chip : dockChips)
                        if (chip->getSourceIndex() == (int) Mod::Source::OpLfo && visibleInTree (chip) && chip->onClick != nullptr)
                        {
                            chip->onClick (*chip);
                            break;
                        }
                    settle (500);
                    save (*editor, outDir.getChildFile ("play-op-lfo-popup.png"));
                }
                {
                    std::vector<juce::TextButton*> buttons;
                    findAll<juce::TextButton> (*editor, buttons);
                    for (auto* button : buttons)
                        if (button->getButtonText() == juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) && button->getName() != "removeRoute" && visibleInTree (button))
                        {
                            button->triggerClick();
                            break;
                        }
                }
                set ("osc1_amp_env", 0.0f);
                set ("filters_parallel", 0.0f);
                set ("f2_type", 0.0f);
                set ("sym_on", 0.0f);
                set ("sym_manual", 0.0f);
                shell->showPlayViews (0, 0, 0);
                settle (400);
            }
        }
        // Three open (OSC 2 and 3 switched on; OSC 3 is "sub"): three two-row cards fill the column.
        for (const char* id : { "osc2_on", "sub_on" })
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (1.0f);
        settle (500);
        // The pin on the SUB + NOISE seam: SUB + NOISE below the scrolling cards.
        const auto setPin = [&] (bool pinned)
        {
            std::vector<juce::Button*> buttons;
            findAll<juce::Button> (*editor, buttons);
            for (auto* button : buttons)
                if (button->getName() == "pin" && button->getToggleState() != pinned)
                {
                    button->triggerClick();
                    break;
                }
            settle (500);
        };
        setPin (false);
        save (*editor, outDir.getChildFile ("play-3osc.png"));
        // Scrolled to the end: the + ADD OSC 4 row under the third card.
        {
            std::vector<DashedAddButton*> adds;
            findAll<DashedAddButton> (*editor, adds);
            for (auto* add : adds)
                if (visibleInTree (add) && add->getButtonText().contains ("ADD OSC"))
                    if (auto* view = add->findParentComponentOfClass<juce::Viewport>())
                    {
                        view->setViewPosition (0, 1 << 20);
                        settle (300);
                        save (*editor, outDir.getChildFile ("play-3osc-scrolled.png"));
                        view->setViewPosition (0, 0);
                        settle (300);
                        break;
                    }
        }
        setPin (true);
        save (*editor, outDir.getChildFile ("play-3osc-pinned.png"));
        setPin (false);
        // The other engines' cards: a string, a sample and grains.
        for (const auto& [id, mode] : { std::pair<const char*, float> { "osc1_mode", 1.0f }, { "osc2_mode", 2.0f }, { "sub_mode", 3.0f } })
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (mode));
        settle (600);
        save (*editor, outDir.getChildFile ("play-engines.png"));
        for (const auto& [id, mode] : { std::pair<const char*, float> { "osc1_mode", 0.0f }, { "osc2_mode", 0.0f }, { "sub_mode", 0.0f } })
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (mode));
        settle (400);
        processor.addOscillator (3);
        processor.addOscillator (4);
        settle (500);
        save (*editor, outDir.getChildFile ("play-5osc.png"));
        processor.addOscillator (5);
        settle (500);
        save (*editor, outDir.getChildFile ("play-6osc.png"));
        setPin (true);
        save (*editor, outDir.getChildFile ("play-6osc-pinned.png"));
        setPin (false);
        return 0;
    }

    // ILANA_SNAPSHOT_OSC: just OSC, in the states the design is checked in
    // (design review: oscillator counts 1, 3 and 6, a Sample, a String, an
    // Operator EG, a Grain and a Live oscillator); then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_OSC", "").isNotEmpty())
    {
        const auto set = [&processor] (const juce::String& id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };
        const auto shot = [&] (const juce::String& name, int scrollTo = 0)
        {
            settle (500);
            if (auto* viewport = dynamic_cast<juce::Viewport*> (pages->getCurrentPage()))
            {
                viewport->setViewPosition (0, scrollTo);
                settle (200);
            }
            save (*editor, outDir.getChildFile (name + ".png"));
        };
        auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ilana-shot-sfz");
        folder.createDirectory();
        {
            juce::AudioBuffer<float> tone (1, 22050);
            for (int i = 0; i < tone.getNumSamples(); ++i)
                tone.setSample (0, i, 0.5f * std::sin ((float) i * 0.06f) * std::exp (-(float) i / 8000.0f) * (0.6f + 0.4f * std::sin ((float) i * 0.0011f)));
            juce::WavAudioFormat wav;
            auto file = folder.getChildFile ("tone.wav");
            file.deleteFile();
            if (auto stream = std::unique_ptr<juce::OutputStream> (file.createOutputStream()))
                if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (stream.get(), 44100.0, 1, 16, {}, 0)))
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer (tone, 0, tone.getNumSamples());
                }
            folder.getChildFile ("test.sfz").replaceWithText ("<region> sample=tone.wav lokey=36 hikey=59 pitch_keycenter=48\n"
                                                              "<region> sample=tone.wav lokey=60 hikey=84 pitch_keycenter=72\n");
        }

        // (ILANA_SNAPSHOT_SMALL: the 75 % window the UI tests also check.)
        if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_SMALL", "").isNotEmpty())
            editor->getTopLevelComponent()->setSize (795, 540);
        pages->showPage ("OSC");
        shot ("osc-as-loaded");
        processor.addOscillator (2);
        shot ("osc-3");
        processor.addOscillator (3);
        processor.addOscillator (4);
        processor.addOscillator (5);
        shot ("osc-6-top");
        shot ("osc-6-bottom", 10000);
        // One of each engine: Sample, String, FM / DX7 on the Operator EG, Grain, Live.
        set ("osc1_mode", 2.0f);
        set ("osc2_mode", 1.0f);
        set ("sub_mode", 5.0f);
        set ("sub_amp_env", 17.0f);
        set ("sub_tune", 1.0f);
        set ("osc4_mode", 3.0f);
        set ("osc5_mode", 4.0f);
        processor.loadUserSample (0, folder.getChildFile ("test.sfz"));
        settle (400);
        shot ("osc-mixed-1");
        shot ("osc-mixed-2", 330);
        shot ("osc-mixed-3", 10000);
        // The sample alone, with its unison open.
        for (const auto slot : { 1, 2, 3, 4, 5 })
            processor.removeOscillator (slot);
        shot ("osc-sample-alone");
        set ("osc1_unison", 4.0f);
        shot ("osc-sample-unison");
        set ("osc1_mode", 1.0f);
        shot ("osc-string-alone");
        set ("osc1_mode", 5.0f);
        set ("osc1_amp_env", 17.0f);
        set ("osc1_tune", 1.0f);
        shot ("osc-operator-alone");
        // FM / DX7 on another envelope: its LEVEL where OUTPUT stands, the wave.
        set ("osc1_amp_env", 0.0f);
        set ("osc1_unison", 1.0f);
        shot ("osc-fm-plain");
        set ("osc1_tune", 0.0f);
        set ("osc1_mode", 0.0f);
        shot ("osc-1");
        // The strip opened under MORE.
        {
            std::vector<juce::TextButton*> buttons;
            findAll<juce::TextButton> (*editor, buttons);
            for (auto* button : buttons)
                if (button->isShowing() && button->getButtonText().startsWith ("MORE"))
                    button->triggerClick();
        }
        set ("sym_on", 1.0f);
        set ("sym_manual", 1.0f);
        shot ("osc-strip-open");
        folder.deleteRecursively();
        return 0;
    }

    // ILANA_SNAPSHOT_FX: the FX rack in the states the design is checked in
    // (the mockup's five effects with one bypassed, a long rack, bands, the
    // delay's taps, a duplicate, Airwindows models, an empty rack); then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_FX", "").isNotEmpty())
    {
        pages->showPage ("FX");
        const auto setParam = [&] (const juce::String& id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };
        const auto clear = [&]
        {
            for (int slot = 1; slot <= IlanaSynthAudioProcessor::numFxSlots; ++slot)
            {
                const auto prefix = "fx_slot" + juce::String (slot);
                setParam (prefix, 0.0f);
                setParam (prefix + "_bypass", 0.0f);
                setParam (prefix + "_solo", 0.0f);
                setParam (prefix + "_band", 0.0f);
                setParam (prefix + "_mix", 1.0f);
            }
        };
        const auto fill = [&] (std::vector<int> types)
        {
            clear();
            auto slot = 1;
            for (const auto type : types)
                processor.assignFxSlot (slot++, type);
        };
        const auto shot = [&] (const juce::String& name)
        {
            settle (500);
            save (*editor, outDir.getChildFile ("fx-" + name + ".png"));
        };

        fill ({ 27, 2, 20, 36, 34 });
        setParam ("fx_slot4_bypass", 1.0f);
        shot ("mock");
        // The same rack routed in parallel (design round 2: the ladder rail).
        setParam ("fx_routing", 1.0f);
        shot ("mock-parallel");
        setParam ("fx_routing", 0.0f);
        fill ({ 27, 2, 20, 7, 13, 9, 18, 1 });
        shot ("eight");
        fill ({ 27, 2, 20, 7, 13, 9, 18, 1, 29, 4 });
        shot ("ten");
        fill ({ 9, 16, 29, 4 });
        setParam ("fx_taps_on", 1.0f);
        shot ("taps-gate");
        fill ({ 12, 22, 19, 21, 31, 25, 17, 10 });
        shot ("plain");
        fill ({ 2, 6, 20, 4 });
        setParam ("fx_slot2_band", 1.0f);
        setParam ("fx_slot3_band", 2.0f);
        setParam ("fx_slot4_band", 3.0f);
        shot ("bands");
        fill ({ 2, 2, 32, 34, 38, 37 });
        shot ("duplicate-aw");
        fill ({ 30, 13, 1, 3 });
        shot ("aw-all");
        // AW Delay's SYNC and DIV: off, then on (the time knob dims), and
        // StarChild2 (no time: no SYNC).
        fill ({ 35, 9, 2 });
        shot ("awdelay");
        setParam ("fx_awdelay_sync", 1.0f);
        shot ("awdelay-sync");
        setParam ("fx_awdelay_algo", 4.0f);
        shot ("awdelay-sync-doublelay");
        setParam ("fx_awdelay_algo", 2.0f);
        shot ("awdelay-starchild");
        setParam ("fx_awdelay_algo", 0.0f);
        setParam ("fx_awdelay_sync", 0.0f);

        // The effect picker (mockup "fxadd"): from the rack's + ADD EFFECT
        // tile and from slot 1's type button, at 100 % and 75 %. (Saved
        // quickly: a call-out closes itself within 200 ms under xvfb.)
        const auto picker = [&] (const juce::String& name, bool fromTitle)
        {
            settle (400);
            if (fromTitle)
            {
                std::vector<FxTypeButton*> titles;
                findAll<FxTypeButton> (*editor, titles);
                for (auto* title : titles)
                    if (visibleInTree (title) && title->getTooltip().startsWith ("Slot 1:") && title->onClick != nullptr)
                    {
                        title->onClick();
                        break;
                    }
            }
            else
            {
                std::vector<DashedAddButton*> tiles;
                findAll<DashedAddButton> (*editor, tiles);
                for (auto* tile : tiles)
                    if (visibleInTree (tile) && tile->getButtonText().contains ("ADD EFFECT"))
                    {
                        tile->triggerClick();
                        break;
                    }
            }
            settle (60);
            save (*editor, outDir.getChildFile ("fx-" + name + ".png"));
            std::vector<FxLibraryView*> views;
            findAll<FxLibraryView> (*editor, views);
            for (auto* view : views)
                if (auto* box = view->findParentComponentOfClass<juce::CallOutBox>())
                    box->dismiss();
            settle (300);
        };
        fill ({ 32, 2, 20 });
        picker ("picker-add", false);
        picker ("picker-replace", true);
        if (auto* top = editor->getTopLevelComponent())
        {
            const auto before = top->getBounds();
            top->setSize (795, 540);
            picker ("picker-replace-75", true);
            top->setBounds (before);
        }

        clear();
        shot ("empty");
        return 0;
    }

    // ILANA_SNAPSHOT_FILTERFX: FX type 42, FILTER (ilana 2026-10-11): its
    // RACK card, a Delay > FILTER > back into Delay loop on the PATCH tab
    // with the loop selected, and FILTER's mini window; then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_FILTERFX", "").isNotEmpty())
    {
        const auto setParam = [&] (const juce::String& id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };
        const auto shot = [&] (const juce::String& name)
        {
            settle (500);
            save (*editor, outDir.getChildFile ("filterfx-" + name + ".png"));
        };
        for (int slot = 1; slot <= IlanaSynthAudioProcessor::numFxSlots; ++slot)
            setParam ("fx_slot" + juce::String (slot), 0.0f);
        settle (100);
        for (const auto type : { 9, 42, 13 })
            processor.addFxEffect (type);
        setParam ("fx_delay_on", 1.0f);
        setParam ("fx_reverb_on", 1.0f);
        setParam ("fx_filter_type", (float) FilterType::LadderLow);
        setParam ("fx_filter_cutoff", 900.0f);
        setParam ("fx_filter_reso", 0.7f);
        setParam ("fx_filter_spread", 0.5f);
        pages->showPage ("FX");
        shot ("rack");

        processor.setFxPatcherOn (true);
        settle (200);
        auto graph = processor.getFxPatchGraph();
        std::map<int, PatchGraph::NodeId> nodeOf;
        for (const auto& node : graph.nodes())
            if (! node.isFixed())
                nodeOf[node.ref] = node.id;
        graph.connect ({ nodeOf[2], 0 }, { nodeOf[1], 0 }, 0.5f); // FILTER back into Delay
        processor.setFxPatchGraph (graph);
        pages->showPage ("PATCH");
        settle (200);
        if (auto* view = findChild<PatchCableView> (*editor))
        {
            const auto loop = view->getGraph().findCable ({ nodeOf[2], 0 }, { nodeOf[1], 0 });
            view->selectCable (loop);
        }
        shot ("loop");
        if (auto* view = findChild<PatchCableView> (*editor))
            for (const auto& node : view->getGraph().nodes())
                if (node.ref == 2 && node.id < (1u << 20))
                {
                    const auto centre = view->nodeBounds (node).getCentre() * view->getZoom();
                    view->mouseDoubleClick (PatchCableUi::mouse (*view, centre, false));
                    break;
                }
        shot ("window");
        return 0;
    }

    // ILANA_SNAPSHOT_FXLFO: the PATCH tab's LFO boxes (ilana 2026-10-12):
    // Delay, Delay 2, FILTER and Reverb; FX LFO 1 cabled to Delay 2 moving
    // TIME and FEEDBACK, FX LFO 2 (Sample & Hold, synced) on FILTER's cutoff; the
    // patch, an LFO selected, its cable selected, the windows, and a knob's
    // right-click menu in an effect's window; then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_FXLFO", "").isNotEmpty())
    {
        const auto setParam = [&] (const juce::String& id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };
        const auto shot = [&] (const juce::String& name)
        {
            settle (500);
            save (*editor, outDir.getChildFile ("fxlfo-" + name + ".png"));
        };
        const auto nodeCentre = [&] (PatchCableView& view, PatchGraph::NodeId id)
        {
            for (const auto& node : view.getGraph().nodes())
                if (node.id == id)
                    return view.nodeBounds (node).getCentre() * view.getZoom();
            return juce::Point<float>();
        };
        for (int slot = 1; slot <= IlanaSynthAudioProcessor::numFxSlots; ++slot)
            setParam ("fx_slot" + juce::String (slot), 0.0f);
        settle (100);
        for (const auto type : { 9, 9, 42, 13 })
            processor.addFxEffect (type);
        setParam ("fx_delay_on", 1.0f);
        setParam ("fx_reverb_on", 1.0f);
        setParam ("fx_filter_cutoff", 1200.0f);
        processor.setFxPatcherOn (true);
        settle (200);
        auto graph = processor.getFxPatchGraph();
        std::map<int, PatchGraph::NodeId> nodeOf;
        for (const auto& node : graph.nodes())
            if (! node.isFixed())
                nodeOf[node.ref] = node.id;
        for (const auto& cable : std::vector<PatchGraph::Cable> (graph.cables()))
            graph.disconnect (cable.from, cable.to);
        graph.moveNode (PatchGraph::inputNodeId, 0.0f, 100.0f);
        graph.moveNode (nodeOf[1], 160.0f, 40.0f);   // Delay
        graph.moveNode (nodeOf[2], 160.0f, 160.0f);  // Delay 2
        graph.moveNode (nodeOf[3], 360.0f, 100.0f);  // FILTER
        graph.moveNode (nodeOf[4], 560.0f, 100.0f);  // Reverb
        graph.moveNode (PatchGraph::outputNodeId, 760.0f, 100.0f);
        graph.connect ({ PatchGraph::inputNodeId, 0 }, { nodeOf[1], 0 });
        graph.connect ({ PatchGraph::inputNodeId, 0 }, { nodeOf[2], 0 });
        graph.connect ({ nodeOf[1], 0 }, { nodeOf[3], 0 });
        graph.connect ({ nodeOf[2], 0 }, { nodeOf[3], 0 });
        graph.connect ({ nodeOf[3], 0 }, { nodeOf[4], 0 });
        graph.connect ({ nodeOf[4], 0 }, { PatchGraph::outputNodeId, 0 });
        processor.setFxPatchGraph (graph);

        const auto first = processor.addFxLfo (360.0f, 260.0f);
        FxLfo::Settings slow;
        slow.rateHz = 0.4f;
        processor.setFxLfoSettings (first, slow);
        processor.setFxLfoCable (first, 1, true);
        processor.assignFxLfo (first, 1, "fx_delay_time", true, 0.4f);
        processor.assignFxLfo (first, 1, "fx_delay_feedback", true, -0.3f);
        const auto second = processor.addFxLfo (360.0f, -40.0f);
        FxLfo::Settings steps;
        steps.shape = 5;
        steps.sync = true;
        steps.division = 6; // 1/8
        steps.bipolar = false;
        steps.depth = 0.8f;
        processor.setFxLfoSettings (second, steps);
        processor.assignFxLfo (second, 2, "fx_filter_cutoff", true, 0.6f);

        pages->showPage ("PATCH");
        settle (300);
        PatchCableView* view = nullptr;
        {
            std::vector<PatchCableView*> views;
            findAll<PatchCableView> (*editor, views);
            for (auto* candidate : views)
                if (candidate->getName() == "PATCH")
                    view = candidate;
        }
        if (view == nullptr)
            return 1;
        shot ("patch");

        const auto lfoNode = (PatchGraph::NodeId) ((1u << 21) + (unsigned) first);
        view->mouseDown (PatchCableUi::mouse (*view, nodeCentre (*view, lfoNode), true));
        view->mouseUp (PatchCableUi::mouse (*view, nodeCentre (*view, lfoNode), false));
        shot ("lfo-selected");

        view->selectCable (view->getGraph().findCable ({ lfoNode, 0 }, { nodeOf[2], 0 }));
        shot ("cable-selected");

        view->mouseDoubleClick (PatchCableUi::mouse (*view, nodeCentre (*view, nodeOf[2]), false));
        view->mouseDoubleClick (PatchCableUi::mouse (*view, nodeCentre (*view, lfoNode), false));
        shot ("windows");

        // The LFO window with a drawn shape (a ramp up, a drop, a bump), SMOOTH
        // and RETRIG; then the same points as Steps.
        {
            auto drawn = slow;
            drawn.shape = 9;
            drawn.smooth = 0.06f;
            drawn.retrig = true;
            for (int i = 0; i < FxLfo::drawSteps; ++i)
            {
                const auto t = (float) i / (float) FxLfo::drawSteps;
                drawn.drawn[(size_t) i] = t < 0.5f ? -1.0f + 3.2f * t : t < 0.6f ? -0.4f - 6.0f * (t - 0.5f) : 0.6f * std::sin ((t - 0.6f) * 15.7f);
            }
            processor.setFxLfoSettings (first, drawn);
            shot ("drawn-window");
            drawn.shape = 10;
            drawn.smooth = 0.0f;
            processor.setFxLfoSettings (first, drawn);
            shot ("steps-window");
        }

        // A knob's right-click menu in Delay 2's window: assigned (TIME, its
        // amount submenu beside it) and not yet (DAMPING).
        auto& lf = juce::LookAndFeel::getDefaultLookAndFeel();
        const auto writePng = [&outDir] (const juce::Image& image, const juce::String& name)
        {
            const auto file = outDir.getChildFile (name);
            file.deleteFile();
            juce::FileOutputStream stream (file);
            juce::PNGImageFormat().writeImageToStream (image, stream);
            std::cout << file.getFullPathName() << std::endl;
        };
        std::vector<KnobControl*> knobs;
        findAll<KnobControl> (*editor, knobs);
        for (auto* knob : knobs)
        {
            auto inWindow = false;
            for (auto* parent = knob->getParentComponent(); parent != nullptr; parent = parent->getParentComponent())
                inWindow = inWindow || parent->getName().startsWith ("FX WINDOW") && ! parent->getName().contains ("LFO");
            if (! visibleInTree (knob) || ! inWindow)
                continue;
            if (knob->getParameterId() == "fx_delay_time")
            {
                const auto menu = knob->buildModMenu();
                juce::PopupMenu sub;
                for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                    if (it.getItem().subMenu != nullptr && sub.getNumItems() == 0)
                        sub = *it.getItem().subMenu;
                writePng (renderMenuSheet (lf, { { menu, 0 }, { sub, 1 } }, 1.5f), "fxlfo-menu-assigned.png");
            }
            if (knob->getParameterId() == "fx_delay_damping")
                writePng (renderMenuSheet (lf, { { knob->buildModMenu(), 0 } }, 1.5f), "fxlfo-menu-assign.png");
        }
        return 0;
    }

    // ILANA_SNAPSHOT_FXDIM: a Delay with SYNC on, its RACK card and its
    // PATCH window side by side (ilana 2026-10-22: TIME greyed in the rack
    // but not in the window); prints each window's bounds; then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_FXDIM", "").isNotEmpty())
    {
        const auto setParam = [&] (const juce::String& id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };
        for (int slot = 1; slot <= IlanaSynthAudioProcessor::numFxSlots; ++slot)
            setParam ("fx_slot" + juce::String (slot), 0.0f);
        settle (100);
        processor.addFxEffect (9);
        setParam ("fx_delay_on", 1.0f);
        setParam ("fx_delay_sync", 1.0f);
        settle (200);
        pages->showPage ("FX");
        settle (500);
        save (*editor, outDir.getChildFile ("fxdim-rack.png"));

        processor.setFxPatcherOn (true);
        pages->showPage ("PATCH");
        settle (400);
        std::vector<PatchCableView*> views;
        findAll<PatchCableView> (*editor, views);
        for (auto* view : views)
            if (view->getName() == "PATCH")
                for (const auto& node : view->getGraph().nodes())
                    if (! node.isFixed() && node.kind == 9)
                        view->mouseDoubleClick (PatchCableUi::mouse (*view, view->nodeBounds (node).getCentre() * view->getZoom(), false));
        settle (500);
        std::vector<juce::Component*> all;
        findAll<juce::Component> (*editor, all);
        for (auto* item : all)
            if (item->getName().startsWith ("FX WINDOW") && visibleInTree (item))
                std::cout << "WINDOW " << editor->getLocalArea (item, item->getLocalBounds()).toString() << std::endl;
        save (*editor, outDir.getChildFile ("fxdim-patch.png"));
        return 0;
    }

    // ILANA_SNAPSHOT_PATCHER: the FX tab's PATCHER (roadmap step 17): a rack
    // shown as cables, then a patch with a parallel split, two instances of
    // one effect and a loop, the RACK page with those instances, and the
    // patcher at 75 %; then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_PATCHER", "").isNotEmpty())
    {
        const auto setParam = [&] (const juce::String& id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };
        const auto shot = [&] (const juce::String& name)
        {
            settle (500);
            save (*editor, outDir.getChildFile ("patcher-" + name + ".png"));
        };
        for (int slot = 1; slot <= IlanaSynthAudioProcessor::numFxSlots; ++slot)
            setParam ("fx_slot" + juce::String (slot), 0.0f);
        settle (100);
        for (const auto type : { 2, 9, 13, 7 })
            processor.addFxEffect (type);
        pages->showPage ("PATCH");
        shot ("rack");

        // Drive splits into two Delays (the second its own instance, a
        // shorter echo), both into Chorus then Reverb; Reverb feeds Drive back.
        processor.addFxEffect (9);
        setParam ("fx_delay_time", 0.12f);
        setParam ("fx_delay_feedback", 0.2f);
        processor.setFxPatcherOn (true);
        settle (200);
        auto graph = processor.getFxPatchGraph();
        std::map<int, PatchGraph::NodeId> nodeOf;
        for (const auto& node : graph.nodes())
            if (! node.isFixed())
                nodeOf[node.ref] = node.id;
        for (const auto& cable : std::vector<PatchGraph::Cable> (graph.cables()))
            graph.disconnect (cable.from, cable.to);
        const auto place = [&] (PatchGraph::NodeId id, float x, float y) { graph.moveNode (id, x, y); };
        place (PatchGraph::inputNodeId, 0.0f, 80.0f);
        place (nodeOf[1], 140.0f, 80.0f);   // Drive
        place (nodeOf[2], 330.0f, 20.0f);   // Delay 1
        place (nodeOf[5], 330.0f, 140.0f);  // Delay 2
        place (nodeOf[4], 520.0f, 80.0f);   // Chorus
        place (nodeOf[3], 710.0f, 80.0f);   // Reverb
        place (PatchGraph::outputNodeId, 900.0f, 80.0f);
        const auto wire = [&] (PatchGraph::NodeId from, PatchGraph::NodeId to, float gain = 1.0f) { graph.connect ({ from, 0 }, { to, 0 }, gain); };
        wire (PatchGraph::inputNodeId, nodeOf[1]);
        wire (nodeOf[1], nodeOf[2]);
        wire (nodeOf[1], nodeOf[5], 0.7f);
        wire (nodeOf[2], nodeOf[4]);
        wire (nodeOf[5], nodeOf[4]);
        wire (nodeOf[4], nodeOf[3]);
        wire (nodeOf[3], PatchGraph::outputNodeId);
        wire (nodeOf[3], nodeOf[1], 0.25f);
        processor.setFxPatchGraph (graph);
        if (auto* view = findChild<PatchCableView> (*editor))
            view->centreContent();
        shot ("loop");
        // Delay 2 selected: its row under the canvas.
        if (auto* view = findChild<PatchCableView> (*editor))
            for (const auto& node : view->getGraph().nodes())
                if (node.ref == 5 && node.id < (1u << 20)) // (not a voice box: PatchPage::voiceId)
                {
                    const auto centre = view->nodeBounds (node).getCentre() * view->getZoom();
                    view->mouseDown (PatchCableUi::mouse (*view, centre, true));
                    view->mouseUp (PatchCableUi::mouse (*view, centre, false));
                }
        shot ("selected");

        // Mini windows: Delay 2's and Reverb's, opened by double-clicks.
        if (auto* view = findChild<PatchCableView> (*editor))
            for (const auto ref : { 3, 5 })
            {
                for (const auto& node : view->getGraph().nodes())
                    if (node.ref == ref && node.id < (1u << 20))
                    {
                        const auto centre = view->nodeBounds (node).getCentre() * view->getZoom();
                        view->mouseDoubleClick (PatchCableUi::mouse (*view, centre, false));
                        break;
                    }
                // (One window docks at the right and the graph refits beside it.)
                if (ref == 3)
                    shot ("window-one");
            }
        shot ("windows");

        pages->showPage ("FX");
        shot ("rack-instances");

        // ilana's patch (2026-10-11): Reverb into Smear and EQ, Smear into
        // EQ, an EQ to Reverb loop (here at its 70 % top, so the guard holds
        // it), with the loop selected; then the RACK with a second Reverb.
        {
            for (int slot = 1; slot <= IlanaSynthAudioProcessor::numFxSlots; ++slot)
                setParam ("fx_slot" + juce::String (slot), 0.0f);
            settle (100);
            for (const auto type : { 13, 11, 29 })
                processor.addFxEffect (type);
            setParam ("fx_reverb_on", 1.0f);
            setParam ("fx_smear_on", 1.0f);
            processor.setFxPatcherOn (true);
            auto patch = processor.getFxPatchGraph();
            std::map<int, PatchGraph::NodeId> boxOf;
            for (const auto& node : patch.nodes())
                if (! node.isFixed())
                    boxOf[node.ref] = node.id;
            for (const auto& cable : std::vector<PatchGraph::Cable> (patch.cables()))
                patch.disconnect (cable.from, cable.to);
            patch.moveNode (PatchGraph::inputNodeId, 0.0f, 80.0f);
            patch.moveNode (boxOf[1], 140.0f, 140.0f);
            patch.moveNode (boxOf[2], 380.0f, 0.0f);
            patch.moveNode (boxOf[3], 450.0f, 80.0f);
            patch.moveNode (PatchGraph::outputNodeId, 650.0f, 80.0f);
            patch.connect ({ PatchGraph::inputNodeId, 0 }, { boxOf[1], 0 });
            patch.connect ({ boxOf[1], 0 }, { boxOf[2], 0 });
            patch.connect ({ boxOf[1], 0 }, { boxOf[3], 0 });
            patch.connect ({ boxOf[2], 0 }, { boxOf[3], 0 });
            patch.connect ({ boxOf[3], 0 }, { PatchGraph::outputNodeId, 0 });
            patch.connect ({ boxOf[3], 0 }, { boxOf[1], 0 }, 0.7f);
            processor.setFxPatchGraph (patch);
            auto hot = processor.getFxPatchGraph();
            hot.setCableGain (hot.findCable ({ boxOf[3], 0 }, { boxOf[1], 0 }), 0.7f);
            hot.setCableDelay (hot.findCable ({ boxOf[3], 0 }, { boxOf[1], 0 }), 120.0f);
            processor.setFxPatchGraph (hot);
            pages->showPage ("PATCH");
            settle (200);
            if (auto* view = findChild<PatchCableView> (*editor))
            {
                view->fitContent (1.4f);
                view->selectCable (processor.getFxPatchGraph().findCable ({ boxOf[3], 0 }, { boxOf[1], 0 }));
            }
            shot ("ilana-loop");

            processor.setFxPatcherOn (false);
            setParam ("fx_routing", 0.0f);
            processor.addFxEffect (13);
            setParam ("fx_reverb_size", 0.8f);
            pages->showPage ("FX");
            shot ("rack-reverb-instance");
        }

        if (auto* top = editor->getTopLevelComponent())
        {
            const auto before = top->getBounds();
            top->setSize (795, 540);
            pages->showPage ("PATCH");
            settle (200);
            if (auto* view = findChild<PatchCableView> (*editor))
                view->centreContent();
            shot ("loop-75");
            top->setBounds (before);
        }
        return 0;
    }

    // ILANA_SNAPSHOT_PATCHNAV: the PATCH tab without modes (2026-10-12):
    // fitted, zoomed in with the map, an effect dragged over a cable (lit
    // up) and dropped there; then stop. Best with Neuro Wobble.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_PATCHNAV", "").isNotEmpty())
    {
        const auto shot = [&] (const juce::String& name)
        {
            settle (400);
            save (*editor, outDir.getChildFile ("patchnav-" + name + ".png"));
        };
        pages->showPage ("PATCH");
        shot ("fit");
        auto* view = findChild<PatchCableView> (*editor);
        if (view == nullptr)
            return 1;
        const auto toScreen = [view] (juce::Point<float> p) { return p * view->getZoom(); };
        const auto& graph = view->getGraph();
        std::vector<const PatchGraph::Node*> effects;
        for (const auto& node : graph.nodes())
            if (! node.isFixed() && node.id < (1u << 20))
                effects.push_back (&node);
        std::sort (effects.begin(), effects.end(), [] (const auto* a, const auto* b) { return a->x < b->x; });

        // Zoomed in on the effects: the map in the corner.
        if (! effects.empty())
        {
            view->zoomAround (2.0f, toScreen (view->nodeBounds (*effects.front()).getCentre()));
            view->onNavigated();
            shot ("zoomed");
        }
        if (view->onFitRequested)
            view->onFitRequested();
        view->finishGlide();
        settle (200);

        // The last effect dragged onto the cable between the first two.
        if (effects.size() >= 3)
        {
            const auto moving = effects.back()->id, first = effects[0]->id, second = effects[1]->id;
            const auto target = (view->portPosition ({ first, 0 }, false) + view->portPosition ({ second, 0 }, true)) * 0.5f;
            const auto grab = view->nodeBounds (*graph.find (moving)).getCentre();
            view->mouseDown (PatchCableUi::mouse (*view, toScreen (grab), true));
            for (int step = 1; step <= 8; ++step)
                view->mouseDrag (PatchCableUi::mouse (*view, toScreen (grab + (target - grab) * ((float) step / 8.0f)), true));
            view->setMotion (false); // (the picture without the swing in flight)
            view->repaint();
            shot ("drop-hover");
            view->mouseUp (PatchCableUi::mouse (*view, toScreen (target), false));
            view->setMotion (true);
            shot ("dropped");
        }
        return 0;
    }

    // ILANA_SNAPSHOT_CAROUSEL: the PATCH tab's row of effects under the canvas
    // (2026-10-18): idle, scrolled, one tile dragged over a cable (lit up) and
    // dropped there; then stop. Best with Neuro Wobble.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_CAROUSEL", "").isNotEmpty())
    {
        const auto shot = [&] (const juce::String& name)
        {
            settle (400);
            save (*editor, outDir.getChildFile ("carousel-" + name + ".png"));
        };
        pages->showPage ("PATCH");
        shot ("idle");
        auto* view = findChild<PatchCableView> (*editor);
        auto* carousel = findChild<FxCarousel> (*editor);
        if (view == nullptr || carousel == nullptr)
        {
            std::printf ("carousel snapshot: view %p carousel %p\n", (void*) view, (void*) carousel);
            return 1;
        }
        std::printf ("carousel bounds %s showing %d tiles %d\n", carousel->getBounds().toString().toRawUTF8(), (int) carousel->isShowing(), carousel->numTiles());
        carousel->scrollTo (carousel->getScroll() + 400.0f);
        shot ("scrolled");
        carousel->mouseDown (PatchCableUi::mouse (*carousel, carousel->chipBounds (2).toFloat().getCentre(), true));
        carousel->mouseUp (PatchCableUi::mouse (*carousel, carousel->chipBounds (2).toFloat().getCentre(), false));
        shot ("category");
        carousel->showGroup (-1);
        carousel->scrollTo (0.0f);
        const auto& graph = view->getGraph();
        std::vector<const PatchGraph::Node*> effects;
        for (const auto& node : graph.nodes())
            if (! node.isFixed() && node.id < (1u << 20))
                effects.push_back (&node);
        std::sort (effects.begin(), effects.end(), [] (const auto* a, const auto* b) { return a->x < b->x; });
        auto tile = 0;
        for (int i = 0; i < carousel->numTiles(); ++i)
            if (carousel->tileAt (i).name == "Dimension")
                tile = i;
        const auto from = carousel->tileBounds (tile).toFloat().getCentre();
        if (effects.size() >= 2)
        {
            const auto target = (view->portPosition ({ effects[0]->id, 0 }, false) + view->portPosition ({ effects[1]->id, 0 }, true)) * 0.5f;
            const auto to = carousel->getLocalPoint (view, target * view->getZoom());
            carousel->mouseDown (PatchCableUi::mouse (*carousel, from, true));
            for (int step = 1; step <= 10; ++step)
                carousel->mouseDrag (PatchCableUi::mouse (*carousel, from + (to - from) * ((float) step / 10.0f), true, false, from));
            view->setMotion (false);
            view->repaint();
            shot ("drag-over-cable");
            carousel->mouseUp (PatchCableUi::mouse (*carousel, to, false, false, from));
            view->setMotion (true);
            shot ("dropped");
        }
        return 0;
    }

    // ILANA_SNAPSHOT_ROUTING: the FILTER tab's ROUTING page (roadmap step
    // 16): a preset's chain as cables, a cabled patch (WEST before the
    // filters, an oscillator split into Filter 1 and OUT, the sub straight
    // out), one box selected, the FILTER page's strip while cables rule, and
    // the page at 75 %; then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_ROUTING", "").isNotEmpty())
    {
        const auto shot = [&] (const juce::String& name)
        {
            settle (500);
            save (*editor, outDir.getChildFile ("routing-" + name + ".png"));
        };
        const auto preset = juce::SystemStats::getEnvironmentVariable ("ILANA_ROUTING_PRESET", "Supersaw Anthem");
        processor.loadFactoryPreset (juce::jmax (0, processor.getFactoryPresetNames().indexOf (preset)));
        settle (300);
        pages->showPage ("PATCH");
        shot ("chain");
        // One effect's window: it docks at the right and the graph refits beside it.
        if (auto* view = findChild<PatchCableView> (*editor))
            for (const auto& node : view->getGraph().nodes())
                if (juce::String (node.label) == "REVERB")
                {
                    view->mouseDoubleClick (PatchCableUi::mouse (*view, view->nodeBounds (node).getCentre() * view->getZoom(), false));
                    break;
                }
        shot ("chain-window");

        using namespace VoiceRouting;
        auto patch = processor.getChainAsCables();
        patch.source[0] = toBody | toOut;
        patch.source[1] = toWest;
        patch.source[2] = toF2;
        patch.source[subNoiseSource] = toOut;
        patch.stage[body] = toF1;
        patch.stage[west] = toF1;
        patch.stage[f1] = toF2;
        patch.stage[f2] = toOut;
        processor.setRouteCables (patch, true);
        shot ("cables");

        if (auto* view = OscRoutingUi::findRoutingView (*editor))
            if (const auto* node = OscRoutingUi::nodeNamed (view->getGraph(), "OSC 1"))
            {
                const auto centre = view->nodeBounds (*node).getCentre() * view->getZoom();
                view->mouseDown (PatchCableUi::mouse (*view, centre, true));
                view->mouseUp (PatchCableUi::mouse (*view, centre, false));
            }
        shot ("selected");

        pages->showPage ("FILTER");
        shot ("filter-page");

        if (auto* top = editor->getTopLevelComponent())
        {
            const auto before = top->getBounds();
            top->setSize (795, 540);
            pages->showPage ("PATCH");
            shot ("75");
            top->setBounds (before);
        }
        return 0;
    }

    // ILANA_SNAPSHOT_FM: just the FM page (UI review 6's DX7 pass): the
    // first operator, its KEYS & VELOCITY tab, PITCH & LFO, each other
    // operator, then the patch on DX7 algorithm 1 (the tallest stack); then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_FM", "").isNotEmpty())
    {
        pages->showPage ("FM");
        settle (400);
        save (*editor, outDir.getChildFile ("fm-1.png"));
        auto* page = pages->getCurrentPage();
        std::vector<CardTabs*> tabs;
        findAll<CardTabs> (*page, tabs);
        for (auto* tab : tabs)
            if (tab->isVisible() && tab->getNames().contains ("KEYS & VELOCITY"))
            {
                tab->setSelected (1, true);
                settle (300);
                save (*editor, outDir.getChildFile ("fm-1-keys.png"));
                tab->setSelected (0, true);
            }
        std::vector<juce::TextButton*> buttons;
        findAll<juce::TextButton> (*page, buttons);
        for (auto* button : buttons)
            if (button->getButtonText().contains ("OP PITCH") && button->onClick != nullptr)
            {
                // The link opens OP PITCH on MOD.
                button->onClick();
                settle (300);
                save (*editor, outDir.getChildFile ("fm-pitch-lfo.png"));
                pages->showPage ("FM");
                settle (300);
            }
        if (auto* picker = findChild<OscPicker> (*page))
            for (const auto osc : std::vector<int> (picker->getOscillators()))
                if (osc != 0)
                {
                    picker->pick (osc);
                    settle (200);
                    save (*editor, outDir.getChildFile ("fm-" + juce::String (osc + 1) + ".png"));
                }
        processor.applyDx7Algorithm (1);
        settle (400);
        save (*editor, outDir.getChildFile ("fm-dx7-algorithm-1.png"));
        // ILANA_SNAPSHOT_FM_ALGORITHMS="5,18,32": more DX7 algorithms.
        for (const auto& number : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_FM_ALGORITHMS", ""), ",", ""))
        {
            processor.applyDx7Algorithm (number.getIntValue());
            settle (300);
            save (*editor, outDir.getChildFile ("fm-dx7-algorithm-" + number + ".png"));
        }
        // Operator 1 on the Operator Env: the card as the DX7 voices have it.
        if (auto* picker = findChild<OscPicker> (*page))
            picker->pick (0);
        if (auto* parameter = processor.apvts.getParameter ("osc1_amp_env"))
        {
            parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) OperatorEg::envelopeChoice));
            settle (400);
            save (*editor, outDir.getChildFile ("fm-opeg.png"));
        }
        // OSC 2's WARP FM from OSC 1, drawn dashed beside the routes.
        for (const auto& [id, value] : { std::pair<const char*, float> { "osc2_warp", (float) Warp::Fm }, { "osc2_warp_amt", 0.5f } })
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        settle (400);
        save (*editor, outDir.getChildFile ("fm-warp.png"));
        return 0;
    }

    // ILANA_SNAPSHOT_T2: review 9's package T2 views (the VOICE tab, PROB SEQ
    // and CLIP with notes, the tour with WHAT'S NEW open, the remap dock, the
    // table browser with a favourite), then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_T2", "").isNotEmpty())
    {
        pages->showPage ("OSC");
        settle (300);
        std::vector<StateTabs*> tabRows;
        findAll<StateTabs> (*editor, tabRows);
        for (auto* tabs : tabRows)
            for (int i = 0; i < tabs->getNumItems(); ++i)
                if (tabs->getItem (i).name == "VOICE" && tabs->onSelect != nullptr)
                {
                    tabs->setSelected (i);
                    tabs->onSelect (i);
                }
        settle (300);
        save (*editor, outDir.getChildFile ("t2-osc-voice.png"));

        pages->showPage ("ARP/SEQ");
        settle (300);
        std::vector<CardTabs*> engineTabs;
        findAll<CardTabs> (*editor, engineTabs);
        for (auto* tabs : engineTabs)
            if (tabs->getNames().contains ("PROB SEQ") && visibleInTree (tabs))
            {
                tabs->setSelected (2, true);
                settle (300);
                save (*editor, outDir.getChildFile ("t2-seq-probseq.png"));
                tabs->setSelected (1, true);
                settle (300);
                save (*editor, outDir.getChildFile ("t2-seq-euclid.png"));
                Clip clip;
                clip.bars = 2;
                clip.notes = { { 0.0f, 1.0f, 60, 100 }, { 1.0f, 0.5f, 64, 70 }, { 2.0f, 1.0f, 67, 127 }, { 4.0f, 2.0f, 62, 40 }, { 6.0f, 1.0f, 65, 90 } };
                processor.getClipState().setClip (0, clip);
                tabs->setSelected (3, true);
                settle (500);
                save (*editor, outDir.getChildFile ("t2-seq-clip.png"));
                tabs->setSelected (0, true);
            }

        pages->showPage ("MATRIX");
        settle (300);
        std::vector<CurveControl*> curves;
        findAll<CurveControl> (*editor, curves);
        for (auto* curve : curves)
            if (visibleInTree (curve))
            {
                curve->openRemapEditor();
                break;
            }
        settle (400);
        save (*editor, outDir.getChildFile ("t2-matrix-remap.png"));

        pages->showPage ("MAIN");
        if (auto* tutorial = findChild<TutorialOverlay> (*editor))
        {
            tutorial->setVisible (true);
            tutorial->toFront (false);
            settle (500);
            save (*editor, outDir.getChildFile ("t2-tour-closed.png"));
            tutorial->setWhatsNewOpen (true);
            settle (200);
            save (*editor, outDir.getChildFile ("t2-tour-open.png"));
            tutorial->setMovesOpen (true);
            settle (200);
            save (*editor, outDir.getChildFile ("t2-tour-moves.png"));
            tutorial->setMovesOpen (false);
            tutorial->setVisible (false);
        }

        // The undo note after a Remove (then the step undone again).
        if (auto* cutoff = processor.apvts.getParameter ("f1_cutoff"))
        {
            const auto before = cutoff->getValue();
            processor.performEdit ("Remove REVERB", [cutoff] { cutoff->setValueNotifyingHost (0.3f); });
            settle (300);
            save (*editor, outDir.getChildFile ("t2-undo-note.png"));
            processor.getUndoManager().undo();
            cutoff->setValueNotifyingHost (before);
            settle (200);
        }

        if (auto* display = findChild<PresetDisplay> (*editor); display != nullptr && display->onClick != nullptr)
        {
            display->onClick();
            settle (500);
            if (auto* panel = findChild<PresetPanel> (*editor); panel != nullptr && panel->onDockRequest != nullptr)
            {
                panel->selectFilter ("");
                panel->clickChip ("pack:dx7");
                panel->clickChip ("bank:ROM1A");
                settle (200);
                save (*editor, outDir.getChildFile ("t2-browser-dx7.png"));
                panel->onDockRequest (true);
                settle (500);
                save (*editor, outDir.getChildFile ("t2-browser-docked-dx7.png"));
                panel->clickChip ("bank:ROM1A");
                panel->clickChip ("pack:dx7");
                panel->selectFilter ("");
                panel->onDockRequest (false);
                settle (300);
                panel->close();
                settle (300);
            }
        }

        pages->savePresetAs();
        settle (300);
        save (*editor, outDir.getChildFile ("t2-save-as.png"));
        pages->getSaveOverlay().setTagsFieldOpen (true);
        settle (200);
        save (*editor, outDir.getChildFile ("t2-save-as-field.png"));
        pages->getSaveOverlay().cancel();

        if (auto* settingsFile = tableBrowserSettings())
            settingsFile->setValue ("tablefav_Basic", "1");
        {
            TableBrowser browser (processor, "osc1_table", IlanaTheme::accent());
            browser.setLookAndFeel (&pages->getLookAndFeel());
            browser.setSize (912, 576);
            settle (200);
            save (browser, outDir.getChildFile ("t2-table-browser.png"));
        }
        if (auto* settingsFile = tableBrowserSettings())
            settingsFile->removeValue ("tablefav_Basic");
        return 0;
    }

    // ILANA_SNAPSHOT_EXTRAS: the UI review 2 additions (the SPEC view, a
    // knob's source card, the docked browser), then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_EXTRAS", "").isNotEmpty())
    {
        pages->showPage ("MAIN");
        settle (400);
        std::vector<WaveDisplay*> waves;
        findAll<WaveDisplay> (*editor, waves);
        for (auto* wave : waves)
            if (visibleInTree (wave) && wave->getOscIndex() == 0)
            {
                wave->setViewMode (2);
                settle (300);
            }
        std::vector<KnobControl*> knobs;
        findAll<KnobControl> (*editor, knobs);
        KnobControl* busiest = nullptr;
        for (auto* knob : knobs)
            if (visibleInTree (knob) && knob->getNumRoutings() > (busiest != nullptr ? busiest->getNumRoutings() : 0))
                busiest = knob;
        if (busiest != nullptr)
            busiest->openModCard (true);
        save (*editor, outDir.getChildFile ("extra-spec-and-card.png"));
        for (auto* knob : knobs)
            knob->closeModCard();

        if (auto* display = findChild<PresetDisplay> (*editor); display != nullptr && display->onClick != nullptr)
        {
            display->onClick();
            settle (500);
            save (*editor, outDir.getChildFile ("extra-browser-dropdown.png"));
            if (auto* panel = findChild<PresetPanel> (*editor); panel != nullptr && panel->onDockRequest != nullptr)
            {
                // DX7 with its bank chips (the settings are left as they were).
                panel->selectFilter ("");
                panel->clickChip ("pack:dx7");
                panel->clickChip ("bank:ROM1A");
                settle (200);
                save (*editor, outDir.getChildFile ("extra-browser-dx7.png"));
                panel->onDockRequest (true);
                settle (500);
                save (*editor, outDir.getChildFile ("extra-browser-docked-dx7.png"));
                panel->clickChip ("bank:ROM1A");
                panel->clickChip ("pack:dx7");
                panel->selectFilter ("");
                settle (200);
                save (*editor, outDir.getChildFile ("extra-browser-docked.png"));
                panel->onDockRequest (false);
                settle (300);
                panel->close();
                settle (300);
            }
        }

        // SAVE AS, with a name that loses characters (cancelled, so nothing is written).
        pages->savePresetAs();
        pages->getSaveOverlay().getNameField().setText ("Acid: Bass / Mk 2", true);
        settle (300);
        save (*editor, outDir.getChildFile ("extra-save-as.png"));
        pages->getSaveOverlay().cancel();

        {
            TableBrowser browser (processor, "osc1_table", IlanaTheme::accent());
            browser.setLookAndFeel (&pages->getLookAndFeel());
            browser.setSize (912, 576);
            browser.setSearchText ("saw");
            settle (200);
            save (browser, outDir.getChildFile ("extra-table-search.png"));
            browser.setSearchText ("emu vsco");
            settle (400);
            save (browser, outDir.getChildFile ("extra-table-builtin.png"));
        }
        return 0;
    }

    // ILANA_SNAPSHOT_LFOSH: LFO 1 on Sample & Hold with the dot in the second half of the cycle
    // (the dot used to count sixteen steps against the eight the trace draws), then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_LFOSH", "").isNotEmpty())
    {
        const auto setParam = [&] (const char* id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };
        setParam ("lfo1_shape", (float) LfoShapes::SampleHold);
        setParam ("lfo1_sync", 0.0f);
        setParam ("lfo1_rate", 1.0f);
        pages->showPage ("ENV/LFO");
        settle (400);
        if (auto* page = pages->getCurrentPage())
            if (auto* thumbs = findChild<LfoThumbBar> (*page); thumbs != nullptr && thumbs->onSelect != nullptr)
                thumbs->onSelect (0);
        juce::AudioBuffer<float> audio (2, 512);
        const auto sampleRate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 44100.0;
        const auto blocksPerCycle = sampleRate / 512.0; // 1 Hz
        int block = 0;
        for (const auto [fraction, name] : std::initializer_list<std::pair<double, const char*>> {
                 { 0.30, "lfosh-a.png" }, { 0.62, "lfosh-b.png" }, { 0.88, "lfosh-c.png" } })
        {
            const auto target = (int) (fraction * blocksPerCycle) + (int) blocksPerCycle * 2;
            for (; block < target; ++block)
            {
                juce::MidiBuffer midi;
                if (block == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
                audio.clear();
                processor.processBlock (audio, midi);
            }
            settle (150);
            save (*editor, outDir.getChildFile (name));
        }
        return 0;
    }

    // ILANA_SNAPSHOT_R16P4: review 16's FILTER / MOD / MATRIX states, then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_R16P4", "").isNotEmpty())
    {
        const auto setParam = [&] (const char* id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };
        pages->showPage ("FILTER");
        settle (500);
        save (*editor, outDir.getChildFile ("r-filter.png"));
        pages->showPage ("ENV/LFO");
        settle (400);
        if (auto* page = pages->getCurrentPage())
            if (auto* thumbs = findChild<LfoThumbBar> (*page); thumbs != nullptr && thumbs->onSelect != nullptr)
            {
                thumbs->onSelect (0);
                settle (300);
                save (*editor, outDir.getChildFile ("r-lfo1.png"));
                setParam ("lfo1_shape", (float) LfoShapes::Pendulum);
                settle (400);
                save (*editor, outDir.getChildFile ("r-lfo-physics.png"));
                for (const auto& [shape, name] : std::initializer_list<std::pair<int, const char*>> {
                         { LfoSimShapes::Lorenz, "lorenz" }, { LfoSimShapes::Pendulum, "pendulum" }, { LfoSimShapes::Spring, "spring" },
                         { LfoSimShapes::Henon, "henon" } })
                {
                    setParam ("lfo1_shape", (float) shape);
                    const auto& info = LfoSimInfo::get (shape);
                    for (int param = 0; param < LfoSimInfo::numParams; ++param)
                        if (auto* parameter = processor.apvts.getParameter ("lfo1_p" + juce::String (param + 1)))
                            parameter->setValueNotifyingHost (info.params[(size_t) param].defaultValue);
                    setParam ("lfo1_rate", 1.0f);
                    settle (1500);
                    save (*editor, outDir.getChildFile ("r-lfo-" + juce::String (name) + ".png"));
                }
                setParam ("lfo1_shape", 0.0f);
                thumbs->onSelect (IlanaSynthAudioProcessor::numLfos + 2);
                settle (400);
                save (*editor, outDir.getChildFile ("r-oplfo.png"));
            }
        pages->showPage ("MATRIX");
        settle (500);
        save (*editor, outDir.getChildFile ("r-matrix.png"));
        std::vector<CurveControl*> curves;
        findAll<CurveControl> (*editor, curves);
        for (auto* curve : curves)
            if (visibleInTree (curve))
            {
                curve->openRemapEditor();
                settle (500);
                save (*editor, outDir.getChildFile ("r-matrix-remap.png"));
                curve->openRemapEditor();
                break;
            }
        return 0;
    }

    // ILANA_SNAPSHOT_REMAP: the matrix with a drawn remap on the first row,
    // its editor open, then stop.
    // ILANA_SNAPSHOT_GRAPH: the cable view of roadmap step A, then stop.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_GRAPH", "").isNotEmpty())
    {
        snapshotPatchCableView (processor, outDir);
        return 0;
    }

    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_REMAP", "").isNotEmpty())
    {
        pages->showPage ("MATRIX");
        settle (400);
        std::vector<CurveControl*> curves;
        findAll<CurveControl> (*editor, curves);
        for (auto* curve : curves)
            if (visibleInTree (curve))
            {
                const auto slot = curve->getSlotIndex();
                processor.setModRemap (slot, RemapEditor::shape (4));
                settle (300);
                save (*editor, outDir.getChildFile ("remap-matrix.png"));
                // The editor, docked under its row.
                curve->openRemapEditor();
                settle (500);
                save (*editor, outDir.getChildFile ("remap-editor.png"));
                curve->openRemapEditor(); // closes it again
                break;
            }
        return 0;
    }

    // ILANA_SNAPSHOT_P1: the modulation views of UI review 6 (P1), then
    // stop: knob rings and badges, a knob's and a macro's cards (with a
    // target whose module is off), a group chip's tray, and the matrix with
    // an idle row, a repeated row and a docked remap editor.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_P1", "").isNotEmpty())
    {
        const auto route = [&processor] (int slot, Mod::Source source, Mod::Destination destination, float depth)
        {
            const auto prefix = "mod" + juce::String (slot + 1);
            for (const auto& [field, value] : { std::pair<const char*, float> { "_src", (float) source },
                                                { "_dst", (float) destination }, { "_amt", depth } })
                if (auto* parameter = processor.apvts.getParameter (prefix + field))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };
        auto freeSlot = 0;
        while (freeSlot < Mod::maxSlots && processor.readModSlot (freeSlot).source != Mod::Source::None)
            ++freeSlot;

        // Macro 1 into OSC 3 (off in most patches) and a repeat of slot 1.
        route (freeSlot, Mod::macroSourceFor (0), Mod::Destination::SubLevel, 0.5f);
        if (const auto first = processor.readModSlot (0); first.source != Mod::Source::None)
            route (freeSlot + 1, first.source, (Mod::Destination) first.destination, 0.25f);
        settle (300);

        for (const auto* page : { "MAIN", "FILTER" })
        {
            pages->showPage (page);
            settle (500);
            save (*editor, outDir.getChildFile ("p1-" + juce::String (page).toLowerCase() + ".png"));
        }

        pages->showPage ("MAIN");
        settle (400);
        std::vector<KnobControl*> knobs;
        findAll<KnobControl> (*editor, knobs);
        KnobControl* busiest = nullptr;
        for (auto* knob : knobs)
            if (visibleInTree (knob) && ! knob->isCompact() && knob->getNumRoutings() > (busiest != nullptr ? busiest->getNumRoutings() : 0))
                busiest = knob;
        if (busiest != nullptr)
        {
            busiest->openModCard (true);
            settle (300);
            save (*editor, outDir.getChildFile ("p1-knob-card.png"));
            busiest->closeModCard();
            settle (200);
        }

        std::vector<StripKnob*> macros;
        findAll<StripKnob> (*editor, macros);
        for (auto* macro : macros)
            if (auto* card = ModHoverPopup::instance(); card != nullptr && macro->getMacroIndex() == 0)
            {
                card->holdOpen (true);
                macro->openCard();
                settle (400);
                save (*editor, outDir.getChildFile ("p1-macro-card.png"));
                card->holdOpen (false);
                card->close();
            }

        // Every LFO and envelope in the pool: the bar folds them into group
        // chips; open the envelopes' tray.
        for (int i = 0; i < IlanaSynthAudioProcessor::numLfos; ++i)
            processor.setRevealed (IlanaSynthAudioProcessor::Module::Lfo, i, true);
        for (int i = 0; i < 16; ++i)
            processor.setRevealed (IlanaSynthAudioProcessor::Module::Envelope, i, true);
        settle (400);
        std::vector<ModSourceGroupChip*> groups;
        findAll<ModSourceGroupChip> (*editor, groups);
        for (auto* group : groups)
            if (visibleInTree (group) && group->onOpen != nullptr)
            {
                group->onOpen (*group);
                settle (300);
                save (*editor, outDir.getChildFile ("p1-chip-tray.png"));
                group->onOpen (*group);
                break;
            }

        pages->showPage ("MATRIX");
        settle (500);
        save (*editor, outDir.getChildFile ("p1-matrix.png"));
        std::vector<CurveControl*> curves;
        findAll<CurveControl> (*editor, curves);
        for (auto* curve : curves)
            if (visibleInTree (curve) && curve->getSlotIndex() == 1)
            {
                curve->openRemapEditor();
                settle (500);
                save (*editor, outDir.getChildFile ("p1-remap.png"));
                curve->openRemapEditor();
                break;
            }
        return 0;
    }

    // ILANA_SNAPSHOT_ADDOSC="3,4,5": those oscillator slots (0-based) added first, to see PLAY at five or six strips.
    for (const auto& slot : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_ADDOSC", ""), ",", ""))
        processor.addOscillator (slot.getIntValue());

    // ILANA_SNAPSHOT_SET="osc2_mode=1,osc3_mode=2": parameters set to plain values first.
    for (const auto& pair : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_SET", ""), ",", ""))
        if (auto* parameter = processor.apvts.getParameter (pair.upToFirstOccurrenceOf ("=", false, false)))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (pair.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));

    // ILANA_SNAPSHOT_SAMPLE="1": oscillator 1 holds a decaying electric-piano-like sample (as the mockups do), so a
    // Sample or Granular card shows its waveform instead of the drop zone.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_SAMPLE", "").isNotEmpty())
    {
        auto piano = std::make_shared<SampleData>();
        piano->sampleRate = 48000.0;
        piano->name = "Rhodes C3.wav";
        piano->buffer.setSize (2, 144000);
        for (int i = 0; i < 144000; ++i)
        {
            const auto t = i / 48000.0;
            const auto tone = std::sin (juce::MathConstants<double>::twoPi * 131.0 * t) + 0.5 * std::sin (juce::MathConstants<double>::twoPi * 262.0 * t + 0.4)
                              + 0.25 * std::sin (juce::MathConstants<double>::twoPi * 393.0 * t);
            const auto value = (float) (0.5 * std::exp (-1.1 * t) * (1.0 - std::exp (-t * 400.0)) * tone);
            piano->buffer.setSample (0, i, value);
            piano->buffer.setSample (1, i, value);
        }
        processor.setUserSample (0, piano, {});
    }

    const auto pageIds = pages->getPageIds();
    // ILANA_SNAPSHOT_PAGES="MAIN,OSC": only those pages (no extras), then stop.
    const auto onlyPages = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_PAGES", ""), ",", "");
    // ILANA_SNAPSHOT_BROWSER: the wavetable browser, sized as it opens over the editor.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_BROWSER", "").isNotEmpty())
    {
        TableBrowser browser (processor, "osc1_table", IlanaTheme::accent());
        browser.setLookAndFeel (&editor->getLookAndFeel());
        browser.setSize (juce::jmax (740, juce::roundToInt ((float) editor->getWidth() * 0.86f)), juce::jmax (520, juce::roundToInt ((float) editor->getHeight() * 0.8f)));
        settle (200);
        save (browser, outDir.getChildFile ("table-browser.png"));
    }

    // ILANA_SNAPSHOT_FLOWOPEN: the FILTER page's SIGNAL FLOW enlarged (it opens on hover or click).
    filterFlowForcedOpen() = juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_FLOWOPEN", "").isNotEmpty();

    // ILANA_SNAPSHOT_POPOVER="LFO 1:FILTER": a dock chip's pop-out editor open over a page.
    if (const auto popover = juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_POPOVER", ""); popover.isNotEmpty())
    {
        std::vector<ModSourceChip*> found;
        findAll<ModSourceChip> (*editor, found);
        for (auto* chip : found)
            if (chip->getSourceName() == popover.upToFirstOccurrenceOf (":", false, false) && chip->isVisible())
            {
                pages->showPage (popover.fromFirstOccurrenceOf (":", false, false));
                settle (300);
                pages->toggleSourcePopover (chip->getSourceIndex(), *chip);
                settle (400);
                save (*editor, outDir.getChildFile ("popover.png"));
                pages->toggleSourcePopover (chip->getSourceIndex(), *chip);
                break;
            }
    }

    for (int i = 0; i < pageIds.size(); ++i)
    {
        if (! onlyPages.isEmpty() && ! onlyPages.contains (pageIds[i]))
            continue;

        pages->showPage (pageIds[i]);
        settle (450);
        // ILANA_SNAPSHOT_3D: the wavetable pictures flipped to their 3D stack.
        if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_3D", "").isNotEmpty())
        {
            std::vector<WaveDisplay*> pictures;
            findAll<WaveDisplay> (*editor, pictures);
            for (auto* picture : pictures)
                picture->setStack3d (true);
            settle (300);
        }
        const auto stem = juce::String (i + 1).paddedLeft ('0', 2) + "-" + pageIds[i].replaceCharacter ('/', '-');
        save (*editor, outDir.getChildFile (stem + ".png"));

        // ILANA_SNAPSHOT_KNOBS: every shown knob's id, cell and drawn dial (the dial-size guard's numbers).
        if (juce::SystemStats::getEnvironmentVariable ("ILANA_SNAPSHOT_KNOBS", "").isNotEmpty())
        {
            std::vector<KnobControl*> knobs;
            findAll<KnobControl> (*editor, knobs);
            for (auto* knob : knobs)
                if (visibleInTree (knob) && knob->getWidth() > 0)
                    std::fprintf (stderr, "KNOB %s %s cell %dx%d maxDial %d dial %d radius %.1f\n", pageIds[i].toRawUTF8(), knob->getParameterId().toRawUTF8(),
                                  knob->getWidth(), knob->getHeight(), knob->getMaxDial(), knob->getDialSize(), knob->getDialRadius());
        }

        if (pageIds[i] == "OSC")
        {
            if (auto* mode = processor.apvts.getParameter ("osc1_mode"))
            {
                // (Put back as it was, not to Wavetable: a DX7 patch's OSC 1 is an operator, N16-3.)
                const auto modeBefore = mode->getValue();
                mode->setValueNotifyingHost (mode->convertTo0to1 (1.0f));
                settle (300);
                save (*editor, outDir.getChildFile ("osc-physical.png"));
                if (auto* excite = processor.apvts.getParameter ("osc1_excite"))
                    excite->setValueNotifyingHost (excite->convertTo0to1 (4.0f));
                if (auto* buzz = processor.apvts.getParameter ("osc1_bridge_buzz"))
                    buzz->setValueNotifyingHost (buzz->convertTo0to1 (0.6f));
                if (auto* rattle = processor.apvts.getParameter ("osc1_fret_rattle"))
                    rattle->setValueNotifyingHost (rattle->convertTo0to1 (0.4f));
                settle (300);
                save (*editor, outDir.getChildFile ("osc-bow-buzz.png"));
                if (auto* viewport = dynamic_cast<juce::Viewport*> (pages->getCurrentPage()))
                {
                    if (auto* on = processor.apvts.getParameter ("sym_on"))
                        on->setValueNotifyingHost (on->convertTo0to1 (1.0f));
                    if (auto* amount = processor.apvts.getParameter ("sym_amount"))
                        amount->setValueNotifyingHost (amount->convertTo0to1 (0.5f));
                    // Let the page grow for the opened card, then scroll to it.
                    settle (250);
                    viewport->setViewPosition (0, 10000);
                    settle (100);
                    save (*editor, outDir.getChildFile ("osc-sympathetic.png"));
                    if (auto* manual = processor.apvts.getParameter ("sym_manual"))
                        manual->setValueNotifyingHost (manual->convertTo0to1 (1.0f));
                    settle (250);
                    viewport->setViewPosition (0, 10000);
                    settle (100);
                    save (*editor, outDir.getChildFile ("osc-sympathetic-manual.png"));
                    viewport->setViewPosition (0, 0);
                }
                mode->setValueNotifyingHost (modeBefore);
            }
        }

        // Every sub-tab past the first on this page.
        std::vector<SubTabBar*> bars;

        if (auto* page = pages->getCurrentPage())
            findAll<SubTabBar> (*page, bars);

        for (size_t b = 0; b < bars.size(); ++b)
        {
            for (int item = 1; item < bars[b]->getNumRevealed(); ++item)
            {
                bars[b]->onSelect (item);
                settle (300);
                save (*editor, outDir.getChildFile (stem + "-bar" + juce::String ((int) b + 1) + "-" + juce::String (item + 1) + ".png"));
            }

            if (bars[b]->getNumRevealed() > 1)
                bars[b]->onSelect (0);
        }

        // The FM page's PITCH & LFO (the Operator Env's pitch envelope and
        // LFO, which a DX7 preset's operators follow).
        if (auto* page = pages->getCurrentPage())
        {
            std::vector<juce::TextButton*> buttons;
            findAll<juce::TextButton> (*page, buttons);
            for (auto* button : buttons)
                if (button->getButtonText().contains ("OP PITCH") && button->isVisible() && button->onClick != nullptr)
                {
                    button->onClick();
                    settle (300);
                    save (*editor, outDir.getChildFile (stem + "-pitch-lfo.png"));
                    // (The link jumps to MOD's pools: back to this page, so the shots after it are this page's, N16-10.)
                    pages->showPage (pageIds[i]);
                    settle (300);
                    break;
                }
        }

        // Every envelope card on the ENV/LFO page.
        if (auto* page = pages->getCurrentPage())
        {
            if (auto* envCards = findChild<EnvThumbBar> (*page); envCards != nullptr && envCards->onSelect != nullptr)
            {
                for (int env = 1; env < 5; ++env)
                {
                    envCards->onSelect (env);
                    settle (300);
                    save (*editor, outDir.getChildFile (stem + "-env" + juce::String (env + 1) + ".png"));
                }

                processor.setRevealed (IlanaSynthAudioProcessor::Module::Envelope, 5, true);
                envCards->onSelect (5);
                settle (300);
                save (*editor, outDir.getChildFile ("env-pool-revealed.png"));
                for (int env = 0; env < 16; ++env)
                    processor.setRevealed (IlanaSynthAudioProcessor::Module::Envelope, env, true);
                envCards->onSelect (15);
                settle (300);
                save (*editor, outDir.getChildFile ("env-pool-full.png"));

                envCards->onSelect (0);
            }
        }

        // Each FX module's editor, placed in slot 1.
        if (pageIds[i] == "FX")
        {
            for (int type = 1; type <= IlanaSynthAudioProcessor::numFxTypes; ++type)
            {
                processor.assignFxSlot (1, type);
                settle (900);
                save (*editor, outDir.getChildFile ("fx-" + juce::String (type).paddedLeft ('0', 2) + ".png"));
            }

            // UI review 6: a split group (drive on the lows, reverb on the
            // highs) and a duplicate card, then the rack as it was.
            std::array<float, 3 * IlanaSynthAudioProcessor::numFxSlots> kept {};
            for (int slot = 0; slot < IlanaSynthAudioProcessor::numFxSlots; ++slot)
                for (int k = 0; k < 3; ++k)
                    kept[(size_t) (slot * 3 + k)] = processor.apvts.getParameter ("fx_slot" + juce::String (slot + 1) + (k == 0 ? "" : (k == 1 ? "_band" : "_bypass")))->getValue();
            const auto setBand = [&] (int slot, int band)
            {
                auto* parameter = processor.apvts.getParameter ("fx_slot" + juce::String (slot) + "_band");
                parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) band));
            };
            const int splitRack[] { 7, 2, 13, 20, 0, 0, 0, 0, 0, 0 };
            for (int slot = 0; slot < IlanaSynthAudioProcessor::numFxSlots; ++slot)
            {
                processor.assignFxSlot (slot + 1, splitRack[slot]);
                setBand (slot + 1, 0);
            }
            setBand (2, 1);
            setBand (3, 3);
            settle (600);
            save (*editor, outDir.getChildFile ("fx-split.png"));
            processor.assignFxSlot (5, 20); // a second OTT
            settle (600);
            if (auto* viewport = findChild<juce::Viewport> (*pages->getCurrentPage()))
                viewport->setViewPosition (0, 10000);
            settle (100);
            save (*editor, outDir.getChildFile ("fx-duplicate.png"));
            // A mid / side group, then the empty rack with its library.
            const int midSideRack[] { 2, 7, 13, 0, 0, 0, 0, 0, 0, 0 };
            for (int slot = 0; slot < IlanaSynthAudioProcessor::numFxSlots; ++slot)
            {
                processor.assignFxSlot (slot + 1, midSideRack[slot]);
                setBand (slot + 1, 0);
            }
            setBand (2, 5);
            setBand (3, 4);
            settle (600);
            save (*editor, outDir.getChildFile ("fx-midside.png"));
            for (int slot = 0; slot < IlanaSynthAudioProcessor::numFxSlots; ++slot)
            {
                processor.assignFxSlot (slot + 1, 0);
                setBand (slot + 1, 0);
            }
            settle (600);
            save (*editor, outDir.getChildFile ("fx-empty.png"));
            for (int slot = 0; slot < IlanaSynthAudioProcessor::numFxSlots; ++slot)
                for (int k = 0; k < 3; ++k)
                    processor.apvts.getParameter ("fx_slot" + juce::String (slot + 1) + (k == 0 ? "" : (k == 1 ? "_band" : "_bypass")))
                        ->setValueNotifyingHost (kept[(size_t) (slot * 3 + k)]);
            settle (300);
        }
    }

    // The scope floats over a page; the hover line over the chip row
    // (ILANA_SNAPSHOT_PAGES=SCOPE shows only these).
    if (onlyPages.isEmpty() || onlyPages.contains ("SCOPE"))
    {
        pages->showPage ("MAIN");
        pages->setScopeOpen (true);
        settle (500);
        save (*editor, outDir.getChildFile ("scope-panel.png"));
        pages->setScopeOpen (false);
        settle (100);

        std::vector<KnobControl*> knobs;
        findAll<KnobControl> (*editor, knobs);
        for (auto* knob : knobs)
            if (visibleInTree (knob))
            {
                pages->getHoverLine().restOn (knob);
                save (*editor, outDir.getChildFile ("hover-line.png"));
                pages->getHoverLine().restOn (nullptr);
                break;
            }
    }

    if (! onlyPages.isEmpty())
        return 0;

    // The drawable Curve LFO editor.
    if (auto* shape = processor.apvts.getParameter ("lfo1_shape"))
    {
        pages->showPage ("ENV/LFO");
        if (auto* page = pages->getCurrentPage())
            if (auto* thumbs = findChild<LfoThumbBar> (*page); thumbs != nullptr && thumbs->onSelect != nullptr)
                thumbs->onSelect (0);
        for (const auto physicsShape : { LfoShapes::Bounce, LfoShapes::Pendulum, LfoShapes::Spring, LfoShapes::Friction })
        {
            shape->setValueNotifyingHost (shape->convertTo0to1 ((float) physicsShape));
            settle (350);
            save (*editor, outDir.getChildFile ("lfo-physics-" + juce::String (physicsShape) + ".png"));
        }
    }

    // The drawable Curve LFO editor.
    if (auto* shape = processor.apvts.getParameter ("lfo1_shape"))
    {
        shape->setValueNotifyingHost (shape->convertTo0to1 ((float) IlanaSynthAudioProcessor::curveShape));
        processor.setLfoCurve (0, LfoCurve::preset (9));
        pages->showPage ("ENV/LFO");

        if (auto* page = pages->getCurrentPage())
            if (auto* thumbs = findChild<LfoThumbBar> (*page); thumbs != nullptr && thumbs->onSelect != nullptr)
                thumbs->onSelect (0);

        settle (500);
        save (*editor, outDir.getChildFile ("lfo-curve.png"));

        // Steps, edited on the LFO graph, and the MSEG card's editor.
        shape->setValueNotifyingHost (shape->convertTo0to1 ((float) LfoShapes::Steps));
        settle (400);
        save (*editor, outDir.getChildFile ("lfo-steps.png"));
        if (auto* page = pages->getCurrentPage())
            if (auto* thumbs = findChild<LfoThumbBar> (*page); thumbs != nullptr && thumbs->onSelect != nullptr)
            {
                thumbs->onSelect (IlanaSynthAudioProcessor::numLfos);
                settle (400);
                save (*editor, outDir.getChildFile ("lfo-mseg.png"));
                thumbs->onSelect (0);
            }
        shape->setValueNotifyingHost (shape->convertTo0to1 ((float) IlanaSynthAudioProcessor::curveShape));
    }

    // The LFO pool, every card revealed, the last one selected.
    {
        for (int lfo = 0; lfo < IlanaSynthAudioProcessor::numLfos; ++lfo)
            processor.setRevealed (IlanaSynthAudioProcessor::Module::Lfo, lfo, true);
        pages->showPage ("ENV/LFO");
        if (auto* page = pages->getCurrentPage())
            if (auto* thumbs = findChild<LfoThumbBar> (*page); thumbs != nullptr && thumbs->onSelect != nullptr)
                thumbs->onSelect (13);
        settle (400);
        save (*editor, outDir.getChildFile ("lfo-pool-full.png"));
        pages->showPage ("MAIN");
        settle (300);
        save (*editor, outDir.getChildFile ("lfo-pool-main.png"));
        for (int lfo = 3; lfo < IlanaSynthAudioProcessor::numLfos; ++lfo)
            processor.setRevealed (IlanaSynthAudioProcessor::Module::Lfo, lfo, false);
    }

    // M8.1: the simulated LFO shapes, each with its picture and named knobs
    // (the envelope pool back to the patch's, so the chips show in full).
    {
        for (int env = 3; env < 16; ++env)
            processor.setRevealed (IlanaSynthAudioProcessor::Module::Envelope, env, false);
        pages->showPage ("ENV/LFO");
        if (auto* page = pages->getCurrentPage())
            if (auto* thumbs = findChild<LfoThumbBar> (*page); thumbs != nullptr && thumbs->onSelect != nullptr)
                thumbs->onSelect (0);
        const std::pair<int, const char*> shapes[] {
            { LfoSimShapes::Bounce, "bounce" }, { LfoSimShapes::Pendulum, "pendulum" }, { LfoSimShapes::Spring, "spring" },
            { LfoSimShapes::Friction, "friction" }, { LfoSimShapes::Lorenz, "lorenz" }, { LfoSimShapes::DoublePendulum, "double-pendulum" },
            { LfoSimShapes::Duffing, "duffing" }, { LfoSimShapes::Perlin, "perlin" }, { LfoSimShapes::Henon, "henon" } };
        for (const auto& [shape, name] : shapes)
        {
            if (auto* parameter = processor.apvts.getParameter ("lfo1_shape"))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) shape));
            const auto& info = LfoSimInfo::get (shape);
            for (int param = 0; param < LfoSimInfo::numParams; ++param)
                if (auto* parameter = processor.apvts.getParameter ("lfo1_p" + juce::String (param + 1)))
                    parameter->setValueNotifyingHost (info.params[(size_t) param].defaultValue);
            if (auto* rate = processor.apvts.getParameter ("lfo1_rate"))
                rate->setValueNotifyingHost (rate->convertTo0to1 (1.0f));
            settle (1500);
            save (*editor, outDir.getChildFile ("lfo-sim-" + juce::String (name) + ".png"));
        }
        if (auto* parameter = processor.apvts.getParameter ("lfo1_shape"))
            parameter->setValueNotifyingHost (0.0f);
        if (auto* rate = processor.apvts.getParameter ("lfo1_rate"))
            rate->setValueNotifyingHost (rate->convertTo0to1 (4.0f));
        settle (200);
    }

    // The filter overhaul: the grid's second page with the new models
    // (303 Acid on Filter 1, Vowel Morph on Filter 2).
    {
        const auto setType = [&processor] (const char* id, int type)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) type));
        };
        setType ("f1_type", FilterType::Acid303);
        setType ("f2_type", FilterType::VowelMorph);
        pages->showPage ("FILTER");
        settle (500);
        save (*editor, outDir.getChildFile ("filter-new-models.png"));
        setType ("f1_type", 0);
        setType ("f2_type", 0);
        settle (200);
    }

    // M8.3: the WEST card (its own card on the FILTER page), after the
    // filters, then in Filter 2's place.
    {
        if (auto* parameter = processor.apvts.getParameter ("west_on"))
            parameter->setValueNotifyingHost (1.0f);
        pages->showPage ("FILTER");
        settle (500);
        save (*editor, outDir.getChildFile ("filter-west.png"));
        if (auto* parameter = processor.apvts.getParameter ("west_pos"))
            parameter->setValueNotifyingHost (1.0f);
        settle (500);
        save (*editor, outDir.getChildFile ("filter-west-replace.png"));
        if (auto* parameter = processor.apvts.getParameter ("west_pos"))
            parameter->setValueNotifyingHost (0.0f);

        // UI review 7 (I7-1): the busiest SIGNAL FLOW: WEST, BODY, strings
        // and board on, the filters in parallel, OSC 1 bypassing them.
        const auto setPlain = [&processor] (const char* id, float plain)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
        };
        const std::array<const char*, 5> busy { "res_on", "sym_on", "sb_on", "filters_parallel", "osc1_route" };
        std::array<float, 5> before {};
        for (size_t i = 0; i < busy.size(); ++i)
            before[i] = processor.apvts.getRawParameterValue (busy[i])->load();
        for (size_t i = 0; i < busy.size(); ++i)
            setPlain (busy[i], i + 1 < busy.size() ? 1.0f : 3.0f);
        settle (500);
        save (*editor, outDir.getChildFile ("filter-flow-busy.png"));
        for (size_t i = 0; i < busy.size(); ++i)
            setPlain (busy[i], before[i]);
        setPlain ("west_on", 0.0f);
    }

    // M8.6: an oscillator playing a bounce of the patch.
    {
        IlanaSynthAudioProcessor::BounceRequest request;
        request.targetOsc = 1;
        request.muteOthers = false;
        processor.startBounce (request);
        for (int i = 0; i < 400 && processor.getBounceState() == IlanaSynthAudioProcessor::BounceState::Rendering; ++i)
            settle (50);
        pages->showPage ("OSC");
        settle (600);
        save (*editor, outDir.getChildFile ("osc-bounce.png"));
        for (const auto* id : { "osc2_mode", "osc2_on", "osc2_semi" })
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->getDefaultValue());
    }

    // M8.7: a Physical oscillator's card, a moment after a note (Felt Hammer
    // Board, then a feedback guitar).
    for (const auto& [presetName, file] : { std::pair<const char*, const char*> { "Felt Hammer Board", "physical-page.png" },
                                            { "", "physical-feedback.png" } })
    {
        if (juce::String (presetName).isNotEmpty())
            processor.loadFactoryPreset (processor.getFactoryPresetNames().indexOf (presetName));
        else
        {
            processor.loadFactoryPreset (0);
            for (const auto& [id, value] : { std::pair<const char*, float> { "osc1_mode", 1.0f }, { "osc1_excite", 10.0f },
                                             { "osc1_string_sustain", 0.6f }, { "osc2_on", 0.0f } })
                if (auto* parameter = processor.apvts.getParameter (id))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        }
        pages->showPage ("OSC");
        settle (300);
        juce::AudioBuffer<float> buffer (2, 512);
        for (int block = 0; block < 20; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 110), 0);
            buffer.clear();
            processor.processBlock (buffer, midi);
        }
        settle (350);
        save (*editor, outDir.getChildFile (file));
        processor.panic();
    }
    processor.loadFactoryPreset (0);

    // M8.5: the VECTOR page, with the pad on and a path.
    {
        for (const auto& [id, value] : { std::pair<const char*, float> { "vec_on", 1.0f }, { "vec_path", 1.0f },
                                         { "vec_drift", 0.3f }, { "macro1_evolve", 0.4f }, { "macro3_evolve", 0.2f },
                                         { "osc4_on", 1.0f } })
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        pages->showPage ("VECTOR");
        settle (600);
        save (*editor, outDir.getChildFile ("vector-page.png"));
        for (const auto* id : { "vec_on", "vec_path", "vec_drift", "macro1_evolve", "macro3_evolve", "osc4_on" })
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->getDefaultValue());
    }

    // M8.4: the second page of filter models.
    {
        if (auto* parameter = processor.apvts.getParameter ("f1_type"))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) FilterType::VowelBank));
        if (auto* parameter = processor.apvts.getParameter ("f1_reso"))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (0.6f));
        pages->showPage ("FILTER");
        settle (500);
        save (*editor, outDir.getChildFile ("filter-models-2.png"));
        if (auto* parameter = processor.apvts.getParameter ("f1_type"))
            parameter->setValueNotifyingHost (0.0f);
        if (auto* parameter = processor.apvts.getParameter ("f1_reso"))
            parameter->setValueNotifyingHost (parameter->getDefaultValue());
    }

    // M4: the Hammered Strings preset on the OSC page.
    if (const auto program = processor.getFactoryPresetNames().indexOf ("Hammered Strings"); program >= 0)
    {
        processor.loadFactoryPreset (program);
        pages->showPage ("OSC");
        settle (500);
        save (*editor, outDir.getChildFile ("keys-grand-osc.png"));
        if (auto* viewport = dynamic_cast<juce::Viewport*> (pages->getCurrentPage()))
        {
            viewport->setViewPosition (0, 10000);
            settle (200);
            save (*editor, outDir.getChildFile ("keys-grand-osc-scrolled.png"));
            viewport->setViewPosition (0, 0);
        }
        processor.loadFactoryPreset (0);
    }

    // Added oscillators: MAIN and OSC scroll, FM grows its matrix.
    {
        processor.addOscillator (3);
        processor.addOscillator (4);
        for (const auto* page : { "MAIN", "OSC", "FM" })
        {
            pages->showPage (page);
            settle (400);
            save (*editor, outDir.getChildFile ("added-osc-" + juce::String (page) + ".png"));

            if (auto* content = pages->getCurrentPage())
            {
                std::vector<juce::Viewport*> views;
                if (auto* own = dynamic_cast<juce::Viewport*> (content))
                    views.push_back (own);
                findAll<juce::Viewport> (*content, views);
                for (auto* view : views)
                    view->setViewPosition (0, 10000);
                if (! views.empty())
                {
                    settle (150);
                    save (*editor, outDir.getChildFile ("added-osc-" + juce::String (page) + "-scrolled.png"));
                    for (auto* view : views)
                        view->setViewPosition (0, 0);
                }
            }
        }
        processor.removeOscillator (4);
        processor.removeOscillator (3);
    }

    // M5/M6: a six-operator algorithm, a PD chain and a DAHDSR envelope.
    {
        const auto set = [&processor] (const juce::String& id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };

        processor.applyFmAlgorithm (10);
        set ("osc1_tune", 1.0f);
        set ("fm_noise3", 0.2f);
        pages->showPage ("FM");
        settle (400);
        save (*editor, outDir.getChildFile ("fm-dx-keys.png"));
        pages->showPage ("FILTER");
        settle (300);
        save (*editor, outDir.getChildFile ("filter-six-osc.png"));

        set ("osc1_mode", 0.0f);
        set ("osc1_table", 8.0f);
        set ("osc1_warp", (float) Warp::PdRes2);
        set ("osc1_warp_amt", 0.4f);
        set ("osc1_warp2", 8.0f);
        set ("osc1_warp2_amt", 0.3f);
        set ("osc1_pd_env", 2.0f);
        pages->showPage ("OSC");
        settle (400);
        save (*editor, outDir.getChildFile ("osc-pd-chain.png"));

        set ("amp_delay", 0.15f);
        set ("amp_hold", 0.3f);
        pages->showPage ("ENV/LFO");
        settle (400);
        save (*editor, outDir.getChildFile ("env-dahdsr.png"));

        set ("lfo1_shape", 12.0f);   // Bounce
        set ("lfo1_phys_b", 0.7f);
        settle (400);
        save (*editor, outDir.getChildFile ("lfo-bounce.png"));
    }

    // FM into oscillators that ignore it: OSC 2 as a sample, OSC 3 a string.
    {
        const auto set = [&processor] (const juce::String& id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };

        processor.loadFactoryPreset (0);
        set ("osc2_mode", 2.0f);
        set ("sub_mode", 1.0f);
        set ("fm_1to2", 0.4f);
        pages->showPage ("FM");
        settle (400);
        save (*editor, outDir.getChildFile ("fm-no-input.png"));
        processor.loadFactoryPreset (0);
        pages->showPage ("MAIN");
        settle (300);
    }

    // Sample mode (UI review 9, I9-22): OSC 1 empty with its LOAD, then an
    // SFZ's two zones under the wave.
    {
        auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ilana-shot-sfz");
        folder.createDirectory();
        {
            juce::AudioBuffer<float> tone (1, 22050);
            for (int i = 0; i < tone.getNumSamples(); ++i)
                tone.setSample (0, i, 0.5f * std::sin ((float) i * 0.06f) * std::exp (-(float) i / 8000.0f));
            juce::WavAudioFormat wav;
            auto file = folder.getChildFile ("tone.wav");
            file.deleteFile();
            if (auto stream = std::unique_ptr<juce::OutputStream> (file.createOutputStream()))
                if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (stream.get(), 44100.0, 1, 16, {}, 0)))
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer (tone, 0, tone.getNumSamples());
                }
            folder.getChildFile ("test.sfz").replaceWithText ("<region> sample=tone.wav lokey=36 hikey=59 pitch_keycenter=48\n"
                                                              "<region> sample=tone.wav lokey=60 hikey=84 pitch_keycenter=72\n");
        }
        processor.loadFactoryPreset (0);
        if (auto* parameter = processor.apvts.getParameter ("osc1_mode"))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (2.0f));
        pages->showPage ("OSC");
        settle (400);
        save (*editor, outDir.getChildFile ("osc-sample-empty.png"));
        // (The factory preset's queued sample clear runs on the message thread; on a busy box it can
        // land after the load and wipe it, which made this shot look like the empty one. Load again
        // until the sample is still there after the settle.)
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            processor.loadUserSample (0, folder.getChildFile ("test.sfz"));
            settle (500);
            if (processor.getSampleForOsc (0) != nullptr)
                break;
        }
        save (*editor, outDir.getChildFile ("osc-sample-sfz.png"));
        folder.deleteRecursively();
        processor.loadFactoryPreset (0);
        pages->showPage ("MAIN");
        settle (300);
    }

    // M7.1: the Generative card's EUCLID and PROB SEQ tabs, and strum.
    {
        const auto set = [&processor] (const juce::String& id, float value)
        {
            if (auto* parameter = processor.apvts.getParameter (id))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        };

        pages->showPage ("ARP/SEQ");
        settle (100);
        std::vector<CardTabs*> cardTabs;
        findAll<CardTabs> (*editor, cardTabs);
        CardTabs* engineTabs = nullptr;
        for (auto* candidate : cardTabs)
            if (candidate->getNames().contains ("EUCLID"))
                engineTabs = candidate;

        set ("euc_on", 1.0f);
        set ("euc_hits", 7.0f);
        set ("euc_rotate", 2.0f);
        set ("spray_strum", 1.0f);

        if (engineTabs != nullptr)
        {
            engineTabs->setSelected (1, true);
            settle (400);
            save (*editor, outDir.getChildFile ("gen-euclid.png"));

            set ("pseq_on", 1.0f);
            set ("pseq_length", 12.0f);
            for (int step = 1; step <= 16; ++step)
            {
                set ("pseq_chance" + juce::String (step), step % 3 == 0 ? 0.4f : 0.9f);
                set ("pseq_range" + juce::String (step), (float) ((step * 5) % 13));
                set ("pseq_ratchet" + juce::String (step), step % 4 == 0 ? 3.0f : 1.0f);
            }
            engineTabs->setSelected (2, true);
            settle (400);
            save (*editor, outDir.getChildFile ("gen-probseq.png"));

            // The CLIP tab with a short riff and chords at mixed velocities.
            {
                Clip riff;
                riff.bars = 2;
                const int pitches[] = { 48, 55, 60, 58, 55, 51, 53, 55 };
                for (int i = 0; i < 8; ++i)
                    riff.notes.push_back ({ (float) i * 0.75f, 0.5f, pitches[i], 60 + (i * 37) % 67 });
                for (const auto& [start, root] : { std::pair<float, int> { 0.0f, 63 }, { 4.0f, 65 } })
                    for (const auto interval : { 0, 4, 7 })
                        riff.notes.push_back ({ start, 3.5f, root + interval, 90 });
                processor.getClipState().setClip (0, riff);
                processor.clipsEdited();
            }
            set ("pseq_on", 0.0f);
            set ("clip_on", 1.0f);
            engineTabs->setSelected (3, true);
            settle (200);

            if (auto* roll = findChild<ClipEditor> (*editor))
            {
                roll->reload (true);
                // Review 7 (V7-38): part of the clip selected (the second
                // bar), so picked and unpicked notes show side by side.
                roll->selectStartingIn (4.0f, 8.0f);
                settle (200);
                save (*editor, outDir.getChildFile ("gen-clip-selected.png"));
                roll->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
            }

            settle (200);
            save (*editor, outDir.getChildFile ("gen-clip.png"));

            // Review 6: the roll expanded over a folded GENERATE.
            {
                std::vector<juce::TextButton*> buttons;
                findAll<juce::TextButton> (*editor, buttons);
                for (auto* button : buttons)
                    if (button->getButtonText() == "EXPAND" && visibleInTree (button))
                    {
                        button->triggerClick();
                        settle (300);
                        save (*editor, outDir.getChildFile ("gen-clip-expanded.png"));
                        button->triggerClick();
                        settle (100);
                        break;
                    }
            }

            // Review 6: the arp's step lanes, drawn, with the arp on.
            set ("clip_on", 0.0f);
            set ("euc_on", 0.0f);
            set ("arp_on", 1.0f);
            set ("arp_steps", 12.0f);
            for (int step = 1; step <= 16; ++step)
            {
                set ("arp_vel" + juce::String (step), (float) (step % 4 == 1 ? 120 : 60 + (step * 23) % 50));
                set ("arp_len" + juce::String (step), step % 6 == 0 ? 0.0f : step % 4 == 3 ? 1.6f : 0.6f + 0.1f * (float) (step % 3));
                set ("arp_pitch" + juce::String (step), (float) (step % 8 == 5 ? 12 : step % 8 == 7 ? -5 : step % 4 == 2 ? 7 : 0));
            }
            engineTabs->setSelected (0, true);
            settle (400);
            save (*editor, outDir.getChildFile ("gen-arp-lanes.png"));
            engineTabs->setSelected (0, true);
        }

        processor.loadFactoryPreset (0);
        pages->showPage ("MAIN");
        settle (300);
    }

    // A source chip clicked: the knobs it drives stay lit (the first chip that drives a visible knob).
    {
        pages->showPage ("MAIN");
        settle (300);
        std::vector<ModSourceChip*> chipsFound;
        std::vector<KnobControl*> knobsFound;
        findAll<ModSourceChip> (*editor, chipsFound);
        findAll<KnobControl> (*editor, knobsFound);

        for (auto* chip : chipsFound)
        {
            auto drives = false;
            for (auto* knob : knobsFound)
                drives = drives || (visibleInTree (knob) && knob->isDrivenBy (chip->getSourceIndex()));

            if (! visibleInTree (chip) || ! drives)
                continue;

            chip->togglePinned();
            settle (150);
            save (*editor, outDir.getChildFile ("mod-pinned-chip.png"));
            chip->togglePinned();
            break;
        }
    }

    // The preset browser, opened from the preset name.
    if (auto* display = findChild<PresetDisplay> (*editor); display != nullptr && display->onClick != nullptr)
    {
        display->onClick();
        settle (500);
        save (*editor, outDir.getChildFile ("preset-browser.png"));
        display->onClick();
        settle (400);
    }

    // The wavetable browser on its own.
    {
        TableBrowser browser (processor, "osc1_table", IlanaTheme::accent());
        browser.setLookAndFeel (&editor->getLookAndFeel());
        browser.setSize (740, 520);
        settle (100);
        save (browser, outDir.getChildFile ("table-browser.png"));

        // As the oscillator opens it: with LOAD... and LIBRARY.
        browser.setLoaders ([] {}, [] (const juce::File&) {});
        settle (100);
        save (browser, outDir.getChildFile ("table-browser-library.png"));

        // The size it opens at over a 100% editor (see TableBrowser::show).
        browser.setSize (912, 576);
        settle (100);
        save (browser, outDir.getChildFile ("table-browser-editor.png"));
    }

    editor.reset();
    return 0;
}
