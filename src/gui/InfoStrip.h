#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <tuple>

#include "IlanaLookAndFeel.h"
#include "AnimationUtils.h"

// The hover line (UI review 6: the status line only while hovering). It
// has a thin row of its own between the pages and the source chips (review
// 8, S8-11 / V8-31: it used to float over the cards' bottom edges; review
// 7, S7-20: never over the chips): while the mouse rests on a control that
// explains itself (its tooltip text), the line fades in with the control's
// name and description, and fades out when the mouse leaves it, comes down
// to the chips or the macros (quietArea), or a drag is under way. It never
// takes a click.
class InfoStrip : public juce::Component,
                  private IlanaAnim::FrameTimer
{
public:
    // A line a control says while you work it (a knob's full value during a
    // drag, the range while you type one): shown at once, drag or not, in
    // place of the hover text, until the control clears it (usability
    // flair, pass 3). Message thread only; a window shows only its own.
    struct Live
    {
        juce::Component::SafePointer<juce::Component> owner;
        juce::String title, description;
        bool warning = false;
    };
    static Live& live()
    {
        static Live line;
        return line;
    }
    static void say (juce::Component& owner, const juce::String& title, const juce::String& description, bool warning = false)
    {
        live() = { &owner, title, description, warning };
    }
    static void clear (juce::Component& owner)
    {
        if (live().owner == &owner)
            live() = {};
    }

    InfoStrip()
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (20);
    }

    // Where the line stays hidden (in the parent's coordinates): the rows
    // it covers and the strip under them.
    void setQuietArea (juce::Rectangle<int> area) { quietArea = area; }

    // Whether the line is showing, and what it says, for the tests.
    bool isShowingLine() const { return target > 0.5f; }
    juce::String getShownTitle() const { return isShowingLine() ? title : juce::String(); }

    // The text under a component, as the hover line reads it: the first
    // tooltip on the way up, its first line the title.
    static std::pair<juce::String, juce::String> describe (juce::Component* component)
    {
        for (auto* current = component; current != nullptr; current = current->getParentComponent())
        {
            if (auto* tooltipClient = dynamic_cast<juce::TooltipClient*> (current))
            {
                const auto tooltip = tooltipClient->getTooltip();

                if (tooltip.isNotEmpty())
                {
                    const auto lines = juce::StringArray::fromLines (tooltip);
                    juce::String description;

                    for (int i = 1; i < lines.size(); ++i)
                        description += (i > 1 ? "  " : "") + lines[i];

                    return { lines[0], description };
                }
            }
        }

        return {};
    }

    void paint (juce::Graphics& g) override
    {
        if (shown <= 0.01f)
            return;

        g.setOpacity (juce::jlimit (0.0f, 1.0f, shown));
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (IlanaTheme::Ui::bg);
        g.fillRect (bounds);
        IlanaTheme::paintWell (g, bounds.reduced (0.0f, 1.0f), 4.0f);

        auto textArea = getLocalBounds().reduced (10, 0);
        const auto titleFont = juce::Font (IlanaTheme::font (IlanaTheme::TextSize::body, true));
        const auto titleWidth = juce::jmin (280, juce::GlyphArrangement::getStringWidthInt (titleFont, title) + 16);

        g.setColour (warning ? juce::Colour (0xffff5656) : IlanaTheme::accent());
        g.setFont (titleFont);
        g.drawText (title, textArea.removeFromLeft (titleWidth), juce::Justification::centredLeft, true);

        // The description wraps onto a second line, shrinks to the passive
        // floor, then trails off with an ellipsis (the full text is in the
        // tooltip): it never runs under the fade into MASTER (A16-2).
        g.setColour (IlanaTheme::Ui::text2);
        g.setFont (IlanaTheme::font (IlanaTheme::TextSize::body));
        const auto descriptionArea = textArea.reduced (0, 2);
        const auto [shownText, shownFont] = fitHelp (g.getCurrentFont(), description, descriptionArea.getWidth(), descriptionArea.getHeight());
        g.setFont (shownFont);
        IlanaTheme::drawFitted (g, shownText, descriptionArea, juce::Justification::centredLeft, 2);
    }

    // The help that fits the line whole: as many of its sentences as wrap into the line's rows at
    // the text's own size; else its first sentence at the smallest size that holds it (down to the
    // passive floor); else its first clause. Never a sentence cut short ("With Hard Sync this
    // set..."): the tooltip has all of it.
    static std::pair<juce::String, juce::Font> fitHelp (const juce::Font& base, const juce::String& text, int room, int height)
    {
        const auto fits = [&] (const juce::Font& font, const juce::String& candidate)
        {
            if (juce::GlyphArrangement::getStringWidth (font, candidate) <= (float) room - 2.0f)
                return true;
            juce::GlyphArrangement wrapped;
            wrapped.addJustifiedText (font, candidate, 0.0f, font.getAscent(), (float) room - 2.0f, juce::Justification::left);
            const auto lines = wrapped.getNumGlyphs() > 0 ? juce::roundToInt (wrapped.getBoundingBox (0, -1, true).getHeight() / font.getHeight()) : 1;
            return lines <= 2 && (float) lines * font.getHeight() <= (float) height + 0.5f;
        };

        const auto trimmed = text.trim();
        if (fits (base, trimmed))
            return { trimmed, base };

        juce::StringArray sentences;
        for (auto from = 0;;)
        {
            const auto end = trimmed.indexOf (from, ". ");
            if (end < 0)
            {
                sentences.add (trimmed.substring (0, trimmed.length()));
                break;
            }
            sentences.add (trimmed.substring (0, end + 1));
            from = end + 2;
        }

        // Sentence prefixes (each the text so far), the longest that fits at the text's size.
        for (auto i = sentences.size() - 1; i >= 0; --i)
            if (fits (base, sentences[i]))
                return { sentences[i], base };

        // The first sentence alone, smaller, down to the floor.
        const auto floor = juce::jmin (base.getHeight(), IlanaTheme::TextSize::minPassive);
        for (auto size = base.getHeight() - 1.0f; size >= floor - 0.01f; size -= 1.0f)
        {
            const auto smaller = base.withHeight (size);
            if (fits (smaller, sentences[0]))
                return { sentences[0], smaller };
        }

        // Still too long: its last clause break that fits at the floor.
        const auto smallest = base.withHeight (floor);
        const auto& first = sentences[0];
        for (auto end = first.length() - 1; end > 0; --end)
            if ((first[end] == ',' || first[end] == ';' || first[end] == ':') && fits (smallest, first.substring (0, end)))
                return { first.substring (0, end) + ".", smallest };

        return { first, smallest };
    }

    // For the tests: the line showing this text at once.
    void showTextForTest (const juce::String& newTitle, const juce::String& newDescription)
    {
        title = pendingTitle = newTitle;
        description = pendingDescription = newDescription;
        shown = target = 1.0f;
    }

    // For the tests: what the line does with the mouse resting on a
    // component (or nothing) for long enough.
    void restOn (juce::Component* component)
    {
        forced = true;

        for (int i = 0; i < 12; ++i)
            step (component, false);

        forced = false;
    }

