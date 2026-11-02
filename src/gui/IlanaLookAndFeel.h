#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "AnimationUtils.h"
#include "../SectionColours.h"
#include <BinaryData.h>

#include <cmath>
#include <map>
#include <unordered_map>

namespace IlanaTheme
{
inline juce::Colour& accentRef()
{
    static juce::Colour accent (0xffff6a3d);
    return accent;
}

inline juce::Colour accent() { return accentRef(); }
// The accent as text on an accent-tinted fill (the sheet's acc2): lighter,
// in every skin's own hue.
inline juce::Colour accentText() { return accentRef().interpolatedWith (juce::Colours::white, 0.2f); }

inline const juce::uint32 palette[]
{
    0xffff6a3d, 0xff35c8ff, 0xff8fff3b, 0xffff4fd8
};

inline constexpr int numPalettes = 4;

// The colour tokens. Surfaces are flat graphite with a faint cool bias; the
// accent is kept for what is active or carries a value, so it stays the one
// thing the eye goes to.
namespace Ui
{
    // (The approved mockup's ladder, mock.css: ink0..ink5, line, line2, t1..t3.)
    inline const juce::Colour bg      { 0xff0f1014 }; // page (ink 1)
    inline const juce::Colour header  { 0xff0a0b0e }; // header strip and dock (ink 0)
    inline const juce::Colour panel   { 0xff15171c }; // cards, header keys (ink 2)
    inline const juce::Colour raised  { 0xff1c1f26 }; // controls: menus, pills, buttons, knob caps (ink 3)
    inline const juce::Colour hover   { 0xff313642 }; // lighter than line, so hovers show (ink 5)
    inline const juce::Colour well    { 0xff0a0b0e }; // displays (ink 0)
    inline const juce::Colour line    { 0xff2a2e37 }; // borders
    inline const juce::Colour line2   { 0xff20232a }; // hairlines inside a card (under its header)
    inline const juce::Colour track   { 0xff262a33 }; // knob and slider tracks, a switch while off (ink 4)
    inline const juce::Colour text    { 0xffeef0f4 }; // values, titles
    inline const juce::Colour text2   { 0xffaab0bb }; // labels
    inline const juce::Colour text3   { 0xff868d9b }; // hints, disabled (4.9:1 on raised; Impeccable audit)
    inline const juce::Colour accent2 { 0xffff8a63 }; // the accent as text on a tinted fill
}

// The oscillators' identity colours: shown as a small tag, a card edge and a
// waveform, never as the accent.
inline juce::Colour oscColour (int index)
{
    static const juce::uint32 colours[] { 0xfff2b83d, 0xff5b8cff, 0xff3ecf9a, 0xffa47bff, 0xffe39a6b, 0xff3fc7e8 };
    index = juce::jlimit (0, 5, index);
    if (const auto picked = SectionColours::pickedFor (SectionColours::oscKey (index)); picked != 0)
        return juce::Colour (picked);
    return juce::Colour (colours[(size_t) index]);
}

// A page whose controls keep an identity colour (a knob's arc, a switch's
// glow) hears when the shown patch's colour coding changes, and sets them
// again. Keyed by the processor, so one instance's change leaves another's
// open editor alone.
struct IdentityColourListener
{
    virtual ~IdentityColourListener() = default;
    virtual void identityColoursChanged() = 0;
};

inline juce::ListenerList<IdentityColourListener>& identityColourListeners (const void* processor)
{
    static std::map<const void*, juce::ListenerList<IdentityColourListener>> lists;
    return lists[processor];
}

// SUB and NOISE: silver, as their box on PATCH (not the accent's orange).
inline juce::Colour subNoiseColour()
{
    const auto picked = SectionColours::pickedFor (SectionColours::subKey);
    return juce::Colour (picked != 0 ? picked : 0xffc9ced8);
}

// STRINGS and BODY, the physical part of the sound: one wood colour.
inline juce::Colour physicalColour()
{
    const auto picked = SectionColours::pickedFor (SectionColours::physicalKey);
    return juce::Colour (picked != 0 ? picked : 0xffc9a27e);
}

// The envelopes' identity colours: one table for the chips, rings and cables
// (modSourceColour), the ENV page and the filter cards, so FILT ENV and
// FILT 2 are the same violet and blue wherever they appear (G-09). 0 = AMP
// ENV, 1 = FILT ENV, 2 = FILT 2, 3 = MOD ENV, 4 = ENV 4.
inline juce::Colour envIdentityColour (int index)
{
    // (MOD ENV a softer lime than the Acid skin's accent, ENV 4 green rather than
    // OSC 2's blue: Impeccable audit, identity colours shared.)
    static const juce::uint32 colours[] { 0xffff5a4a, 0xffb46bff, 0xff7d86ff, 0xff9bdb63, 0xff59eb89 };
    index = juce::jlimit (0, 4, index);
    if (const auto picked = SectionColours::pickedFor (SectionColours::envKey (index)); picked != 0)
        return juce::Colour (picked);
    return juce::Colour (colours[(size_t) index]);
}

// The picture a chip or card is dragged by. The ghost lives in its own window
// at the screen's scale, while the editor is drawn through its zoom: rendered
// at 1.0 it came out soft and smaller than what was picked up (G-13). The
// snapshot is taken at zoom x display scale and handed over at the display
// scale, so it covers the same screen size, sharp.
inline juce::ScaledImage dragGhost (juce::Component& source, juce::Rectangle<int> area, float alpha)
{
    const auto zoom = juce::jmax (0.25f, juce::Component::getApproximateScaleFactorForComponent (&source));
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (source.getScreenBounds());
    const auto displayScale = display != nullptr ? juce::jmax (1.0f, (float) display->scale) : 1.0f;
    auto image = source.createComponentSnapshot (area, true, zoom * displayScale);
    image.multiplyAllAlphas (alpha);
    return juce::ScaledImage (image, (double) displayScale);
}

// Whether a colour is one of the oscillators' (whatever its alpha).
inline bool isOscIdentityColour (juce::Colour colour)
{
    for (int i = 0; i < 6; ++i)
        if (oscColour (i).withAlpha (1.0f) == colour.withAlpha (1.0f))
            return true;
    return false;
}

// How opaque a control is while it does nothing (its section is off): dimmed,
// but still readable against the card (about 4:1 for the label text).
inline constexpr float dimmedAlpha = 166.0f / 255.0f; // about 65 %, exact in a component's 8-bit alpha

// Knob sizes by role (UI review 4, V22), the same on every page: the largest
// dial a knob takes (its cell; the ring drawn inside is 8 px smaller). A
// smaller cell shrinks the dial, down to `minimum`.
//   main:  the parameters a card is about (oscillator, filter, envelope...)
//   small: detail rows under them (a physical string's exciter, body...)
//   mini:  grids of amounts (the FM send matrix)
namespace KnobSize
{
    inline constexpr int main    = 48;
    inline constexpr int compact = 40; // (not "small": a macro in rpcndr.h on Windows)
    inline constexpr int mini    = 36;
    inline constexpr int minimum = 28;
}

// The type scale (component units; the editor zooms them). Every text in the
// UI uses one of these, so sizes stay consistent from page to page. UI review
// 6 (#46) raised the whole scale by 10 %, so that at 75 % zoom (0.75) tiny
// text still renders 9 px tall.
namespace TextSize
{
    inline constexpr float tiny    = 12.1f;  // axis ticks, badges, hints
    inline constexpr float label   = 12.65f; // knob and field labels
    inline constexpr float body    = 14.3f;  // values, menus, buttons
    inline constexpr float title   = 15.95f; // tabs, section titles
    inline constexpr float large   = 18.15f; // preset name
    inline constexpr float display = 23.1f;
    inline constexpr float hero    = 28.6f;

    // The floors (UI review 4, S25), in the same units (a font's full
    // height; Manrope's letters are about 0.73 of it): no text a user clicks
    // is smaller than minInteractive (buttons, tabs, pills, menus), no other
    // text smaller than minPassive. The UI test walks every page's labels
    // and buttons against them.
    inline constexpr float minInteractive = 13.75f;
    inline constexpr float minPassive     = tiny;

    // The smallest zoom the editor offers, and the floor at it in screen
    // pixels (UI review 5 #31, 6 #46): text that would draw smaller is left
    // out (a hint) rather than shrunk. 12 units at 75 % is 9 px, so
    // the smallest sizes grow to 10 px there (UI review 9, V9-26).
    // The packed OSC panels' button and segment text (review 23's 9.5 px at 1.5x); font() holds the screen floor.
    inline constexpr float packedTextHeight = 7.8f;

    // The packed (PLAY / review-23) cards' own steps, named so a page doesn't
    // type them (Impeccable audit: 47 raw sizes). Same values as before.
    inline constexpr float packedLabel = 7.5f;  // section names, switch and bar names, OUT
    inline constexpr float packedBody  = 8.5f;  // pills, chips, menu text in the packed cards
    inline constexpr float packedTitle = 10.0f; // a packed card's own title

    inline constexpr float smallestZoom  = 0.75f;
    inline constexpr float screenFloorPx = 10.0f;
    inline constexpr float zoomFloor     = screenFloorPx / smallestZoom;
}

// The UI test's probe: while armed, the smallest text drawn (in device
// pixels, through font() below) and how often it went under a limit.
struct FontProbe
{
    bool armed = false;
    float smallest = 1.0e6f;
    float limit = 0.0f;
    int under = 0;
};

inline FontProbe& fontProbe()
{
    static FontProbe probe;
    return probe;
}

// The fonts, released when JUCE shuts down (after the last editor closes)
// rather than as statics when the plugin is unloaded: by then JUCE's font
// engine is gone, and freeing a FreeType face after it crashed hosts on exit.
struct ThemeResources : private juce::DeletedAtShutdown
{
    // Manrope (SIL Open Font License, resources/fonts/OFL-Manrope.txt).
    juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (BinaryData::Manrope400Regular_ttf, (size_t) BinaryData::Manrope400Regular_ttfSize);
    // Used for small text: light strokes thin out at tiny sizes, so the
    // medium weight keeps stems readable.
    juce::Typeface::Ptr medium = juce::Typeface::createSystemTypefaceFor (BinaryData::Manrope500Medium_ttf, (size_t) BinaryData::Manrope500Medium_ttfSize);
    juce::Typeface::Ptr bold = juce::Typeface::createSystemTypefaceFor (BinaryData::Manrope700Bold_ttf, (size_t) BinaryData::Manrope700Bold_ttfSize);

    ~ThemeResources() override { clearSingletonInstance(); }

    JUCE_DECLARE_SINGLETON_INLINE (ThemeResources, false)
};

inline juce::Typeface::Ptr& regularTypefaceRef() { return ThemeResources::getInstance()->regular; }
inline juce::Typeface::Ptr& mediumTypefaceRef() { return ThemeResources::getInstance()->medium; }
inline juce::Typeface::Ptr& boldTypefaceRef() { return ThemeResources::getInstance()->bold; }

// The editor renders its design space through a scale transform; this mirrors
// that zoom so fonts can be snapped to whole device pixels (fractional
// rasterisation is what makes text look soft).
inline float& uiScaleRef()
{
    static float scale = 1.0f;
    return scale;
}

// A page that lays text out by its drawn width (the packed cards' buttons, pills and segments)
// hears when the zoom changes: the floor under the text makes the same words wider at a small zoom.
struct UiScaleListener
{
    virtual ~UiScaleListener() = default;
    virtual void uiScaleChanged() = 0;
};

inline std::vector<UiScaleListener*>& uiScaleListeners()
{
    static std::vector<UiScaleListener*> listeners;
    return listeners;
}

inline void notifyUiScaleListeners()
{
    const auto copy = uiScaleListeners();
    for (auto* listener : copy)
        listener->uiScaleChanged();
}

// All UI text should be created through this: a Font constructed from plain
// FontOptions resolves its typeface through the process default LookAndFeel,
// not the one set on our editor, which is why it fell back to Segoe UI.
// Pass tabular for numbers that change while you watch (knob values, BPM,
// CPU), so they don't shift sideways; everywhere else digits are
// proportional, or a "1" sits in a wide gap ("LFO 1", "1/8").
inline juce::FontOptions font (float height, bool bold = false, bool tabular = false)
{
    const auto scale = juce::jmax (0.25f, uiScaleRef());
    // No text under the screen floor, whatever the zoom (V9-26), and none under the floor's size at 100 %
    // at a bigger zoom: the approved pictures are drawn at 100 %, so the small OSC card text read that size
    // there and drew smaller in ilana's 175 % window (2026-10-23).
    const auto deviceHeight = juce::jmax (height, TextSize::screenFloorPx / juce::jmin (1.0f, scale)) * scale;
    if (auto& probe = fontProbe(); probe.armed)
    {
        probe.smallest = juce::jmin (probe.smallest, deviceHeight);
        probe.under += deviceHeight < probe.limit - 0.01f ? 1 : 0;
    }
    const auto snapped = juce::jmax (1.0f, std::round (deviceHeight)) / scale;
    // The mockup sets all text in Manrope 500 (medium) and 700 (bold), at every size.
    const auto typeface = bold ? boldTypefaceRef() : mediumTypefaceRef();

    const auto options = juce::FontOptions().withHeight (snapped)
                                            .withStyle (bold ? "Bold" : "Regular")
                                            .withTypeface (typeface);
    return tabular ? options.withFeatureEnabled ("tnum") : options;
}

// The tallest a text of `height` units draws at any of the editor's zooms
// (75 % to 200 %): font() snaps to whole device pixels and to the screen
// floor, so at 75 % a 14.3 unit name is 14.67 units tall. A line laid out
// for it holds it at every zoom (the layout doesn't change with the zoom).
inline float tallestAtAnyZoom (float height)
{
    auto tallest = height;
    for (const auto zoom : { 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f })
        tallest = juce::jmax (tallest, juce::jmax (1.0f, std::round (juce::jmax (height, TextSize::screenFloorPx / juce::jmin (1.0f, zoom)) * zoom)) / zoom);
    return tallest;
}

// An inline knob's value text (the OSC cards, the voice strip): left-aligned
// under the name, in the label size, so a cell of a dial and a few words holds
// it. `on` false restores the usual centred value.
inline void styleInlineValueBox (juce::Slider& slider, bool on)
{
    for (auto* child : slider.getChildren())
        if (auto* box = dynamic_cast<juce::Label*> (child))
        {
            box->setJustificationType (on ? juce::Justification::centredLeft : juce::Justification::centred);
            box->setBorderSize (on ? juce::BorderSize<int> (1, 0, 1, 1) : juce::BorderSize<int> (1, 1, 1, 1));
            box->setFont (font (on ? TextSize::label : TextSize::body, false, true));
        }
}

// The UI test's other probe (UI review 8, V8-12 / I8-25): while armed, every
// text drawFitted() below had to shrink, and every one it still had to cut.
struct TextFitProbe
{
    bool armed = false;
    juce::StringArray shrunk, cut;
    juce::StringArray cutDetails; // "text: needs N px of M at H"
    juce::StringArray respelled;  // "text -> line": a value drawn without its space or with a shorter unit
    juce::StringArray garbled;    // text holding double-encoded UTF-8 (V10-1)

