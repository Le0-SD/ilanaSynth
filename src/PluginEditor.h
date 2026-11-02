#pragma once

#include <map>
#include <set>
#include <juce_audio_processors/juce_audio_processors.h>

// macOS and Linux draw the UI through OpenGL (CMake links juce_opengl there);
// Windows already draws with Direct2D.
#if JUCE_MODULE_AVAILABLE_juce_opengl && ! JUCE_WINDOWS
 #include <juce_opengl/juce_opengl.h>
 #define ILANA_GPU_UI 1
#else
 #define ILANA_GPU_UI 0
#endif

#include <functional>
#include <memory>
#include <vector>

#include "PluginProcessor.h"
#include "gui/HeaderWidgets.h"
#include "gui/ConfirmOverlay.h"
#include "gui/IlanaLookAndFeel.h"
#include "gui/InfoStrip.h"
#include "gui/KeyboardStrip.h"
#include "gui/LogoComponent.h"
#include "gui/MacroStrip.h"
#include "gui/ModHoverPopup.h"
#include "gui/ModSourceChip.h"
#include "gui/OutputMeter.h"
#include "gui/OutputView.h"
#include "gui/PresetPanel.h"
#include "gui/SavePresetOverlay.h"
#include "gui/SectionPage.h"
#include "gui/TutorialOverlay.h"
#include "gui/UndoToast.h"

class WavetableEditor;
class ZoneMapEditor;
class SampleBrowser;

