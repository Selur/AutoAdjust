// SPDX-License-Identifier: GPL-2.0-or-later
// AutoAdjust core implementation, following docs/SPEC.md (C reference path, single precision).
#include "autoadjust.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "bluenoise64.h"
#include "kernels.hpp"

namespace autoadjust {

namespace {

float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Correctly rounded single-precision log and pow (SPEC §3).
float logF(float x) { return static_cast<float>(std::log(static_cast<double>(x))); }
float powF(float t, float g) {
  return static_cast<float>(std::pow(static_cast<double>(t), static_cast<double>(g)));
}

void readRow(const ConstPlane& p, int y, uint16_t* out) {
  const uint8_t* row = p.data + p.stride * y;
  switch (p.kind) {
    case SampleKind::U8:
      for (int x = 0; x < p.width; x++) out[x] = row[x];
      break;
    case SampleKind::U16:
      std::memcpy(out, row, sizeof(uint16_t) * p.width);
      break;
  }
}

std::vector<float> histogram(const ConstPlane& p, int bins) {
  std::vector<uint32_t> count(bins, 0);
  std::vector<uint16_t> row(p.width);
  const int mask = bins - 1;
  for (int y = 0; y < p.height; y++) {
    readRow(p, y, row.data());
    for (int x = 0; x < p.width; x++) count[row[x] & mask]++;
  }
  const float inv = 1.0f / static_cast<float>(p.width * p.height);
  std::vector<float> h(bins);
  for (int v = 0; v < bins; v++) h[v] = static_cast<float>(count[v]) * inv;
  return h;
}

// Float sum in the order of the reference path: a sequential peel into lane 0, then four lanes,
// reduced as (L0 + L2) + (L1 + L3), then the tail (SPEC §5.3).
constexpr int kPeel = 2;

float laneSum4(const float* x, int n) {
  if (n < kPeel + 4) {
    float s = 0;
    for (int i = 0; i < n; i++) s += x[i];
    return s;
  }
  float lane[4] = {0, 0, 0, 0};
  for (int i = 0; i < kPeel; i++) lane[0] += x[i];
  const int end = n - (n - kPeel) % 4;
  for (int i = kPeel; i < end; i++) lane[(i - kPeel) % 4] += x[i];
  float s = (lane[0] + lane[2]) + (lane[1] + lane[3]);
  for (int i = end; i < n; i++) s += x[i];
  return s;
}

// Blocks of 8 with two 4-lane accumulators added lane-wise (SPEC §5.4, weighted sum).
float laneSum8Pair(const float* x, int n) {
  if (n < kPeel + 8) {
    float s = 0;
    for (int i = 0; i < n; i++) s += x[i];
    return s;
  }
  float a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0};
  for (int i = 0; i < kPeel; i++) a[0] += x[i];
  const int end = n - (n - kPeel) % 8;
  for (int blk = kPeel; blk < end; blk += 8) {
    for (int l = 0; l < 4; l++) a[l] += x[blk + l];
    for (int l = 0; l < 4; l++) b[l] += x[blk + 4 + l];
  }
  float s0 = a[0] + b[0], s1 = a[1] + b[1], s2 = a[2] + b[2], s3 = a[3] + b[3];
  float s = (s0 + s2) + (s1 + s3);
  for (int i = end; i < n; i++) s += x[i];
  return s;
}

// Blocks of 8 into a single 4-lane accumulator (SPEC §5.4, weight sum).
float laneSum8Single(const float* x, int n) {
  if (n < kPeel + 8) {
    float s = 0;
    for (int i = 0; i < n; i++) s += x[i];
    return s;
  }
  float a[4] = {0, 0, 0, 0};
  for (int i = 0; i < kPeel; i++) a[0] += x[i];
  const int end = n - (n - kPeel) % 8;
  for (int blk = kPeel; blk < end; blk += 8) {
    for (int l = 0; l < 4; l++) a[l] += x[blk + l];
    for (int l = 0; l < 4; l++) a[l] += x[blk + 4 + l];
  }
  float s = (a[0] + a[2]) + (a[1] + a[3]);
  for (int i = end; i < n; i++) s += x[i];
  return s;
}

void minMax(const std::vector<float>& h, int maxv, float td, float tb, float& fmin, float& fmax) {
  const float kFracMax = 0.999999f;
  fmin = 0;
  float c = 0;
  for (int v = 0; v <= maxv; v++) {
    c += h[v];
    if (c > td) {
      float frac = 0;
      if (v < maxv) {
        if (h[v] == 0) {
          frac = kFracMax;
        } else {
          frac = (((td - c) + h[v]) / h[v]) * (h[v + 1] / h[v]);
          if (frac >= kFracMax) frac = kFracMax;
        }
      }
      fmin = static_cast<float>(v) + frac;
      break;
    }
  }
  fmax = 0;
  c = 0;
  for (int v = maxv; v >= 0; v--) {
    c += h[v];
    if (c > tb) {
      float frac = 0;
      if (v >= 1) {
        if (h[v] == 0) {
          frac = kFracMax;
        } else {
          frac = (((tb - c) + h[v]) / h[v]) * (h[v - 1] / h[v]);
          if (frac >= kFracMax) frac = kFracMax;
        }
      }
      fmax = static_cast<float>(v) - frac;
      break;
    }
  }
}

float lumaAverage(const std::vector<float>& h, float fmin, float fmax) {
  const int n = static_cast<int>(h.size());
  auto cl = [&](int v) {
    const float fv = static_cast<float>(v);
    return fv < fmin ? fmin : (fmax < fv ? fmax : fv);
  };
  std::vector<float> prod(n);
  for (int v = 0; v < n; v++) prod[v] = cl(v) * h[v];
  const float mean = laneSum4(prod.data(), n) / laneSum4(h.data(), n);
  float med = 0, c = 0;
  for (int v = 0; v < n; v++) {
    c += h[v];
    if (c >= 0.5f) {
      med = cl(v);
      break;
    }
  }
  return 0.5f * (mean + med);
}

float chromaAverage(const std::vector<float>& h) {
  const int n = static_cast<int>(h.size());
  std::vector<float> prod(n);
  for (int v = 0; v < n; v++) prod[v] = static_cast<float>(v) * h[v];
  const float mean = laneSum8Pair(prod.data(), n) / laneSum8Single(h.data(), n);
  float med = 0, c = 0;
  for (int v = 0; v < n; v++) {
    c += h[v];
    if (c >= 0.5f) {
      med = static_cast<float>(v);
      break;
    }
  }
  return (mean + med) * 0.5f;
}

struct Weighted {
  float value, weight;
};

std::vector<Weighted> sortedCopy(const std::vector<Weighted>& list) {
  std::vector<Weighted> s(list);
  std::stable_sort(s.begin(), s.end(), [](const Weighted& a, const Weighted& b) { return a.value < b.value; });
  return s;
}

float weightSum(const std::vector<Weighted>& s) {
  float w = 0;
  for (const auto& e : s) w += e.weight;
  return w;
}

// Removes weight t from the bottom (or top) of the sorted list s, kept as [lo, hi).
void trimBottom(std::vector<Weighted>& s, size_t& lo, size_t hi, float t) {
  float c = 0;
  while (hi - lo >= 2) {
    c += s[lo].weight;
    if (t <= c) {
      s[lo].weight -= (c - t);
      return;
    }
    lo++;
  }
}

void trimTop(std::vector<Weighted>& s, size_t lo, size_t& hi, float t) {
  float c = 0;
  while (hi - lo >= 2) {
    c += s[hi - 1].weight;
    if (t <= c) {
      s[hi - 1].weight -= (c - t);
      return;
    }
    hi--;
  }
}

float trimmedMean(const std::vector<Weighted>& list, float q) {
  std::vector<Weighted> s = sortedCopy(list);
  const float t = (0.5f * q) * weightSum(s);
  size_t lo = 0, hi = s.size();
  if (t > 0) {
    trimBottom(s, lo, hi, t);
    trimTop(s, lo, hi, t);
  }
  float num = 0, den = 0;
  for (size_t i = lo; i < hi; i++) num += s[i].value * s[i].weight;
  for (size_t i = lo; i < hi; i++) den += s[i].weight;
  return num / den;
}

float lowPercentile(const std::vector<Weighted>& list) {
  std::vector<Weighted> s = sortedCopy(list);
  const float t = (0.5f * 0.5f) * weightSum(s);
  size_t lo = 0, hi = s.size();
  trimBottom(s, lo, hi, t);
  return s[lo].value;
}

float highPercentile(const std::vector<Weighted>& list) {
  std::vector<Weighted> s = sortedCopy(list);
  const float t = (0.5f * 0.5f) * weightSum(s);
  size_t lo = 0, hi = s.size();
  trimTop(s, lo, hi, t);
  return s[hi - 1].value;
}

float mid(const std::vector<Weighted>& list) {
  return (trimmedMean(list, 0.25f) + trimmedMean(list, 0.75f)) * 0.5f;
}

// First and last tap of one output position (SPEC §9.2), clamped to the plane.
void tapRange(int p, int length, int rho, int& start, int& end) {
  start = p < rho ? 0 : p - rho;
  end = std::min(p + rho, length - 1);
}

int bayer16(int row, int col) {
  // Recursive 2x2 construction [[0, 2], [3, 1]], transposed; the outermost quadrant is the lowest digit.
  int v = 0;
  for (int bit = 0; bit < 4; bit++) {
    const int r = (col >> bit) & 1, c = (row >> bit) & 1;
    v = v * 4 + (r == 0 ? (c == 0 ? 0 : 2) : (c == 0 ? 3 : 1));
  }
  return v;
}

}  // namespace

