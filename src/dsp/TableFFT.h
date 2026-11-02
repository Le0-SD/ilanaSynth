#pragma once

// A plain radix-2 complex FFT for building wavetables (TableFactory's
// additive tables and Wavetable's mipmaps). Built here rather than with
// juce::dsp::FFT so a table is the same on every platform: JUCE picks a
// different engine per platform (vDSP on macOS), and the factory tables,
// and so every preset, must not depend on that. The inverse transform is
// scaled by 1 / N, as juce::dsp::FFT's is.

#include <cmath>
#include <complex>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
 #include <emmintrin.h>
 #define TABLE_FFT_SSE2 1
#else
 #define TABLE_FFT_SSE2 0
#endif

class TableFFT
{
public:
    explicit TableFFT (int order)
        : size (1 << order), twiddles ((size_t) size / 2), bitReversed ((size_t) size), data ((size_t) size)
    {
        for (int k = 0; k < size / 2; ++k)
        {
            const auto angle = -2.0 * 3.14159265358979323846 * k / size;
            twiddles[(size_t) k] = { std::cos (angle), std::sin (angle) };
        }
        for (int i = 0; i < size; ++i)
        {
            auto reversed = 0;
            for (int bit = 1, rest = i; bit < size; bit <<= 1, rest >>= 1)
                reversed = (reversed << 1) | (rest & 1);
            bitReversed[(size_t) i] = reversed;
        }
    }

    int getSize() const noexcept { return size; }

    // Not for concurrent calls on one instance (each table builder has its own).
    void perform (const std::complex<float>* input, std::complex<float>* output, bool inverse) const
    {
        for (int i = 0; i < size; ++i)
            data[(size_t) bitReversed[(size_t) i]] = std::complex<double> (input[i]);

        for (int length = 2; length <= size; length <<= 1)
        {
            const auto half = length / 2, stride = size / length;
           #if TABLE_FFT_SSE2
            // One complex per SSE2 register, the same operations as below:
            // br = yr wr - yi wi (as yr wr + -(yi wi), which IEEE defines as
            // the same), bi = yr wi + yi wr. Bit for bit the scalar result.
            auto* d = reinterpret_cast<double*> (data.data());
            const auto signs = _mm_set_pd (1.0, -1.0);
            for (int start = 0; start < size; start += length)
                for (int k = 0; k < half; ++k)
                {
                    const auto& t = twiddles[(size_t) (k * stride)];
                    const auto wr = _mm_set1_pd (t.real()), wi = _mm_set1_pd (inverse ? -t.imag() : t.imag());
                    auto* x = d + 2 * (start + k);
                    auto* y = d + 2 * (start + k + half);
                    const auto yv = _mm_loadu_pd (y), xv = _mm_loadu_pd (x);
                    const auto swapped = _mm_shuffle_pd (yv, yv, 1);
                    const auto b = _mm_add_pd (_mm_mul_pd (yv, wr), _mm_mul_pd (_mm_mul_pd (swapped, wi), signs));
                    _mm_storeu_pd (y, _mm_sub_pd (xv, b));
                    _mm_storeu_pd (x, _mm_add_pd (xv, b));
                }
           #else
            for (int start = 0; start < size; start += length)
                for (int k = 0; k < half; ++k)
                {
                    const auto& t = twiddles[(size_t) (k * stride)];
                    const auto wr = t.real(), wi = inverse ? -t.imag() : t.imag();
                    auto& x = data[(size_t) (start + k)];
                    auto& y = data[(size_t) (start + k + half)];
                    const auto br = y.real() * wr - y.imag() * wi, bi = y.real() * wi + y.imag() * wr;
                    y = { x.real() - br, x.imag() - bi };
                    x = { x.real() + br, x.imag() + bi };
                }
           #endif
        }

        const auto scale = inverse ? 1.0 / size : 1.0;
        for (int i = 0; i < size; ++i)
            output[i] = std::complex<float> (data[(size_t) i] * scale);
    }

private:
    int size;
    std::vector<std::complex<double>> twiddles;
    std::vector<int> bitReversed;
    mutable std::vector<std::complex<double>> data;
};
