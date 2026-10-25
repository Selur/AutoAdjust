# AutoAdjust 2.60 — behavioural specification

Clean-room description of the AviSynth plugin **AutoAdjust** by "LaTo INV." (x64 build of 2016-01-05,
version string "AutoAdjust v2.60"). It is written so that a new AviSynth+ / VapourSynth filter can be
implemented without access to the original binary. Formulas and pseudo-code below are new; names are
descriptive, not taken from the binary.

Status markers used throughout:

- **[V]** verified by black-box experiments against the original DLL (bit-exact where stated),
- **[I]** inferred from static analysis only, not confirmed by an experiment,
- **[?]** open question.

The "reference path" is the plain C path, selected with `asm_opt=1`. The default `asm_opt=0` selects
the best SIMD path of the CPU (AVX2 on current machines), which differs from the reference by ±1 in a
few hundred pixels per 1080p frame (see §12).

---

## 1. Overview

AutoAdjust is an automatic, temporally stabilised levels/gamma/saturation/white-balance corrector.

Per output frame `n`:

1. **Statistics** (§5) are computed for every frame of `external_clip` that is needed: luma minimum and
   maximum with a small exclusion percentage and sub-bin precision, a robust luma average, the robust
   averages of U and V, and a scene-change status.
2. **Temporal combination** (§6): the statistics of the frames in a window of ±`temporal_radius`
   around `n`, limited by scene changes, are combined with triangular weights through weighted
   trimmed means and weighted percentiles.
3. **Frame parameters** (§7): from the combined values the target black/white points `low`/`high`,
   a gamma exponent, a chroma gain and U/V balance offsets are derived.
4. **Pixel transform** (§8): every luma pixel goes through a piecewise linear stretch plus gamma;
   every chroma pixel through a saturation-weighted gain plus offset. The transform is kept as a
   floating point *delta* (new value − old value) per pixel.
5. **Delta smoothing** (`use_interp`, §9): the delta plane is smoothed with a clamped box filter
   (vertical pass, then horizontal pass) to hide banding.
6. **Output** (§10): `old + delta` is rounded (`+0.5`) or ordered-dithered (`use_dither`) and
   truncated to the output sample format.

With the defaults (`auto_gain=false`, `auto_balance=false`) the filter is an exact identity **[V]**.

---

## 2. Interface

### 2.1 Signature

AviSynth name `AutoAdjust`, argument string (one string, shown wrapped):

```
c[external_clip]c[auto_gain]b[dark_limit]f[bright_limit]f[gamma_limit]f[dark_exclude]f
[bright_exclude]f[gain_mode]i[chroma_process]i[avg_safety]f[input_tv]b[output_tv]b
[auto_balance]b[chroma_limit]f[balance_str]f[scd_threshold]i[temporal_radius]i
[change_status]s[use_interp]b[use_dither]b[high_quality]b[high_bitdepth]b
[threads_count]i[asm_opt]i[debug_view]b
```

Float arguments are handled as 32-bit floats.

### 2.2 Parameters

| Parameter | Type | Default | Valid range | Meaning |
|---|---|---|---|---|
| `external_clip` | clip | the input clip | same pixel type | Clip the statistics are measured on (§4.3). |
| `auto_gain` | bool | false | | Enable the luma transform (levels + gamma) and the gain-coupled chroma saturation. |
| `dark_limit` | float | 1.5 | 1.0 … 100.0 | Gain applied to the distance avg→min to get `low` (§7). |
| `bright_limit` | float | 1.5 | 1.0 … 100.0 | Gain applied to the distance avg→max to get `high`. |
| `gamma_limit` | float | 1.25 | 1.0 … 100.0 | Strength of the gamma correction; 1.0 = no gamma. |
| `dark_exclude` | float | 0.5 | 0.0 … 1.0 | Percentage (of all luma pixels) ignored at the dark end when finding the frame minimum. |
| `bright_exclude` | float | 0.5 | 0.0 … 1.0 | Same for the bright end / maximum. |
| `gain_mode` | int | 0 | 0 or 1 | Luma curve type (§8.1/§8.2) and the gamma target (§7). |
| `chroma_process` | int | 100 | 0 … 200 | Percentage of the luma contrast gain that is applied to saturation. |
| `avg_safety` | float | 0.25 | 0.0 … 1.0 | Blend between a central and a conservative temporal estimate of min/max (§6.4). |
| `input_tv` | bool | true | | Clamp the measured min/max/avg to TV range before the curve is built. |
| `output_tv` | bool | true | | Clamp the targets `low`/`high` to TV range. Output pixels are **not** clamped to TV range. |
| `auto_balance` | bool | false | | Enable the U/V offset (white balance) correction. |
| `chroma_limit` | float | 1.05 | 1.0 … 2.0 | Maximum balance offset = (chroma_limit−1)·half-range. |
| `balance_str` | float | 0.75 | 0.0 … 1.0 | Strength of the balance offset. |
| `scd_threshold` | int | 12 | 0 … 100 | Scene-change threshold in percent (§5.5). |
| `temporal_radius` | int | 20 | ≥ 0 (no upper limit) | Half width of the temporal window in frames. |
| `change_status` | string | "" | §5.6 | Manual scene-change overrides. |
| `use_interp` | bool | true | | Smooth the delta plane (§9). |
| `use_dither` | bool | true | | Ordered dither instead of rounding at the output (§10). |
| `high_quality` | bool | false | | Delta smoothing uses every pixel (step 1) instead of every second one (step 2). |
| `high_bitdepth` | bool | false | | Input, external clip and output are 16-bit "stacked" (MSB/LSB) clips (§4.2). |
| `threads_count` | int | 0 | 0 … 16 | Internal worker threads, 0 = automatic (§13). |
| `asm_opt` | int | 0 | 0 … 3 | 0 = auto, 1 = C, 2 = SSE2, 3 = AVX2 (§12). |
| `debug_view` | bool | false | | Replace the output by an analysis canvas (§14). |