std::string validateParams(const Params& p) {
  if (p.dark_limit < 1.0f || p.dark_limit > 100.0f) return "AutoAdjust: dark_limit must be between 1.0 and 100.0";
  if (p.bright_limit < 1.0f || p.bright_limit > 100.0f)
    return "AutoAdjust: bright_limit must be between 1.0 and 100.0";
  if (p.gamma_limit < 1.0f || p.gamma_limit > 100.0f) return "AutoAdjust: gamma_limit must be between 1.0 and 100.0";
  if (p.dark_exclude < 0.0f || p.dark_exclude > 1.0f) return "AutoAdjust: dark_exclude must be between 0.0 and 1.0";
  if (p.bright_exclude < 0.0f || p.bright_exclude > 1.0f)
    return "AutoAdjust: bright_exclude must be between 0.0 and 1.0";
  if (static_cast<unsigned>(p.gain_mode) > 1) return "AutoAdjust: gain_mode must be 0 or 1";
  if (static_cast<unsigned>(p.chroma_process) > 200) return "AutoAdjust: chroma_process must be between 0 and 200";
  if (p.avg_safety < 0.0f || p.avg_safety > 1.0f) return "AutoAdjust: avg_safety must be between 0.0 and 1.0";
  if (p.chroma_limit < 1.0f || p.chroma_limit > 2.0f) return "AutoAdjust: chroma_limit must be between 1.0 and 2.0";
  if (p.balance_str < 0.0f || p.balance_str > 1.0f) return "AutoAdjust: balance_str must be between 0.0 and 1.0";
  if (static_cast<unsigned>(p.scd_threshold) > 100) return "AutoAdjust: scd_threshold must be between 0 and 100";
  if (p.temporal_radius < 0) return "AutoAdjust: temporal_radius must be >= 0";
  return {};
}

