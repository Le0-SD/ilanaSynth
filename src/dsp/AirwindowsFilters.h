#pragma once

#if defined(_M_X64) || defined(__x86_64__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
 #include <xmmintrin.h>
#endif

// Voice filters ported from Airwindows (Chris Johnson, https://www.airwindows.com,
// MIT license: see airwindows/LICENSE.txt). Each is one channel of one voice,
// set from a cutoff in Hz and a resonance 0..1 like the other models; the
// plugins' own knobs are fixed at the values noted.

#include <juce_core/juce_core.h>

#include "FilterCore.h"

#include <array>
#include <cmath>

namespace Airwindows
{
// The encode/decode curve the Y filters wrap around their biquad
// (by torridgristle, MIT): y = 1 - (1 - x)^p on each polarity, clamped to
// +-1. A table per exponent (the plugins call pow per sample).
class PowCurve
{
public:
    explicit PowCurve (double exponent)
    {
        for (size_t i = 0; i < table.size(); ++i)
        {
            const auto x = (double) i / (double) (size - 1);
            table[i] = (float) (1.0 - std::pow (1.0 - x, exponent));
        }
    }

    double operator() (double x) const
    {
        const auto a = std::abs (x);
        if (a >= 1.0)
            return x > 0.0 ? 1.0 : -1.0;

        const auto position = a * (double) (size - 1);
        const auto index = (size_t) position;
        const auto fraction = (float) (position - (double) index);
        const auto y = (double) (table[index] + (table[index + 1] - table[index]) * fraction);
        return x > 0.0 ? y : -y;
    }

private:
    static constexpr int size = 4097;
    std::array<float, (size_t) size + 1> table {};
};

// A transposed direct form II biquad whose coefficients glide to a new set
// over the next 16 samples (the voice sets them every 16), as the plugins
// glide theirs over a host buffer.
struct GlideBiquad
{
    std::array<double, 5> from {}, to {}, now {};
    double s1 = 0.0, s2 = 0.0;
    int step = 0;
    static constexpr int steps = 16;

    void set (const std::array<double, 5>& target, bool jump)
    {
        from = jump ? target : now;
        to = target;
        step = jump ? steps : 0;
        if (jump)
            now = target;
    }

    double process (double x)
    {
        if (step < steps)
        {
            ++step;
            const auto t = (double) step / (double) steps;
            for (size_t i = 0; i < 5; ++i)
                now[i] = from[i] + (to[i] - from[i]) * t;
        }

        const auto y = x * now[0] + s1;
        s1 = x * now[1] - y * now[3] + s2;
        s2 = x * now[2] - y * now[4];
        return y;
    }

    void reset() { s1 = s2 = 0.0; }

    // The glide's next coefficients, as process() takes them.
    void advance()
    {
        if (step < steps)
        {
            ++step;
            const auto t = (double) step / (double) steps;
            for (size_t i = 0; i < 5; ++i)
                now[i] = from[i] + (to[i] - from[i]) * t;
        }
    }

    bool glidesWith (const GlideBiquad& other) const
    {
        return step == other.step && from == other.from && to == other.to && now == other.now;
    }
};

// YLowpass, YHighpass, YBandpass and YNotch: a resonant two-pole filter
// between the encode and decode curves (RESEDGE), with fixed 20 kHz
// Butterworth low-passes on the way in and out. The plugins' resonant stage is
// a biquad whose coefficients glide over a buffer; here it is the same
// response computed as a zero-delay state-variable filter, because a voice's
// filter envelope moves the cutoff every 16 samples and a direct-form biquad
// overshoots under that (up to +3.7 dB peaks on the presets). Resonance 0..1
// sets the damping as the old SVF models did (Q = 1 / (2 - 2 r), so their
// linear response and every preset's tone balance carry over), topped at
// Q 40 up to resonance 0.98; above it they self-oscillate (see makeCoefficients).
class YFilter
{
public:
    enum class Mode { LowPass, BandPass, HighPass, Notch };

    struct Coefficients
    {
        double g = 0.0, k = 2.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
        bool selfOscillating = false;
        std::array<double, 5> fixed {};
    };

    // RESEDGE: the plugins' D knob, p = (D + 0.9)^4. At its default 0.1 the
    // curve is a straight line; above it the resonant peak gets its edge.
    static constexpr double resEdge = 0.25;
    // The voice runs hotter than a mix bus: the curve sees the signal at this
    // level (and the gain comes back after).
    static constexpr double headroom = 0.5;

