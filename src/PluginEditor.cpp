#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

#include "PluginEditor.h"

#include <set>

#include <algorithm>
#include <array>

#include "dsp/BodyTuning.h"
#include "dsp/Modulation.h"
#include "dsp/TableFactory.h"
#include "gui/EnvelopeDisplay.h"
#include "gui/EqCurve.h"
#include "gui/FilterDisplay.h"
#include "gui/LfoDisplay.h"
#include "gui/CardTabs.h"
#include "gui/EnvThumbs.h"
#include "gui/FilterWidgets.h"
#include "gui/VectorPad.h"
#include "gui/PhysicalView.h"
#include "gui/FmDiagram.h"
#include "gui/FmWidgets.h"
#include "gui/FmOperatorInfo.h"
#include "gui/OperatorEnvDisplay.h"
#include "gui/OperatorPoolCards.h"
#include "gui/GenerativeWidgets.h"
#include "gui/ClipEditor.h"
#include "gui/TableBrowser.h"
#include "gui/WavetableEditor.h"
#include "gui/ZoneMapEditor.h"
#include "gui/SampleBrowser.h"
#include "gui/LfoShapeMenu.h"
#include "gui/LfoThumbs.h"
#include "gui/ModuleMenu.h"
#include "gui/ModulationCardView.h"
#include "gui/MatrixWidgets.h"
#include "gui/ParamControls.h"
#include "gui/FxInstanceControls.h"
#include "gui/OutputView.h"
#include "gui/ScopeDisplay.h"
#include "gui/SequencerEditors.h"
#include "gui/SubTabBar.h"
#include "gui/FxCarousel.h"
#include "gui/PatchCableView.h"
#include "gui/SectionColourPicker.h"
#include "gui/TutorialOverlay.h"
#include "gui/WaveDisplay.h"

// Page classes (anonymous namespace), in dependency order.
#include "gui/StateTabs.h"
#include "gui/pages/PageHelpers.h"
#include "gui/PhysicalApplies.h"
#include "gui/pages/OscPage.h"
#include "gui/pages/FilterVectorPhysicalPages.h"
#include "gui/pages/EnvLfoPages.h"
#include "gui/pages/ModSourcePanel.h"
#include "gui/pages/FmInputPages.h"
#include "gui/pages/SeqPage.h"
#include "gui/pages/PlayPage.h"
#include "gui/pages/MatrixPage.h"
#include "gui/FxInfoText.h"
#include "gui/pages/FxWidgets.h"
#include "gui/pages/FxPage.h"
#include "gui/pages/PatchLfo.h"
#include "gui/pages/PatchPage.h"


// The scope over the current page. Docked (the default, review 7) it fills
// the page area, as the docked preset browser does; FLOAT makes it a small
// panel at the page's bottom right that drags anywhere in the page area by
// its title bar, DOCK puts it back. The cross closes it. FLOAT / DOCK and
// the cross are the browser's pair, in the same place. Engine quality and
// oversampling live in the settings menu, not here.
class ScopePanel : public juce::Component
{
public:
    explicit ScopePanel (IlanaSynthAudioProcessor& p)
        : scope (p)
    {
        addAndMakeVisible (scope);

        dockButton.onClick = [this]
        {
            setDocked (! docked);

            if (onDockChange != nullptr)
                onDockChange (docked);
        };

        closeButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")));
        closeButton.setTooltip ("Close the scope (or click SCOPE in the tab row again)");
        closeButton.onClick = [this]
        {
            if (onClose != nullptr)
                onClose();
        };

        windowButton.setButtonText ("WINDOW");
        windowButton.setTooltip ("Open the scope in its own window, beside the synth: it covers no page");
        windowButton.onClick = [this]
        {
            if (onPopOut != nullptr)
                onPopOut();
        };

        addAndMakeVisible (windowButton);
        addAndMakeVisible (dockButton);
        addAndMakeVisible (closeButton);
        setDocked (true);
    }

    std::function<void()> onClose, onPopOut;
    std::function<void (bool docked)> onDockChange;

    bool isDocked() const { return docked; }

    void setDocked (bool shouldDock)
    {
        docked = shouldDock;
        dockButton.setButtonText (docked ? "FLOAT" : "DOCK");
        dockButton.setTooltip (docked ? "A small panel over the page instead, that you can drag"
                                      : "Fill the page area with the scope");
        resized();
        repaint();
    }

