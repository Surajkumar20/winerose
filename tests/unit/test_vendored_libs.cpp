// Smoke tests for the vendored DSP libraries (libs/): they build with our flags and behave as expected,
// so engine branches can rely on them.

#include "hiir/PolyphaseIir2Designer.h"
#include "hiir/Upsampler2xFpu.h"
#include "pffft.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using Catch::Approx;

TEST_CASE("pffft real forward/inverse round-trips", "[libs][pffft]")
{
    constexpr int n = 2048;   // the wavetable frame size (SPEC §1.2)
    PFFFT_Setup* setup = pffft_new_setup(n, PFFFT_REAL);
    REQUIRE(setup != nullptr);

    auto* in   = static_cast<float*>(pffft_aligned_malloc(n * sizeof(float)));
    auto* freq = static_cast<float*>(pffft_aligned_malloc(n * sizeof(float)));
    auto* out  = static_cast<float*>(pffft_aligned_malloc(n * sizeof(float)));
    for (int i = 0; i < n; ++i) in[i] = std::sin(2.0f * 3.14159265f * 5.0f * static_cast<float>(i) / n);

    pffft_transform(setup, in, freq, nullptr, PFFFT_FORWARD);
    pffft_transform(setup, freq, out, nullptr, PFFFT_BACKWARD);
    for (int i = 0; i < n; ++i) REQUIRE(out[i] / n == Approx(in[i]).margin(1e-5));   // pffft is unscaled

    pffft_aligned_free(in);
    pffft_aligned_free(freq);
    pffft_aligned_free(out);
    pffft_destroy_setup(setup);
}

TEST_CASE("hiir designs a half-band filter and upsamples", "[libs][hiir]")
{
    constexpr int nbrCoefs = 8;
    double coefs[nbrCoefs];
    hiir::PolyphaseIir2Designer::compute_coefs_spec_order_tbw(coefs, nbrCoefs, 0.04);
    for (double c : coefs) CHECK((c > 0.0 && c < 1.0));

    hiir::Upsampler2xFpu<nbrCoefs> up;
    up.set_coefs(coefs);
    std::vector<float> in(1024, 1.0f), out(2048, 0.0f);   // narrow transition band: allow time to settle
    up.process_block(out.data(), in.data(), static_cast<long>(in.size()));
    CHECK(out.back() == Approx(1.0f).margin(1e-3));   // DC passes at unity once settled
}