    static constexpr double oscillationStart = 0.98, oscDamping = 0.5, oscScale = 3.4;

    static double exponent() { return std::pow (resEdge + 0.9, 4.0); }

    static Coefficients makeCoefficients (double sampleRate, double cutoff, double resonance)
    {
        Coefficients c;
        const auto f = juce::jlimit (15.0, sampleRate * 0.45, cutoff) / sampleRate;
        const auto r = juce::jlimit (0.0, 1.0, resonance);
        c.g = std::tan (juce::MathConstants<double>::pi * f);
        // Above 0.98 the damping goes negative and the filter sings at the
        // cutoff, held at a steady level by nonlinear damping (as the old SVF
        // models did; the Airwindows plugins stop short of this).
        c.selfOscillating = r > oscillationStart;
        c.k = c.selfOscillating ? 2.0 - 2.0 * oscillationStart - (r - oscillationStart) * 3.0
                                : juce::jmax (2.0 - 2.0 * r, 1.0 / 40.0);
        c.a1 = 1.0 / (1.0 + c.g * (c.g + c.k));
        c.a2 = c.g * c.a1;
        c.a3 = c.g * c.a2;

        const auto fixedQ = 0.7071;
        const auto k = std::tan (juce::MathConstants<double>::pi * juce::jmin (20000.0, sampleRate * 0.45) / sampleRate);
        const auto norm = 1.0 / (1.0 + k / fixedQ + k * k);
        c.fixed = { k * k * norm, 2.0 * k * k * norm, k * k * norm, 2.0 * (k * k - 1.0) * norm, (1.0 - k / fixedQ + k * k) * norm };
        return c;
    }

    void setMode (Mode newMode) { mode = newMode; }

    void setCoefficients (const Coefficients& c)
    {
        coefficients = c;
        fixedIn.set (c.fixed, true);
        fixedOut.set (c.fixed, true);
    }

    void reset()
    {
        ic1 = ic2 = ic3 = ic4 = 0.0;
        band1 = band2 = 0.0;
        fixedIn.reset();
        fixedOut.reset();
    }

    // 24 dB: the second stage sits inside the same encode/decode pair.
    float process (float input, bool slope24 = false)
    {
        auto x = encode (fixedIn.process ((double) input) * headroom);
        x = stage (x, ic1, ic2, band1);
        if (slope24)
            x = stage (x, ic3, ic4, band2);
        return (float) fixedOut.process (decode (x) / headroom);
    }

    // A voice's left and right filters over a block, side by side in SSE2
    // registers (double lanes): the same arithmetic as process() on each,
    // lane by lane, while both have the same settings and glide (else, and
    // while self-oscillating, each runs on its own).
    static void processPairBlock (YFilter& l, YFilter& r, const float* inL, const float* inR,
                                  float* outL, float* outR, int n, bool slope24)
    {
       #if defined(_M_X64) || defined(__x86_64__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
        if (l.pairsWith (r))
        {
            l.processPair (r, inL, inR, outL, outR, n, slope24);
            return;
        }
       #endif
        for (int i = 0; i < n; ++i)
            outL[i] = l.process (inL[i], slope24);
        for (int i = 0; i < n; ++i)
            outR[i] = r.process (inR[i], slope24);
    }

private:
    bool pairsWith (const YFilter& o) const
    {
        const auto& a = coefficients;
        const auto& b = o.coefficients;
        return mode == o.mode && ! a.selfOscillating && ! b.selfOscillating
               && a.g == b.g && a.k == b.k && a.a1 == b.a1 && a.a2 == b.a2 && a.a3 == b.a3 && a.fixed == b.fixed
               && fixedIn.glidesWith (o.fixedIn) && fixedOut.glidesWith (o.fixedOut);
    }