    // Where the panel goes in the page area: all of it when docked, else
    // its own size where it was dragged (first at the bottom right), kept
    // inside.
    juce::Rectangle<int> placeIn (juce::Rectangle<int> area)
    {
        pageArea = area;

        if (docked)
            return area.reduced (2, 0).withTrimmedBottom (2);

        const auto size = juce::Point<int> (juce::jmin (floatingWidth, area.getWidth() - 16), juce::jmin (floatingHeight, area.getHeight() - 16));

        if (! placed)
            topLeft = { area.getRight() - 8 - size.x, area.getBottom() - 8 - size.y };

        return constrain ({ topLeft.x, topLeft.y, size.x, size.y });
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (docked ? 0.0f : 4.0f);

        // Floating: a soft dark shadow under it, so the cards it covers
        // read as behind it rather than cut off.
        if (! docked)
        {
            for (int ring = 4; ring >= 1; --ring)
            {
                g.setColour (juce::Colours::black.withAlpha (0.12f));
                g.fillRoundedRectangle (bounds.expanded ((float) ring).translated (0.0f, 2.0f), 8.0f + (float) ring);
            }
        }

        // The browser's card and title: an accent dot, then the name.
        IlanaTheme::paintCard (g, bounds, 9.0f, IlanaTheme::accent().withAlpha (docked ? 0.3f : 0.45f));

        auto header = bounds.toNearestInt().reduced (14, 0).removeFromTop (34);
        g.setColour (IlanaTheme::accent());
        g.fillEllipse ((float) header.getX(), (float) header.getCentreY() - 3.0f, 6.0f, 6.0f);
        g.setColour (IlanaTheme::Ui::text);
        g.setFont (IlanaTheme::font (IlanaTheme::TextSize::body, true));
        g.drawText ("SCOPE", header.withTrimmedLeft (14), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (docked ? 0 : 4);
        auto header = area.removeFromTop (34).reduced (12, 0).withSizeKeepingCentre (area.getWidth() - 24, 20);
        closeButton.setBounds (header.removeFromRight (22));
        header.removeFromRight (4);
        dockButton.setBounds (header.removeFromRight (56));
        header.removeFromRight (4);
        windowButton.setBounds (header.removeFromRight (64));
        scope.setBounds (area.reduced (8, 0).withTrimmedBottom (8));
    }

    void mouseMove (const juce::MouseEvent& event) override
    {
        setMouseCursor (! docked && event.y < 34 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        dragging = ! docked && event.y < 34;
        dragStart = getPosition();
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (! dragging)
            return;

        const auto moved = constrain (getBounds().withPosition (dragStart + event.getOffsetFromDragStart()));
        topLeft = moved.getPosition();
        placed = true;
        setBounds (moved);
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

private:
    static constexpr int floatingWidth = 440, floatingHeight = 300;

    juce::Rectangle<int> constrain (juce::Rectangle<int> bounds) const
    {
        return pageArea.isEmpty() ? bounds : bounds.constrainedWithin (pageArea);
    }

    ScopeDisplay scope;
    juce::TextButton windowButton, dockButton, closeButton;
    juce::Rectangle<int> pageArea;
    juce::Point<int> topLeft, dragStart;
    bool docked = true, placed = false, dragging = false;
};

// The scope in its own window (step 14, Serum 2 review S23: "dock the scope
// as a resizable strip or a pop-out window"): beside the synth, so it covers
// no page. Resizable; closing it closes the scope, and SCOPE opens it in the
// page again (WINDOW pops it out once more).
class ScopeWindow : public juce::Component
{
public:
    ScopeWindow (IlanaSynthAudioProcessor& p, juce::LookAndFeel& look, std::function<void()> closed)
        : scope (p), onClosed (std::move (closed))
    {
        setName ("ilanaSynth scope");
        setLookAndFeel (&look);
        setOpaque (true);
        addAndMakeVisible (scope);
        constrainer.setSizeLimits (320, 200, 1800, 1200);
        centreWithSize (560, 340);
        addToDesktop (juce::ComponentPeer::windowHasTitleBar | juce::ComponentPeer::windowIsResizable
                      | juce::ComponentPeer::windowHasCloseButton | juce::ComponentPeer::windowAppearsOnTaskbar);
        if (auto* peer = getPeer())
            peer->setConstrainer (&constrainer);
       #if ! JUCE_LINUX
        // (On Linux "always on top" asks the window manager for an atom a
        // bare X server, the UI test's, doesn't have.)
        setAlwaysOnTop (true);
       #endif
    }

    ~ScopeWindow() override
    {
        removeFromDesktop();
        setLookAndFeel (nullptr);
    }

    void userTriedToCloseWindow() override
    {
        if (onClosed != nullptr)
            onClosed();
    }

    void paint (juce::Graphics& g) override { g.fillAll (IlanaTheme::Ui::bg); }
    void resized() override { scope.setBounds (getLocalBounds().reduced (8)); }

private:
    ScopeDisplay scope;
    juce::ComponentBoundsConstrainer constrainer;
    std::function<void()> onClosed;
};

IlanaSynthAudioProcessorEditor::IlanaSynthAudioProcessorEditor (IlanaSynthAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p)
{
   #if defined (_WIN32)
    // Some hosts create plugin windows on a thread that has been switched to
    // DPI-unaware, which makes Windows virtualise (bitmap-stretch) the editor
    // on scaled displays.  If the process itself is DPI aware we can safely
    // switch this thread back for the window creation so the window matches
    // the host's awareness; if the whole process is unaware there is nothing
    // a plug-in can do about the stretch, so we leave it alone.
    if (processDpiAwareness() > 0)
        previousDpiContext = setThreadDpiContext (perMonitorAwareV2Context());
   #endif

    setLookAndFeel (&lookAndFeel);
    tooltipWindow.setOpaque (false);

    {
        juce::PropertiesFile::Options options;
        options.applicationName = "ilanaSynth";
        options.filenameSuffix = "settings";
        options.folderName = "ilanaSynth";
        settings = std::make_unique<juce::PropertiesFile> (options);
        tableBrowserSettings() = settings.get();
    }

    themeIndex = juce::jlimit (0, IlanaTheme::numPalettes - 1, settings->getIntValue ("themeIndex", 0));
    IlanaAnim::fullMotion() = settings->getBoolValue ("fullMotion", IlanaAnim::fullMotion()); // (the snapshot tool turns it off first: still pictures)
    lookAndFeel.setAccent (juce::Colour (IlanaTheme::palette[themeIndex]));

    // The pages are built in this patch's colour coding.
    shownSectionColourVersion = processorRef.getSectionColourVersion();
    if (processorRef.getSectionColours() != SectionColours::shown())
    {
        SectionColours::shown() = processorRef.getSectionColours();
        ++SectionColours::version();
    }

    addAndMakeVisible (content);
    content.onPaint = [this] (juce::Graphics& g) { paintHeader (g); };

    content.addAndMakeVisible (logo);
    logo.isSounding = [this] { return processorRef.getActiveVoiceCount() > 0; };
    logo.outputLevel = [this] { return processorRef.getOutputPeak(); };
    content.addAndMakeVisible (infoStrip);

    // KEYS and ? sit at the right of the tab row, beside SCOPE (the status
    // line they shared is now only a hover line over the chips).
    keysButton.setClickingTogglesState (true);
    keysButton.setTooltip ("Keyboard\nShow or hide the on-screen keyboard.  It sits below the synth and makes the window taller.");
    keysButton.onClick = [this] { setKeyboardVisible (keysButton.getToggleState()); };
    helpButton.setTooltip ("Welcome tour\nThe one-minute tour: the essentials, the shortcuts and what's new.");
    helpButton.onClick = [this]
    {
        tutorial.setVisible (true);
        tutorial.toFront (false);
    };

    // A "new in" chip opens its page.
    tutorial.onShowPage = [this] (const juce::String& id) { showPage (id); };

    tutorial.onDismiss = [this] (bool dontShowAgain)
    {
        if (settings != nullptr)
        {
            settings->setValue ("seenIntro", dontShowAgain ? "1" : "0");
            settings->saveIfNeeded();
        }
    };

    auto* playPage = new PlayPage (p);
    auto* envLfoPage = new EnvLfoPage (p, *settings);

    // Eight tabs; the ones holding several pages switch them from the tab
    // row (PLAY: overview and vector, MOD: envelopes, LFOs and the MSEG).
    const auto addSection = [this] (const juce::String& name, std::initializer_list<std::tuple<juce::String, juce::String, juce::Component*>> pages)
    {
        auto* section = new SectionPage();

        for (const auto& [id, label, page] : pages)
            section->addPage (id, label, page);

        // The page slides in as a whole; its displays don't replay their
        // own entrances on top.
        section->onPageShown = [this] (juce::Component& page) { startTabTransition (&page); };
        section->switcher.onSelect = [this, section] (int index)
        {
            section->show (index, true);
        };
        tabs.addTab (name, IlanaTheme::Ui::panel, section, true);
        sections.push_back (section);
    };

    addSection ("PLAY", { { "MAIN", "OVERVIEW", playPage }, { "VECTOR", "VECTOR", new VectorPage (p) } });
    auto* matrixPage = new MatrixPage (p);
    // The matrix has its own tab (ilana, 2026-10-10).
    addSection ("MATRIX", { { "MATRIX", "MATRIX", matrixPage } });
    // An LFO card's DRIVES row opens its routing in the matrix.
    ModNames::openMatrixRow() = [this, matrixPage] (int slot)
    {
        showPage ("MATRIX");
        matrixPage->focusSlot (slot);
    };
    // A slot's matrix destination names the effect now in it (S4-13).
    ModNames::fxSlotEffectName() = [&p] (int slot)
    {
        if (const auto* type = p.apvts.getRawParameterValue ("fx_slot" + juce::String (slot)))
            if (const auto value = juce::roundToInt (type->load()); value > 0)
                return fxTypeName (value);
        return juce::String();
    };
    auto* fmPage = new FmPage (p);
    addSection ("FM",{ { "FM", "FM", fmPage } });
    addSection ("SEQ", { { "ARP/SEQ", "SEQ", new SeqPage (p, SeqPage::Part::notes) } });
    auto* fxPage = new FxPage (p);
    addSection ("FX", { { "FX", "RACK", fxPage } });
    // PATCH: the oscillators' routing (roadmap step 16) and the FX patcher
    // (17) as one canvas, each note's part and then the effects
    // (ilana, 2026-10-11).
    auto* patchPage = new PatchPage (p, *fxPage);
    addSection ("PATCH", { { "PATCH", "PATCH", patchPage } });
    patchPage->onShowPage = [this] (const juce::String& page) { showPage (page); };
    patchPage->onOpenSlot = [this, fxPage] (int slot)
    {
        showPage ("FX");
        fxPage->editInstance (slot);
    };
    FilterFlow::openRouting() = [this] { showPage ("PATCH"); };
    fxPage->onShowPatcher = [this] { showPage ("PATCH"); };
    // M7.5: ilanaSynth FX adds its INPUT page (last, so tab shortcuts stay).
    if (IlanaSynthAudioProcessor::isEffectBuild)
        addSection ("INPUT", { { "INPUT", "INPUT", new InputPage (p) } });

    // One-tab PLAY (ilana's mockup v3, 2026-10-25): the OSC, FILTER and MOD tabs are gone from the row, PLAY
    // holds what they did. Their pages stay behind the row (no tab button, last so the shortcuts stay):
    // the UI tests reach them (a pop-up editor holds every setting MOD's ENV / LFO page had).
    visibleTabCount = tabs.getNumTabs();
    auto* oscViewport = new OscPageViewport (p);
    addSection ("OSC", { { "OSC", "OSCILLATORS", oscViewport } });
    addSection ("FILTER", { { "FILTER", "FILTER", new FilterPage (p) } });
    addSection ("MOD", { { "ENV/LFO", "ENV / LFO", envLfoPage } });
    for (int index = visibleTabCount; index < tabs.getNumTabs(); ++index)
        if (auto* button = tabs.getTabbedButtonBar().getTabButton (index))
        {
            button->getProperties().set ("hiddenTab", true);
            button->setInterceptsMouseClicks (false, false);
        }

    // EDIT (or the title) on PLAY's ENVELOPE and LFO: the source's pop-up editor, as a dock chip opens it.
    playPage->onEditSource = [this] (int source, juce::Component& anchor) { toggleSourcePopover (source, anchor); };
    showPlayBody = [playPage] { playPage->showBody (0); };
    showPlayViews = [playPage] (int filter, int strings, int body)
    {
        if (filter >= 0)
            playPage->showFilter (filter);
        if (strings >= 0)
            playPage->showStrings (strings);
        if (body >= 0)
            playPage->showBody (body);
    };

    playPage->onOpenPage = [this] (const juce::String& name) { showPage (name); };
    // The header's VOICES: the OSC page's VOICE tab (one voice panel).
    // (The voice settings are in the dock now: this opens the voice mode's menu there.)
    showVoicePanel = [this]
    {
        if (voiceModeMenu != nullptr)
            voiceModeMenu->getComboBox().showPopup();
    };
    getOscCardBounds = [this, oscViewport] (int osc) -> juce::Rectangle<int>
    {
        if (auto* page = oscViewport->getPage())
            return getLocalArea (page, page->getCardBounds (osc));
        return {};
    };
    // EDIT OP ENV (PLAY and OSC): the operator's envelope lives on FM.
    editOperator = [this, fmPage] (int op)
    {
        fmPage->selectOperator (op);
        showPage ("FM");
    };
    playPage->onEditOperator = [this] (int op) { showOperatorEnvelope (op); };

    // Any page can open the FM page on an operator (-1: as it was), and
    // MOD on OP PITCH and OP LFO (the FM card's link to them).
    sampleBrowserHook() = [safe = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this)]
                          (int osc, std::function<bool (const juce::File&)> loadFile)
    {
        if (safe != nullptr)
            safe->openSampleBrowser (osc, std::move (loadFile));
    };
    FmOperatorInfo::hooks().openOperator = [this, fmPage] (int osc)
    {
        if (osc >= 0)
            fmPage->selectOperator (osc);
        showPage ("FM");
    };
    // OP PITCH and OP LFO: their pop-up editors by their dock chips, on PLAY (UI review 25: the MOD page has no tab now).
    FmOperatorInfo::hooks().openPitchAndLfo = [this, playPage]
    {
        showPage ("MAIN");
        if (getSourcePopoverSource() == (int) Mod::Source::OpPitchEnv)
            return;
        for (auto& chip : chips)
            if (chip->getSourceIndex() == (int) Mod::Source::OpPitchEnv && chip->isShowing())
            {
                toggleSourcePopover (chip->getSourceIndex(), *chip);
                return;
            }
        // (No chip in the dock's row: by PLAY's ENVELOPE card.)
        toggleSourcePopover ((int) Mod::Source::OpPitchEnv, playPage->getEnvelopeCard());
    };

    // The scope floats over any page.
    scopePanel = std::make_unique<ScopePanel> (p);
    static_cast<ScopePanel*> (scopePanel.get())->onClose = [this] { setScopeOpen (false); };
    static_cast<ScopePanel*> (scopePanel.get())->onPopOut = [this]
    {
        scopePanel->setVisible (false);
        setScopeWindowOpen (true);
    };
    if (settings != nullptr)
        static_cast<ScopePanel*> (scopePanel.get())->setDocked (settings->getBoolValue ("scopeDocked", true));
    static_cast<ScopePanel*> (scopePanel.get())->onDockChange = [this] (bool docked)
    {
        if (settings != nullptr)
        {
            settings->setValue ("scopeDocked", docked);
            settings->saveIfNeeded();
        }

        resized();
    };
    scopeButton.setClickingTogglesState (true);
    scopeButton.setTooltip ("Scope\nShow the oscilloscope and spectrum over any page.");
    scopeButton.onClick = [this] { setScopeOpen (scopeButton.getToggleState()); };

    content.addAndMakeVisible (tabs);

    // In front of the tab bar: the page switches, the scope button and panel.
    for (auto* section : sections)
        content.addChildComponent (section->switcher);

    content.addAndMakeVisible (scopeButton);
    content.addAndMakeVisible (keysButton);
    content.addAndMakeVisible (helpButton);
    content.addChildComponent (*scopePanel);

    // A source chip's click opens its editor over the page (the dock's
    // pop-out, 2026-10-09).
    {
        auto popover = std::make_unique<SourcePopover> (p, *settings);
        popover->onClose = [this] { closeSourcePopover(); };
        popover->onSizeChanged = [this]
        {
            if (auto* open = static_cast<SourcePopover*> (sourcePopover.get()); open != nullptr && open->isVisible())
                open->setBounds (open->placeIn (pageArea(), sourcePopoverAnchor));
        };
        popover->getEditor().onEditOperatorEnv = [this]
        {
            const auto operators = OperatorPool::operatorsOnEnv (processorRef);
            closeSourcePopover();
            showOperatorEnvelope (operators.empty() ? 0 : operators.front());
        };
        content.addChildComponent (*popover);
        sourcePopover = std::move (popover);
        sourceGrow = std::make_unique<IlanaAnim::PopGrow> (*sourcePopover);
    }

    // Bottom strip: macros, then performance controls, then master.
    for (int macro = 0; macro < Mod::numMacros; ++macro)
    {
        auto knob = std::make_unique<StripKnob> (p, "macro" + juce::String (macro + 1),
                                                 "Macro " + juce::String (macro + 1), macro,
                                                 modSourceColour ((int) Mod::macroSourceFor (macro)), false);
        // + ASSIGN: a new matrix row from this macro, then the matrix.
        knob->onAssign = [this] (int index)
        {
            for (int slot = 0; slot < Mod::maxSlots; ++slot)
            {
                const auto routing = processorRef.readModSlot (slot);

                if (routing.source == Mod::Source::None && routing.destination == 0)
                {
                    processorRef.performEdit ("Assign macro", [this, slot, index]
                    {
                        processorRef.clearModSlot (slot);
                        processorRef.setModSlotValue (slot, "src", (float) Mod::macroSourceFor (index));
                        processorRef.setModSlotValue (slot, "amt", 0.5f);
                    });
                    break;
                }
            }

            showPage ("MATRIX");
        };
        macroKnobs.push_back (std::move (knob));
    }
    {
        std::vector<StripKnob*> knobs;
        for (auto& knob : macroKnobs)
            knobs.push_back (knob.get());
        macroPager.setMacros (knobs);
        macroPager.onPageChanged = [this] (int page) { macroPageSwitch.setPage (page); };
        macroPageSwitch.onPick = [this] (int page) { macroPager.setPage (page, true); };
        content.addAndMakeVisible (macroPager);
        content.addAndMakeVisible (macroPageSwitch);
    }

    // VOICE (ilana 2026-10-22: in the dock beside MASTER, as Serum and Vital
    // do; it was the OSC tab's VOICE panel). Names beside the knobs, as the
    // macros' are.
    voiceModeMenu = std::make_unique<ComboControl> (p.apvts, "voice_mode", "MODE");
    voiceModeMenu->setCompactLayout (true);
    voiceModeMenu->setPackedMenu (true);
    voicesKnob = std::make_unique<StripKnob> (p, "poly_voices", "Voices", -1, IlanaTheme::Ui::text2, false);
    voicesKnob->setValueLineHidden (true);
    bendKnob = std::make_unique<StripKnob> (p, "bend_range", "Bend", -1, IlanaTheme::Ui::text2, false);
    glideKnob = std::make_unique<StripKnob> (p, "glide", "Glide", -1, IlanaTheme::Ui::text2, false);
    legatoToggle = std::make_unique<ToggleControl> (p.apvts, "glide_legato", "LEGATO");
    legatoToggle->setPackedName (ToggleControl::NamePlace::right);
    legatoToggle->setRightNameSize (11.0f);
    for (auto* component : std::initializer_list<juce::Component*> { voiceModeMenu.get(), voicesKnob.get(), bendKnob.get(), glideKnob.get(), legatoToggle.get() })
        content.addAndMakeVisible (component);
    // STEREO beside VOICES (mockup v3, 2026-10-25): the OSC tab's STEREO settings, in a pop-up over the page.
    {
        auto button = std::make_unique<PopButton> ("STEREO", StereoPopover::colour());
        button->setTagStyle (true);
        button->setTooltip ("Stereo settings\nSPREAD, PHASE, DRIFT and LOOSE: how wide and how loose the voices play.");
        button->onClick = [this] { setStereoOpen (stereoPopover == nullptr || ! stereoPopover->isVisible() || stereoGrow->isClosing()); };
        content.addAndMakeVisible (*button);
        stereoButton = std::move (button);
        auto popover = std::make_unique<StereoPopover> (p);
        popover->onClose = [this] { setStereoOpen (false); };
        popover->isOwnButton = [this] (juce::Component* c) { return c == stereoButton.get(); };
        content.addChildComponent (*popover);
        stereoPopover = std::move (popover);
        stereoGrow = std::make_unique<IlanaAnim::PopGrow> (*stereoPopover);
    }
    // Master is a setting rather than a sound control: neutral, so it doesn't
    // outshine the page (and yellow stays the macros' colour).
    masterKnob = std::make_unique<StripKnob> (p, "master", "Master", -1, IlanaTheme::Ui::text2, false);
    outputMeter = std::make_unique<OutputMeter> (p);
    content.addAndMakeVisible (*outputMeter);
    content.addChildComponent (modHoverPopup);

    headerScope.setStrip (true);
    headerScope.setInterceptsMouseClicks (false, false); // the preset display under it takes the click
    headerScope.onStripClick = [this]
    {
        scopeButton.setToggleState (! scopeButton.getToggleState(), juce::dontSendNotification);
        setScopeOpen (scopeButton.getToggleState());
    };
    content.addChildComponent (headerScope);

    voicesArea.setTooltip ("Voices\nNotes sounding, of the most that can. The voice settings (mode, voices, bend, glide, legato) are in the bottom bar; click for the voice mode.");
    voicesArea.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    voicesArea.onClick = [this] { showVoicePanel(); };
    content.addAndMakeVisible (voicesArea);

    // BPM and CPU are plain read-outs; VOICES opens the voice settings. All
    // three live in the tab bar now (tiny bold caps, label grey, value light).
    {
        const auto readoutFont = [] { return IlanaTheme::font (13.2f, true, true).withKerningFactor (0.04f); };
        const auto paintReadout = [readoutFont] (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& label, const juce::String& value,
                                                 juce::Colour valueColour, juce::Colour labelColour)
        {
            const auto font = juce::Font (readoutFont());
            const auto labelWidth = juce::GlyphArrangement::getStringWidthInt (font, label + " ");
            const auto valueWidth = juce::GlyphArrangement::getStringWidthInt (font, value);
            auto at = area.withTrimmedLeft (juce::jmax (0, area.getWidth() - labelWidth - valueWidth));
            g.setFont (readoutFont());
            g.setColour (labelColour);
            g.drawText (label, at.removeFromLeft (labelWidth), juce::Justification::centredLeft);
            g.setColour (valueColour);
            g.drawText (value, at, juce::Justification::centredLeft);
        };

        bpmArea.setInterceptsMouseClicks (false, false);
        bpmArea.onPaint = [this, paintReadout] (juce::Graphics& g, juce::Rectangle<int> area, bool)
        {
            paintReadout (g, area, "BPM", juce::String (processorRef.getCurrentBpm(), 1), IlanaTheme::Ui::text2, IlanaTheme::Ui::text3); // (the sheet's .stat b: t2)
        };
        content.addAndMakeVisible (bpmArea);

        cpuArea.setInterceptsMouseClicks (false, false);
        cpuArea.onPaint = [this, paintReadout] (juce::Graphics& g, juce::Rectangle<int> area, bool)
        {
            const auto cpu = processorRef.getCpuUsage() * 100.0f;
            const auto colour = cpu < 30.0f ? IlanaTheme::Ui::text2
                                            : (cpu < 60.0f ? IlanaTheme::Ui::text2.interpolatedWith (IlanaTheme::accent(), (cpu - 30.0f) / 30.0f)
                                                           : IlanaTheme::accent().interpolatedWith (juce::Colours::red, juce::jlimit (0.0f, 1.0f, (cpu - 60.0f) / 40.0f)));
            paintReadout (g, area, "CPU", juce::String (juce::roundToInt (cpu)) + "%", colour, IlanaTheme::Ui::text3);
        };
        content.addAndMakeVisible (cpuArea);

        voicesArea.onPaint = [this, readoutFont] (juce::Graphics& g, juce::Rectangle<int> area, bool hover)
        {
            // The voice mode when it isn't the usual Poly, so Mono or Legato
            // shows without opening the settings.
            const auto* modeValue = processorRef.apvts.getRawParameterValue ("voice_mode");
            const auto mode = modeValue != nullptr ? juce::roundToInt (modeValue->load()) : 0;
            const juce::String label (mode == 1 ? "MONO" : (mode == 2 ? "LEGATO" : "VOICES"));
            const auto font = juce::Font (readoutFont());
            const auto activeVoices = getVoicesText().upToFirstOccurrenceOf ("/", false, false).getIntValue();
            const auto maxVoices = getVoicesText().fromFirstOccurrenceOf ("/", false, false).getIntValue();
            const juce::String active (activeVoices), maxText (" / " + juce::String (maxVoices));
            const auto labelWidth = juce::GlyphArrangement::getStringWidthInt (font, label + " ");
            const auto activeWidth = juce::GlyphArrangement::getStringWidthInt (font, active);
            const auto maxWidth = juce::GlyphArrangement::getStringWidthInt (font, maxText);
            auto at = area.withTrimmedLeft (juce::jmax (0, area.getWidth() - labelWidth - activeWidth - maxWidth));
            g.setFont (readoutFont());
            g.setColour (hover ? IlanaTheme::Ui::text : IlanaTheme::Ui::text3);
            g.drawText (label, at.removeFromLeft (labelWidth), juce::Justification::centredLeft);
            g.setColour (hover ? IlanaTheme::Ui::text : IlanaTheme::Ui::text2);
            g.drawText (active, at.removeFromLeft (activeWidth), juce::Justification::centredLeft);
            g.drawText (maxText, at, juce::Justification::centredLeft);

            if (hover)
                g.fillRect (area.getX() + juce::jmax (0, area.getWidth() - labelWidth - activeWidth - maxWidth), area.getBottom() - 3,
                            labelWidth + activeWidth + maxWidth - 2, 1);
        };
    }

    // TUNING while a Scala scale (or an MTS-ESP master) retunes the notes: a
    // small accent pill with the scale's name in the free space of the header.
    tuningArea.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    tuningArea.onClick = [this] { showSettingsMenu (true); };
    tuningArea.onPaint = [this] (juce::Graphics& g, juce::Rectangle<int> area, bool hover)
    {
        const auto pill = area.toFloat().reduced (0.5f);
        g.setColour (IlanaTheme::accent().withAlpha (hover ? 0.22f : 0.12f));
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
        g.setColour (IlanaTheme::accent().withAlpha (0.7f));
        g.drawRoundedRectangle (pill, pill.getHeight() * 0.5f, 1.0f);
        g.setColour (hover ? IlanaTheme::Ui::text : IlanaTheme::accent().interpolatedWith (IlanaTheme::Ui::text, 0.4f));
        g.setFont (IlanaTheme::font (IlanaTheme::TextSize::tiny, true));
        g.drawText (tuningText.fromFirstOccurrenceOf ("TUNING: ", false, false), area.reduced (8, 0), juce::Justification::centred, true);
    };
    content.addChildComponent (tuningArea);

    content.addAndMakeVisible (*masterKnob);

    presetDisplay.onClick = [this] { togglePresetPanel(); };
    prevButton.setFlat (true);
    nextButton.setFlat (true);

    // Both arrows walk the browser's list (its filter, search and sort), so
    // they follow what the browser shows; the panel is made (closed) first.
    prevButton.onClick = [this] { stepPreset (-1); };
    nextButton.onClick = [this] { stepPreset (1); };
    favButton.setClickingTogglesState (true);
    favButton.onClick = [this] { toggleFavourite(); updateHeaderButtons(); };
    saveButton.setText ("SAVE");
    saveButton.setEmphasis (true);
    saveButton.onClick = [this] { savePreset(); };
    saveButton.setTooltip ("Save preset  (Ctrl+S)\nSaves over your preset; a factory or new preset asks for a name.");
    moreButton.onClick = [this] { showPresetMenu(); };
    undoButton.onClick = [this] { undoOrRedo (false); };
    redoButton.onClick = [this] { undoOrRedo (true); };
    historyButton.onClick = [this] { showHistoryMenu(); };
    abButton.setButtonText ("A");
    abButton.setTooltip ("Compare A | B\nFlip between two versions of the patch to compare them; the lit letter is the one playing.");
    abButton.onClick = [this] { toggleAB(); };
    diceButton.onClick = [this] { showDiceMenu(); };
    settingsButton.onClick = [this] { showSettingsMenu(); };

    for (auto* button : std::initializer_list<juce::Component*> { &presetDisplay, &prevButton, &nextButton, &favButton,
                                                                  &saveButton, &moreButton, &undoButton, &redoButton,
                                                                  &historyButton, &abButton, &diceButton, &settingsButton })
        content.addAndMakeVisible (*button);

    keyboard = std::make_unique<KeyboardStrip> (p);
    content.addAndMakeVisible (*keyboard);

    setWantsKeyboardFocus (true);

    // Macros are dragged from their own names in the strip, so they have no
    // chip here. The bar is built from the MOD page's pools (review 8, V8-3):
    // an LFO or envelope chip shows while its card is in the pool (added, or
    // in use), with the rest one click away behind "+". The performance
    // sources always show. Names are the sources' one name (ModNames), never
    // a code: a full region folds its last chips instead (layoutChips).
    // Three groups, as the MOD page's rows file them (I7-16): LFOs, then
    // envelopes, then the performance sources and the vector's X / Y. The
    // Operator Env's LFO and pitch envelope lead their groups while an
    // oscillator plays the Operator Env (they are the voice's own
    // modulators then: S8-2, I8-9) and are absent otherwise. The patch MSEG
    // (one drawn-shape system: LFO SHAPE > MSEG) shows only while an older
    // patch routes it.
    struct ChipSpec
    {
        Mod::Source source;
        int revealKind = -1, revealIndex = 0;
    };
    std::vector<ChipSpec> chipSpecs;
    chipSpecs.push_back ({ Mod::Source::OpLfo, -2 });
    for (int lfo = 0; lfo < IlanaSynthAudioProcessor::numLfos; ++lfo)
        chipSpecs.push_back ({ Mod::lfoSourceFor (lfo), (int) IlanaSynthAudioProcessor::Module::Lfo, lfo });
    chipSpecs.push_back ({ Mod::Source::Mseg, -4 });
    chipSpecs.push_back ({ Mod::Source::OpPitchEnv, -2 });
    for (int env = 0; env < 16; ++env)
        chipSpecs.push_back ({ ModNames::envelopeSourceFor (env), (int) IlanaSynthAudioProcessor::Module::Envelope, env });
    for (const auto source : { Mod::Source::Velocity, Mod::Source::KeyTrack, Mod::Source::ModWheel, Mod::Source::Aftertouch,
                               Mod::Source::Random, Mod::Source::InputEnv })
        chipSpecs.push_back ({ source });
    // The vector pad's position, while the vector is on (kind -3; I7-22).
    chipSpecs.push_back ({ Mod::Source::VectorX, -3 });
    chipSpecs.push_back ({ Mod::Source::VectorY, -3 });

    for (const auto& spec : chipSpecs)
    {
        // The input's envelope only exists in ilanaSynth FX.
        if (spec.source == Mod::Source::InputEnv && ! IlanaSynthAudioProcessor::isEffectBuild)
            continue;
        // One chip per source, whatever else lists it.
        if (std::any_of (chips.begin(), chips.end(), [&spec] (const auto& c) { return c->getSourceIndex() == (int) spec.source; }))
            continue;

        auto chip = makeSourceChip ((int) spec.source);
        content.addAndMakeVisible (*chip);
        chip->setVisible (spec.revealKind == -1);
        const auto group = ModNames::groupOf ((int) spec.source);
        chipGroup.push_back (group == ModNames::SourceGroup::lfo ? 0 : group == ModNames::SourceGroup::envelope ? 1 : 2);
        chips.push_back (std::move (chip));
        chipReveal.push_back ({ spec.revealKind, spec.revealIndex });
        chipWanted.push_back (spec.revealKind == -1);
        chipFolded.push_back (false);
    }

    const char* const groupNames[] { "LFOs", "Envelopes", "More sources" };
    for (size_t group = 0; group < groupChips.size(); ++group)
    {
        groupChips[group] = std::make_unique<ModSourceGroupChip> (groupNames[group]);
        groupChips[group]->onOpen = [this] (ModSourceGroupChip& groupChip)
        {
            if (chipTray.isOpenFor (groupChip))
                chipTray.close();
            else
                chipTray.openFor (groupChip);
        };
        content.addChildComponent (*groupChips[group]);
    }
    chipTray.makeChip = [this] (int source) { return makeSourceChip (source); };
    content.addChildComponent (chipTray);

    moreChipsButton.setButtonText ("+");
    moreChipsButton.setTooltip ("Add an LFO or envelope: it gets a chip here to drag, and a pill on PLAY's ENVELOPE or LFO card.");
    moreChipsButton.onClick = [this] { showChipPicker(); };
    content.addChildComponent (moreChipsButton);

    {
        // The presets the browser lists (a voice two cartridges carry counts once).
        auto listed = processorRef.getFactoryPresetNames().size();
        for (auto original : processorRef.getPresetRepeats())
            listed -= original >= 0 ? 1 : 0;
        tutorial.setPresetCount (listed);
    }
    content.addAndMakeVisible (tutorial);
    content.addChildComponent (undoToast);
    routeBadgeWatch.tick = [this] { updateRouteBadges(); updateEditedAndAbMarks(); updateOffSwitchGlow(); };
    routeBadgeWatch.startTimerHz (15);
    undoToast.onUndo = [this]
    {
        undoOrRedo (false);
        updateHeaderButtons();
    };
    undoToastKey = undoToastKeyNow();
    processorRef.getUndoManager().addChangeListener (this);
    content.addChildComponent (confirmOverlay);
    content.addChildComponent (saveOverlay);
    saveOverlay.setSettings (settings.get());
    saveOverlay.onSaved = [this] (const juce::File&)
    {
        presetSaved (saveOverlay.isEditing());

        if (auto then = std::exchange (afterSave, nullptr))
            then();
    };
    saveOverlay.onCancelled = [this] { afterSave = nullptr; };

    // Applied after adding: addAndMakeVisible forces the component visible.
    tutorial.setVisible (! settings->getBoolValue ("seenIntro", false));

    if (tutorial.isVisible())
        tutorial.toFront (false);

    // The keyboard opens on request (KEYS), so the pages get the room.
    keyboardVisible = settings->getBoolValue ("showKeyboard", false);
    keysButton.setToggleState (keyboardVisible, juce::dontSendNotification);
    keyboard->setVisible (keyboardVisible);

    updateHeaderButtons();
    updateUndoButtons();
    adoptLoadedFingerprint();

    // Drag the corner (or the host's window edge) to any size between 75% and
    // 200%; the aspect ratio is fixed and the size is remembered.
    setResizable (true, true);

    startTimer (250);
    addMouseListener (this, true); // mouseEnter: back from another instance's window
    frameSource = std::make_unique<IlanaAnim::FrameClock::Source> (*this);
    animator.onFrame = [this] { animate(); };

    presetDocked = settings->getBoolValue ("presetBrowserDocked", false);

    applyUiZoom ((float) settings->getDoubleValue ("uiZoom", 1.0));

    if (presetDocked && settings->getBoolValue ("presetBrowserDockOpen", false))
        setPresetDockShown (true);
    displayScaleApplied = settings->containsKey ("uiZoom") && ! juce::approximatelyEqual (uiZoom, 1.0f);

    headerScope.setVisible (settings->getBoolValue ("headerWaveform", true));
    setGpuRendering (settings->getBoolValue ("gpuRendering", true)
                     && juce::SystemStats::getEnvironmentVariable ("ILANA_NO_GPU", "").isEmpty());

    tabs.getTabbedButtonBar().addChangeListener (this);
}

void IlanaSynthAudioProcessorEditor::setGpuRendering (bool shouldUseGpu)
{
   #if ILANA_GPU_UI
    if (shouldUseGpu == (openGL != nullptr))
        return;

    if (shouldUseGpu)
    {
        // The whole component tree is painted into the context on its render
        // thread. Frames come from repaints (the frame clock, ChangeGate), not
        // a continuous loop, so an idle UI stays idle. The context attaches
        // once the editor is on screen: off-screen snapshots still draw in
        // software.
        openGL = std::make_unique<juce::OpenGLContext>();
        openGL->setComponentPaintingEnabled (true);
        openGL->setContinuousRepainting (false);
        openGL->attachTo (*this);
    }
    else
    {
        openGL->detach();
        openGL.reset();
        repaint();
    }
   #else
    juce::ignoreUnused (shouldUseGpu);
   #endif
}

bool IlanaSynthAudioProcessorEditor::isGpuRendering() const
{
   #if ILANA_GPU_UI
    return openGL != nullptr && openGL->isAttached();
   #else
    return false;
   #endif
}

void IlanaSynthAudioProcessorEditor::openWavetableEditor (int slot, juce::Colour colour)
{
    closeWavetableEditor();
    wavetableEditor = std::make_unique<WavetableEditor> (processorRef, slot, colour);
    wavetableEditor->onClose = [safe = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this)]
    {
        if (safe != nullptr)
            safe->closeWavetableEditor();
    };
    content.addAndMakeVisible (*wavetableEditor);
    wavetableEditor->setBounds (content.getLocalBounds().reduced (24, 62));
    wavetableEditor->grabKeyboardFocus();
}

void IlanaSynthAudioProcessorEditor::closeWavetableEditor()
{
    if (wavetableEditor != nullptr)
    {
        content.removeChildComponent (wavetableEditor.get());
        wavetableEditor.reset();
    }
}

void IlanaSynthAudioProcessorEditor::openZoneEditor (int oscIndex, juce::Colour colour)
{
    closeZoneEditor();
    closeWavetableEditor();
    zoneEditor = std::make_unique<ZoneMapEditor> (processorRef, oscIndex, colour);
    zoneEditor->onClose = [safe = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this)]
    {
        if (safe != nullptr)
            safe->closeZoneEditor();
    };
    content.addAndMakeVisible (*zoneEditor);
    zoneEditor->setBounds (content.getLocalBounds().reduced (24, 62));
    zoneEditor->grabKeyboardFocus();
}

