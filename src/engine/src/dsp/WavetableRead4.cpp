#include "engine/dsp/WavetableBank.h"

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  #include <emmintrin.h>
  #define WINEROSE_READ4_SSE2 1
#endif

namespace winerose::dsp {

#if defined(WINEROSE_READ4_SSE2)

void WavetableBank::read4(const double* phase, const FramePos& fp, const LevelChoice* lc, float* out) const noexcept
{
    // Table positions and fractions, four lanes. Same float arithmetic as read(): pos = float(phase·2048).
    const __m128 pos = _mm_set_ps(static_cast<float>(phase[3] * kFrameSize), static_cast<float>(phase[2] * kFrameSize),
                                  static_cast<float>(phase[1] * kFrameSize), static_cast<float>(phase[0] * kFrameSize));
    __m128i idx = _mm_cvttps_epi32(pos);
    const __m128 t = _mm_sub_ps(pos, _mm_cvtepi32_ps(idx));
    // idx >= 2048 → idx - 2048 (phase == 1.0 is phase 0); idx < 0 → 0.
    const __m128i over = _mm_cmpgt_epi32(idx, _mm_set1_epi32(kFrameSize - 1));
    idx = _mm_sub_epi32(idx, _mm_and_si128(over, _mm_set1_epi32(kFrameSize)));
    idx = _mm_andnot_si128(_mm_cmplt_epi32(idx, _mm_setzero_si128()), idx);

    alignas(16) int ix[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(ix), idx);

    // Gather the four samples around each lane's index straight into registers (no store→load round trip).
    auto gather = [&](int frame, const int* level, __m128& x0, __m128& x1, __m128& x2, __m128& x3) {
        const float* p0 = frameData(level[0], frame) + ix[0];
        const float* p1 = frameData(level[1], frame) + ix[1];
        const float* p2 = frameData(level[2], frame) + ix[2];
        const float* p3 = frameData(level[3], frame) + ix[3];
        x0 = _mm_set_ps(p3[-1], p2[-1], p1[-1], p0[-1]);
        x1 = _mm_set_ps(p3[0],  p2[0],  p1[0],  p0[0]);
        x2 = _mm_set_ps(p3[1],  p2[1],  p1[1],  p0[1]);
        x3 = _mm_set_ps(p3[2],  p2[2],  p1[2],  p0[2]);
    };
    auto lerpInto = [](__m128& s, __m128 target, __m128 f) { s = _mm_add_ps(s, _mm_mul_ps(_mm_sub_ps(target, s), f)); };

    const int level[4] = {lc[0].level, lc[1].level, lc[2].level, lc[3].level};
    __m128 s0, s1, s2, s3;
    gather(fp.f0, level, s0, s1, s2, s3);
    const bool morph = fp.ft > 0.0f;
    const __m128 ft = _mm_set1_ps(fp.ft);
    if (morph) {
        __m128 b0, b1, b2, b3;
        gather(fp.f1, level, b0, b1, b2, b3);
        lerpInto(s0, b0, ft); lerpInto(s1, b1, ft); lerpInto(s2, b2, ft); lerpInto(s3, b3, ft);
    }

    if (lc[0].blend > 0.0f || lc[1].blend > 0.0f || lc[2].blend > 0.0f || lc[3].blend > 0.0f) {
        // Lanes without a blend read their own level again (blend 0 leaves s unchanged) — never past the top.
        int next[4];
        for (int i = 0; i < 4; ++i) next[i] = lc[i].blend > 0.0f ? lc[i].level + 1 : lc[i].level;
        __m128 u0, u1, u2, u3;
        gather(fp.f0, next, u0, u1, u2, u3);
        if (morph) {
            __m128 d0, d1, d2, d3;
            gather(fp.f1, next, d0, d1, d2, d3);
            lerpInto(u0, d0, ft); lerpInto(u1, d1, ft); lerpInto(u2, d2, ft); lerpInto(u3, d3, ft);
        }
        const __m128 blend = _mm_set_ps(lc[3].blend, lc[2].blend, lc[1].blend, lc[0].blend);
        lerpInto(s0, u0, blend); lerpInto(s1, u1, blend); lerpInto(s2, u2, blend); lerpInto(s3, u3, blend);
    }

    // 4-point Hermite, same expression order as read().
    const __m128 half = _mm_set1_ps(0.5f);
    const __m128 c1 = _mm_mul_ps(half, _mm_sub_ps(s2, s0));
    const __m128 c2 = _mm_sub_ps(_mm_add_ps(_mm_sub_ps(s0, _mm_mul_ps(_mm_set1_ps(2.5f), s1)), _mm_mul_ps(_mm_set1_ps(2.0f), s2)),
                                 _mm_mul_ps(half, s3));
    const __m128 c3 = _mm_add_ps(_mm_mul_ps(half, _mm_sub_ps(s3, s0)), _mm_mul_ps(_mm_set1_ps(1.5f), _mm_sub_ps(s1, s2)));
    __m128 y = _mm_add_ps(_mm_mul_ps(c3, t), c2);
    y = _mm_add_ps(_mm_mul_ps(y, t), c1);
    y = _mm_add_ps(_mm_mul_ps(y, t), s1);
    _mm_storeu_ps(out, y);
}

#else

void WavetableBank::read4(const double* phase, const FramePos& fp, const LevelChoice* lc, float* out) const noexcept
{
    for (int i = 0; i < 4; ++i) out[i] = read(phase[i], fp, lc[i]);
}

#endif

} // namespace winerose::dsp
