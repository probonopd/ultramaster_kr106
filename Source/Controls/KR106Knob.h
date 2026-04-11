#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "KR106Tooltip.h"
#include "PluginProcessor.h"

// ============================================================================
// KR106Knob — Bitmap knob with manhattan distance drag.
// Sprite sheet is @2x, N frames in a horizontal strip.
// Frame count is auto-detected: numFrames = imageWidth / imageHeight.
// Both vertical and horizontal mouse movement contribute to the value.
// Up and right increase; down and left decrease.
// ============================================================================
class KR106Knob : public juce::Component
{
public:
    KR106Knob(juce::RangedAudioParameter* param, const juce::Image& spriteSheet,
              KR106Tooltip* tip = nullptr, int numFrames = 0)
        : mParam(param)
        , mSpriteSheet(spriteSheet)
        , mTooltip(tip)
        , mNumFrames(numFrames > 0 ? numFrames
                     : (spriteSheet.getHeight() > 0 ? spriteSheet.getWidth() / spriteSheet.getHeight() : 32))
    {
    }

    void setBackgroundImage(const juce::Image& img) { mBgImage = img; }

    void setMidiLearn(KR106AudioProcessor* proc, int paramIdx)
    { mProcessor = proc; mParamIdx = paramIdx; }

    void paint(juce::Graphics& g) override
    {
        float val = mParam ? mParam->getValue() : 0.f;
        int maxIdx = mNumFrames - 1;
        int idx = (int)std::round(val * (float)maxIdx);
        idx = juce::jlimit(0, maxIdx, idx);

        int frameW2x = mSpriteSheet.getWidth() / mNumFrames;
        int h2x = mSpriteSheet.getHeight();
        float frameW = frameW2x / 2.f;
        float frameH = h2x / 2.f;

        float w = (float)getWidth();
        float h = (float)getHeight();

        if (mBgImage.isValid())
        {
            g.drawImage(mBgImage,
                        0.f, 0.f, w, h,
                        0, 0, mBgImage.getWidth(), mBgImage.getHeight());
        }

        // Centre sprite within bounds
        float ox = (w - frameW) * 0.5f;
        float oy = (h - frameH) * 0.5f;
        g.drawImage(mSpriteSheet,
                    ox, oy, frameW, frameH,                        // dest: 1x
                    idx * frameW2x, 0, frameW2x, h2x);            // src: 2x

        // Green border when in MIDI learn mode
        if (mProcessor && mParamIdx >= 0
            && mProcessor->mMidiLearnParam.load(std::memory_order_relaxed) == mParamIdx)
        {
            g.setColour(juce::Colour(0, 255, 0));
            g.drawRect(getLocalBounds(), 1);
        }
    }

