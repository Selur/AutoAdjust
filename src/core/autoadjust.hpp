// SPDX-License-Identifier: GPL-2.0-or-later
// AutoAdjust core: statistics, temporal combination and pixel transform, independent of the host API.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "once_cache.hpp"

namespace autoadjust {

struct Kernels;

struct Params {
  bool auto_gain = false;
  float dark_limit = 1.5f;
  float bright_limit = 1.5f;
  float gamma_limit = 1.25f;
  float dark_exclude = 0.5f;
  float bright_exclude = 0.5f;
  int gain_mode = 0;
  int chroma_process = 100;
  float avg_safety = 0.25f;
  bool input_tv = true;
  bool output_tv = true;
  bool auto_balance = false;
  float chroma_limit = 1.05f;
  float balance_str = 0.75f;
  int scd_threshold = 12;
  int temporal_radius = 20;
  std::string change_status;
  bool use_interp = true;
  bool use_dither = true;
  bool high_quality = false;
  bool debug_view = false;
};

// Returns an empty string when valid, otherwise the error message (checks in the original order).
std::string validateParams(const Params& p);

struct ChangeRange {
  int first, last;
  bool scene_change;
};

// Parses change_status; returns false on a syntax error or a frame outside [0, frame_count).
bool parseChangeStatus(const std::string& text, int frame_count, std::vector<ChangeRange>& out);

// Sample storage of a plane as the host delivers it.
enum class SampleKind { U8, U16 };

struct ConstPlane {
  const uint8_t* data = nullptr;
  ptrdiff_t stride = 0;  // bytes
  int width = 0;
  int height = 0;
  SampleKind kind = SampleKind::U8;
};

struct Plane {
  uint8_t* data = nullptr;
  ptrdiff_t stride = 0;
  int width = 0;
  int height = 0;
  SampleKind kind = SampleKind::U8;
};

struct Format {
  int bits = 8;  // 8..16, native samples
  bool has_chroma = true;
  int ssw = 1;  // chroma subsampling shift, horizontal
  int ssh = 1;  // chroma subsampling shift, vertical
};

// One external_clip frame for the statistics; holder keeps the host's frame alive while the planes are read.
struct SourceFrame {
  ConstPlane y, u, v;
  std::shared_ptr<const void> holder;
};
using FetchFrame = std::function<SourceFrame(int k)>;

struct FrameStats {
  int status = 1;  // 1 = scene start, 2 = continuation
  float fmin = 0, fmax = 0, favg = 0, uavg = 0, vavg = 0;
};

struct Correction {
  float cmin = 0, cmax = 0, avg = 0;  // after the input_tv clamp
  float cmin_raw = 0, cmax_raw = 0;   // before it (debug view)
  float low = 0, high = 0, gamma = 1, cgain = 1, bal_u = 0, bal_v = 0;
};

class Engine {
 public:
  Engine(const Params& p, const Format& fmt, int luma_width, int luma_height, int frame_count,
         std::vector<ChangeRange> overrides, int asm_opt = 0);

  const Params& params() const { return p_; }
  const Format& format() const { return fmt_; }
  bool isPassthrough() const { return !p_.auto_gain && !p_.auto_balance; }

  // Statistics of external_clip frames [first, last]; every frame is computed once, whichever thread asks first.
  std::vector<FrameStats> windowStats(int first, int last, const FetchFrame& fetch) const;
  bool needsPrevious(int k) const;  // false when an override decides the status of k

  // Temporal combination of stats[k - first] for k in [first, first + stats.size()).
  Correction combine(int n, int first, const std::vector<FrameStats>& stats) const;
  // Frames of external_clip whose statistics combine() may need for output frame n.
  void window(int n, int& first, int& last) const;

  // Whether apply() writes plane i (0 = Y, 1 = U, 2 = V); the host copies or references the others.
  bool processesPlane(const Correction& c, int plane) const;
  // Writes the corrected planes processesPlane() selects from src to dst (src and dst may be the same).
  void apply(const Correction& c, const ConstPlane* src, Plane* dst, int plane_count) const;

 private:
  FrameStats computeStats(int k, const FetchFrame& fetch) const;
  std::shared_ptr<const std::vector<float>> lumaHistogram(int k, const ConstPlane& y) const;
  void lumaTable(const Correction& c, std::vector<float>& lut) const;
  void chromaTable(const Correction& c, float bal, std::vector<float>& lut) const;
  void processPlane(const std::vector<float>& lut, const ConstPlane& src, Plane& dst, int rx, int ry) const;

  Params p_;
  Format fmt_;
  int frame_count_;
  std::vector<ChangeRange> overrides_;
  int maxv_, bins_;
  float half_, tv_lo_, tv_hi_, thr_;
  int radius_;  // luma smoothing radius
  float dither_[64][64];
  float rounding_[64];
  const Kernels* k_;
  mutable OnceCache<FrameStats> stats_cache_{1024};
  mutable OnceCache<std::shared_ptr<const std::vector<float>>> hist_cache_{64};
};

}  // namespace autoadjust
