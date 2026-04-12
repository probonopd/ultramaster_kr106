#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "KR106MenuSheet.h"

// ============================================================================
// KR106VarianceSheet — per-voice component variance editor.
// Grid showing 10 voices × 6 variance parameters with drag-to-edit.
// Black background, green LED font, same aesthetic as preset/menu sheets.
// ============================================================================
class KR106VarianceSheet : public juce::Component
{
public:
    static constexpr int kMaxVoices = 10;
    static constexpr int kNumParams = 6;
    // Layout: title, noise row (analog + chorus on one line), header, 10 voices
    static constexpr int kFirstVoiceRow = 3; // title + noise + header
    static constexpr int kTotalRows = kFirstVoiceRow + kMaxVoices;
    static constexpr int kLabelCol = 1;   // voice label column
    static constexpr int kTotalCols = kLabelCol + kNumParams;

    KR106VarianceSheet(KR106AudioProcessor* proc, juce::Typeface::Ptr typeface,
                       std::function<void()> onClose)
        : mProcessor(proc), mTypeface(typeface), mOnClose(std::move(onClose))
    {
        setWantsKeyboardFocus(true);
        readFromVoices();
        if (mProcessor)
        {
            mAnalogNoiseKnob = mulToKnob(mProcessor->mDSP.mNoiseFloorMul);
            mMainsNoiseKnob = mulToKnob(mProcessor->mDSP.mMainsMul);
            mClockNoiseKnob = mulToKnob(mProcessor->mDSP.mChorus.mClockMul);
            // Note: chorus mAnalogMul is always kept in sync with mNoiseFloorMul
        }
    }

    // Cell sizes match the 8×16 preset sheet grid (940/8 = 117w, 224/16 = 14h)
    static constexpr int kColW = 117;
    static constexpr int kRowH = 14;

    int calcWidth() const { return kTotalCols * kColW; }
    int calcHeight() const { return kTotalRows * kRowH + 2; }

    void showAt(juce::Rectangle<int> bounds)
    {
        setBounds(bounds);
        grabKeyboardFocus();
    }

