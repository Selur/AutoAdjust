// SPDX-License-Identifier: GPL-2.0-or-later
// CPU detection and kernel selection for asm_opt.
#include "kernels.hpp"

#if defined(AUTOADJUST_X86)
#if defined(_MSC_VER)
#include <immintrin.h>
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace autoadjust {

namespace {

#if defined(AUTOADJUST_X86)
bool cpuHasAvx2() {
  int r[4];
#if defined(_MSC_VER)
  __cpuid(r, 0);
  if (r[0] < 7) return false;
  __cpuid(r, 1);
#else
  unsigned a, b, c, d;
  if (__get_cpuid_max(0, nullptr) < 7) return false;
  __cpuid(1, a, b, c, d);
  r[2] = static_cast<int>(c);
#endif
  const bool osxsave = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
  if (!osxsave || !avx) return false;
#if defined(_MSC_VER)
  const unsigned long long xcr0 = _xgetbv(0);
  __cpuidex(r, 7, 0);
#else
  unsigned lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  const unsigned long long xcr0 = (static_cast<unsigned long long>(hi) << 32) | lo;
  __cpuid_count(7, 0, a, b, c, d);
  r[1] = static_cast<int>(b);
#endif
  // The OS must save the XMM and YMM registers.
  if ((xcr0 & 6) != 6) return false;
  return (r[1] & (1 << 5)) != 0;
}
#endif

}  // namespace

bool simdAvailable(int asm_opt) {
  switch (asm_opt) {
    case 0:
    case 1:
      return true;
#if defined(AUTOADJUST_X86)
    case 2:
      return true;  // part of x86-64
    case 3:
      return cpuHasAvx2();
#endif
    default:
      return false;
  }
}

const Kernels& kernelsFor(int asm_opt) {
#if defined(AUTOADJUST_X86)
  if (asm_opt == 0) asm_opt = cpuHasAvx2() ? 3 : 2;
  if (asm_opt == 3) return kernelsAVX2();
  if (asm_opt == 2) return kernelsSSE2();
#elif defined(AUTOADJUST_NEON)
  if (asm_opt == 0) return kernelsNEON();
#endif
  return kernelsC();
}

}  // namespace autoadjust