   #if defined(_M_X64) || defined(__x86_64__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
    void processPair (YFilter& o, const float* inL, const float* inR, float* outL, float* outR, int n, bool slope24)
    {
        const auto& c = coefficients;
        const auto k = _mm_set1_pd (c.k), a1 = _mm_set1_pd (c.a1), a2 = _mm_set1_pd (c.a2), a3 = _mm_set1_pd (c.a3);
        const auto two = _mm_set1_pd (2.0), head = _mm_set1_pd (headroom);
        const auto pair = [] (double a, double b) { return _mm_set_pd (b, a); };
        auto in1 = pair (fixedIn.s1, o.fixedIn.s1), in2 = pair (fixedIn.s2, o.fixedIn.s2);
        auto out1 = pair (fixedOut.s1, o.fixedOut.s1), out2 = pair (fixedOut.s2, o.fixedOut.s2);
        auto p1 = pair (ic1, o.ic1), p2 = pair (ic2, o.ic2), q1 = pair (ic3, o.ic3), q2 = pair (ic4, o.ic4);
        auto bandA = pair (band1, o.band1), bandB = pair (band2, o.band2);
        const auto m = mode;

        const auto stagePair = [&] (__m128d x, __m128d& s1, __m128d& s2, __m128d& lastBand)
        {
            const auto v3 = _mm_sub_pd (x, s2);
            const auto v1 = _mm_add_pd (_mm_mul_pd (a1, s1), _mm_mul_pd (a2, v3));
            const auto v2 = _mm_add_pd (_mm_add_pd (s2, _mm_mul_pd (a2, s1)), _mm_mul_pd (a3, v3));
            s1 = _mm_sub_pd (_mm_mul_pd (two, v1), s1);
            s2 = _mm_sub_pd (_mm_mul_pd (two, v2), s2);
            lastBand = v1;
            switch (m)
            {
                case Mode::LowPass:  return v2;
                case Mode::BandPass: return v1;
                case Mode::HighPass: return _mm_sub_pd (_mm_sub_pd (x, _mm_mul_pd (k, v1)), v2);
                case Mode::Notch:    return _mm_sub_pd (x, _mm_mul_pd (k, v1));
            }
            return x;
        };
        const auto curve = [] (const PowCurve& f, __m128d x)
        {
            alignas (16) double v[2];
            _mm_store_pd (v, x);
            return _mm_set_pd (f (v[1]), f (v[0]));
        };

        for (int i = 0; i < n; ++i)
        {
            fixedIn.advance();
            const auto& b = fixedIn.now;
            const auto x = pair ((double) inL[i], (double) inR[i]);
            const auto y = _mm_add_pd (_mm_mul_pd (x, _mm_set1_pd (b[0])), in1);
            in1 = _mm_add_pd (_mm_sub_pd (_mm_mul_pd (x, _mm_set1_pd (b[1])), _mm_mul_pd (y, _mm_set1_pd (b[3]))), in2);
            in2 = _mm_sub_pd (_mm_mul_pd (x, _mm_set1_pd (b[2])), _mm_mul_pd (y, _mm_set1_pd (b[4])));

            auto v = curve (encode, _mm_mul_pd (y, head));
            v = stagePair (v, p1, p2, bandA);
            if (slope24)
                v = stagePair (v, q1, q2, bandB);
            const auto d = _mm_div_pd (curve (decode, v), head);

            fixedOut.advance();
            const auto& e = fixedOut.now;
            const auto z = _mm_add_pd (_mm_mul_pd (d, _mm_set1_pd (e[0])), out1);
            out1 = _mm_add_pd (_mm_sub_pd (_mm_mul_pd (d, _mm_set1_pd (e[1])), _mm_mul_pd (z, _mm_set1_pd (e[3]))), out2);
            out2 = _mm_sub_pd (_mm_mul_pd (d, _mm_set1_pd (e[2])), _mm_mul_pd (z, _mm_set1_pd (e[4])));

            alignas (16) double w[2];
            _mm_store_pd (w, z);
            outL[i] = (float) w[0];
            outR[i] = (float) w[1];
        }

        const auto unpair = [] (__m128d v, double& a, double& b)
        {
            alignas (16) double w[2];
            _mm_store_pd (w, v);
            a = w[0];
            b = w[1];
        };
        unpair (in1, fixedIn.s1, o.fixedIn.s1);
        unpair (in2, fixedIn.s2, o.fixedIn.s2);
        unpair (out1, fixedOut.s1, o.fixedOut.s1);
        unpair (out2, fixedOut.s2, o.fixedOut.s2);
        unpair (p1, ic1, o.ic1);
        unpair (p2, ic2, o.ic2);
        unpair (q1, ic3, o.ic3);
        unpair (q2, ic4, o.ic4);
        unpair (bandA, band1, o.band1);
        unpair (bandB, band2, o.band2);
        o.fixedIn.step = fixedIn.step;
        o.fixedIn.now = fixedIn.now;
        o.fixedOut.step = fixedOut.step;
        o.fixedOut.now = fixedOut.now;
    }
   #endif

