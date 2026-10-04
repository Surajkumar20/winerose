#pragma once

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
  #include <xmmintrin.h>
  #define WINEROSE_DENORMALS_SSE 1
#endif

namespace winerose {

/**
 * @brief Enables flush-to-zero / denormals-are-zero for the current thread while in scope (SPEC §1.9).
 *        JUCE-free replacement for juce::ScopedNoDenormals, so offline hosts get the same behaviour.
 */
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept
    {
#if defined(WINEROSE_DENORMALS_SSE)
        m_saved = _mm_getcsr();
        _mm_setcsr(m_saved | 0x8040u);   // FTZ (bit 15) | DAZ (bit 6)
#elif defined(__aarch64__)
        asm volatile("mrs %0, fpcr" : "=r"(m_saved));
        const unsigned long long fz = m_saved | (1ull << 24);   // FZ
        asm volatile("msr fpcr, %0" : : "r"(fz));
#endif
    }

    ~ScopedFlushDenormals()
    {
#if defined(WINEROSE_DENORMALS_SSE)
        _mm_setcsr(m_saved);
#elif defined(__aarch64__)
        asm volatile("msr fpcr, %0" : : "r"(m_saved));
#endif
    }

    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
#if defined(__aarch64__) && !defined(WINEROSE_DENORMALS_SSE)
    unsigned long long m_saved = 0;
#else
    unsigned int m_saved = 0;
#endif
};

} // namespace winerose