Which parameters matter:

- `dark_limit`, `bright_limit`, `gamma_limit`, `gain_mode`, `avg_safety`, `input_tv`, `output_tv`
  influence the output only through the luma transform and the chroma gain, i.e. only with
  `auto_gain=true`. The balance (§7) uses only the U/V averages.
- `chroma_process` only matters with `auto_gain=true` (it scales the gain-coupled saturation). With
  `auto_gain=false` the chroma planes still run through the chroma pipeline when
  `chroma_process ≥ 1`, but the gain is exactly 1 and the result is an identity **[V]**.
- `chroma_limit`, `balance_str` only matter with `auto_balance=true`.
- `dark_exclude`, `bright_exclude`, `scd_threshold`, `temporal_radius`, `change_status` always shape
  the statistics (and with them every enabled correction).
- `high_quality` only matters with `use_interp=true`.

### 2.3 Validation (order and messages) [V]

Checks run in this order; the first failure throws (AviSynth `ThrowError`) with exactly this text:

1. `dark_limit` outside [1.0, 100.0] → `AutoAdjust: dark_limit must be between 1.0 and 100.0`
2. `bright_limit` → `AutoAdjust: bright_limit must be between 1.0 and 100.0`
3. `gamma_limit` → `AutoAdjust: gamma_limit must be between 1.0 and 100.0`
4. `dark_exclude` outside [0.0, 1.0] → `AutoAdjust: dark_exclude must be between 0.0 and 1.0`
5. `bright_exclude` → `AutoAdjust: bright_exclude must be between 0.0 and 1.0`
6. `gain_mode` not 0/1 (unsigned compare, negatives fail) → `AutoAdjust: gain_mode must be 0 or 1`
7. `chroma_process` outside 0…200 (unsigned) → `AutoAdjust: chroma_process must be between 0 and 200`
8. `avg_safety` outside [0.0, 1.0] → `AutoAdjust: avg_safety must be between 0.0 and 1.0`
9. `chroma_limit` outside [1.0, 2.0] → `AutoAdjust: chroma_limit must be between 1.0 and 2.0`
10. `balance_str` outside [0.0, 1.0] → `AutoAdjust: balance_str must be between 0.0 and 1.0`
11. `scd_threshold` outside 0…100 (unsigned) → `AutoAdjust: scd_threshold must be between 0 and 100`
12. `temporal_radius` < 0 → `AutoAdjust: temporal_radius must be >= 0`
13. `threads_count` outside 0…16 (unsigned) → `AutoAdjust: threads_count must be between 0 and 16`
14. `asm_opt` outside 0…3 (unsigned) → `AutoAdjust: asm_opt must be between 0 and 3`
15. input width odd, or input height (halved first when `high_bitdepth`) odd →
    `AutoAdjust: Width and height must be MOD2`
16. input not planar → `AutoAdjust: Colorspace must be YUV Planar` (RGB24/RGB32/YUY2 fail here)
17. the same MOD2 check on `external_clip` (its own width/height, height halved with `high_bitdepth`)
18. `external_clip` not planar → `AutoAdjust: Colorspace must be YUV Planar` **[I]**
19. pixel types of input and `external_clip` differ →
    `AutoAdjust: Colorspace of clip and external_clip must be the same`
20. `asm_opt=2` without SSE2, or `asm_opt=3` without AVX2 → `AutoAdjust: Invalid SIMD` **[I]**
21. `change_status` syntax error → `AutoAdjust: change_status invalid syntax!`
22. a frame number in `change_status` outside [0, frame count of `external_clip`) →
    `AutoAdjust: change_status invalid syntax!` (same text).

Range checks use `<`/`>` on floats, so the bounds themselves are valid (`dark_limit=1.0` and
`100.0`, `chroma_limit=2.0` accepted) **[V]**. During `GetFrame`, a failing worker task raises
`AutoAdjust: Fatal error` **[I]**.

---

## 3. Numeric conventions

- All statistics and pixel arithmetic use IEEE single precision (`float`), round-to-nearest-even, no
  fused multiply-add in the reference path. Every intermediate value is rounded to float. The order of
  operations given in this document is significant for bit-exactness; expressions are evaluated left
  to right as written unless parenthesised.
- `log` (used for the gamma exponent) must be a **correctly rounded** single-precision natural
  logarithm. `(float)log((double)x)` is sufficient; an approximate `logf` that was off by one ulp
  changed ~6 output pixels per 1080p frame in the experiments **[V]**.
- `pow` (C path) is single precision; `(float)pow((double)t, (double)g)` matched the original
  bit-exactly in all tests **[V]**.
- `clamp(x, lo, hi)` is defined as: `x < lo → lo`, else `x > hi → hi`, else `x`.
- Bit depth `B` is 8, or 16 with `high_bitdepth`. Derived constants:

| Name | 8-bit | 16-bit (stacked) |
|---|---|---|
| `maxv` = 2^B − 1 | 255 | 65535 |
| `bins` = 2^B | 256 | 65536 |
| `half` = 2^(B−1) | 128 | 32768 |
| `tv_lo` = 16·2^(B−8) | 16 | 4096 |
| `tv_hi` = 235·2^(B−8) | 235 | 60160 |
| `thr` (delta smoothing tolerance, §9) | 0.5 | 128 |

