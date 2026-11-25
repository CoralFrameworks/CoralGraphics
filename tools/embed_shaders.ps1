<#
  Copyright (C) 2026 The Coral Project

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  embeds the canvas shader set, as SPIR-V, into a C++ translation unit.

  ## Why a generated C++ file rather than a data directory

  A renderer that loads its shaders by path has to be told where they are, at run time, on every
  platform, including the ones where "a path" means something inside an APK. The alternative -- an
  Android asset manager, an iOS bundle lookup, a Windows relative-to-the-executable guess -- is three
  platform layers whose only job is to answer a question this build already knows the answer to. A
  byte array has no answer to give and no failure mode: if the shader is not in the binary, the
  program does not link.

  The cost is that a shader change means a rebuild, which is what the build rule already arranges.

  ## Why the offsets are `uint32_t` counts

  The RHI's canonical shader input is SPIR-V *words*, and `Device::createShaderModule` takes a word
  count rather than a byte size. Emitting a word count here means no caller has to remember to divide
  by four, which is the kind of arithmetic that is right until somebody adds a header to the blob.
#>

param(
  [Parameter(Mandatory = $true)][string]$Output,
  # The directory the `.spv` were compiled into, rather than a list of their paths. CMake passes a list as
  # separate command-line tokens and PowerShell binds only the first of them to an array parameter, so a
  # list cannot be handed over without a joining trick that turned out to concatenate the paths instead.
  # A directory is one argument, needs no trick, and is the shape a caller would want anyway.
  [Parameter(Mandatory = $true)][string]$SourceDir,
  # The Vulkan-environment set, when the build compiles one. It lands in `shaders::vulkan` rather than
  # replacing the OpenGL set: the two are different modules from the same source, and the OpenGL backend
  # must keep the module it has always been given.
  [string]$VulkanSourceDir = "",
  # Where the shader sources are, when the caller wants the generated header stamped with a fingerprint of them.
  # The committed fallback copy (`src/gpu/CanvasShadersEmbedded.h`) is checked against that fingerprint, so the
  # stamp is what makes "regenerate after editing a shader" a build error rather than a picture that differs.
  [string]$ShaderSourceDir = "",
  # The committed fallback copy, when the caller wants it validated. This is the build that can ask the question -- it just
  # computed the stamp of the sources -- and a stale copy is otherwise discovered only by the machines that fall back to it,
  # as a picture that differs for no visible reason.
  [string]$CommittedFile = ""
)

if (-not (Test-Path $SourceDir)) {
  throw "embed_shaders: SourceDir '$SourceDir' does not exist. Nothing was compiled, so nothing can be embedded."
}

$inputFiles = @(Get-ChildItem -Path $SourceDir -Filter '*.spv' -File)
[Array]::Sort($inputFiles, [System.Comparison[System.IO.FileInfo]] { param($left, $right) [string]::CompareOrdinal($left.Name, $right.Name) })
$inputPaths = @($inputFiles | Select-Object -ExpandProperty FullName)
if ($inputPaths.Count -eq 0) {
  throw "embed_shaders: no .spv under '$SourceDir'. An empty shader set would build a renderer that reports every lookup as missing, so this is an error rather than a warning."
}

$ErrorActionPreference = "Stop"

