// SPDX-License-Identifier: GPL-2.0-or-later
// Bit-exact comparison of every kernel set against the C kernels on random data.
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "core/kernels.hpp"

using namespace autoadjust;

static int failures = 0;

static bool same(const void* a, const void* b, size_t bytes) { return std::memcmp(a, b, bytes) == 0; }

static void check(const Kernels& k, std::mt19937& rng) {
  std::uniform_real_distribution<float> val(-3.0f, 3.0f);
  std::uniform_int_distribution<int> coin(0, 3);
  for (int w : {1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 64, 65, 129, 957, 1918}) {
    // vpass: n tap rows, interior (factor) and border (divide)
    for (int n : {1, 2, 3, 7, 8, 12, 13, 25, 33, 65}) {
      std::vector<std::vector<float>> rows(n, std::vector<float>(w));
      std::vector<float> center(w);
      for (auto& r : rows)
        for (auto& v : r) v = coin(rng) == 0 ? 0.0f : val(rng);
      for (auto& v : center) v = val(rng);
      std::vector<const float*> taps;
      for (auto& r : rows) taps.push_back(r.data());
      for (bool divide : {false, true}) {
        std::vector<float> a(w), b(w);
        const float factor = divide ? static_cast<float>(n) : 1.0f / (static_cast<float>(n - 1) + 1.0f);
        kernelsC().vpass(taps.data(), n, center.data(), a.data(), w, 0.5f, factor, divide);
        k.vpass(taps.data(), n, center.data(), b.data(), w, 0.5f, factor, divide);
        if (!same(a.data(), b.data(), w * sizeof(float))) {
          std::printf("%s vpass differs: w=%d n=%d divide=%d\n", k.name, w, n, divide);
          failures++;
        }
      }
    }
    // hpass: interior positions [rho, w - rho)
    for (int step : {1, 2}) {
      for (int rho : {2, 3, 4, 6, 8, 11, 12, 13, 16, 23, 24, 32}) {
        if (w <= 2 * rho) continue;
        const int n = (2 * rho) / step + 1;
        const float inv = 1.0f / (static_cast<float>(2 * rho) / static_cast<float>(step) + 1.0f);
        std::vector<float> src(w), a(w, 7.0f), b(w, 7.0f);
        for (auto& v : src) v = coin(rng) == 0 ? 0.0f : val(rng);
        const float thr = coin(rng) == 0 ? 128.0f : 0.5f;
        kernelsC().hpass(src.data(), a.data(), rho, w - rho, rho, step, n, thr, inv);
        k.hpass(src.data(), b.data(), rho, w - rho, rho, step, n, thr, inv);
        if (!same(a.data(), b.data(), w * sizeof(float))) {
          std::printf("%s hpass differs: w=%d rho=%d step=%d\n", k.name, w, rho, step);
          failures++;
        }
      }
    }
    // store: deltas around and beyond the range, both dither rows
    std::vector<float> add(64), d(w);
    for (auto& v : add) v = std::uniform_real_distribution<float>(0.0f, 0.999f)(rng);
    for (auto& v : d) v = std::uniform_real_distribution<float>(-300.0f, 300.0f)(rng);
    std::vector<uint8_t> s8(w), a8(w), b8(w);
    for (auto& v : s8) v = static_cast<uint8_t>(rng());
    kernelsC().store8(d.data(), s8.data(), a8.data(), w, add.data());
    k.store8(d.data(), s8.data(), b8.data(), w, add.data());
    if (!same(a8.data(), b8.data(), w)) {
      std::printf("%s store8 differs: w=%d\n", k.name, w);
      failures++;
    }
    // sad: random rows, both sample sizes
    {
      std::vector<uint8_t> a8s(w), b8s(w);
      for (auto& v : a8s) v = static_cast<uint8_t>(rng());
      for (auto& v : b8s) v = static_cast<uint8_t>(rng());
      std::vector<uint16_t> a16s(w), b16s(w);
      for (auto& v : a16s) v = static_cast<uint16_t>(rng());
      for (auto& v : b16s) v = static_cast<uint16_t>(rng());
      if (kernelsC().sad8(a8s.data(), b8s.data(), w) != k.sad8(a8s.data(), b8s.data(), w) ||
          kernelsC().sad16(a16s.data(), b16s.data(), w) != k.sad16(a16s.data(), b16s.data(), w)) {
        std::printf("%s sad differs: w=%d\n", k.name, w);
        failures++;
      }
    }
    for (float maxv : {1023.0f, 4095.0f, 65535.0f}) {
      std::vector<uint16_t> s16(w), a16(w), b16(w);
      std::vector<float> d16(w);
      for (auto& v : d16) v = std::uniform_real_distribution<float>(-maxv * 0.3f, maxv * 0.3f)(rng);
      for (auto& v : s16) v = static_cast<uint16_t>(rng() % (static_cast<unsigned>(maxv) + 1));
      kernelsC().store16(d16.data(), s16.data(), a16.data(), w, add.data(), maxv);
      k.store16(d16.data(), s16.data(), b16.data(), w, add.data(), maxv);
      if (!same(a16.data(), b16.data(), w * 2)) {
        std::printf("%s store16 differs: w=%d maxv=%g\n", k.name, w, maxv);
        failures++;
      }
    }
  }
}

int main() {
  std::mt19937 rng(12345);
  const Kernels* sets[] = {
#if defined(AUTOADJUST_X86)
      &kernelsSSE2(), &kernelsAVX2(),
#endif
#if defined(AUTOADJUST_NEON)
      &kernelsNEON(),
#endif
  };
  for (const Kernels* k : sets) {
    for (int round = 0; round < 20; round++) check(*k, rng);
    std::printf("%s checked\n", k->name);
  }
  std::printf(failures ? "%d failure(s)\n" : "all kernels bit-identical to C\n", failures);
  return failures ? 1 : 0;
}
