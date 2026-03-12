#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include <algorithm>
#include <cmath>

// ============================================================================
// KR106Scope -- oscilloscope display (green waveform on black)
// Triggered by oscillator sync pulse; reads from processor's ring buffer
// Port of KR106ScopeControl from iPlug2
// ============================================================================
class KR106Scope : public juce::Component
{
public:
    static constexpr int RING_SIZE = 4096;

    KR106Scope(KR106AudioProcessor* processor) : mProcessor(processor)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void paint(juce::Graphics& g) override
    {
        int w = getWidth();
        int h = getHeight();

        const auto black  = juce::Colour(0, 0, 0);
        const auto dim    = juce::Colour(0, 128, 0);
        const auto mid    = juce::Colour(0, 192, 0);
        const auto bright = juce::Colour(0, 255, 0);

        // Black background
        g.setColour(black);
        g.fillRect(0, 0, w, h);

        // Check power
        if (mProcessor && mProcessor->getParam(kPower)->getValue() <= 0.5f)
            return;

        if (mScaleIdx < 3)
            paintWaveform(g, w, h, black, dim, mid, bright);
        else
            paintADSR(g, w, h, dim, mid, bright);
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        mScaleIdx = (mScaleIdx + 1) % 4; // 0-2: waveform scales, 3: ADSR
        repaint();
    }