# The fingerprint of the shader *sources*, for the committed fallback copy.
#
# FNV-1a 64 over every source's name and bytes in name order -- the same recipe
# `GpuSurface.TheCommittedShaderBlobMatchesItsSources` recomputes in C++. It is computed with `BigInteger` and
# reduced modulo 2^64 because PowerShell's `[int64]` multiplication wraps or promotes to `Decimal` and throws:
# the first version of this produced the FNV offset basis unchanged (nothing was mixed in) and recorded a
# fingerprint that matched no source at all, which is worse than no fingerprint.
function Get-ShaderSourceManifest([string]$directory) {
  # A text manifest instead of a 64-bit hash: `name:size:checksum;` per shader, in name order, where the checksum is the
  # sum of `byte * (index + 1)` modulo 2^32.
  #
  # The first version of this used FNV-1a 64 and the C++ side recomputed it -- and the two disagreed. Two implementations
  # of 64-bit arithmetic, in a language that overflows into `Decimal` and one that wraps silently, is a fingerprint that
  # reports a difference when there is none. This recipe is a handful of operations that cannot overflow differently, and
  # when it does differ the manifest names the file that changed.
  $parts = @()
  $files = @(Get-ChildItem -Path $directory -File |
      Where-Object { $_.Extension -in '.vert', '.frag', '.comp' })
  # Ordinal, not `Sort-Object Name`: cultural collation ranks `.` against `_` differently from the byte comparison the C++
  # guard does, so the two manifests would differ in order and the strings would not be equal.
  [Array]::Sort($files, [System.Comparison[System.IO.FileInfo]] { param($left, $right) [string]::CompareOrdinal($left.Name, $right.Name) })
  foreach ($file in $files) {
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    [uint64]$sum = 0
    for ($index = 0; $index -lt $bytes.Length; $index++) {
      $sum = ($sum + ([uint64]$bytes[$index] * [uint64]($index + 1))) % 4294967296
    }
    $parts += ("{0}:{1}:{2}" -f $file.Name, $bytes.Length, $sum)
  }
  return ($parts -join ';')
}
$manifest = ""
if ($ShaderSourceDir -ne "") {
  if (-not (Test-Path $ShaderSourceDir)) {
    throw "embed_shaders: ShaderSourceDir '$ShaderSourceDir' does not exist, so the header cannot be stamped."
  }
  $manifest = Get-ShaderSourceManifest $ShaderSourceDir
}

# The committed copy must have been generated from these sources. Checked here rather than in CMake because this is where
# the stamp is known, and rather than only in a test because a build *with* a shader compiler is the one that can answer
# the question at all -- a build without one has nothing to regenerate and no way to compare.
if ($CommittedFile -ne "" -and $manifest -ne "" -and (Test-Path $CommittedFile)) {
  $committedText = [System.IO.File]::ReadAllText($CommittedFile)
  if ($committedText -match 'CORAL_CANVAS_SHADERS_SOURCE_MANIFEST "([^"]+)"') {
    $committedManifest = $Matches[1]
    if ($committedManifest -ne $manifest) {
      throw ("embed_shaders: '$CommittedFile' is stale.`n" +
        "  it was generated from: $committedManifest`n" +
        "  the current sources are:  $manifest`n" +
        "Regenerate it: embed the compiled .spv with this script and copy the result over the committed file, keeping " +
        "its banner. The no-shader-compiler build uses that copy, so a stale one is a picture that differs.")
    }
  }
}

function Convert-ToIdent([string]$name) {
  $builder = New-Object System.Text.StringBuilder
  foreach ($character in $name.ToCharArray()) {
    if ([char]::IsLetterOrDigit($character)) {
      [void]$builder.Append([char]::ToLowerInvariant($character))
    }
    else {
      [void]$builder.Append('_')
    }
  }
  return $builder.ToString()
}

