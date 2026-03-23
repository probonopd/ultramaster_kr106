#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"
#include <functional>
#include <vector>

// ============================================================================
// KR106Theme — shared LED menu drawing constants and helpers
// ============================================================================
namespace KR106Theme
{
    inline constexpr int kTextOffset = 2;  // px up from cell bottom for text baseline

    inline juce::Colour bg()        { return juce::Colour(0, 0, 0); }
    inline juce::Colour bright()    { return juce::Colour(0, 255, 0); }
    inline juce::Colour dim()       { return juce::Colour(0, 160, 0); }
    inline juce::Colour disabled()  { return juce::Colour(0, 60, 0); }
    inline juce::Colour hoverBg()   { return juce::Colour(0, 60, 0); }
    inline juce::Colour grid()      { return juce::Colour(0, 40, 0); }
    inline juce::Colour border()    { return juce::Colour(0, 100, 0); }

    inline juce::Font ledFont(juce::Typeface::Ptr tf)
    {
        return juce::Font(juce::FontOptions(tf)
            .withMetricsKind(juce::TypefaceMetricsKind::legacy)).withHeight(8.f);
    }

    // Draw a single cell: hover fill + coloured text at baseline
    inline void drawCell(juce::Graphics& g, const juce::String& text,
                         int x, int y, int w, int h, bool hover, bool active)
    {
        if (active)
        {
            g.setColour(bright());
            g.fillRect(x, y, w, h);
            g.setColour(bg());
        }
        else
        {
            if (hover)
            {
                g.setColour(hoverBg());
                g.fillRect(x, y, w, h);
            }
            g.setColour(bright());
        }
        g.drawSingleLineText(text, x + 4, y + h - kTextOffset);
    }
}

// ============================================================================
// KR106MenuSheet — custom popup menu with black background and green LED font.
// A transparent backdrop covers the parent to catch outside clicks.
// Click item to select, click outside or Escape to dismiss.
// ============================================================================

struct KR106MenuItem
{
    int id = 0;
    juce::String text;
    bool enabled = true;
    bool ticked = false;
    bool separator = false;

    static KR106MenuItem item(int id, const juce::String& text, bool enabled = true, bool ticked = false)
    {
        return { id, text, enabled, ticked, false };
    }

    static KR106MenuItem sep()
    {
        return { 0, {}, false, false, true };
    }
};

class KR106MenuSheet : public juce::Component
{
public:
    static constexpr int kRowH = 14;
    static constexpr int kSepH = 1;
    static constexpr int kPadX = 6;
    static constexpr int kPadY = 2;
    static constexpr int kColW = 180;

    KR106MenuSheet(std::vector<KR106MenuItem> items,
                   juce::Typeface::Ptr typeface,
                   std::function<void(int)> onSelect)
        : mItems(std::move(items)), mTypeface(typeface), mOnSelect(std::move(onSelect))
    {
        setInterceptsMouseClicks(true, true);
        setWantsKeyboardFocus(true);
    }

    // Call after adding to parent. Resizes this component to fill the parent,
    // then positions the menu panel at the given bounds within the parent.
    void showAt(juce::Rectangle<int> menuBounds)
    {
        auto* parent = getParentComponent();
        if (!parent) return;
        setBounds(parent->getLocalBounds());
        mMenuBounds = menuBounds;
        setAlwaysOnTop(true);
        setVisible(true);
        grabKeyboardFocus();
    }

    int calcHeight() const
    {
        int h = kPadY * 2;
        for (auto& item : mItems)
            h += item.separator ? kSepH : kRowH;
        return h;
    }

    bool hasAnyTicked() const
    {
        for (auto& item : mItems)
            if (item.ticked) return true;
        return false;
    }

    int calcWidth() const
    {
        auto font = juce::Font(juce::FontOptions(mTypeface)
            .withMetricsKind(juce::TypefaceMetricsKind::legacy)).withHeight(8.f);
        bool ticked = hasAnyTicked();
        int maxW = 0;
        for (auto& item : mItems)
        {
            if (item.separator) continue;
            juce::String label = ticked ? ("* " + item.text) : item.text;
            juce::GlyphArrangement glyphs;
            glyphs.addLineOfText(font, label, 0.f, 0.f);
            int w = (int)std::ceil(glyphs.getBoundingBox(0, -1, false).getWidth());
            maxW = juce::jmax(maxW, w);
        }
        return maxW + kPadX * 2;
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(KR106Theme::bg());
        g.fillRect(mMenuBounds);
        g.setFont(KR106Theme::ledFont(mTypeface));

        bool anyTicked = hasAnyTicked();
        int y = mMenuBounds.getY() + kPadY;
        for (int i = 0; i < (int)mItems.size(); i++)
        {
            auto& item = mItems[i];

            if (item.separator)
            {
                g.setColour(KR106Theme::border());
                g.drawHorizontalLine(y, static_cast<float>(mMenuBounds.getX()),
                                     static_cast<float>(mMenuBounds.getRight()));
                y += kSepH;
                continue;
            }

            bool hover = (i == mHoverIndex && item.enabled);
            juce::String label = anyTicked ? (item.ticked ? ("* " + item.text) : ("  " + item.text)) : item.text;

            if (!item.enabled)
            {
                g.setColour(KR106Theme::disabled());
                g.drawSingleLineText(label, mMenuBounds.getX() + kPadX, y + kRowH - KR106Theme::kTextOffset);
            }
            else
            {
                KR106Theme::drawCell(g, label, mMenuBounds.getX(), y,
                                     mMenuBounds.getWidth(), kRowH, hover, false);
            }

            y += kRowH;
        }

        g.setColour(KR106Theme::border());
        g.drawRect(mMenuBounds);
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        int idx = itemAtPos(e.getPosition());
        if (idx != mHoverIndex)
        {
            mHoverIndex = idx;
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        int idx = itemAtPos(e.getPosition());
        if (idx >= 0 && idx < (int)mItems.size() && mItems[idx].enabled && !mItems[idx].separator)
        {
            int id = mItems[idx].id;
            setVisible(false);
            auto cb = std::move(mOnSelect);
            mOnSelect = nullptr;
            if (cb) cb(id);
            return;
        }
        // Click outside menu or on separator/disabled item — dismiss
        dismiss();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (mHoverIndex >= 0)
        {
            mHoverIndex = -1;
            repaint();
        }
    }


private:
    int itemAtPos(juce::Point<int> pos) const
    {
        if (!mMenuBounds.contains(pos)) return -1;
        int py = pos.y - mMenuBounds.getY() - kPadY;
        if (py < 0) return -1;
        int y = 0;
        for (int i = 0; i < (int)mItems.size(); i++)
        {
            int h = mItems[i].separator ? kSepH : kRowH;
            if (py >= y && py < y + h)
                return i;
            y += h;
        }
        return -1;
    }

    void dismiss()
    {
        setVisible(false);
        auto cb = std::move(mOnSelect);
        mOnSelect = nullptr;
        if (cb) cb(0);
    }

    std::vector<KR106MenuItem> mItems;
    juce::Typeface::Ptr mTypeface;
    std::function<void(int)> mOnSelect;
    juce::Rectangle<int> mMenuBounds;
    int mHoverIndex = -1;
};