---

## 4. Formats and geometry

### 4.1 Supported input [V]

| Input | Behaviour |
|---|---|
| YV12, YV16, YV24, YV411 (8-bit, no alpha) | Y, U and V processed; chroma subsampling shifts (h, v) = (1,1), (1,0), (0,0), (2,0). |
| Y8 | Luma only. |
| YUY2, RGB24, RGB32 | Error "Colorspace must be YUV Planar". |
| AviSynth+ planar formats other than the five above (YUV4xxP10/12/14/16, YUV4xxPS, YUVA…, Y10/12/14/16/32) | **Accepted but broken**: treated as "luma only", and the luma plane is read and written as if it held 8-bit samples (the first *width* bytes of each row). A reimplementation should reject these or support them properly (§16). |

Width and (stacked-halved) height must be even. The output has the same video info as the input
(except with `debug_view`, §14).

### 4.2 `high_bitdepth` (stacked 16-bit) [V]

With `high_bitdepth=true` every plane of the input, of `external_clip` and of the output is a stacked
16-bit plane inside an 8-bit clip of double height: the upper half of each plane holds the MSBs, the
lower half the LSBs, sample = `MSB·256 + LSB`. All processing uses `B = 16` and the halved height. On
output `MSB = (int)f >> 8`, `LSB = (int)f & 0xFF`. Verified bit-exact with a synthetic stacked clip
(`StackVertical(clipA, clipB)`, i.e. sample = A·256 + B).

### 4.3 `external_clip` [V]

All statistics (§5) — histograms, min/max/averages, scene-change detection, the `change_status` frame
range check — are taken from `external_clip`, frame `n` of it for output frame `n`. The pixel transform
is applied to the input clip. `external_clip` must have the same pixel type; its size may differ
(verified bit-exact with a 640×360 external clip for a 1920×1080 input). Frame counts are not compared;
the temporal window is limited by the frame count of the **input** clip, and frames of a shorter
`external_clip` are requested anyway (their content is up to the source filter).

### 4.4 Which planes are touched

- Luma is processed iff `auto_gain`.
- U and V are processed iff the format has chroma and (`auto_balance` or `chroma_process ≥ 1`).
- Unprocessed planes are passed through unchanged: the output is the input frame made writable and
  modified in place.

---

## 5. Per-frame statistics

For each frame `k` of `external_clip` the filter computes a record
`S(k) = {status, fmin, fmax, favg, uavg, vavg}`. It is a pure function of the frame content (and of
frame `k−1` for the scene-change test). It is cached (up to 1024 records, least recently used), and the
results do not depend on the request order **[V]**.

### 5.1 Luma histogram

Over **all** `W×H` luma samples (no border exclusion, no subsampling): `count[v]` = number of samples
with value `v` (accumulated as float, +1.0 per sample). Normalised histogram, float:
`inv = 1.0f / (float)(W·H)`, `h[v] = count[v] · inv` for `v = 0…maxv`.

### 5.2 Minimum and maximum with exclusion [V]

`td = dark_exclude / 100.0f`, `tb = bright_exclude / 100.0f` (float divisions; the parameters are
percentages of the pixel count).

Minimum: walk `v = 0, 1, …` accumulating `c += h[v]` (float). At the first `v` with `c > td`:

```
if v < maxv:
    if h[v] == 0: frac = 0.999999f
    else:
        frac = (((td − c) + h[v]) / h[v]) * (h[v+1] / h[v])
        if frac >= 0.999999f: frac = 0.999999f
else:
    frac = 0
fmin = (float)v + frac
```

Maximum: walk `v = maxv, maxv−1, …` accumulating `c += h[v]`. At the first `v` with `c > tb`:

```
if v < 1: frac = 0
else if h[v] == 0: frac = 0.999999f
else:
    frac = (((tb − c) + h[v]) / h[v]) * (h[v−1] / h[v])
    if frac >= 0.999999f: frac = 0.999999f
fmax = (float)v − frac
```

`0.999999f` is the float nearest to 0.999999 (0.99999898672…). The factor `h[v±1]/h[v]` is unusual
but is what the original computes. If no bin ever exceeds the threshold the value stays 0 (does not
happen for valid thresholds).

### 5.3 Robust luma average [V]

`cl(v) = v < fmin ? fmin : (fmax < v ? fmax : v)` (float, `v` converted to float).

- Weighted mean `mean = Σ cl(v)·h[v] / Σ h[v]` over `v = 0…maxv`. Both sums are float sums in this
  exact order (needed for bit-exactness; each product `cl(v)·h[v]` is rounded to float first):
  - `peel` = 2 in the C path (`asm_opt=1`), 0 in the SSE2/AVX2 paths (the histogram buffer alignment
    differs; `peel = 2` was confirmed experimentally for the C path, 0 gave mismatches **[V]**);
  - lane 0 starts with the sequential sum of the elements `0 … peel−1`, lanes 1–3 start at 0;
  - the elements `peel … end−1`, `end = n − ((n − peel) mod 4)` (`n = bins`), are added into four
    lanes, element `i` into lane `(i − peel) mod 4`, each lane sequentially;
  - the lanes are reduced as `(L0 + L2) + (L1 + L3)`;
  - the remaining elements `end … n−1` are added sequentially to that result.
  (If `n < peel + 4` everything is summed sequentially.)
- Median: walk `v = 0…maxv`, `c += h[v]`; the first `v` with `c ≥ 0.5` gives `med = cl(v)`.
- `favg = 0.5f · (mean + med)`.