    // Call from editor's timer callback (~30 Hz) to pull data from processor
    void updateFromProcessor()
    {
        if (!mProcessor) return;

        // Read processor's write position (acquire ordering)
        int writePos = mProcessor->mScopeWritePos.load(std::memory_order_acquire);

        // Calculate how many new samples are available
        int newSamples = (writePos - mLocalReadPos + KR106AudioProcessor::kScopeRingSize)
                         % KR106AudioProcessor::kScopeRingSize;
        if (newSamples == 0)
        {
            // ADSR mode reads slider params directly — repaint even with no audio
            if (mScaleIdx == 3) repaint();
            return;
        }

        // Copy new samples into local ring buffer
        float peak = 0.f;
        for (int i = 0; i < newSamples; i++)
        {
            int srcIdx = (mLocalReadPos + i) % KR106AudioProcessor::kScopeRingSize;
            float s = mProcessor->mScopeRing[srcIdx];
            float absS = s < 0.f ? -s : s;
            if (absS > peak) peak = absS;

            mRing[mRingWritePos]     = s;
            mRingR[mRingWritePos]    = mProcessor->mScopeRingR[srcIdx];
            mSyncRing[mRingWritePos] = mProcessor->mScopeSyncRing[srcIdx];
            mRingWritePos = (mRingWritePos + 1) % RING_SIZE;
        }
        mLocalReadPos = writePos;
        mSamplesAvail += newSamples;
        if (mSamplesAvail > RING_SIZE) mSamplesAvail = RING_SIZE;

        // If audio is silent, clear display
        if (peak < 1e-6f)
        {
            mHasData = false;
            repaint();
            return;
        }

        // Search backwards for two consecutive sync pulses -- the samples
        // between them are one full sub-oscillator period (two DCO cycles)
        int endDist = -1, startDist = -1;
        for (int s = 1; s <= mSamplesAvail; s++)
        {
            int idx = (mRingWritePos - s + RING_SIZE) % RING_SIZE;
            if (mSyncRing[idx] > 0.f)
            {
                if (endDist < 0)
                    endDist = s;
                else
                {
                    startDist = s;
                    break;
                }
            }
        }

        if (startDist > 0 && endDist > 0)
        {
            int period = startDist - endDist;
            if (period > 1)
            {
                mDisplayLen = period;
                int startIdx = (mRingWritePos - startDist + RING_SIZE) % RING_SIZE;
                for (int i = 0; i < period; i++)
                {
                    int idx = (startIdx + i) % RING_SIZE;
                    mDisplay[i]  = mRing[idx];
                    mDisplayR[i] = mRingR[idx];
                }
                mHasData = true;
            }
        }

        repaint();
    }

private:
    // ---- Waveform display ----
    void paintWaveform(juce::Graphics& g, int w, int h,
                       juce::Colour /*black*/, juce::Colour dim, juce::Colour mid, juce::Colour bright)
    {
        int v2 = h / 2;
        float scale = kScales[mScaleIdx];

        // Vertical: 3 interior lines (skip edges)
        g.setColour(dim);
        for (int i = 1; i <= 3; i++)
        {
            float x = std::round(static_cast<float>(i) / 4.f * (w - 1));
            g.fillRect(x, 0.f, 1.f, static_cast<float>(h));
        }

        // Horizontal: one line per 0.5 amplitude units (skip edge lines)
        int numSteps = static_cast<int>(scale / 0.5f);
        for (int i = -numSteps + 1; i <= numSteps - 1; i++)
        {
            float y = std::round((i * 0.5f / scale) * -v2 + v2);
            g.setColour(i == 0 ? mid : dim);
            g.fillRect(0.f, y, static_cast<float>(w), 1.f);
        }

        // Waveform -- one full period interpolated to fill the display width
        if (mHasData && mDisplayLen > 1)
        {
            // R channel (dimmer, drawn first so L overlays it)
            g.setColour(dim);
            int lastYR = static_cast<int>((mDisplayR[0] / scale) * -v2 + v2);
            for (int i = 0; i < w; i++)
            {
                float pos = static_cast<float>(i) / static_cast<float>(w) * mDisplayLen;
                int s0 = static_cast<int>(pos);
                float frac = pos - s0;
                if (s0 >= mDisplayLen - 1) { s0 = mDisplayLen - 2; frac = 1.f; }

                float sample = mDisplayR[s0] + frac * (mDisplayR[s0 + 1] - mDisplayR[s0]);
                int y = static_cast<int>((sample / scale) * -v2 + v2);

                int y1 = std::min(lastYR, y);
                int y2 = std::max(lastYR, y) + 1;
                g.fillRect(static_cast<float>(i), static_cast<float>(y1),
                           1.f, static_cast<float>(y2 - y1));
                lastYR = y;
            }

            // L channel (bright, on top)
            g.setColour(bright);
            int lastY = static_cast<int>((mDisplay[0] / scale) * -v2 + v2);
            for (int i = 0; i < w; i++)
            {
                float pos = static_cast<float>(i) / static_cast<float>(w) * mDisplayLen;
                int s0 = static_cast<int>(pos);
                float frac = pos - s0;
                if (s0 >= mDisplayLen - 1) { s0 = mDisplayLen - 2; frac = 1.f; }

                float sample = mDisplay[s0] + frac * (mDisplay[s0 + 1] - mDisplay[s0]);
                int y = static_cast<int>((sample / scale) * -v2 + v2);

                int y1 = std::min(lastY, y);
                int y2 = std::max(lastY, y) + 1;
                g.fillRect(static_cast<float>(i), static_cast<float>(y1),
                           1.f, static_cast<float>(y2 - y1));
                lastY = y;
            }
        }
    }

