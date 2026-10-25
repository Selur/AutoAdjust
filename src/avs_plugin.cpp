// SPDX-License-Identifier: GPL-2.0-or-later
// AviSynth+ interface of AutoAdjust (name and arguments of the original 2.60 plugin, without high_bitdepth).
#include <memory>
#include <string>
#include <vector>

#include "avisynth.h"
#include "core/autoadjust.hpp"
#include "core/kernels.hpp"
#include "version.hpp"

using namespace autoadjust;

namespace {

const int kPlanes[3] = {PLANAR_Y, PLANAR_U, PLANAR_V};

class AutoAdjustAvs : public GenericVideoFilter {
 public:
  AutoAdjustAvs(PClip child, PClip external, const Params& p, std::vector<ChangeRange> overrides, int asm_opt,
                IScriptEnvironment* env)
      : GenericVideoFilter(child), external_(external) {
    try {
      env->CheckVersion(8);
      has_v8_ = true;
    } catch (const AvisynthError&) {
      has_v8_ = false;
    }
    const VideoInfo& evi = external_->GetVideoInfo();
    ext_frames_ = evi.num_frames;
    fmt_.bits = vi.BitsPerComponent();
    fmt_.has_chroma = !vi.IsY();
    fmt_.ssw = fmt_.has_chroma ? vi.GetPlaneWidthSubsampling(PLANAR_U) : 0;
    fmt_.ssh = fmt_.has_chroma ? vi.GetPlaneHeightSubsampling(PLANAR_U) : 0;
    engine_ = std::make_unique<Engine>(p, fmt_, vi.width, vi.height, vi.num_frames, std::move(overrides), asm_opt);
  }

  PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) override {
    n = n < 0 ? 0 : (n >= vi.num_frames ? vi.num_frames - 1 : n);
    if (engine_->isPassthrough()) return child->GetFrame(n, env);
    int first, last;
    engine_->window(n, first, last);
    const FetchFrame fetch = [this, env](int k) { return sourceFrame(k, env); };
    const Correction c = engine_->combine(n, first, engine_->windowStats(first, last, fetch));

    PVideoFrame src = child->GetFrame(n, env);
    PVideoFrame dst = has_v8_ ? env->NewVideoFrameP(vi, &src) : env->NewVideoFrame(vi);
    const int count = fmt_.has_chroma ? 3 : 1;
    ConstPlane in[3];
    Plane out[3];
    for (int i = 0; i < count; i++) {
      in[i] = readPlane(src, kPlanes[i]);
      out[i] = writablePlane(dst, kPlanes[i]);
      if (!engine_->processesPlane(c, i)) copyPlane(src, dst, kPlanes[i], env);
    }
    if (vi.IsYUVA()) copyPlane(src, dst, PLANAR_A, env);
    engine_->apply(c, in, out, count);
    return dst;
  }

  int __stdcall SetCacheHints(int cachehints, int frame_range) override {
    (void)frame_range;
    return cachehints == CACHE_GET_MTMODE ? MT_NICE_FILTER : 0;
  }

 private:
  SampleKind kind() const { return fmt_.bits > 8 ? SampleKind::U16 : SampleKind::U8; }

  ConstPlane readPlane(const PVideoFrame& f, int plane) const {
    ConstPlane p;
    p.data = f->GetReadPtr(plane);
    p.stride = f->GetPitch(plane);
    p.kind = kind();
    p.width = f->GetRowSize(plane) / (p.kind == SampleKind::U16 ? 2 : 1);
    p.height = f->GetHeight(plane);
    return p;
  }

  Plane writablePlane(PVideoFrame& f, int plane) const {
    const ConstPlane c = readPlane(f, plane);
    return {f->GetWritePtr(plane), f->GetPitch(plane), c.width, c.height, c.kind};
  }

  static void copyPlane(const PVideoFrame& src, PVideoFrame& dst, int plane, IScriptEnvironment* env) {
    env->BitBlt(dst->GetWritePtr(plane), dst->GetPitch(plane), src->GetReadPtr(plane), src->GetPitch(plane),
                src->GetRowSize(plane), src->GetHeight(plane));
  }

  SourceFrame sourceFrame(int k, IScriptEnvironment* env) const {
    auto frame = std::make_shared<PVideoFrame>(external_->GetFrame(k < ext_frames_ ? k : ext_frames_ - 1, env));
    SourceFrame f;
    f.y = readPlane(*frame, PLANAR_Y);
    if (fmt_.has_chroma) {
      f.u = readPlane(*frame, PLANAR_U);
      f.v = readPlane(*frame, PLANAR_V);
    }
    f.holder = frame;
    return f;
  }

