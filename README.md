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

Parameters and defaults are those of the original, see [docs/SPEC.md §2.2](docs/SPEC.md). In
VapourSynth `high_bitdepth`, `threads_count` and `asm_opt` do not exist: native 8–16 bit formats are
used directly and the host does the threading.

## Differences from the original

- With `use_dither=false` the output is **bit-exact** with the original's C path (`asm_opt=1`) for all
  tested parameter sets, 8-bit planar formats, `external_clip`, scene changes / `change_status` and
  stacked 16 bit. The original's default `asm_opt=0` uses AVX2 with an approximate `pow`, which differs
  by ±1 in a few hundred pixels per 1080p frame; the port always computes the exact reference.
- Dither: the original seeds its dither offsets from the clock, so its output changes between runs. The
  port uses fixed offsets and its own 64×64 blue-noise table ([tools/gen_bluenoise.py](tools/gen_bluenoise.py))
  — output is reproducible.
- Native high bit depth (10–16 bit) is supported in both hosts; the original silently mangled such
  clips. Float formats are rejected. Alpha planes are passed through.
- `threads_count` and `asm_opt` are accepted in AviSynth+ for compatibility and ignored.
- Black or full-scale frame averages give gamma 1 instead of NaN (black output).
- Frame numbers outside the clip are clamped.
- `debug_view` is not implemented yet.

## Building

Windows, Visual Studio 2022+: `build.bat` → `build\AutoAdjust.dll`.

Floating point must stay strict (`/fp:precise`, no FMA contraction; `-ffp-contract=off` with GCC/Clang),
otherwise the output is no longer bit-exact with the reference.
