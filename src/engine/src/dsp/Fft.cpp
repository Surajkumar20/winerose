#include "engine/dsp/Fft.h"

#include "pffft.h"

#include <cstring>
#include <stdexcept>

namespace winerose::dsp {

RealFft::RealFft(int size)
    : m_size(size)
    , m_setup(pffft_new_setup(size, PFFFT_REAL))
{
    if (m_setup == nullptr) throw std::invalid_argument("RealFft: unsupported size");
    m_bufA = static_cast<float*>(pffft_aligned_malloc(sizeof(float) * static_cast<std::size_t>(size)));
    m_bufB = static_cast<float*>(pffft_aligned_malloc(sizeof(float) * static_cast<std::size_t>(size)));
}

RealFft::~RealFft()
{
    pffft_aligned_free(m_bufA);
    pffft_aligned_free(m_bufB);
    pffft_destroy_setup(static_cast<PFFFT_Setup*>(m_setup));
}

void RealFft::forward(const float* input, float* spectrum)
{
    std::memcpy(m_bufA, input, sizeof(float) * static_cast<std::size_t>(m_size));
    pffft_transform_ordered(static_cast<PFFFT_Setup*>(m_setup), m_bufA, m_bufB, nullptr, PFFFT_FORWARD);
    std::memcpy(spectrum, m_bufB, sizeof(float) * static_cast<std::size_t>(m_size));
}

void RealFft::inverse(const float* spectrum, float* output)
{
    std::memcpy(m_bufA, spectrum, sizeof(float) * static_cast<std::size_t>(m_size));
    pffft_transform_ordered(static_cast<PFFFT_Setup*>(m_setup), m_bufA, m_bufB, nullptr, PFFFT_BACKWARD);
    const float scale = 1.0f / static_cast<float>(m_size);
    for (int i = 0; i < m_size; ++i) output[i] = m_bufB[i] * scale;
}

} // namespace winerose::dsp