    void paint(juce::Graphics& g) override
    {
        using namespace KR106Theme;
        g.fillAll(bg());
        g.setFont(ledFont(mTypeface));

        int w = getWidth();
        int h = getHeight();
        int activeVoices = mProcessor ? mProcessor->mVoiceCount : 6;

        // Title row
        g.setColour(bright());
        g.drawSingleLineText("COMPONENT VARIANCE",
                             4, kRowH - 2);

        // Tune buttons (right side of title row)
        static constexpr int kBtnW = 90;
        mTunePoorlyRect  = { w - kBtnW * 3, 0, kBtnW, kRowH };
        mTuneLazyRect    = { w - kBtnW * 2, 0, kBtnW, kRowH };
        mTunePerfectRect = { w - kBtnW,     0, kBtnW, kRowH };

        auto drawBtn = [&](juce::Rectangle<int>& r, const char* label, int hoverId) {
            if (mHoverRow == hoverId)
            {
                g.setColour(hoverBg());
                g.fillRect(r);
            }
            g.setColour(bright());
            g.drawSingleLineText(label, r.getX() + 4, r.getBottom() - 2);
        };
        drawBtn(mTunePoorlyRect,  " Out of Tune ",  -2);
        drawBtn(mTuneLazyRect,    " Human Tune ",   -3);
        drawBtn(mTunePerfectRect, " Robot Tune ",   -4);

        // Noise row: 3 controls side by side
        {
            int y = kRowH;
            int cellH = kRowH;
            int cellW = w / 3;

            auto paintNoiseCell = [&](int hitId, int x0, int cw, const char* label, float knobVal) {
                bool isHover = (mHoverRow == hitId);
                bool isSel = (mSelectedRow == hitId);
                if (isSel)
                {
                    g.setColour(bright());
                    g.fillRect(x0 + 1, y + 1, cw - 1, cellH - 1);
                }
                else if (isHover)
                {
                    g.setColour(hoverBg());
                    g.fillRect(x0 + 1, y + 1, cw - 1, cellH - 1);
                }
                g.setColour(isSel ? bg() : bright());
                g.drawSingleLineText(label, x0 + 4, y + cellH - 2);
                int pct = juce::roundToInt(knobVal * 100.f);
                g.drawSingleLineText(juce::String(pct) + " %",
                                     x0 + cw - 8, y + cellH - 2,
                                     juce::Justification::right);
            };
            paintNoiseCell(-5, 0,          cellW,         "ANALOG NOISE", mAnalogNoiseKnob);
            paintNoiseCell(-6, cellW,      cellW,         "MAINS NOISE",  mMainsNoiseKnob);
            paintNoiseCell(-7, cellW * 2,  w - cellW * 2, "CLOCK NOISE",  mClockNoiseKnob);
        }

        // Header row (inverted: bright bg, black text)
        int headerY = 2 * kRowH;
        int headerH = kRowH;
        g.setColour(bright());
        g.fillRect(0, headerY, w, headerH);
        g.setColour(bg());
        g.drawSingleLineText("VOICE", 4, headerY + headerH - 2);
        for (int p = 0; p < kNumParams; p++)
        {
            auto info = kr106::Voice<float>::GetVarianceInfo(p);
            int x = (p + 1) * kColW;
            g.drawSingleLineText(info.name, x + kColW - 8, headerY + headerH - 2,
                                 juce::Justification::right);
        }

        // Grid lines
        g.setColour(grid());
        for (int row = 1; row <= kTotalRows; row++)
        {
            int y = row * kRowH;
            g.drawHorizontalLine(y, 0.f, static_cast<float>(w));
        }
        int voiceGridTop = static_cast<int>(kFirstVoiceRow * kRowH);
        g.drawVerticalLine(kColW, static_cast<float>(kRowH), static_cast<float>(h));
        for (int p = 1; p < kNumParams; p++)
        {
            int x = (p + 1) * kColW;
            g.drawVerticalLine(x, static_cast<float>(voiceGridTop), static_cast<float>(h));
        }

        // Voice rows
        for (int v = 0; v < kMaxVoices; v++)
        {
            int row = v + kFirstVoiceRow;
            int y = row * kRowH;
            int cellH = (row + 1) * kRowH - y;
            bool inactive = v >= activeVoices;

            // Voice label
            static const char* voiceNames[] = {
                "ONE", "TWO", "THREE", "FOUR", "FIVE",
                "SIX", "SEVEN", "EIGHT", "NINE", "TEN"
            };
            g.setColour(inactive ? disabled() : bright());
            g.drawSingleLineText(voiceNames[v], 4, y + cellH - 2);

            // Parameter values
            for (int p = 0; p < kNumParams; p++)
            {
                int x = (p + 1) * kColW;
                bool isHover = (mHoverRow == v && mHoverCol == p);
                bool isSelected = (mSelectedRow == v && mSelectedCol == p);

                if (isSelected)
                {
                    g.setColour(bright());
                    g.fillRect(x + 1, y + 1, kColW - 1, cellH - 1);
                }
                else if (isHover)
                {
                    g.setColour(hoverBg());
                    g.fillRect(x + 1, y + 1, kColW - 1, cellH - 1);
                }

                auto info = kr106::Voice<float>::GetVarianceInfo(p);
                int display = juce::roundToInt(mValues[v][p] * info.displayScale + info.displayOffset);
                juce::String text;
                if (info.displayOffset != 0.f)
                    text = juce::String(display);
                else
                    text = (display >= 0 ? "+" : "") + juce::String(display);
                text += " " + juce::String(info.unit);

                g.setColour(inactive ? disabled() : isSelected ? bg() : bright());
                g.drawSingleLineText(text, x + kColW - 8, y + cellH - 2,
                                     juce::Justification::right);
            }
        }

        g.setColour(border());
        g.drawRect(getLocalBounds());
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        auto [row, col] = hitTest(e.getPosition());
        if (row != mHoverRow || col != mHoverCol)
        {
            mHoverRow = row;
            mHoverCol = col;
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        // Check tune buttons
        if (mTunePoorlyRect.contains(e.getPosition()))  { tuneRandomize(1.f);  return; }
        if (mTuneLazyRect.contains(e.getPosition()))    { tuneRandomize(0.2f); return; }
        if (mTunePerfectRect.contains(e.getPosition())) { tunePerfect();       return; }

        auto [row, col] = hitTest(e.getPosition());
        if (row == -5 || row == -6 || row == -7)
        {
            mSelectedRow = row;
            mSelectedCol = 0;
            mDragStartY = e.y;
            mDragStartValue = noiseKnob(row);
            repaint();
        }
        else if (row >= 0 && row < kMaxVoices && col >= 0 && col < kNumParams)
        {
            mSelectedRow = row;
            mSelectedCol = col;
            mDragStartY = e.y;
            mDragStartValue = mValues[row][col];
            repaint();
        }
        else
        {
            dismiss();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (mSelectedRow == -5 || mSelectedRow == -6 || mSelectedRow == -7)
        {
            float delta = (mDragStartY - e.y) * (1.f / 80.f);
            noiseKnob(mSelectedRow) = std::clamp(mDragStartValue + delta, 0.f, 1.f);
            applyNoise();
            repaint();
            return;
        }
        if (mSelectedRow < 0) return;
        auto info = kr106::Voice<float>::GetVarianceInfo(mSelectedCol);
        float delta = (mDragStartY - e.y) * (info.range / 50.f);
        float newVal = std::clamp(mDragStartValue + delta, -info.range, info.range);
        mValues[mSelectedRow][mSelectedCol] = newVal;
        applyToVoice(mSelectedRow, mSelectedCol);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        // Keep selection active for arrow key editing
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (mHoverRow >= -1 || mHoverCol >= 0)
        {
            mHoverRow = -1;
            mHoverCol = -1;
            repaint();
        }
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            dismiss();
            return true;
        }

        if (mSelectedRow == -5 || mSelectedRow == -6 || mSelectedRow == -7)
        {
            float& knob = noiseKnob(mSelectedRow);
            if (key == juce::KeyPress::upKey)
            {
                knob = std::clamp(knob + 0.01f, 0.f, 1.f);
                applyNoise();
                repaint();
                return true;
            }
            if (key == juce::KeyPress::downKey)
            {
                knob = std::clamp(knob - 0.01f, 0.f, 1.f);
                applyNoise();
                repaint();
                return true;
            }
            return false;
        }

        if (mSelectedRow < 0) return false;

        auto info = kr106::Voice<float>::GetVarianceInfo(mSelectedCol);

        if (key == juce::KeyPress::upKey)
        {
            mValues[mSelectedRow][mSelectedCol] = std::clamp(
                mValues[mSelectedRow][mSelectedCol] + info.step,
                -info.range, info.range);
            applyToVoice(mSelectedRow, mSelectedCol);
            repaint();
            return true;
        }
        if (key == juce::KeyPress::downKey)
        {
            mValues[mSelectedRow][mSelectedCol] = std::clamp(
                mValues[mSelectedRow][mSelectedCol] - info.step,
                -info.range, info.range);
            applyToVoice(mSelectedRow, mSelectedCol);
            repaint();
            return true;
        }
        if (key == juce::KeyPress::leftKey)
        {
            mSelectedCol = std::max(0, mSelectedCol - 1);
            repaint();
            return true;
        }
        if (key == juce::KeyPress::rightKey)
        {
            mSelectedCol = std::min(kNumParams - 1, mSelectedCol + 1);
            repaint();
            return true;
        }

        return false;
    }

    void focusLost(FocusChangeType) override
    {
        dismiss();
    }

private:
    void readFromVoices()
    {
        if (!mProcessor) return;
        for (int v = 0; v < kMaxVoices; v++)
        {
            auto* voice = mProcessor->mDSP.GetVoice(v);
            for (int p = 0; p < kNumParams; p++)
                mValues[v][p] = voice->GetVariance(p);
        }
    }

    void applyToVoice(int v, int p)
    {
        if (!mProcessor) return;
        auto* voice = mProcessor->mDSP.GetVoice(v);
        voice->SetVariance(p, mValues[v][p]);
    }

    void tunePerfect()
    {
        for (int v = 0; v < kMaxVoices; v++)
        {
            for (int p = 0; p < kNumParams; p++)
            {
                mValues[v][p] = 0.f;
                applyToVoice(v, p);
            }
        }
        mAnalogNoiseKnob = 0.f;
        mMainsNoiseKnob = 0.f;
        mClockNoiseKnob = 0.f;
        applyNoise();
        repaint();
    }

    void tuneRandomize(float scale)
    {
        // Use current time as seed for a new "hardware unit"
        uint32_t baseSeed = static_cast<uint32_t>(juce::Time::currentTimeMillis());
        for (int v = 0; v < kMaxVoices; v++)
        {
            uint32_t seed = baseSeed + static_cast<uint32_t>(v) * 2654435761u;
            auto rng = [&seed]() -> float {
                seed = seed * 196314165u + 907633515u;
                return static_cast<float>(seed) / static_cast<float>(0xFFFFFFFF) * 2.f - 1.f;
            };
            for (int p = 0; p < kNumParams; p++)
            {
                auto info = kr106::Voice<float>::GetVarianceInfo(p);
                mValues[v][p] = rng() * info.range * scale;
            }
            for (int p = 0; p < kNumParams; p++)
                applyToVoice(v, p);
        }
        repaint();
    }

    std::pair<int, int> hitTest(juce::Point<int> pos) const
    {
        if (pos.y < kRowH)
        {
            if (mTunePoorlyRect.contains(pos))  return { -2, -1 };
            if (mTuneLazyRect.contains(pos))    return { -3, -1 };
            if (mTunePerfectRect.contains(pos)) return { -4, -1 };
        }

        // Noise row: y = 1*kRowH to 2*kRowH, three cells
        if (pos.y >= kRowH && pos.y < 2 * kRowH)
        {
            int cellW = getWidth() / 3;
            if (pos.x < cellW)      return { -5, 0 };
            if (pos.x < cellW * 2)  return { -6, 0 };
            return { -7, 0 };
        }

        int row = pos.y / kRowH - kFirstVoiceRow;
        int col = pos.x / kColW - 1;

        if (row < 0 || row >= kMaxVoices || col < 0 || col >= kNumParams)
            return { -1, -1 };

        return { row, col };
    }

    // Noise floor taper: 0..1 knob -> multiplier.
    // 0 = silent, 0.5 = unity, 1 = 4x.
    static float knobToMul(float knob)
    {
        if (knob <= 0.f) return 0.f;
        return powf(4.f, 2.f * knob - 1.f); // 0.25..1..4
    }

    static float mulToKnob(float mul)
    {
        if (mul <= 0.f) return 0.f;
        return (log2f(mul) / 2.f + 1.f) * 0.5f; // inverse of knobToMul
    }

    float& noiseKnob(int hitId)
    {
        if (hitId == -6) return mMainsNoiseKnob;
        if (hitId == -7) return mClockNoiseKnob;
        return mAnalogNoiseKnob; // -5
    }

    void applyNoise()
    {
        if (!mProcessor) return;
        float analogMul = knobToMul(mAnalogNoiseKnob);
        mProcessor->mDSP.mNoiseFloorMul = analogMul;
        mProcessor->mDSP.mChorus.mAnalogMul = analogMul;
        mProcessor->mDSP.mMainsMul = knobToMul(mMainsNoiseKnob);
        mProcessor->mDSP.mChorus.mMainsMul = knobToMul(mMainsNoiseKnob);
        mProcessor->mDSP.mChorus.mClockMul = knobToMul(mClockNoiseKnob);
    }

    void dismiss()
    {
        if (mOnClose) mOnClose();
    }

    KR106AudioProcessor* mProcessor = nullptr;
    juce::Typeface::Ptr mTypeface;
    std::function<void()> mOnClose;

    float mAnalogNoiseKnob = 0.5f;  // 0..1, 0.5 = unity
    float mMainsNoiseKnob = 0.5f;
    float mClockNoiseKnob = 0.5f;
    float mValues[kMaxVoices][kNumParams] = {};
    int mHoverRow = -1, mHoverCol = -1;
    int mSelectedRow = -1, mSelectedCol = -1;
    float mDragStartY = 0.f;
    float mDragStartValue = 0.f;
    juce::Rectangle<int> mTunePoorlyRect, mTuneLazyRect, mTunePerfectRect;
};
