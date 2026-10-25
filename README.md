# AutoAdjust (clean-room port)

Automatic, temporally stabilised levels / gamma / saturation / white-balance correction for
**AviSynth+** and **VapourSynth** — a new implementation of LaTo's closed-source AutoAdjust 2.60.

One DLL serves both hosts.

## Clean-room process

The original binary was analysed separately and its behaviour described in
[docs/SPEC.md](docs/SPEC.md) (prose, formulas, new pseudo-code — no decompiled code). This code was
written from that specification only. The original DLL and any analysis output are not part of this
repository.

## Usage

AviSynth+ (same name and arguments as the original, without `high_bitdepth`):

```
AutoAdjust(clip clip, clip "external_clip", bool "auto_gain", float "dark_limit", float "bright_limit",
           float "gamma_limit", float "dark_exclude", float "bright_exclude", int "gain_mode",
           int "chroma_process", float "avg_safety", bool "input_tv", bool "output_tv",
           bool "auto_balance", float "chroma_limit", float "balance_str", int "scd_threshold",
           int "temporal_radius", string "change_status", bool "use_interp", bool "use_dither",
           bool "high_quality", int "threads_count", int "asm_opt", bool "debug_view")
```

VapourSynth:

```python
core.autoadjust.AutoAdjust(clip clip[, clip external_clip, int auto_gain, float dark_limit,
    float bright_limit, float gamma_limit, float dark_exclude, float bright_exclude, int gain_mode,
    int chroma_process, float avg_safety, int input_tv, int output_tv, int auto_balance,
    float chroma_limit, float balance_str, int scd_threshold, int temporal_radius,
    data change_status, int use_interp, int use_dither, int high_quality, int debug_view])
```

Examples:

```
AutoAdjust(auto_gain=true, auto_balance=true)                      # AviSynth+
```

```python
clip = core.autoadjust.AutoAdjust(clip, auto_gain=True, auto_balance=True)
```

With the defaults (`auto_gain` and `auto_balance` off) the filter returns its input unchanged; switch
on at least one of the two.

### Formats

YUV (any subsampling, chroma dimensions included) and Gray, 8–16 bit integer, width and height
divisible by 2. Float formats and RGB are rejected. High bit depth is processed natively
(`ConvertBits(16)` in AviSynth+, e.g. `vs.YUV420P16` in VapourSynth); alpha planes are passed through.

### Parameters

**clip** — the clip to correct.

**external_clip** (clip, default: *clip*) — the clip the statistics are measured on. The correction
computed from frame *n* of `external_clip` is applied to frame *n* of `clip`. Must have the same
format as `clip`; its size may differ (a downscaled copy measures faster, a cleaned or cropped copy
keeps borders, logos or noise out of the measurement).

#### Auto gain (luma levels, gamma and saturation)

**auto_gain** (bool, default false) — enables the luma correction: the measured black and white
points are stretched towards the target range and the gamma is adjusted; the saturation follows the
contrast gain (`chroma_process`).

**dark_limit** (float, 1.0 … 100.0, default 1.5) — how far the black point may be moved. The target
black point lies at `average − (average − measured black) × dark_limit`; 1.0 = no change.

**bright_limit** (float, 1.0 … 100.0, default 1.5) — the same for the white point.

**gamma_limit** (float, 1.0 … 100.0, default 1.25) — strength of the gamma correction. The gamma
moves the frame average towards the middle of the target range; 1.0 = no gamma correction.

**dark_exclude** (float, 0.0 … 1.0, default 0.5) — percentage of the darkest pixels that is ignored
when the black point is measured (0.5 = the darkest 0.5 %). Keeps a few black pixels (borders, noise)
from deciding the black point.

**bright_exclude** (float, 0.0 … 1.0, default 0.5) — the same for the brightest pixels and the white
point.

**gain_mode** (int, 0 or 1, default 0) — luma curve:

- 0 = contrast expansion: the ranges below and above the average are stretched separately (the
  average itself stays), then the gamma is applied;
- 1 = levels adjustment: one linear stretch from the measured black/white point to the target range,
  then the gamma; the gamma target uses the measured range.

**chroma_process** (int, 0 … 200, default 100) — how much of the luma contrast gain is applied to
the saturation, in percent. 0 = saturation unchanged, 100 = follows the contrast, 200 = twice that.
Strongly saturated colours are raised less than weak ones. Only active with `auto_gain`.

**avg_safety** (float, 0.0 … 1.0, default 0.25) — blend between the averaged black/white point of the
temporal window and a conservative one (the black point of the darkest quarter, the white point of the
brightest quarter of the window, by weight). 0 = most stable, 1 = most conservative.

**input_tv** (bool, default true) — limits the measured black/white points and the average to the TV
range (16–235 at 8 bit, scaled for higher bit depths) before the correction is built. Switch off for
full-range sources.

**output_tv** (bool, default true) — keeps the target black/white points inside the TV range.
Samples outside the measured range can still leave it; the output is not clipped.

#### Auto balance (white balance)

**auto_balance** (bool, default false) — shifts U and V so that the robust average colour of the
temporal window becomes neutral grey.

**chroma_limit** (float, 1.0 … 2.0, default 1.05) — maximum shift: `(chroma_limit − 1)` × half the
chroma range. 1.0 = no balance, 2.0 = no limit.