    void mouseEnter(const juce::MouseEvent&) override
    {
        if (mTooltip && !mDragging && !mEditor)
        {
            updateCCLine();
            mTooltip->show(mParam, this);
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (mTooltip && !mDragging) mTooltip->hide();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        applyCursorWorkaround(e);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && mProcessor && mParamIdx >= 0)
        {
            mProcessor->startMidiLearn(mParamIdx);
            updateCCLine();
            if (mTooltip) mTooltip->show(mParam, this);
            repaint();
            return;
        }
        if (e.mods.isPopupMenu()) return;
        // Left click cancels MIDI learn if active, then falls through to start drag
        if (mProcessor && mProcessor->mMidiLearnParam.load(std::memory_order_relaxed) >= 0)
        {
            mProcessor->cancelMidiLearn();
            if (mTooltip) mTooltip->hide();
            repaint();
        }
        mDragging = true;
        mAccumVal = mParam ? mParam->getValue() : 0.f;
        mLastRawDelta = 0.f;
        if (mParam) mParam->beginChangeGesture();
        if (mTooltip) mTooltip->show(mParam, this);
        setMouseCursor(juce::MouseCursor::NoCursor);
        e.source.enableUnboundedMouseMovement(true);
        if (isAppleHost()) mCursorDirty = true;
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        applyCursorWorkaround(e);
        if (!mParam) return;

        float offsetY = (float)e.getOffsetFromDragStart().y;
        float offsetX = (float)e.getOffsetFromDragStart().x;

        // Manhattan drag: -(dY - dX) so up+right = increase
        float rawDelta = -(offsetY - offsetX);
        float increment = rawDelta - mLastRawDelta;
        mLastRawDelta = rawDelta;

        // Cumulative gearing: shift only affects new movement
        // Scale by display DPI so non-retina screens get the same physical throw
        float dpiScale = std::max(1.f, static_cast<float>(getTopLevelComponent()->getDesktopScaleFactor()));
        float gearing = 127.f;
        if (e.mods.isCommandDown()) gearing *= 100.f;
        else if (e.mods.isShiftDown()) gearing *= 10.f;
        gearing /= dpiScale;
        mAccumVal = juce::jlimit(0.f, 1.f, mAccumVal + increment / gearing);
        float newVal = mAccumVal;

        mParam->setValueNotifyingHost(newVal);
        if (mTooltip) mTooltip->update();
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& /*e*/) override
    {
        if (!mParam) return;
        if (mEditor) dismissEdit();
        if (mTooltip) mTooltip->hide();

        auto* parent = mTooltip ? mTooltip->getParentComponent() : getTopLevelComponent();
        if (!parent) return;

        mEditor = std::make_unique<juce::TextEditor>();
        mEditor->setLookAndFeel(&getEditBoxLnF());
        mEditor->setFont(juce::FontOptions(11.f));
        mEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0, 0, 0));
        mEditor->setColour(juce::TextEditor::textColourId, juce::Colour(255, 255, 255));
        mEditor->setColour(juce::TextEditor::outlineColourId, juce::Colour(128, 128, 128));
        mEditor->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(128, 128, 128));
        mEditor->setColour(juce::TextEditor::highlightColourId, juce::Colour(80, 80, 80));
        mEditor->setJustification(juce::Justification::centred);

        juce::String displayText = mParam->getCurrentValueAsText();
        mEditor->setText(displayText, false);

        auto srcBounds = parent->getLocalArea(getParentComponent(), getBounds());
        juce::Font font{juce::FontOptions(11.f)};
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText(font, displayText, 0.f, 0.f);
        int tw = std::max(50, juce::roundToInt(glyphs.getBoundingBox(0, -1, false).getWidth()) + 16);
        int th = 16;
        int tx = srcBounds.getCentreX() - tw / 2;
        int ty = srcBounds.getBottom() + 2;
        auto pb = parent->getLocalBounds();
        tx = juce::jlimit(0, pb.getWidth() - tw, tx);
        ty = juce::jmin(pb.getHeight() - th, ty);
        mEditor->setBounds(tx, ty, tw, th);

        parent->addAndMakeVisible(mEditor.get());
        mEditor->grabKeyboardFocus();

        int numEnd = findNumericEnd(displayText);
        mEditor->setHighlightedRegion(juce::Range<int>(0, numEnd));

        mEditor->onReturnKey = [this]() { commitEdit(); };
        mEditor->onEscapeKey = [this]() { dismissEdit(); };
        mEditor->onFocusLost = [this]() { dismissEdit(); };
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (!mDragging) return;
        mDragging = false;
        if (mParam) mParam->endChangeGesture();
        if (mTooltip) mTooltip->hide();
        setMouseCursor(juce::MouseCursor::NormalCursor);
        e.source.enableUnboundedMouseMovement(false);

        // Warp cursor to centre of knob so it appears on the control after release
        auto screenPos = localPointToGlobal(juce::Point<int>(getWidth() / 2, getHeight() / 2));
        juce::Desktop::getInstance().getMainMouseSource().setScreenPosition(screenPos.toFloat());
        if (isAppleHost()) mCursorDirty = true;
    }

    void updateCCLine()
    {
        if (!mTooltip) return;
        if (!mProcessor || mParamIdx < 0) { mTooltip->setLine2({}); return; }
        if (mProcessor->mMidiLearnParam.load(std::memory_order_relaxed) == mParamIdx)
        {
            int cc = mProcessor->getCCForParam(mParamIdx);
            mTooltip->setLine2(cc >= 0 ? "MIDI LEARN (CC " + juce::String(cc) + ")" : "MIDI LEARN");
            return;
        }
        int cc = mProcessor->getCCForParam(mParamIdx);
        mTooltip->setLine2(cc >= 0 ? "CC " + juce::String(cc) : "CC ??");
    }

private:
    void commitEdit()
    {
        if (!mEditor || !mParam) return;
        juce::String raw = mEditor->getText().trim();
        if (raw.isEmpty()) { dismissEdit(); return; }
        float normalized = juce::jlimit(0.f, 1.f, mParam->getValueForText(raw));
        mParam->beginChangeGesture();
        mParam->setValueNotifyingHost(normalized);
        mParam->endChangeGesture();
        repaint();
        dismissEdit();
    }

    void dismissEdit()
    {
        if (!mEditor) return;
        auto editor = std::move(mEditor);
        editor->setLookAndFeel(nullptr);
        editor->onFocusLost = nullptr;
        if (auto* parent = editor->getParentComponent())
            parent->removeChildComponent(editor.get());
    }

    struct EditBoxLnF : juce::LookAndFeel_V4
    {
        void drawTextEditorOutline(juce::Graphics& g, int w, int h, juce::TextEditor&) override
        {
            g.setColour(juce::Colour(128, 128, 128));
            g.drawRect(0, 0, w, h, 1);
        }
    };
    static EditBoxLnF& getEditBoxLnF() { static EditBoxLnF lnf; return lnf; }

    static int findNumericEnd(const juce::String& text)
    {
        int i = 0, len = text.length();
        if (i < len && (text[i] == '+' || text[i] == '-')) i++;
        bool hasDigit = false;
        while (i < len && (juce::CharacterFunctions::isDigit(text[i]) || text[i] == '.'))
        { if (text[i] != '.') hasDigit = true; i++; }
        return hasDigit ? i : len;
    }

    juce::RangedAudioParameter* mParam = nullptr;
    juce::Image mSpriteSheet;
    KR106Tooltip* mTooltip = nullptr;
    KR106AudioProcessor* mProcessor = nullptr;
    int mParamIdx = -1;
    int mNumFrames = 32;
    juce::Image mBgImage;
    float mAccumVal = 0.f;
    float mLastRawDelta = 0.f;
    bool mDragging = false;
    bool mCursorDirty = false;
    std::unique_ptr<juce::TextEditor> mEditor;

    void applyCursorWorkaround(const juce::MouseEvent& e)
    {
        if (!mCursorDirty) return;
        mCursorDirty = false;
        if (auto* source = juce::Desktop::getInstance().getMouseSource(e.source.getIndex()))
        {
            source->showMouseCursor(getMouseCursor());
            source->forceMouseCursorUpdate();
        }
    }
};
