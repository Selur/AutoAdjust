// SPDX-License-Identifier: GPL-2.0-or-later
// VapourSynth (API 4) interface of AutoAdjust: native 8-16 bit integer YUV and Gray clips.
#include <memory>
#include <string>
#include <vector>

#include "VSHelper4.h"
#include "VapourSynth4.h"
#include "core/autoadjust.hpp"
#include "core/kernels.hpp"
#include "core/stats_cache.hpp"
#include "version.hpp"

using namespace autoadjust;

namespace {

struct FilterData {
  VSNode* node = nullptr;
  VSNode* external = nullptr;
  int ext_frames = 0;
  Format fmt;
  std::unique_ptr<Engine> engine;
  StatsCache cache;
};

SampleKind kindOf(const Format& f) { return f.bits > 8 ? SampleKind::U16 : SampleKind::U8; }

ConstPlane readPlane(const VSFrame* f, int plane, const Format& fmt, const VSAPI* vsapi) {
  return {vsapi->getReadPtr(f, plane), vsapi->getStride(f, plane), vsapi->getFrameWidth(f, plane),
          vsapi->getFrameHeight(f, plane), kindOf(fmt)};
}

int externalIndex(const FilterData* d, int k) { return k < d->ext_frames ? k : d->ext_frames - 1; }

FrameStats statsOf(FilterData* d, int k, VSFrameContext* ctx, const VSAPI* vsapi) {
  FrameStats s;
  if (d->cache.get(k, s)) return s;
  const VSFrame* cur = vsapi->getFrameFilter(externalIndex(d, k), d->external, ctx);
  const VSFrame* prev = nullptr;
  ConstPlane prev_y;
  if (d->engine->needsPrevious(k)) {
    prev = vsapi->getFrameFilter(externalIndex(d, k - 1), d->external, ctx);
    prev_y = readPlane(prev, 0, d->fmt, vsapi);
  }
  const ConstPlane y = readPlane(cur, 0, d->fmt, vsapi);
  ConstPlane u, v;
  if (d->fmt.has_chroma) {
    u = readPlane(cur, 1, d->fmt, vsapi);
    v = readPlane(cur, 2, d->fmt, vsapi);
  }
  s = d->engine->frameStats(k, y, prev ? &prev_y : nullptr, d->fmt.has_chroma ? &u : nullptr,
                            d->fmt.has_chroma ? &v : nullptr);
  vsapi->freeFrame(prev);
  vsapi->freeFrame(cur);
  d->cache.put(k, s);
  return s;
}

const VSFrame* VS_CC getFrame(int n, int reason, void* instance, void**, VSFrameContext* ctx, VSCore* core,
                              const VSAPI* vsapi) {
  auto* d = static_cast<FilterData*>(instance);
  if (reason == arInitial) {
    vsapi->requestFrameFilter(n, d->node, ctx);
    if (!d->engine->isPassthrough()) {
      int first, last;
      d->engine->window(n, first, last);
      for (int k = first > 0 ? first - 1 : 0; k <= last; k++)
        vsapi->requestFrameFilter(externalIndex(d, k), d->external, ctx);
    }
    return nullptr;
  }
  if (reason != arAllFramesReady) return nullptr;

  const VSFrame* src = vsapi->getFrameFilter(n, d->node, ctx);
  if (d->engine->isPassthrough()) return src;
  int first, last;
  d->engine->window(n, first, last);
  std::vector<FrameStats> stats;
  stats.reserve(last - first + 1);
  for (int k = first; k <= last; k++) stats.push_back(statsOf(d, k, ctx, vsapi));
  const Correction c = d->engine->combine(n, first, stats);

  VSFrame* dst = vsapi->copyFrame(src, core);
  vsapi->freeFrame(src);
  Plane planes[3];
  const int count = d->fmt.has_chroma ? 3 : 1;
  for (int i = 0; i < count; i++)
    planes[i] = {vsapi->getWritePtr(dst, i), vsapi->getStride(dst, i), vsapi->getFrameWidth(dst, i),
                 vsapi->getFrameHeight(dst, i), kindOf(d->fmt)};
  d->engine->apply(c, planes, count);
  return dst;
}

void VS_CC freeFilter(void* instance, VSCore*, const VSAPI* vsapi) {
  auto* d = static_cast<FilterData*>(instance);
  vsapi->freeNode(d->node);
  vsapi->freeNode(d->external);
  delete d;
}

bool checkFormat(const VSVideoInfo* vi, std::string& err) {
  if (!vsh::isConstantVideoFormat(vi)) {
    err = "AutoAdjust: only constant format input is supported";
    return false;
  }
  const VSVideoFormat& f = vi->format;
  if ((f.colorFamily != cfYUV && f.colorFamily != cfGray) || f.sampleType != stInteger || f.bitsPerSample > 16) {
    err = "AutoAdjust: only 8-16 bit integer YUV or Gray clips are supported";
    return false;
  }
  if ((vi->width & 1) || (vi->height & 1)) {
    err = "AutoAdjust: Width and height must be MOD2";
    return false;
  }
  return true;
}

void VS_CC create(const VSMap* in, VSMap* out, void*, VSCore* core, const VSAPI* vsapi) {
  auto d = std::make_unique<FilterData>();
  int e;
  auto getBool = [&](const char* key, bool def) {
    const int64_t v = vsapi->mapGetInt(in, key, 0, &e);
    return e ? def : v != 0;
  };
  auto getInt = [&](const char* key, int def) {
    const int64_t v = vsapi->mapGetInt(in, key, 0, &e);
    return e ? def : static_cast<int>(v);
  };
  auto getFloat = [&](const char* key, float def) {
    const double v = vsapi->mapGetFloat(in, key, 0, &e);
    return e ? def : static_cast<float>(v);
  };
  auto fail = [&](const std::string& msg) {
    vsapi->mapSetError(out, msg.c_str());
    vsapi->freeNode(d->node);
    vsapi->freeNode(d->external);
  };

  d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
  d->external = vsapi->mapGetNode(in, "external_clip", 0, &e);
  if (e) d->external = vsapi->addNodeRef(d->node);

  Params p;
  p.auto_gain = getBool("auto_gain", false);
  p.dark_limit = getFloat("dark_limit", 1.5f);
  p.bright_limit = getFloat("bright_limit", 1.5f);
  p.gamma_limit = getFloat("gamma_limit", 1.25f);
  p.dark_exclude = getFloat("dark_exclude", 0.5f);
  p.bright_exclude = getFloat("bright_exclude", 0.5f);
  p.gain_mode = getInt("gain_mode", 0);
  p.chroma_process = getInt("chroma_process", 100);
  p.avg_safety = getFloat("avg_safety", 0.25f);
  p.input_tv = getBool("input_tv", true);
  p.output_tv = getBool("output_tv", true);
  p.auto_balance = getBool("auto_balance", false);
  p.chroma_limit = getFloat("chroma_limit", 1.05f);
  p.balance_str = getFloat("balance_str", 0.75f);
  p.scd_threshold = getInt("scd_threshold", 12);
  p.temporal_radius = getInt("temporal_radius", 20);
  const char* cs = vsapi->mapGetData(in, "change_status", 0, &e);
  if (!e) p.change_status = cs;
  p.use_interp = getBool("use_interp", true);
  p.use_dither = getBool("use_dither", true);
  p.high_quality = getBool("high_quality", false);
  p.debug_view = getBool("debug_view", false);
  const int asm_opt = getInt("asm_opt", 0);

  std::string err = validateParams(p);
  if (!err.empty()) return fail(err);
  if (static_cast<unsigned>(asm_opt) > 3) return fail("AutoAdjust: asm_opt must be between 0 and 3");
  const VSVideoInfo* vi = vsapi->getVideoInfo(d->node);
  const VSVideoInfo* evi = vsapi->getVideoInfo(d->external);
  if (!checkFormat(vi, err) || !checkFormat(evi, err)) return fail(err);
  if (!vsh::isSameVideoFormat(&vi->format, &evi->format))
    return fail("AutoAdjust: Colorspace of clip and external_clip must be the same");
  if (!simdAvailable(asm_opt)) return fail("AutoAdjust: Invalid SIMD");
  std::vector<ChangeRange> overrides;
  if (!parseChangeStatus(p.change_status, evi->numFrames, overrides))
    return fail("AutoAdjust: change_status invalid syntax!");
  if (p.debug_view) return fail("AutoAdjust: debug_view is not implemented yet");

  d->ext_frames = evi->numFrames;
  d->fmt.bits = vi->format.bitsPerSample;
  d->fmt.has_chroma = vi->format.colorFamily == cfYUV;
  d->fmt.ssw = d->fmt.has_chroma ? vi->format.subSamplingW : 0;
  d->fmt.ssh = d->fmt.has_chroma ? vi->format.subSamplingH : 0;
  d->engine = std::make_unique<Engine>(p, d->fmt, vi->width, vi->height, vi->numFrames, std::move(overrides), asm_opt);

  VSFilterDependency deps[] = {{d->node, rpStrictSpatial}, {d->external, rpGeneral}};
  vsapi->createVideoFilter(out, "AutoAdjust", vi, getFrame, freeFilter, fmParallel, deps, 2, d.release(), core);
}

}  // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi) {
  vspapi->configPlugin("io.github.selur.autoadjust", "autoadjust", "AutoAdjust " AUTOADJUST_VERSION,
                       VS_MAKE_VERSION(AUTOADJUST_VERSION_MAJOR, AUTOADJUST_VERSION_MINOR), VAPOURSYNTH_API_VERSION,
                       0, plugin);
  vspapi->registerFunction("AutoAdjust",
                           "clip:vnode;external_clip:vnode:opt;auto_gain:int:opt;dark_limit:float:opt;"
                           "bright_limit:float:opt;gamma_limit:float:opt;dark_exclude:float:opt;"
                           "bright_exclude:float:opt;gain_mode:int:opt;chroma_process:int:opt;avg_safety:float:opt;"
                           "input_tv:int:opt;output_tv:int:opt;auto_balance:int:opt;chroma_limit:float:opt;"
                           "balance_str:float:opt;scd_threshold:int:opt;temporal_radius:int:opt;"
                           "change_status:data:opt;use_interp:int:opt;use_dither:int:opt;high_quality:int:opt;"
                           "asm_opt:int:opt;debug_view:int:opt;",
                           "clip:vnode;", create, nullptr, plugin);
}