    // With recordRects on, where each text was drawn (editor coordinates once
    // `origin` is the painting component's), as far as its letters reach, so a
    // test can see a label that a sibling covers (V14-17).
    bool recordRects = false;
    juce::Point<int> origin;
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> rects;
};

// Whether a string holds the marks of UTF-8 read as Latin-1 and encoded again
// ("Â·" for a middle dot, "â€" for a dash or quote): never real text here.
inline bool isMojibake (const juce::String& text)
{
    return text.contains (juce::String::fromUTF8 ("\xc3\x82")) || text.contains (juce::String::fromUTF8 ("\xc3\x83"))
           || text.contains (juce::String::fromUTF8 ("\xc3\xa2\xe2\x82\xac")) || text.contains (juce::String::fromUTF8 ("\xef\xbf\xbd"));
}

inline TextFitProbe& textFitProbe()
{
    static TextFitProbe probe;
    return probe;
}

inline void noteTextRect (const juce::String& text, const juce::Font& font, juce::Rectangle<int> area, juce::Justification justification)
{
    auto& probe = textFitProbe();
    if (! probe.armed || ! probe.recordRects)
        return;

    const auto width = (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, text));
    auto box = area.withSizeKeepingCentre (area.getWidth(), juce::jmin (area.getHeight(), (int) std::ceil (font.getHeight())));
    if (justification.testFlags (juce::Justification::left))
        box = box.withWidth (juce::jmin (width, box.getWidth()));
    else if (justification.testFlags (juce::Justification::right))
        box = box.withLeft (juce::jmax (box.getX(), box.getRight() - width));
    else
        box = box.withSizeKeepingCentre (juce::jmin (width, box.getWidth()), box.getHeight());
    probe.rects.push_back ({ text, box.translated (probe.origin.x, probe.origin.y) });
}

// Text in a box, the one way the UI fits text that may be too long (UI review
// 8, V8-12): never squeezed sideways (JUCE's fitted text condenses glyphs,
// which reads as a broken font next to normal text). A line that doesn't fit
// tries, in turn, until it does:
//   1. a smaller size, down to `floorHeight` (the floor for its kind: the
//      interactive one for values, menus and buttons);
//   2. the same, with its letters set a little closer (tracking, up to
//      0.04 of the height between letters: spacing, not narrower glyphs,
//      and never enough to close a gap between words);
//   3. for a value with a unit ("-30.9 dB"), the space goes and a long
//      unit is shortened ("kHz" to "k"): a value never loses its unit;
//   4. down to the passive floor (still 10 px at 75 %), tracked;
//   5. wrapped onto its other lines where it may take more, else cut with
//      an ellipsis.
// The probe records every line that had to change and every one cut.
inline juce::Font fittedFont (const juce::Font& font, const juce::String& line, float room, float floor)
{
    const auto scale = juce::jmax (0.25f, uiScaleRef());
    const auto width = juce::GlyphArrangement::getStringWidth (font, line);
    // (Nor shrunk under the screen floor at a small zoom: V9-26.)
    floor = juce::jmin (font.getHeight(), juce::jmax (floor, TextSize::screenFloorPx / juce::jmin (1.0f, scale)));

    // Width follows height closely, so one step lands near the fit; snap
    // down to whole device pixels, as font() does, so the glyphs stay crisp.
    auto height = juce::jmax (floor, std::floor (font.getHeight() * room / juce::jmax (1.0f, width) * scale) / scale);
    auto smaller = font.withHeight (height);

    while (height > floor && juce::GlyphArrangement::getStringWidth (smaller, line) > room + 0.01f)
    {
        height = juce::jmax (floor, height - 1.0f / scale);
        smaller = font.withHeight (height);
    }

    // (Tracking is a touch only, never more than 0.04 of the height: more
    // closes the gap between words, "AMPENV", and reads as condensed type.)
    for (auto tracking = -0.01f; tracking >= -0.0401f && juce::GlyphArrangement::getStringWidth (smaller, line) > room + 0.01f;
         tracking -= 0.01f)
        smaller = font.withHeight (height).withExtraKerningFactor (tracking);

    return smaller;
}

inline bool fitsIn (const juce::Font& font, const juce::String& line, float room)
{
    if (juce::GlyphArrangement::getStringWidth (font, line) > room + 0.01f)
        return false;

    // The width fits, but drawText curtails by its own glyph positions (the
    // letters' advances, snapped): a line within a pixel or two of the edge
    // can lose its last letter ("PRESSUR", A16-2). Ask it the same way.
    if (juce::GlyphArrangement::getStringWidth (font, line) > room - 2.0f)
    {
        juce::GlyphArrangement whole, curtailed;
        whole.addLineOfText (font, line, 0.0f, 0.0f);
        curtailed.addCurtailedLineOfText (font, line, 0.0f, 0.0f, room, false);
        return curtailed.getNumGlyphs() >= whole.getNumGlyphs();
    }

    return true;
}

inline void drawFitted (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
                        juce::Justification justification, int maximumLines = 1,
                        float floorHeight = TextSize::minPassive)
{
    if (area.isEmpty() || text.isEmpty())
        return;

    if (auto& probe = textFitProbe(); probe.armed && isMojibake (text))
        probe.garbled.addIfNotAlreadyThere (text);

    if (text.containsAnyOf ("\r\n"))
    {
        g.drawFittedText (text, area, justification, juce::jmax (1, maximumLines), 1.0f);
        return;
    }

    auto line = text.trim();
    const auto font = g.getCurrentFont();
    const auto room = (float) area.getWidth();

    if (fitsIn (font, line, room))
    {
        noteTextRect (line, font, area, justification);
        g.drawText (line, area, justification, false);
        return;
    }

    // Text given more than one line wraps at its own size first, when the
    // lines fit the box; it only shrinks when they don't.
    if (maximumLines > 1)
    {
        juce::GlyphArrangement wrapped;
        wrapped.addJustifiedText (font, line, 0.0f, font.getAscent(), room, juce::Justification::left);
        const auto lines = wrapped.getNumGlyphs() > 0
                               ? juce::roundToInt (wrapped.getBoundingBox (0, -1, true).getHeight() / font.getHeight())
                               : 1;

        if (lines <= maximumLines && (float) lines * font.getHeight() <= (float) area.getHeight() + 0.5f)
        {
            g.drawFittedText (line, area, justification, maximumLines, 1.0f);
            return;
        }
    }

    auto fitted = fittedFont (font, line, room, floorHeight);

    if (! fitsIn (fitted, line, room))
    {
        // A number with a unit after a space never loses its unit (V9-5):
        // the space goes first ("-30.9dB"), then a long unit is shortened
        // ("kHz" to "k"), each at the floor size; the passive floor last.
        const auto number = line.upToLastOccurrenceOf (" ", false, false);
        const auto unit = line.fromLastOccurrenceOf (" ", false, false);
        const auto isValue = number.isNotEmpty() && unit.length() <= 3 && ! unit.containsAnyOf ("0123456789")
                             && number.retainCharacters ("0123456789").isNotEmpty()
                             && number.removeCharacters ("0123456789.,+-:/").removeCharacters (juce::String::fromUTF8 ("\xe2\x88\x92\xc3\x97")).isEmpty();
        auto settled = false;

        if (isValue)
        {
            const auto shortUnit = unit == "kHz" ? juce::String ("k") : unit == "oct" ? juce::String ("o") : unit;
            const juce::String candidates[] { number + unit, number + shortUnit };

            for (const auto& candidate : candidates)
            {
                const auto attempt = fittedFont (font, candidate, room, floorHeight);

                if (fitsIn (attempt, candidate, room))
                {
                    line = candidate;
                    fitted = attempt;
                    settled = true;
                    break;
                }
            }

            if (! settled)
            {
                line = number + shortUnit;
                fitted = fittedFont (font, line, room, TextSize::minPassive);
                settled = true;
            }
        }

        if (! settled)
            fitted = fittedFont (font, line, room, TextSize::minPassive);
    }

    const auto fits = fitsIn (fitted, line, room);

    if (! fits && maximumLines > 1)
    {
        g.drawFittedText (line, area, justification, maximumLines, 1.0f);
        // (A wrapped text that needs more lines than the box holds is cut like any other: V13-20.)
        if (auto& probe = textFitProbe(); probe.armed)
        {
            juce::AttributedString attributed;
            attributed.append (line, fitted);
            juce::TextLayout layout;
            layout.createLayout (attributed, (float) area.getWidth());
            const auto lineHeight = fitted.getHeight() + 1.0f;
            if (layout.getHeight() > (float) area.getHeight() + lineHeight * 0.25f)
            {
                probe.cut.addIfNotAlreadyThere (text.trim());
                probe.cutDetails.add (line + ": wraps to more lines than " + juce::String (area.getHeight()) + " px hold");
            }
        }
        return;
    }

    if (auto& probe = textFitProbe(); probe.armed)
    {
        if (line != text.trim())
            probe.respelled.addIfNotAlreadyThere (text.trim() + " -> " + line);
        (fits ? probe.shrunk : probe.cut).addIfNotAlreadyThere (text.trim());
        if (! fits)
            probe.cutDetails.add (line + ": needs " + juce::String (juce::GlyphArrangement::getStringWidth (fitted, line), 1) + " of "
                                  + juce::String (room, 1) + " px at " + juce::String (fitted.getHeight(), 2));
    }

    noteTextRect (line, fitted, area, justification);
    g.setFont (fitted);
    g.drawText (line, area, justification, ! fits);
    g.setFont (font);
}

inline void drawFitted (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                        juce::Justification justification, int maximumLines = 1,
                        float floorHeight = TextSize::minPassive)
{
    drawFitted (g, text, area.toNearestInt(), justification, maximumLines, floorHeight);
}

inline void paintPageBackground (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    g.setColour (Ui::bg);
    g.fillRect (bounds);

    // A faint accent haze from the top, so the page isn't dead flat.
    juce::ColourGradient haze (accent().withAlpha (0.035f), (float) bounds.getCentreX(), (float) bounds.getY() - 80.0f,
                               accent().withAlpha (0.0f), (float) bounds.getCentreX(), (float) bounds.getY() + (float) bounds.getHeight() * 0.7f, true);
    g.setGradientFill (haze);
    g.fillRect (bounds);
}

// A soft glow around a rounded rectangle, built from a few widening strokes.
inline void paintGlow (juce::Graphics& g, juce::Rectangle<float> bounds, float radius, juce::Colour colour, float strength)
{
    if (strength <= 0.01f)
        return;

    for (int i = 3; i >= 1; --i)
    {
        const auto spread = (float) i * 2.2f;
        g.setColour (colour.withAlpha (0.07f * strength / (float) i));
        g.drawRoundedRectangle (bounds.expanded (spread * 0.5f), radius + spread * 0.5f, spread);
    }
}

// The component sheet's card (.cd): ink 2, a hairline, 10 px corners (a
// small box keeps its own smaller ones); its family colour only as a 7 %
// wash at the top that is gone by mid-card (.tint).
inline void paintCard (juce::Graphics& g, juce::Rectangle<float> bounds, float radius, juce::Colour tint)
{
    if (bounds.getHeight() >= 40.0f && bounds.getWidth() >= 120.0f)
        radius = juce::jmax (radius, 10.0f);

    g.setColour (Ui::panel);
    g.fillRoundedRectangle (bounds, radius);

    // The family colour as the sheet's .tint: 7 % at the very top, gone by
    // mid-card, so the colour reads as identity and the eye lands on the
    // displays.
    juce::ignoreUnused (tint);

    g.setColour (Ui::line);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius - 0.5f, 1.0f);
}

inline void paintRecessedPanel (juce::Graphics& g, juce::Rectangle<float> bounds, float radius)
{
    g.setColour (Ui::bg.interpolatedWith (Ui::panel, 0.5f));
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (Ui::line.withAlpha (0.7f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
}

// The sheet's well (.well): ink 0, a line2 hairline, a 1 px black shadow
// along its inner top edge.
inline void paintWell (juce::Graphics& g, juce::Rectangle<float> bounds, float radius)
{
    g.setColour (Ui::well);
    g.fillRoundedRectangle (bounds, radius);
    {
        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (bounds.reduced (1.0f), juce::jmax (0.0f, radius - 1.0f));
        g.reduceClipRegion (clip);
        g.setColour (juce::Colours::black);
        g.fillRect (bounds.reduced (1.0f).withHeight (1.0f));
    }
    g.setColour (Ui::line2);
    g.drawRoundedRectangle (bounds.reduced (0.5f), juce::jmax (0.0f, radius - 0.5f), 1.0f);
}

// Kept for callers that layered it over a well; the flat theme has no gloss.
inline void paintGlassOverlay (juce::Graphics&, juce::Rectangle<float>, float) {}

// A section's identity tag: a small glowing dot in its colour.
inline void paintTag (juce::Graphics& g, juce::Point<float> centre, juce::Colour colour)
{
    // (The sheet's .ch .dot: 8 px with a 3 px halo at 20 %.)
    g.setColour (colour.withAlpha (0.2f));
    g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (centre));
    g.setColour (colour);
    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (centre));
}