class IlanaSynthAudioProcessorEditor : public juce::AudioProcessorEditor,
                                       public juce::DragAndDropContainer,
                                       public juce::FileDragAndDropTarget,
                                       private juce::ChangeListener,
                                       private juce::Timer
{
public:
    explicit IlanaSynthAudioProcessorEditor (IlanaSynthAudioProcessor&);
    ~IlanaSynthAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    bool keyStateChanged (bool isKeyDown) override;
    void focusOfChildComponentChanged (FocusChangeType) override;

    // M7.4: opens the wavetable editor on patch table slot (0-15) over the
    // whole window.
    void openWavetableEditor (int slot, juce::Colour colour);
    void closeWavetableEditor();
    // Roadmap step 36: an oscillator's zone map, over the page like the
    // wavetable editor.
    void openZoneEditor (int oscIndex, juce::Colour colour);
    void closeZoneEditor();
    ZoneMapEditor* getZoneEditor() const { return zoneEditor.get(); }
    // Roadmap step 42: the sample browser for one oscillator, over the page like
    // the zone map. loadFile loads a chosen file into the oscillator (default:
    // loadUserSample with fromUser = true, then the oscillator turns to sample mode).
    void openSampleBrowser (int oscIndex, std::function<bool (const juce::File&)> loadFile = {});
    void closeSampleBrowser();
    SampleBrowser* getSampleBrowser() const { return sampleBrowser.get(); }
    WavetableEditor* getWavetableEditor() const { return wavetableEditor.get(); }

    // Pages by id ("MAIN", "VECTOR", "OSC", "FILTER", "ENV/LFO",
    // "MATRIX", "FM", "ARP/SEQ", "FX", "INPUT"): each lives in one of
    // the seven top-level tabs. "SCOPE" opens the scope panel.
    void showPage (const juce::String& id);
    // PLAY's card views, for the tests and snapshots: FILTER 1 or 2, STRINGS or KEYS, BODY or BOARD (-1 keeps one).
    std::function<void (int filter, int strings, int body)> showPlayViews;
    // The voice settings are OSC > VOICE; the header's VOICES and the settings menu jump there.
    std::function<void()> showVoicePanel;
    // OSC's card for an oscillator, in this editor's coordinates (the UI tests).
    std::function<juce::Rectangle<int> (int)> getOscCardBounds;
    // An oscillator's Operator EG: the FM page with that operator chosen.
    void showOperatorEnvelope (int op);
    juce::String getCurrentPageId() const;
    juce::StringArray getPageIds() const;
    juce::Component* getCurrentPage() const;
    void setScopeOpen (bool shouldBeOpen);
    bool isScopeOpen() const;
    // The scope in its own window beside the synth (it covers no page).
    void setScopeWindowOpen (bool shouldBeOpen);
    bool isScopeWindowOpen() const;

    // UI review 4 (S1): EDITED covers parameters and the patch's other data
    // (drawn curves, remaps, clips); a load or random patch that would
    // replace an edited patch asks first, unless the user ticked "Don't ask
    // again" (settings file, also in the settings menu).
    bool isPatchEdited() const;
    bool asksBeforeReplacingEdits() const;
    void setAsksBeforeReplacingEdits (bool shouldAsk);

    // UI review 4 (V3): SAVE (Ctrl+S) writes the loaded user preset in
    // place; a factory or never-saved patch goes to SAVE AS (Ctrl+Shift+S),
    // the themed panel that asks before overwriting.
    void savePreset();
    void savePresetAs();
    SavePresetOverlay& getSaveOverlay() { return saveOverlay; }
    // The header arrows: the preset before (-1) or after (1) in the browser's list.
    void stepPreset (int delta);

    // Review 6: the header's EDITED badge as drawn (the confirm reads the
    // same isPatchEdited, and refreshes the badge before it asks); the
    // DX7 bank import (the browser's IMPORT .SYX); the docked browser.
    bool isEditedBadgeShown() const { return presetDisplay.isShowingModified(); }
    void importSyxFile (const juce::File& file);
    // A .syx bank dropped anywhere on the window imports like IMPORT .SYX, and
    // an .ilanapreset loads (the oscillators' sample drop zones take their own
    // files first).
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    bool isPresetDockOpen() const { return presetDockShown; }
    InfoStrip& getHoverLine() { return infoStrip; }
    // The header's VOICES count as drawn ("3/32") and its tuning text ("" in
    // 12-TET), for the tests.
    juce::String getVoicesText() const;
    juce::String getTuningIndicatorText() const { return tuningArea.isVisible() ? tuningText : juce::String(); }
    // The preset's own level (output_trim, review 7): as MASTER's hover
    // reads it, and the preset menu's reset; MASTER's hover text.
    juce::String presetLevelText() const;
    void resetPresetLevel();
    juce::String getMasterTooltip()
    {
        updateMasterTooltip();
        return masterKnob != nullptr ? masterKnob->getKnob().getTooltip() : juce::String();
    }

    // GPU drawing on macOS and Linux (settings menu > GPU rendering, saved as
    // "gpuRendering", on by default; ILANA_NO_GPU=1 turns it off for a run).
    // Windows always draws with Direct2D, so these do nothing there.
    void setGpuRendering (bool shouldUseGpu);
    bool isGpuRendering() const;

private:
    struct Content : public juce::Component
    {
        std::function<void (juce::Graphics&)> onPaint;

        void paint (juce::Graphics& g) override
        {
            if (onPaint != nullptr)
                onPaint (g);
        }
    };

    void paintHeader (juce::Graphics& g);
    void presetSaved (bool inPlace);
    bool closeTopPopup();
    void exportPreset();
    void loadPreset();
    void togglePresetPanel();
    // The browser docked over the page area (the window keeps its size).
    void createPresetPanel();
    void loadPresetFile (const juce::File& file);
    void setPresetDockShown (bool shown);
    bool isPresetDockShown() const { return presetDockShown; }
    juce::Rectangle<int> pageArea() const;
    void chooseSyxFile();
    int currentDesignWidth() const { return designWidth; }
    void applyAspectAndLimits();
    void showPresetMenu();
    void showDiceMenu();
    void showSettingsMenu (bool tuningOnly = false);

    void randomize();
    void randomizeGroup (int group);
    void mutate (float amount);
    // Asks first when the patch is edited (see isPatchEdited).
    void loadPresetIndex (int index, std::function<void (bool loaded)> then = {});
    void confirmReplacingPatch (const juce::String& replacement, const juce::String& confirmText,
                                std::function<void (bool confirmed)> then);
    void undoOrRedo (bool redo);
    void showUndoResult (const std::vector<float>& before, const juce::String& step, bool redo);
    juce::String undoToastKeyNow() const;
    void updateUndoToast();
    void layoutUndoToast();
    void updateHeaderButtons();
    void updateUndoButtons();
    void showHistoryMenu();
    bool isFavourite (const juce::String& presetName) const;
    void toggleFavourite();
    void toggleAB();
    void setTheme (int newThemeIndex);
    void setKeyboardVisible (bool shouldBeVisible);
    juce::int64 parameterFingerprint() const;
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void timerCallback() override;
    void mouseEnter (const juce::MouseEvent&) override;
    // Copies the patch's colour coding to what the pages paint with and
    // tells them (when it changed, or always with `force`).
    void syncSectionColours (bool force);
    int shownSectionColourVersion = -1;
    void startTabTransition (juce::Component* page = nullptr);
    void layoutTabRow();
    SectionPage* currentSection() const;
    void applyDisplayScale();
    void applyUiZoom (float newZoom);
    float displayScale() const;
    float hostScaleFactor() const;

    // The hover line's own strip, between the pages and the source chips.
    static constexpr int designWidth = 1060;
    static constexpr int designHeight = 720;
    // The KEYBOARD strip sits below the 720 px design: showing it grows the
    // window, so the pages above keep their size and layout.
    static constexpr int keyboardHeight = 32;
    int totalDesignHeight() const { return designHeight + (keyboardVisible ? keyboardHeight : 0); }
    static constexpr const char* appVersion = "1.4 BETA";

    IlanaSynthAudioProcessor& processorRef;
    std::array<bool, (size_t) Mod::Source::Count> usedModSources {};
    std::vector<bool> chipSecondOutputs; // the LFO chips showing a "B" (they lay out wider)
    // One look shared by every open editor, and made the process default
    // while any is open, so menus, dialogs and file browsers shown on the
    // desktop draw in it too.
    DefaultLookAndFeelScope defaultLook;
    IlanaLookAndFeel& lookAndFeel { defaultLook.get() };
    juce::TooltipWindow tooltipWindow { this, 900 }; // (see-through: its card has round corners)
    Content content;
    LogoComponent logo;
    InfoStrip infoStrip;
    TutorialOverlay tutorial;
    UndoToast undoToast;
    // The main tabs' route badges (usability flair, pass 1): while a mod
    // source is hovered or pinned, each tab counts the knobs it drives
    // there. Polled about 15 times a second; recounted only on a change.
    struct RouteBadgeWatch : juce::Timer
    {
        std::function<void()> tick;
        void timerCallback() override { tick(); }
    } routeBadgeWatch;
    int routeBadgeSource = 0;
    juce::uint64 routeBadgeKey = 0;
    void updateRouteBadges();
    // The tabs' edited-since-load dots and, while A|B is hovered, the blue
    // marks on what differs from the other slot (usability flair, pass 2).
    juce::int64 editedDotsKey = -1;
    juce::uint64 abMarksKey = 0;
    juce::uint64 editedSetKey = 1;
    void updateEditedAndAbMarks();
    void updateAbMarks();
    // Hovering something greyed out because its card is switched off: that
    // card's ON switch glows (usability flair, pass 2).
    juce::Component::SafePointer<juce::Component> offGlowUnder;
    juce::Component::SafePointer<ToggleControl> offGlowSwitch;
    void updateOffSwitchGlow();
    std::map<juce::String, int> sectionKnobCounts (const std::set<juce::String>& parameterIds, bool markAb);
    juce::String undoToastKey;
    bool undoToastSkip = false;
    ConfirmOverlay confirmOverlay;
    SavePresetOverlay saveOverlay { processorRef };

    // The tabs span the whole width, so the pages start at x = 0 and keep their
    // own 14 px gutter (the mockup's content rectangle); the tab bar's row of
    // buttons is inset 14 px.
    struct ShellTabs : public juce::TabbedComponent
    {
        ShellTabs() : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop) {}

        void resized() override
        {
            juce::TabbedComponent::resized();
            getTabbedButtonBar().setBounds (14, 0, getWidth() - 28, getTabBarDepth());
        }
    };
    ShellTabs tabs;
    std::vector<SectionPage*> sections; // owned by tabs
    std::function<void()> showPlayBody;
    // The dock's STEREO tag under VOICES and the pop-up it opens (SPREAD, PHASE, DRIFT, LOOSE).
    std::unique_ptr<juce::Button> stereoButton;
    std::unique_ptr<juce::Component> stereoPopover;
    std::unique_ptr<IlanaAnim::PopGrow> stereoGrow; // (grows from STEREO, folds back into it)
    void setStereoOpen (bool open);
    juce::Rectangle<int> stereoPopoverBounds() const;
    int visibleTabCount = 0; // the tabs with a button in the row; the rest (OSC, FILTER, MOD) sit behind it
    std::unique_ptr<juce::Component> scopePanel;
    std::unique_ptr<juce::Component> scopeWindow; // the scope popped out (WINDOW)
    GhostButton scopeButton { "SCOPE" };

    PresetDisplay presetDisplay;
    IconButton prevButton { "prev", IlanaIcons::Icon::ChevronLeft, "Previous preset" };
    IconButton nextButton { "next", IlanaIcons::Icon::ChevronRight, "Next preset" };
    IconButton favButton { "fav", IlanaIcons::Icon::Star, "Favourite\nMark this preset as a favourite." };
    IconButton saveButton { "save", IlanaIcons::Icon::Save, "Save preset\nSave the current sound as a user preset." };
    IconButton moreButton { "more", IlanaIcons::Icon::More, "Preset options\nInit, load from file, open the preset folder." };
    IconButton undoButton { "undo", IlanaIcons::Icon::Undo, "Undo  (Ctrl+Z)" };
    IconButton redoButton { "redo", IlanaIcons::Icon::Redo, "Redo  (Ctrl+Shift+Z)" };
    IconButton historyButton { "history", IlanaIcons::Icon::History, "History\nJump back to any earlier change." };
    ABButton abButton;
    IconButton diceButton { "dice", IlanaIcons::Icon::Dice, "Randomise\nRoll a new patch, or randomise one part of it." };
    IconButton settingsButton { "settings", IlanaIcons::Icon::Gear, "Settings\nVoice mode, voices and pitch-bend range, skin, interface size, keyboard and the welcome tour." };
    GhostButton keysButton { "KEYBOARD" };
    GhostButton helpButton { "?" };

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<WavetableEditor> wavetableEditor;
    std::unique_ptr<ZoneMapEditor> zoneEditor;
    std::unique_ptr<SampleBrowser> sampleBrowser;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<PresetPanel> presetPanel;
    ModHoverPopup modHoverPopup { processorRef };
    // The header's live waveform strip (click: the scope).
    OutputView headerScope { processorRef };
    // VOICES in the status line: a click opens the voice settings.
    struct ClickArea : public juce::Component, public juce::SettableTooltipClient
    {
        std::function<void()> onClick;
        // What the area draws (the tab bar's read-outs, the tuning pill).
        std::function<void (juce::Graphics&, juce::Rectangle<int>, bool hover)> onPaint;
        void paint (juce::Graphics& g) override
        {
            if (onPaint != nullptr)
                onPaint (g, getLocalBounds(), isMouseOver());
        }
        void mouseEnter (const juce::MouseEvent&) override { if (onClick != nullptr) repaint(); }
        void mouseExit (const juce::MouseEvent&) override { if (onClick != nullptr) repaint(); }
        void mouseUp (const juce::MouseEvent& event) override
        {
            if (onClick != nullptr && getLocalBounds().contains (event.getPosition()))
                onClick();
        }
    };
    ClickArea voicesArea;
    juce::String readoutSignature; // (what the BPM / CPU / VOICES readouts last said)
    // TUNING in the status line while a Scala scale or MTS-ESP plays (a
    // click opens the tuning menu).
    ClickArea tuningArea;
    juce::String tuningText;
    void updateTuningIndicator();
    // After "Save and load": the load waiting for SAVE AS to finish.
    std::function<void()> afterSave;
    bool presetDocked = false;     // the user's choice: dock rather than drop down
    bool presetDockShown = false;  // the docked browser is open
    // "Load anyway" was chosen once while browsing: the rest of that
    // browsing session loads without asking (review 7, S7-32).
    bool browsingAccepted = false;
    void updateMasterTooltip();
    juce::String masterBaseTooltip, shownPresetLevel;
    // The shell (shell mockup): a 52 px header, a 34 px tab bar and a 78 px
    // dock; the pages get what is between (86 px from the top, 78 from the
    // bottom).
    static constexpr int headerHeight = 52, tabBarHeight = 34, dockHeight = 78;
    // The tab bar's read-outs, right-aligned: tempo, VOICES (a button: it
    // opens the voice settings) and CPU.
    ClickArea bpmArea, cpuArea;
    // The dock's place, and where its source groups part (x of the thin rules).
    juce::Rectangle<int> dockBounds, chipRowBounds;
    std::array<int, 2> chipSeparatorX {};
    int statusStartX = 0;

    std::vector<std::unique_ptr<ModSourceChip>> chips;
    // Every LFO and envelope has a chip, shown while that module is in the
    // pool (or the matrix uses it). Parallel to chips; kind -1 for the
    // performance sources, which always show; -2 the Operator Env's (while
    // an oscillator plays it), -3 the vector's (while it is on), -4 the
    // patch MSEG (while routed).
    std::vector<std::pair<int, int>> chipReveal;
    std::vector<bool> chipWanted;
    // Wanted, but folded into its region's "+N" chip because it is full.
    std::vector<bool> chipFolded;
    // Each chip's group (0 LFOs, 1 envelopes, 2 the rest).
    std::vector<int> chipGroup;
    // Each region's "+N": the chips it folds away, in a tray.
    std::array<std::unique_ptr<ModSourceGroupChip>, 3> groupChips;
    static constexpr int chipPickerWidth = 34;
    static std::array<juce::Rectangle<float>, 3> chipRegions (juce::Rectangle<int> row);
    ModSourceTray chipTray;
    std::unique_ptr<ModSourceChip> makeSourceChip (int source);
    void showSourceMenu (juce::Component& target, int source);
    // "+": a picker for the LFOs and envelopes not in the pool yet.
    GhostButton moreChipsButton;
    void updateChipVisibility();
    void layoutChips (juce::Rectangle<int> row);
    void showChipPicker();
    // A chip's click: its source's editor floating over the page (SourcePopover).
    std::unique_ptr<juce::Component> sourcePopover;
    std::unique_ptr<IlanaAnim::PopGrow> sourceGrow; // (grows from its chip or EDIT button)
    juce::Rectangle<int> sourcePopoverAnchor;
    void closeSourcePopover();