# One entry per shader, in name order so that the generated file is stable across runs: a directory listing
# that reordered itself would rewrite the file every configure and make every shader change look like a
# change to all of them.
function Get-ShaderEntries([string[]]$paths) {
  $entries = @()
  foreach ($input in $paths) {
    $file = Get-Item $input
    if (-not $file.Exists) {
      throw "embed_shaders: '$input' does not exist. A shader compiled from a stale rule would be embedded as an empty array, which fails only at draw time."
    }

    # The extension *before* `.spv`, because the build names a compiled shader `quad.vert.spv` -- the stage
    # stays on the front so that `quad.vert` and `quad.frag` do not both become `quad.spv`. Reading the last
    # extension here would see `.spv` for every file and reject all of them.
    $sourceExtension = [System.IO.Path]::GetExtension([System.IO.Path]::GetFileNameWithoutExtension($file.Name))
    $stage = switch ($sourceExtension) {
      ".vert" { "Vertex" }
      ".frag" { "Fragment" }
      ".comp" { "Compute" }
      default { throw "embed_shaders: '$($file.Name)' has no stage before its .spv extension." }
    }

    # `quad.vert.spv` -> `quad_vert`, which is the key the registry below uses and the identifier namespace
    # the blob's declarations live in. The stage stays in the name for the same reason it stays in the file's.
    $base = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)
    $entries += [pscustomobject]@{
      Name     = $base
      Stage    = $stage
      Key      = Convert-ToIdent $base
      FullPath = $file.FullName
      Bytes    = [System.IO.File]::ReadAllBytes($file.FullName)
    }
  }
  return ,$entries
}

$entries = Get-ShaderEntries $inputPaths

$vulkanEntries = @()
if ($VulkanSourceDir -ne "") {
  if (-not (Test-Path $VulkanSourceDir)) {
    throw "embed_shaders: VulkanSourceDir '$VulkanSourceDir' does not exist."
  }
  $vulkanFiles = @(Get-ChildItem -Path $VulkanSourceDir -Filter '*.spv' -File)
  [Array]::Sort($vulkanFiles, [System.Comparison[System.IO.FileInfo]] { param($left, $right) [string]::CompareOrdinal($left.Name, $right.Name) })
  if ($vulkanFiles.Count -eq 0) {
    throw "embed_shaders: no .spv under '$VulkanSourceDir', but a Vulkan shader set was requested. A build that embeds nothing would draw nothing."
  }
  $vulkanEntries = Get-ShaderEntries @($vulkanFiles | Select-Object -ExpandProperty FullName)
}

