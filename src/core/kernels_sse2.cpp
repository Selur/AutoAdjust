// SPDX-License-Identifier: GPL-2.0-or-later
// SSE2 kernels (4 lanes).
#include "kernels.hpp"

#if defined(AUTOADJUST_X86)
#include <emmintrin.h>

#include <cstring>

#include "kernels_simd.hpp"

namespace {

struct VSse2 {
  using T = __m128;
  static constexpr int N = 4;
  static T zero() { return _mm_setzero_ps(); }
  static T set1(float v) { return _mm_set1_ps(v); }
  static T load(const float* p) { return _mm_loadu_ps(p); }
  static void store(float* p, T v) { _mm_storeu_ps(p, v); }
  static T add(T a, T b) { return _mm_add_ps(a, b); }
  static T sub(T a, T b) { return _mm_sub_ps(a, b); }
  static T mul(T a, T b) { return _mm_mul_ps(a, b); }
  static T div(T a, T b) { return _mm_div_ps(a, b); }
  static T min(T a, T b) { return _mm_min_ps(a, b); }
  static T max(T a, T b) { return _mm_max_ps(a, b); }
  static T loadU8(const uint8_t* p) {
    int bytes;
    std::memcpy(&bytes, p, 4);
    const __m128i z = _mm_setzero_si128();
    const __m128i v = _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(bytes), z), z);
    return _mm_cvtepi32_ps(v);
  }
  static T loadU16(const uint16_t* p) {
    const __m128i v = _mm_unpacklo_epi16(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p)), _mm_setzero_si128());
    return _mm_cvtepi32_ps(v);
  }
  static void storeTruncU8(uint8_t* p, T v) {
    const __m128i i = _mm_cvttps_epi32(v);
    const __m128i b = _mm_packus_epi16(_mm_packs_epi32(i, i), _mm_setzero_si128());
    const int bytes = _mm_cvtsi128_si32(b);
    std::memcpy(p, &bytes, 4);
  }
  static void storeTruncU16(uint16_t* p, T v) {
    // No unsigned 32 -> 16 bit pack in SSE2: shift into the signed range, pack, shift back.
    const __m128i i = _mm_sub_epi32(_mm_cvttps_epi32(v), _mm_set1_epi32(32768));
    const __m128i w = _mm_xor_si128(_mm_packs_epi32(i, i), _mm_set1_epi16(static_cast<short>(0x8000)));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(p), w);
  }
};

uint64_t sad8Sse2(const uint8_t* a, const uint8_t* b, int w) {
  __m128i acc = _mm_setzero_si128();
  int x = 0;
  for (; x + 16 <= w; x += 16)
    acc = _mm_add_epi64(acc, _mm_sad_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + x)),
                                          _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + x))));
  const uint64_t s = static_cast<uint64_t>(_mm_cvtsi128_si32(acc)) +
                     static_cast<uint64_t>(_mm_cvtsi128_si32(_mm_srli_si128(acc, 8)));
  return s + sadTail(a, b, x, w);
}

uint64_t sad16Sse2(const uint16_t* a, const uint16_t* b, int w) {
  // 32-bit lanes hold one row: at most w / 4 * 65535 per lane.
  const __m128i z = _mm_setzero_si128();
  __m128i acc = z;
  int x = 0;
  for (; x + 8 <= w; x += 8) {
    const __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + x));
    const __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + x));
    const __m128i d = _mm_or_si128(_mm_subs_epu16(va, vb), _mm_subs_epu16(vb, va));
    acc = _mm_add_epi32(acc, _mm_add_epi32(_mm_unpacklo_epi16(d, z), _mm_unpackhi_epi16(d, z)));
  }
  alignas(16) uint32_t lane[4];
  _mm_store_si128(reinterpret_cast<__m128i*>(lane), acc);
  return static_cast<uint64_t>(lane[0]) + lane[1] + lane[2] + lane[3] + sadTail(a, b, x, w);
}

}  // namespace

namespace autoadjust {

const Kernels& kernelsSSE2() {
  static const Kernels k{"SSE2", vpassT<VSse2>, hpassT<VSse2>, store8T<VSse2>, store16T<VSse2>, sad8Sse2, sad16Sse2};
  return k;
}

}  // namespace autoadjust

#endif
