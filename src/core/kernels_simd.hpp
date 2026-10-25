// SPDX-License-Identifier: GPL-2.0-or-later
// SIMD kernels over a vector traits type V; every lane runs exactly the operations of the C kernels in the same order.
// Included by one source file per instruction set, inside an anonymous namespace, so no inline symbol is shared.
#pragma once

#include <algorithm>
#include <cstdint>

namespace {

template <class V>
inline typename V::T clampV(typename V::T v, typename V::T lo, typename V::T hi) {
  return V::min(V::max(v, lo), hi);
}

inline float clampS(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

template <typename T>
uint64_t sadTail(const T* a, const T* b, int x, int w) {
  uint64_t s = 0;
  for (; x < w; x++) s += a[x] > b[x] ? a[x] - b[x] : b[x] - a[x];
  return s;
}

template <class V>
void vpassT(const float* const* taps, int n, const float* center, float* out, int w, float thr, float factor, bool divide) {
  using T = typename V::T;
  const T vthr = V::set1(thr), vfactor = V::set1(factor);
  int x = 0;
  for (; x + V::N <= w; x += V::N) {
    const T c = V::load(center + x);
    const T lo = V::sub(c, vthr), hi = V::add(c, vthr);
    T even = V::zero(), odd = V::zero();
    for (int j = 0; j + 1 < n; j += 2) {
      even = V::add(even, clampV<V>(V::load(taps[j] + x), lo, hi));
      odd = V::add(odd, clampV<V>(V::load(taps[j + 1] + x), lo, hi));
    }
    T sum = V::add(even, odd);
    if (n & 1) sum = V::add(sum, clampV<V>(V::load(taps[n - 1] + x), lo, hi));
    V::store(out + x, divide ? V::div(sum, vfactor) : V::mul(sum, vfactor));
  }
  for (; x < w; x++) {
    const float lo = center[x] - thr, hi = center[x] + thr;
    float even = 0, odd = 0;
    for (int j = 0; j + 1 < n; j += 2) {
      even += clampS(taps[j][x], lo, hi);
      odd += clampS(taps[j + 1][x], lo, hi);
    }
    float sum = even + odd;
    if (n & 1) sum += clampS(taps[n - 1][x], lo, hi);
    out[x] = divide ? sum / factor : sum * factor;
  }
}

template <class V>
void hpassT(const float* src, float* out, int p0, int p1, int rho, int step, int n, float thr, float inv) {
  using T = typename V::T;
  const T vthr = V::set1(thr), vinv = V::set1(inv);
  const int n8 = n & ~7;
  int p = p0;
  for (; p + V::N <= p1; p += V::N) {
    const T c = V::load(src + p);
    const T lo = V::sub(c, vthr), hi = V::add(c, vthr);
    const float* t = src + p - rho;
    T sum;
    if (n >= 8) {
      T acc[8] = {V::zero(), V::zero(), V::zero(), V::zero(), V::zero(), V::zero(), V::zero(), V::zero()};
      for (int j = 0; j < n8; j += 8)
        for (int l = 0; l < 8; l++) acc[l] = V::add(acc[l], clampV<V>(V::load(t + (j + l) * step), lo, hi));
      const T s0 = V::add(acc[0], acc[4]), s1 = V::add(acc[1], acc[5]);
      const T s2 = V::add(acc[2], acc[6]), s3 = V::add(acc[3], acc[7]);
      sum = V::add(V::add(s0, s2), V::add(s1, s3));
      for (int j = n8; j < n; j++) sum = V::add(sum, clampV<V>(V::load(t + j * step), lo, hi));
    } else {
      sum = V::zero();
      for (int j = 0; j < n; j++) sum = V::add(sum, clampV<V>(V::load(t + j * step), lo, hi));
    }
    V::store(out + p, V::mul(sum, vinv));
  }
  for (; p < p1; p++) {
    const float lo = src[p] - thr, hi = src[p] + thr;
    const float* t = src + p - rho;
    float sum;
    if (n >= 8) {
      float a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
      for (int j = 0; j < n8; j += 8)
        for (int l = 0; l < 8; l++) a[l] += clampS(t[(j + l) * step], lo, hi);
      sum = ((a[0] + a[4]) + (a[2] + a[6])) + ((a[1] + a[5]) + (a[3] + a[7]));
      for (int j = n8; j < n; j++) sum += clampS(t[j * step], lo, hi);
    } else {
      sum = 0;
      for (int j = 0; j < n; j++) sum += clampS(t[j * step], lo, hi);
    }
    out[p] = sum * inv;
  }
}

template <class V>
void store8T(const float* d, const uint8_t* src, uint8_t* dst, int w, const float* add) {
  using T = typename V::T;
  const T zero = V::zero(), maxv = V::set1(255.0f);
  int x = 0;
  for (; x + V::N <= w; x += V::N) {
    T f = V::add(V::add(V::load(d + x), V::loadU8(src + x)), V::load(add + (x & 63)));
    V::storeTruncU8(dst + x, V::min(V::max(f, zero), maxv));
  }
  for (; x < w; x++) {
    float f = (d[x] + static_cast<float>(src[x])) + add[x & 63];
    f = f >= 0 ? std::min(f, 255.0f) : 0.0f;
    dst[x] = static_cast<uint8_t>(static_cast<int>(f));
  }
}

template <class V>
void store16T(const float* d, const uint16_t* src, uint16_t* dst, int w, const float* add, float maxv) {
  using T = typename V::T;
  const T zero = V::zero(), vmax = V::set1(maxv);
  int x = 0;
  for (; x + V::N <= w; x += V::N) {
    T f = V::add(V::add(V::load(d + x), V::loadU16(src + x)), V::load(add + (x & 63)));
    V::storeTruncU16(dst + x, V::min(V::max(f, zero), vmax));
  }
  for (; x < w; x++) {
    float f = (d[x] + static_cast<float>(src[x])) + add[x & 63];
    f = f >= 0 ? std::min(f, maxv) : 0.0f;
    dst[x] = static_cast<uint16_t>(static_cast<int>(f));
  }
}

}  // namespace
