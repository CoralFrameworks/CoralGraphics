#!/usr/bin/env pwsh
#
# Checks that the two translated modules are still self-contained.
#
#   gcms/     a translation of skcms   -- standard library only
#   pathops/  a reworking of pathkit   -- standard library and `geometry`
#
# See the note at the top of this file for why this is worth a script rather than a sentence in the README.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$graphics = $root

$modules = @(
  @{
    Name = 'gcms'
    Dirs = @((Join-Path $graphics 'src/gcms'), (Join-Path $graphics 'include/private/gcms'))
    Allowed = @()
    Upstream = 'skcms'
  }
  @{
    Name = 'pathops'
    Dirs = @((Join-Path $graphics 'src/pathops'), (Join-Path $graphics 'include/private/pathops'))
    Allowed = @('geometry/')
    Upstream = 'pathkit'
  }
)

# Layers a translated module must not reach for. `geometry` is absent on purpose: requirement 1 asks for it by name.
$layers = @('graphics/', 'private/', 'backend/', 'gpu/', 'codec/', 'font/', 'raster/', 'utils/', 'minilog/')

# Two categories, because they mean different things.
#
# `skcms_` is the upstream project's *API*, and this module is that API's replacement -- so the prefix has no business
# appearing anywhere, comments included.
$upstreamApi = @('skcms_\w+')

# An `Sk`-prefixed type name is upstream *vocabulary*, and a comment reading "`SkDLine` became `Line`" is the most useful
# sentence in the file. Only a use counts, so these are matched against the code with comments stripped.
$upstreamTypes = @('Sk[A-Z]\w*', 'skpathops|SkPathOps')

$failures = 0

function Get-CodeOnlyText {
  param([string[]] $Lines, [ref] $InComment)

  # Returns the lines with the contents of block comments blanked, so a use can be told from a mention. The state has to
  # carry across lines: `/**` at the top of a doc block and `*/` twenty lines later enclose prose, not code.
  $out = @()
  foreach ($line in $Lines) {
    $text = $line
    $result = ''
    $i = 0
    while ($i -lt $text.Length) {
      if ($InComment.Value) {
        $end = $text.IndexOf('*/', $i)
        if ($end -lt 0) { $i = $text.Length; continue }
        $InComment.Value = $false
        $i = $end + 2
        continue
      }
      $block = $text.IndexOf('/*', $i)
      $lineComment = $text.IndexOf('//', $i)

      if ($lineComment -ge 0 -and ($block -lt 0 -or $lineComment -lt $block)) {
        $result += $text.Substring($i, $lineComment - $i)
        $i = $text.Length
        continue
      }
      if ($block -ge 0) {
        $result += $text.Substring($i, $block - $i) + ' '
        $InComment.Value = $true
        $i = $block + 2
        continue
      }
      $result += $text.Substring($i)
      $i = $text.Length
    }
    $out += $result
  }
  return $out
}

foreach ($module in $modules) {
  Write-Host ''
  Write-Host "== $($module.Name) (translated from $($module.Upstream)) =="

  $files = @()
  foreach ($dir in $module.Dirs) {
    if (Test-Path $dir) { $files += Get-ChildItem $dir -Recurse -File -Include *.h,*.cpp }
  }
  Write-Host ("  {0} files" -f $files.Count)

  $moduleProblems = 0

  foreach ($file in $files) {
    $lines = Get-Content $file.FullName
    $inComment = $false
    $code = Get-CodeOnlyText -Lines $lines -InComment ([ref] $inComment)
    $short = $file.FullName.Replace("$graphics\", '')

    for ($i = 0; $i -lt $lines.Count; $i++) {
      # --- the include graph -----------------------------------------------------------------------------------
      $m = [regex]::Match($lines[$i], '^\s*#include\s+[<"]([^">]+)[">]')
      if ($m.Success) {
        $target = $m.Groups[1].Value
        $isOwn = $target.StartsWith("$($module.Name)/", [System.StringComparison]::Ordinal)
        $isStd = -not $target.Contains('/')
        $isAllowed = $false
        foreach ($prefix in $module.Allowed) {
          if ($target.StartsWith($prefix, [System.StringComparison]::Ordinal)) { $isAllowed = $true }
        }
        if (-not ($isOwn -or $isStd -or $isAllowed)) {
          Write-Host ("  {0}:{1} includes '{2}', which is outside the module" -f $short, ($i + 1), $target) -ForegroundColor Red
          $moduleProblems++
        }
      }

      # --- the upstream API, which may not appear even in a comment ----------------------------------------------
      foreach ($pattern in $upstreamApi) {
        if ($lines[$i] -cmatch $pattern) {
          Write-Host ("  {0}:{1} names upstream API '{2}'" -f $short, ($i + 1), $Matches[0]) -ForegroundColor Red
          $moduleProblems++
        }
      }

      # --- upstream type names, which may appear in prose but not in code ----------------------------------------
      foreach ($pattern in $upstreamTypes) {
        # `-cmatch`, not `-match`: PowerShell compares case-insensitively by default, which made `Sk[A-Z]` match the
        # English words "asking" and "asked" -- six false positives in a file whose only sin was discussing what a
        # caller is asking for.
        if ($code[$i] -cmatch $pattern) {
          Write-Host ("  {0}:{1} names upstream type '{2}' in code" -f $short, ($i + 1), $Matches[0]) -ForegroundColor Red
          $moduleProblems++
        }
      }

      # --- the upstream project's name in code, which is the case a comment is allowed to mention ---------------
      if ($code[$i] -cmatch "(?<![A-Za-z0-9_])$($module.Upstream)(?![A-Za-z0-9_])") {
        Write-Host ("  {0}:{1} names '{2}' in code rather than in a comment" -f $short, ($i + 1), $module.Upstream) -ForegroundColor Red
        $moduleProblems++
      }
    }
  }

  # --- the layer above -----------------------------------------------------------------------------------------
  $reach = Select-String -Path ($files.FullName) -Pattern ('^\s*#include\s+[<"](private/|' + ($layers -join '|') + ')') -ErrorAction SilentlyContinue
  if ($reach) {
    foreach ($r in $reach) {
      Write-Host ("  {0}:{1} reaches for '{2}'" -f $r.Path.Replace("$graphics\", ''), $r.LineNumber, $r.Line.Trim()) -ForegroundColor Red
      $moduleProblems++
    }
  }

  if ($moduleProblems -eq 0) {
    Write-Host '  standard library only, no upstream identifier, no dependency on a layer above'
  }
  $failures += $moduleProblems
}

Write-Host ''
if ($failures -gt 0) {
  Write-Error "$failures module-isolation problem(s)"
}
Write-Host 'Both translated modules are self-contained.'