### 5.4 Robust U/V averages [V]

Only computed when `auto_balance` is true and the format has chroma; otherwise `uavg = vavg = 0`
(unused). For each chroma plane separately: normalised histogram as in §5.1 over all chroma samples
(`inv = 1/(Wc·Hc)`), then

- `mean = Σ v·h[v] / Σ h[v]` (no clamping), in float, with the peel/lane scheme of §5.3 but in blocks
  of 8 elements (`end = n − ((n − peel) mod 8)`; sequential if `n < peel + 8`):
  - weighted sum: two 4-lane accumulators, the first receives elements `b+l`, the second `b+4+l`
    (`b` = block start, `l` = 0…3, lane 0 of the first one starts with the peel sum); they are added
    lane-wise, then reduced `(L0 + L2) + (L1 + L3)`, then the tail is added sequentially;
  - weight sum: a single 4-lane accumulator receiving first `h[b+l]`, then `h[b+4+l]` in each block,
    same peel, reduction and tail;
- median: the first `v` with cumulative sum `≥ 0.5` (no clamping);
- `uavg` (resp. `vavg`) `= (mean + median) · 0.5f`.

### 5.5 Scene-change status [V]

`status(k)` ∈ {1 = first frame of a scene, 2 = continuation}:

1. If `change_status` has an entry covering `k` (§5.6), that entry decides: plain entry → 1, entry with
   `!` → 2. When several entries cover `k`, the one written **last** wins.
2. Else if `k = 0` → 1.
3. Else compare frame `k` with frame `k−1` (luma only):
   - `a = SAD / ((float)(W·H) · maxv)` with `SAD = Σ |Y_k − Y_{k−1}|` over all luma samples
     (accumulated in float in the original; an exact integer sum differs only exactly at the
     threshold);
   - `b = 0.5f · Σ_v |h_k[v] − h_{k−1}[v]|` (float, the normalised histograms of §5.1);
   - `t1 = (float)scd_threshold / 100`, `t2 = (2.5f · (float)scd_threshold) / 100`;
   - status = 1 if `a > t1` or `b > t2`, else 2.

`scd_threshold=0` makes almost every frame a scene start; `scd_threshold=100` disables detection except
for frame 0 (a > 1 and b > 2.5 are impossible). With `scd_threshold=40` even the two hard cuts of the test clip were no longer detected **[V]**.

The original splits both sums over its worker threads and adds the partial sums under a lock in
completion order, so the float result can vary in the last bits with `threads_count > 1`; this matters
only exactly at the threshold.

### 5.6 `change_status` syntax [V]

- `""` or exactly `"empty"` → no entries.
- Otherwise the string is split at `;`. A trailing `;` is allowed (`"5;"`); any other empty token
  (`";5"`, `"5;;6"`) is an error.
- A token consists only of the characters `0-9`, `-`, `!` (spaces are errors). At most one `-` and at
  most one `!`; `!` may only be the first character; `-` may be neither the first nor the last
  character. These checks run before the `!` is removed, so `"!-5"` is accepted and parsed as `!0-5`.
- Token `N` → range [N, N]; token `A-B` → range [A, B], error if `B < A`. A leading `!` marks the range
  "no scene change" (status 2), otherwise "scene change" (status 1). Numbers are parsed as decimal
  `int` (stream extraction; an empty number parses as 0).
- Every frame of every range must lie in [0, frame count of `external_clip`), otherwise the same error.

Examples: `"0;250;1000-1005"` forces scene starts; `"!60"` suppresses a detected cut at 60; `"1-"`,
`"-1"`, `"3-1"`, `"1-2-3"`, `"1--2"`, `"!!1"`, `"1!"`, `"a"`, `" 1"`, `"-"`, `"!"` are errors.

Forcing status 1 on a range makes every frame of the range its own scene start (no temporal smoothing
across them, verified with `"100-105"`).

---

## 6. Temporal combination [V]

Let `r = temporal_radius`, `N` = frame count of the input clip, `S(k)` the records of §5.

### 6.1 Window

Five weighted lists are built — for `favg`, `fmin`, `fmax`, `uavg`, `vavg` — each holding
`(value, weight)` pairs in insertion order. The weight for distance `i` (1 ≤ i ≤ r) is
`w(i) = (float)(r − i) / (float)(2r) + 0.5f` (`w(1) = 0.975` for r = 20, `w(r) = 0.5`).

```
push S(n) with weight 1.0
if S(n).status == 2 and r > 0:            # continuation: look back
    for i = 1 … r:
        k = n − i; if k < 0: continue
        push S(k) with w(i)
        if S(k).status == 1: break         # the scene start itself is included
for i = 1 … r:                             # always look ahead (also when S(n).status == 1)
    k = n + i; if k ≥ N: continue
    if S(k).status == 1: break             # the next scene start is excluded
    push S(k) with w(i)
```

### 6.2 Weighted trimmed mean `TM(list, q)`

1. Sort a copy by value, ascending, stable.
2. `Wsum` = sequential float sum of the weights; `t = (0.5f · q) · Wsum`.
3. If `t > 0`, trim from the bottom: `c = 0`; while the list has ≥ 2 entries: `c += weight(first)`;
   if `t ≤ c`: `weight(first) −= (c − t)` and stop; otherwise remove the first entry and continue.
4. If `t > 0`, trim from the top the same way (fresh `c = 0`, using the last entry).
5. Result: `(Σ value·weight) / (Σ weight)` over the remaining entries (two sequential float loops: the
   weighted sum with each product rounded, then the weight sum).