bool parseChangeStatus(const std::string& text, int frame_count, std::vector<ChangeRange>& out) {
  out.clear();
  if (text.empty() || text == "empty") return true;
  std::vector<std::string> tokens;
  size_t pos = 0;
  while (true) {
    const size_t semi = text.find(';', pos);
    if (semi == std::string::npos) {
      tokens.push_back(text.substr(pos));
      break;
    }
    tokens.push_back(text.substr(pos, semi - pos));
    pos = semi + 1;
    if (pos == text.size()) break;  // one trailing ';' is allowed
  }
  auto parseNumber = [](const std::string& s) {
    int v = 0;
    for (char ch : s) v = v * 10 + (ch - '0');
    return v;
  };
  for (const std::string& tok : tokens) {
    if (tok.empty()) return false;
    int dashes = 0, bangs = 0;
    for (char ch : tok) {
      if (ch == '-') dashes++;
      else if (ch == '!') bangs++;
      else if (ch < '0' || ch > '9') return false;
    }
    if (dashes > 1 || bangs > 1) return false;
    if (bangs == 1 && tok[0] != '!') return false;
    if (dashes == 1 && (tok.front() == '-' || tok.back() == '-')) return false;
    const bool keep = bangs == 1;
    const std::string body = keep ? tok.substr(1) : tok;
    if (body.empty()) return false;
    ChangeRange r{};
    const size_t dash = body.find('-');
    if (dash == std::string::npos) {
      r.first = r.last = parseNumber(body);
    } else {
      r.first = parseNumber(body.substr(0, dash));
      r.last = parseNumber(body.substr(dash + 1));
      if (r.last < r.first) return false;
    }
    if (r.first < 0 || r.last >= frame_count) return false;
    r.scene_change = !keep;
    out.push_back(r);
  }
  return true;
}

