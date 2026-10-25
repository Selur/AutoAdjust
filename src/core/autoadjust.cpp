// SPDX-License-Identifier: GPL-2.0-or-later
// AutoAdjust core implementation, following docs/SPEC.md (C reference path, single precision).
#include "autoadjust.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>

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

// First and last tap of one output position (SPEC §9.2), clamped to the plane.
void tapRange(int p, int length, int rho, int& start, int& end) {
  start = p < rho ? 0 : p - rho;
  end = std::min(p + rho, length - 1);
}

template <typename T>
void countSamples(const ConstPlane& p, int bins, std::vector<uint32_t>& count) {
  // Four partial histograms (for small bin counts) keep repeated values from serialising the increments.
  const int parts = bins <= 4096 ? 4 : 1;
  thread_local std::vector<uint32_t> part;
  part.assign(static_cast<size_t>(parts) * bins, 0);
  const unsigned mask = static_cast<unsigned>(bins - 1);
  uint32_t* c0 = part.data();
  uint32_t* c1 = part.data() + (parts > 1 ? bins : 0);
  uint32_t* c2 = part.data() + (parts > 1 ? 2 * bins : 0);
  uint32_t* c3 = part.data() + (parts > 1 ? 3 * bins : 0);
  for (int y = 0; y < p.height; y++) {
    const T* row = reinterpret_cast<const T*>(p.data + p.stride * y);
    int x = 0;
    for (; x + 4 <= p.width; x += 4) {
      c0[row[x] & mask]++;
      c1[row[x + 1] & mask]++;
      c2[row[x + 2] & mask]++;
      c3[row[x + 3] & mask]++;
    }
    for (; x < p.width; x++) c0[row[x] & mask]++;
  }
  count.assign(bins, 0);
  for (int k = 0; k < parts; k++)
    for (int v = 0; v < bins; v++) count[v] += part[static_cast<size_t>(k) * bins + v];
}

std::vector<float> histogram(const ConstPlane& p, int bins) {
  std::vector<uint32_t> count;
  if (p.kind == SampleKind::U8) countSamples<uint8_t>(p, bins, count);
  else countSamples<uint16_t>(p, bins, count);
  const float inv = 1.0f / static_cast<float>(p.width * p.height);
  std::vector<float> h(bins);
  for (int v = 0; v < bins; v++) h[v] = static_cast<float>(count[v]) * inv;
  return h;
}