public:
    void toggleSourcePopover (int source, juce::Component& chip);
    // The source the pop-out editor shows, or 0 while it is closed (the UI test).
    int getSourcePopoverSource() const;
    juce::Component* getSourcePopover() { return sourcePopover.get(); }

private:

public:
    // Adds chip chipIndex's LFO or envelope to the pool (the "+" picker).
    void addPoolSource (int chipIndex);

private:
    std::function<void (int)> editOperator;
    std::unique_ptr<KeyboardStrip> keyboard;
    // Computer-keyboard notes (G-06): the key codes held and the note each
    // started, and the octave Z / X set (C3 = 60 at 0).
    std::vector<std::pair<int, int>> heldNoteKeys;
    int noteKeyOctave = 0;
    void releaseNoteKeys (bool onlyThoseUp);
    // All eight macros, in two pages of four (ilana 2026-10-22): the pager
    // shows one page and slides to the other from its 1 / 2 switch.
    std::vector<std::unique_ptr<StripKnob>> macroKnobs;
    MacroPager macroPager;
    MacroPageSwitch macroPageSwitch;
    void updateMacroStrip();
    // VOICE in the dock, beside MASTER (ilana 2026-10-22, as Serum and Vital):
    // the voice mode, voices, pitch-bend range, glide and legato.
    std::unique_ptr<ComboControl> voiceModeMenu;
    std::unique_ptr<StripKnob> voicesKnob, bendKnob, glideKnob;
    std::unique_ptr<ToggleControl> legatoToggle;
    std::array<int, 2> dockDividerX {};
    juce::Rectangle<int> macroRowBounds;
    std::unique_ptr<StripKnob> masterKnob;
    std::unique_ptr<OutputMeter> outputMeter;
    // Where the header's action groups (file, edit, tools) part, in header x.
    std::array<int, 2> headerSeparatorX {};
    bool keyboardVisible = false;
    juce::int64 loadedFingerprint = 0;
    void rememberLoadedFingerprint();
    void adoptLoadedFingerprint();

    juce::ValueTree slotA, slotB;
    bool showingA = true;
    int currentPresetIndex = -1;
    juce::String shownPresetName;
    int themeIndex = 0;
    juce::String shownCategory, presetDisplayName;

    // Display-rate animation: the page transition and the preset flash.
    struct Animator : IlanaAnim::FrameTimer
    {
        std::function<void()> onFrame;
        void timerCallback() override { onFrame(); }
    };
    Animator animator;
    std::unique_ptr<IlanaAnim::FrameClock::Source> frameSource;
    void animate();

    juce::Component* transitionPage = nullptr;
    double transitionStart = 0.0;
    float presetLoadFlash = 0.0f;
    bool displayScaleApplied = false;
    void* previousDpiContext = nullptr;
    float uiZoom = 1.0f;
    bool zoomNeedsSaving = false;

   #if ILANA_GPU_UI
    std::unique_ptr<juce::OpenGLContext> openGL;
   #endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IlanaSynthAudioProcessorEditor)
};