  PClip external_;
  int ext_frames_ = 0;
  bool has_v8_ = false;
  Format fmt_;
  std::unique_ptr<Engine> engine_;
};
void checkClip(const VideoInfo& vi, IScriptEnvironment* env) {
  if ((vi.width & 1) || (vi.height & 1)) env->ThrowError("AutoAdjust: Width and height must be MOD2");
  if (!vi.IsPlanar() || !vi.IsYUV()) env->ThrowError("AutoAdjust: Colorspace must be YUV Planar");
  if (vi.BitsPerComponent() > 16) env->ThrowError("AutoAdjust: float formats are not supported");
}

AVSValue __cdecl Create(AVSValue args, void*, IScriptEnvironment* env) {
  PClip child = args[0].AsClip();
  PClip external = args[1].Defined() ? args[1].AsClip() : child;
  Params p;
  p.auto_gain = args[2].AsBool(false);
  p.dark_limit = args[3].AsFloatf(1.5f);
  p.bright_limit = args[4].AsFloatf(1.5f);
  p.gamma_limit = args[5].AsFloatf(1.25f);
  p.dark_exclude = args[6].AsFloatf(0.5f);
  p.bright_exclude = args[7].AsFloatf(0.5f);
  p.gain_mode = args[8].AsInt(0);
  p.chroma_process = args[9].AsInt(100);
  p.avg_safety = args[10].AsFloatf(0.25f);
  p.input_tv = args[11].AsBool(true);
  p.output_tv = args[12].AsBool(true);
  p.auto_balance = args[13].AsBool(false);
  p.chroma_limit = args[14].AsFloatf(1.05f);
  p.balance_str = args[15].AsFloatf(0.75f);
  p.scd_threshold = args[16].AsInt(12);
  p.temporal_radius = args[17].AsInt(20);
  p.change_status = args[18].AsString("");
  p.use_interp = args[19].AsBool(true);
  p.use_dither = args[20].AsBool(true);
  p.high_quality = args[21].AsBool(false);
  const int threads_count = args[22].AsInt(0);  // accepted for compatibility, the host does the threading
  const int asm_opt = args[23].AsInt(0);
  p.debug_view = args[24].AsBool(false);

  const std::string err = validateParams(p);
  if (!err.empty()) env->ThrowError("%s", err.c_str());
  if (static_cast<unsigned>(threads_count) > 16) env->ThrowError("AutoAdjust: threads_count must be between 0 and 16");
  if (static_cast<unsigned>(asm_opt) > 3) env->ThrowError("AutoAdjust: asm_opt must be between 0 and 3");

  const VideoInfo& vi = child->GetVideoInfo();
  const VideoInfo& evi = external->GetVideoInfo();
  checkClip(vi, env);
  checkClip(evi, env);
  if (!vi.IsSameColorspace(evi)) env->ThrowError("AutoAdjust: Colorspace of clip and external_clip must be the same");
  if (!simdAvailable(asm_opt)) env->ThrowError("AutoAdjust: Invalid SIMD");

  std::vector<ChangeRange> overrides;
  if (!parseChangeStatus(p.change_status, evi.num_frames, overrides))
    env->ThrowError("AutoAdjust: change_status invalid syntax!");
  if (p.debug_view) env->ThrowError("AutoAdjust: debug_view is not implemented yet");

  return new AutoAdjustAvs(child, external, p, std::move(overrides), asm_opt, env);
}

}  // namespace

#ifdef _WIN32
#define AUTOADJUST_AVS_EXPORT extern "C" __declspec(dllexport)
#else
#define AUTOADJUST_AVS_EXPORT extern "C" __attribute__((visibility("default")))
#endif

const AVS_Linkage* AVS_linkage = nullptr;

AUTOADJUST_AVS_EXPORT const char* __stdcall AvisynthPluginInit3(IScriptEnvironment* env,
                                                                const AVS_Linkage* const vectors) {
  AVS_linkage = vectors;
  env->AddFunction("AutoAdjust",
                   "c[external_clip]c[auto_gain]b[dark_limit]f[bright_limit]f[gamma_limit]f[dark_exclude]f"
                   "[bright_exclude]f[gain_mode]i[chroma_process]i[avg_safety]f[input_tv]b[output_tv]b"
                   "[auto_balance]b[chroma_limit]f[balance_str]f[scd_threshold]i[temporal_radius]i"
                   "[change_status]s[use_interp]b[use_dither]b[high_quality]b"
                   "[threads_count]i[asm_opt]i[debug_view]b",
                   Create, nullptr);
  return "AutoAdjust " AUTOADJUST_VERSION;
}