Engine::Engine(const Params& p, const Format& fmt, int luma_width, int luma_height, int frame_count,
               std::vector<ChangeRange> overrides, int asm_opt)
    : p_(p), fmt_(fmt), frame_count_(frame_count), overrides_(std::move(overrides)), k_(&kernelsFor(asm_opt)) {
  std::fill(std::begin(rounding_), std::end(rounding_), 0.5f);
  const int b = fmt_.bits;
  maxv_ = (1 << b) - 1;
  bins_ = 1 << b;
  half_ = static_cast<float>(1 << (b - 1));
  tv_lo_ = static_cast<float>(16 << (b - 8));
  tv_hi_ = static_cast<float>(235 << (b - 8));
  thr_ = std::ldexp(1.0f, b - 9);

  const int step = p_.high_quality ? 1 : 2;
  const int base = ((luma_width + luma_height) * 16) >> 11;
  const int q = base / step, m = base % step;
  radius_ = std::clamp(q * step + (m >= (step + 1) / 2 ? step : 0), 8, 32);

  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++)
      dither_[y][x] = clampf((static_cast<float>(bayer16(y % 16, x % 16)) / 256.0f) * 0.25f +
                                 (static_cast<float>(kBlueNoise64[y][x]) / 4096.0f) * 0.75f,
                             0.0f, 1.0f);
}

bool Engine::needsPrevious(int k) const {
  if (k == 0) return false;
  for (const auto& r : overrides_)
    if (k >= r.first && k <= r.last) return false;
  return true;
}

FrameStats Engine::frameStats(int k, const ConstPlane& y, const ConstPlane* prev_y, const ConstPlane* u,
                              const ConstPlane* v) const {
  FrameStats s;
  const std::vector<float> h = histogram(y, bins_);
  minMax(h, maxv_, p_.dark_exclude / 100.0f, p_.bright_exclude / 100.0f, s.fmin, s.fmax);
  s.favg = lumaAverage(h, s.fmin, s.fmax);
  if (p_.auto_balance && fmt_.has_chroma && u && v) {
    s.uavg = chromaAverage(histogram(*u, bins_));
    s.vavg = chromaAverage(histogram(*v, bins_));
  }

  int status = 0;
  for (const auto& r : overrides_)  // the last matching entry wins
    if (k >= r.first && k <= r.last) status = r.scene_change ? 1 : 2;
  if (status == 0 && k == 0) status = 1;
  if (status == 0 && prev_y) {
    const std::vector<float> hp = histogram(*prev_y, bins_);
    uint64_t sad = 0;
    std::vector<uint16_t> a(y.width), b(y.width);
    for (int row = 0; row < y.height; row++) {
      readRow(y, row, a.data());
      readRow(*prev_y, row, b.data());
      for (int x = 0; x < y.width; x++) sad += a[x] > b[x] ? a[x] - b[x] : b[x] - a[x];
    }
    const float da = static_cast<float>(sad) / (static_cast<float>(y.width * y.height) * static_cast<float>(maxv_));
    float hd = 0;
    for (int i = 0; i < bins_; i++) hd += std::fabs(h[i] - hp[i]);
    const float db = 0.5f * hd;
    const float t1 = static_cast<float>(p_.scd_threshold) / 100.0f;
    const float t2 = (2.5f * static_cast<float>(p_.scd_threshold)) / 100.0f;
    status = (da > t1 || db > t2) ? 1 : 2;
  }
  s.status = status == 0 ? 1 : status;
  return s;
}

