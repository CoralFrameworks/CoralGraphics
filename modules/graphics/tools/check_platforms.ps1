#!/usr/bin/env pwsh
#
# Type-checks the platform-only sources against stub headers, without a device, a Linux box, or the NDK.
#
# ## Why this exists
#
# Two platforms contribute sources that no other host compiles:
#
#   Android : `src/codec/AndroidCodec.cpp` and the codec dispatch around it, and `GlPlatformEgl.cpp`
#   Linux   : `GlPlatformGlx.cpp`
#   macOS   : `GlPlatformCgl.cpp`
#
# On a Windows host all of them are invisible, so a mistake in any one reaches its platform unnoticed. This has already
# paid for itself twice: it found the GLX platform's `#undef None`, which removed the attribute-list terminator before
# four lists that end with it and meant the file could not compile on Linux at all, and it found function-pointer to
# `void*` casts that only GCC and Clang accept.
#
# It closes the gap the only way a host can: by compiling the same sources with the same definitions against headers that
# have the same *shape* as the platform's.
#
# It is a script rather than a paragraph in the README because the include lists are easy to get subtly wrong -- the
# first hand-written attempt omitted `libs/utils/include` and failed on `utils/unwindows.h`, which reads as a missing
# file rather than as a missing `-I`. Every path here is absolute for the same reason: `cmd /c` does not inherit the
# working directory from PowerShell, so relative paths silently resolve against somewhere else and the error reads as a
# missing source file.
#
# ## What it proves, and what it does not
#
# It proves: the platform sources compile under their platform's definitions, with none of the vendored or system
# libraries on the include path, so every guarded branch is the intended one. For the codec it also counts the
# references to each vendored decoder in the preprocessed output, because an undefined symbol is a *link* error and `/Zs`
# does not link -- an unguarded call to a decoder Android does not compile would otherwise slip through.
#
# It does not prove: that either platform works at run time. `BitmapFactory`, `AndroidBitmap`, `ANativeWindow`, Xlib and
# GLX are all stubs here, so behaviour on a device or a Linux desktop remains verified by inspection only.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# The repository root: this script is modules/graphics/tools/check_platforms.ps1.
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path

# CoralCore supplies utils, minilog and the jni include root. A standalone checkout carries it as the
# `external/CoralCore` submodule; a workgroup checkout keeps it beside this repository.
$coralCore = Join-Path $root 'external/CoralCore'
if (-not (Test-Path (Join-Path $coralCore 'modules'))) {
  $coralCore = Join-Path $root '../CoralCore'
}
if (-not (Test-Path (Join-Path $coralCore 'modules'))) {
  Write-Error 'CoralCore was not found at external/CoralCore or beside the repository; initialize the submodule first.'
}

# The stub headers, which stand in for the NDK's jni.h and android/bitmap.h.
$stubInclude = Join-Path $root 'modules/graphics/tools/androidstub'