void IlanaSynthAudioProcessorEditor::closeZoneEditor()
{
    if (zoneEditor != nullptr)
    {
        content.removeChildComponent (zoneEditor.get());
        zoneEditor.reset();
    }
}

void IlanaSynthAudioProcessorEditor::openSampleBrowser (int oscIndex, std::function<bool (const juce::File&)> loadFile)
{
    closeSampleBrowser();
    closeZoneEditor();
    closeWavetableEditor();
    oscIndex = juce::jlimit (0, OscillatorIds::count - 1, oscIndex);

    if (loadFile == nullptr)
    {
        loadFile = [this, oscIndex] (const juce::File& file)
        {
            if (! processorRef.loadUserSample (oscIndex, file, true))
                return false;

            // The oscillator plays the sample (Sample or Granular mode as it is).
            const auto modeId = juce::String (OscillatorIds::prefixes[(size_t) oscIndex]) + "_mode";
            if (auto* mode = processorRef.apvts.getRawParameterValue (modeId))
            {
                const auto current = (int) mode->load();
                if (current != 2 && current != 3)
                    processorRef.setOscillatorMode (oscIndex, OscMode::sample);
            }
            return true;
        };
    }

    sampleBrowser = std::make_unique<SampleBrowser> (processorRef, oscIndex, IlanaTheme::oscColour (oscIndex), settings.get(), std::move (loadFile));
    sampleBrowser->onClose = [safe = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this)]
    {
        if (safe != nullptr)
            safe->closeSampleBrowser();
    };
    content.addAndMakeVisible (*sampleBrowser);
    sampleBrowser->setBounds (content.getLocalBounds().reduced (24, 62));
    sampleBrowser->grabKeyboardFocus();
}

void IlanaSynthAudioProcessorEditor::closeSampleBrowser()
{
    if (sampleBrowser != nullptr)
    {
        content.removeChildComponent (sampleBrowser.get());
        sampleBrowser.reset();
    }
}

IlanaSynthAudioProcessorEditor::~IlanaSynthAudioProcessorEditor()
{
    routeBadgeWatch.stopTimer();
    sampleBrowser.reset();
    zoneEditor.reset();
    setGpuRendering (false); // before any child goes: the render thread paints them
    scopeWindow.reset();
    sampleBrowserHook() = nullptr;
    FmOperatorInfo::hooks().openOperator = nullptr;
    FmOperatorInfo::hooks().openPitchAndLfo = nullptr;
    ModNames::openMatrixRow() = nullptr;
    ModNames::fxSlotEffectName() = nullptr;
    tableBrowserSettings() = nullptr;
    closeWavetableEditor();
    tabs.getTabbedButtonBar().removeChangeListener (this);
    processorRef.getUndoManager().removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void IlanaSynthAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &processorRef.getUndoManager())
    {
        updateUndoToast();
        return;
    }

    // Picking a tab shows its page: the docked browser over it steps aside,
    // and the wavetable editor over the old page closes.
    if (presetDockShown)
        setPresetDockShown (false);

    closeWavetableEditor();
    closeSampleBrowser();

    layoutTabRow();

    if (auto* page = tabs.getCurrentContentComponent())
        page->repaint();

    startTabTransition();
}

void IlanaSynthAudioProcessorEditor::startTabTransition (juce::Component* pageToAnimate)
{
    if (transitionPage != nullptr)
    {
        transitionPage->setTransform ({});
        transitionPage->setAlpha (1.0f);
        transitionPage = nullptr;
    }

    if (auto* page = pageToAnimate != nullptr ? pageToAnimate : tabs.getCurrentContentComponent())
    {
        transitionPage = page;
        transitionStart = juce::Time::getMillisecondCounterHiRes();
        page->setAlpha (0.0f);
        page->setTransform (juce::AffineTransform::translation (0.0f, 8.0f));
        animator.startTimerHz (60);
    }
}

float IlanaSynthAudioProcessorEditor::displayScale() const
{
    auto scale = 1.0f;

    if (auto* peer = getPeer())
    {
        scale = juce::jmax (scale, (float) peer->getPlatformScaleFactor());

       #if defined (_WIN32)
        scale = juce::jmax (scale, queryMonitorScale (peer->getNativeHandle()));
       #endif
    }

    if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        scale = juce::jmax (scale, (float) display->scale);

    return juce::jlimit (1.0f, 3.0f, scale);
}

float IlanaSynthAudioProcessorEditor::hostScaleFactor() const
{
    const auto& transform = getTransform();
    return (float) juce::jmax (std::abs (transform.mat00), std::abs (transform.mat11));
}

void IlanaSynthAudioProcessorEditor::applyUiZoom (float newZoom)
{
    uiZoom = juce::jlimit (0.75f, 2.0f, newZoom);

    if (settings != nullptr)
    {
        settings->setValue ("uiZoom", uiZoom);
        settings->saveIfNeeded();
    }

    applyAspectAndLimits();
    setSize (juce::roundToInt ((float) currentDesignWidth() * uiZoom),
             juce::roundToInt ((float) totalDesignHeight() * uiZoom));
}

void IlanaSynthAudioProcessorEditor::applyAspectAndLimits()
{
    const auto width = currentDesignWidth();
    const auto height = totalDesignHeight();
    setResizeLimits (juce::roundToInt ((float) width * 0.75f), juce::roundToInt ((float) height * 0.75f),
                     width * 2, height * 2);

    if (auto* boundsConstrainer = getConstrainer())
        boundsConstrainer->setFixedAspectRatio ((double) width / (double) height);
}

void IlanaSynthAudioProcessorEditor::applyDisplayScale()
{
   #if ! defined (_WIN32)
    // macOS and Linux hosts scale plugin windows themselves.
    displayScaleApplied = true;
    return;
   #else
    // The thread context was switched to per-monitor aware for the window
    // creation; put it back now that the peer exists.
    if (previousDpiContext != nullptr && getPeer() != nullptr)
    {
        setThreadDpiContext (previousDpiContext);
        previousDpiContext = nullptr;
    }

    if (displayScaleApplied || getPeer() == nullptr)
        return;

    // If JUCE applied a host-window scale to this editor, it is already
    // handling the DPI and resizing ourselves would double up.
    if (hostScaleFactor() > 1.01f)
    {
        displayScaleApplied = true;
        return;
    }

    const auto scale = displayScale();

    if (scale <= 1.01f)
        return;

    displayScaleApplied = true;

    // The host is not scaling for us (our window is at design size), so size
    // ourselves for the real display DPI: the host then creates a matching
    // window and the UI renders at native resolution instead of being
    // bitmap-stretched.
    if (getHeight() > totalDesignHeight() + 20)
        return;

    setResizeLimits (juce::roundToInt ((float) currentDesignWidth() * 0.75f * scale), juce::roundToInt (540.0f * scale),
                     juce::roundToInt ((float) currentDesignWidth() * 1.5f * scale), juce::roundToInt (1080.0f * scale));

    if (auto* boundsConstrainer = getConstrainer())
        boundsConstrainer->setFixedAspectRatio ((double) currentDesignWidth() / (double) totalDesignHeight());

    setSize (juce::roundToInt ((float) currentDesignWidth() * scale),
             juce::roundToInt ((float) totalDesignHeight() * scale));
   #endif
}

// Pool chips follow the pools (a card added or in use), read against last
// tick's usage; the row re-lays out only when that changes. Routing a chip
// never moves it: only whether it shows does (V8-3).
void IlanaSynthAudioProcessorEditor::updateChipVisibility()
{
    auto changed = false;

    for (size_t i = 0; i < chips.size() && i < chipReveal.size(); ++i)
    {
        const auto [kind, index] = chipReveal[i];

        if (kind == -1)
            continue;

        const auto source = juce::jlimit (0, (int) Mod::Source::Count - 1, chips[i]->getSourceIndex());
        const auto lfoKind = (int) IlanaSynthAudioProcessor::Module::Lfo;
        const auto shown = kind == -2   ? FmOperatorInfo::anyOperatorEnv (processorRef)
                         : kind == -3   ? processorRef.apvts.getRawParameterValue ("vec_on")->load() > 0.5f
                         : kind == -4   ? msegModuleInUse (processorRef)
                         : kind == lfoKind ? processorRef.isLfoShown (index)
                         // AMP ENV leaves the bar while the voice plays the
                         // Operator Env and nothing uses it (UI review 9, S9-17).
                         : index == 0      ? envelopeInUse (processorRef, 0) || ! FmOperatorInfo::anyOperatorEnv (processorRef)
                                           : envelopeShown (processorRef, index);
        const auto wanted = shown || usedModSources[(size_t) source];

        if (chipWanted[i] != wanted)
        {
            chipWanted[i] = wanted;
            changed = true;
        }
    }

    // An LFO chip widens for its "B" while its shape has a second output.
    std::vector<bool> seconds (chips.size());
    for (size_t i = 0; i < chips.size(); ++i)
        seconds[i] = chips[i]->hasSecondOutput != nullptr && chips[i]->hasSecondOutput();
    if (seconds != chipSecondOutputs)
    {
        chipSecondOutputs = seconds;
        changed = true;
    }

    if (changed)
        resized();
}

// A source chip for the bar or a group's tray. It glows with its source
// while that source modulates something (macros, the wheel and pressure
// always).
std::unique_ptr<ModSourceChip> IlanaSynthAudioProcessorEditor::makeSourceChip (int sourceIndex)
{
    auto chip = std::make_unique<ModSourceChip> (ModNames::sourceUpper (sourceIndex), sourceIndex);
    chip->valueProvider = [this, source = (Mod::Source) sourceIndex]
    {
        using S = Mod::Source;
        const auto played = Mod::macroIndexFor (source) >= 0 || source == S::ModWheel || source == S::Aftertouch
                            || source == S::InputEnv;

        if (! played && ! usedModSources[(size_t) juce::jlimit (0, (int) S::Count - 1, (int) source)])
            return 0.0f;

        if ((source == S::Velocity || source == S::KeyTrack || source == S::Random)
            && processorRef.getActiveVoiceCount() == 0)
            return 0.0f;

        return processorRef.getSourceDisplayValue ((int) source);
    };
    chip->routeCount = [this, sourceIndex] { return modRouteCount (processorRef, sourceIndex); };
    chip->onClick = [this] (ModSourceChip& clicked) { toggleSourcePopover (clicked.getSourceIndex(), clicked); };
    chip->onMenu = [this] (ModSourceChip& clicked, int source) { showSourceMenu (clicked, source); };
    // A simulated LFO shape's second output gets a "B" on its chip.
    if (const auto lfo = Mod::lfoIndexFor ((Mod::Source) sourceIndex); lfo >= 0)
    {
        chip->secondIndex = (int) Mod::lfoBSourceFor (lfo);
        chip->hasSecondOutput = [this, lfo]
        {
            const auto* shape = processorRef.apvts.getRawParameterValue ("lfo" + juce::String (lfo + 1) + "_shape");
            return shape != nullptr && LfoSimShapes::isSim ((int) shape->load());
        };
    }
    return chip;
}

// A source chip's right-click menu: every knob the source drives (or scales,
// as VIA) with its depth, each item removing that route; Remove all; and
// the matrix row of each (ilana 2026-10-28, as Vital's sources do).
void IlanaSynthAudioProcessorEditor::showSourceMenu (juce::Component& target, int source)
{
    const auto slots = modSlotsUsing (processorRef, { (Mod::Source) source });
    juce::PopupMenu menu, matrixRows;
    menu.addSectionHeader (ModNames::source (source, &processorRef));

    if (slots.empty())
        menu.addItem (-1, "Drives nothing yet: drag it onto a knob", false);

    for (const auto slot : slots)
    {
        const auto routing = processorRef.readModSlot (slot);
        const auto via = routing.source != (Mod::Source) source;
        const auto label = ModNames::destination (routing.destination)
                           + (via ? juce::String ("  (as VIA)") : "  (" + juce::String (Mod::shownPercent (routing.depth, Mod::isBipolarRoute (routing))) + "%)");
        menu.addItem (1 + slot, "Remove from " + label);
        matrixRows.addItem (1001 + slot, label);
    }

    if (slots.size() > 1)
    {
        menu.addSeparator();
        menu.addItem (2000, "Remove all " + juce::String ((int) slots.size()) + " routes");
    }

    if (! slots.empty() && ModNames::openMatrixRow() != nullptr)
    {
        menu.addSeparator();
        menu.addSubMenu ("Show in the matrix", matrixRows);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target),
                        [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this), source] (int result)
                        {
                            if (safeThis == nullptr || result <= 0)
                                return;

                            auto& processor = safeThis->processorRef;
                            const auto name = ModNames::source (source, &processor);

                            if (result >= 1001)
                            {
                                if (ModNames::openMatrixRow() != nullptr)
                                    ModNames::openMatrixRow() (result - 1001);
                            }
                            else if (result == 2000)
                                processor.performEdit ("Remove " + name + " routes", [&processor, source]
                                                       { removeModRoutes (processor, { (Mod::Source) source }); });
                            else
                            {
                                const auto slot = result - 1;
                                processor.performEdit ("Remove " + name + " route", [&processor, slot, source]
                                {
                                    if (processor.readModSlot (slot).source == (Mod::Source) source)
                                        processor.clearModSlot (slot);
                                    else
                                        processor.setModSlotValue (slot, "aux", 0.0f); // it only scaled it
                                });
                            }
                        });
}

// The bar, in three regions (LFOs | envelopes | performance) and the "+"
// picker at the right end, each region at a fixed place and width, so a
// chip sits where it sat on the last patch (review 8, V8-3, S8-2, I8-9).
// The regions are sized from a fixed set of names (the widest likely set
// of each: the Operator Env's chips with three LFOs or envelopes, and the
// four main performance sources), the same at every zoom (V7-39). In a
// region the chips keep the pool's order at their full names' width, the
// spare shared out; when they don't fit, the last fold into a "+N" chip at
// the region's end (its tray holds them). Folding depends only on width
// and order, never on routing: sources the patch plays (the Operator Env's
// LFO and pitch envelope, the vector's X / Y while it is on) fold last.
// Nothing is ever shortened to a code.
void IlanaSynthAudioProcessorEditor::layoutChips (juce::Rectangle<int> row)
{
    if (chips.empty())
        return;

    std::array<std::vector<size_t>, 3> members;

    for (size_t i = 0; i < chips.size(); ++i)
        if (chipWanted[i])
            members[(size_t) chipGroup[i]].push_back (i);

    // A pool chip left to add (the Operator Env's come and go by themselves).
    auto picker = false;
    for (size_t i = 0; i < chips.size() && i < chipReveal.size(); ++i)
        picker = picker || (chipReveal[i].first >= 0 && ! chipWanted[i]);

    for (size_t i = 0; i < chips.size(); ++i)
        chipFolded[i] = false;

    const auto regions = chipRegions (row);
    chipSeparatorX = { juce::roundToInt (regions[0].getRight() + (regions[1].getX() - regions[0].getRight()) * 0.5f),
                       juce::roundToInt (regions[1].getRight() + (regions[2].getX() - regions[1].getRight()) * 0.5f) };

    for (size_t g = 0; g < 3; ++g)
    {
        const auto& list = members[g];
        const auto played = [&] (size_t index)
        {
            const auto kind = chipReveal[index].first;
            return kind == -2 || kind == -3 || chips[index]->getSourceIndex() == (int) Mod::Source::InputEnv;
        };
        const auto widthOf = [&]
        {
            auto sum = 0.0f;
            auto folded = 0;
            for (const auto index : list)
            {
                if (chipFolded[index])
                    ++folded;
                else
                    sum += chips[index]->getLayoutWidth();
            }
            return sum + (folded > 0 ? ModSourceGroupChip::layoutWidthFor ({}, folded) : 0.0f);
        };

        // From the end: the sources the patch doesn't play first.
        for (auto pass = 0; pass < 2 && widthOf() > regions[g].getWidth(); ++pass)
            for (auto k = (int) list.size() - 1; k >= 0 && widthOf() > regions[g].getWidth(); --k)
                if (pass == 1 || ! played (list[(size_t) k]))
                    chipFolded[list[(size_t) k]] = true;

        std::vector<int> folded;
        auto items = 0;
        for (const auto index : list)
        {
            items += chipFolded[index] ? 0 : 1;
            if (chipFolded[index])
                folded.push_back (chips[index]->getSourceIndex());
        }
        items += folded.empty() ? 0 : 1;

        const auto used = widthOf();
        const auto spare = 0.0f; // the dock's chips keep their natural width, left-aligned
        const auto squeeze = juce::jmin (1.0f, regions[g].getWidth() / juce::jmax (1.0f, used));
        auto x = regions[g].getX();
        const auto place = [&] (juce::Component& component, float natural)
        {
            const auto width = (natural + spare) * squeeze;
            component.setBounds (juce::Rectangle<float> (x, (float) row.getY(), width, (float) row.getHeight()).toNearestInt().reduced (2, 1));
            x += width;
        };

        for (const auto index : list)
            if (! chipFolded[index])
                place (*chips[index], chips[index]->getLayoutWidth());

        groupChips[g]->setSources (folded);
        groupChips[g]->setVisible (! folded.empty());

        if (! folded.empty())
            place (*groupChips[g], groupChips[g]->getLayoutWidth());
    }

    for (size_t i = 0; i < chips.size(); ++i)
        chips[i]->setVisible (chipWanted[i] && ! chipFolded[i]);

    moreChipsButton.setVisible (picker);
    moreChipsButton.setBounds (juce::Rectangle<int> (row.getRight() - chipPickerWidth, row.getY(), chipPickerWidth, row.getHeight()).reduced (2, 1));
}

