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

uint64_t sad8Avx2(const uint8_t* a, const uint8_t* b, int w) {
  __m256i acc = _mm256_setzero_si256();
  int x = 0;
  for (; x + 32 <= w; x += 32)
    acc = _mm256_add_epi64(acc, _mm256_sad_epu8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + x)),
                                                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + x))));
  alignas(32) uint64_t lane[4];
  _mm256_store_si256(reinterpret_cast<__m256i*>(lane), acc);
  return lane[0] + lane[1] + lane[2] + lane[3] + sadTail(a, b, x, w);
}

uint64_t sad16Avx2(const uint16_t* a, const uint16_t* b, int w) {
  // 32-bit lanes hold one row: at most w / 8 * 65535 per lane.
  const __m256i z = _mm256_setzero_si256();
  __m256i acc = z;
  int x = 0;
  for (; x + 16 <= w; x += 16) {
    const __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + x));
    const __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + x));
    const __m256i d = _mm256_or_si256(_mm256_subs_epu16(va, vb), _mm256_subs_epu16(vb, va));
    acc = _mm256_add_epi32(acc, _mm256_add_epi32(_mm256_unpacklo_epi16(d, z), _mm256_unpackhi_epi16(d, z)));
  }
  alignas(32) uint32_t lane[8];
  _mm256_store_si256(reinterpret_cast<__m256i*>(lane), acc);
  uint64_t s = 0;
  for (uint32_t v : lane) s += v;
  return s + sadTail(a, b, x, w);
}

}  // namespace

namespace autoadjust {

const Kernels& kernelsAVX2() {
  static const Kernels k{"AVX2", vpassT<VAvx2>, hpassT<VAvx2>, store8T<VAvx2>, store16T<VAvx2>, sad8Avx2, sad16Avx2};
  return k;
}

}  // namespace autoadjust

#endif