void Engine::window(int n, int& first, int& last) const {
  const int r = p_.temporal_radius;
  first = std::max(0, n - r);
  last = std::min(frame_count_ - 1, n + r);
}

Correction Engine::combine(int n, int first, const std::vector<FrameStats>& stats) const {
  const int r = p_.temporal_radius;
  auto at = [&](int k) -> const FrameStats& { return stats[k - first]; };
  std::vector<Weighted> lavg, lmin, lmax, lu, lv;
  auto push = [&](const FrameStats& s, float w) {
    lavg.push_back({s.favg, w});
    lmin.push_back({s.fmin, w});
    lmax.push_back({s.fmax, w});
    lu.push_back({s.uavg, w});
    lv.push_back({s.vavg, w});
  };
  auto weight = [&](int i) { return static_cast<float>(r - i) / static_cast<float>(2 * r) + 0.5f; };

  push(at(n), 1.0f);
  if (at(n).status == 2 && r > 0) {
    for (int i = 1; i <= r; i++) {
      const int k = n - i;
      if (k < 0) continue;
      push(at(k), weight(i));
      if (at(k).status == 1) break;
    }
  }
  for (int i = 1; i <= r; i++) {
    const int k = n + i;
    if (k >= frame_count_) continue;
    if (at(k).status == 1) break;
    push(at(k), weight(i));
  }

  Correction c;
  const float s = p_.avg_safety;
  float avg = mid(lavg);
  float cmin = ((1 - s) * 0.5f) * (trimmedMean(lmin, 0.25f) + trimmedMean(lmin, 0.75f)) + s * lowPercentile(lmin);
  float cmax = ((1 - s) * 0.5f) * (trimmedMean(lmax, 0.25f) + trimmedMean(lmax, 0.75f)) + s * highPercentile(lmax);
  float cu = 0, cv = 0;
  if (p_.auto_balance) {
    cu = mid(lu);
    cv = mid(lv);
  }
  if (std::fabs(cmin - cmax) < 1e-6f) cmin = cmax = avg;
  c.cmin_raw = cmin;
  c.cmax_raw = cmax;

  const float maxv = static_cast<float>(maxv_);
  float low = clampf(((cmin - avg) * p_.dark_limit) + avg, 0, maxv);
  float high = clampf(((cmax - avg) * p_.bright_limit) + avg, 0, maxv);
  if (p_.input_tv) {
    cmin = clampf(cmin, tv_lo_, tv_hi_);
    cmax = clampf(cmax, tv_lo_, tv_hi_);
    avg = clampf(avg, tv_lo_, tv_hi_);
  }
  if (p_.output_tv) {
    low = clampf(low, tv_lo_, tv_hi_);
    high = clampf(high, tv_lo_, tv_hi_);
  }
  const float sum = p_.gain_mode == 0 ? (low + high) : (cmin + cmax);
  const float target = (((1 - (1 / p_.gamma_limit)) * ((0.5f * sum) - avg)) + avg) / maxv;
  float gamma = logF(target) / logF(avg / maxv);
  if (!std::isfinite(gamma)) gamma = 1.0f;  // black or full-scale averages (SPEC §16.3)

  float cgain = 1;
  if (p_.auto_gain && p_.chroma_process >= 1 && high != low && cmax != cmin)
    cgain = ((((high - low) / (cmax - cmin)) - 1) * 0.8515f) * (static_cast<float>(p_.chroma_process) / 100.0f) + 1;

  if (p_.auto_balance) {
    const float lim = (p_.chroma_limit - 1) * half_;
    c.bal_u = clampf((half_ - cu) * p_.balance_str, -lim, lim);
    c.bal_v = clampf((half_ - cv) * p_.balance_str, -lim, lim);
  }
  c.cmin = cmin;
  c.cmax = cmax;
  c.avg = avg;
  c.low = low;
  c.high = high;
  c.gamma = gamma;
  c.cgain = cgain;
  return c;
}