// The three regions' places in the bar, fixed for a given width (and the
// same at every zoom): each in proportion to its planned names.
std::array<juce::Rectangle<float>, 3> IlanaSynthAudioProcessorEditor::chipRegions (juce::Rectangle<int> row)
{
    static const std::array<std::vector<const char*>, 3> planned { {
        { "OP LFO", "LFO 1", "LFO 2", "LFO 3" },
        { "OP PITCH", "AMP ENV", "FILT ENV", "FILT 2 ENV" },
        { "VELOCITY", "KEY TRACK", "MOD WHEEL", "PRESSURE", "RANDOM" },
    } };
    constexpr float groupGap = 14.0f; // room for the thin rule between groups

    std::array<float, 3> wanted {};
    auto total = 0.0f;
    for (size_t g = 0; g < 3; ++g)
    {
        for (const auto* name : planned[g])
            wanted[g] += ModSourceChip::layoutWidthFor (name);
        total += wanted[g];
    }

    const auto usable = (float) row.getWidth() - (float) chipPickerWidth - groupGap * 3.0f;
    std::array<juce::Rectangle<float>, 3> result;
    auto x = (float) row.getX();
    for (size_t g = 0; g < 3; ++g)
    {
        const auto width = usable * wanted[g] / total;
        result[g] = { x, (float) row.getY(), width, (float) row.getHeight() };
        x += width + groupGap;
    }
    return result;
}

// The "+" after the chips: the LFOs and envelopes not in the pool yet.
// Picking one adds it (its tile on MOD > ENV / LFO and its chip here).
void IlanaSynthAudioProcessorEditor::showChipPicker()
{
    juce::PopupMenu lfos, envelopes;

    for (size_t i = 0; i < chips.size() && i < chipReveal.size(); ++i)
    {
        const auto [kind, index] = chipReveal[i];

        if (kind < 0 || chipWanted[i])
            continue;

        auto& menu = kind == (int) IlanaSynthAudioProcessor::Module::Lfo ? lfos : envelopes;
        menu.addItem ((int) i + 1, ModNames::source (chips[i]->getSourceIndex()));
    }

    juce::PopupMenu menu;
    menu.addSectionHeader ("Add a source");
    if (lfos.getNumItems() > 0)
        menu.addSubMenu ("LFOs", lfos);
    if (envelopes.getNumItems() > 0)
        menu.addSubMenu ("Envelopes", envelopes);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&moreChipsButton),
                        [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this)] (int result)
                        {
                            if (safeThis != nullptr && result > 0)
                                safeThis->addPoolSource (result - 1);
                        });
}

void IlanaSynthAudioProcessorEditor::addPoolSource (int chipIndex)
{
    if (! juce::isPositiveAndBelow (chipIndex, (int) chipReveal.size()))
        return;

    const auto [kind, index] = chipReveal[(size_t) chipIndex];

    if (kind >= 0)
        processorRef.setRevealed ((IlanaSynthAudioProcessor::Module) kind, index, true);

    updateChipVisibility();
}

// A chip's click: its source's editor over the page, above the dock; a
// second click on the same chip closes it, another chip's switches it.
void IlanaSynthAudioProcessorEditor::toggleSourcePopover (int source, juce::Component& chip)
{
    auto* popover = static_cast<SourcePopover*> (sourcePopover.get());
    if (popover == nullptr)
        return;

    if (popover->isVisible() && ! sourceGrow->isClosing() && popover->getSource() == source)
    {
        closeSourcePopover();
        return;
    }

    sourcePopoverAnchor = content.getLocalArea (&chip, chip.getLocalBounds());
    popover->setSource (source);
    popover->setBounds (popover->placeIn (pageArea(), sourcePopoverAnchor));
    pinnedModSource() = source;
    if (! popover->isVisible() || sourceGrow->isClosing())
    {
        popover->setVisible (true);
        sourceGrow->open (sourcePopoverAnchor.getCentre().toFloat());
    }
    popover->toFront (true);
    content.repaint();
}

void IlanaSynthAudioProcessorEditor::closeSourcePopover()
{
    if (sourcePopover == nullptr || ! sourcePopover->isVisible() || sourceGrow->isClosing())
        return;

    if (pinnedModSource() == static_cast<SourcePopover*> (sourcePopover.get())->getSource())
        pinnedModSource() = 0;
    sourceGrow->close ([safe = juce::Component::SafePointer<juce::Component> (sourcePopover.get())]
                       {
                           if (safe != nullptr)
                               safe->setVisible (false);
                       });
    content.repaint();
}

int IlanaSynthAudioProcessorEditor::getSourcePopoverSource() const
{
    return sourcePopover != nullptr && sourcePopover->isVisible() && ! sourceGrow->isClosing() ? static_cast<SourcePopover*> (sourcePopover.get())->getSource() : 0;
}

// The macro pages: + ASSIGN shows on the first idle macro only, and the page
// switch marks the page not shown when a macro on it is in use.
void IlanaSynthAudioProcessorEditor::updateMacroStrip()
{
    auto anyAssigned = false;
    std::array<bool, MacroPager::pages> used {};
    for (int macro = 0; macro < (int) macroKnobs.size(); ++macro)
    {
        const auto* value = processorRef.apvts.getRawParameterValue ("macro" + juce::String (macro + 1));
        const auto named = processorRef.getMacroName (macro) != "Macro " + juce::String (macro + 1);
        const auto assigned = macroKnobs[(size_t) macro]->isAssigned();
        anyAssigned = anyAssigned || assigned;
        used[(size_t) MacroPager::pageOf (macro)] = used[(size_t) MacroPager::pageOf (macro)] || assigned || named
                                                    || (value != nullptr && value->load() > 0.001f);
    }

    for (int page = 0; page < MacroPager::pages; ++page)
        macroPageSwitch.setUsed (page, used[(size_t) page]);

    for (int macro = 0; macro < (int) macroKnobs.size(); ++macro)
        macroKnobs[(size_t) macro]->setQuietAssign (! anyAssigned && macro % MacroPager::perPage > 0);
}

void IlanaSynthAudioProcessorEditor::syncSectionColours (bool force)
{
    const auto version = processorRef.getSectionColourVersion();

    if (! force && version == shownSectionColourVersion)
        return;

    shownSectionColourVersion = version;
    const auto table = processorRef.getSectionColours();

    if (! force && table == SectionColours::shown())
        return;

    SectionColours::shown() = table;
    ++SectionColours::version();
    IlanaTheme::identityColourListeners (&processorRef).call ([] (IlanaTheme::IdentityColourListener& listener) { listener.identityColoursChanged(); });
    repaint();
}

void IlanaSynthAudioProcessorEditor::mouseEnter (const juce::MouseEvent&)
{
    // Back from another instance's window: this patch's colours again.
    if (processorRef.getSectionColours() != SectionColours::shown())
        syncSectionColours (true);
}

void IlanaSynthAudioProcessorEditor::timerCallback()
{
    applyDisplayScale();

    if (zoomNeedsSaving && ! juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown()
        && settings != nullptr)
    {
        zoomNeedsSaving = false;
        settings->setValue ("uiZoom", uiZoom);
        settings->saveIfNeeded();
    }


    // Colour coding: a picked colour, a loaded patch, an undo or an A/B swap
    // (or another instance's editor painted last): the pages set their
    // colours again.
    syncSectionColours (false);

    // Program changes, host state restores and A/B swaps change the preset
    // behind the editor's back.
    if (processorRef.getCurrentPresetName() != shownPresetName)
        updateHeaderButtons();

    updateMasterTooltip();

    // Sample files a loaded patch could not find: one prompt per change.
    if (processorRef.takeMissingSamplesPrompt())
        SamplePortability::promptMissing (processorRef, this);

    // "Edited" marker: cheap checksum of every parameter against the one
    // taken when the preset was loaded.
    if (presetLoadFlash <= 0.01f)
        presetDisplay.setPreset (presetDisplayName, shownCategory, isFavourite (shownPresetName), isPatchEdited());

    // Which sources the matrix uses, for the source chips' glow.
    updateChipVisibility();
    {
        usedModSources.fill (false);

        for (int slot = 0; slot < Mod::maxSlots; ++slot)
        {
            const auto routing = processorRef.readModSlot (slot);

            if (routing.isActive())
            {
                usedModSources[(size_t) juce::jlimit (0, (int) Mod::Source::Count - 1, (int) routing.source)] = true;
                usedModSources[(size_t) juce::jlimit (0, (int) Mod::Source::Count - 1, (int) routing.aux)] = true;
            }
        }
    }

    // The voice dots' tooltip counts them (each dot is a voice).
    {
        const auto* voicesValue = processorRef.apvts.getRawParameterValue ("poly_voices");
        const auto maxVoices = juce::jlimit (1, 32, voicesValue != nullptr ? juce::roundToInt (voicesValue->load()) : 32);
        const auto text = "Voices: " + juce::String (juce::jmin (maxVoices, processorRef.getActiveVoiceCount())) + " of "
                          + juce::String (maxVoices) + " playing\nNotes sounding / the most that can sound. "
                          + "Click for the voice mode (VOICE, in the bottom bar).";

        if (voicesArea.getTooltip() != text)
            voicesArea.setTooltip (text);
    }

    updateMacroStrip();
    updateTuningIndicator();

    if (transitionPage == nullptr)
    {
        updateUndoButtons();

        // The three readouts redraw when what they say changes (they
        // repainted four times a second regardless).
        const auto* voiceMode = processorRef.apvts.getRawParameterValue ("voice_mode");
        const auto signature = juce::String (processorRef.getCurrentBpm(), 1) + "|" + juce::String (juce::roundToInt (processorRef.getCpuUsage() * 100.0f))
                               + "|" + getVoicesText() + "|" + juce::String (voiceMode != nullptr ? juce::roundToInt (voiceMode->load()) : 0);

        if (signature != readoutSignature)
        {
            readoutSignature = signature;
            bpmArea.repaint();
            cpuArea.repaint();
            voicesArea.repaint();
        }
    }
}

// Runs on every display frame while the page slides in or the preset
// display flashes, then stops.
void IlanaSynthAudioProcessorEditor::animate()
{
    if (presetLoadFlash > 0.0f)
    {
        // Tuned at one 0.86 step per 60 ms.
        presetLoadFlash = IlanaAnim::decay (presetLoadFlash, 0.86f, animator.frameSeconds() * 1000.0f / 60.0f);

        if (presetLoadFlash <= 0.01f)
            presetLoadFlash = 0.0f;

        presetDisplay.setFlash (presetLoadFlash);
    }

    if (transitionPage != nullptr)
    {
        // A short slide up with a fade: no scaling, which would soften text.
        constexpr double duration = 160.0;
        const auto elapsed = juce::Time::getMillisecondCounterHiRes() - transitionStart;
        const auto progress = juce::jlimit (0.0, 1.0, elapsed / duration);
        const auto eased = IlanaAnim::easeOutCubic ((float) progress);

        transitionPage->setAlpha (eased);
        transitionPage->setTransform (juce::AffineTransform::translation (0.0f, std::round ((1.0f - eased) * 8.0f)));

        if (progress >= 1.0)
        {
            transitionPage->setTransform ({});
            transitionPage->setAlpha (1.0f);
            transitionPage = nullptr;
        }
    }

    if (transitionPage == nullptr && presetLoadFlash <= 0.0f)
        animator.stopTimer();
}

void IlanaSynthAudioProcessorEditor::paint (juce::Graphics& g)
{
    IlanaTheme::paintPageBackground (g, getLocalBounds());
}

void IlanaSynthAudioProcessorEditor::paintHeader (juce::Graphics& g)
{
    // The header (52 px).
    g.setColour (IlanaTheme::Ui::header);
    g.fillRect (juce::Rectangle<int> (0, 0, designWidth, headerHeight));
    g.setColour (IlanaTheme::Ui::line2);
    g.fillRect (juce::Rectangle<int> (0, headerHeight - 1, designWidth, 1));

    // The preset display's field (its rim, flash and text are the display's
    // own, over the live wave that sits on this field).
    g.setColour (IlanaTheme::Ui::panel);
    g.fillRoundedRectangle (presetDisplay.getBounds().toFloat().reduced (0.5f), 10.0f);

    // The rule between the edit and tool groups.
    g.setColour (IlanaTheme::Ui::line);
    for (const auto x : headerSeparatorX)
        if (x > 0)
            g.fillRect (juce::Rectangle<int> (x, 15, 1, 22));

    // The tab bar's ground and its hairline, under the tabs.
    g.setColour (IlanaTheme::Ui::bg);
    g.fillRect (juce::Rectangle<int> (0, headerHeight, designWidth, tabBarHeight));
    g.setColour (IlanaTheme::Ui::line2);
    g.fillRect (juce::Rectangle<int> (0, headerHeight + tabBarHeight - 1, designWidth, 1));

    // The dock (78 px): sources, then macros, master and the meter.
    if (! dockBounds.isEmpty())
    {
        g.setColour (IlanaTheme::Ui::header);
        g.fillRect (dockBounds);
        g.setColour (IlanaTheme::Ui::line2);
        g.fillRect (dockBounds.withHeight (1));

        g.setColour (IlanaTheme::Ui::line);
        for (const auto x : chipSeparatorX)
            if (x > 0)
                g.fillRect (juce::Rectangle<int> (x, chipRowBounds.getCentreY() - 8, 1, 16));
        // The rules round VOICE in the macro row.
        for (const auto x : dockDividerX)
            if (x > 0)
                g.fillRect (juce::Rectangle<int> (x, macroRowBounds.getY() + 3, 1, macroRowBounds.getHeight() - 6));
    }
}

// The status line's TUNING tag: the scale's name while tuning is on, or the
// MTS-ESP master's; hidden on 12-TET.
juce::String IlanaSynthAudioProcessorEditor::getVoicesText() const
{
    const auto* voicesValue = processorRef.apvts.getRawParameterValue ("poly_voices");
    const auto maxVoices = juce::jlimit (1, 32, voicesValue != nullptr ? juce::roundToInt (voicesValue->load()) : 32);
    return juce::String (juce::jmin (maxVoices, processorRef.getActiveVoiceCount())) + "/" + juce::String (maxVoices);
}

void IlanaSynthAudioProcessorEditor::updateTuningIndicator()
{
    juce::String text;

    if (processorRef.isMtsEspConnected())
        text = "TUNING: " + processorRef.getMtsEspScaleName() + " (MTS-ESP)";
    else if (processorRef.getTuningState().hasScale() && processorRef.apvts.getRawParameterValue ("tuning_on")->load() > 0.5f)
        text = "TUNING: " + processorRef.getTuningState().getDescription();

    if (text == tuningText)
        return;

    tuningText = text;
    tuningArea.setVisible (text.isNotEmpty());
    tuningArea.setTooltip (text.isNotEmpty() ? "Tuning\nThe notes play " + text.fromFirstOccurrenceOf ("TUNING: ", false, false)
                                                   + ", not 12-TET. Click to load another scale or switch back."
                                             : juce::String());
    tuningArea.repaint();
}

void IlanaSynthAudioProcessorEditor::resized()
{
    const auto scale = (float) getHeight() / (float) totalDesignHeight();
    IlanaTheme::uiScaleRef() = scale * hostScaleFactor();

    // A corner drag changes the zoom; remember it (saved on the timer so a
    // drag doesn't hit the disk on every step).
    if (displayScaleApplied && std::abs (scale - uiZoom) > 0.005f)
    {
        uiZoom = scale;
        zoomNeedsSaving = true;
    }

    content.setBounds (0, 0, designWidth, totalDesignHeight());
    content.setTransform (juce::AffineTransform::scale (scale));
    IlanaTheme::notifyUiScaleListeners();

    auto area = content.getLocalBounds();

    // ---- Header (52 px): the mark and name, the preset display with its
    // prev / next segments and the live scope, star, SAVE and the menu; at
    // the right undo, redo, history, A/B and the dice and settings keys.
    {
        // (The component is bigger than the mark by the glow's room, left, top and bottom.)
        const auto margin = (int) LogoComponent::glowMargin;
        logo.setBounds (14 - margin, 9 - margin, 160 + margin, 34 + 2 * margin);
    }
    logo.version = juce::String ("v") + appVersion;

    constexpr int key = 30, step = 10, rowY = 11;
    int x = designWidth - 14;
    const auto placeFromRight = [&] (juce::Component& c, int width)
    {
        x -= width;
        c.setBounds (x, rowY, width, key);
        x -= step;
    };
    placeFromRight (settingsButton, key);
    placeFromRight (diceButton, key);
    headerSeparatorX[1] = x - 1; // the rule between A/B and the tools: 10 px, 1 px, 10 px
    x -= 11;
    placeFromRight (abButton, 54);
    placeFromRight (historyButton, key);
    placeFromRight (redoButton, key);
    placeFromRight (undoButton, key);
    headerSeparatorX[0] = 0;

    presetDisplay.setBounds (180, 7, 330, 38);
    prevButton.setBounds (180, 7, PresetDisplay::arrowWidth, 38);
    nextButton.setBounds (180 + 330 - PresetDisplay::arrowWidth, 7, PresetDisplay::arrowWidth, 38);
    headerScope.setBounds (330, 13, 150, 26);
    favButton.setBounds (519, rowY, key, key);
    saveButton.setBounds (559, rowY, 75, key);
    moreButton.setBounds (644, rowY, key, key);
    // TUNING (while a scale or MTS-ESP retunes the notes) in the free space between
    // the file keys and the edit keys; its text is the scale's name.
    tuningArea.setBounds (684, rowY + 4, juce::jmax (0, undoButton.getX() - step - 684), key - 8);
    area.removeFromTop (headerHeight);

    tutorial.setBounds (content.getLocalBounds());
    confirmOverlay.setBounds (content.getLocalBounds());
    saveOverlay.setBounds (content.getLocalBounds());

    // ---- Bottom: the optional keyboard, then the 78 px dock.
    if (keyboard != nullptr)
    {
        keyboard->setVisible (keyboardVisible);

        if (keyboardVisible)
            keyboard->setBounds (area.removeFromBottom (keyboardHeight).reduced (14, 2));
    }

    dockBounds = area.removeFromBottom (dockHeight);
    layoutUndoToast();

    // The macro row (ilana 2026-10-22): a page of four macros and its 1 / 2
    // switch, a rule, VOICE (mode, voices, bend, glide, legato), a rule, then
    // MASTER and the meter at the right. The window's resize grip owns the
    // corner: the meter keeps clear of it.
    auto strip = dockBounds.withTrimmedTop (1 + 6 + 22 + 6).withTrimmedBottom (6).reduced (14, 0);
    macroRowBounds = strip;
    strip.removeFromRight (12); // (the meter ends 26 px from the edge: the resize grip's corner)
    outputMeter->setBounds (strip.removeFromRight (100).withSizeKeepingCentre (100, 36));
    strip.removeFromRight (6);
    masterKnob->setBounds (strip.removeFromRight (86));
    strip.removeFromRight (9);
    dockDividerX[1] = strip.getRight();
    strip.removeFromRight (6); // (LEGATO's box ends in a few px of air: the rule stands midway)

    // VOICE, right to left from the rule before MASTER.
    {
        constexpr int voiceGap = 6;
        legatoToggle->setBounds (strip.removeFromRight (68).withSizeKeepingCentre (68, 15));
        strip.removeFromRight (voiceGap);
        glideKnob->setBounds (strip.removeFromRight (78));
        strip.removeFromRight (voiceGap);
        bendKnob->setBounds (strip.removeFromRight (68));
        strip.removeFromRight (voiceGap);
        // VOICES' second line is the STEREO tag (mockup v3); the count reads in the tab row's VOICES.
        voicesKnob->setBounds (strip.removeFromRight (90));
        {
            const auto line = voicesKnob->valueLineBounds();
            const auto width = static_cast<PopButton*> (stereoButton.get())->idealWidth();
            stereoButton->setBounds (juce::Rectangle<int> (line.getX() - 1, line.getY() - 1, width, 16));
        }
        strip.removeFromRight (voiceGap);
        voiceModeMenu->setBounds (strip.removeFromRight (76).withSizeKeepingCentre (76, 22));
        strip.removeFromRight (9);
        dockDividerX[0] = strip.getRight();
        strip.removeFromRight (9);
    }

    if (stereoPopover != nullptr && stereoPopover->isVisible())
        stereoPopover->setBounds (stereoPopoverBounds());

    // The page switch, then the macros' page in what is left.
    macroPageSwitch.setBounds (strip.removeFromRight (16));
    strip.removeFromRight (4);
    macroPager.setBounds (strip);

    // The hover line: in the tab row's free middle (layoutTabRow), the dock
    // being full.
    infoStrip.setQuietArea ({}); // (it sits in the tab row now: the dock's controls explain themselves too)
    infoStrip.toFront (false);

    // The source chips: one row, 22 px chips.
    chipRowBounds = juce::Rectangle<int> (14, dockBounds.getY() + 5, designWidth - 28, 24);
    layoutChips (chipRowBounds);

    tabs.setTabBarDepth (tabBarHeight);
    tabs.setBounds (area);
    layoutTabRow();

    if (scopePanel != nullptr)
    {
        auto* panel = static_cast<ScopePanel*> (scopePanel.get());
        // A running fade-in would snap the panel back to its old bounds.
        juce::Desktop::getInstance().getAnimator().cancelAnimation (scopePanel.get(), false);
        scopePanel->setAlpha (1.0f);
        scopePanel->setBounds (panel->placeIn (pageArea()));
    }

    if (presetDockShown && presetPanel != nullptr)
    {
        presetPanel->setBounds (pageArea().reduced (2, 0).withTrimmedBottom (2));
        presetPanel->toFront (false);
    }

    if (sourcePopover != nullptr && sourcePopover->isVisible())
        sourcePopover->setBounds (static_cast<SourcePopover*> (sourcePopover.get())->placeIn (pageArea(), sourcePopoverAnchor));

    for (auto* overlay : std::initializer_list<juce::Component*> { &tutorial, &confirmOverlay, &saveOverlay })
        if (overlay->isVisible())
            overlay->toFront (false);
}