So `TM(q)` removes the fraction `q/2` of the total weight at each end.

### 6.3 Weighted percentiles

- `LOW(list)`: sort; `t = (0.5f · 0.5f) · Wsum`; the bottom-trim loop of §6.2; return the **value** of
  the first remaining entry.
- `HIGH(list)`: the same from the top; return the value of the last remaining entry.

### 6.4 Combined values

```
mid(L)   = (TM(L, 0.25) + TM(L, 0.75)) · 0.5f
avg      = mid(favg list)
s        = avg_safety
cmin     = ((1 − s) · 0.5f) · (TM(fmin, 0.25) + TM(fmin, 0.75)) + s · LOW(fmin list)
cmax     = ((1 − s) · 0.5f) · (TM(fmax, 0.25) + TM(fmax, 0.75)) + s · HIGH(fmax list)
cu       = mid(uavg list),   cv = mid(vavg list)
if |cmin − cmax| < 1e-6f: cmin = cmax = avg
```

---

## 7. Frame parameters [V]

```
low  = clamp(((cmin − avg) · dark_limit) + avg,   0, maxv)
high = clamp(((cmax − avg) · bright_limit) + avg, 0, maxv)
if input_tv:  cmin, cmax, avg = clamp(each, tv_lo, tv_hi)     # AFTER low/high were computed
if output_tv: low, high       = clamp(each, tv_lo, tv_hi)

sum    = (gain_mode == 0) ? (low + high) : (cmin + cmax)
target = (((1 − (1 / gamma_limit)) · ((0.5f · sum) − avg)) + avg) / maxv
gamma  = log(target) / log(avg / maxv)                        # float, correctly rounded logs

if auto_gain and chroma_process ≥ 1 and high ≠ low and cmax ≠ cmin:
    cgain = ((((high − low) / (cmax − cmin)) − 1) · 0.8515f) · ((float)chroma_process / 100) + 1
else:
    cgain = 1

if auto_balance:
    lim  = (chroma_limit − 1) · half
    balU = clamp((half − cu) · balance_str, −lim, lim)
    balV = clamp((half − cv) · balance_str, −lim, lim)
else:
    balU = balV = 0
```

`cmin`, `cmax`, `avg` from here on are the (possibly TV-clamped) values. `gamma < 1` brightens: the
gamma maps the frame average towards `target`, which lies between the average and the midpoint of the
target range (`gamma_limit` = 1 → `target` = average → `gamma` = 1).

Degenerate inputs (`avg = 0` or `avg = maxv`, `target ≤ 0`) give infinities/NaN in `gamma`; the
original does not guard against this **[I]**. A uniform frame gives `cmin = cmax = avg = low = high`,
`gamma = 1` and an identity.

---

## 8. Pixel transforms [V]

All per pixel with `v = (float)sample`. Each transform yields the delta `d = r − v`, which is kept
together with `v` for §9/§10.

### 8.1 Luma, `gain_mode = 0` (piecewise linear around the average, then gamma)

```
# upper segment [avg, cmax] -> [avg, high]
if cmax == high or cmax <= avg or high <= avg: up = v
else: up = ((v − avg) · (high − avg)) / (cmax − avg) + avg
# lower segment [cmin, avg] -> [low, avg]
if cmin == low or avg <= cmin or avg <= low: dn = v
else: dn = ((v − cmin) · (avg − low)) / (avg − cmin) + low
r = (v < avg) ? dn : ((v > avg) ? up : v)
if low ≠ high and gamma ≠ 1.0 and low ≤ r ≤ high:
    r = pow((r − low) / (high − low), gamma) · (high − low) + low
d = r − v
```

The linear segments extrapolate outside [cmin, cmax]; values below `low` / above `high` are neither
gamma corrected nor clamped here.

### 8.2 Luma, `gain_mode = 1` (linear min/max stretch, then gamma)

```
if cmin == cmax or low == high: r = v
else:
    t = (v − cmin) / (cmax − cmin)
    if 0 ≤ t ≤ 1: t = pow(t, gamma)
    r = t · (high − low) + low
d = r − v
```

### 8.3 Chroma (U with `balU`, V with `balV`)

```
c = half
x = v − c
r = ((x · (((1 − |x / c|) · (cgain − 1)) + 1)) + bal) + c
d = r − v
```

The gain is full near neutral and falls linearly to 1 at the extremes, so saturated colours are not
pushed into clipping; `bal` is a plain offset.

---

## 9. Delta smoothing (`use_interp`) [V]

With `use_interp=false` this section is skipped. Otherwise the delta plane `D` (size `w×h`) of every
processed plane is filtered twice — vertically into a temporary plane `T`, then horizontally from `T`
back into `D` — with a **clamped box filter**: every tap is clamped to `center ± thr` (`thr` from §3),
so only small differences (below half a code value) are averaged and steps in the correction stay
sharp. This hides the banding caused by applying a fractional correction to integer samples.

### 9.1 Radius and step

`step = high_quality ? 1 : 2`. With `W`, `H` the input clip's luma width and (stacked-halved) height:

```
base = ((W + H) · 16) >> 11                    # integer, = (W+H) / 128
q, m = divmod(base, step)
R    = q·step + (m ≥ (step + 1) / 2 ? step : 0)  # round half up to a multiple of step (integer division)
R    = clamp(R, 8, 32)
```

Luma uses `R` in both directions; chroma uses `R >> vshift` vertically and `R >> hshift` horizontally
(YV12 1080p: luma 24 / chroma 12 with step 2, luma 23 / chroma 11 with step 1). The radius depends on
the input clip, not on `external_clip`.

