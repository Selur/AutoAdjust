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

AviSynth+ (same name and arguments as the original):

```
AutoAdjust(auto_gain=true, auto_balance=true)
```

VapourSynth:

```python
clip = core.autoadjust.AutoAdjust(clip, auto_gain=True, auto_balance=True)
```

Parameters and defaults are those of the original, see [docs/SPEC.md §2.2](docs/SPEC.md), except
`high_bitdepth`: there is no stacked 16-bit mode, high bit depth clips are passed natively
(`ConvertBits(16)` in AviSynth+, a 16-bit format in VapourSynth). In VapourSynth `threads_count` and
`asm_opt` do not exist either; the host does the threading.

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