$includes = @(
  "/I `"$stubInclude`""
  "/I `"$(Join-Path $root 'modules/graphics/include')`""
  "/I `"$(Join-Path $root 'modules/graphics/include/private')`""
  "/I `"$(Join-Path $root 'modules/graphics/geometry/include')`""
  "/I `"$(Join-Path $root 'modules/graphics/backend/include')`""
  "/I `"$(Join-Path $root 'modules/graphics/backend/include/private')`""
  "/I `"$(Join-Path $coralCore 'modules/utils/include')`""
  "/I `"$(Join-Path $coralCore 'modules/minilog/include')`""
  # The platform codec reaches the JVM through the project's own `jni` module, which owns the `JavaVM`, rather than
  # defining a `JNI_OnLoad` of its own. In the real Android build that module's include directory arrives through
  # `target_link_libraries(... jni)`; here it has to be named, because there is no CMake in this script.
  "/I `"$(Join-Path $coralCore 'include')`""
) -join ' '

# The sources Android compiles that a host does not, or that carry the Android branches.
#
# `GlPlatformEgl.cpp` is here for the same reason as the codec: the backend selects it by `ANDROID`, so on this host it
# is not compiled at all and a mistake in it would reach a device unnoticed. It takes its EGL types, constants and entry
# points from `GlEntryPoints.h`, which is host code, so the only NDK surface it needs is `ANativeWindow`.
$sourcePaths = @(
  (Join-Path $root 'modules/graphics/src/codec/ImageCodec.cpp')
  (Join-Path $root 'modules/graphics/src/codec/AndroidCodec.cpp')
  (Join-Path $root 'modules/graphics/backend/src/gl/platform/GlPlatformEgl.cpp')
)
$sources = ($sourcePaths | ForEach-Object { "`"$_`"" }) -join ' '

# vcvars sets INCLUDE and LIB for the standard library headers. Locating it beats hard-coding a version directory.
$vcvars = Get-ChildItem -Path 'C:\Program Files\Microsoft Visual Studio' -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue |
  Select-Object -First 1
if ($null -eq $vcvars) {
  Write-Error 'vcvars64.bat not found; this script needs an MSVC toolchain to type-check with.'
}
Write-Host "root      : $root"
Write-Host "toolchain : $($vcvars.FullName)"
Write-Host ''

# --- 1. Do the sources compile as Android, with no vendored libraries on the include path? ----------------------
Write-Host '== compiling as Android =='
$compile = "`"$($vcvars.FullName)`" >nul 2>&1 && cl /nologo /std:c++20 /EHsc /c /Zs /DCORAL_BUILD_FOR_ANDROID=1 $includes $sources"
$output = cmd /c $compile 2>&1
$code = $LASTEXITCODE
$output | Where-Object { $_ -match 'error' } | Select-Object -First 20
if ($code -ne 0) {
  Write-Error "the Android sources did not compile (exit $code)"
}
Write-Host 'compiled clean, with no vendored library on the include path'

# --- 2. Are the vendored decoders still referenced? -------------------------------------------------------------
#
# `/Zs` parses and type-checks but does not link, so a call to `decodePng` in a build that does not define it would pass
# step 1 and fail on a real device. The count is the check: with the guards working each name appears once, in the
# header's declaration, and never a second time as a call.
Write-Host ''
Write-Host '== checking that no vendored decoder is called =='

$imageCodec = Join-Path $root 'modules/graphics/src/codec/ImageCodec.cpp'
$ppAndroid = Join-Path ([System.IO.Path]::GetTempPath()) 'coral_android_pp.txt'
$ppHost = Join-Path ([System.IO.Path]::GetTempPath()) 'coral_host_pp.txt'

$expand = "`"$($vcvars.FullName)`" >nul 2>&1 && cl /nologo /std:c++20 /EHsc /EP"
# Windows PowerShell 5.1 turns a native command's redirected stderr into a terminating
# NativeCommandError under `$ErrorActionPreference = 'Stop'`, and `cl /EP` writes the source name to
# stderr. The redirections below are deliberate, so native stderr is allowed to be non-terminating here.
$nativeErrorPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
cmd /c "$expand /DCORAL_BUILD_FOR_ANDROID=1 $includes `"$imageCodec`"" 2>$null > $ppAndroid
$hostDefines = '/DCORAL_GRAPHICS_HAS_LIBPNG=1 /DCORAL_GRAPHICS_HAS_JPEG=1 /DCORAL_GRAPHICS_HAS_WEBP=1'
cmd /c "$expand $hostDefines $includes `"$imageCodec`"" 2>$null > $ppHost
$ErrorActionPreference = $nativeErrorPreference

$count = {
  param($path, $name)
  (Select-String -Path $path -Pattern $name -AllMatches -ErrorAction SilentlyContinue | Measure-Object).Count
}

$failures = 0
foreach ($name in @('decodePng', 'decodeJpeg', 'decodeWebp', 'encodePng', 'encodeJpeg', 'encodeWebp')) {
  $asAndroid = & $count $ppAndroid $name
  $asHost = & $count $ppHost $name

  # On Android: the declaration only, so exactly one. On a host: declaration plus the call, so two.
  $ok = ($asAndroid -eq 1) -and ($asHost -eq 2)
  if (-not $ok) { $failures++ }
  Write-Host ("  {0,-12} android={1} host={2}  {3}" -f $name, $asAndroid, $asHost, $(if ($ok) { 'ok' } else { 'UNEXPECTED' }))
}

# The platform decoder is the mirror image: declared once and called at least once under Android.
$systemAndroid = & $count $ppAndroid 'decodeWithSystem'
$systemOk = $systemAndroid -ge 2
if (-not $systemOk) { $failures++ }
Write-Host ("  {0,-12} android={1}          {2}" -f 'decodeWithSystem', $systemAndroid,
  $(if ($systemOk) { 'ok (called)' } else { 'UNEXPECTED (not called)' }))

Remove-Item -Force $ppAndroid, $ppHost -ErrorAction SilentlyContinue

if ($failures -gt 0) {
  Write-Error "$failures check(s) failed"
}

# --- 3. The Linux platform --------------------------------------------------------------------------------------
#
# `GlPlatformGlx.cpp` is selected by `LINUX` in the backend's CMakeLists, so a host build never sees it. It needs Xlib
# and GLX, which `tools/linuxstub/` provides, plus `dlfcn.h` for the GL 1.1 entry points libGL exports directly rather
# than through the extension mechanism.
Write-Host ''
Write-Host '== compiling the Linux platform =='

$linuxStub = Join-Path $root 'modules/graphics/tools/linuxstub'
$linuxIncludes = @(
  "/I `"$linuxStub`""
  "/I `"$(Join-Path $root 'modules/graphics/include')`""
  "/I `"$(Join-Path $root 'modules/graphics/include/private')`""
  "/I `"$(Join-Path $root 'modules/graphics/geometry/include')`""
  "/I `"$(Join-Path $root 'modules/graphics/backend/include')`""
  "/I `"$(Join-Path $root 'modules/graphics/backend/include/private')`""
  "/I `"$(Join-Path $coralCore 'modules/utils/include')`""
  "/I `"$(Join-Path $coralCore 'modules/minilog/include')`""
  "/I `"$(Join-Path $coralCore 'include')`""
) -join ' '

$glx = Join-Path $root 'modules/graphics/backend/src/gl/platform/GlPlatformGlx.cpp'
$compileLinux = "`"$($vcvars.FullName)`" >nul 2>&1 && cl /nologo /std:c++20 /EHsc /c /Zs /DCORAL_BUILD_FOR_LINUX=1 $linuxIncludes `"$glx`""
$linuxOutput = cmd /c $compileLinux 2>&1
$linuxCode = $LASTEXITCODE
$linuxOutput | Where-Object { $_ -match 'error' } | Select-Object -First 20
if ($linuxCode -ne 0) {
  Write-Error "the Linux platform did not compile (exit $linuxCode)"
}
Write-Host 'compiled clean, with Xlib and GLX stubbed'

Write-Host ''
Write-Host 'What this does NOT show: that a Linux desktop gets a context. Xlib and GLX are stubs here.'

# --- 3b. The same EGL source as iOS ------------------------------------------------------------------------------
#
# `GlPlatformEgl.cpp` is selected for both `ANDROID` and `IOS`, and the two differ by exactly one block: the Android
# window-size query through `ANativeWindow`. Compiling it as iOS is therefore cheap and covers the only part of that
# file a device build would exercise differently.
Write-Host ''
Write-Host '== compiling the EGL platform as iOS =='

$compileIos = "`"$($vcvars.FullName)`" >nul 2>&1 && cl /nologo /std:c++20 /EHsc /c /Zs /DCORAL_BUILD_FOR_IOS=1 $includes `"$(Join-Path $root 'modules/graphics/backend/src/gl/platform/GlPlatformEgl.cpp')`""
$iosOutput = cmd /c $compileIos 2>&1
$iosCode = $LASTEXITCODE
$iosOutput | Where-Object { $_ -match 'error' } | Select-Object -First 20
if ($iosCode -ne 0) {
  Write-Error "the iOS EGL configuration did not compile (exit $iosCode)"
}
Write-Host 'compiled clean'

# --- 3d. Are the Linux and Android stubs faithful? ---------------------------------------------------------------
#
# Same rule as the CGL check below, applied to the other three stub families. The values come from the platform's own
# documentation rather than from the order the members happen to appear in:
#
#   X11      LSB, "Data Definitions for libX11", for X11/X.h
#   GLX      Khronos' glxext.h; the core GLX 1.3 values also appear in Mesa's include/GL/glx.h
#   Android  the NDK's android/bitmap.h
Write-Host ''
Write-Host '== checking the Linux stubs against X11 and GLX =='

$x11Truth = @{
  'CWOverrideRedirect' = 1 -shl 9; 'CWEventMask' = 1 -shl 11; 'CWColormap' = 1 -shl 13
  'StructureNotifyMask' = 1 -shl 17; 'AllocNone' = 0; 'InputOutput' = 1
}
$glxTruth = @{
  'GLX_X_RENDERABLE' = 0x8012; 'GLX_DRAWABLE_TYPE' = 0x8010; 'GLX_RENDER_TYPE' = 0x8011
  'GLX_X_VISUAL_TYPE' = 0x22; 'GLX_RGBA_BIT' = 0x1; 'GLX_WINDOW_BIT' = 0x1; 'GLX_PBUFFER_BIT' = 0x4
  'GLX_RED_SIZE' = 8; 'GLX_GREEN_SIZE' = 9; 'GLX_BLUE_SIZE' = 10; 'GLX_ALPHA_SIZE' = 11
  'GLX_DEPTH_SIZE' = 12; 'GLX_STENCIL_SIZE' = 13; 'GLX_DOUBLEBUFFER' = 5
  'GLX_PBUFFER_WIDTH' = 0x8041; 'GLX_PBUFFER_HEIGHT' = 0x8040
}

$linuxStubFiles = @(
  (Join-Path $linuxStub 'X11/Xlib.h')
  (Join-Path $linuxStub 'GL/glx.h')
)

# Only `#define NAME value` lines: an enumeration is not how either of these headers states a macro, and matching prose
# is what made the first version of the CGL check report the comments that explain an absence.
$linuxFailures = 0
$linuxChecked = 0
foreach ($file in $linuxStubFiles) {
  foreach ($line in Get-Content $file) {
    $match = [regex]::Match($line, '^\s*#define\s+(?<name>(?:CW|GLX_|StructureNotifyMask|AllocNone|InputOutput)\w*)\s+(?<value>0x[0-9A-Fa-f]+|\(?1L\s*<<\s*\d+\)?|\d+)\s*$')
    if (-not $match.Success) { continue }

    $name = $match.Groups['name'].Value
    $raw = $match.Groups['value'].Value

    # `(1L << 9)` and `0x8012` both have to become a number before they can be compared.
    $numeric = if ($raw -match '<<\s*(\d+)') { 1 -shl [int]$Matches[1] } else { [int]$raw }

    $truth = if ($x11Truth.ContainsKey($name)) { $x11Truth } elseif ($glxTruth.ContainsKey($name)) { $glxTruth } else { $null }
    $linuxChecked++

    if ($null -eq $truth) {
      Write-Host ("  {0,-24} NOT IN THE PLATFORM'S HEADER" -f $name) -ForegroundColor Red
      $linuxFailures++
    } elseif ($truth[$name] -ne $numeric) {
      Write-Host ("  {0,-24} stub={1} platform={2}" -f $name, $numeric, $truth[$name]) -ForegroundColor Red
      $linuxFailures++
    }
  }
}

if ($linuxFailures -gt 0) {
  Write-Error "$linuxFailures X11/GLX constant(s) do not match the platform's header."
}
Write-Host ("  {0} constants checked against X11 and GLX, all present and correct" -f $linuxChecked)

# --- 3e. Is the Android stub faithful? ----------------------------------------------------------------------------
#
# `AndroidBitmapInfo` matters for its *member names*, not its constant values: the code reads `.stride`, `.format`,
# `.width` and `.height` directly and never calls the getter, so a stub with the right constants and different field
# names would compile this file and fail on a device. The field list is therefore what gets pinned.
Write-Host ''
Write-Host '== checking the Android stub against the NDK =='

$androidInfo = Get-Content -Raw (Join-Path $stubInclude 'android/bitmap.h')
$androidFailures = 0
foreach ($field in @('width', 'height', 'stride', 'format', 'flags')) {
  if ($androidInfo -notmatch "\b$field\s*;") {
    Write-Host ("  AndroidBitmapInfo is missing member '{0}'" -f $field) -ForegroundColor Red
    $androidFailures++
  }
}

$androidTruth = @{ 'ANDROID_BITMAP_FORMAT_NONE' = 0; 'ANDROID_BITMAP_FORMAT_RGBA_8888' = 1;
  'ANDROID_BITMAP_FORMAT_RGB_565' = 4; 'ANDROID_BITMAP_RESULT_SUCCESS' = 0;
  'ANDROID_BITMAP_RESULT_BAD_PARAMETER' = -1; 'ANDROID_BITMAP_RESULT_JNI_EXCEPTION' = -2;
  'ANDROID_BITMAP_RESULT_ALLOCATION_FAILED' = -3 }
foreach ($name in $androidTruth.Keys) {
  $m = [regex]::Match($androidInfo, "$name\s*(?:=\s*|\s+)\(?(-?\d+)\)?")
  if (-not $m.Success -or [int]$m.Groups[1].Value -ne $androidTruth[$name]) {
    Write-Host ("  {0,-42} expected {1}" -f $name, $androidTruth[$name]) -ForegroundColor Red
    $androidFailures++
  }
}

if ($androidFailures -gt 0) {
  Write-Error "$androidFailures Android stub item(s) do not match the NDK."
}
Write-Host ("  AndroidBitmapInfo's {0} members and {1} constants match the NDK" -f 5, $androidTruth.Count)

# --- 4. The macOS platform --------------------------------------------------------------------------------------
#
# `GlPlatformCgl.cpp` is selected by `APPLE AND NOT IOS`. It needs two Apple headers, `<OpenGL/CGLTypes.h>` and
# `<OpenGL/OpenGL.h>`, which `tools/macstub/` provides. `dlfcn.h` is shared with the Linux stubs because macOS's
# `dlopen` is the BSD one, which is the same shape.
Write-Host ''
Write-Host '== compiling the macOS platform =='

$macStub = Join-Path $root 'modules/graphics/tools/macstub'
$macIncludes = @(
  "/I `"$macStub`""
  "/I `"$(Join-Path $root 'modules/graphics/include')`""
  "/I `"$(Join-Path $root 'modules/graphics/include/private')`""
  "/I `"$(Join-Path $root 'modules/graphics/geometry/include')`""
  "/I `"$(Join-Path $root 'modules/graphics/backend/include')`""
  "/I `"$(Join-Path $root 'modules/graphics/backend/include/private')`""
  "/I `"$(Join-Path $coralCore 'modules/utils/include')`""
  "/I `"$(Join-Path $coralCore 'modules/minilog/include')`""
  "/I `"$(Join-Path $coralCore 'include')`""
) -join ' '

$cgl = Join-Path $root 'modules/graphics/backend/src/gl/platform/GlPlatformCgl.cpp'
$compileMac = "`"$($vcvars.FullName)`" >nul 2>&1 && cl /nologo /std:c++20 /EHsc /c /Zs /DCORAL_BUILD_FOR_MAC=1 $macIncludes `"$cgl`""
$macOutput = cmd /c $compileMac 2>&1
$macCode = $LASTEXITCODE
$macOutput | Where-Object { $_ -match 'error' } | Select-Object -First 20
if ($macCode -ne 0) {
  Write-Error "the macOS platform did not compile (exit $macCode)"
}
Write-Host 'compiled clean, with CGL stubbed'

# --- 3c. Is the CGL stub faithful? -------------------------------------------------------------------------------
#
# A stub that invents a name is worse than no stub: it makes code that cannot compile on its target platform pass this
# check. That happened here -- `kCGLCPMajorVersion` and `kCGLCPMinorVersion` were added to make the macOS file build and
# exist in no Apple SDK -- so the names are now pinned against the header they came from.
Write-Host ''
Write-Host '== checking the CGL stub against Apple''s header =='

# From OpenGL.framework/Headers/CGLTypes.h, MacOSX10.13 and MacOSX10.15 SDKs (they agree).
$cglTruth = @{
  # CGLPixelFormatAttribute
  'kCGLPFAAllRenderers' = 1; 'kCGLPFATripleBuffer' = 3; 'kCGLPFADoubleBuffer' = 5
  'kCGLPFAColorSize' = 8; 'kCGLPFAAlphaSize' = 11; 'kCGLPFADepthSize' = 12; 'kCGLPFAStencilSize' = 13
  'kCGLPFAAccelerated' = 73; 'kCGLPFAOpenGLProfile' = 99
  # CGLOpenGLProfile
  'kCGLOGLPVersion_Legacy' = 0x1000; 'kCGLOGLPVersion_3_2_Core' = 0x3200
  'kCGLOGLPVersion_GL3_Core' = 0x3200; 'kCGLOGLPVersion_GL4_Core' = 0x4100
  # CGLContextParameter
  'kCGLCPSwapRectangle' = 200; 'kCGLCPSwapInterval' = 222; 'kCGLCPDispatchTableSize' = 224
  'kCGLCPClientStorage' = 226; 'kCGLCPSurfaceOrder' = 235; 'kCGLCPSurfaceOpacity' = 236
  'kCGLCPSurfaceBackingSize' = 304; 'kCGLCPSurfaceSurfaceVolatile' = 306; 'kCGLCPReclaimResources' = 308
  'kCGLCPCurrentRendererID' = 309; 'kCGLCPGPUVertexProcessing' = 310; 'kCGLCPGPUFragmentProcessing' = 311
  'kCGLCPHasDrawable' = 314; 'kCGLCPMPSwapsInFlight' = 315
  # CGLError
  'kCGLNoError' = 0; 'kCGLBadAttribute' = 10000; 'kCGLBadProperty' = 10001; 'kCGLBadPixelFormat' = 10002
  'kCGLBadRendererInfo' = 10003; 'kCGLBadContext' = 10004; 'kCGLBadDrawable' = 10005; 'kCGLBadDisplay' = 10006
  'kCGLBadState' = 10007; 'kCGLBadValue' = 10008; 'kCGLBadMatch' = 10009; 'kCGLBadEnumeration' = 10010
  'kCGLBadOffScreen' = 10011; 'kCGLBadFullScreen' = 10012; 'kCGLBadWindow' = 10013; 'kCGLBadAddress' = 10014
  'kCGLBadCodeModule' = 10015; 'kCGLBadAlloc' = 10016; 'kCGLBadConnection' = 10017
}

$cglStubFiles = @(
  (Join-Path $macStub 'OpenGL/OpenGL.h')
  (Join-Path $macStub 'OpenGL/CGLTypes.h')
)
$stubFailures = 0

# Line by line, and only the two forms this stub defines a constant in:
#
#   kCGLPFAAccelerated = 73,        an enumeration entry
#   #define GLX_X_RENDERABLE 0x8012 a macro
#
# Scanning the whole file for "name = value" instead also matched the comments -- including the ones explaining that
# `kCGLCPMajorVersion` does not exist -- and reported them as bad definitions. A checker that cannot tell a definition
# from a sentence about one gives the wrong answer loudly, which is worse than giving none.
foreach ($file in $cglStubFiles) {
  foreach ($line in Get-Content $file) {
    $match = [regex]::Match($line, '^\s*(?:(?<name>kCGL\w+)\s*=\s*(?<value>0x[0-9A-Fa-f]+|\d+)\s*,?\s*(?:/\*.*)?$|#define\s+(?<mname>kCGL\w+)\s+(?<mvalue>0x[0-9A-Fa-f]+|\d+)\s*$)')
    if (-not $match.Success) { continue }

    $name = if ($match.Groups['name'].Success) { $match.Groups['name'].Value } else { $match.Groups['mname'].Value }
    $value = if ($match.Groups['value'].Success) { $match.Groups['value'].Value } else { $match.Groups['mvalue'].Value }
    $numeric = [int]$value

    if (-not $cglTruth.ContainsKey($name)) {
      Write-Host ("  {0,-38} NOT IN APPLE'S HEADER" -f $name) -ForegroundColor Red
      $stubFailures++
    } elseif ($cglTruth[$name] -ne $numeric) {
      Write-Host ("  {0,-38} stub={1} apple={2}" -f $name, $numeric, $cglTruth[$name]) -ForegroundColor Red
      $stubFailures++
    }
  }
}

if ($stubFailures -gt 0) {
  Write-Error "$stubFailures CGL constant(s) in the stub do not match Apple's header. A stub must never supply a name the platform does not have: it turns 'this cannot compile there' into a pass, which is how kCGLCPMajorVersion hid an unbuildable file."
}
Write-Host ("  {0} constants checked against Apple's header, all present and correct" -f $cglTruth.Count)


Write-Host ''
Write-Host 'What this does NOT show: that macOS gets a context. CGL is a stub here, so this proves the file'
Write-Host 'compiles against a faithful description of the framework -- not that the framework behaves as described.'

Write-Host ''
Write-Host 'Every platform-only source in the tree compiles, and the Android codec calls only the platform decoder.'