### 9.2 Tap sets (one pass along an axis of length `L`, radius `ρ`, step `s`)

For output position `p` with center value `c = src[p]`, each tap is `min(max(src[t], c − thr), c + thr)`:

| Region | Taps `t` | Normalisation |
|---|---|---|
| `p < ρ` | `0, s, 2s, …` while `t ≤ p + ρ` | `sum / (float)count` |
| `ρ ≤ p < L − ρ` | `p−ρ, p−ρ+s, …, p+ρ` | `sum · (1 / ((float)(2ρ) / (float)s + 1))` |
| `p ≥ L − ρ` | `p−ρ, p−ρ+s, …` while `t ≤ L − 1` | `sum / (float)count` |

The top/left border taps start at index 0 (not at `p − ρ` on the step grid). The interior multiplies by
a precomputed reciprocal, the borders divide.

For planes shorter than `2ρ` in a direction the regions overlap; the original then reads outside the
plane (undefined) and the later region overwrites the earlier one **[I]**. A reimplementation should
clamp tap indices to the plane in that case.

### 9.3 Summation order (reference path)

- Vertical pass: two interleaved accumulators, `E = t0 + t2 + t4 + …` and `O = t1 + t3 + …`, filled
  pair by pair; `sum = E + O`; if the tap count is odd, the last tap is added afterwards.
- Horizontal pass: if the tap count `n ≥ 8`, taps `0 … n8−1` (`n8 = n & ~7`) go into eight
  accumulators, `A0..A3` receiving taps `8j+l` and `B0..B3` taps `8j+4+l`; then `Sl = Al + Bl`,
  `sum = (S0 + S2) + (S1 + S3)`, then taps `n8 … n−1` are added sequentially. If `n < 8` the taps are
  summed sequentially.

With this order the reference model is bit-exact against `asm_opt=1` **[V]**.

---

## 10. Output conversion and dither

### 10.1 Rounding / dithering [V]

For every processed sample at plane coordinates (x, y), with `d` the smoothed delta (or the raw delta
with `use_interp=false`):

```
f = (d + v) + 0.5f                 # use_dither = false
f = (d + v) + M[y & 63][x & 63]    # use_dither = true
f = (f >= 0) ? min(f, maxv) : 0    # NaN -> 0
out = (int)f                       # truncation
```

8-bit: one byte; stacked 16-bit: MSB/LSB as in §4.2. There is no clamping to TV range here.

### 10.2 Dither matrix [V]

A 64×64 float threshold matrix `M` is built once per filter instance and used for all planes and all
frames, indexed with the absolute plane coordinates:

```
M[y][x] = clamp( Bayer[(y + by) mod 16][(x + bx) mod 16] / 256 · 0.25
               + Noise[(y + ny) mod 64][(x + nx) mod 64] / 4096 · 0.75 , 0, 1 )
```

evaluated in float as `((float)Bayer / 256.0f) · 0.25f + ((float)Noise / 4096.0f) · 0.75f`. `M` is
strictly below 1, so samples with `d = 0` stay unchanged.

- `Bayer` (values 0…255): let `P1 = [[0, 2], [3, 1]]` and let `P(k+1)` be the 2^(k+1)-square matrix
  made of the quadrants `[[4·Pk, 4·Pk + 2], [4·Pk + 3, 4·Pk + 1]]`; then `Bayer[row][col] = P4[col][row]`
  (the transpose). Row 0 is `0 192 48 240 12 204 60 252 3 195 51 243 15 207 63 255`, row 1 is
  `128 64 176 112 140 76 188 124 131 67 179 115 143 79 191 127`.
- `Noise` (values 0…4095, a permutation of 0…4095 — a 64×64 rank / blue-noise style matrix). The
  original's table is not reproduced here; since its offsets are clock-seeded, its output cannot be
  matched anyway, and any 64×64 blue-noise rank matrix gives the same visual behaviour.
- The offsets `bx, by ∈ [0, 15]`, `nx, ny ∈ [0, 63]` are drawn at construction from a `minstd_rand`
  engine (multiplier 48271, modulus 2^31−1) seeded with the current Unix time in seconds
  (`time(NULL) mod (2^31−1)`, 0 replaced by 1), in the order `bx, by, nx, ny`, through the C++ standard
  library's uniform integer distribution **[I]**.

Verified **[V]**: with `use_dither=true` the output differs between runs (by ±1 in a large share of the
pixels of a processed plane) depending on the second in which the script was opened. The matrix
structure, the 0.25/0.75 weights, the clamping and the indexing were confirmed by recovering `M` from
one instance (offsets bx=2, by=15, nx=43, ny=63; zero mismatches over a 64×64 period). A
reimplementation cannot reproduce the original's dither pattern of a given run; it should keep the
matrix definition and use fixed offsets or a user seed. All bit-exact comparisons in this document use
`use_dither=false`.

---

## 11. Per-frame algorithm (pseudo-code)

```
GetFrame(n):
    S(k)    : cached record of §5 for external_clip frame k
    lists   = temporal window of §6.1 around n
    cmin, cmax, avg, cu, cv               = §6.4
    low, high, gamma, cgain, balU, balV   = §7
    frame   = writable copy of input frame n
    for each processed plane P (§4.4):
        for every sample: (v, d) = §8.1 / §8.2 (luma) or §8.3 (chroma)
        if use_interp: d = horizontal_pass(vertical_pass(d))      # §9
        write §10 into P
    return frame               (or the debug canvas of §14)
```

Apart from caches there is no state across frames; the output of frame `n` does not depend on which
frames were requested before or in which order **[V]**.