// Clamped box filter for one border position of the horizontal pass (SPEC §9.2).
void hBorder(const float* src, float* out, int p, int w, int rx, int step, float thr) {
  int start, end;
  tapRange(p, w, rx, start, end);
  const float lo = src[p] - thr, hi = src[p] + thr;
  float tap[2 * 32 + 1];
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

std::shared_ptr<const std::vector<float>> Engine::lumaHistogram(int k, const ConstPlane& y) const {
  return hist_cache_.get(k, [&] { return std::make_shared<const std::vector<float>>(histogram(y, bins_)); });
}

FrameStats Engine::computeStats(int k, const FetchFrame& fetch) const {
  const SourceFrame cur = fetch(k);
  const auto hist = lumaHistogram(k, cur.y);
  const std::vector<float>& h = *hist;
  FrameStats s;
  minMax(h, maxv_, p_.dark_exclude / 100.0f, p_.bright_exclude / 100.0f, s.fmin, s.fmax);
  s.favg = lumaAverage(h, s.fmin, s.fmax);
  if (p_.auto_balance && fmt_.has_chroma && cur.u.data && cur.v.data) {
    s.uavg = chromaAverage(histogram(cur.u, bins_));
    s.vavg = chromaAverage(histogram(cur.v, bins_));
  }

  int status = 0;
  for (const auto& r : overrides_)  // the last matching entry wins
    if (k >= r.first && k <= r.last) status = r.scene_change ? 1 : 2;
  if (status == 0 && k == 0) status = 1;
  if (status == 0 && needsPrevious(k)) {
    const SourceFrame prev = fetch(k - 1);
    const auto hp = lumaHistogram(k - 1, prev.y);
    const ConstPlane& a = cur.y;
    const ConstPlane& b = prev.y;
    uint64_t sad = 0;
    for (int row = 0; row < a.height; row++) {
      const uint8_t* ra = a.data + a.stride * row;
      const uint8_t* rb = b.data + b.stride * row;
      if (a.kind == SampleKind::U8) sad += k_->sad8(ra, rb, a.width);
      else sad += k_->sad16(reinterpret_cast<const uint16_t*>(ra), reinterpret_cast<const uint16_t*>(rb), a.width);
    }
    const float da = static_cast<float>(sad) / (static_cast<float>(a.width * a.height) * static_cast<float>(maxv_));
    float hd = 0;
    for (int i = 0; i < bins_; i++) hd += std::fabs(h[i] - (*hp)[i]);
    const float db = 0.5f * hd;
    const float t1 = static_cast<float>(p_.scd_threshold) / 100.0f;
    const float t2 = (2.5f * static_cast<float>(p_.scd_threshold)) / 100.0f;
    status = (da > t1 || db > t2) ? 1 : 2;
  }
  s.status = status == 0 ? 1 : status;
  return s;
}

std::vector<FrameStats> Engine::windowStats(int first, int last, const FetchFrame& fetch) const {
  const int n = last - first + 1;
  std::vector<std::shared_future<FrameStats>> futures(n);
  // Compute every frame this thread claims first, then wait for those other threads are computing.
  for (int i = 0; i < n; i++)
    if (auto promise = stats_cache_.claim(first + i, futures[i]))
      stats_cache_.fulfil(first + i, *promise, [&] { return computeStats(first + i, fetch); });
  std::vector<FrameStats> stats(n);
  for (int i = 0; i < n; i++) stats[i] = futures[i].get();
  return stats;
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

bool Engine::processesPlane(const Correction& c, int plane) const {
  if (plane == 0) return p_.auto_gain;
  if (plane > 2 || !fmt_.has_chroma || !(p_.auto_balance || p_.chroma_process >= 1)) return false;
  // cgain == 1 and bal == 0 give a zero delta everywhere, i.e. an exact identity.
  const float bal = plane == 1 ? c.bal_u : c.bal_v;
  return c.cgain != 1.0f || bal != 0.0f;
}

void Engine::apply(const Correction& c, const ConstPlane* src, Plane* dst, int plane_count) const {
  thread_local std::vector<float> lut;
  for (int i = 0; i < plane_count && i < 3; i++) {
    if (!processesPlane(c, i)) continue;
    lut.resize(src[i].kind == SampleKind::U8 ? 256 : 65536);
    if (i == 0) {
      lumaTable(c, lut);
      processPlane(lut, src[i], dst[i], radius_, radius_);
    } else {
      chromaTable(c, i == 1 ? c.bal_u : c.bal_v, lut);
      processPlane(lut, src[i], dst[i], radius_ >> fmt_.ssw, radius_ >> fmt_.ssh);
    }
  }
}

// The delta depends only on the sample value, so it is computed once per possible value (SPEC §8); the table
// covers every value the sample type can hold, so out-of-range samples are handled like in the formula.
void Engine::lumaTable(const Correction& c, std::vector<float>& lut) const {
  const float avg = c.avg, cmin = c.cmin, cmax = c.cmax, low = c.low, high = c.high, gamma = c.gamma;
  for (size_t i = 0; i < lut.size(); i++) {
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
}

void Engine::chromaTable(const Correction& c, float bal, std::vector<float>& lut) const {
  const float half = half_, g1 = c.cgain - 1;
  for (size_t i = 0; i < lut.size(); i++) {
    const float v = static_cast<float>(i);
    const float xv = v - half;
    const float r = ((xv * (((1 - std::fabs(xv / half)) * g1) + 1)) + bal) + half;
    lut[i] = r - v;
  }
}

// Row by row: deltas from the table into a ring of 2 * ry + 1 rows, vertical pass, horizontal pass, store.
void Engine::processPlane(const std::vector<float>& lut, const ConstPlane& src, Plane& dst, int rx, int ry) const {
  const int w = src.width, h = src.height;
  const float* table = lut.data();
  thread_local std::vector<float> ring, vrow, hrow;
  thread_local std::vector<const float*> taps;
  auto deltaRow = [&](int y, float* out) {
    const uint8_t* row = src.data + src.stride * y;
    if (src.kind == SampleKind::U8) {
      for (int x = 0; x < w; x++) out[x] = table[row[x]];
    } else {
      const uint16_t* r16 = reinterpret_cast<const uint16_t*>(row);
      for (int x = 0; x < w; x++) out[x] = table[r16[x]];
    }
  };
  auto storeRow = [&](int y, const float* d) {
    const float* add = p_.use_dither ? dither_[y & 63] : rounding_;
    const uint8_t* in = src.data + src.stride * y;
    uint8_t* out = dst.data + dst.stride * y;
    if (src.kind == SampleKind::U8) {
      k_->store8(d, in, out, w, add);
    } else {
      k_->store16(d, reinterpret_cast<const uint16_t*>(in), reinterpret_cast<uint16_t*>(out), w, add,
                  static_cast<float>(maxv_));
    }
  };
  vrow.resize(w);
  if (!p_.use_interp) {
    for (int y = 0; y < h; y++) {
      deltaRow(y, vrow.data());
      storeRow(y, vrow.data());
    }
    return;
  }

  const int step = p_.high_quality ? 1 : 2;
  const int ring_rows = 2 * ry + 1;
  ring.resize(static_cast<size_t>(ring_rows) * w);
  hrow.resize(w);
  auto slot = [&](int q) { return ring.data() + static_cast<size_t>(q % ring_rows) * w; };
  const float vinv = 1.0f / (static_cast<float>(2 * ry) / static_cast<float>(step) + 1.0f);
  const float hinv = 1.0f / (static_cast<float>(2 * rx) / static_cast<float>(step) + 1.0f);
  const int interior_taps = (2 * rx) / step + 1;
  const int p0 = rx, p1 = w - rx;
  int next = 0;
  for (int p = 0; p < h; p++) {
    int start, end;
    tapRange(p, h, ry, start, end);
    for (; next <= end; next++) deltaRow(next, slot(next));
    taps.clear();
    for (int q = start; q <= end; q += step) taps.push_back(slot(q));
    const int n = static_cast<int>(taps.size());
    const bool interior = p >= ry && p < h - ry;
    k_->vpass(taps.data(), n, slot(p), vrow.data(), w, thr_, interior ? vinv : static_cast<float>(n), !interior);
    if (p0 < p1) {
      for (int x = 0; x < p0; x++) hBorder(vrow.data(), hrow.data(), x, w, rx, step, thr_);
      k_->hpass(vrow.data(), hrow.data(), p0, p1, rx, step, interior_taps, thr_, hinv);
      for (int x = p1; x < w; x++) hBorder(vrow.data(), hrow.data(), x, w, rx, step, thr_);
    } else {
      for (int x = 0; x < w; x++) hBorder(vrow.data(), hrow.data(), x, w, rx, step, thr_);
    }
    storeRow(p, hrow.data());
  }
}

}  // namespace autoadjust