$content = New-Object System.Text.StringBuilder
[void]$content.AppendLine("/*")
[void]$content.AppendLine(" * Copyright (C) 2026 The Coral Project")
[void]$content.AppendLine(" *")
[void]$content.AppendLine(" * Licensed under the Apache License, Version 2.0 (the `"License`");")
[void]$content.AppendLine(" * you may not use this file except in compliance with the License.")
[void]$content.AppendLine(" * You may obtain a copy of the License at")
[void]$content.AppendLine(" *")
[void]$content.AppendLine(" *      http://www.apache.org/licenses/LICENSE-2.0")
[void]$content.AppendLine(" *")
[void]$content.AppendLine(" * Unless required by applicable law or agreed to in writing, software")
[void]$content.AppendLine(" * distributed under the License is distributed on an `"AS IS`" BASIS,")
[void]$content.AppendLine(" * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.")
[void]$content.AppendLine(" * See the License for the specific language governing permissions and")
[void]$content.AppendLine(" * limitations under the License.")
[void]$content.AppendLine(" */")
[void]$content.AppendLine("")
[void]$content.AppendLine("/*")
[void]$content.AppendLine(" * GENERATED FILE - DO NOT EDIT.")
[void]$content.AppendLine(" *")
[void]$content.AppendLine(" * Produced by tools/embed_shaders.ps1 from the GLSL under shaders/.")
[void]$content.AppendLine(" * Every shader here was compiled to SPIR-V by tools/shader_compiler at build time, so")
[void]$content.AppendLine(" * changing a shader and rebuilding is the whole workflow; there is no run-time")
[void]$content.AppendLine(" * compilation step and no shader text in the binary.")
[void]$content.AppendLine(" */")
[void]$content.AppendLine("")
[void]$content.AppendLine("#ifndef CORAL_GPU_CANVAS_SHADERS_H")
[void]$content.AppendLine("#define CORAL_GPU_CANVAS_SHADERS_H")
[void]$content.AppendLine("")
if ($manifest -ne "") {
  [void]$content.AppendLine("/**")
  [void]$content.AppendLine(" * A manifest of every shader source: `name:size:checksum;`, in name order.")
  [void]$content.AppendLine(" *")
  [void]$content.AppendLine(" * The committed fallback copy carries the same stamp, and the build compares the two: a shader edited")
  [void]$content.AppendLine(" * without regenerating that copy is then a configure error rather than a picture that differs on the")
  [void]$content.AppendLine(" * machines that fall back to it. `GpuSurface.TheCommittedShaderBlobMatchesItsSources` checks the same")
  [void]$content.AppendLine(" * number against the sources at run time, for a build that has no shader compiler to compare with.")
  [void]$content.AppendLine(" */")
  [void]$content.AppendLine("#ifndef CORAL_CANVAS_SHADERS_SOURCE_MANIFEST")
  [void]$content.AppendLine("#define CORAL_CANVAS_SHADERS_SOURCE_MANIFEST `"$manifest`"")
  [void]$content.AppendLine("#endif")
  [void]$content.AppendLine("")
}
[void]$content.AppendLine("#include <cstddef>")
[void]$content.AppendLine("#include <cstdint>")
[void]$content.AppendLine("")
[void]$content.AppendLine("namespace graphics {")
[void]$content.AppendLine("namespace gpu {")
[void]$content.AppendLine("namespace shaders {")
[void]$content.AppendLine("")
[void]$content.AppendLine("/** Which stage a blob is for, mirroring `backend::ShaderStage` without including the backend. */")
[void]$content.AppendLine("enum class Stage : uint8_t { Vertex, Fragment, Compute };")
[void]$content.AppendLine("")
[void]$content.AppendLine("/** One compiled shader: a pointer into static storage and a word count, not a byte count. */")
[void]$content.AppendLine("struct Blob {")
[void]$content.AppendLine("  const uint32_t* words = nullptr;")
[void]$content.AppendLine("  size_t wordCount = 0;")
[void]$content.AppendLine("  Stage stage = Stage::Vertex;")
[void]$content.AppendLine("  const char* name = nullptr;")
[void]$content.AppendLine("")
[void]$content.AppendLine("  [[nodiscard]] bool isValid() const noexcept { return words != nullptr && wordCount > 0; }")
[void]$content.AppendLine("};")
[void]$content.AppendLine("")

foreach ($entry in $entries) {
  $bytes = $entry.Bytes
  $wordCount = [int]($bytes.Length / 4)
  [void]$content.AppendLine("// $($entry.Name): $($bytes.Length) bytes, $wordCount SPIR-V words.")
  [void]$content.AppendLine("namespace $($entry.Key) {")
  [void]$content.AppendLine("inline constexpr uint32_t kWords[] = {")
  $line = "  "
  for ($index = 0; $index -lt $wordCount; $index++) {
    $offset = $index * 4
    $word = [uint32]$bytes[$offset] -bor ([uint32]$bytes[$offset + 1] -shl 8) -bor ([uint32]$bytes[$offset + 2] -shl 16) -bor ([uint32]$bytes[$offset + 3] -shl 24)
    $line += ("0x{0:x8}u," -f $word)
    if ($line.Length -ge 108) {
      [void]$content.AppendLine($line)
      $line = "  "
    }
  }
  if ($line.Trim().Length -gt 0) { [void]$content.AppendLine($line) }
  [void]$content.AppendLine("};")
  [void]$content.AppendLine("inline constexpr Stage kStage = Stage::$($entry.Stage);")
  [void]$content.AppendLine("inline constexpr const char* kName = `"$($entry.Name)`";")
  [void]$content.AppendLine("inline constexpr Blob kBlob { kWords, sizeof(kWords) / sizeof(uint32_t), kStage, kName };")
  [void]$content.AppendLine("} // namespace $($entry.Key)")
  [void]$content.AppendLine("")
}

[void]$content.AppendLine("/**")
[void]$content.AppendLine(" * @return true when @p a and @p b are the same text, which avoids a `<cstring>` include here.")
[void]$content.AppendLine(" *")
[void]$content.AppendLine(" * Emitted *before* `find` rather than after it. A header whose first function calls a second one")
[void]$content.AppendLine(" * declared further down does not compile, and the diagnostic names the callee rather than the")
[void]$content.AppendLine(" * ordering -- so the order is the fix, not a forward declaration in every includer.")
[void]$content.AppendLine(" */")
[void]$content.AppendLine("[[nodiscard]] inline bool compare(const char* a, const char* b) noexcept {")
[void]$content.AppendLine("  size_t index = 0;")
# The terminator is the two-character sequence backslash-zero *in the emitted source*, which is what a C++
# character literal needs. PowerShell's double-quoted string interpolates, so the backslash is written twice
# here to survive into the file: a single one produced the characters `\` and `0` as a *multi-character
# literal*, whose value is implementation-defined and is never a real NUL -- so the loop ran off the end of
# every string and no shader name resolved, while `count()` correctly reported seven.
[void]$content.AppendLine("  while (a[index] != '\0' && a[index] == b[index]) ++index;")
[void]$content.AppendLine("  return a[index] == b[index];")
[void]$content.AppendLine("}")
[void]$content.AppendLine("")
[void]$content.AppendLine("/**")
[void]$content.AppendLine(" * @return the blob a shader was embedded under, or an invalid one when the name is unknown.")
[void]$content.AppendLine(" *")
[void]$content.AppendLine(" * Looked up by the *source* file name -- `quad.vert`, not `quad.vert.spv` -- so that a caller names")
[void]$content.AppendLine(" * the shader the way the source tree does and a rename is one edit. A missing name is not fatal")
[void]$content.AppendLine(" * here: it reports an invalid blob, and the caller decides whether that is a fallback or a failure.")
[void]$content.AppendLine(" * Silently returning an empty shader would not be.")
[void]$content.AppendLine(" */")
[void]$content.AppendLine("[[nodiscard]] inline Blob find(const char* name) noexcept {")
[void]$content.AppendLine("  if (name == nullptr) return Blob {};")
foreach ($entry in $entries) {
  [void]$content.AppendLine("  if (compare(name, `"$($entry.Name)`")) return $($entry.Key)::kBlob;")
}
[void]$content.AppendLine("  return Blob {};")
[void]$content.AppendLine("}")
[void]$content.AppendLine("")
[void]$content.AppendLine("/** @return how many shaders are embedded, which a diagnostic reports. */")
[void]$content.AppendLine("[[nodiscard]] inline size_t count() noexcept { return $($entries.Count); }")
[void]$content.AppendLine("")

if ($vulkanEntries.Count -gt 0) {
  [void]$content.AppendLine("/**")
  [void]$content.AppendLine(" * The same shaders compiled for the **Vulkan** environment.")
  [void]$content.AppendLine(" *")
  [void]$content.AppendLine(" * A separate table rather than a replacement, because the two modules differ in a way that")
  [void]$content.AppendLine(" * matters: only this one carries the `DescriptorSet`/`Binding` decorations a")
  [void]$content.AppendLine(" * `VkDescriptorSetLayout` is built from. The OpenGL backend consumes the set above through")
  [void]$content.AppendLine(" * SPIRV-Cross, which neither needs nor understands those decorations.")
  [void]$content.AppendLine(" */")
  [void]$content.AppendLine("namespace vulkan {")
  [void]$content.AppendLine("")
  foreach ($entry in $vulkanEntries) {
    $bytes = $entry.Bytes
    $wordCount = [int]($bytes.Length / 4)
    [void]$content.AppendLine("// $($entry.Name): $($bytes.Length) bytes, $wordCount SPIR-V words.")
    [void]$content.AppendLine("namespace $($entry.Key) {")
    [void]$content.AppendLine("inline constexpr uint32_t kWords[] = {")
    $line = "  "
    for ($index = 0; $index -lt $wordCount; $index++) {
      $offset = $index * 4
      $word = [uint32]$bytes[$offset] -bor ([uint32]$bytes[$offset + 1] -shl 8) -bor ([uint32]$bytes[$offset + 2] -shl 16) -bor ([uint32]$bytes[$offset + 3] -shl 24)
      $line += ("0x{0:x8}u," -f $word)
      if ($line.Length -ge 108) {
        [void]$content.AppendLine($line)
        $line = "  "
      }
    }
    if ($line.Trim().Length -gt 0) { [void]$content.AppendLine($line) }
    [void]$content.AppendLine("};")
    [void]$content.AppendLine("inline constexpr Stage kStage = Stage::$($entry.Stage);")
    [void]$content.AppendLine("inline constexpr const char* kName = `"$($entry.Name)`";")
    [void]$content.AppendLine("inline constexpr Blob kBlob { kWords, sizeof(kWords) / sizeof(uint32_t), kStage, kName };")
    [void]$content.AppendLine("} // namespace $($entry.Key)")
    [void]$content.AppendLine("")
  }
  [void]$content.AppendLine("/** @return the Vulkan-environment blob for @p name, or an invalid one when it is not embedded. */")
  [void]$content.AppendLine("[[nodiscard]] inline Blob find(const char* name) noexcept {")
  [void]$content.AppendLine("  if (name == nullptr) return Blob {};")
  foreach ($entry in $vulkanEntries) {
    [void]$content.AppendLine("  if (compare(name, `"$($entry.Name)`")) return $($entry.Key)::kBlob;")
  }
  [void]$content.AppendLine("  return Blob {};")
  [void]$content.AppendLine("}")
  [void]$content.AppendLine("")
  [void]$content.AppendLine("[[nodiscard]] inline size_t count() noexcept { return $($vulkanEntries.Count); }")
  [void]$content.AppendLine("")
  [void]$content.AppendLine("} // namespace vulkan")
  [void]$content.AppendLine("")
} else {
  # `GpuRenderer` names `shaders::vulkan` unconditionally, so a build without the Vulkan backend
  # still needs the namespace; an empty table is the correct answer.
  [void]$content.AppendLine("namespace vulkan {")
  [void]$content.AppendLine("")
  [void]$content.AppendLine("[[nodiscard]] inline Blob find(const char*) noexcept { return Blob {}; }")
  [void]$content.AppendLine("[[nodiscard]] inline size_t count() noexcept { return 0; }")
  [void]$content.AppendLine("")
  [void]$content.AppendLine("} // namespace vulkan")
  [void]$content.AppendLine("")
}
[void]$content.AppendLine("} // namespace shaders")
[void]$content.AppendLine("} // namespace gpu")
[void]$content.AppendLine("} // namespace graphics")
[void]$content.AppendLine("")
[void]$content.AppendLine("#endif // CORAL_GPU_CANVAS_SHADERS_H")

$directory = Split-Path -Parent $Output
if ($directory -and -not (Test-Path $directory)) {
  New-Item -ItemType Directory -Path $directory -Force | Out-Null
}

# Written only when the content differs, so that a no-op configure does not touch the file and make
# every downstream object look stale.
$newContent = $content.ToString()
if ((Test-Path $Output) -and ([System.IO.File]::ReadAllText($Output) -eq $newContent)) {
  Write-Host "embed_shaders: $Output is unchanged ($($entries.Count) shader(s))"
  exit 0
}

# UTF-8 without a BOM: MSVC decodes a BOM-less file as UTF-8 only when told to, which the target does
# with /utf-8, and a BOM would make the generated header differ from every hand-written one.
[System.IO.File]::WriteAllText($Output, $newContent, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "embed_shaders: wrote $Output ($($entries.Count) shader(s))"