void Engine::apply(const Correction& c, Plane* planes, int plane_count) const {
  if (p_.auto_gain && plane_count > 0) processLuma(c, planes[0]);
  if (fmt_.has_chroma && plane_count >= 3 && (p_.auto_balance || p_.chroma_process >= 1)) {
    // cgain == 1 and bal == 0 give a zero delta everywhere, i.e. an exact identity.
    if (c.cgain != 1.0f || c.bal_u != 0.0f) processChroma(c, planes[1], c.bal_u);
    if (c.cgain != 1.0f || c.bal_v != 0.0f) processChroma(c, planes[2], c.bal_v);
  }
}

// The delta depends only on the sample value, so it is computed once per possible value (SPEC §8).
std::vector<float> Engine::lumaTable(const Correction& c, int size) const {
  std::vector<float> lut(size);
  const float avg = c.avg, cmin = c.cmin, cmax = c.cmax, low = c.low, high = c.high, gamma = c.gamma;
  for (int i = 0; i < size; i++) {
    const float v = static_cast<float>(i);
    float r;
    if (p_.gain_mode == 0) {
      float up, dn;
      if (cmax == high || cmax <= avg || high <= avg) up = v;
      else up = ((v - avg) * (high - avg)) / (cmax - avg) + avg;
      if (cmin == low || avg <= cmin || avg <= low) dn = v;
      else dn = ((v - cmin) * (avg - low)) / (avg - cmin) + low;
      r = v < avg ? dn : (v > avg ? up : v);
      if (low != high && gamma != 1.0f && low <= r && r <= high)
        r = powF((r - low) / (high - low), gamma) * (high - low) + low;
    } else {
      if (cmin == cmax || low == high) {
        r = v;
      } else {
        float t = (v - cmin) / (cmax - cmin);
        if (0 <= t && t <= 1) t = powF(t, gamma);
        r = t * (high - low) + low;
      }
    }
    lut[i] = r - v;
  }
  return lut;
}

std::vector<float> Engine::chromaTable(const Correction& c, float bal, int size) const {
  std::vector<float> lut(size);
  const float half = half_, g1 = c.cgain - 1;
  for (int i = 0; i < size; i++) {
    const float v = static_cast<float>(i);
    const float xv = v - half;
    const float r = ((xv * (((1 - std::fabs(xv / half)) * g1) + 1)) + bal) + half;
    lut[i] = r - v;
  }
  return lut;
}

void Engine::deltaFromTable(const std::vector<float>& lut, const Plane& pl, std::vector<float>& d) const {
  const int w = pl.width, h = pl.height;
  d.resize(static_cast<size_t>(w) * h);
  const float* table = lut.data();
  for (int y = 0; y < h; y++) {
    const uint8_t* row = pl.data + pl.stride * y;
    float* out = d.data() + static_cast<size_t>(y) * w;
    if (pl.kind == SampleKind::U8) {
      for (int x = 0; x < w; x++) out[x] = table[row[x]];
    } else {
      const uint16_t* r16 = reinterpret_cast<const uint16_t*>(row);
      for (int x = 0; x < w; x++) out[x] = table[r16[x]];
    }
  }
}

// The table covers every value the sample type can hold, so out-of-range samples are handled like in the formula.
static int tableSize(const Plane& pl) { return pl.kind == SampleKind::U8 ? 256 : 65536; }

void Engine::processLuma(const Correction& c, Plane& pl) const {
  std::vector<float> d;
  deltaFromTable(lumaTable(c, tableSize(pl)), pl, d);
  if (p_.use_interp) smooth(d, pl.width, pl.height, radius_, radius_);
  store(d, pl);
}

