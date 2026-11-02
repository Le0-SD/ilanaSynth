// The undo note: after a click that throws something away (Remove, Init,
// Clear, a random roll, a load), a small note at the bottom of the page names
// the step with an UNDO key, for a few seconds (Impeccable re-score, H9:
// recover from a slip without knowing the shortcut). ilana picked it
// 2026-10-28 with the shortcut sheet.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "IlanaLookAndFeel.h"

class UndoToast : public juce::Component,
                  private juce::Timer
{
public:
    static constexpr int height = 34;
    static constexpr int shownMs = 4500;

    UndoToast()
    {
        undoButton.setButtonText ("UNDO");
       #if JUCE_MAC
        undoButton.setTooltip ("Undo this step (Cmd+Z)");
       #else
        undoButton.setTooltip ("Undo this step (Ctrl+Z)");
       #endif
        undoButton.onClick = [this]
        {
            hide();

            if (const auto run = action; run != nullptr)
                run();
        };
        addAndMakeVisible (undoButton);
        setInterceptsMouseClicks (false, true);
    }

    std::function<void()> onUndo;

    // Whether a step's name is one the note is for: a click that throws
    // something away. (A knob turn, an added route or a move is not.)
    static bool isDestructive (const juce::String& step)
    {
        for (const auto* prefix : { "Remove", "Random", "Dice", "Init ", "Load ", "Clear", "Shake" })
            if (step.startsWith (prefix))
                return true;

        return false;
    }

    void show (const juce::String& step)
    {
        undoButton.setButtonText ("UNDO");
       #if JUCE_MAC
        undoButton.setTooltip ("Undo this step (Cmd+Z)");
       #else
        undoButton.setTooltip ("Undo this step (Ctrl+Z)");
       #endif
        action = [this] { if (onUndo != nullptr) onUndo(); };
        showText (step, shownMs);
    }

    // After an undo or redo (usability flair, pass 1): what it changed
    // ("Undid F1 RESO 60 % to 25 %") for a moment, with a key that redoes
    // it, or SHOW when the knob is on another page.
    void showNote (const juce::String& note, const juce::String& buttonText, std::function<void()> buttonAction)
    {
        undoButton.setButtonText (buttonText);
        undoButton.setTooltip (buttonText == "SHOW" ? "Show the page this knob is on"
                                                    : buttonText == "REDO" ? "Redo it" : "Undo it again");
        action = std::move (buttonAction);
        showText (note, noteMs);
    }

    static constexpr int noteMs = 2500;

private:
    void showText (const juce::String& step, int milliseconds)
    {
        text = step;
        undoButton.setColour (juce::TextButton::textColourOffId, IlanaTheme::accent()); // (the theme's, now)
        setSize (preferredWidth(), height);
        resized();
        setVisible (true);
        toFront (false);
        repaint();
        startTimer (milliseconds);
    }

public:

    void hide()
    {
        stopTimer();
        setVisible (false);
    }

    const juce::String& getText() const { return text; }
    juce::Button& getUndoButton() { return undoButton; }

    int preferredWidth() const
    {
        const auto textWidth = juce::GlyphArrangement::getStringWidthInt (IlanaTheme::font (IlanaTheme::TextSize::body), text);
        return juce::jlimit (200, 560, 16 + textWidth + 16 + buttonWidth + 5);
    }

    void resized() override
    {
        undoButton.setBounds (getLocalBounds().removeFromRight (buttonWidth + 5).reduced (0, 5).withTrimmedRight (5));
    }

    void paint (juce::Graphics& g) override
    {
        const auto box = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (IlanaTheme::Ui::raised);
        g.fillRoundedRectangle (box, 8.0f);
        g.setColour (IlanaTheme::Ui::line.brighter (0.2f));
        g.drawRoundedRectangle (box, 8.0f, 1.0f);

        g.setColour (IlanaTheme::Ui::text);
        g.setFont (IlanaTheme::font (IlanaTheme::TextSize::body));
        g.drawText (text, getLocalBounds().withTrimmedLeft (16).withTrimmedRight (buttonWidth + 16), juce::Justification::centredLeft, true);
    }

private:
    static constexpr int buttonWidth = 72;

    void timerCallback() override { hide(); }

    juce::TextButton undoButton;
    juce::String text;
    std::function<void()> action;
};