// The pages' area: under the tab row, above the chips.
juce::Rectangle<int> IlanaSynthAudioProcessorEditor::pageArea() const
{
    // (The overlays that dock over the pages keep a 14 px margin at each side.)
    return tabs.getBounds().withTrimmedTop (tabs.getTabBarDepth()).reduced (14, 0);
}

SectionPage* IlanaSynthAudioProcessorEditor::currentSection() const
{
    const auto index = tabs.getCurrentTabIndex();
    return index >= 0 && index < (int) sections.size() ? sections[(size_t) index] : nullptr;
}

void IlanaSynthAudioProcessorEditor::showOperatorEnvelope (int op)
{
    if (editOperator != nullptr)
        editOperator (op);
}

juce::Rectangle<int> IlanaSynthAudioProcessorEditor::stereoPopoverBounds() const
{
    const auto button = stereoButton != nullptr ? stereoButton->getBounds() : juce::Rectangle<int>();
    const auto area = pageArea();
    return juce::Rectangle<int> (StereoPopover::width, StereoPopover::height)
        .withPosition (button.getCentreX() - StereoPopover::width / 2, dockBounds.getY() - StereoPopover::height - 2)
        .constrainedWithin (area.reduced (4, 0).withTrimmedBottom (-2));
}

void IlanaSynthAudioProcessorEditor::setStereoOpen (bool open)
{
    if (stereoPopover == nullptr)
        return;
    if (open)
    {
        stereoPopover->setBounds (stereoPopoverBounds());
        stereoPopover->setVisible (true);
        stereoPopover->toFront (true);
        stereoGrow->open (stereoButton != nullptr ? stereoButton->getBounds().getCentre().toFloat()
                                                  : stereoPopover->getBounds().getCentre().toFloat());
    }
    else if (stereoPopover->isVisible() && ! stereoGrow->isClosing())
    {
        stereoGrow->close ([safe = juce::Component::SafePointer<juce::Component> (stereoPopover.get())]
                           {
                               if (safe != nullptr)
                                   safe->setVisible (false);
                           });
    }
    stereoButton->setToggleState (open, juce::dontSendNotification);
}

void IlanaSynthAudioProcessorEditor::showPage (const juce::String& id)
{
    if (id == "SCOPE")
    {
        setScopeOpen (true);
        return;
    }

    closeWavetableEditor(); // it belongs to the page it was opened from

    // STEPS & MSEG went into ENV / LFO (UI review 6, V5-7): its old id
    // opens there.
    if (id == "STEPS")
    {
        showPage ("ENV/LFO");
        return;
    }

    // The PHYSICAL tab folded into the oscillator cards (2026-10-10), and
    // the cards onto PLAY (2026-10-25): its old id opens PLAY.
    if (id == "PHYSICAL")
    {
        showPage ("MAIN");
        return;
    }

    // A Physical card's BODY link: PLAY, its BODY · BOARD card on BODY.
    if (id == "BODY")
    {
        showPage ("MAIN");
        if (showPlayBody != nullptr)
            showPlayBody();
        return;
    }

    for (int index = 0; index < (int) sections.size(); ++index)
    {
        const auto page = sections[(size_t) index]->indexOf (id);

        if (page < 0)
            continue;

        const auto sameTab = tabs.getCurrentTabIndex() == index;
        sections[(size_t) index]->show (page, sameTab);

        if (! sameTab)
            tabs.setCurrentTabIndex (index);

        layoutTabRow();
        return;
    }
}

juce::String IlanaSynthAudioProcessorEditor::getCurrentPageId() const
{
    if (auto* section = currentSection())
        return section->getCurrentId();

    return {};
}

juce::StringArray IlanaSynthAudioProcessorEditor::getPageIds() const
{
    juce::StringArray ids;

    for (auto* section : sections)
        for (int page = 0; page < section->getNumPages(); ++page)
            ids.add (section->getPageId (page));

    return ids;
}

juce::Component* IlanaSynthAudioProcessorEditor::getCurrentPage() const
{
    if (auto* section = currentSection())
        return section->getCurrentPage();

    return nullptr;
}

void IlanaSynthAudioProcessorEditor::setScopeWindowOpen (bool shouldBeOpen)
{
    if (shouldBeOpen && scopeWindow == nullptr)
    {
        scopeWindow = std::make_unique<ScopeWindow> (processorRef, lookAndFeel, [this]
        {
            // Closing the window closes the scope; it opens in the page next time.
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this)]
            {
                if (safe != nullptr)
                    safe->setScopeOpen (false);
            });
        });
        scopeWindow->setVisible (true);
    }
    else if (! shouldBeOpen)
    {
        scopeWindow.reset();
    }

    scopeButton.setToggleState (shouldBeOpen || scopePanel->isVisible(), juce::dontSendNotification);
    tabs.getTabbedButtonBar().setAlpha (1.0f);
    for (auto* section : sections)
        section->switcher.setAlpha (1.0f);
}

bool IlanaSynthAudioProcessorEditor::isScopeWindowOpen() const
{
    return scopeWindow != nullptr && scopeWindow->isVisible();
}

void IlanaSynthAudioProcessorEditor::setScopeOpen (bool shouldBeOpen)
{
    // Popped out: SCOPE brings the window forward, closing closes it.
    if (scopeWindow != nullptr)
    {
        if (shouldBeOpen)
        {
            scopeWindow->toFront (true);
            scopeButton.setToggleState (true, juce::dontSendNotification);
            return;
        }

        setScopeWindowOpen (false);
    }

    scopeButton.setToggleState (shouldBeOpen, juce::dontSendNotification);

    // A docked scope covers the page: the tab and the page switch it covers
    // step back, so SCOPE alone reads as the page shown (UI review 9, V9-14).
    const auto coversPage = [this] (bool open)
    {
        const auto alpha = open && static_cast<ScopePanel*> (scopePanel.get())->isDocked() ? 0.45f : 1.0f;
        tabs.getTabbedButtonBar().setAlpha (alpha);
        for (auto* section : sections)
            section->switcher.setAlpha (alpha);
    };

    if (shouldBeOpen == scopePanel->isVisible())
    {
        coversPage (shouldBeOpen);
        return;
    }

    coversPage (shouldBeOpen);

    if (shouldBeOpen)
    {
        resized();
        scopePanel->setAlpha (0.0f);
        scopePanel->setVisible (true);
        scopePanel->toFront (false);
        juce::Desktop::getInstance().getAnimator().fadeIn (scopePanel.get(), 180);
    }
    else
    {
        scopePanel->setVisible (false);
    }
}

bool IlanaSynthAudioProcessorEditor::isScopeOpen() const
{
    return (scopePanel != nullptr && scopePanel->isVisible()) || isScopeWindowOpen();
}

// The current tab's page switch follows the tabs, left-aligned beside the
// tab it belongs to (review 8, V8-36: at the right end, beside SCOPE, it read
// as a tool, not as the page's own pages); the scope, keys and help buttons
// sit at the right end of the tab row.
void IlanaSynthAudioProcessorEditor::layoutTabRow()
{
    // The tab buttons are placed again from the current font on every layout, so the row is the same on every page (S1-11).
    tabs.getTabbedButtonBar().resized();
    const auto bar = tabs.getBounds().withHeight (tabBarHeight);
    constexpr int gap = 6, buttonHeight = 24;
    const auto y = bar.getY() + (tabBarHeight - buttonHeight) / 2;

    // From the right: ?, KEYBOARD, SCOPE, a rule, then CPU, VOICES and BPM
    // (moved here from the header).
    auto x = bar.getRight() - 14;
    const auto place = [&] (juce::Component& component, int width, int after)
    {
        x -= width;
        component.setBounds (x, y, width, buttonHeight);
        x -= after;
    };
    place (helpButton, 26, gap);
    place (keysButton, keysButton.getIdealWidth(), gap);
    place (scopeButton, scopeButton.getIdealWidth(), 14);

    const auto textWidth = [] (const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidthInt (juce::Font (IlanaTheme::font (13.2f, true, true).withKerningFactor (0.04f)), text) + 2;
    };
    constexpr int readoutGap = 12;
    place (cpuArea, textWidth ("CPU 100%"), readoutGap);
    place (voicesArea, textWidth ("VOICES 32 / 32"), readoutGap);

    auto& tabBar = tabs.getTabbedButtonBar();
    auto* lastTab = visibleTabCount > 0 ? tabBar.getTabButton (visibleTabCount - 1) : nullptr; // (the last one with a button)
    const auto tabsRight = lastTab != nullptr ? tabs.getX() + tabBar.getX() + lastTab->getRight() : bar.getX();

    // BPM gives way when the current tab's page switch would not fit
    // whole beside the tabs (nine tabs and PLAY's OVERVIEW / VECTOR).
    auto switchWidth = 0;
    for (auto* section : sections)
        if (section == currentSection() && section->getNumPages() > 1)
            switchWidth = section->switcher.getIdealWidth();
    const auto bpmWidth = textWidth ("BPM 120.0");
    const auto showBpm = switchWidth <= x - bpmWidth - 12 - (tabsRight + 12);
    bpmArea.setVisible (showBpm);
    if (showBpm)
        place (bpmArea, bpmWidth, 0);
    statusStartX = x;
    auto row = juce::Rectangle<int> (tabsRight + 12, bar.getY() + 3, juce::jmax (0, statusStartX - 12 - (tabsRight + 12)), tabBarHeight - 7);

    for (auto* section : sections)
    {
        const auto current = section == currentSection() && section->getNumPages() > 1;
        section->switcher.setVisible (current);

        if (current)
        {
            section->switcher.setBounds (row.removeFromLeft (juce::jmin (row.getWidth(), section->switcher.getIdealWidth())));
            row.removeFromLeft (12);
        }
    }

    // The hover line (the dock is full since VOICE moved there, ilana 2026-10-22):
    // the tab row from the last tab to SCOPE, two lines high, over the page
    // switch and BPM, VOICES and CPU while it shows (it paints its own ground
    // and shows only while a control is rested on).
    infoStrip.setBounds (juce::Rectangle<int> (tabsRight + 12, bar.getY() + 1, juce::jmax (0, scopeButton.getX() - 14 - (tabsRight + 12)), tabBarHeight - 3));
    infoStrip.setVisible (true);
    infoStrip.toFront (false);
}

void IlanaSynthAudioProcessorEditor::setKeyboardVisible (bool shouldBeVisible)
{
    keyboardVisible = shouldBeVisible;
    keysButton.setToggleState (shouldBeVisible, juce::dontSendNotification);

    if (settings != nullptr)
    {
        settings->setValue ("showKeyboard", shouldBeVisible);
        settings->saveIfNeeded();
    }

    // The window grows (or shrinks back) by the strip at the current zoom;
    // the design above is untouched.
    if (getHeight() > 0)
    {
        const auto zoom = (float) getHeight() / (float) (designHeight + (shouldBeVisible ? 0 : keyboardHeight));
        applyAspectAndLimits();
        setSize (getWidth(), juce::roundToInt ((float) totalDesignHeight() * zoom));
    }

    resized();
}

// Every parameter, plus the patch data that isn't a parameter (drawn LFO
// steps and curves, remap curves, clips), so drawing lights EDITED too.
juce::int64 IlanaSynthAudioProcessorEditor::parameterFingerprint() const
{
    juce::int64 hash = 0;
    auto index = 1;

    for (auto* parameter : processorRef.getParameters())
    {
        hash += (juce::int64) juce::roundToInt (parameter->getValue() * 100000.0f) * (index * 2654435761LL % 1000003);
        ++index;
    }

    return hash ^ processorRef.getPatchDataHash();
}

bool IlanaSynthAudioProcessorEditor::isPatchEdited() const
{
    return parameterFingerprint() != loadedFingerprint;
}

bool IlanaSynthAudioProcessorEditor::asksBeforeReplacingEdits() const
{
    return settings == nullptr || settings->getBoolValue ("confirmReplaceEdited", true);
}

void IlanaSynthAudioProcessorEditor::setAsksBeforeReplacingEdits (bool shouldAsk)
{
    if (settings != nullptr)
    {
        settings->setValue ("confirmReplaceEdited", shouldAsk);
        settings->saveIfNeeded();
    }
}

// Runs then (true) at once while the patch is unedited; otherwise asks.
// Stepping through presets asks once: the patch it lands on is unedited.
// The question and the header's EDITED badge read the same isPatchEdited,
// and the badge is brought up to date before the question shows (it
// otherwise follows on the editor's timer). "Save and load" saves first:
// in place for your own preset, through SAVE AS otherwise (the load
// waits for it, and a cancelled SAVE AS loads nothing).
void IlanaSynthAudioProcessorEditor::confirmReplacingPatch (const juce::String& replacement, const juce::String& confirmText,
                                                            std::function<void (bool confirmed)> then)
{
    if (! isPatchEdited() || ! asksBeforeReplacingEdits())
    {
        then (true);
        return;
    }

    presetDisplay.setPreset (presetDisplayName, shownCategory, isFavourite (shownPresetName), true);

    const auto current = processorRef.getCurrentPresetName();
    ConfirmOverlay::Choices choices;
    choices.confirmText = confirmText;
    // A factory preset has no file to save over: the first step is a name
    // dialog, and the button says so (I13-16).
    const auto asksName = ! PresetFiles::loadedUserPreset (processorRef).existsAsFile();
    choices.alternativeText = juce::String (asksName ? "Save as... and " : "Save and ") + (confirmText.startsWith ("Roll") ? "roll" : "load");
    choices.onAlternative = [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this), then]
    {
        if (safeThis == nullptr)
            return;

        if (PresetFiles::loadedUserPreset (safeThis->processorRef).existsAsFile())
        {
            safeThis->savePreset();

            if (! safeThis->isPatchEdited())
                then (true);

            return;
        }

        safeThis->afterSave = [then] { then (true); };
        safeThis->savePresetAs();
    };

    confirmOverlay.ask ("Replace your edits?",
                        "'" + (current.isNotEmpty() ? (presetDisplayName.isNotEmpty() ? presetDisplayName : current) : juce::String ("Init"))
                            + "' has changes that aren't saved. " + replacement + " replaces them.",
                        choices,
                        [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this), then] (bool confirmed, bool dontAsk)
                        {
                            if (safeThis == nullptr)
                                return;

                            if (confirmed && dontAsk)
                                safeThis->setAsksBeforeReplacingEdits (false);

                            then (confirmed);
                        });
}

// The step on top of the history, with how deep it is: a new step changes it
// even when it has the last one's name.
juce::String IlanaSynthAudioProcessorEditor::undoToastKeyNow() const
{
    auto& undoManager = processorRef.getUndoManager();
    return undoManager.getUndoDescription() + "|" + juce::String (undoManager.getUndoDescriptions().size());
}

// A new step that threw something away shows the undo note (an undo or redo
// doesn't; nor does an edit that only joins the step on top).
void IlanaSynthAudioProcessorEditor::updateUndoToast()
{
    const auto key = undoToastKeyNow();
    const auto fromUndoOrRedo = std::exchange (undoToastSkip, false);

    if (key == undoToastKey)
        return;

    undoToastKey = key;
    const auto step = processorRef.getUndoManager().getUndoDescription();

    if (! fromUndoOrRedo && processorRef.getUndoManager().canUndo() && UndoToast::isDestructive (step))
    {
        undoToast.show (step);
        layoutUndoToast();
    }
}

// Centred over the page, just above the dock.
void IlanaSynthAudioProcessorEditor::layoutUndoToast()
{
    undoToast.setTopLeftPosition ((designWidth - undoToast.getWidth()) / 2, dockBounds.getY() - 10 - UndoToast::height);
}

// Edits still open (a gesture without its mouse-up, parameter changes not
// yet in the tree) join their own step before it is undone.
void IlanaSynthAudioProcessorEditor::undoOrRedo (bool redo)
{
    processorRef.endEdit();
    processorRef.apvts.copyState();
    undoToastSkip = true;
    undoToast.hide();

    // What the step moves, to show it afterwards (usability flair, pass 1).
    auto& undoManager = processorRef.getUndoManager();
    const auto step = redo ? undoManager.getRedoDescription() : undoManager.getUndoDescription();
    const auto& parameters = processorRef.getParameters();
    std::vector<float> before;
    before.reserve ((size_t) parameters.size());
    for (auto* parameter : parameters)
        before.push_back (parameter->getValue());

    const auto done = redo ? undoManager.redo() : undoManager.undo();

    if (done)
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this), before, step, redo]
        {
            if (safeThis != nullptr)
                safeThis->showUndoResult (before, step, redo);
        });
}