void Engine::processChroma(const Correction& c, Plane& pl, float bal) const {
  std::vector<float> d;
  deltaFromTable(chromaTable(c, bal, tableSize(pl)), pl, d);
  if (p_.use_interp) smooth(d, pl.width, pl.height, radius_ >> fmt_.ssw, radius_ >> fmt_.ssh);
  store(d, pl);
}

void Engine::smooth(std::vector<float>& d, int w, int h, int rx, int ry) const {
  const int step = p_.high_quality ? 1 : 2;
  const float thr = thr_;
  std::vector<float> t(d.size());

  // Vertical pass d -> t: per output row the tap rows are the same for every column.
  {
    const float inv = 1.0f / (static_cast<float>(2 * ry) / static_cast<float>(step) + 1.0f);
    std::vector<const float*> taps;
    for (int p = 0; p < h; p++) {
      int start, end;
      tapRange(p, h, ry, start, end);
      taps.clear();
      for (int q = start; q <= end; q += step) taps.push_back(d.data() + static_cast<size_t>(q) * w);
      const int n = static_cast<int>(taps.size());
      const bool interior = p >= ry && p < h - ry;
      k_->vpass(taps.data(), n, d.data() + static_cast<size_t>(p) * w, t.data() + static_cast<size_t>(p) * w, w, thr,
                interior ? inv : static_cast<float>(n), !interior);
    }
  }

  // Horizontal pass t -> d: the borders here, the interior in the kernel.
  {
    const float inv = 1.0f / (static_cast<float>(2 * rx) / static_cast<float>(step) + 1.0f);
    const int interior_taps = (2 * rx) / step + 1;
    const int p0 = rx, p1 = w - rx;
    std::vector<float> tap(2 * 32 + 1);
    auto border = [&](const float* src, float* out, int p) {
      int start, end;
      tapRange(p, w, rx, start, end);
      const float lo = src[p] - thr, hi = src[p] + thr;
      int n = 0;
      for (int q = start; q <= end; q += step) tap[n++] = std::min(std::max(src[q], lo), hi);
      float sum;
      if (n >= 8) {
        const int n8 = n & ~7;
        float a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0};
        for (int j = 0; j < n8; j += 8) {
          for (int l = 0; l < 4; l++) a[l] += tap[j + l];
          for (int l = 0; l < 4; l++) b[l] += tap[j + 4 + l];
        }
        const float s0 = a[0] + b[0], s1 = a[1] + b[1], s2 = a[2] + b[2], s3 = a[3] + b[3];
        sum = (s0 + s2) + (s1 + s3);
        for (int j = n8; j < n; j++) sum += tap[j];
      } else {
        sum = 0;
        for (int j = 0; j < n; j++) sum += tap[j];
      }
      out[p] = sum / static_cast<float>(n);
    };
    for (int y = 0; y < h; y++) {
      const float* src = t.data() + static_cast<size_t>(y) * w;
      float* out = d.data() + static_cast<size_t>(y) * w;
      if (p0 < p1) {
        for (int p = 0; p < p0; p++) border(src, out, p);
        k_->hpass(src, out, p0, p1, rx, step, interior_taps, thr, inv);
        for (int p = p1; p < w; p++) border(src, out, p);
      } else {
        for (int p = 0; p < w; p++) border(src, out, p);
      }
    }
  }
}

void Engine::store(const std::vector<float>& d, Plane& pl) const {
  const int w = pl.width, h = pl.height;
  const float maxv = static_cast<float>(maxv_);
  for (int y = 0; y < h; y++) {
    const float* dr = d.data() + static_cast<size_t>(y) * w;
    const float* add = p_.use_dither ? dither_[y & 63] : rounding_;
    uint8_t* row = pl.data + pl.stride * y;
    if (pl.kind == SampleKind::U8) {
      k_->store8(dr, row, row, w, add);
    } else {
      uint16_t* r16 = reinterpret_cast<uint16_t*>(row);
      k_->store16(dr, r16, r16, w, add, maxv);
    }
  }
}

}  // namespace autoadjust