    double stage (double x, double& s1, double& s2, double& lastBand) const
    {
        const auto& c = coefficients;
        auto k = c.k, a1 = c.a1, a2 = c.a2, a3 = c.a3;

        if (c.selfOscillating)
        {
            // The damping rises with the band-pass level (from the last
            // sample): kappa = k + c (1 - tanh (a bp) / (a bp)).
            k += oscDamping * (1.0 - FilterCore::tanhOverX (oscScale * lastBand));
            a1 = 1.0 / (1.0 + c.g * (c.g + k));
            a2 = c.g * a1;
            a3 = c.g * a2;
        }

        const auto v3 = x - s2;
        const auto v1 = a1 * s1 + a2 * v3;
        const auto v2 = s2 + a2 * s1 + a3 * v3;
        s1 = 2.0 * v1 - s1;
        s2 = 2.0 * v2 - s2;
        lastBand = v1;

        switch (mode)
        {
            case Mode::LowPass:  return v2;
            case Mode::BandPass: return v1; // peak gain Q, as the old band-pass
            case Mode::HighPass: return x - k * v1 - v2;
            case Mode::Notch:    return x - k * v1;
        }
        return x;
    }

    inline static const PowCurve encode { exponent() };
    inline static const PowCurve decode { 1.0 / exponent() };

    Mode mode = Mode::LowPass;
    Coefficients coefficients;
    double ic1 = 0.0, ic2 = 0.0, ic3 = 0.0, ic4 = 0.0, band1 = 0.0, band2 = 0.0;
    GlideBiquad fixedIn, fixedOut;
};
// The Y Low Pass while it sits wide open (above 19 kHz, low resonance): the
// same fixed Butterworths and resonant stage, without the encode/decode
// curves (which cancel while the stage passes the band unchanged) and without
// the gliding coefficient sets. Measured against YFilter (ILANA_OPEN_FILTER_CHECK)
// it matches within 0.1 dB up to 15 kHz and 0.2 dB at 18 kHz.
class OpenLowPass
{
public:
    void set (double sampleRate, double cutoff, double resonance)
    {
        const auto c = YFilter::makeCoefficients (sampleRate, cutoff, resonance);
        for (size_t i = 0; i < 5; ++i)
            fixed[i] = (float) c.fixed[i];
        a1 = (float) c.a1;
        a2 = (float) c.a2;
        a3 = (float) c.a3;
    }

    void reset() { in1 = in2 = out1 = out2 = s1 = s2 = 0.0f; }

    float process (float x)
    {
        // Transposed direct form II, as GlideBiquad.
        const auto b = x * fixed[0] + in1;
        in1 = x * fixed[1] - b * fixed[3] + in2;
        in2 = x * fixed[2] - b * fixed[4];

        const auto v3 = b - s2;
        const auto v1 = a1 * s1 + a2 * v3;
        const auto v2 = s2 + a2 * s1 + a3 * v3;
        s1 = 2.0f * v1 - s1;
        s2 = 2.0f * v2 - s2;

        const auto y = v2 * fixed[0] + out1;
        out1 = v2 * fixed[1] - y * fixed[3] + out2;
        out2 = v2 * fixed[2] - y * fixed[4];
        return y;
    }

    // process() over the samples where both sides are equal, from the start
    // of the block, writing each output to both sides; returns how many.
    // The state lives in locals for the run (the per-sample member reads and
    // writes were the cost); the arithmetic is process()'s, bit for bit.
    int processLinkedRun (const float* inL, const float* inR, float* outL, float* outR, int n)
    {
        const auto f0 = fixed[0], f1 = fixed[1], f2 = fixed[2], f3 = fixed[3], f4 = fixed[4];
        const auto c1 = a1, c2 = a2, c3 = a3;
        auto i1 = in1, i2 = in2, o1 = out1, o2 = out2, t1 = s1, t2 = s2;
        auto i = 0;

        for (; i < n; ++i)
        {
            const auto x = inL[i];
            if (! (x == inR[i]))
                break;

            const auto b = x * f0 + i1;
            i1 = x * f1 - b * f3 + i2;
            i2 = x * f2 - b * f4;

            const auto v3 = b - t2;
            const auto v1 = c1 * t1 + c2 * v3;
            const auto v2 = t2 + c2 * t1 + c3 * v3;
            t1 = 2.0f * v1 - t1;
            t2 = 2.0f * v2 - t2;

            const auto y = v2 * f0 + o1;
            o1 = v2 * f1 - y * f3 + o2;
            o2 = v2 * f2 - y * f4;
            outL[i] = y;
            outR[i] = y;
        }

        in1 = i1; in2 = i2; out1 = o1; out2 = o2; s1 = t1; s2 = t2;
        return i;
    }