// Whether a tab's switchable part is on (review 7, I7-37: one indicator on
// every tab that names a switch, CardTabs and the FM operator pills): a lit
// dot with a halo while on, a quiet ring while off.
// `on` may be between 0 and 1 while it fades (fade()).
inline void paintOnDot (juce::Graphics& g, juce::Point<float> centre, juce::Colour colour, float on)
{
    const auto dot = juce::Rectangle<float> (6.0f, 6.0f).withCentre (centre);
    on = juce::jlimit (0.0f, 1.0f, on);

    if (on > 0.0f)
    {
        g.setColour (colour.withAlpha (0.3f * on));
        g.fillEllipse (dot.expanded (2.5f));
        g.setColour (colour.interpolatedWith (juce::Colours::white, 0.15f).withMultipliedAlpha (on));
        g.fillEllipse (dot);
    }

    if (on < 1.0f)
    {
        g.setColour (juce::Colours::white.withAlpha (0.28f * (1.0f - on)));
        g.drawEllipse (dot.reduced (0.5f), 1.0f);
    }
}

// A header caption (a card's or a section's subtitle) as the one grammar
// headers use: a lower-case fragment without a full stop (review 7, I7-35;
// hints under a control are sentences). See docs/UI-CONVENTIONS.md.
inline juce::String captionFragment (const juce::String& text)
{
    auto fragment = text.trim();

    if (fragment.endsWithChar ('.') && ! fragment.endsWith (".."))
        fragment = fragment.dropLastCharacters (1);

    // A role written in caps ("TO OUTPUT, MODULATES 1") reads as the sheet's
    // lower-case caption ("to output, modulates 1").
    if (fragment.length() > 6 && fragment.containsChar (' ') && fragment == fragment.toUpperCase()
        && fragment.containsAnyOf ("ABCDEFGHIJKLMNOPQRSTUVWXYZ"))
        return fragment.toLowerCase();

    if (fragment.length() > 1 && juce::CharacterFunctions::isUpperCase (fragment[0])
        && juce::CharacterFunctions::isLowerCase (fragment[1]))
        fragment = fragment.substring (0, 1).toLowerCase() + fragment.substring (1);

    return fragment;
}

// A card's title: the sheet's h4 (11 px at 800, tracked 0.08 em).
inline juce::FontOptions cardTitleFont() { return font (15.0f, true).withKerningFactor (0.08f); }

// Where a card title's subtitle can start: past the tag and the name.
inline int cardTitleWidth (const juce::String& text)
{
    // (The tag and its 8 px gap, the title, then 8 px before the caption: the sheet's .ch gap.)
    return juce::GlyphArrangement::getStringWidthInt (juce::Font (cardTitleFont()), text) + 22;
}

// A card's title: its tag, then the name in the text colour. A card with no
// family colour (a grey or white one: OUTPUT, SIGNAL FLOW) has no tag, so
// no grey dot reads as "switched off" beside the on dots (review 8, I8-32).
inline bool hasFamilyColour (juce::Colour colour) { return colour.getSaturation() > 0.15f; }

inline void paintCardTitle (juce::Graphics& g, juce::Rectangle<int> header, const juce::String& text, juce::Colour colour)
{
    const auto tagged = hasFamilyColour (colour);

    if (tagged)
        paintTag (g, { (float) header.getX() + 3.0f, (float) header.getCentreY() }, colour);

    g.setColour (Ui::text);
    g.setFont (cardTitleFont());
    // (A title never ends in an ellipsis: it shrinks to fit, as every other
    // text does. A card with no tag starts at its own left edge.)
    drawFitted (g, text, header.withTrimmedLeft (tagged ? 14 : 0), juce::Justification::centredLeft, 1, TextSize::minInteractive);
}

// A card's header, the same everywhere: tag, title, then a quiet subtitle
// right after the title (never pushed to the right edge, which belongs to the
// card's tabs and its on switch).
// A hint cut to the room it has: whole " · " parts are dropped from the end,
// and a hint that still doesn't fit is left out rather than cut mid-word
// ("Operator E...") or shrunk.
inline juce::String fittedHint (const juce::String& hint, const juce::Font& f, float width)
{
    const auto dot = juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 "));
    auto text = hint;

    while (text.isNotEmpty() && juce::GlyphArrangement::getStringWidth (f, text) > width)
        text = text.contains (dot) ? text.upToLastOccurrenceOf (dot, false, false) : juce::String();

    return text;
}

inline void paintCardHeader (juce::Graphics& g, juce::Rectangle<int> header, const juce::String& title,
                             const juce::String& subtitle, juce::Colour colour, int rightReserve = 100)
{
    paintCardTitle (g, header, title, colour);

    if (subtitle.isEmpty())
        return;

    // The sheet's .cap: 10.5 px in t3, 8 px after the title.
    auto area = header.withTrimmedLeft (cardTitleWidth (title)).withTrimmedRight (rightReserve);
    g.setColour (Ui::text3);
    g.setFont (font (TextSize::body));
    g.drawText (fittedHint (captionFragment (subtitle), juce::Font (font (TextSize::body)), (float) area.getWidth()), area,
                juce::Justification::centredLeft, false);
}

// Where a card's on switch goes (a ToggleControl with its 13 px label
// space), centred on the title line: at the card's right edge (its pill
// level with the right of the card's content), or left of the remove
// button on cards that have one.
inline juce::Rectangle<int> cardSwitchBounds (juce::Rectangle<int> card, int titleCentreY, bool besideRemoveButton = false)
{
    // Level with the title's letters, which sit a touch below the line's
    // centre.
    titleCentreY += 1;

    if (besideRemoveButton)
    {
        constexpr int width = 56, rightInset = 8 + 28;
        return { card.getRight() - rightInset - width, titleCentreY - 13 - 10, width, 13 + 20 };
    }

    // A 32 px pill centred in 40, its right edge 12 px in from the card's:
    // the same margin as the title's tag on the left.
    constexpr int width = 40, rightInset = 12 - 4;
    return { card.getRight() - rightInset - width, titleCentreY - 13 - 10, width, 13 + 20 };
}

// An on/off switch: a pill with a sliding knob. `amount` runs 0 (off) to 1
// (on) so callers can animate it; the on state glows in `colour`.
// The one animator for every small state change drawn in paint() (a
// switch's slide, a pill's choice and hover, a segment's fill): eases a
// value toward its target over time, keyed by the component and a channel,
// and repaints the owner until it settles. A key not painted for two
// seconds (a page just opened, a component at a reused address) starts
// where it should be rather than sliding in from a stale state.
// The rates match ToggleControl's own timer (30 % and 22 % a 60 Hz frame).
namespace FadeRate
{
    inline constexpr float slide = 21.4f; // a switch's knob, a choice's fill
    inline constexpr float hover = 14.9f; // a hover's tint
}

// The core, keyed by any address and a channel: the look-and-feel's own
// hovers (animatedHover) run on it too. `fresh` false starts it at the target.
struct FadeState { float value = 0.0f; double time = 0.0; };
inline std::map<std::pair<const void*, int>, FadeState>& fadeStates()
{
    static std::map<std::pair<const void*, int>, FadeState> states;
    return states;
}

// Where a fade is right now, without advancing it (`fallback` when it has
// not run lately): for a second painter that has to agree with the first.
inline float fadePeek (const void* key, int channel, float fallback)
{
    const auto& states = fadeStates();
    const auto found = states.find ({ key, channel });

    if (found == states.end() || found->second.time <= 0.0 || juce::Time::getMillisecondCounterHiRes() - found->second.time > 200.0)
        return fallback;

    return found->second.value;
}

inline float fadeValue (const void* key, int channel, float target, float rate, bool fresh, bool& moving)
{
    using State = FadeState;
    auto& states = fadeStates();
    static double lastSweep = 0.0;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    moving = false;

    if (states.size() > 4096 && now - lastSweep > 1000.0)
    {
        lastSweep = now;
        for (auto it = states.begin(); it != states.end();)
            it = now - it->second.time > 60000.0 ? states.erase (it) : std::next (it);
    }

    auto& state = states[{ key, channel }];

    if (! fresh || ! IlanaAnim::fullMotion() || state.time <= 0.0 || now - state.time > 2000.0)
    {
        state = { target, now };
        return target;
    }

    const auto dt = juce::jlimit (0.0, 0.1, (now - state.time) * 0.001);
    state.time = now;
    state.value += (target - state.value) * (1.0f - std::exp (-(float) dt * rate));

    if (std::abs (target - state.value) < 0.004f)
        state.value = target;
    else
        moving = true;

    return state.value;
}

inline float fade (juce::Component& owner, int channel, float target, float rate = FadeRate::slide)
{
    auto moving = false;
    const auto value = fadeValue (&owner, channel, target, rate, IlanaAnim::showing (owner), moving);

    if (moving)
        owner.repaint();

    return value;
}

inline void paintSwitch (juce::Graphics& g, juce::Rectangle<float> area, float amount, juce::Colour colour, float hover = 0.0f)
{
    // The component sheet's switch (.tg): 32 x 18, ink 4 with a hairline
    // while off, filled with its colour while on (no glow); a 12 px knob
    // 3 px in, grey while off, white while on.
    const auto h = juce::jmin (area.getHeight(), 18.0f);

    if (h < 6.0f)
        return;

    const auto w = h * 32.0f / 18.0f;
    const auto pill = juce::Rectangle<float> (w, h).withCentre (area.getCentre());
    const auto fill = Ui::track.interpolatedWith (colour, amount).interpolatedWith (juce::Colours::white, 0.06f * hover);

    g.setColour (fill);
    g.fillRoundedRectangle (pill, h * 0.5f);
    g.setColour (Ui::line.interpolatedWith (colour, amount));
    g.drawRoundedRectangle (pill.reduced (0.5f), h * 0.5f - 0.5f, 1.0f);

    const auto inset = h / 6.0f;
    const auto knob = h - 2.0f * inset;
    const auto x = pill.getX() + inset + (w - 2.0f * inset - knob) * amount;
    g.setColour (juce::Colour (0xff98a0ad).interpolatedWith (juce::Colours::white, amount));
    g.fillEllipse (x, pill.getY() + inset, knob, knob);
}

// A pill's text: clickable, so at the interactive floor.
inline juce::FontOptions pillFont() { return font (TextSize::minInteractive, true).withKerningFactor (0.06f); }

// A choice pill: the one style for every small selector (F1 / F2, 12 / 24
// dB, OSC 1 / 2 / 3, ARP / EUCLID, WAVE / SPEC ...). The chosen one is tinted
// in its colour with a bright edge; the others are quiet.
inline void paintPill (juce::Graphics& g, juce::Rectangle<float> pill, const juce::String& text, juce::Colour colour,
                       float active, float hover = 0.0f, bool enabled = true, float fontSize = 0.0f)
{
    // The component sheet's pill (.pill): ink 3 with a hairline and t2
    // text; chosen, its colour at 22 % over ink 3, a 75 % edge and t1 text.
    // `active` may be between 0 and 1 while a choice fades (fade()).
    active = juce::jlimit (0.0f, 1.0f, active);
    const auto radius = pill.getHeight() * 0.5f;
    g.setColour (Ui::raised.interpolatedWith (colour, 0.22f * active).interpolatedWith (Ui::hover, 0.5f * hover));
    g.fillRoundedRectangle (pill, radius);

    g.setColour (Ui::line.interpolatedWith (Ui::text3, 0.4f * hover).interpolatedWith (colour.withAlpha (0.75f), active));
    g.drawRoundedRectangle (pill.reduced (0.5f), radius - 0.5f, 1.0f);

    g.setColour (Ui::text2.withAlpha (enabled ? 1.0f : 0.5f).interpolatedWith (Ui::text, juce::jmax (active, 0.5f * hover)));
    g.setFont (fontSize > 0.0f ? font (fontSize, true).withKerningFactor (0.08f) : pillFont());
    g.drawText (text, pill, juce::Justification::centred);
}
// A card header's "EDIT ›" link (the sheet's .lk): bold caps tracked 0.08 em
// in the card's colour, text only.
inline juce::FontOptions linkFont() { return font (TextSize::minInteractive, true).withKerningFactor (0.08f); }

// Draws a TextButton as a choice pill in `colour` when chosen.
inline void makePill (juce::Button& button, juce::Colour colour)
{
    button.getProperties().set ("pill", true);
    button.setColour (juce::TextButton::buttonOnColourId, colour);
    button.repaint();
}

} // namespace IlanaTheme

class IlanaLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // A select's text (the sheet's 11.5 px).
    static constexpr float selectText = 15.5f;
    // The packed OSC panels' text (review 23 mockups, drawn at 1.5x: 11.5, 9 and 8.5 px), in units; font() holds the 10 px floor.
    static constexpr float packedMenuText = 9.0f, packedNameText = 7.5f;
    // A packed menu's text size: its own ("packedMenuSize", the OSC strip's lane menus) or the cards' 9 px.
    static float packedMenuSizeOf (const juce::ComboBox& box) { return (float) box.getProperties().getWithDefault ("packedMenuSize", packedMenuText); }

    // A button's corners: 8 px at 30 px high, 6 px at 24 (the sheet's .btn and .btn.sm).
    static float buttonRadius (float height) { return juce::jlimit (3.0f, 8.0f, height * 0.26f); }

    IlanaLookAndFeel()
    {
        using namespace IlanaTheme;

        regularTypeface = mediumTypefaceRef(); // (the mockup's body weight, 500)
        boldTypeface = boldTypefaceRef();

        if (regularTypeface != nullptr)
            setDefaultSansSerifTypeface (regularTypeface);

        setColour (juce::ResizableWindow::backgroundColourId, Ui::bg);

        setColour (juce::Slider::textBoxTextColourId, Ui::text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::thumbColourId, juce::Colours::white);

        setColour (juce::Label::textColourId, Ui::text2);

        setColour (juce::ComboBox::backgroundColourId, Ui::raised);
        setColour (juce::ComboBox::outlineColourId, Ui::line);
        setColour (juce::ComboBox::textColourId, Ui::text);
        setColour (juce::ComboBox::arrowColourId, Ui::text3);

        setColour (juce::PopupMenu::backgroundColourId, Ui::raised.withAlpha (0.995f)); // (not opaque: a see-through window keeps the round corners)
        setColour (juce::PopupMenu::textColourId, Ui::text);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::PopupMenu::headerTextColourId, Ui::text2);

        setColour (juce::TextButton::buttonColourId, Ui::raised);
        setColour (juce::TextButton::textColourOffId, Ui::text2);
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);

        setColour (juce::ToggleButton::textColourId, Ui::text2);

        setColour (juce::TabbedComponent::backgroundColourId, Ui::bg);
        setColour (juce::TabbedComponent::outlineColourId, Ui::line);

        setColour (juce::TabbedButtonBar::tabOutlineColourId, Ui::line);
        setColour (juce::TabbedButtonBar::tabTextColourId, Ui::text2);
        setColour (juce::TabbedButtonBar::frontTextColourId, Ui::text);

        setColour (juce::ScrollBar::thumbColourId, Ui::track);
        setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);

        setColour (juce::TooltipWindow::backgroundColourId, Ui::raised);
        setColour (juce::TooltipWindow::textColourId, Ui::text);
        setColour (juce::TooltipWindow::outlineColourId, Ui::line);

        setColour (juce::TextEditor::backgroundColourId, Ui::well);
        setColour (juce::TextEditor::outlineColourId, Ui::line);
        setColour (juce::TextEditor::textColourId, Ui::text);
        setColour (juce::TextEditor::highlightedTextColourId, Ui::text);
        setColour (juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);

        // Dialogs (the sheet's .dlg): ink 2 with a hairline rim.
        setColour (juce::AlertWindow::backgroundColourId, Ui::panel);
        setColour (juce::AlertWindow::textColourId, Ui::text);
        setColour (juce::AlertWindow::outlineColourId, Ui::line);

        // Lists and JUCE's own file browser (used where no native chooser is).
        setColour (juce::ListBox::backgroundColourId, Ui::well);
        setColour (juce::ListBox::outlineColourId, Ui::line);
        setColour (juce::ListBox::textColourId, Ui::text);
        setColour (juce::DirectoryContentsDisplayComponent::textColourId, Ui::text);
        setColour (juce::FileBrowserComponent::currentPathBoxBackgroundColourId, Ui::well);
        setColour (juce::FileBrowserComponent::currentPathBoxTextColourId, Ui::text);
        setColour (juce::FileBrowserComponent::currentPathBoxArrowColourId, Ui::text3);
        setColour (juce::FileBrowserComponent::filenameBoxBackgroundColourId, Ui::well);
        setColour (juce::FileBrowserComponent::filenameBoxTextColourId, Ui::text);
        setColour (juce::FileChooserDialogBox::titleTextColourId, Ui::text);
        setColour (juce::DocumentWindow::textColourId, Ui::text);

        setColour (juce::ProgressBar::backgroundColourId, Ui::track);
        setColour (juce::BubbleComponent::backgroundColourId, Ui::raised);
        setColour (juce::BubbleComponent::outlineColourId, Ui::hover);
        setColour (juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::TreeView::backgroundColourId, Ui::well);
        setColour (juce::TreeView::linesColourId, Ui::line);

        applyAccent();
    }

    void setAccent (juce::Colour colour)
    {
        IlanaTheme::accentRef() = colour;
        applyAccent();
    }

    // Everything that is clicked to act or choose shows the hand, as the
    // custom pills, tabs and chips do: every button and menu box (a cursor
    // a component sets itself is kept).
    juce::MouseCursor getMouseCursorFor (juce::Component& component) override
    {
        const auto cursor = LookAndFeel_V4::getMouseCursorFor (component);

        if (cursor == juce::MouseCursor::NormalCursor && component.isEnabled()
            && (dynamic_cast<juce::Button*> (&component) != nullptr || dynamic_cast<juce::ComboBox*> (&component) != nullptr))
            return juce::MouseCursor::PointingHandCursor;

        return cursor;
    }

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font& font) override
    {
        if (regularTypeface == nullptr)
            return LookAndFeel_V4::getTypefaceForFont (font);

        const auto style = font.getTypefaceStyle();

        if (boldTypeface != nullptr
            && (style.containsIgnoreCase ("Bold") || style.containsIgnoreCase ("Semi")
                || style.containsIgnoreCase ("Black")))
            return boldTypeface;

        return regularTypeface;
    }

    // Every menu's text the same size from 18 px boxes up (review 8, S8-22:
    // a 19 px EXCITE box drew "Piano Hammer" a size down from its
    // neighbours); only shorter boxes go to the interactive floor.
    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        // The OSC cards' packed menus (review 23): 11.5 px at the mockup's scale.
        if (box.getProperties().getWithDefault ("packedMenu", false))
            return juce::Font (IlanaTheme::font (packedMenuSizeOf (box)));
        // (The sheet's select text is 11.5 px: 15.7 units; boxes under 22 px keep the body size.)
        return juce::Font (IlanaTheme::font (box.getHeight() >= 22 ? selectText : box.getHeight() >= 18 ? IlanaTheme::TextSize::body
                                                                                                          : IlanaTheme::TextSize::minInteractive));
    }

    juce::Font getPopupMenuFont() override
    {
        return juce::Font (IlanaTheme::font (selectText)); // (the sheet's menu text: 11.5 px)
    }

    juce::Font getLabelFont (juce::Label& label) override
    {
        // Labels are built once, but painted at every zoom level, so re-run the
        // size through IlanaTheme::font to keep device pixels whole.
        // A knob's value is always bold, as the component sheet's (.kv: 700).
        const auto& current = label.getFont();
        const auto isValue = dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr;
        // (The label's letter-spacing is kept: names are tracked caps.)
        return juce::Font (IlanaTheme::font (current.getHeight(), current.isBold() || isValue,
                                             label.getProperties().contains ("tabular"))
                               .withKerningFactor (current.getExtraKerningFactor()));
    }

    juce::Font getTextButtonFont (juce::TextButton& button, int buttonHeight) override
    {
        // The OSC cards' header buttons (review 23 mockups, 9.5 px at 1.5x): drawn through
        // font(), so the 10 px screen floor holds; the UI test checks them against it.
        if (button.getProperties().contains ("packedButton"))
        {
            const auto text = button.getButtonText();
            if (text.length() == 1 && ! juce::CharacterFunctions::isLetterOrDigit (text[0]))
                return juce::Font (IlanaTheme::font (IlanaTheme::TextSize::packedTitle, true));
            return juce::Font (IlanaTheme::font (IlanaTheme::TextSize::packedTextHeight, true).withKerningFactor (0.07f));
        }

        // A lone symbol ("×", "+") is a glyph a third the font's height:
        // draw it larger, so a remove or add button reads as one.
        if (const auto text = button.getButtonText(); text.length() == 1 && ! juce::CharacterFunctions::isLetterOrDigit (text[0]))
            return juce::Font (IlanaTheme::font (juce::jlimit (IlanaTheme::TextSize::minInteractive, 22.0f, (float) buttonHeight * 0.95f)));


        // Never under the interactive floor, however short the button; bold
        // and tracked 0.04 em, as the sheet's .btn (11 px at 30, 10 px at 24).
        return juce::Font (IlanaTheme::font (juce::jlimit (IlanaTheme::TextSize::minInteractive, 15.0f,
                                                           (float) buttonHeight * 0.5f), true)
                               .withKerningFactor (0.04f));
    }

    juce::Label* createSliderTextBox (juce::Slider& slider) override
    {
        auto* label = LookAndFeel_V4::createSliderTextBox (slider);
        label->getProperties().set ("tabular", true); // a live value
        label->setBorderSize ({ 1, 1, 1, 1 });          // (the value keeps its room, and its unit's space: V10-8)
        label->setFont (IlanaTheme::font (IlanaTheme::TextSize::body, true, true)); // bold, as the mockup's values

        if (slider.getProperties().getWithDefault ("inlineKnob", false))
        {
            label->setJustificationType (juce::Justification::centredLeft);
            label->setBorderSize ({ 1, 0, 1, 1 });
            label->setFont (IlanaTheme::font (IlanaTheme::TextSize::body, false, true));
        }

        return label;
    }

    // An inline knob (a dial with its name and value to the right): the dial
    // a square at the left, the value under the middle line.
    juce::Slider::SliderLayout getSliderLayout (juce::Slider& slider) override
    {
        // A packed dial (review 23): the slider is the dial's square, no text box.
        if ((float) slider.getProperties().getWithDefault ("packedDial", 0.0f) > 0.0f)
        {
            juce::Slider::SliderLayout layout;
            layout.sliderBounds = slider.getLocalBounds();
            layout.textBoxBounds = {};
            return layout;
        }

        if (slider.getProperties().getWithDefault ("inlineKnob", false))
        {
            const auto area = slider.getLocalBounds();
            // (40, or the knob's own inline size: the FX rack's taller rows.)
            const auto dial = juce::jmin (area.getHeight(), (int) slider.getProperties().getWithDefault ("inlineDial", 40));
            juce::Slider::SliderLayout layout;
            layout.sliderBounds = juce::Rectangle<int> (dial, dial).withCentre ({ dial / 2, area.getCentreY() });
            layout.textBoxBounds = { dial + 3, area.getCentreY(), juce::jmax (0, area.getWidth() - dial - 3), 14 };
            return layout;
        }

        return LookAndFeel_V4::getSliderLayout (slider);
    }

    void applyAccent()
    {
        const auto accent = IlanaTheme::accent();

        setColour (juce::Slider::rotarySliderFillColourId, accent);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.28f));
        setColour (juce::TextButton::buttonOnColourId, accent.withAlpha (0.85f));
        setColour (juce::ToggleButton::tickColourId, accent);
        setColour (juce::TabbedButtonBar::frontOutlineColourId, accent);
        setColour (juce::TextEditor::focusedOutlineColourId, accent);
        setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
        setColour (juce::CaretComponent::caretColourId, accent);
        setColour (juce::ProgressBar::foregroundColourId, accent);
        setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, accent.withAlpha (0.15f));
        setColour (juce::DirectoryContentsDisplayComponent::highlightedTextColourId, IlanaTheme::accentText());
        setColour (juce::TreeView::selectedItemBackgroundColourId, accent.withAlpha (0.15f));
    }

    // The tab bar of the shell mockup: bold caps in grey (white while chosen),
    // no pill, a 2 px accent underline under the chosen tab.
    static juce::FontOptions tabFont() { return IlanaTheme::font (15.0f, true).withKerningFactor (0.08f); } // (the sheet's 11 px tabs)

    int getTabButtonBestWidth (juce::TabBarButton& button, int) override
    {
        if (button.getProperties()["hiddenTab"]) // (a page behind the row: no button)
            return 0;
        return juce::GlyphArrangement::getStringWidthInt (juce::Font (tabFont()), button.getButtonText()) + 22;
    }

    void drawTabButton (juce::TabBarButton& button, juce::Graphics& g, bool isMouseOver, bool isMouseDown) override
    {
        if (button.getProperties()["hiddenTab"])
            return;
        const auto bounds = button.getLocalBounds().toFloat();
        const auto active = button.getToggleState();
        const auto hover = animatedHover (&button, isMouseOver || isMouseDown, 14.0f);
        const auto lit = animatedHover (reinterpret_cast<const char*> (&button) + 1, active, 10.0f);

        g.setColour (IlanaTheme::Ui::text3.interpolatedWith (IlanaTheme::Ui::text, juce::jmax (lit, hover * 0.7f)));
        g.setFont (tabFont());
        g.drawText (button.getButtonText(), bounds.withTrimmedBottom (1.0f), juce::Justification::centred);

        // A hovered or pinned mod source's routes on this page: a badge in
        // the source's colour with the count, at the name's top right
        // (usability flair, pass 1; the editor sets "routeCount").
        const auto routes = (int) button.getProperties().getWithDefault ("routeCount", 0);
        const auto badge = IlanaTheme::fade (button, 903, routes > 0 ? 1.0f : 0.0f, 22.0f);
        if (badge > 0.01f)
        {
            const auto shownCount = routes > 0 ? routes : (int) button.getProperties().getWithDefault ("routeCountShown", 0);
            if (routes > 0)
                button.getProperties().set ("routeCountShown", routes);
            const auto colour = juce::Colour ((juce::uint32) (juce::int64) button.getProperties().getWithDefault ("routeColour", (juce::int64) 0xffff5c9a));
            const auto textWidth = juce::GlyphArrangement::getStringWidth (juce::Font (tabFont()), button.getButtonText());
            const auto label = shownCount > 9 ? juce::String ("9+") : juce::String (shownCount);
            const auto font = IlanaTheme::font (10.5f, true);
            const auto width = juce::jmax (12.0f, juce::GlyphArrangement::getStringWidth (juce::Font (font), label) + 6.0f);
            const auto pill = juce::Rectangle<float> (width, 12.0f).withCentre ({ bounds.getCentreX() + textWidth * 0.5f + width * 0.5f - 1.0f,
                                                                                  bounds.getCentreY() - 9.0f })
                                  .constrainedWithin (bounds.reduced (0.5f));
            g.setColour (colour.withMultipliedAlpha (badge));
            g.fillRoundedRectangle (pill, 6.0f);
            g.setColour (IlanaTheme::Ui::bg.withMultipliedAlpha (badge));
            g.setFont (font);
            g.drawText (label, pill, juce::Justification::centred);
        }
        if (badge > 0.01f && badge < 0.99f)
            button.repaint();

        // A small dot where the badge goes while none shows: blue while A|B
        // is hovered and the other slot differs here, else orange where a
        // knob moved since the preset loaded (pass 2; the editor sets
        // "abCount" and "editedCount").
        {
            const auto ab = (int) button.getProperties().getWithDefault ("abCount", 0) > 0;
            const auto edited = (int) button.getProperties().getWithDefault ("editedCount", 0) > 0;
            const auto dotOn = IlanaTheme::fade (button, 904, (ab || edited) && routes == 0 ? 1.0f : 0.0f, 22.0f);
            if (dotOn > 0.01f)
            {
                const auto textWidth = juce::GlyphArrangement::getStringWidth (juce::Font (tabFont()), button.getButtonText());
                const auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ bounds.getCentreX() + textWidth * 0.5f + 4.5f, bounds.getCentreY() - 9.0f })
                                     .constrainedWithin (bounds.reduced (0.5f));
                g.setColour ((ab ? juce::Colour (0xff5aa0ff) : IlanaTheme::accent()).withMultipliedAlpha (dotOn));
                g.fillEllipse (dot);
            }
            if (dotOn > 0.01f && dotOn < 0.99f)
                button.repaint();
        }

        // (The underline is the bar's: drawTabAreaBehindFrontButton.)

        if ((hover > 0.01f && hover < 0.99f) || (lit > 0.01f && lit < 0.99f))
            button.repaint();
    }

    // The chosen tab's 2 px accent underline, drawn by the bar so it glides
    // to a new tab (about 160 ms, review 26 flair) instead of jumping.
    void drawTabAreaBehindFrontButton (juce::TabbedButtonBar& bar, juce::Graphics& g, int w, int h) override
    {
        juce::LookAndFeel_V4::drawTabAreaBehindFrontButton (bar, g, w, h);

        const auto* front = bar.getTabButton (bar.getCurrentTabIndex());
        const auto shown = front != nullptr && ! (bool) front->getProperties()["hiddenTab"] && front->getWidth() > 20;
        const auto alpha = IlanaTheme::fade (bar, 902, shown ? 1.0f : 0.0f);

        if (! shown && alpha < 0.01f)
            return;

        // (Its own state, not the shared fade: a bar unpainted for a while must still glide.)
        auto& glide = tabGlides[&bar];
        if (shown)
        {
            const auto target = front->getBounds().toFloat();
            const auto to = juce::Point<float> (target.getCentreX(), target.getWidth() - 20.0f);
            if (to != glide.to)
            {
                glide.from = glide.to.x < 0.0f ? to : glide.shown;
                glide.to = to;
                glide.tween.start (160.0f);
            }
            glide.shown = glide.from + (glide.to - glide.from) * IlanaAnim::easeOutCubic (glide.tween.linear());
            if (glide.tween.running())
                bar.repaint();
        }
        const auto centre = glide.shown.x, width = glide.shown.y;

        if (width < 1.0f)
            return;

        g.setColour (IlanaTheme::accent().withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (juce::Rectangle<float> (width, 2.0f).withCentre ({ centre, (float) h - 1.0f }), 1.0f);
    }

    struct TabGlide
    {
        juce::Point<float> from, to { -1.0f, 0.0f }, shown; // (centre x, width)
        IlanaAnim::Tween tween;
    };
    std::map<const juce::TabbedButtonBar*, TabGlide> tabGlides;

    // Check boxes (the tour's "Don't show this again") in the theme's font
    // and colours; the stock one drew its text in the default typeface.
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool) override
    {
        const auto box = juce::Rectangle<float> (18.0f, 18.0f).withCentre ({ 13.0f, (float) button.getHeight() * 0.5f });
        // The hover and the tick's rim fade (the shared animator); a
        // keyboard focus rings the box in the accent.
        const auto hover = IlanaTheme::fade (button, 0, highlighted ? 1.0f : 0.0f, IlanaTheme::FadeRate::hover);
        const auto ticked = IlanaTheme::fade (button, 1, button.getToggleState() ? 1.0f : 0.0f);
        g.setColour (IlanaTheme::Ui::raised.interpolatedWith (IlanaTheme::Ui::hover, hover));
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (IlanaTheme::Ui::text3.interpolatedWith (IlanaTheme::accent(), ticked).withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
        g.drawRoundedRectangle (box, 4.0f, 1.4f);

        if (button.hasKeyboardFocus (false))
        {
            g.setColour (IlanaTheme::accent().withAlpha (0.5f));
            g.drawRoundedRectangle (box.expanded (2.5f), 6.0f, 1.0f);
        }

        if (ticked > 0.01f)
        {
            g.setColour (IlanaTheme::accent().withAlpha (ticked));
            juce::Path tick;
            tick.startNewSubPath (box.getX() + 4.5f, box.getCentreY());
            tick.lineTo (box.getX() + 7.8f, box.getBottom() - 5.0f);
            tick.lineTo (box.getRight() - 4.0f, box.getY() + 5.0f);
            g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setColour (button.findColour (juce::ToggleButton::textColourId));
        g.setFont (IlanaTheme::font (IlanaTheme::TextSize::body));
        g.drawText (button.getButtonText(), button.getLocalBounds().withTrimmedLeft (30), juce::Justification::centredLeft);
    }

    // The packed dial (review 23 mockups): a track arc and the value arc, no cap and
    // no pointer; the value is written inside by the knob. Its arc radius is
    // KnobControl's packedRadius - 1 (see there).
    // A double-click reset just now (KnobSlider marks it): the hand glides
    // back to the default instead of jumping, as a preset's values do.
    static bool justReset (const juce::Slider& slider)
    {
        const auto at = (double) slider.getProperties().getWithDefault ("resetGlideAt", 0.0);
        return at > 0.0 && juce::Time::getMillisecondCounterHiRes() - at < 400.0;
    }

    // Hovering a knob: a small grey notch on the track where a double-click
    // resets it to (its default), unless it is already there.
    static void paintDefaultNotch (juce::Graphics& g, juce::Slider& slider, juce::Point<float> centre, float arcRadius, float lineWidth,
                                   float hover, float sliderPos, float rotaryStartAngle, float rotaryEndAngle)
    {
        if (hover < 0.01f || ! slider.isDoubleClickReturnEnabled() || ! slider.isEnabled())
            return;

        const auto proportion = (float) slider.valueToProportionOfLength (slider.getDoubleClickReturnValue());

        if (! std::isfinite (proportion) || std::abs (proportion - sliderPos) < 0.01f)
            return;

        const auto angle = rotaryStartAngle + juce::jlimit (0.0f, 1.0f, proportion) * (rotaryEndAngle - rotaryStartAngle);
        const auto dir = juce::Point<float> (std::cos (angle - juce::MathConstants<float>::halfPi),
                                             std::sin (angle - juce::MathConstants<float>::halfPi));
        juce::Path notch;
        notch.startNewSubPath (centre + dir * (arcRadius - lineWidth * 1.4f));
        notch.lineTo (centre + dir * (arcRadius + lineWidth * 0.5f));
        g.setColour (IlanaTheme::Ui::text.withAlpha (0.8f * juce::jmin (1.0f, hover * 2.5f))); // (in quickly: there before the eye lands on it)
        g.strokePath (notch, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Where a dragged knob started (KnobSlider sets "dragGhostPos" on the
    // press and "dragGhostRelease" on the release): a white tick on the
    // track at the starting value and a faint stretch to where it is now,
    // so the move reads at a glance; it fades out 400 ms after the release.
    static void paintDragGhost (juce::Graphics& g, juce::Slider& slider, juce::Point<float> centre, float arcRadius, float lineWidth,
                                float angle, float rotaryStartAngle, float rotaryEndAngle)
    {
        auto& properties = slider.getProperties();
        const auto start = (float) properties.getWithDefault ("dragGhostPos", -1.0f);

        if (start < 0.0f)
            return;

        const auto released = (double) properties.getWithDefault ("dragGhostRelease", 0.0);
        auto strength = 1.0f;

        if (released > 0.0)
        {
            const auto since = (float) ((juce::Time::getMillisecondCounterHiRes() - released) * IlanaAnim::motionClockRate());
            strength = IlanaAnim::fullMotion() ? 1.0f - IlanaAnim::easeInOutCubic (since / 400.0f) : 0.0f;

            if (strength <= 0.0f)
            {
                properties.remove ("dragGhostPos");
                properties.remove ("dragGhostRelease");
                return;
            }

            slider.repaint(); // (the fade's next frame)
        }

        const auto startAngle = rotaryStartAngle + start * (rotaryEndAngle - rotaryStartAngle);

        if (std::abs (startAngle - angle) < 0.02f)
            return; // (not moved yet: nothing to mark)

        juce::Path moved;
        moved.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (startAngle, angle), juce::jmax (startAngle, angle), true);
        g.setColour (juce::Colours::white.withAlpha (0.22f * strength));
        g.strokePath (moved, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const auto dir = juce::Point<float> (std::cos (startAngle - juce::MathConstants<float>::halfPi),
                                             std::sin (startAngle - juce::MathConstants<float>::halfPi));
        juce::Path tick;
        tick.startNewSubPath (centre + dir * (arcRadius - lineWidth * 1.7f));
        tick.lineTo (centre + dir * (arcRadius + lineWidth * 0.5f));
        g.setColour (juce::Colours::white.withAlpha (0.9f * strength));
        g.strokePath (tick, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void drawPackedDial (juce::Graphics& g, int x, int y, int width, int height, float targetPos,
                         float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
    {
        using namespace IlanaTheme;

        const auto hover = animatedHover (&slider, slider.isMouseOver() || slider.isMouseButtonDown(), 12.0f);
        auto gliding = false;
        const auto sliderPos = fadeValue (&slider, 90, targetPos, 20.0f, (! slider.isMouseButtonDown() || justReset (slider)) && IlanaAnim::showing (slider), gliding);
        if (gliding)
            slider.repaint();

        const auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
        const auto centre = area.getCentre();
        const auto lineWidth = 2.4f;
        const auto arcRadius = juce::jmax (4.0f, juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - lineWidth * 0.5f - 0.3f);
        const auto enabled = slider.isEnabled();
        const auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const auto flare = valueFlare (&slider, targetPos, slider.isMouseOverOrDragging());

        if (slider.hasKeyboardFocus (false))
        {
            g.setColour (accent().withAlpha (0.6f));
            g.drawEllipse (juce::Rectangle<float> (arcRadius * 2.0f + 5.0f, arcRadius * 2.0f + 5.0f).withCentre (centre), 1.2f);
        }

        const auto stroke = juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        juce::Path backgroundArc;
        backgroundArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (Ui::track.interpolatedWith (juce::Colours::white, 0.14f * hover));
        if (slider.getProperties().getWithDefault ("notModulatable", false))
        {
            juce::Path dotted;
            const float dashes[] { 1.0f, lineWidth * 1.15f };
            juce::PathStrokeType (lineWidth * 0.85f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createDashedStroke (dotted, backgroundArc, dashes, 2);
            g.fillPath (dotted);
        }
        else
        {
            g.strokePath (backgroundArc, stroke);
        }

        auto originPos = 0.0f;
        if (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0)
            originPos = (float) slider.valueToProportionOfLength (0.0);
        const auto originAngle = rotaryStartAngle + originPos * (rotaryEndAngle - rotaryStartAngle);
        const auto fill = enabled ? slider.findColour (juce::Slider::rotarySliderFillColourId) : Ui::text3;

        if (std::abs (sliderPos - originPos) > 0.001f)
        {
            juce::Path valueArc;
            valueArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                    juce::jmin (originAngle, angle), juce::jmax (originAngle, angle), true);
            g.setColour (fill.interpolatedWith (juce::Colours::white, 0.2f * flare + 0.18f * hover));
            g.strokePath (valueArc, stroke);
        }

        paintDragGhost (g, slider, centre, arcRadius, lineWidth, angle, rotaryStartAngle, rotaryEndAngle);
        paintDefaultNotch (g, slider, centre, arcRadius, lineWidth, hover, sliderPos, rotaryStartAngle, rotaryEndAngle);

        if (flare > 0.01f || (hover > 0.01f && hover < 0.99f))
            slider.repaint();
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float targetPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        using namespace IlanaTheme;

        if ((float) slider.getProperties().getWithDefault ("packedDial", 0.0f) > 0.0f)
        {
            drawPackedDial (g, x, y, width, height, targetPos, rotaryStartAngle, rotaryEndAngle, slider);
            return;
        }

        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
        const auto hover = animatedHover (&slider, slider.isMouseOver() || slider.isMouseButtonDown(), 12.0f);

        // The hand follows the value under the pointer exactly; a value that
        // changes by itself (a preset, undo, a reset, automation) glides to
        // its place in a few frames instead of jumping. Offscreen (snapshots,
        // tests) it is always exact.
        auto gliding = false;
        const auto sliderPos = fadeValue (&slider, 90, targetPos, 20.0f, (! slider.isMouseButtonDown() || justReset (slider)) && IlanaAnim::showing (slider), gliding);

        if (gliding)
            slider.repaint();

        // (An inline knob with rings caps its dial so a ring fits its cell: "dialRadiusCap".)
        const auto radius = juce::jmin ((float) slider.getProperties().getWithDefault ("dialRadiusCap", 30.0f),
                                        juce::jlimit (14.0f, 30.0f, juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f));
        const auto centre = bounds.getCentre();
        const auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        // The component sheet's knob (lib.js knobSvg): for a cell of s px the
        // arc runs at s/2 - 5 with a 3 px stroke, the cap is a disc 4 px
        // inside it, the pointer a 2 px white line from near the centre to
        // 3 px short of the arc.
        const auto arcRadius = radius - 1.0f;
        const auto lineWidth = arcRadius >= 12.0f ? 3.0f : 2.5f;
        const auto enabled = slider.isEnabled();

        // The keyboard is on this knob (Tab): a ring around the dial.
        if (slider.hasKeyboardFocus (false))
        {
            g.setColour (accent().withAlpha (0.6f));
            g.drawEllipse (juce::Rectangle<float> (arcRadius * 2.0f + 6.0f, arcRadius * 2.0f + 6.0f).withCentre (centre), 1.2f);
        }

        // Recent movement makes the arc flare, then settle.
        const auto flare = valueFlare (&slider, targetPos, slider.isMouseOverOrDragging());

        juce::Path backgroundArc;
        backgroundArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (Ui::track.interpolatedWith (juce::Colours::white, 0.04f * hover));
        if (slider.getProperties().getWithDefault ("notModulatable", false))
        {
            // A knob no source can drive: a dotted track (UI review 7, I7-5).
            juce::Path dotted;
            const float dashes[] { 1.0f, lineWidth * 1.15f };
            juce::PathStrokeType (lineWidth * 0.85f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
                .createDashedStroke (dotted, backgroundArc, dashes, 2);
            g.fillPath (dotted);
        }
        else
        {
            g.strokePath (backgroundArc, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Pages dim controls that do nothing right now (a section switched
        // off) by fading their control; those arcs go grey too, so an idle
        // knob never looks live. Only the knob's own wrappers count, not the
        // page fading in.
        // (Wrappers are small; a page, card or panel is wide, and its alpha
        // is a transition, so the walk stops before reading one.)
        auto dimmed = false;
        for (auto* c = slider.getParentComponent(); c != nullptr && ! dimmed; c = c->getParentComponent())
        {
            if (c->getWidth() > 200 || dynamic_cast<juce::Viewport*> (c) != nullptr)
                break;
            dimmed = c->getAlpha() < 0.99f;
        }

        // (The sheet dims an idle knob as a whole, .kn.dim, its arc keeping
        // its colour under the fade; only a disabled one goes grey.)
        juce::ignoreUnused (dimmed);
        const auto accent = enabled ? slider.findColour (juce::Slider::rotarySliderFillColourId)
                                    : Ui::text3;

        // Bipolar parameters (pan, fine, bend...) fill from the centre.
        auto originPos = 0.0f;

        if (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0)
            originPos = (float) slider.valueToProportionOfLength (0.0);

        const auto originAngle = rotaryStartAngle + originPos * (rotaryEndAngle - rotaryStartAngle);

        // The value arc: flat, in the family colour (no glow); it brightens
        // for a moment while the user moves it.
        if (std::abs (sliderPos - originPos) > 0.001f)
        {
            juce::Path valueArc;
            valueArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                    juce::jmin (originAngle, angle), juce::jmax (originAngle, angle), true);
            g.setColour (accent.interpolatedWith (juce::Colours::white, 0.2f * flare + 0.18f * hover));
            g.strokePath (valueArc, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        paintDragGhost (g, slider, centre, arcRadius, lineWidth, angle, rotaryStartAngle, rotaryEndAngle);
        paintDefaultNotch (g, slider, centre, arcRadius, lineWidth, hover, sliderPos, rotaryStartAngle, rotaryEndAngle);

        // The cap: ink 3 with a hairline rim, a touch lighter on hover.
        const auto bodyRadius = arcRadius - 4.0f;
        const auto bodyBounds = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);

        g.setColour (Ui::raised.interpolatedWith (Ui::hover, 0.5f * hover));
        g.fillEllipse (bodyBounds);
        g.setColour (Ui::line.interpolatedWith (Ui::text3, 0.4f * hover));
        g.drawEllipse (bodyBounds.reduced (0.5f), 1.0f);

        // The pointer, from near the centre to 3 px short of the arc.
        const auto dir = juce::Point<float> (std::cos (angle - juce::MathConstants<float>::halfPi),
                                             std::sin (angle - juce::MathConstants<float>::halfPi));
        const auto p1 = centre + dir * juce::jmax (4.0f, arcRadius - 11.0f);
        const auto p2 = centre + dir * (arcRadius - 3.0f);
        juce::Path pointer;
        pointer.startNewSubPath (p1);
        pointer.lineTo (p2);
        g.setColour (enabled ? Ui::text : Ui::text3);
        g.strokePath (pointer, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (flare > 0.01f || (hover > 0.01f && hover < 0.99f))
            slider.repaint(); // keep the flare and hover easing
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        using namespace IlanaTheme;

        const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        const auto hover = animatedHover (&button, shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown, 14.0f);

        if (hover > 0.01f && hover < 0.99f)
            button.repaint();

        if (button.getProperties().contains ("pill"))
        {
            paintPill (g, bounds, button.getButtonText(), button.findColour (juce::TextButton::buttonOnColourId).withAlpha (1.0f),
                       fade (button, 0, button.getToggleState() ? 1.0f : 0.0f), hover, button.isEnabled(),
                       (float) button.getProperties().getWithDefault ("pillFontSize", 0.0f));
            return;
        }

        if (button.getProperties().contains ("switch"))
        {
            // Switches without an animation driver just show their state.
            const auto amount = button.getProperties().contains ("switchAmount")
                                    ? (float) button.getProperties()["switchAmount"]
                                    : fade (button, 1, button.getToggleState() ? 1.0f : 0.0f); // (one without its own driver slides by the shared animator)
            // A group's own name draws the state: a dot, then the name in the
            // family colour, dimmed while off (drawn here, on the switch that
            // takes the click, so nothing sits over its text).
            if (button.getProperties().contains ("groupName"))
            {
                const auto on = juce::jlimit (0.0f, 1.0f, amount); // (fades as the switch slides)
                const auto area = button.getLocalBounds().toFloat();
                const auto groupColour = juce::Colour ((juce::uint32) (int) button.getProperties()["groupColour"]);
                // (In a packed panel the name is the panel's title: its size, the dot smaller.)
                const auto packed = button.getProperties().contains ("packedGroup");
                IlanaTheme::paintOnDot (g, { area.getX() + (packed ? 3.0f : 4.0f), area.getCentreY() }, groupColour, on);
                g.setColour (groupColour.withAlpha (0.55f + 0.35f * on));
                g.setFont (packed ? IlanaTheme::font (7.6f, true).withKerningFactor (0.13f) : IlanaTheme::font (IlanaTheme::TextSize::label, true));
                IlanaTheme::drawFitted (g, button.getProperties()["groupText"].toString(), button.getLocalBounds().withTrimmedLeft (packed ? 9 : 13),
                                        juce::Justification::centredLeft, 1);
                return;
            }

            // Centred in its space, under its name when it has one; in the
            // owner's colour when it has one (an oscillator's).
            const auto colour = button.getProperties().contains ("switchColour")
                                    ? juce::Colour ((juce::uint32) (int) button.getProperties()["switchColour"])
                                    : accent();
            paintSwitch (g, bounds, amount, colour, hover);

            if (button.hasKeyboardFocus (false))
            {
                g.setColour (accent().withAlpha (0.6f));
                g.drawRoundedRectangle (bounds.expanded (2.5f), bounds.getHeight() * 0.5f + 2.5f, 1.0f);
            }

            return;
        }

        // The component sheet's button (.btn): ink 3 with a hairline, 8 px
        // corners at 30 px and 6 px at 24 (.sm). Lit, its colour at 22 % over
        // ink 3 with a 75 % edge, never a solid block.
        // (Lit and unlit fade into each other: the shared animator.)
        const auto on = fade (button, 2, button.getToggleState() ? 1.0f : 0.0f);
        const auto onColour = button.findColour (juce::TextButton::buttonOnColourId).withAlpha (1.0f);
        const auto radius = buttonRadius (bounds.getHeight());

        auto fill = backgroundColour.interpolatedWith (Ui::raised.interpolatedWith (onColour, 0.22f), on);
        fill = fill.interpolatedWith (Ui::hover, hover * 0.5f);

        if (shouldDrawButtonAsDown)
            fill = fill.darker (0.2f);

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, radius);

        g.setColour (Ui::line.interpolatedWith (Ui::text3, 0.45f * hover).interpolatedWith (onColour.withAlpha (0.75f), on));
        g.drawRoundedRectangle (bounds.reduced (0.5f), radius - 0.5f, 1.0f);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool highlighted, bool down) override
    {
        if (button.getProperties().contains ("switch") || button.getProperties().contains ("pill"))
            return;

        juce::ignoreUnused (highlighted, down);
        g.setFont (getTextButtonFont (button, button.getHeight()));

        // The lit fill fades (drawButtonBackground); the text's colour follows
        // it instead of snapping (the same fade: asking again in the same
        // frame returns the value it just reached).
        const auto lit = IlanaTheme::fade (button, 2, button.getToggleState() ? 1.0f : 0.0f);
        const auto offText = button.findColour (juce::TextButton::textColourOffId);

        if (button.getToggleState() && button.isEnabled())
        {
            g.setColour (offText.interpolatedWith (IlanaTheme::Ui::text, lit));
            IlanaTheme::drawFitted (g, button.getButtonText(), button.getLocalBounds().reduced (4, 2),
                                    juce::Justification::centred, 1, IlanaTheme::TextSize::minInteractive);
            return;
        }

        // The stock text's insets (LookAndFeel_V2), drawn through the one
        // fitting rule instead of its condensed two-line fit.
        const auto yIndent = juce::jmin (4, button.proportionOfHeight (0.3f));
        const auto cornerSize = juce::jmin (button.getHeight(), button.getWidth()) / 2;
        const auto fontHeight = juce::roundToInt (getTextButtonFont (button, button.getHeight()).getHeight() * 0.6f);
        const auto leftIndent = juce::jmin (fontHeight, 2 + cornerSize / (button.isConnectedOnLeft() ? 4 : 2));
        const auto rightIndent = juce::jmin (fontHeight, 2 + cornerSize / (button.isConnectedOnRight() ? 4 : 2));
        g.setColour ((button.getToggleState() ? button.findColour (juce::TextButton::textColourOnId)
                                              : lit > 0.01f && button.isEnabled() ? offText.interpolatedWith (IlanaTheme::Ui::text, lit) // (fading out)
                                                                                  : offText)
                         .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
        IlanaTheme::drawFitted (g, button.getButtonText(),
                                juce::Rectangle<int> (leftIndent, yIndent, button.getWidth() - leftIndent - rightIndent, button.getHeight() - yIndent * 2),
                                juce::Justification::centred, 1, IlanaTheme::TextSize::minInteractive);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                       int, int, int, int, juce::ComboBox& box) override
    {
        using namespace IlanaTheme;

        const auto bounds = juce::Rectangle<float> ((float) width, (float) height).reduced (0.5f);
        const auto hover = animatedHover (&box, box.isMouseOver() || isButtonDown, 14.0f);

        if (hover > 0.01f && hover < 0.99f)
            box.repaint();
        // The component sheet's select (.sel): ink 3, a hairline, 7 px
        // corners, a small t3 chevron 8 px in from the right.
        const auto packedBox = (bool) box.getProperties().getWithDefault ("packedMenu", false);
        const auto radius = packedBox ? 4.0f : juce::jmin (7.0f, bounds.getHeight() * 0.27f);

        g.setColour (box.findColour (juce::ComboBox::backgroundColourId).interpolatedWith (Ui::hover, 0.45f * hover));
        g.fillRoundedRectangle (bounds, radius);
        g.setColour (box.findColour (juce::ComboBox::outlineColourId).interpolatedWith (Ui::text3, 0.45f * hover));
        g.drawRoundedRectangle (bounds.reduced (0.5f), radius - 0.5f, 1.0f);

        const auto size = juce::jmin (7.0f, (float) height * 0.28f);
        const auto c = juce::Point<float> ((float) width - (packedBox ? 8.0f : 11.5f), (float) height * 0.5f);
        juce::Path chevron;
        chevron.startNewSubPath (c.x - size * 0.5f, c.y - size * 0.25f);
        chevron.lineTo (c.x, c.y + size * 0.25f);
        chevron.lineTo (c.x + size * 0.5f, c.y - size * 0.25f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId).interpolatedWith (Ui::text, 0.6f * hover)
                        .withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
        g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // A packed menu (review 23): the name inside, then the value, both painted here
        // (the box's own label is left empty, the text is the menu's).
        if (packedBox)
        {
            const auto prefixW = comboPrefixWidth (box);
            const auto textOn = box.isEnabled() ? 1.0f : 0.5f;
            if (prefixW > 0)
            {
                g.setColour (Ui::text3.withMultipliedAlpha (textOn));
                g.setFont (font (packedNameText, true).withKerningFactor (0.09f));
                g.drawText (box.getProperties()["textPrefix"].toString(), juce::Rectangle<int> (5, 0, prefixW, height), juce::Justification::centredLeft, false);
            }
            const auto value = box.getText();
            auto textX = 5 + prefixW;
            // A value that names another oscillator wears that oscillator's dot.
            if (box.getProperties().contains ("valueDot"))
            {
                for (const auto& start : juce::StringArray::fromTokens (box.getProperties()["valueDotPrefixes"].toString(), "|", ""))
                    if (value.startsWith (start))
                    {
                        g.setColour (juce::Colour ((juce::uint32) (int) box.getProperties()["valueDot"]).withMultipliedAlpha (textOn));
                        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ (float) textX + 2.5f, (float) height * 0.5f }));
                        textX += 9;
                        break;
                    }
            }
            g.setColour ((isPlaceholderValue (value) ? Ui::text3 : Ui::text).withMultipliedAlpha (textOn));
            g.setFont (font (packedMenuSizeOf (box)));
            IlanaTheme::drawFitted (g, value, juce::Rectangle<int> (textX, 0, juce::jmax (0, width - textX - 15), height), juce::Justification::centredLeft, 1);
            return;
        }

        // A menu that says what it is: its name in the dim label colour before the value.
        if (const auto prefix = box.getProperties()["textPrefix"].toString(); prefix.isNotEmpty() && comboPrefixWidth (box) > 0)
        {
            g.setColour (Ui::text3.withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
            g.setFont (font (TextSize::tiny, true).withKerningFactor (0.07f));
            g.drawText (prefix, juce::Rectangle<int> (10, 0, comboPrefixWidth (box), height), juce::Justification::centredLeft, false);
        }
    }

    static int comboPrefixWidth (const juce::ComboBox& box)
    {
        const auto prefix = box.getProperties()["textPrefix"].toString();
        if (prefix.isNotEmpty() && (bool) box.getProperties().getWithDefault ("packedMenu", false))
        {
            const auto prefixW = juce::GlyphArrangement::getStringWidthInt (IlanaTheme::font (packedNameText, true).withKerningFactor (0.09f), prefix) + 5;
            // (The value comes first: a menu with no room for both drops its name, which the tooltip keeps.)
            const auto valueW = juce::GlyphArrangement::getStringWidthInt (IlanaTheme::font (packedMenuSizeOf (box)), box.getText()) + 2;
            return valueW > box.getWidth() - 5 - prefixW - 15 ? 0 : prefixW;
        }
        // (A menu too narrow for its name and a value drops the name: the 75 % zoom's.)
        if (prefix.isEmpty() || box.getWidth() < 118)
            return 0;
        return juce::GlyphArrangement::getStringWidthInt (IlanaTheme::font (IlanaTheme::TextSize::tiny, true).withKerningFactor (0.07f), prefix) + 4;
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        // (A packed menu paints its own text: drawComboBox.)
        if (box.getProperties().getWithDefault ("packedMenu", false))
        {
            label.setBounds ({});
            return;
        }
        const auto shift = comboPrefixWidth (box);
        label.setBounds (4 + shift, 1, box.getWidth() - 23 - shift, box.getHeight() - 2); // (the sheet's 10 px of padding: 4 here, 6 the label's own)
        label.setFont (getComboBoxFont (box));
    }

    // The value words that mean "no setting of its own" (the knob's
    // automatic or free state), drawn dim under a knob.
    static bool isPlaceholderValue (const juce::String& text)
    {
        // ("Auto" and "Random" are settings and read in the value colour: review 11, S11-17.)
        return text == "Free" || text == "Off" || text == "None";
    }

    // Labels (slider values included) never draw a box: a value reads as
    // text under its knob on every page.
    void drawLabel (juce::Graphics& g, juce::Label& label) override
    {
        g.fillAll (label.findColour (juce::Label::backgroundColourId));

        if (! label.isBeingEdited())
        {
            // A disabled name is never fainter than 3:1 against its card
            // (V9-25): its own fade and its card's dimming are counted
            // together, and the label gives back what they take.
            auto shown = 1.0f;
            for (const juce::Component* c = &label; c != nullptr; c = c->getParentComponent())
                shown *= c->getAlpha();
            const auto alpha = label.isEnabled() ? 1.0f : juce::jlimit (0.5f, 1.0f, 0.55f / juce::jmax (0.01f, shown));
            auto font = getLabelFont (label);
            // A row of names shares one size (layoutRow sets the cap).
            if (label.getProperties().contains ("fitCap"))
                font = font.withHeight (juce::jmin (font.getHeight(), (float) label.getProperties()["fitCap"]));
            // A word standing in for a number ("Auto", "Free", "Off") reads
            // dim, as a default rather than a setting (UI review 9, I9-25).
            const auto placeholder = dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr
                                     && isPlaceholderValue (label.getText());
            // "Auto" and "Random" are settings, set apart from a number by the
            // quieter colour of a menu's entry (review 12, S12-5).
            const auto entryWord = ! placeholder && dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr
                                   && (label.getText() == "Auto" || label.getText() == "Random");
            g.setColour ((placeholder ? IlanaTheme::Ui::text3 : entryWord ? IlanaTheme::Ui::text2 : label.findColour (juce::Label::textColourId)).withMultipliedAlpha (alpha)); // (dimmer than a number: I10-15)
            g.setFont (font);
            const auto textArea = getLabelBorderSize (label).subtractedFrom (label.getLocalBounds());
            // Never condensed (V8-12): shrunk to the floor, then cut. A value
            // or a combo's text is clicked, so it keeps the interactive floor.
            const auto interactive = label.isEditable() || dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr
                                     || dynamic_cast<juce::ComboBox*> (label.getParentComponent()) != nullptr;
            IlanaTheme::drawFitted (g, label.getText(), textArea, label.getJustificationType(),
                                    juce::jmax (1, (int) ((float) textArea.getHeight() / font.getHeight())),
                                    interactive ? IlanaTheme::TextSize::minInteractive : IlanaTheme::TextSize::minPassive);
        }
    }

    // The component sheet's menu (.menu): ink 3 with an ink 5 rim, 10 px
    // corners and 5 px of padding; 26 px rows whose highlight (mouse or
    // keys) is a rounded accent tint with its text in the light accent;
    // section names in small tracked t3 caps; hairline separators; ticks
    // in the accent. Every menu draws through here: the editor makes this
    // look the process default while it is open (DefaultLookAndFeelScope),
    // as a menu shown on the desktop never asks its target for one.
    static constexpr int menuBorder = 5;
    static constexpr int menuRowHeight = 26;
    static constexpr int menuSeparatorHeight = 9;
    static constexpr int menuSectionHeight = 27;
    static constexpr float menuRadius = 10.0f;

    static juce::Font menuSectionFont()
    {
        return juce::Font (IlanaTheme::font (13.0f, true).withKerningFactor (0.1f)); // (the sheet's 9.5 px, 800)
    }

    int getPopupMenuBorderSize() override { return menuBorder; }

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override
    {
        if (isSeparator)
        {
            idealWidth = 50;
            idealHeight = menuSeparatorHeight;
            return;
        }

        // A combo's menu asks for rows its box's height; the sheet's rows
        // are 26 px whatever opened them.
        idealHeight = juce::jmax (menuRowHeight, standardMenuItemHeight);
        // 10 px padding, the tick column and its gap, the text, then room
        // for a submenu's chevron and the right padding.
        idealWidth = juce::GlyphArrangement::getStringWidthInt (getPopupMenuFont(), text) + 10 + 18 + 32;
    }

    void getIdealPopupMenuSectionHeaderSizeWithOptions (const juce::String& text, int, int& idealWidth, int& idealHeight,
                                                        const juce::PopupMenu::Options&) override
    {
        idealHeight = menuSectionHeight;
        idealWidth = juce::GlyphArrangement::getStringWidthInt (menuSectionFont(), text.toUpperCase()) + 24;
    }

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        const auto bounds = juce::Rectangle<float> ((float) width, (float) height);

        // Where the window can't be see-through its corners are filled
        // in the darkest ink, as the menu's shadow.
        if (! juce::Desktop::canUseSemiTransparentWindows())
            g.fillAll (IlanaTheme::Ui::header);

        g.setColour (IlanaTheme::Ui::raised);
        g.fillRoundedRectangle (bounds, menuRadius);
        g.setColour (IlanaTheme::Ui::hover);
        g.drawRoundedRectangle (bounds.reduced (0.5f), menuRadius - 0.5f, 1.0f);
    }

    void drawPopupMenuUpDownArrow (juce::Graphics& g, int width, int height, bool isScrollUpArrow) override
    {
        using namespace IlanaTheme;
        g.setGradientFill (juce::ColourGradient (Ui::raised, 0.0f, (float) height * 0.5f,
                                                 Ui::raised.withAlpha (0.0f), 0.0f, isScrollUpArrow ? (float) height : 0.0f, false));
        g.fillRect (1, 1, width - 2, height - 2);

        const auto cx = (float) width * 0.5f;
        const auto cy = (float) height * 0.5f;
        const auto dy = isScrollUpArrow ? -2.0f : 2.0f;
        juce::Path chevron;
        chevron.startNewSubPath (cx - 4.0f, cy - dy);
        chevron.lineTo (cx, cy + dy);
        chevron.lineTo (cx + 4.0f, cy - dy);
        g.setColour (Ui::text3);
        g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour) override
    {
        using namespace IlanaTheme;

        if (isSeparator)
        {
            g.setColour (Ui::line);
            g.fillRect (area.withSizeKeepingCentre (area.getWidth(), 1));
            return;
        }

        const auto row = area.toFloat();
        const auto lit = isHighlighted && isActive;

        if (lit)
        {
            g.setColour (accent().withAlpha (0.15f));
            g.fillRoundedRectangle (row, 6.0f);
        }

        auto colour = textColour != nullptr ? *textColour : Ui::text;
        if (lit)
            colour = accentText();
        if (! isActive)
            colour = Ui::text3;

        auto r = area.reduced (10, 0);
        auto shown = getPopupMenuFont();
        shown = shown.withHeight (juce::jmin (shown.getHeight(), (float) area.getHeight() / 1.3f));

        // The tick column, then an 8 px gap.
        const auto tickArea = r.removeFromLeft (10).toFloat();
        r.removeFromLeft (8);

        if (icon != nullptr)
        {
            icon->drawWithin (g, tickArea.withSizeKeepingCentre (12.0f, 12.0f),
                              juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize, 1.0f);
        }
        else if (isTicked)
        {
            const auto box = tickArea.withSizeKeepingCentre (9.0f, 8.0f);
            juce::Path tick;
            tick.startNewSubPath (box.getX(), box.getCentreY());
            tick.lineTo (box.getX() + box.getWidth() * 0.38f, box.getBottom());
            tick.lineTo (box.getRight(), box.getY());
            g.setColour (isActive ? accent() : Ui::text3);
            g.strokePath (tick, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        if (hasSubMenu)
        {
            // A chevron, as the sheet's "›".
            const auto arrowArea = r.removeFromRight (10).toFloat();
            const auto x = arrowArea.getCentreX() - 1.5f;
            const auto cy = arrowArea.getCentreY();
            juce::Path arrow;
            arrow.startNewSubPath (x, cy - 3.5f);
            arrow.lineTo (x + 3.5f, cy);
            arrow.lineTo (x, cy + 3.5f);
            g.setColour (lit ? accentText() : Ui::text3);
            g.strokePath (arrow, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            r.removeFromRight (6);
        }

        if (shortcutKeyText.isNotEmpty())
        {
            const auto keyFont = shown.withHeight (shown.getHeight() * 0.88f);
            const auto keyWidth = juce::GlyphArrangement::getStringWidthInt (keyFont, shortcutKeyText) + 2;
            g.setColour (Ui::text3);
            g.setFont (keyFont);
            g.drawText (shortcutKeyText, r.removeFromRight (keyWidth), juce::Justification::centredRight, false);
            r.removeFromRight (12);
        }

        g.setColour (colour);
        g.setFont (shown);
        IlanaTheme::drawFitted (g, text, r, juce::Justification::centredLeft, 1);
    }

    void drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName) override
    {
        g.setFont (menuSectionFont());
        g.setColour (IlanaTheme::Ui::text3);
        g.drawText (sectionName.toUpperCase(), area.reduced (10, 0).withTrimmedTop (8).withTrimmedBottom (4),
                    juce::Justification::centredLeft, true);
    }

    //==============================================================================
    // Tooltips: a small menu-like card (ink 3, ink 5 rim, 7 px corners)
    // with body text, wrapped at about 340 px.
    static juce::TextLayout tooltipLayout (const juce::String& text, juce::Colour colour)
    {
        juce::AttributedString s;
        s.setJustification (juce::Justification::centredLeft);
        s.setLineSpacing (1.0f);
        s.append (text.trim(), juce::Font (IlanaTheme::font (IlanaTheme::TextSize::body)), colour);
        juce::TextLayout layout;
        layout.createLayoutWithBalancedLineLengths (s, 340.0f);
        return layout;
    }

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override
    {
        const auto layout = tooltipLayout (tipText, IlanaTheme::Ui::text);
        const auto w = (int) std::ceil (layout.getWidth()) + 22;
        const auto h = (int) std::ceil (layout.getHeight()) + 14;

        return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                     screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6,
                                     w, h)
            .constrainedWithin (parentArea);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override
    {
        using namespace IlanaTheme;
        const auto bounds = juce::Rectangle<float> ((float) width, (float) height);
        g.setColour (Ui::raised);
        g.fillRoundedRectangle (bounds, 7.0f);
        g.setColour (Ui::hover);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 6.5f, 1.0f);
        tooltipLayout (text, Ui::text).draw (g, bounds.reduced (11.0f, 7.0f));
    }

    //==============================================================================
    // Dialogs (the sheet's .dlg): ink 2, 14 px corners, the accent's rim
    // as the save dialog's; a small tinted badge in place of JUCE's icons.
    int getAlertWindowButtonHeight() override { return 32; }
    juce::Font getAlertWindowTitleFont() override { return juce::Font (IlanaTheme::font (IlanaTheme::TextSize::display, true)); }
    juce::Font getAlertWindowMessageFont() override { return juce::Font (IlanaTheme::font (IlanaTheme::TextSize::title)); }
    juce::Font getAlertWindowFont() override { return juce::Font (IlanaTheme::font (IlanaTheme::TextSize::body)); }

    void drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert, const juce::Rectangle<int>& textArea, juce::TextLayout& textLayout) override
    {
        using namespace IlanaTheme;
        juce::ignoreUnused (textArea);
        const auto bounds = alert.getLocalBounds().toFloat();

        if (alert.isOpaque())
            g.fillAll (Ui::header);

        g.setColour (Ui::panel);
        g.fillRoundedRectangle (bounds, 14.0f);
        g.setColour (accent().withAlpha (0.5f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 13.5f, 1.0f);

        auto iconSpace = 0;

        if (alert.getAlertType() != juce::MessageBoxIconType::NoIcon)
        {
            const auto warning = alert.getAlertType() == juce::MessageBoxIconType::WarningIcon;
            const auto tone = warning ? accent() : Ui::text2;
            const auto badge = juce::Rectangle<float> (26.0f, 30.0f, 34.0f, 34.0f);
            g.setColour (tone.withAlpha (0.16f));
            g.fillEllipse (badge);
            g.setColour (tone.withAlpha (0.6f));
            g.drawEllipse (badge.reduced (0.5f), 1.0f);
            g.setColour (tone);
            g.setFont (font (TextSize::large, true));
            g.drawText (warning ? "!" : alert.getAlertType() == juce::MessageBoxIconType::InfoIcon ? "i" : "?",
                        badge, juce::Justification::centred, false);
            iconSpace = 80;
        }

        const juce::Rectangle<int> textBounds (1 + iconSpace, 30, alert.getWidth() - 2,
                                               alert.getHeight() - getAlertWindowButtonHeight() - 20);
        textLayout.draw (g, textBounds.toFloat());
    }

    //==============================================================================
    // Call-out boxes: the menu's card on a soft shadow.
    void drawCallOutBoxBackground (juce::CallOutBox& box, juce::Graphics& g, const juce::Path& path, juce::Image& cachedImage) override
    {
        if (cachedImage.isNull())
        {
            cachedImage = { juce::Image::ARGB, box.getWidth(), box.getHeight(), true };
            juce::Graphics g2 (cachedImage);
            juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 14, { 0, 6 }).drawForPath (g2, path);
        }

        g.setColour (juce::Colours::black);
        g.drawImageAt (cachedImage, 0, 0);
        g.setColour (IlanaTheme::Ui::raised);
        g.fillPath (path);
        g.setColour (IlanaTheme::Ui::hover);
        g.strokePath (path, juce::PathStrokeType (1.0f));
    }

    float getCallOutBoxCornerSize (const juce::CallOutBox&) override { return menuRadius; }

    //==============================================================================
    // A slider's value bubble: the tooltip's card with bold body text.
    void drawBubble (juce::Graphics& g, juce::BubbleComponent&, const juce::Point<float>&, const juce::Rectangle<float>& body) override
    {
        g.setColour (IlanaTheme::Ui::raised);
        g.fillRoundedRectangle (body, 6.0f);
        g.setColour (IlanaTheme::Ui::hover);
        g.drawRoundedRectangle (body.reduced (0.5f), 5.5f, 1.0f);
    }

    juce::Font getSliderPopupFont (juce::Slider&) override { return juce::Font (IlanaTheme::font (IlanaTheme::TextSize::body, true)); }

    //==============================================================================
    // Progress: the slider's 3 px-style track, rounded, filled in the accent.
    void drawProgressBar (juce::Graphics& g, juce::ProgressBar& bar, int width, int height, double progress, const juce::String& textToShow) override
    {
        using namespace IlanaTheme;
        const auto track = juce::Rectangle<float> ((float) width, (float) height).withSizeKeepingCentre ((float) width, juce::jmin ((float) height, 6.0f));
        g.setColour (bar.findColour (juce::ProgressBar::backgroundColourId));
        g.fillRoundedRectangle (track, track.getHeight() * 0.5f);

        if (progress >= 0.0 && progress <= 1.0)
        {
            g.setColour (bar.findColour (juce::ProgressBar::foregroundColourId));
            g.fillRoundedRectangle (track.withWidth (track.getWidth() * (float) progress), track.getHeight() * 0.5f);
        }
        else
        {
            // Unknown progress: a sliding third of the track.
            const auto phase = (float) std::fmod (juce::Time::getMillisecondCounterHiRes() * 0.0008, 1.0);
            const auto seg = track.withWidth (track.getWidth() / 3.0f).withX (track.getX() + (track.getWidth() * 4.0f / 3.0f) * phase - track.getWidth() / 3.0f);
            g.setColour (bar.findColour (juce::ProgressBar::foregroundColourId));
            g.fillRoundedRectangle (seg.getIntersection (track), track.getHeight() * 0.5f);
        }

        if (textToShow.isNotEmpty() && height >= 14)
        {
            g.setColour (Ui::text);
            g.setFont (font (TextSize::label, true));
            g.drawText (textToShow, 0, 0, width, height, juce::Justification::centred, false);
        }
    }

    //==============================================================================
    // Text fields (the sheet's .inp): a well with 8 px corners and a
    // hairline rim, the accent's rim while typing; selection in the
    // accent at 35 %, the caret in the accent.
    static float textFieldRadius (int height) { return juce::jmin (8.0f, (float) height * 0.27f); }

    void fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
    {
        const auto fill = editor.findColour (juce::TextEditor::backgroundColourId);

        if (dynamic_cast<juce::ComboBox*> (editor.getParentComponent()) != nullptr || fill.isTransparent())
        {
            g.fillAll (fill);
            return;
        }

        g.setColour (fill);
        g.fillRoundedRectangle (juce::Rectangle<float> ((float) width, (float) height), textFieldRadius (height));
    }

    void drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
    {
        if (dynamic_cast<juce::ComboBox*> (editor.getParentComponent()) != nullptr)
            return;

        const auto bounds = juce::Rectangle<float> ((float) width, (float) height);
        const auto radius = textFieldRadius (height);
        const auto typing = editor.isEnabled() && editor.hasKeyboardFocus (true) && ! editor.isReadOnly();
        auto rim = editor.findColour (typing ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId);

        if (rim.isTransparent())
            return;

        if (! editor.isEnabled())
            rim = rim.withMultipliedAlpha (0.5f);

        g.setColour (rim);
        g.drawRoundedRectangle (bounds.reduced (typing ? 0.75f : 0.5f), radius, typing ? 1.5f : 1.0f);
    }

    //==============================================================================
    // JUCE's own file browser rows (only where no native chooser exists).
    void drawFileBrowserRow (juce::Graphics& g, int width, int height, const juce::File&, const juce::String& filename, juce::Image* icon,
                             const juce::String& fileSizeDescription, const juce::String& fileTimeDescription,
                             bool isDirectory, bool isItemSelected, int, juce::DirectoryContentsDisplayComponent& dcc) override
    {
        using namespace IlanaTheme;
        auto row = juce::Rectangle<int> (width, height);

        if (isItemSelected)
        {
            g.setColour (accent().withAlpha (0.15f));
            g.fillRoundedRectangle (row.reduced (2, 1).toFloat(), 6.0f);
        }

        juce::ignoreUnused (icon, dcc);
        row.removeFromLeft (10);
        g.setColour (isDirectory ? Ui::text3 : (isItemSelected ? accent() : Ui::text3).withAlpha (0.8f));
        g.fillEllipse (row.removeFromLeft (6).toFloat().withSizeKeepingCentre (6.0f, 6.0f));
        row.removeFromLeft (8);

        g.setFont (font (TextSize::body));
        g.setColour (isItemSelected ? accentText() : Ui::text);
        const auto details = width > 450 && ! isDirectory;
        g.drawFittedText (filename, details ? row.withWidth (row.getWidth() * 3 / 5) : row, juce::Justification::centredLeft, 1);

        if (details)
        {
            g.setColour (Ui::text3);
            g.drawText (fileSizeDescription + "   " + fileTimeDescription, row, juce::Justification::centredRight, true);
        }
    }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override
    {
        auto thumb = isScrollbarVertical ? juce::Rectangle<int> (x, thumbStartPosition, width, thumbSize)
                                         : juce::Rectangle<int> (thumbStartPosition, y, thumbSize, height);
        thumb = isScrollbarVertical ? thumb.reduced (juce::jmax (1, width / 2 - 2), 2) : thumb.reduced (2, juce::jmax (1, height / 2 - 2));

        g.setColour ((isMouseOver || isMouseDown) ? IlanaTheme::Ui::text3 : IlanaTheme::Ui::track);
        g.fillRoundedRectangle (thumb.toFloat(), 2.0f);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearBar)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, 0.0f, 0.0f, style, slider);
            return;
        }

        // The component sheet's slider (.ssl): a 3 px ink 4 track, the fill
        // in its colour (no glow), an 11 px white thumb ringed in the card's ink.
        const auto trackHeight = 3.0f;
        const auto centreY = (float) y + (float) height * 0.5f;
        const juce::Rectangle<float> track ((float) x, centreY - trackHeight * 0.5f, (float) width, trackHeight);

        g.setColour (IlanaTheme::Ui::track);
        g.fillRoundedRectangle (track, trackHeight * 0.5f);

        // Bipolar ranges fill outwards from zero, so a centred value reads as "none".
        const auto bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const auto originX = bipolar ? (float) x + (float) width * (float) slider.valueToProportionOfLength (0.0)
                                     : (float) x;
        const auto thumbPos = juce::jlimit ((float) x, (float) (x + width), sliderPos);
        const auto fill = juce::Rectangle<float>::leftTopRightBottom (juce::jmin (originX, thumbPos), track.getY(),
                                                                     juce::jmax (originX, thumbPos), track.getBottom());

        const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
        const auto hover = animatedHover (&slider, slider.isMouseOver() || slider.isMouseButtonDown(), 12.0f);

        if (fill.getWidth() > 0.5f)
        {
            g.setColour (accent);
            g.fillRoundedRectangle (fill, trackHeight * 0.5f);
        }

        const auto thumbX = juce::jlimit ((float) x, (float) (x + width), sliderPos);
        const auto thumb = juce::Rectangle<float> (11.0f + 2.0f * hover, 11.0f + 2.0f * hover).withCentre ({ thumbX, centreY });

        if (slider.hasKeyboardFocus (false))
        {
            g.setColour (IlanaTheme::accent().withAlpha (0.6f));
            g.drawEllipse (thumb.expanded (4.0f), 1.0f);
        }

        g.setColour (IlanaTheme::Ui::panel);
        g.fillEllipse (thumb.expanded (2.0f));
        g.setColour (juce::Colours::white);
        g.fillEllipse (thumb);

        if (hover > 0.01f && hover < 0.99f)
            slider.repaint();
    }

private:
    // A hover's ease for what the look-and-feel draws, on the shared
    // animator (IlanaTheme::fadeValue); the caller repaints while it moves.
    float animatedHover (const void* key, bool isOver, float rate)
    {
        auto moving = false;
        return IlanaTheme::fadeValue (key, -1, isOver ? 1.0f : 0.0f, rate, true, moving);
    }

    // 1 just after the user moves a knob, easing back to 0 over about half a
    // second; keyed like animatedHover. Preset loads, undo and automation
    // move knobs too but don't flare.
    static float valueFlare (const void* key, float position, bool byUser)
    {
        struct Flare { float last = -1.0f; float value = 0.0f; double time = 0.0; };
        static std::unordered_map<const void*, Flare> flares;
        const auto now = juce::Time::getMillisecondCounterHiRes();

        // Drop entries not painted for a minute once there are many.
        static double lastSweep = 0.0;

        if (flares.size() > 2048 && now - lastSweep > 1000.0)
        {
            lastSweep = now;
            for (auto it = flares.begin(); it != flares.end();)
                it = now - it->second.time > 60000.0 ? flares.erase (it) : std::next (it);
        }

        auto& flare = flares[key];
        const auto dt = flare.time > 0.0 ? juce::jlimit (0.0, 0.1, (now - flare.time) * 0.001) : 0.0;
        flare.time = now;

        if (byUser && flare.last >= 0.0f && std::abs (position - flare.last) > 0.0005f)
            flare.value = 1.0f;
        else
            flare.value = juce::jmax (0.0f, flare.value - (float) dt * 2.2f);

        flare.last = position;
        return flare.value;
    }

    juce::Typeface::Ptr regularTypeface;
    juce::Typeface::Ptr boldTypeface;
};

// Makes one shared IlanaLookAndFeel the process's default while any editor
// is open. A PopupMenu shown on the desktop (a knob's right-click menu, its
// submenus, the header's and the dock's menus), an AlertWindow with no
// owner and JUCE's own file browser never ask their target component for
// a look: they use the default, which was JUCE's stock V4 grey (ilana's
// Windows screenshots). The last editor to close hands the default back.
class DefaultLookAndFeelScope
{
public:
    DefaultLookAndFeelScope()
    {
        ++openScopes();
        juce::LookAndFeel::setDefaultLookAndFeel (&shared.get());
    }

    ~DefaultLookAndFeelScope()
    {
        if (--openScopes() == 0 && &juce::LookAndFeel::getDefaultLookAndFeel() == &shared.get())
        {
            // Nothing may keep drawing with it once it's gone.
            juce::PopupMenu::dismissAllActiveMenus();
            juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        }
    }

    IlanaLookAndFeel& get() { return shared.get(); }

private:
    // (Counted here, on the message thread, as editors open and close.)
    static int& openScopes()
    {
        static int count = 0;
        return count;
    }

    juce::SharedResourcePointer<IlanaLookAndFeel> shared;

    JUCE_DECLARE_NON_COPYABLE (DefaultLookAndFeelScope)
};