    // ---- ADSR envelope display (split view) ----
    // Top half: ADS (attack + decay + sustain fill)
    // Bottom half: SR (sustain fill + release curve)
    // Both halves use a fixed 15s time window.
    void paintADSR(juce::Graphics& g, int w, int h,
                   juce::Colour dim, juce::Colour mid, juce::Colour bright)
    {
        if (!mProcessor) return;

        using DSP = KR106DSP<float>;
        auto& dsp = mProcessor->mDSP;

        // --- Read ADSR parameters ---
        bool j6 = (dsp.mAdsrMode == 0);
        float sustain = std::max(mProcessor->getParam(kEnvS)->getValue(), 0.001f);

        // Constants matching KR106ADSR.h
        static constexpr float kAttackTarget    = 1.2f;
        static constexpr float kDecayUndershoot = -0.1f;
        static constexpr float kReleaseTarget   = -0.1f;
        static constexpr float kMinLevel        = 0.001f;
        static constexpr float kWindowMsJ6  = 15000.f;
        static constexpr float kWindowMs106 = 27500.f;

        // Compute times and coefficients per mode
        float attackMs, decayMs, releaseMs;
        float attackCoeff = 0.f, decayCoeff = 0.f, releaseCoeff = 0.f;
        float decayMul = 0.f, releaseMul = 0.f;

        if (j6)
        {
            // Juno-6: tau in seconds (must match KR106_DSP_SetParam.h)
            float attackTau  = 0.001f * std::pow(3000.f, dsp.mSliderA);
            float decayTau   = 0.004f * std::pow(1000.f, dsp.mSliderD);
            float releaseTau = 0.004f * std::pow(1000.f, dsp.mSliderR);

            // Per-ms coefficients (matching ADSR per-sample coeff but at 1kHz)
            attackCoeff  = 1.f - expf(-0.001f / attackTau);
            decayCoeff   = 1.f - expf(-0.001f / decayTau);
            releaseCoeff = 1.f - expf(-0.001f / releaseTau);

            // Approximate completion times for phase boundaries
            // Attack: solve kAttackTarget*(1-(1-c)^n) = 1.0
            attackMs = -logf(1.f - 1.f / kAttackTarget) / -logf(1.f - attackCoeff);
            // Decay/release: 3*tau ≈ 95% completion (matches tooltip)
            decayMs   = decayTau * 3000.f;
            releaseMs = releaseTau * 3000.f;
        }
        else
        {
            // Juno-106: ms from LUT
            attackMs  = DSP::LookupLUT(DSP::kAttackLUT,  dsp.mSliderA);
            decayMs   = DSP::LookupLUT(DSP::kDecayLUT,   dsp.mSliderD);
            releaseMs = DSP::LookupLUT(DSP::kReleaseLUT, dsp.mSliderR);

            // Per-ms multipliers
            decayMul   = (decayMs > 0.f)   ? expf(logf(kMinLevel) / decayMs)   : kMinLevel;
            releaseMul = (releaseMs > 0.f) ? expf(logf(kMinLevel) / releaseMs) : kMinLevel;
        }

        float kWindowMs = j6 ? kWindowMsJ6 : kWindowMs106;

        // --- Layout ---
        int hTop = h / 2;
        int hBot = h - hTop - 1; // 1px divider
        int yDivider = hTop;
        int yBotStart = hTop + 1;

        // Horizontal divider line
        g.setColour(dim);
        g.fillRect(0.f, static_cast<float>(yDivider), static_cast<float>(w), 1.f);

        // Phase boundaries as pixel positions within 15s window
        float xAD = std::round(std::min(attackMs / kWindowMs, 1.f) * (w - 1));
        float xDD = std::round(std::min((attackMs + decayMs) / kWindowMs, 1.f) * (w - 1));
        float xSR = std::round(std::max(1.f - releaseMs / kWindowMs, 0.f) * (w - 1));

        // A|D and D|S boundary lines (top half)
        g.setColour(dim);
        if (xAD > 0.f && xAD < w - 1)
            g.fillRect(xAD, 0.f, 1.f, static_cast<float>(hTop));
        if (xDD > xAD && xDD < w - 1)
            g.fillRect(xDD, 0.f, 1.f, static_cast<float>(hTop));

        // S|R boundary line (bottom half)
        if (xSR > 0.f && xSR < w - 1)
            g.fillRect(xSR, static_cast<float>(yBotStart), 1.f, static_cast<float>(hBot));

        // Sustain level lines (both halves)
        float susYTop = std::round((1.f - sustain) * (hTop - 1));
        float susYBot = std::round((1.f - sustain) * (hBot - 1)) + yBotStart;
        g.setColour(dim);
        if (susYTop > 0.f && susYTop < hTop - 1)
            g.fillRect(0.f, susYTop, static_cast<float>(w), 1.f);
        if (susYBot > yBotStart && susYBot < yBotStart + hBot - 1)
            g.fillRect(0.f, susYBot, static_cast<float>(w), 1.f);

        // --- Envelope evaluation helper ---
        // Returns envelope value [0, 1] given elapsed ms and phase
        auto evalEnv = [&](float ms, int phase) -> float
        {
            // phase: 0=attack, 1=decay, 2=sustain, 3=release
            float env = 0.f;
            if (phase == 0)
            {
                if (j6)
                {
                    env = kAttackTarget * (1.f - powf(1.f - attackCoeff, ms));
                    if (env > 1.f) env = 1.f;
                }
                else
                {
                    env = (attackMs > 0.f) ? ms / attackMs : 1.f;
                    if (env > 1.f) env = 1.f;
                }
            }
            else if (phase == 1)
            {
                if (j6)
                {
                    float target = sustain + kDecayUndershoot;
                    env = target + (1.f - target) * powf(1.f - decayCoeff, ms);
                    if (env < sustain) env = sustain;
                }
                else
                {
                    env = powf(decayMul, ms);
                    if (env < sustain) env = sustain;
                }
            }
            else if (phase == 2)
            {
                env = sustain;
            }
            else // release
            {
                if (j6)
                {
                    env = kReleaseTarget + (sustain - kReleaseTarget) * powf(1.f - releaseCoeff, ms);
                    if (env < 0.f) env = 0.f;
                }
                else
                {
                    env = sustain * powf(releaseMul, ms);
                }
            }
            return std::clamp(env, 0.f, 1.f);
        };

        // --- Draw top half: ADS ---
        g.setColour(bright);
        int lastY = hTop - 1;

        for (int px = 0; px < w; px++)
        {
            float ms = (static_cast<float>(px) / static_cast<float>(w - 1)) * kWindowMs;
            float env;

            if (ms < attackMs)
                env = evalEnv(ms, 0);
            else if (ms < attackMs + decayMs)
                env = evalEnv(ms - attackMs, 1);
            else
                env = evalEnv(0.f, 2); // sustain fill

            int y = static_cast<int>(std::round((1.f - env) * (hTop - 1)));
            int y1 = std::min(lastY, y);
            int y2 = std::max(lastY, y) + 1;
            g.fillRect(static_cast<float>(px), static_cast<float>(y1),
                       1.f, static_cast<float>(y2 - y1));
            lastY = y;
        }

        // --- Draw bottom half: SR ---
        lastY = static_cast<int>(std::round((1.f - sustain) * (hBot - 1)));

        for (int px = 0; px < w; px++)
        {
            float ms = (static_cast<float>(px) / static_cast<float>(w - 1)) * kWindowMs;
            float releaseStartMs = kWindowMs - releaseMs;
            float env;

            if (ms < releaseStartMs)
                env = evalEnv(0.f, 2); // sustain fill
            else
                env = evalEnv(ms - releaseStartMs, 3);

            int y = static_cast<int>(std::round((1.f - env) * (hBot - 1)));
            int y1 = std::min(lastY, y);
            int y2 = std::max(lastY, y) + 1;
            g.fillRect(static_cast<float>(px), static_cast<float>(y1 + yBotStart),
                       1.f, static_cast<float>(y2 - y1));
            lastY = y;
        }
    }

    static constexpr float kScales[3] = { 0.5f, 1.f, 1.5f };
    int mScaleIdx = 1; // default 1.0

    KR106AudioProcessor* mProcessor = nullptr;

    // Local ring buffers (copied from processor)
    float mRing[RING_SIZE] = {};
    float mRingR[RING_SIZE] = {};
    float mSyncRing[RING_SIZE] = {};
    int mRingWritePos = 0;
    int mLocalReadPos = 0;
    int mSamplesAvail = 0;

    // Display buffer (one extracted period)
    float mDisplay[RING_SIZE] = {};
    float mDisplayR[RING_SIZE] = {};
    int mDisplayLen = 0;
    bool mHasData = false;
};