private:
    void timerCallback() override
    {
        if (forced)
            return;

        auto mouse = juce::Desktop::getInstance().getMainMouseSource();
        auto* under = mouse.getComponentUnderMouse();
        auto* parent = getParentComponent();
        const auto dragging = mouse.isDragging() || juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown();
        const auto quiet = parent != nullptr
                           && quietArea.contains (parent->getLocalPoint (nullptr, mouse.getScreenPosition()).roundToInt());
        const auto inside = parent != nullptr && under != nullptr && (under == parent || parent->isParentOf (under));

        step (inside && ! quiet ? under : nullptr, dragging);
    }

    void step (juce::Component* under, bool dragging)
    {
        // A control's live line wins, at once.
        if (const auto& line = live(); line.owner != nullptr && line.title.isNotEmpty()
                                       && line.owner->getTopLevelComponent() == getTopLevelComponent())
        {
            if (line.title != title || line.description != description || line.warning != warning)
            {
                title = pendingTitle = line.title;
                description = pendingDescription = line.description;
                warning = line.warning;
                repaint();
            }
            restTicks = 6.0f;
            target = 1.0f;
            if (shown < 1.0f)
            {
                shown = 1.0f;
                repaint();
            }
            return;
        }
        if (warning)
        {
            warning = false;
            repaint();
        }

        juce::String newTitle, newDescription;

        if (under != nullptr && ! dragging && ! wouldCover (under))
            std::tie (newTitle, newDescription) = describe (under);

        // A control's text shows once the mouse has rested on it a moment,
        // so sweeping across the page doesn't flicker the chips away.
        if (newTitle != pendingTitle || newDescription != pendingDescription)
        {
            pendingTitle = newTitle;
            pendingDescription = newDescription;
            restTicks = 0.0f;
        }
        else
        {
            restTicks += forced ? 1.0f : frameTicks();
        }

        const auto wanted = pendingTitle.isNotEmpty() && restTicks >= 6.0f;

        if (wanted && (pendingTitle != title || pendingDescription != description))
        {
            title = pendingTitle;
            description = pendingDescription;
            repaint();
        }

        target = wanted ? 1.0f : 0.0f;

        if (forced)
        {
            shown = target;
            repaint();
        }
        else if (std::abs (shown - target) > 0.01f)
        {
            shown = target > shown ? juce::jmin (1.0f, shown + 0.35f * frameTicks()) : juce::jmax (0.0f, shown - 0.5f * frameTicks());
            repaint();
        }
        else if (shown != target)
        {
            shown = target;
            repaint();
        }
    }

    // Whether the line would sit over the control under the mouse (a page
    // or a big panel, which the line always overlaps, doesn't count).
    bool wouldCover (juce::Component* under) const
    {
        auto* parent = getParentComponent();
        if (parent == nullptr || under == parent)
            return false;
        const auto area = parent->getLocalArea (under, under->getLocalBounds());
        return area.getHeight() < 200 && area.intersects (getBounds());
    }

    juce::String title, description, pendingTitle, pendingDescription;
    juce::Rectangle<int> quietArea;
    float shown = 0.0f, target = 0.0f, restTicks = 0.0f;
    bool forced = false, warning = false;
};
