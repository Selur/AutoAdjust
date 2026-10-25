// SPDX-License-Identifier: GPL-2.0-or-later
// AutoAdjust core: statistics, temporal combination and pixel transform, independent of the host API.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

  // Statistics of external_clip frame k; prev_y is the luma of frame k-1 (only read when needsPrevious(k)).
  FrameStats frameStats(int k, const ConstPlane& y, const ConstPlane* prev_y, const ConstPlane* u,
                        const ConstPlane* v) const;
  bool needsPrevious(int k) const;  // false when an override decides the status of k

  // Temporal combination of stats[k - first] for k in [first, first + stats.size()).
  Correction combine(int n, int first, const std::vector<FrameStats>& stats) const;
  // Frames of external_clip whose statistics combine() may need for output frame n.
  void window(int n, int& first, int& last) const;

  // Applies the correction to the planes of dst (already holding a copy of the input frame).
  void apply(const Correction& c, Plane* planes, int plane_count) const;

 private:
  std::vector<float> lumaTable(const Correction& c, int size) const;
  std::vector<float> chromaTable(const Correction& c, float bal, int size) const;
  void deltaFromTable(const std::vector<float>& lut, const Plane& pl, std::vector<float>& d) const;
  void processLuma(const Correction& c, Plane& pl) const;
  void processChroma(const Correction& c, Plane& pl, float bal) const;
  void smooth(std::vector<float>& d, int w, int h, int rx, int ry) const;
  void store(const std::vector<float>& d, Plane& pl) const;

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
};

}  // namespace autoadjust
