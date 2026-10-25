// SPDX-License-Identifier: GPL-2.0-or-later
// Pixel kernels per instruction set; every set gives results bit-identical to the C kernels.
#pragma once

#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define AUTOADJUST_X86 1
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
#define AUTOADJUST_NEON 1
#endif

namespace autoadjust {

// One output row of the vertical smoothing pass over the n tap rows (SPEC §9.3 order), scaled by factor or divided by it.
using VPassFn = void (*)(const float* const* taps, int n, const float* center, float* out, int w, float thr, float factor,
                         bool divide);
// Interior positions [p0, p1) of the horizontal smoothing pass: n taps at p - rho + k * step, times inv.
using HPassFn = void (*)(const float* src, float* out, int p0, int p1, int rho, int step, int n, float thr, float inv);
// One output row: trunc(clamp((d + v) + add[x & 63], 0, maxv)).
using Store8Fn = void (*)(const float* d, const uint8_t* src, uint8_t* dst, int w, const float* add);
using Store16Fn = void (*)(const float* d, const uint16_t* src, uint16_t* dst, int w, const float* add, float maxv);
// Sum of absolute differences of one row (scene change detection, SPEC §5.5).
using Sad8Fn = uint64_t (*)(const uint8_t* a, const uint8_t* b, int w);
using Sad16Fn = uint64_t (*)(const uint16_t* a, const uint16_t* b, int w);

struct Kernels {
  const char* name;
  VPassFn vpass;
  HPassFn hpass;
  Store8Fn store8;
  Store16Fn store16;
  Sad8Fn sad8;
  Sad16Fn sad16;
};

// asm_opt values: 0 = best available, 1 = C, 2 = SSE2, 3 = AVX2; on arm64 0 is NEON.
bool simdAvailable(int asm_opt);
const Kernels& kernelsFor(int asm_opt);

const Kernels& kernelsC();
#if defined(AUTOADJUST_X86)
const Kernels& kernelsSSE2();
const Kernels& kernelsAVX2();
#endif
#if defined(AUTOADJUST_NEON)
const Kernels& kernelsNEON();
#endif

}  // namespace autoadjust