**balance_str** (float, 0.0 … 1.0, default 0.75) — strength of the shift. 0 = none, 1 = full.

#### Temporal stabilisation and scene changes

**temporal_radius** (int, ≥ 0, default 20) — number of frames before and after the current one
whose statistics are combined into the correction, weighted by distance (1.0 for the current frame,
0.5 at the edge of the window). The window never reaches across a scene change. Larger radii keep the
correction steady within a scene, 0 corrects every frame on its own (may pump).

**scd_threshold** (int, 0 … 100, default 12) — scene change threshold in percent. A frame starts a
new scene when its luma differs from the previous frame by more than this percentage (mean absolute
difference) or its luma histogram by more than 2.5 × this percentage. Useful range about 8–16;
0 = every frame is a scene start, 100 = only frame 0 starts a scene.

**change_status** (string, default "") — manual scene change overrides, `;`-separated:

- `N` or `A-B`: frame *N* (or every frame from *A* to *B*) is a scene start;
- `!N` or `!A-B`: no scene change there (e.g. a flash detected as a cut);
- later entries win over earlier ones; a trailing `;` is allowed; `""` or `"empty"` = no overrides.

Example: `change_status="0;250;!812;1000-1005"`. Frame numbers refer to `external_clip`; a syntax
error or a frame outside the clip raises `AutoAdjust: change_status invalid syntax!`.

#### Precision

**use_interp** (bool, default true) — smooths the correction (the change per pixel, not the picture)
with a small box filter that only averages differences below half a code value, so the fractional
correction does not produce banding while edges in the correction stay sharp. The filter radius grows
with the frame size (8 … 32 pixels).

**high_quality** (bool, default false) — the smoothing uses every pixel instead of every second
one; slower, slightly smoother. Only with `use_interp`.

**use_dither** (bool, default true) — ordered dither (16×16 Bayer pattern plus 64×64 blue noise)
when the corrected values are rounded back to integers; keeps stretched gradients free of banding.
false = plain rounding. The pattern is fixed, so the output is reproducible.

#### Compatibility

**threads_count** (int, 0 … 16, default 0), **asm_opt** (int, 0 … 3, default 0) — AviSynth+ only,
accepted for scripts written for the original and ignored: the host does the threading, and there
is one code path (the exact one of the original's C path).

**debug_view** (bool, default false) — the original's analysis canvas; not implemented yet, `true`
raises an error.

### How it works

Per frame, the luma histogram gives a black and a white point (with the `*_exclude` percentages and
sub-level precision) and a robust average (mean and median of the clipped histogram); the chroma
histograms give the average U and V. These statistics are cached per frame and combined over the
temporal window with weighted trimmed means. From the combined values the filter derives the target
levels, a gamma, a saturation gain and the U/V shifts, applies them per pixel, smooths the change
(`use_interp`) and rounds or dithers the result (`use_dither`). The full description is
[docs/SPEC.md](docs/SPEC.md).

## Differences from the original

- With `use_dither=false` the output is **bit-exact** with the original's C path (`asm_opt=1`) for all
  tested parameter sets, 8-bit planar formats, `external_clip` and scene changes / `change_status`;
  native 16 bit matches the original's stacked 16-bit mode bit-exactly. The original's default `asm_opt=0` uses AVX2 with an approximate `pow`, which differs
  by ±1 in a few hundred pixels per 1080p frame; the port always computes the exact reference.
- Dither: the original seeds its dither offsets from the clock, so its output changes between runs. The
  port uses fixed offsets and its own 64×64 blue-noise table ([tools/gen_bluenoise.py](tools/gen_bluenoise.py))
  — output is reproducible.
- High bit depth is native only (10–16 bit, both hosts); the original needed stacked 16 bit
  (`high_bitdepth=true`) and silently mangled native clips. `high_bitdepth` no longer exists, so old
  scripts using it fail with an argument error. Float formats are rejected. Alpha planes are passed
  through.
- `threads_count` and `asm_opt` are accepted in AviSynth+ for compatibility and ignored.
- Black or full-scale frame averages give gamma 1 instead of NaN (black output).
- Frame numbers outside the clip are clamped.
- `debug_view` is not implemented yet.

## Building

- Meson: `meson setup build && ninja -C build`; on Windows with MSVC
  `meson setup build --vsenv -Db_vscrt=mt`. The result is `AutoAdjust.dll` / `libAutoAdjust.so` /
  `libAutoAdjust.dylib`, loadable by both AviSynth+ and VapourSynth.
- Wheel for VapourSynth: `pip install .` (installs into `site-packages/vapoursynth/plugins`).
- Windows without Meson: `build.bat` (Visual Studio 2022+) -> `build\AutoAdjust.dll`.
- CI (`.github/workflows/build.yml`) builds and tests Windows x64, Linux x86_64 and macOS arm64, and
  attaches wheels plus zipped plugin binaries to releases for `v*` tags.

Floating point must stay strict (`/fp:precise`; `-ffp-contract=off` with GCC/Clang, set by
`meson.build`), otherwise the output is no longer bit-exact with the reference.

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). The vendored SDK headers in `include/` keep their own licences.
