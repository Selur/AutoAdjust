#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Functional tests for the AutoAdjust VapourSynth plugin (API 4).

Usage: python test/test_autoadjust.py [path/to/AutoAdjust plugin]

Without an argument the plugin is expected to be autoloaded (e.g. from the installed wheel).
"""
import ctypes
import functools
import hashlib
import sys

import vapoursynth as vs

core = vs.core
FAILURES = []
W, H, LENGTH, CUT = 96, 64, 24, 12

# SHA-256 of all planes of frames 0, CUT - 1, CUT and LENGTH - 1 for each case below; every asm_opt the CPU
# supports (C, SSE2, AVX2 on x86, NEON on arm64) must give exactly this result.
# The 8-bit cases without dither equal the original AutoAdjust 2.60 C path (asm_opt=1, use_dither=false).
REFERENCE = {
    "gain": "fff6da7bf41bdfa5be3f956ae74f87fca3e0173ac32a84bffcdc4eef96c75efc",
    "gain_mode=1": "b93a31cf31c3d39ef94eb3ed59c440e02aa6b1a8e8b52b4c75638ed5d9ca49c7",
    "gain balance": "77b85fd6b3964b39ab69db00155b03d01d83d806d5884f630cebf7c551f3150e",
    "gain balance high_quality": "9044d572a6acc25a763f7e4e0bf9b790acce00dbef6a0d259863326001cf064c",
    "gain use_interp=0": "b21d34760edf4fb4c59cd113b5c5e55cb4ae5b84958462dbabf915b30d1ac008",
    "gain temporal_radius=0": "9de867320de8822c7d18ebe5d7413a976196025bba46cb26f1a2ff0c8917c8a3",
    "gain pc": "b2addf95dbc7ad3204bf1147c958f8cb5a99e7fe9996c4eb9fb1811890bbb785",
    "gain change_status": "a8def670537656ca23df7429e54faadc6622c869be1340ba3e94ab32d3f62438",
    "gain scd_threshold=100": "6c7d893d2a995e14f895b5335e3ef3486694820b4480678f2c6acf02e4c3528e",
    "gain external_clip": "5cbd3ba8173483f5216afa29e232fefcc58f63eb33f661ef7f94535510ff20e2",
    "gain balance dither": "88cce1d9e59596f87ba2bed3c0054bae149160a471bc62635d0a583a53a53cb3",
    "gain balance YUV444P8": "1e6be20445b8568fb5ddbba6edb56e8006df5bf2e01b6b370e52bd787595c64e",
    "gain GRAY8": "870ac8f7cbd34bde3f80797d3be76bc970376356adf0165c486747761cb044db",
    "gain balance YUV420P10": "545d6b284d54dab836fa231b5ee327600265b6f54f96947ccb2db71e9e9e9733",
    "gain balance YUV420P16": "29c72d52b55a2a8a274254512b2035920e94cfc60d4fdbbfce90d7f9b3844834",
}


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL: " + msg)
        return
    print("ok  ", msg)


def expect_error(fn, msg, contains):
    try:
        fn()
    except vs.Error as e:
        check(contains in str(e), "%s (%s)" % (msg, str(e).strip().splitlines()[-1]))
    else:
        check(False, "no error raised: " + msg)


def pattern(w, h, n, p):
    """Low-contrast gradient plus deterministic noise; two scenes with different levels and casts."""
    scene = 0 if n < CUT else 1
    lo, span = ((70, 100), (40, 70))[scene]
    if p > 0:
        lo, span = ((112, 24), (130, 20))[scene][p - 1], 16
    rows = []
    state = (w * 7919 + h * 104729 + n * 1299709 + p * 15485863) & 0xFFFFFFFF
    for y in range(h):
        row = bytearray(w)
        for x in range(w):
            state = (state * 1103515245 + 12345) & 0x7FFFFFFF
            noise = (state >> 16) % 9 - 4
            base = lo + ((x * span) // w + (y * span) // (2 * h) + n) % (span + 1)
            if (x // 16 + y // 12) % 4 == 0:
                base += span // 3
            row[x] = max(0, min(255, base + noise))
        rows.append(bytes(row))
    return rows


def pattern_clip(fmt=vs.YUV420P8):
    """8 bit pattern clip; other formats are made from it with resize.Point."""
    blank = core.std.BlankClip(width=W, height=H, format=vs.YUV420P8, length=LENGTH)

    def fill(n, f):
        fout = f.copy()
        for p in range(3):
            pw, ph = (W, H) if p == 0 else (W // 2, H // 2)
            ptr = fout.get_write_ptr(p)
            ptr = ptr.value if hasattr(ptr, "value") else ptr
            stride = fout.get_stride(p)
            for y, row in enumerate(pattern(pw, ph, n, p)):
                ctypes.memmove(ptr + y * stride, row, pw)
        return fout

    clip = core.std.ModifyFrame(blank, blank, fill)
    if fmt != vs.YUV420P8:
        clip = core.resize.Point(clip, format=fmt)
    return core.std.Cache(clip) if hasattr(core.std, "Cache") else clip


def plane(clip, p, n=0):
    return bytes(clip.get_frame(n)[p])


def digest(clip, frames=(0, CUT - 1, CUT, LENGTH - 1)):
    h = hashlib.sha256()
    for n in frames:
        f = clip.get_frame(n)
        for p in range(f.format.num_planes):
            h.update(bytes(f[p]))
    return h.hexdigest()


def cases(src, opt):
    aa = functools.partial(core.autoadjust.AutoAdjust, asm_opt=opt)
    gray = core.std.ShufflePlanes(src, 0, vs.GRAY)
    ext = core.std.Levels(src, min_in=30, max_in=220, planes=0)
    return {
        "gain": lambda: aa(src, auto_gain=1, use_dither=0),
        "gain_mode=1": lambda: aa(src, auto_gain=1, gain_mode=1, use_dither=0),
        "gain balance": lambda: aa(src, auto_gain=1, auto_balance=1, use_dither=0),
        "gain balance high_quality": lambda: aa(src, auto_gain=1, auto_balance=1, high_quality=1, use_dither=0),
        "gain use_interp=0": lambda: aa(src, auto_gain=1, use_interp=0, use_dither=0),
        "gain temporal_radius=0": lambda: aa(src, auto_gain=1, temporal_radius=0, use_dither=0),
        "gain pc": lambda: aa(src, auto_gain=1, input_tv=0, output_tv=0, dark_limit=3, bright_limit=2,
                              gamma_limit=2, avg_safety=0.6, dark_exclude=0, bright_exclude=1,
                              chroma_process=150, use_dither=0),
        "gain change_status": lambda: aa(src, auto_gain=1, change_status="!%d;5-7" % CUT, use_dither=0),
        "gain scd_threshold=100": lambda: aa(src, auto_gain=1, scd_threshold=100, use_dither=0),
        "gain external_clip": lambda: aa(src, external_clip=ext, auto_gain=1, use_dither=0),
        "gain balance dither": lambda: aa(src, auto_gain=1, auto_balance=1),
        "gain balance YUV444P8": lambda: aa(pattern_clip(vs.YUV444P8), auto_gain=1, auto_balance=1, use_dither=0),
        "gain GRAY8": lambda: aa(gray, auto_gain=1, use_dither=0),
        "gain balance YUV420P10": lambda: aa(pattern_clip(vs.YUV420P10), auto_gain=1, auto_balance=1, use_dither=0),
        "gain balance YUV420P16": lambda: aa(pattern_clip(vs.YUV420P16), auto_gain=1, auto_balance=1, use_dither=0),
    }


def main():
    if len(sys.argv) > 1:
        core.std.LoadPlugin(sys.argv[1])
    check(hasattr(core, "autoadjust") and hasattr(core.autoadjust, "AutoAdjust"),
          "plugin loaded (namespace autoadjust, function AutoAdjust)")

    src = pattern_clip()
    aa = core.autoadjust.AutoAdjust

    out = aa(src, auto_gain=1)
    check(out.format.id == src.format.id and out.width == W and out.height == H and out.num_frames == LENGTH,
          "format, size and length kept")
    check(digest(aa(src)) == digest(src), "defaults (no auto_gain/auto_balance) are an identity")
    check(digest(aa(src, chroma_process=150)) == digest(src), "chroma_process without auto_gain is an identity")

    y_in, y_out = plane(src, 0), plane(aa(src, auto_gain=1, use_dither=0), 0)
    check(max(y_out) - min(y_out) > max(y_in) - min(y_in) + 40,
          "auto_gain widens the luma range (%d-%d -> %d-%d)" % (min(y_in), max(y_in), min(y_out), max(y_out)))
    gained = aa(src, auto_gain=1, use_dither=0)
    check(plane(gained, 1) != plane(src, 1), "auto_gain also raises the saturation (chroma_process)")
    bal = aa(src, auto_balance=1, use_dither=0)
    check(plane(bal, 0) == plane(src, 0) and plane(bal, 1) != plane(src, 1), "auto_balance alone changes only chroma")

    # Results must not depend on the order in which frames are requested.
    a = aa(src, auto_gain=1, auto_balance=1, use_dither=0)
    b = aa(src, auto_gain=1, auto_balance=1, use_dither=0)
    order_a = [digest(a, (n,)) for n in range(LENGTH)]
    order_b = [digest(b, (n,)) for n in reversed(range(LENGTH))][::-1]
    check(order_a == order_b, "request order does not change the result")
    check(digest(aa(src, auto_gain=1)) == digest(aa(src, auto_gain=1)), "dither is deterministic")

    opts = []
    for opt in (0, 1, 2, 3):
        try:
            aa(src, asm_opt=opt)
            opts.append(opt)
        except vs.Error as e:
            check("Invalid SIMD" in str(e), "asm_opt=%d not available on this CPU" % opt)
    print("info asm_opt values available: %s" % opts)
    for opt in opts:
        for name, make in cases(src, opt).items():
            d = digest(make())
            expected = REFERENCE.get(name)
            if expected is None:
                print("info reference %s (asm_opt=%d): %s" % (name, opt, d))
            else:
                check(d == expected, "reference result: %s, asm_opt=%d" % (name, opt))

    expect_error(lambda: aa(src, dark_limit=0.5), "dark_limit range", "dark_limit must be between 1.0 and 100.0")
    expect_error(lambda: aa(src, gain_mode=2), "gain_mode range", "gain_mode must be 0 or 1")
    expect_error(lambda: aa(src, chroma_process=201), "chroma_process range", "chroma_process must be between 0 and 200")
    expect_error(lambda: aa(src, temporal_radius=-1), "temporal_radius range", "temporal_radius must be >= 0")
    expect_error(lambda: aa(src, asm_opt=4), "asm_opt range", "asm_opt must be between 0 and 3")
    for cs in ("1-", "3-1", "!!1", "a", ";5", "!", "%d" % LENGTH):
        expect_error(lambda cs=cs: aa(src, change_status=cs), "change_status %r" % cs, "change_status invalid syntax!")
    expect_error(lambda: aa(core.resize.Point(src, format=vs.RGB24)), "RGB rejected", "only 8-16 bit integer YUV")
    expect_error(lambda: aa(core.resize.Point(src, format=vs.YUV420PS)), "float rejected", "only 8-16 bit integer YUV")
    expect_error(lambda: aa(src, external_clip=core.resize.Point(src, format=vs.YUV444P8)), "external_clip format",
                 "Colorspace of clip and external_clip must be the same")
    expect_error(lambda: aa(core.std.Crop(core.resize.Point(src, format=vs.YUV444P8), right=1)), "odd width",
                 "Width and height must be MOD2")

    if FAILURES:
        print("%d failure(s)" % len(FAILURES))
        sys.exit(1)
    print("all tests passed")


if __name__ == "__main__":
    main()