// The tabs' route badges: for the hovered (else the pinned) mod source,
// how many knobs on each main tab it drives. Recounted only when the
// source or its routes change.
void IlanaSynthAudioProcessorEditor::updateRouteBadges()
{
    const auto source = highlightedModSource() != 0 ? highlightedModSource() : pinnedModSource();
    std::set<int> destinations;

    if (source != 0)
        for (int i = 0; i < Mod::maxSlots; ++i)
        {
            const auto slot = processorRef.readModSlot (i);
            if ((int) slot.source == source && slot.destination != 0)
                destinations.insert (slot.destination);
        }

    auto key = (juce::uint64) (juce::uint32) source * 0x9e3779b97f4a7c15ull;
    for (auto destination : destinations)
        key = key * 31u + (juce::uint64) destination;

    if (key == routeBadgeKey && source == routeBadgeSource)
        return;

    routeBadgeKey = key;
    routeBadgeSource = source;
    auto& bar = tabs.getTabbedButtonBar();

    for (size_t s = 0; s < sections.size(); ++s)
    {
        std::set<int> found;
        if (! destinations.empty())
        {
            std::function<void (juce::Component&)> scan = [&] (juce::Component& parent)
            {
                for (auto* child : parent.getChildren())
                {
                    if (auto* knob = dynamic_cast<KnobControl*> (child))
                        if (const auto destination = knob->getModDestination(); destination != 0 && destinations.count (destination) > 0)
                            found.insert (destination);
                    scan (*child);
                }
            };
            scan (*sections[s]);
        }

        if (auto* button = bar.getTabButton ((int) s))
        {
            const auto count = (int) found.size();
            if ((int) button->getProperties().getWithDefault ("routeCount", 0) != count)
            {
                button->getProperties().set ("routeCount", count);
                button->getProperties().set ("routeColour", (juce::int64) modSourceColour (source).getARGB());
                button->repaint();
            }
        }
    }
}

// Every main tab's knobs whose parameter is in the set, counted by the
// tab's name; with markAb each knob also takes or drops the blue A|B mark.
std::map<juce::String, int> IlanaSynthAudioProcessorEditor::sectionKnobCounts (const std::set<juce::String>& parameterIds, bool markAb)
{
    std::map<juce::String, int> counts;
    const auto names = tabs.getTabNames();

    for (size_t s = 0; s < sections.size(); ++s)
    {
        auto count = 0;
        std::function<void (juce::Component&)> scan = [&] (juce::Component& parent)
        {
            for (auto* child : parent.getChildren())
            {
                if (auto* knob = dynamic_cast<KnobControl*> (child))
                {
                    const auto hit = parameterIds.count (knob->getParameterId()) > 0;
                    count += hit ? 1 : 0;
                    if (markAb)
                        knob->setDiffMark (hit);
                }
                scan (*child);
            }
        };
        scan (*sections[s]);
        counts[names[(int) s]] = count;
    }

    return counts;
}

// The tabs' small dots: orange where a knob moved since the preset loaded
// (or was saved), and, while A|B is hovered, blue where the other slot
// differs (those knobs get a blue mark too, and A|B's tooltip names them).
void IlanaSynthAudioProcessorEditor::updateEditedAndAbMarks()
{
    const auto& parameters = processorRef.getParameters();
    auto& bar = tabs.getTabbedButtonBar();

    // Edited since load: rescanned only when the patch's fingerprint moves.
    const auto fingerprint = parameterFingerprint();
    if (fingerprint != editedDotsKey)
    {
        editedDotsKey = fingerprint;
        std::set<juce::String> edited;
        const auto& loaded = processorRef.loadedPresetValues;
        if (fingerprint != loadedFingerprint)
            for (int i = 0; i < parameters.size() && i < (int) loaded.size(); ++i)
                if (std::abs (parameters[i]->getValue() - loaded[(size_t) i]) > 1.0e-5f)
                    if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameters[i]))
                        edited.insert (ranged->getParameterID());

        // (Dragging one knob keeps the same set: no rescan while it moves.)
        auto setKey = (juce::uint64) edited.size();
        for (const auto& id : edited)
            setKey = setKey * 31u + (juce::uint64) id.hashCode64();
        if (setKey == editedSetKey)
            return updateAbMarks();
        editedSetKey = setKey;

        const auto counts = sectionKnobCounts (edited, false);
        for (int t = 0; t < bar.getNumTabs(); ++t)
            if (auto* button = bar.getTabButton (t))
            {
                const auto it = counts.find (tabs.getTabNames()[t]);
                const auto n = it != counts.end() ? it->second : 0;
                if ((int) button->getProperties().getWithDefault ("editedCount", 0) != n)
                {
                    button->getProperties().set ("editedCount", n);
                    button->setTooltip (n > 0 ? juce::String (n) + (n == 1 ? " knob" : " knobs") + " changed on " + tabs.getTabNames()[t]
                                                    + " since loading " + processorRef.getCurrentPresetName()
                                              : juce::String());
                    button->repaint();
                }
            }
    }

    updateAbMarks();
}

void IlanaSynthAudioProcessorEditor::updateAbMarks()
{
    auto& bar = tabs.getTabbedButtonBar();
    // A|B: only while hovered and the other slot exists.
    const auto& other = showingA ? slotB : slotA;
    std::set<juce::String> differ;
    if (abButton.isMouseOver() && other.isValid())
        for (auto child : other)
            if (child.hasType ("PARAM"))
                if (auto* parameter = processorRef.apvts.getParameter (child["id"].toString()))
                {
                    const auto otherValue = parameter->convertTo0to1 ((float) child["value"]);
                    if (std::abs (otherValue - parameter->getValue()) > 1.0e-4f)
                        differ.insert (parameter->getParameterID());
                }

    auto key = (juce::uint64) differ.size() * 0x9e3779b97f4a7c15ull + (abButton.isMouseOver() ? 1u : 0u);
    for (const auto& id : differ)
        key = key * 31u + (juce::uint64) id.hashCode64();
    if (key == abMarksKey)
        return;
    abMarksKey = key;

    const auto counts = sectionKnobCounts (differ, true);
    juce::StringArray names, where;
    for (int t = 0; t < bar.getNumTabs(); ++t)
        if (auto* button = bar.getTabButton (t))
        {
            const auto it = counts.find (tabs.getTabNames()[t]);
            const auto n = it != counts.end() ? it->second : 0;
            button->getProperties().set ("abCount", n);
            button->repaint();
            if (n > 0 && ! (bool) button->getProperties()["hiddenTab"])
                where.add (tabs.getTabNames()[t]);
        }

    std::function<void (juce::Component&)> collect = [&] (juce::Component& parent)
    {
        for (auto* child : parent.getChildren())
        {
            if (auto* knob = dynamic_cast<KnobControl*> (child); knob != nullptr && differ.count (knob->getParameterId()) > 0)
                names.addIfNotAlreadyThere (knob->getLabelText().toUpperCase());
            collect (*child);
        }
    };
    for (auto& section : sections)
        collect (*section);

    const auto base = juce::String ("Compare A | B\nFlip between two versions of the patch to compare them; the lit letter is the one playing.");
    if (! other.isValid() || ! abButton.isMouseOver())
        abButton.setTooltip (base);
    else if (differ.empty())
        abButton.setTooltip (juce::String (showingA ? "B" : "A") + " is the same as " + (showingA ? "A" : "B") + ".\n" + base);
    else
    {
        auto list = names.size() > 6 ? names.joinIntoString (", ", 0, 6) + " and " + juce::String (names.size() - 6) + " more"
                                     : names.joinIntoString (", ");
        abButton.setTooltip (juce::String (showingA ? "B" : "A") + " differs from " + (showingA ? "A" : "B") + " in "
                             + juce::String ((int) differ.size()) + (differ.size() == 1 ? " setting" : " settings")
                             + (names.isEmpty() ? juce::String() : ": " + list) + (where.isEmpty() ? juce::String() : " (" + where.joinIntoString (", ") + ")")
                             + "\n" + base);
    }
}

// The control under the mouse, if it is greyed out (it or a box round it
// drawn faded) and belongs to a module with an ON switch that is off: that
// switch glows. A control's module is its parameter's prefix ("osc3_table"
// is OSC 3's, switched by "osc3_on").
void IlanaSynthAudioProcessorEditor::updateOffSwitchGlow()
{
    auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
    ToggleControl* target = nullptr;

    if (under != nullptr && isParentOf (under))
    {
        // (A greyed control often stops taking the mouse, so the component
        // "under" it is its card: find the deepest one drawn there.)
        std::function<juce::Component* (juce::Component&, juce::Point<int>)> deepest = [&] (juce::Component& parent, juce::Point<int> at) -> juce::Component*
        {
            for (int i = parent.getNumChildComponents(); --i >= 0;)
                if (auto* child = parent.getChildComponent (i); child->isVisible() && child->getBounds().contains (at))
                    return deepest (*child, at - child->getPosition());
            return &parent;
        };
        auto* top = deepest (*under, under->getMouseXYRelative());
        if (top == offGlowUnder.getComponent())
            return;
        offGlowUnder = top;

        juce::String id;
        auto dimmed = false;
        for (auto* c = top; c != nullptr && c != this; c = c->getParentComponent())
        {
            dimmed = dimmed || c->getAlpha() < 0.95f || ! c->isEnabled();
            if (id.isEmpty())
            {
                if (auto* knob = dynamic_cast<KnobControl*> (c))
                    id = knob->getParameterId();
                else if (auto* combo = dynamic_cast<ComboControl*> (c))
                    id = combo->getParameterId();
                else if (auto* toggle = dynamic_cast<ToggleControl*> (c); toggle != nullptr && ! toggle->getParameterId().endsWith ("_on"))
                    id = toggle->getParameterId();
            }
        }

        if (dimmed && id.isNotEmpty())
        {
            // (OSC 3 keeps its old "sub_" prefix, so the sub oscillator's
            // three "sub_" knobs are its own; BODY is switched by "res_on".)
            const auto subOscOwn = id == "sub_colour" || id == "sub_octave" || id == "sub_shape";
            const auto switches = [&] (const juce::String& switchId)
            {
                const auto module = switchId.dropLastCharacters (3);
                if (module == "subosc")
                    return subOscOwn || id.startsWith ("subosc_");
                if (module == "res")
                    return id.startsWith ("body_");
                return ! (module == "sub" && subOscOwn) && id.startsWith (module + "_");
            };
            std::function<void (juce::Component&)> find = [&] (juce::Component& parent)
            {
                for (auto* child : parent.getChildren())
                {
                    if (target != nullptr)
                        return;
                    if (auto* toggle = dynamic_cast<ToggleControl*> (child);
                        toggle != nullptr && toggle->getParameterId().endsWith ("_on") && ! toggle->isOn()
                        && switches (toggle->getParameterId()) && IlanaAnim::showing (*toggle))
                        target = toggle;
                    find (*child);
                }
            };
            find (content);
        }
    }

    if (target != offGlowSwitch.getComponent())
    {
        if (offGlowSwitch != nullptr)
            offGlowSwitch->setCallGlow (false);
        offGlowSwitch = target;
        if (target != nullptr)
            target->setCallGlow (true);
    }
}

// After an undo or redo: every knob it moved flashes once, and the undo
// note names the first ("Undid F1 RESO 60 % to 25 %", "and 2 more"), with
// a key that does the opposite; when that knob is on another page the note
// says which and its key, SHOW, goes there.
void IlanaSynthAudioProcessorEditor::showUndoResult (const std::vector<float>& before, const juce::String& step, bool redo)
{
    const auto& parameters = processorRef.getParameters();
    std::vector<int> changed;
    for (int i = 0; i < parameters.size() && i < (int) before.size(); ++i)
        if (std::abs (parameters[i]->getValue() - before[(size_t) i]) > 1.0e-5f)
            changed.push_back (i);

    std::vector<KnobControl*> knobs;
    std::function<void (juce::Component&)> collect = [&] (juce::Component& parent)
    {
        for (auto* child : parent.getChildren())
        {
            if (auto* knob = dynamic_cast<KnobControl*> (child))
                knobs.push_back (knob);
            collect (*child);
        }
    };
    // (A tab that isn't open is out of the window's tree, so walk every page too.)
    collect (content);
    for (auto& section : sections)
        for (int page = 0; page < section->getNumPages(); ++page)
            if (auto* holder = section->getPage (page); holder != nullptr && ! content.isParentOf (holder))
                collect (*holder);
    std::sort (knobs.begin(), knobs.end());
    knobs.erase (std::unique (knobs.begin(), knobs.end()), knobs.end());

    const auto verb = juce::String (redo ? "Redid " : "Undid ");
    const auto again = [this, redo] { undoOrRedo (! redo); };
    const auto oppositeKey = juce::String (redo ? "UNDO" : "REDO");

    if (changed.empty())
    {
        if (step.isNotEmpty())
        {
            undoToast.showNote (verb + step, oppositeKey, again);
            layoutUndoToast();
        }
        return;
    }

    KnobControl* firstKnob = nullptr;
    for (auto index : changed)
    {
        auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (parameters[index]);
        if (parameter == nullptr)
            continue;

        for (auto* knob : knobs)
        {
            if (knob->getParameterId() != parameter->getParameterID())
                continue;

            if (IlanaAnim::showing (*knob))
                knob->flashChanged();
            if (index == changed.front() && (firstKnob == nullptr || IlanaAnim::showing (*knob)))
                firstKnob = knob;
        }
    }

    auto* first = parameters[changed.front()];
    const auto name = firstKnob != nullptr && firstKnob->getLabelText().isNotEmpty() ? firstKnob->getLabelText().toUpperCase()
                                                                                     : first->getName (40).toUpperCase();
    const auto textOf = [first] (float value)
    {
        const auto label = first->getLabel();
        return first->getText (value, 0) + (label.isNotEmpty() ? " " + label : juce::String());
    };

    auto note = verb + name;
    std::function<void()> action = again;
    auto key = oppositeKey;

    if (firstKnob != nullptr && ! IlanaAnim::showing (*firstKnob))
    {
        // On another page: name the tab and offer to go there.
        for (size_t s = 0; s < sections.size(); ++s)
            for (int page = 0; page < sections[s]->getNumPages(); ++page)
                if (auto* holder = sections[s]->getPage (page); holder != nullptr && holder->isParentOf (firstKnob))
                {
                    note << " on " << tabs.getTabNames()[(int) s];
                    key = "SHOW";
                    action = [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this), id = sections[s]->getPageId (page),
                              knob = juce::Component::SafePointer<KnobControl> (firstKnob)]
                    {
                        if (safeThis == nullptr)
                            return;
                        safeThis->showPage (id);
                        if (knob != nullptr)
                            juce::Timer::callAfterDelay (250, [knob] { if (knob != nullptr) knob->flashChanged(); });
                    };
                }
    }
    else
    {
        note << "  " << textOf (before[(size_t) changed.front()]) << juce::String (juce::CharPointer_UTF8 ("  \xe2\x86\x92  ")) << textOf (first->getValue());
    }

    if (changed.size() > 1)
        note << " and " << juce::String ((int) changed.size() - 1) << " more";

    undoToast.showNote (note, key, action);
    layoutUndoToast();
}

void IlanaSynthAudioProcessorEditor::savePreset()
{
    const auto file = PresetFiles::loadedUserPreset (processorRef);

    if (! file.existsAsFile())
    {
        savePresetAs();
        return;
    }

    if (processorRef.savePresetToFile (file))
        presetSaved (true);
    else
        presetDisplay.showNotice ("COULDN'T SAVE");
}

void IlanaSynthAudioProcessorEditor::savePresetAs()
{
    if (presetPanel != nullptr && presetPanel->isOpen() && ! presetPanel->isDocked())
        presetPanel->close();

    saveOverlay.setBounds (content.getLocalBounds());
    saveOverlay.show();
}

// After a save: the patch is no longer EDITED, the browser lists the file,
// and the preset name says so for a moment.
void IlanaSynthAudioProcessorEditor::presetSaved (bool inPlace)
{
    rememberLoadedFingerprint();

    if (presetPanel != nullptr)
        presetPanel->refresh();

    updateHeaderButtons();
    presetDisplay.showNotice (inPlace ? "SAVED" : "SAVED AS NEW PRESET");
    presetLoadFlash = 1.0f;
    animator.startTimerHz (60);
}

void IlanaSynthAudioProcessorEditor::exportPreset()
{
    const auto directory = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                               .getChildFile ("ilanaSynth Presets");
    directory.createDirectory();

    fileChooser = std::make_unique<juce::FileChooser> ("Export preset",
                                                       directory.getChildFile (processorRef.getCurrentPresetName() + ".ilanapreset"),
                                                       "*.ilanapreset");

    juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> safeThis (this);

    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safeThis] (const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();

                                  if (file != juce::File() && safeThis != nullptr)
                                  {
                                      safeThis->processorRef.savePresetToFile (file.withFileExtension ("ilanapreset"));

                                      if (safeThis->presetPanel != nullptr)
                                          safeThis->presetPanel->refresh();
                                  }
                              });
}

void IlanaSynthAudioProcessorEditor::loadPreset()
{
    const auto directory = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                               .getChildFile ("ilanaSynth Presets");
    directory.createDirectory();

    fileChooser = std::make_unique<juce::FileChooser> ("Load preset", directory, "*.ilanapreset");

    juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> safeThis (this);

    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [safeThis] (const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();

                                  if (! file.existsAsFile() || safeThis == nullptr)
                                      return;

                                  safeThis->loadPresetFile (file);
                              });
}

// Loads a preset file (chosen, or dropped on the window), asking first over
// an edited patch.
void IlanaSynthAudioProcessorEditor::loadPresetFile (const juce::File& file)
{
    juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> safeThis (this);
    confirmReplacingPatch ("Loading '" + file.getFileNameWithoutExtension() + "'", "Load anyway",
                           [safeThis, file] (bool confirmed)
                           {
                               if (! confirmed || safeThis == nullptr)
                                   return;

                               auto& processor = safeThis->processorRef;
                               safeThis->closeWavetableEditor(); // it works on the old patch's table
                               processor.beginEdit ("Load " + file.getFileNameWithoutExtension());
                               const auto loaded = processor.loadPresetFromFile (file);
                               processor.endEdit();

                               if (! loaded)
                                   safeThis->presetDisplay.showNotice ("COULDN'T LOAD");
                           });
}

void IlanaSynthAudioProcessorEditor::loadPresetIndex (int index, std::function<void (bool loaded)> then)
{
    const auto names = processorRef.getAllPresetNames();

    if (! juce::isPositiveAndBelow (index, names.size()))
        return;

    const auto bank = processorRef.getAllPresetBanks()[index];
    // The click resolves to a file now: a folder that changes before the
    // answer (a second instance saving) can't shift which preset loads.
    const auto factoryCount = (int) processorRef.getFactoryPresetNames().size();
    const auto userFile = index >= factoryCount ? processorRef.getUserPresetFiles()[index - factoryCount] : juce::File();
    // While the browser is open, one "Load anyway" covers the session.
    const auto browsing = presetPanel != nullptr && (presetPanel->isOpen() || presetDockShown);
    const auto asking = browsing && ! browsingAccepted && isPatchEdited() && asksBeforeReplacingEdits();
    const auto load = [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this), index, userFile, name = names[index], then] (bool confirmed)
    {
        if (safeThis == nullptr)
            return;

        auto loaded = false;

        if (confirmed)
        {
            // One undo step for the whole preset, its data included.
            auto& self = *safeThis;
            self.closeWavetableEditor(); // it works on the old patch's table
            IlanaAnim::armMorphs(); // (the old shapes fade out over the new ones)
            self.processorRef.beginEdit ("Load " + name);
            loaded = userFile != juce::File() ? self.processorRef.loadUserPreset (userFile) : self.processorRef.loadPresetByIndex (index);
            self.processorRef.endEdit();

            if (loaded)
            {
                self.presetLoadFlash = 1.0f;
                IlanaAnim::notePresetLoad();
                IlanaAnim::playMorphs();
                self.animator.startTimerHz (60);

                self.updateHeaderButtons();
                self.rememberLoadedFingerprint();
            }
            else
                self.presetDisplay.showNotice ("COULDN'T LOAD"); // the patch and its name are unchanged
        }

        if (then != nullptr)
            then (loaded);
    };

    if (browsing && browsingAccepted)
    {
        load (true);
        return;
    }

    confirmReplacingPatch ("Loading '" + (bank.isNotEmpty() ? Presets::dx7DisplayName (names[index]) : names[index]) + "'", "Load anyway",
                           [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this), load, asking] (bool confirmed)
                           {
                               if (safeThis != nullptr && confirmed && asking)
                                   safeThis->browsingAccepted = true;

                               load (confirmed);
                           });
}

