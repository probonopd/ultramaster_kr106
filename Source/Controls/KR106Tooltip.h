#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

// Lightweight tooltip overlay — shows formatted parameter value during drag.
// Owned by KR106Editor, passed to sliders/knobs as a raw pointer.
class KR106Tooltip : public juce::Component
{
public:
    KR106Tooltip() { setInterceptsMouseClicks(false, false); }

    void setLine2(const juce::String& line2) { mLine2 = line2; }

    void show(juce::RangedAudioParameter* param, juce::Component* source)
    {
        if (!param || !source || !getParentComponent()) return;
        mParam = param;

        updateText();
        positionBelow(source);
        setVisible(true);
        repaint();
    }

    void update()
    {
        if (!mParam || !isVisible()) return;
        updateText();
        // Resize width to fit text
        int tw = std::max(textWidth(mText), textWidth(mLine2)) + 10;
        setBounds(getX() + (getWidth() - tw) / 2, getY(), tw, getHeight());
        repaint();
    }

    // Show a fixed text string (no param tracking)
    void showText(const juce::String& text, juce::Component* source)
    {
        if (!source || !getParentComponent()) return;
        mParam = nullptr;
        mText = text;
        positionBelow(source);
        setVisible(true);
        repaint();
    }

    // Update text in-place (tooltip stays where it is)
    void setText(const juce::String& text)
    {
        mParam = nullptr;
        mText = text;
        int tw = std::max(textWidth(mText), textWidth(mLine2)) + 10;
        setBounds(getX() + (getWidth() - tw) / 2, getY(), tw, getHeight());
        repaint();
    }

    void hide()
    {
        setVisible(false);
        mParam = nullptr;
        mLine2 = {};
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(juce::Colour(0, 0, 0));
        g.fillRect(getLocalBounds());
        g.setColour(juce::Colour(128, 128, 128));
        g.drawRect(getLocalBounds());
        g.setColour(juce::Colour(255, 255, 255));
        g.setFont(mFont);
        if (mLine2.isEmpty())
        {
            g.drawText(mText, getLocalBounds(), juce::Justification::centred);
        }
        else
        {
            int half = getHeight() / 2;
            g.drawText(mText, 0, 0, getWidth(), half, juce::Justification::centred);
            g.setColour(juce::Colour(160, 160, 160));
            g.drawText(mLine2, 0, half, getWidth(), half, juce::Justification::centred);
        }
    }

private:
    void positionBelow(juce::Component* source)
    {
        auto srcBounds = getParentComponent()->getLocalArea(source->getParentComponent(),
                                                             source->getBounds());
        int tw = std::max(textWidth(mText), textWidth(mLine2)) + 10;
        int th = mLine2.isEmpty() ? 16 : 28;
        int tx = srcBounds.getCentreX() - tw / 2;
        int ty = srcBounds.getBottom() + 2;
        auto pb = getParentComponent()->getLocalBounds();
        tx = juce::jlimit(0, pb.getWidth() - tw, tx);
        ty = juce::jmin(pb.getHeight() - th, ty);
        setBounds(tx, ty, tw, th);
    }

    void updateText()
    {
        if (mParam)
            mText = mParam->getCurrentValueAsText();
    }

    int textWidth(const juce::String& s) const
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText(mFont, s, 0.f, 0.f);
        return juce::roundToInt(glyphs.getBoundingBox(0, glyphs.getNumGlyphs(), false).getWidth());
    }

    juce::RangedAudioParameter* mParam = nullptr;
    juce::String mText;
    juce::String mLine2;
    juce::Font mFont{juce::FontOptions(11.f)};
};
