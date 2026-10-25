// SPDX-License-Identifier: GPL-2.0-or-later
// NEON kernels for arm64 (4 lanes); multiply and add stay separate (no vmla/vfma), like the C kernels.
#include "kernels.hpp"

#if defined(AUTOADJUST_NEON)
#include <arm_neon.h>

#include <cstring>

#include "kernels_simd.hpp"

namespace {

struct VNeon {
  using T = float32x4_t;
  static constexpr int N = 4;
  static T zero() { return vdupq_n_f32(0.0f); }
  static T set1(float v) { return vdupq_n_f32(v); }
  static T load(const float* p) { return vld1q_f32(p); }
  static void store(float* p, T v) { vst1q_f32(p, v); }
  static T add(T a, T b) { return vaddq_f32(a, b); }
  static T sub(T a, T b) { return vsubq_f32(a, b); }
  static T mul(T a, T b) { return vmulq_f32(a, b); }
  static T div(T a, T b) { return vdivq_f32(a, b); }
  static T min(T a, T b) { return vminq_f32(a, b); }
  static T max(T a, T b) { return vmaxq_f32(a, b); }
  static T loadU8(const uint8_t* p) {
    uint32_t bytes;
    std::memcpy(&bytes, p, 4);
    const uint8x8_t b = vreinterpret_u8_u32(vdup_n_u32(bytes));
    return vcvtq_f32_u32(vmovl_u16(vget_low_u16(vmovl_u8(b))));
  }
  static T loadU16(const uint16_t* p) { return vcvtq_f32_u32(vmovl_u16(vld1_u16(p))); }
  static void storeTruncU8(uint8_t* p, T v) {
    // Values are already clamped to [0, 255]; vcvtq_u32_f32 truncates toward zero like the C cast.
    const uint16x4_t w = vmovn_u32(vcvtq_u32_f32(v));
    const uint8x8_t b = vmovn_u16(vcombine_u16(w, w));
    const uint32_t bytes = vget_lane_u32(vreinterpret_u32_u8(b), 0);
    std::memcpy(p, &bytes, 4);
  }
  static void storeTruncU16(uint16_t* p, T v) { vst1_u16(p, vmovn_u32(vcvtq_u32_f32(v))); }
};

}  // namespace

namespace autoadjust {

const Kernels& kernelsNEON() {
  static const Kernels k{"NEON", vpassT<VNeon>, hpassT<VNeon>, store8T<VNeon>, store16T<VNeon>};
  return k;
}

}  // namespace autoadjust

#endif
