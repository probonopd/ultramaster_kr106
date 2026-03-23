#pragma once

#include <cmath>

// Juno-6 continuous HPF frequency curve (PCHIP interpolation of 11 measured points).
// Input: slider position 0–1, Output: HPF cutoff frequency in Hz.
inline float getJuno6HPFFreqPCHIP(float x) {
    static const float y[] = {
        38.6f, 83.5f, 181.3f, 394.7f, 418.4f,
        437.1f, 455.8f, 605.5f, 988.6f, 1183.2f, 1394.2f
    };
    static constexpr int N = 11;
    static constexpr float h = 0.1f;

    if (x <= 0.0f) return y[0];
    if (x >= 1.0f) return y[N - 1];

    float x_scaled = x * 10.0f;
    int i = (int)x_scaled;
    if (i >= N - 1) i = N - 2;
    float t = x_scaled - (float)i;

    auto get_slope = [&](int idx) -> float {
        if (idx <= 0 || idx >= N - 1) return 0.0f;
        float d_prev = (y[idx] - y[idx - 1]) / h;
        float d_next = (y[idx + 1] - y[idx]) / h;
        if (d_prev * d_next <= 0.0f) return 0.0f;
        return 2.0f / (1.0f / d_prev + 1.0f / d_next);
    };

    float m_i = get_slope(i);
    float m_next = get_slope(i + 1);

    float t2 = t * t;
    float t3 = t2 * t;
    float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    float h10 = t3 - 2.0f * t2 + t;
    float h01 = -2.0f * t3 + 3.0f * t2;
    float h11 = t3 - t2;

    return h00 * y[i] + h10 * h * m_i + h01 * y[i + 1] + h11 * h * m_next;
}