void IlanaSynthAudioProcessorEditor::showHistoryMenu()
{
    auto& undoManager = processorRef.getUndoManager();

    const auto undos = undoManager.getUndoDescriptions();
    const auto redos = undoManager.getRedoDescriptions();

    constexpr int maxEntries = 32;

    juce::PopupMenu menu;

    if (undos.isEmpty() && redos.isEmpty())
    {
        menu.addItem (1, "No history yet", false);
    }
    else
    {
        if (! redos.isEmpty())
        {
            menu.addSectionHeader ("REDO");

            for (int i = 0; i < juce::jmin (maxEntries, redos.size()); ++i)
                menu.addItem (2000 + i, redos[i].isNotEmpty() ? redos[i]
                                                              : "Redo change " + juce::String (i + 1));
        }

        if (! undos.isEmpty())
        {
            if (! redos.isEmpty())
                menu.addSeparator();

            menu.addSectionHeader ("UNDO");

            for (int i = 0; i < juce::jmin (maxEntries, undos.size()); ++i)
                menu.addItem (1000 + i, undos[i].isNotEmpty() ? undos[i]
                                                              : "Undo change " + juce::String (i + 1));
        }
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&historyButton),
                        [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this)] (int result)
                        {
                            if (safeThis == nullptr || result < 1000)
                                return;

                            if (result < 2000)
                            {
                                for (int i = 0; i <= result - 1000; ++i)
                                    safeThis->undoOrRedo (false);
                            }
                            else
                            {
                                for (int i = 0; i <= result - 2000; ++i)
                                    safeThis->undoOrRedo (true);
                            }

                            safeThis->updateHeaderButtons();
                        });
}

void IlanaSynthAudioProcessorEditor::updateHeaderButtons()
{
    const auto nameChanged = processorRef.getCurrentPresetName() != shownPresetName;
    shownPresetName = processorRef.getCurrentPresetName();

    const auto names = processorRef.getAllPresetNames();
    currentPresetIndex = names.indexOf (shownPresetName);
    shownCategory = juce::isPositiveAndBelow (currentPresetIndex, names.size())
                        ? processorRef.getAllPresetCategories()[currentPresetIndex]
                        : juce::String();

    // A DX7 voice shows its name in Title Case and its bank by the category
    // ("KEYS  ·  DX7 ROM1A"); the saved name is unchanged.
    const auto bank = juce::isPositiveAndBelow (currentPresetIndex, names.size()) ? processorRef.getAllPresetBanks()[currentPresetIndex]
                                                                                   : juce::String();
    presetDisplayName = bank.isNotEmpty() ? Presets::dx7DisplayName (shownPresetName) : shownPresetName;

    if (bank.isNotEmpty())
        shownCategory << juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  DX7 ")) << bank;

    if (nameChanged)
        adoptLoadedFingerprint();

    presetDisplay.setPreset (presetDisplayName, shownCategory, isFavourite (shownPresetName), isPatchEdited());
    favButton.setToggleState (isFavourite (shownPresetName), juce::dontSendNotification);
    favButton.setIconColour (isFavourite (shownPresetName) ? std::optional<juce::Colour> (juce::Colour (0xffffd447))
                                                           : std::nullopt);
    abButton.setButtonText (showingA ? "A" : "B");
    abButton.setToggleState (! showingA, juce::dontSendNotification);

    for (auto& knob : macroKnobs)
        knob->refreshName();
}

void IlanaSynthAudioProcessorEditor::updateUndoButtons()
{
    auto& undoManager = processorRef.getUndoManager();
    undoButton.setEnabled (undoManager.canUndo());
    redoButton.setEnabled (undoManager.canRedo());
    historyButton.setEnabled (undoManager.canUndo() || undoManager.canRedo());
}

bool IlanaSynthAudioProcessorEditor::isFavourite (const juce::String& presetName) const
{
    return settings != nullptr && settings->getValue ("fav_" + presetName, "0") == "1";
}

void IlanaSynthAudioProcessorEditor::toggleFavourite()
{
    if (settings == nullptr)
        return;

    const auto names = processorRef.getAllPresetNames();

    if (! juce::isPositiveAndBelow (currentPresetIndex, names.size()))
        return;

    const auto name = names[currentPresetIndex];
    settings->setValue ("fav_" + name, isFavourite (name) ? "0" : "1");
    settings->saveIfNeeded();
}

void IlanaSynthAudioProcessorEditor::toggleAB()
{
    closeWavetableEditor(); // the other patch has its own tables

    // The whole patch, not just the parameters: tables, samples, mod remaps,
    // the FX patch, clips, tuning and the preset name go with each slot.
    auto current = processorRef.captureWholePatch();

    if (showingA)
    {
        slotA = current;

        if (! slotB.isValid())
            slotB = slotA;

        processorRef.restoreWholePatch (slotB);
    }
    else
    {
        slotB = current;
        processorRef.restoreWholePatch (slotA);
    }

    processorRef.easeNextBlock(); // the new patch continues from the old one's last sample
    showingA = ! showingA;
    abButton.setButtonText (showingA ? "A" : "B");
    abButton.setToggleState (! showingA, juce::dontSendNotification);
    updateHeaderButtons();
}

void IlanaSynthAudioProcessorEditor::setTheme (int newThemeIndex)
{
    themeIndex = juce::jlimit (0, IlanaTheme::numPalettes - 1, newThemeIndex);
    lookAndFeel.setAccent (juce::Colour (IlanaTheme::palette[themeIndex]));

    if (settings != nullptr)
    {
        settings->setValue ("themeIndex", themeIndex);
        settings->saveIfNeeded();
    }

    sendLookAndFeelChange();
}

// The preset's own level (output_trim, review 7): each factory preset sets
// it so that MASTER reads 0 dB, and it is saved with a patch, so two patches
// at MASTER 0 dB can sit 15 dB apart. MASTER's hover names it, and the
// preset menu resets it.
juce::String IlanaSynthAudioProcessorEditor::presetLevelText() const
{
    const auto* trim = processorRef.apvts.getRawParameterValue ("output_trim");
    const auto db = trim != nullptr ? trim->load() : 0.0f;
    return (db > 0.05f ? "+" : "") + juce::String (std::abs (db) < 0.05f ? 0.0f : db, 1) + " dB";
}

void IlanaSynthAudioProcessorEditor::updateMasterTooltip()
{
    const auto text = presetLevelText();

    if (masterKnob == nullptr || text == shownPresetLevel)
        return;

    auto& knob = masterKnob->getKnob();

    if (masterBaseTooltip.isEmpty())
        masterBaseTooltip = knob.getTooltip();

    shownPresetLevel = text;
    const auto tooltip = masterBaseTooltip + "  This preset's own level: " + text
                         + " (set when it loads, saved with it; the preset menu resets it).";
    knob.setTooltip (tooltip);

    for (auto* child : knob.getChildren())
        if (auto* slider = dynamic_cast<juce::Slider*> (child))
            slider->setTooltip (tooltip);
}

void IlanaSynthAudioProcessorEditor::showPresetMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "New patch");
    menu.addSeparator();
    menu.addItem (2, "Save preset", true);
    menu.addItem (7, "Save preset as...");
    menu.addItem (5, "Export preset file...");
    menu.addItem (3, "Load preset file...");
    menu.addItem (4, "Open user preset folder");
    menu.addSeparator();
    menu.addItem (8, "Reset preset level  (" + presetLevelText() + ")", presetLevelText() != "0.0 dB");
    menu.addSeparator();
    menu.addItem (6, "Import DX7 / Dexed bank (.syx)...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&moreButton),
                        [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this)] (int result)
                        {
                            if (safeThis == nullptr)
                                return;

                            switch (result)
                            {
                                case 1: safeThis->loadPresetIndex (0); break;
                                case 2: safeThis->savePreset(); break;
                                case 7: safeThis->savePresetAs(); break;
                                case 3: safeThis->loadPreset(); break;
                                case 5: safeThis->exportPreset(); break;
                                case 6: safeThis->chooseSyxFile(); break;
                                case 8: safeThis->resetPresetLevel(); break;
                                case 4:
                                {
                                    const auto directory = safeThis->processorRef.getUserPresetDirectory();
                                    directory.createDirectory();
                                    directory.startAsProcess();
                                    break;
                                }
                                default: break;
                            }
                        });
}

void IlanaSynthAudioProcessorEditor::resetPresetLevel()
{
    if (auto* trim = processorRef.apvts.getParameter ("output_trim"))
    {
        processorRef.beginEdit ("Reset preset level");
        trim->setValueNotifyingHost (trim->getDefaultValue());
        processorRef.endEdit();
        updateMasterTooltip();
    }
}

void IlanaSynthAudioProcessorEditor::showDiceMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("RANDOMISE");
    menu.addItem (1, "Whole patch");
    menu.addItem (2, "Oscillators only");
    menu.addItem (3, "Filters only");
    menu.addItem (4, "Envelopes only");
    menu.addItem (5, "Modulation only");
    menu.addItem (6, "Effects chain only");
    menu.addSeparator();
    menu.addSectionHeader ("MUTATE");
    menu.addItem (10, "Nudge  (small changes)");
    menu.addItem (11, "Shake  (bigger changes)");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&diceButton),
                        [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this)] (int result)
                        {
                            if (safeThis == nullptr || result == 0)
                                return;

                            // Each roll is one named undo step.
                            const juce::String names[] { {}, "Random patch", "Random oscillators", "Random filters",
                                                         "Random envelopes", "Random modulation", "Random effects" };
                            const auto name = result <= 6 ? names[result] : juce::String (result == 10 ? "Nudge" : "Shake");
                            const auto roll = [safeThis, result, name]
                            {
                                if (safeThis == nullptr)
                                    return;

                                auto& self = *safeThis;
                                self.processorRef.performEdit (name, [&self, result]
                                {
                                    if (result == 1)
                                        self.randomize();
                                    else if (result == 6)
                                        self.processorRef.randomizeFxChain();
                                    else if (result == 10)
                                        self.mutate (0.06f);
                                    else if (result == 11)
                                        self.mutate (0.18f);
                                    else
                                        self.randomizeGroup (result);
                                });
                            };

                            // A whole new patch replaces the edited one: ask first.
                            if (result == 1)
                                safeThis->confirmReplacingPatch ("A random patch", "Roll anyway",
                                                                 [roll] (bool confirmed) { if (confirmed) roll(); });
                            else
                                roll();
                        });
}

void IlanaSynthAudioProcessorEditor::showSettingsMenu (bool tuningOnly)
{
    const char* const skinNames[] { "Ember", "Ice", "Acid", "Neon" };
    const float zoomChoices[] { 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };

    juce::PopupMenu skins;

    for (int i = 0; i < IlanaTheme::numPalettes; ++i)
        skins.addItem (100 + i, skinNames[i], true, i == themeIndex);

    juce::PopupMenu sizes;

    for (int i = 0; i < 6; ++i)
        sizes.addItem (200 + i, juce::String (juce::roundToInt (zoomChoices[i] * 100.0f)) + "%",
                       true, juce::approximatelyEqual (uiZoom, zoomChoices[i]));

    // Engine quality and oversampling, also on the scope panel (where they were
    // hard to find).
    const auto read = [this] (const char* id) { return processorRef.apvts.getRawParameterValue (id)->load(); };
    juce::PopupMenu quality;
    const char* const qualityNames[] { "Eco (unison capped at 4)", "Normal", "High" };
    for (int i = 0; i < 3; ++i)
        quality.addItem (600 + i, qualityNames[i], true, juce::roundToInt (read ("quality")) == i);
    juce::PopupMenu oversampling;
    const auto oversampled = read ("oversampling") > 0.5f;
    const auto factor = juce::roundToInt (read ("os_factor"));
    oversampling.addItem (700, "Off", true, ! oversampled);
    oversampling.addItem (701, "2x", true, oversampled && factor == 0);
    oversampling.addItem (702, "4x", true, oversampled && factor == 1);

    // Scala microtuning: the tick shows it is on, with the scale's name.
    juce::PopupMenu tuning;
    const auto& tuningState = processorRef.getTuningState();
    const auto hasScale = tuningState.hasScale();
    const auto tuningOn = hasScale && read ("tuning_on") > 0.5f;
    tuning.addItem (800, hasScale ? "Tuning on: " + tuningState.getDescription() : juce::String ("Tuning on (no scale loaded)"),
                    hasScale, tuningOn);
    tuning.addSeparator();
    tuning.addItem (801, "Load Scala tuning (.scl)...");
    tuning.addItem (802, "Load keyboard mapping (.kbm)...");
    tuning.addItem (803, "Reset to 12-TET", hasScale || read ("tuning_on") > 0.5f);
    tuning.addSeparator();
    // MTS-ESP: shown for information; a master in the session takes over.
    tuning.addItem (804, processorRef.isMtsEspConnected() ? "MTS-ESP: " + processorRef.getMtsEspScaleName() + " (overrides the scale)"
                                                          : juce::String ("MTS-ESP: no master in this session"),
                    false, processorRef.isMtsEspConnected());

    juce::PopupMenu menu;
    // One voice panel: OSC > VOICE (review 10, S10-1); the header's VOICES
    // opens it too.
    menu.addItem (960, "Voice settings (OSC > VOICE)...");
    menu.addSeparator();
    menu.addSubMenu ("Skin", skins);
    menu.addSubMenu ("Interface size", sizes);
    menu.addSubMenu ("Engine quality", quality);
    menu.addSubMenu ("Oversampling", oversampling);
    menu.addSubMenu (tuningOn ? "Tuning: " + tuningState.getDescription() : juce::String ("Tuning"), tuning, true, nullptr, tuningOn);
    menu.addItem (300, "Show keyboard", true, keyboardVisible);
    // The live waveform under the preset name moves all the time beside
    // the most-read text: it can be turned off (review 8, S8-41).
    menu.addItem (320, "Waveform under the preset name", true, headerScope.isVisible());
    // Motion: Full or Less (review 26). Less keeps the meters and the live
    // dots and drops the fades, glides, morphs, trails and pop-up growth.
    menu.addItem (330, "Full motion (animations)", true, IlanaAnim::fullMotion());
   #if ILANA_GPU_UI
    menu.addItem (310, "GPU rendering (OpenGL)", true, openGL != nullptr);
   #endif
    menu.addItem (500, "MPE mode (per-note pitch, pressure and slide)", true,
                  processorRef.apvts.getRawParameterValue ("mpe_mode")->load() > 0.5f);
    menu.addItem (510, "Multi-core voices (several threads, same sound)", true, read ("multi_core") > 0.5f);
    menu.addItem (410, "Ask before replacing an edited patch", true, asksBeforeReplacingEdits());
    menu.addSeparator();
    menu.addItem (400, "Show welcome tour");

    // The header's tuning indicator opens just the tuning items.
    if (tuningOnly)
    {
        menu = juce::PopupMenu();
        menu.addSectionHeader ("TUNING");
        for (juce::PopupMenu::MenuItemIterator items (tuning); items.next();)
            menu.addItem (items.getItem());
    }

    auto* target = tuningOnly ? static_cast<juce::Component*> (&tuningArea) : &settingsButton;
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
                        [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this)] (int result)
                        {
                            if (safeThis == nullptr || result == 0)
                                return;

                            const float zooms[] { 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };

                            if (result >= 100 && result < 200)
                                safeThis->setTheme (result - 100);
                            else if (result >= 200 && result < 300)
                                safeThis->applyUiZoom (zooms[juce::jlimit (0, 5, result - 200)]);
                            else if (result == 300)
                                safeThis->setKeyboardVisible (! safeThis->keyboardVisible);
                            else if (result == 330)
                            {
                                IlanaAnim::fullMotion() = ! IlanaAnim::fullMotion();
                                safeThis->settings->setValue ("fullMotion", IlanaAnim::fullMotion());
                                safeThis->settings->saveIfNeeded();
                                safeThis->repaint();
                            }
                            else if (result == 320)
                            {
                                const auto show = ! safeThis->headerScope.isVisible();
                                safeThis->headerScope.setVisible (show);
                                safeThis->settings->setValue ("headerWaveform", show);
                                safeThis->settings->saveIfNeeded();
                            }
                           #if ILANA_GPU_UI
                            else if (result == 310)
                            {
                                const auto useGpu = safeThis->openGL == nullptr;
                                safeThis->setGpuRendering (useGpu);
                                safeThis->settings->setValue ("gpuRendering", useGpu);
                                safeThis->settings->saveIfNeeded();
                            }
                           #endif
                            else if (result == 510)
                            {
                                if (auto* multiCore = safeThis->processorRef.apvts.getParameter ("multi_core"))
                                {
                                    multiCore->beginChangeGesture();
                                    multiCore->setValueNotifyingHost (multiCore->getValue() > 0.5f ? 0.0f : 1.0f);
                                    multiCore->endChangeGesture();
                                }
                            }
                            else if (result == 500)
                            {
                                if (auto* mpe = safeThis->processorRef.apvts.getParameter ("mpe_mode"))
                                {
                                    mpe->beginChangeGesture();
                                    mpe->setValueNotifyingHost (mpe->getValue() > 0.5f ? 0.0f : 1.0f);
                                    mpe->endChangeGesture();
                                }
                            }
                            else if (result >= 600 && result < 800)
                            {
                                const auto set = [&safeThis] (const char* id, float value)
                                {
                                    if (auto* parameter = safeThis->processorRef.apvts.getParameter (id))
                                    {
                                        parameter->beginChangeGesture();
                                        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
                                        parameter->endChangeGesture();
                                    }
                                };
                                if (result < 700)
                                    set ("quality", (float) (result - 600));
                                else
                                {
                                    set ("oversampling", result == 700 ? 0.0f : 1.0f);
                                    if (result > 700)
                                        set ("os_factor", (float) (result - 701));
                                }
                            }
                            else if (result == 960)
                                safeThis->showVoicePanel();
                            else if (result == 400)
                            {
                                safeThis->tutorial.setVisible (true);
                                safeThis->tutorial.toFront (false);
                            }
                            else if (result == 410)
                            {
                                safeThis->setAsksBeforeReplacingEdits (! safeThis->asksBeforeReplacingEdits());
                            }
                            else if (result == 800)
                            {
                                if (auto* on = safeThis->processorRef.apvts.getParameter ("tuning_on"))
                                {
                                    on->beginChangeGesture();
                                    on->setValueNotifyingHost (on->getValue() > 0.5f ? 0.0f : 1.0f);
                                    on->endChangeGesture();
                                }
                            }
                            else if (result == 801 || result == 802)
                            {
                                const auto mapping = result == 802;
                                safeThis->fileChooser = std::make_unique<juce::FileChooser> (
                                    mapping ? "Load keyboard mapping" : "Load Scala tuning",
                                    juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                    mapping ? "*.kbm" : "*.scl");
                                safeThis->fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                                                        | juce::FileBrowserComponent::canSelectFiles,
                                                                    [safeThis, mapping] (const juce::FileChooser& chooser)
                                                                    {
                                                                        const auto file = chooser.getResult();
                                                                        if (safeThis == nullptr || ! file.existsAsFile())
                                                                            return;
                                                                        juce::String error;
                                                                        const auto text = file.loadFileAsString();
                                                                        const auto loaded = mapping ? safeThis->processorRef.loadTuningMapping (text, error)
                                                                                                    : safeThis->processorRef.loadTuningScale (text, error);
                                                                        if (! loaded)
                                                                            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                                                                    "Could not load " + file.getFileName(), error);
                                                                    });
                            }
                            else if (result == 803)
                                safeThis->processorRef.resetTuning();
                        });
}

