#!/usr/bin/env python3
"""Negative-tests every rule in `check_platforms.ps1` and `check_modules.ps1`, one at a time.

Run it after changing either script:  python modules/graphics/tools/check_negative.py

Both scripts have already produced one rule that could not fail. `check_modules.ps1`'s include-graph rule raised
`InvalidOperation` instead of reporting, because PowerShell binds `-f` tighter than `+`; the rule was dead and the
script's exit code came from somewhere else, so from a distance it looked like a pass. That was found by breaking the
thing under test on a hunch, which is not a method.

This is the method. Every rule gets broken deliberately, the script is run, and the specific message that rule prints is
required in the output before the rule counts as working. A rule whose message never appears is a rule that cannot fail,
whether or not the script happens to exit non-zero.

Restores are done in a `finally` from an in-memory copy, and the tree is verified clean at the end, because a test that
leaves the tree modified is worse than no test -- which the first version of this learned the hard way.
"""
import pathlib
import subprocess
import sys

root = (pathlib.Path(__file__).resolve().parents[3])
tools = root / "modules/graphics/tools"

PLATFORMS = tools / "check_platforms.ps1"
MODULES = tools / "check_modules.ps1"

# (script, file to break, anchor, replacement, description, message the rule must print)
CASES = [
    # --- check_platforms.ps1 -----------------------------------------------------------------------------------
    (PLATFORMS,
     root / "modules/graphics/tools/linuxstub/X11/Xlib.h",
     "#define CWEventMask (1L << 11)", "#define CWEventMask (1L << 12)",
     "X11 constant with the wrong value", "do not match the platform's header"),

    (PLATFORMS,
     root / "modules/graphics/tools/linuxstub/GL/glx.h",
     "#define GLX_PBUFFER_HEIGHT 0x8040", "#define GLX_PBUFFER_HEIGHT 0x8040\n#define GLX_MADE_UP 0x9999",
     "GLX constant that does not exist", "NOT IN THE PLATFORM'S HEADER"),

    (PLATFORMS,
     root / "modules/graphics/tools/androidstub/android/bitmap.h",
     "  uint32_t flags;", "",
     "AndroidBitmapInfo member removed", "missing member 'flags'"),

    (PLATFORMS,
     root / "modules/graphics/tools/androidstub/android/bitmap.h",
     "ANDROID_BITMAP_FORMAT_RGBA_8888 = 1", "ANDROID_BITMAP_FORMAT_RGBA_8888 = 2",
     "Android constant with the wrong value", "expected"),

    (PLATFORMS,
     root / "modules/graphics/tools/macstub/OpenGL/OpenGL.h",
     "kCGLPFAAccelerated = 73", "kCGLPFAAccelerated = 74",
     "CGL constant with the wrong value", "do not match Apple's header"),

    (PLATFORMS,
     root / "modules/graphics/tools/macstub/OpenGL/OpenGL.h",
     "typedef enum {", "typedef enum {\n  kCGLInvented = 99,",
     "CGL constant that does not exist", "NOT IN APPLE'S HEADER"),

    # --- check_modules.ps1 -------------------------------------------------------------------------------------
    (MODULES,
     root / "modules/graphics/include/private/gcms/Gamut.h",
     "#include <optional>", "#include <graphics/ColorSpace.h>\n#include <optional>",
     "gcms with a graphics include", "outside the module"),

    (MODULES,
     root / "modules/graphics/src/gcms/Gamut.cpp",
     '#include "gcms/Gamut.h"', '#include "gcms/Gamut.h"\nstruct SkGamutProbe { };',
     "gcms with an Sk type in code", "upstream type"),

    (MODULES,
     root / "modules/graphics/include/private/gcms/Gamut.h",
     "#include <optional>", "// was skcms_TransferFunction_getType\n#include <optional>",
     "gcms naming upstream API in a comment", "upstream API"),

    (MODULES,
     root / "modules/graphics/include/private/pathops/Curve.h",
     "#include <geometry/Point.h>", "#include <geometry/Point.h>\n#include <backend/RHI.h>",
     "pathops reaching for the backend", "reaches for"),
]


def run(script):
    r = subprocess.run(["pwsh", "-NoProfile", "-File", str(script)],
                       capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=root)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


# Which scripts actually exist, so a missing one is reported rather than silently skipped.
for script in (PLATFORMS, MODULES):
    if not script.exists():
        print(f"missing script: {script}")
        sys.exit(1)

caught = 0
for script, path, old, new, description, expected in CASES:
    original = path.read_text(encoding="utf-8")
    if old not in original:
        print(f"  SETUP FAILED (anchor missing): {description}")
        continue

    try:
        path.write_text(original.replace(old, new, 1), encoding="utf-8", newline="\n")
        # Verify the break landed: an unapplied test looks exactly like a pass.
        if new.split("\n")[0] not in path.read_text(encoding="utf-8"):
            print(f"  SETUP FAILED (not applied): {description}")
            continue

        code, output = run(script)
        ok = expected in output
        print(f"  {'caught' if ok else 'MISSED'}: {description}")
        if ok:
            caught += 1
        else:
            print(f"      expected {expected!r}; exit={code}")
            for line in output.split("\n"):
                s = line.strip()
                if s and ("error" in s.lower() or "NOT IN" in s or "expected" in s or "Invalid" in s):
                    print(f"      | {s[:150]}")
    finally:
        path.write_text(original, encoding="utf-8", newline="\n")

print()
print(f"{caught}/{len(CASES)} rule breaks caught")

problems = 0
for script in (PLATFORMS, MODULES):
    code, _ = run(script)
    state = "clean" if code == 0 else "STILL FAILING"
    print(f"after restore, {script.name}: {state}")
    if code != 0:
        problems += 1

sys.exit(0 if (caught == len(CASES) and problems == 0) else 1)
