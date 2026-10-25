// SPDX-License-Identifier: GPL-2.0-or-later
// Reference kernels (plain C++, the order of every float operation follows docs/SPEC.md).
#include <algorithm>

#include "kernels.hpp"

namespace autoadjust {

namespace {

inline float clampTap(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

void vpassC(const float* const* taps, int n, const float* center, float* out, int w, float thr, float factor, bool divide) {
  for (int x = 0; x < w; x++) {
    const float lo = center[x] - thr, hi = center[x] + thr;
    float even = 0, odd = 0;
    for (int j = 0; j + 1 < n; j += 2) {
      even += clampTap(taps[j][x], lo, hi);
      odd += clampTap(taps[j + 1][x], lo, hi);
    }
    float sum = even + odd;
    if (n & 1) sum += clampTap(taps[n - 1][x], lo, hi);
    out[x] = divide ? sum / factor : sum * factor;
  }
}

void hpassC(const float* src, float* out, int p0, int p1, int rho, int step, int n, float thr, float inv) {
  for (int p = p0; p < p1; p++) {
    const float lo = src[p] - thr, hi = src[p] + thr;
    const float* t = src + p - rho;
    float sum;
    if (n >= 8) {
      const int n8 = n & ~7;
      float a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0};
      for (int j = 0; j < n8; j += 8) {
        for (int l = 0; l < 4; l++) a[l] += clampTap(t[(j + l) * step], lo, hi);
        for (int l = 0; l < 4; l++) b[l] += clampTap(t[(j + 4 + l) * step], lo, hi);
      }
      const float s0 = a[0] + b[0], s1 = a[1] + b[1], s2 = a[2] + b[2], s3 = a[3] + b[3];
      sum = (s0 + s2) + (s1 + s3);
      for (int j = n8; j < n; j++) sum += clampTap(t[j * step], lo, hi);
    } else {
      sum = 0;
      for (int j = 0; j < n; j++) sum += clampTap(t[j * step], lo, hi);
    }
    out[p] = sum * inv;
  }
}

template <typename T>
void storeC(const float* d, const T* src, T* dst, int w, const float* add, float maxv) {
  for (int x = 0; x < w; x++) {
    float f = (d[x] + static_cast<float>(src[x])) + add[x & 63];
    f = f >= 0 ? std::min(f, maxv) : 0.0f;
    dst[x] = static_cast<T>(static_cast<int>(f));
  }
}

void store8C(const float* d, const uint8_t* src, uint8_t* dst, int w, const float* add) { storeC(d, src, dst, w, add, 255.0f); }
void store16C(const float* d, const uint16_t* src, uint16_t* dst, int w, const float* add, float maxv) {
  storeC(d, src, dst, w, add, maxv);
}

}  // namespace

const Kernels& kernelsC() {
  static const Kernels k{"C", vpassC, hpassC, store8C, store16C};
  return k;
}

}  // namespace autoadjust