---

## 12. SIMD paths (`asm_opt`)

### 12.1 Selection

| `asm_opt` | Path |
|---|---|
| 0 (default) | AVX2 if the CPU has AVX2, else SSE2 if available, else C. On current CPUs this is AVX2 (output identical to `asm_opt=3` **[V]**). |
| 1 | C (reference) |
| 2 | SSE2 (error "Invalid SIMD" if absent) |
| 3 | AVX2 (error "Invalid SIMD" if absent) |

### 12.2 Differences to the reference

Measured on 1080p frames with `use_dither=false` **[V]**: with `use_interp=false` all three paths gave
identical output in every tested setting; with `use_interp=true` SSE2 and AVX2 differ from C by ±1 in
about 170–220 luma pixels and 0–4 chroma pixels per frame, and from each other in about 5–13 pixels.
With `gamma_limit=1` the difference drops to about 2 pixels, so most of it comes from `pow`. Causes
found in the code **[I]**:

- The SIMD luma kernels compute `pow(t, g)` as `exp2(g · log2(t))` with polynomials. log2: exponent of
  `t` plus `p(m)·(m − 1)` for the mantissa `m ∈ [1, 2)`, `p(m) = ((((c0·m + c1)·m + c2)·m + c3)·m + c4)·m + c5`,
  `c = (−0.034436006, 0.31821337, −1.2315303, 2.5988452, −3.3241990, 3.1157899)`; the product
  `y = g·log2(t)` is clamped to [−126.99999, 129]; exp2: `i = round_to_nearest(y − 0.5)`, `f = y − i`,
  `2^i · q(f)` with `q(f) = ((((d0·f + d1)·f + d2)·f + d3)·f + d4)·f + d5`,
  `d = (0.0018775767, 0.0089893397, 0.055826318, 0.24015361, 0.69315308, 0.99999994)`. The AVX2 path
  evaluates the polynomials with fused multiply-add.
- The SIMD luma kernels skip the gamma step only for `gamma == 0` (the C path for `gamma == 1`), so they
  apply the approximate `pow` even when the gamma is 1; a sample exactly equal to `avg` takes the lower
  segment instead of staying unchanged.
- SIMD chroma evaluates `(x·w + c) + bal` instead of `(x·w + bal) + c`.
- SIMD delta smoothing sums the taps in a different order (sequentially per pixel in the vertical pass,
  a transposed 4-pixel scheme in the horizontal pass).
- The statistics code is shared, except that the histogram buffers are 16/32-byte aligned in the SIMD
  paths (`peel = 0` in §5.3/§5.4) and the scene-change SAD uses vector kernels.

This specification defines the C path exactly; the SIMD paths are documented only to this level.
Matching the *default* output of the original on an AVX2 machine would require replicating the AVX2
kernels including FMA rounding, which was not attempted.

---

## 13. Threads, MT modes, caching

- `threads_count = 0`: number of logical processors, at most 16 — but forced to **1** when the host
  provides both `SetFilterMTMode` and `Prefetch` (AviSynth+), or both `SetMTMode` and `Distributor`
  (AviSynth-MT) **[I]**. Under AviSynth+ the default is therefore single-threaded inside the filter.
- `threads_count > 0`: that many workers. Each plane is split into horizontal stripes (stripe borders
  rounded to multiples of 2/4/8/16 rows depending on the SIMD path), the passes of §8–§10 are separated
  by barriers; the scene-change sums are split the same way.
- The output does not depend on `threads_count` (0, 1, 3 tested: identical) **[V]**, except for the
  last-bit float order of the scene-change sums (§5.5).
- `SetCacheHints` reports `MT_NICE_FILTER` to AviSynth+ (the internal caches are lock protected); the
  filter requests a generic cache on the input clip and a window cache of `temporal_radius + 1` frames
  on `external_clip` **[I]**. Internal caches: statistics records (1024), luma histograms (16), source
  frames used for statistics (16). `GetVersion` returns 6.
- The filter keeps no frame-to-frame state that affects the normal output; only `debug_view` keeps a
  history (§14).

---

## 14. `debug_view` (approximate)

With `debug_view=true` the filter returns an analysis canvas instead of the corrected frame **[V]**:

- **Size**: `Wd = clamp(screen width rounded down to a multiple of 8, 1280, 2560)`,
  `Hd = clamp(screen height rounded down to a multiple of 8, 768, 1600)`, `Hd` doubled with
  `high_bitdepth` (screen size from the OS; 2560×1600 on the test machine). Same pixel type as the
  input. Background black (Y = 16·2^(B−8), U = V = half; LSB halves 0).
- **Title** `AutoAdjust v2.60`, centred near the top.
- **Histograms**: two 256-pixel wide panels, left labelled `Input`, right `Output` (x ≈ 240 and
  x ≈ Wd − 496 on a 2560 canvas, y ≈ 128…383), each with three stacked bars: the luma histogram (the
  excluded dark/bright tails marked in orange), the U histogram on a yellow-to-blue ramp and the V
  histogram on a green-to-red ramp.
- **Parameter dump** in two text columns between the histograms, format `name<padding>= value`; left:
  `auto_gain, dark_limit, bright_limit, gamma_limit, dark_exclude, bright_exclude, gain_mode,
  chroma_process, avg_safety, input_tv, output_tv`; right: `auto_balance, chroma_limit, balance_str`,
  an empty line, `scd_threshold, temporal_radius, change_status` (shown as `empty` when empty),
  `use_interp, use_dither, high_quality, high_bitdepth`. Floats in shortest form (`1.5`, `0.25`,
  `1.05`), bools `true`/`false`. `threads_count`, `asm_opt` and `debug_view` are not shown.