void IlanaSynthAudioProcessorEditor::stepPreset (int delta)
{
    createPresetPanel();

    if (const auto next = presetPanel->presetBeside (currentPresetIndex, delta); next >= 0)
    {
        loadPresetIndex (next);
        return;
    }

    // Nothing listed (a search that matches nothing): every preset, in order.
    if (const auto count = processorRef.getNumAllPresets(); count > 0)
        loadPresetIndex ((juce::jmax (0, currentPresetIndex) + count + delta) % count);
}

void IlanaSynthAudioProcessorEditor::createPresetPanel()
{
    if (presetPanel != nullptr)
        return;

    presetPanel = std::make_unique<PresetPanel> (processorRef, settings.get());
    presetPanel->onLoad = [this] (int index, bool closeAfter)
    {
        loadPresetIndex (index, [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this), closeAfter] (bool loaded)
        {
            if (safeThis == nullptr || safeThis->presetPanel == nullptr)
                return;

            if (! loaded)
                safeThis->presetPanel->selectLoadedPreset(); // back to the patch still playing
            else if (closeAfter)
                safeThis->presetPanel->close();
        });
    };
    presetPanel->onFavouriteChanged = [this] { updateHeaderButtons(); };
    presetPanel->onSaveAs = [this] { savePresetAs(); };
    // Rename or edit info: the preset loads (asking over an edited patch),
    // then the save sheet opens on its own file.
    presetPanel->onEditInfo = [this] (int index)
    {
        loadPresetIndex (index, [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this)] (bool loaded)
        {
            if (safeThis == nullptr || ! loaded)
                return;

            const auto own = PresetFiles::loadedUserPreset (safeThis->processorRef);

            if (! own.existsAsFile())
                return;

            if (safeThis->presetPanel != nullptr && safeThis->presetPanel->isOpen() && ! safeThis->presetPanel->isDocked())
                safeThis->presetPanel->close();

            safeThis->saveOverlay.setBounds (safeThis->content.getLocalBounds());
            safeThis->saveOverlay.showForEdit (own);
        });
    };
    presetPanel->onImportSyx = [this] { chooseSyxFile(); };
    presetPanel->setAnchor (&presetDisplay);
    presetPanel->onDockRequest = [this] (bool dock)
    {
        presetDocked = dock;

        if (settings != nullptr)
        {
            settings->setValue ("presetBrowserDocked", presetDocked);
            settings->saveIfNeeded();
        }

        if (dock)
        {
            presetPanel->setVisible (false);
            setPresetDockShown (true);
        }
        else
        {
            setPresetDockShown (false);
            togglePresetPanel();
        }
    };
    presetPanel->onDockedClose = [this] { setPresetDockShown (false); };
    presetPanel->onClosed = [this] { browsingAccepted = false; };

    // Hidden until open() fades it in.
    content.addChildComponent (*presetPanel);
}

// Docked, the browser covers the page area (the tab row, the chips, the
// macros and the keyboard stay): the window keeps its size (review 6; it
// used to grow by a column).
void IlanaSynthAudioProcessorEditor::setPresetDockShown (bool shown)
{
    createPresetPanel();

    if (shown == presetDockShown)
        return;

    presetDockShown = shown;
    browsingAccepted = false;
    presetPanel->setVisible (false);
    presetPanel->setDocked (shown);

    if (shown)
    {
        presetPanel->setBounds (pageArea().reduced (2, 0).withTrimmedBottom (2));
        presetPanel->open();
    }

    if (settings != nullptr)
    {
        settings->setValue ("presetBrowserDockOpen", presetDockShown);
        settings->saveIfNeeded();
    }

    resized();
}

void IlanaSynthAudioProcessorEditor::togglePresetPanel()
{
    createPresetPanel();

    if (presetDocked)
    {
        setPresetDockShown (! presetDockShown);
        return;
    }

    // Drop down under the preset name, kept inside the window.
    {
        // Wide enough for the details card beside the list (S10-5).
        const auto width = 920;
        const auto x = juce::jlimit (10, designWidth - 10 - width, presetDisplay.getX() - 40);
        presetPanel->setBounds (x, presetDisplay.getBottom() + 6, width, 540);
        presetPanel->setScrimArea (content.getLocalBounds());
    }

    if (presetPanel->isOpen())
        presetPanel->close();
    else
        presetPanel->open();
}

// IMPORT .SYX (the browser) and the preset menu's import: each voice of the
// bank becomes a user preset under DX7/<bank>/.
void IlanaSynthAudioProcessorEditor::chooseSyxFile()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Import a DX7 or Dexed bank",
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.syx");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this)] (const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();

                                  if (safeThis != nullptr && file.existsAsFile())
                                      safeThis->importSyxFile (file);
                              });
}

// Imports, then says so in the theme; "Show in browser" opens the browser
// on the new bank.
void IlanaSynthAudioProcessorEditor::importSyxFile (const juce::File& file)
{
    juce::String message;
    const auto count = processorRef.importDx7File (file, message);
    const auto bank = file.getFileNameWithoutExtension();

    if (presetPanel != nullptr)
        presetPanel->refresh();

    ConfirmOverlay::Choices choices;
    choices.confirmText = count > 0 ? "Show in browser" : "OK";
    choices.cancelText = count > 0 ? "Close" : "Cancel";
    choices.offerDontAsk = false;

    confirmOverlay.ask (count > 0 ? "Imported " + bank : juce::String ("Nothing imported"),
                        count > 0 ? juce::String (count) + (count == 1 ? " DX7 voice is" : " DX7 voices are")
                                        + " now your presets, filed by sound with the bank " + bank + " under the browser's DX7 chip."
                                  : message,
                        choices,
                        [safeThis = juce::Component::SafePointer<IlanaSynthAudioProcessorEditor> (this), count, bank] (bool show, bool)
                        {
                            if (safeThis == nullptr || ! show || count == 0)
                                return;

                            safeThis->createPresetPanel();

                            if (! safeThis->presetPanel->isOpen())
                                safeThis->togglePresetPanel();

                            safeThis->presetPanel->showDx7Bank (bank);
                        });
}

bool IlanaSynthAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
        if (path.endsWithIgnoreCase (".syx") || path.endsWithIgnoreCase (".ilanapreset"))
            return true;

    return false;
}

void IlanaSynthAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& path : files)
        if (path.endsWithIgnoreCase (".syx"))
        {
            importSyxFile (juce::File (path));
            return; // one bank at a time: the confirm names it
        }

    // A preset file dropped anywhere loads, as Vital's does (the first one).
    for (const auto& path : files)
        if (path.endsWithIgnoreCase (".ilanapreset"))
        {
            loadPresetFile (juce::File (path));
            return;
        }
}

void IlanaSynthAudioProcessorEditor::randomize()
{
    for (const auto group : { 2, 3, 4, 5 })
        randomizeGroup (group);
}

// Groups: 2 oscillators, 3 filters, 4 envelopes, 5 modulation.
void IlanaSynthAudioProcessorEditor::randomizeGroup (int group)
{
    juce::Random random;

    const auto setValue = [this] (const char* id, float value)
    {
        if (auto* parameter = processorRef.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };

    const auto randomRange = [&random] (float minimum, float maximum)
    {
        return minimum + random.nextFloat() * (maximum - minimum);
    };

    const auto tableCount = TableFactory::getNumFactoryTables();

    if (group == 2)
    {
        setValue ("osc1_on", 1.0f);
        setValue ("sub_on", 1.0f);
        processorRef.setOscillatorMode (1, OscMode::wavetable); // (drops an operator's ratio and OP ENV)
        processorRef.setOscillatorMode (2, OscMode::wavetable);

        if (random.nextFloat() < 0.25f)
        {
            processorRef.setOscillatorMode (0, OscMode::physical);
            setValue ("osc1_excite", (float) random.nextInt (4));
            setValue ("osc1_string_decay", randomRange (0.6f, 0.95f));
            setValue ("osc1_string_damp", randomRange (0.1f, 0.6f));
        }
        else
        {
            processorRef.setOscillatorMode (0, OscMode::wavetable);
        }

        setValue ("osc1_table", (float) random.nextInt (tableCount));
        setValue ("osc1_frame", random.nextFloat());
        setValue ("osc1_unison", (float) (1 + random.nextInt (4)));
        setValue ("osc1_detune", randomRange (5.0f, 30.0f));
        setValue ("osc2_on", random.nextBool() ? 1.0f : 0.0f);
        setValue ("osc2_table", (float) random.nextInt (tableCount));
        setValue ("osc2_frame", random.nextFloat());
        setValue ("osc2_semi", (float) (random.nextBool() ? -12 : 12));
        setValue ("subosc_on", random.nextFloat() < 0.6f ? 1.0f : 0.0f);
        setValue ("subosc_level", randomRange (0.2f, 0.6f));
        setValue ("sub_shape", (float) random.nextInt (3));
        setValue ("fm_amount", random.nextFloat() < 0.6f ? randomRange (0.0f, 0.5f) : 0.0f);
        setValue ("fm_feedback", random.nextFloat() < 0.3f ? randomRange (0.0f, 0.4f) : 0.0f);
        setValue ("ring_mod", random.nextFloat() < 0.3f ? randomRange (0.0f, 0.8f) : 0.0f);
        setValue ("hard_sync", random.nextFloat() < 0.25f ? 1.0f : 0.0f);
        setValue ("drift", randomRange (0.0f, 0.5f));
        setValue ("osc1_chord", (float) random.nextInt (7));
        setValue ("voice_spread", randomRange (0.0f, 0.5f));
        setValue ("unison_random", randomRange (0.0f, 1.0f));
    }
    else if (group == 3)
    {
        // Low pass, band pass, high pass and both ladders; notch is rarely useful at random.
        const int types[] { FilterType::LowPass, FilterType::BandPass, FilterType::HighPass,
                            FilterType::LadderLow, FilterType::LadderLow, FilterType::LadderHigh };
        setValue ("f1_type", (float) types[random.nextInt (6)]);
        setValue ("f1_cutoff", randomRange (200.0f, 8000.0f));
        setValue ("f1_reso", randomRange (0.0f, 0.6f));
        setValue ("res_on", random.nextFloat() < 0.25f ? 1.0f : 0.0f);
    }
    else if (group == 4)
    {
        setValue ("fe_decay", randomRange (0.05f, 1.5f));
        setValue ("amp_attack", random.nextFloat() < 0.3f ? randomRange (0.05f, 0.8f) : 0.005f);
        setValue ("amp_release", randomRange (0.05f, 2.0f));
    }
    else if (group == 5)
    {
        setValue ("lfo1_rate", randomRange (0.2f, 8.0f));

        if (random.nextFloat() < 0.6f)
        {
            setValue ("mod1_src", 1.0f);
            setValue ("mod1_dst", random.nextFloat() < 0.5f ? 9.0f : 2.0f);
            setValue ("mod1_amt", randomRange (0.15f, 0.6f));
        }
        else
        {
            setValue ("mod1_src", 0.0f);
            setValue ("mod1_dst", 0.0f);
            setValue ("mod1_amt", 0.0f);
        }
    }
}

// Nudges every continuous sound parameter by a small random amount, keeping
// the patch's character. Routing, switches, levels at the end of the chain
// and the effect rack layout are left alone.
void IlanaSynthAudioProcessorEditor::mutate (float amount)
{
    juce::Random random;

    for (auto* parameter : processorRef.getParameters())
    {
        auto* floatParameter = dynamic_cast<juce::AudioParameterFloat*> (parameter);

        if (floatParameter == nullptr)
            continue;

        const auto id = floatParameter->getParameterID();

        if (id == "master" || id.startsWith ("macro") || id.startsWith ("mod") || id.startsWith ("fx_slot")
            || id == "bend_range" || id == "master_clip_gain" || id.contains ("_step"))
            continue;

        // Only touch what's already doing something: a parameter sitting at
        // its default is usually off on purpose.
        const auto current = parameter->getValue();

        if (std::abs (current - parameter->getDefaultValue()) < 1.0e-4f && random.nextFloat() < 0.7f)
            continue;

        const auto offset = ((random.nextFloat() + random.nextFloat() + random.nextFloat()) / 1.5f - 1.0f) * amount;
        floatParameter->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, current + offset));
    }
}

// Esc closes the topmost thing open over the page, one per press: the save
// panel, the tour, the drop-down browser, a held mod card, the remap editor,
// the scope. False when nothing was open.
bool IlanaSynthAudioProcessorEditor::closeTopPopup()
{
    if (saveOverlay.isShowing())
    {
        saveOverlay.cancel();
        return true;
    }

    // Esc closes the wavetable editor wherever the focus is in the window
    // (a text field there would otherwise keep the key).
    if (wavetableEditor != nullptr)
    {
        closeWavetableEditor();
        return true;
    }

    if (zoneEditor != nullptr)
    {
        closeZoneEditor();
        return true;
    }

    if (sampleBrowser != nullptr)
    {
        closeSampleBrowser();
        return true;
    }

    if (tutorial.isVisible())
    {
        tutorial.dismiss();
        return true;
    }

    if (presetPanel != nullptr && presetPanel->isOpen() && ! presetPanel->isDocked())
    {
        presetPanel->close();
        return true;
    }

    if (getSourcePopoverSource() != 0)
    {
        closeSourcePopover();
        return true;
    }

    if (presetDockShown)
    {
        setPresetDockShown (false);
        return true;
    }

    {
        std::vector<KnobControl*> knobs;
        std::function<void (juce::Component&)> collect = [&] (juce::Component& parent)
        {
            for (auto* child : parent.getChildren())
            {
                if (auto* knob = dynamic_cast<KnobControl*> (child))
                    knobs.push_back (knob);

                collect (*child);
            }
        };
        collect (content);
        auto closed = false;

        for (auto* knob : knobs)
        {
            if (knob->isModCardOpen())
            {
                knob->closeModCard();
                closed = true;
            }
        }

        if (closed)
            return true;

        std::function<RemapEditor* (juce::Component&)> findRemap = [&] (juce::Component& parent) -> RemapEditor*
        {
            for (auto* child : parent.getChildren())
            {
                if (auto* remap = dynamic_cast<RemapEditor*> (child); remap != nullptr && remap->isShowing())
                    return remap;

                if (auto* found = findRemap (*child))
                    return found;
            }

            return nullptr;
        };

        if (auto* remap = findRemap (content); remap != nullptr && remap->close())
            return true;
    }

    if (isScopeOpen())
    {
        setScopeOpen (false);
        return true;
    }

    return false;
}

bool IlanaSynthAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    if (confirmOverlay.isAsking())
        return confirmOverlay.keyPressed (key);

    if (saveOverlay.isShowing())
        return saveOverlay.keyPressed (key);

    if (key.getKeyCode() == juce::KeyPress::escapeKey)
        return closeTopPopup();

    const auto modifiers = key.getModifiers();

    // The computer keyboard plays notes (G-06): A to K are the white keys of
    // an octave with W E T Y U O P for the black ones, Z and X shift the
    // octave. Only here, in the editor's own key handler, so a host that
    // keeps its keys never sees a conflict; never while a text field has focus.
    if (keyboard != nullptr && wavetableEditor == nullptr && zoneEditor == nullptr && sampleBrowser == nullptr && ! modifiers.isCommandDown() && ! modifiers.isCtrlDown()
        && ! modifiers.isAltDown()
        && dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) == nullptr)
    {
        const auto character = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());

        if (character == 'z' || character == 'x')
        {
            noteKeyOctave = juce::jlimit (-3, 3, noteKeyOctave + (character == 'x' ? 1 : -1));
            return true;
        }

        if (const auto semitone = juce::String ("awsedftgyhujkolp").indexOfChar (character); semitone >= 0 && character != 0)
        {
            const auto code = key.getKeyCode();

            for (const auto& held : heldNoteKeys)
                if (held.first == code)
                    return true; // auto-repeat

            const auto note = 60 + 12 * noteKeyOctave + semitone;
            heldNoteKeys.emplace_back (code, note);
            keyboard->keyNoteOn (note);
            return true;
        }
    }

    if (modifiers.isCommandDown() || modifiers.isCtrlDown())
    {
        const auto code = juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) key.getKeyCode());

        if (code == 'Z')
        {
            undoOrRedo (modifiers.isShiftDown());
            return true;
        }

        if (code == 'Y')
        {
            undoOrRedo (true);
            return true;
        }

        if (code == 'S')
        {
            modifiers.isShiftDown() ? savePresetAs() : savePreset();
            return true;
        }

        // Previous / next preset, asking first over an edited patch.
        if (key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey)
        {
            (key.getKeyCode() == juce::KeyPress::leftKey ? prevButton : nextButton).onClick();
            return true;
        }

        // Ctrl / Cmd + Shift + 1-3 pick the page inside the tab (PLAY:
        // overview, vector; OSC: oscillators, physical; MOD: env / lfo,
        // matrix), review 9, S9-21. A shifted digit arrives as the digit
        // key or as the symbol on it.
        if (modifiers.isShiftDown() && ! modifiers.isAltDown())
        {
            const auto symbol = juce::String ("!@#$%^&*(").indexOfChar (key.getTextCharacter());
            const auto digit = key.getKeyCode() >= '1' && key.getKeyCode() <= '9' ? key.getKeyCode() - '1' : symbol;

            if (auto* section = currentSection(); section != nullptr && digit >= 0 && digit < section->getNumPages())
            {
                showPage (section->getPageId (digit));
                return true;
            }
        }

        // Ctrl / Cmd + 1-8 pick a tab (bare digits are left to the host:
        // DAWs play notes or run actions with them).
        if (const auto digit = key.getKeyCode(); digit >= '1' && digit <= '9' && ! modifiers.isAltDown() && ! modifiers.isShiftDown())
        {
            const auto index = digit - '1';

            if (index < visibleTabCount)
            {
                tabs.setCurrentTabIndex (index);
                return true;
            }
        }

        return false;
    }

    return false;
}

// Lets go of the computer-keyboard notes whose key is up (or all of them).
void IlanaSynthAudioProcessorEditor::releaseNoteKeys (bool onlyThoseUp)
{
    for (auto it = heldNoteKeys.begin(); it != heldNoteKeys.end();)
    {
        const auto code = it->first;
        const auto down = juce::KeyPress::isKeyCurrentlyDown (code)
                          || juce::KeyPress::isKeyCurrentlyDown ((int) juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) code))
                          || juce::KeyPress::isKeyCurrentlyDown ((int) juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) code));

        if (onlyThoseUp && down)
        {
            ++it;
            continue;
        }

        if (keyboard != nullptr)
            keyboard->keyNoteOff (it->second);

        it = heldNoteKeys.erase (it);
    }
}

bool IlanaSynthAudioProcessorEditor::keyStateChanged (bool isKeyDown)
{
    if (! isKeyDown && ! heldNoteKeys.empty())
        releaseNoteKeys (true);

    return false;
}

void IlanaSynthAudioProcessorEditor::focusOfChildComponentChanged (FocusChangeType)
{
    // A note held while the window loses the keys would never see its key-up.
    if (! heldNoteKeys.empty() && ! hasKeyboardFocus (true))
        releaseNoteKeys (false);
}

// The preset's fingerprint when it was loaded or saved, kept in the
// processor with the preset's name so a reopened editor still shows EDITED.
void IlanaSynthAudioProcessorEditor::rememberLoadedFingerprint()
{
    loadedFingerprint = parameterFingerprint();
    processorRef.loadedPresetFingerprint = { processorRef.getCurrentPresetName(), loadedFingerprint };
    processorRef.loadedPresetValues.clear();
    for (auto* parameter : processorRef.getParameters())
        processorRef.loadedPresetValues.push_back (parameter->getValue());
}

void IlanaSynthAudioProcessorEditor::adoptLoadedFingerprint()
{
    const auto& kept = processorRef.loadedPresetFingerprint;

    if (kept.has_value() && kept->first == processorRef.getCurrentPresetName())
        loadedFingerprint = kept->second;
    else
        rememberLoadedFingerprint();
}