    // Two filters (left and right) over a block, as SSE lanes: the same
    // arithmetic as process(), lane by lane.
    static void processPairBlock (OpenLowPass& l, OpenLowPass& r, const float* inL, const float* inR,
                                  float* outL, float* outR, int n)
    {
       #if defined(_M_X64) || defined(__x86_64__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
        const auto pair = [] (float a, float b) { return _mm_set_ps (0.0f, 0.0f, b, a); };
        const __m128 f0 = pair (l.fixed[0], r.fixed[0]), f1 = pair (l.fixed[1], r.fixed[1]), f2 = pair (l.fixed[2], r.fixed[2]),
                     f3 = pair (l.fixed[3], r.fixed[3]), f4 = pair (l.fixed[4], r.fixed[4]);
        const __m128 a1 = pair (l.a1, r.a1), a2 = pair (l.a2, r.a2), a3 = pair (l.a3, r.a3), two = _mm_set1_ps (2.0f);
        auto in1 = pair (l.in1, r.in1), in2 = pair (l.in2, r.in2), out1 = pair (l.out1, r.out1), out2 = pair (l.out2, r.out2);
        auto s1 = pair (l.s1, r.s1), s2 = pair (l.s2, r.s2);

        for (int i = 0; i < n; ++i)
        {
            const auto x = pair (inL[i], inR[i]);
            const auto b = _mm_add_ps (_mm_mul_ps (x, f0), in1);
            in1 = _mm_add_ps (_mm_sub_ps (_mm_mul_ps (x, f1), _mm_mul_ps (b, f3)), in2);
            in2 = _mm_sub_ps (_mm_mul_ps (x, f2), _mm_mul_ps (b, f4));

            const auto v3 = _mm_sub_ps (b, s2);
            const auto v1 = _mm_add_ps (_mm_mul_ps (a1, s1), _mm_mul_ps (a2, v3));
            const auto v2 = _mm_add_ps (_mm_add_ps (s2, _mm_mul_ps (a2, s1)), _mm_mul_ps (a3, v3));
            s1 = _mm_sub_ps (_mm_mul_ps (two, v1), s1);
            s2 = _mm_sub_ps (_mm_mul_ps (two, v2), s2);

            const auto y = _mm_add_ps (_mm_mul_ps (v2, f0), out1);
            out1 = _mm_add_ps (_mm_sub_ps (_mm_mul_ps (v2, f1), _mm_mul_ps (y, f3)), out2);
            out2 = _mm_sub_ps (_mm_mul_ps (v2, f2), _mm_mul_ps (y, f4));

            alignas (16) float lanes[4];
            _mm_store_ps (lanes, y);
            outL[i] = lanes[0];
            outR[i] = lanes[1];
        }

        const auto store = [] (__m128 v, float& a, float& b)
        {
            alignas (16) float lanes[4];
            _mm_store_ps (lanes, v);
            a = lanes[0];
            b = lanes[1];
        };
        store (in1, l.in1, r.in1);
        store (in2, l.in2, r.in2);
        store (out1, l.out1, r.out1);
        store (out2, l.out2, r.out2);
        store (s1, l.s1, r.s1);
        store (s2, l.s2, r.s2);
       #else
        for (int i = 0; i < n; ++i)
        {
            outL[i] = l.process (inL[i]);
            outR[i] = r.process (inR[i]);
        }
       #endif
    }

private:
    std::array<float, 5> fixed {};
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    float in1 = 0.0f, in2 = 0.0f, out1 = 0.0f, out2 = 0.0f, s1 = 0.0f, s2 = 0.0f;
};
} // namespace Airwindows