- **History graph**: a 256×256 box in the middle of the top area (dark grey, dashed border). One pixel
  column per frame, the current frame at the right edge (x = 255), frame `k` at x = 255 − (n − k);
  y = 255 − value (8-bit scale). Two pairs of curves in two grey levels: per-frame `fmin`/`fmax` and the
  temporal `cmin`/`cmax` (before the TV clamp); dashed white vertical lines at frames with status 1.
  Only frames that this instance has already produced appear (the history keeps up to 1024 frames), so
  the graph depends on the request order.
- **Images**: below the top area the input frame (left) and the output frame (right, identical to the
  non-debug output), each centre-cropped to `(Wd − 32) / 2` columns (1264 on a 2560 canvas, i.e. source
  columns 328…1591 of a 1920 frame), separated by a 32-pixel gap, at full source height (rows 452…1531
  for a 1080-line source on a 1600 canvas). Labels `Input`/`Output` in their top left corners. On the
  input image a darkened box shows

```
MIN: <fmin of frame n>
MAX: <fmax of frame n>

MIN AVG: <cmin, before the input_tv clamp>
MAX AVG: <cmax, before the input_tv clamp>

LAST SC: FRM <latest frame <= n with status 1 in the history, else -1>
CUR POS: FRM <n>
```

  with 6 significant digits (`35.9648`, `250.422`).

Exact pixel positions, font and colours are not specified further; a reimplementation only needs a
comparable overlay. (The values shown were checked against the reference model **[V]**.)

---

## 15. Verification summary

Method: a reference model written from this specification (float32 throughout) compared against the
original DLL in AviSynth+ through the C API, with `asm_opt=1, use_dither=false` unless stated.

| Area | Result |
|---|---|
| Defaults (`AutoAdjust()`), `chroma_process` without `auto_gain` (also with dither) | exact identity [V] |
| YV12 1080p, frames 0/150/590/600: auto_gain; gain_mode 1; auto_balance; high_quality; auto_balance+high_quality+gain_mode 1+radius 5; a "PC" set (input_tv/output_tv off, dark_limit 3, bright_limit 2, gamma_limit 2, avg_safety 0.6, excludes 0/1, chroma_process 150, scd 5) | bit-exact, all planes [V] |
| temporal_radius 0 and 20, use_interp on/off | bit-exact [V] |
| clip with two hard cuts, scd 0/12/40, change_status `"!60;100-105;130"` and `"70-71;!72"` | bit-exact, detected scene starts as predicted [V] |
| YV24, YV16, YV411, Y8, width 1918 | bit-exact [V] |
| stacked 16-bit (`high_bitdepth`): default, gain_mode 1 + balance + radius 3 | bit-exact [V] |
| external_clip 640×360 (other levels) for a 1080p input | bit-exact [V] |
| request order inside one instance | no influence [V] |
| threads_count 0/1/3 | identical [V] |
| dither matrix structure and indexing | recovered from an instance, 0 mismatches [V] |
| dither seeding from the clock, distribution details | [I] |
| error messages, order, change_status grammar | [V] (checks 18 and 20 [I]) |
| SIMD path differences | measured [V], causes [I] |
| debug_view layout | observed [V], details approximate |

## 16. Open questions and recommendations

1. **[?]** The exact SIMD (SSE2/AVX2) arithmetic is not specified; following this document does not
   reproduce the original's default (`asm_opt=0`) output on AVX2 machines bit-exactly (±1 in ~0.01 % of
   luma pixels with `use_interp`).
2. **[?]** Planes shorter than twice the smoothing radius (§9.2) are read out of bounds by the original;
   clamp the indices in a reimplementation.
3. **[?]** NaN/inf handling for `avg = 0` or full-scale frames (§7) is not guarded in the original.
4. **[?]** The exact uniform-distribution algorithm used for the dither offsets was not reproduced
   (irrelevant unless one wants to emulate the clock-seeded offsets).
5. Recommendation: native high bit depth (10–16 bit, float) and alpha formats are not handled by the
   original (§4.1). A new filter should reject them or implement them with `B` = real bit depth
   (2^B histogram bins, `thr = 2^(B−9)`, TV constants scaled by 2^(B−8)), which reproduces the stacked
   16-bit behaviour for B = 16.
6. Recommendation: in VapourSynth, map `high_bitdepth` to native 16-bit formats; do not make the
   `debug_view` canvas depend on the screen size.
7. Recommendation: offer a seed (or fixed offsets) for the dither matrix so results are reproducible.

---

## Appendix A — reference values

Frame 0 of the 1080p test clip (`G:\TestClips&Co\Sample.mp4` via LSMASHVideoSource, ConvertToYV12),
`auto_gain=true, temporal_radius=0`, C path: `fmin = 37.09772`, `fmax = 242.66693`, `favg = 86.98755`, `low = 16`,
`high = 235`, `gamma = 0.9211121`, `cgain = 1.0907756`. Frame 100: `fmin = 35.96483`,
`fmax = 250.42206`, `favg = 92.318405`; with `temporal_radius=20`: `cmin = 35.31876`, `cmax` before the TV clamp `= 249.977`,
`avg = 92.75648`, `gamma = 0.9325411`, `cgain = 1.0823809`.

Suggested regression tests: identity with default parameters; synthetic gradients with
`use_interp=false, use_dither=false` for the curves; a spliced clip with hard cuts for the temporal
window; the `change_status` cases of §5.6; a stacked 16-bit clip.
