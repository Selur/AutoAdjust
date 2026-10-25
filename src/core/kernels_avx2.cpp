// SPDX-License-Identifier: GPL-2.0-or-later
// AVX2 kernels (8 lanes); built with AVX2 enabled but without FMA, so no multiply-add is fused.
#include "kernels.hpp"

#if defined(AUTOADJUST_X86)
#include <immintrin.h>

#include "kernels_simd.hpp"

namespace {

struct VAvx2 {
  using T = __m256;
  static constexpr int N = 8;
  static T zero() { return _mm256_setzero_ps(); }
  static T set1(float v) { return _mm256_set1_ps(v); }
  static T load(const float* p) { return _mm256_loadu_ps(p); }
  static void store(float* p, T v) { _mm256_storeu_ps(p, v); }
  static T add(T a, T b) { return _mm256_add_ps(a, b); }
  static T sub(T a, T b) { return _mm256_sub_ps(a, b); }
  static T mul(T a, T b) { return _mm256_mul_ps(a, b); }
  static T div(T a, T b) { return _mm256_div_ps(a, b); }
  static T min(T a, T b) { return _mm256_min_ps(a, b); }
  static T max(T a, T b) { return _mm256_max_ps(a, b); }
  static T loadU8(const uint8_t* p) {
    return _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p))));
  }
  static T loadU16(const uint16_t* p) {
    return _mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))));
  }
  static void storeTruncU8(uint8_t* p, T v) {
    const __m256i i = _mm256_cvttps_epi32(v);
    const __m128i w = _mm_packus_epi32(_mm256_castsi256_si128(i), _mm256_extracti128_si256(i, 1));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(p), _mm_packus_epi16(w, w));
  }
  static void storeTruncU16(uint16_t* p, T v) {
    const __m256i i = _mm256_cvttps_epi32(v);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(p), _mm_packus_epi32(_mm256_castsi256_si128(i), _mm256_extracti128_si256(i, 1)));
  }
};

}  // namespace

namespace autoadjust {

const Kernels& kernelsAVX2() {
  static const Kernels k{"AVX2", vpassT<VAvx2>, hpassT<VAvx2>, store8T<VAvx2>, store16T<VAvx2>};
  return k;
}

}  // namespace autoadjust

#endif
