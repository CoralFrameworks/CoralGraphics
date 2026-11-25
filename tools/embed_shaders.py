#!/usr/bin/env python3
# Copyright (C) 2026 The Coral Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Embeds the compiled canvas shaders as a generated C++ header.

This is the portable counterpart of ``embed_shaders.ps1``. The PowerShell script
is what the Windows build has always used; a Linux host has no ``pwsh`` in a
normal installation, so the build picks this script when one is available. Both
write the same public surface: the manifest, one ``kWords`` array per shader,
``find()``/``count()``, and the separate Vulkan-environment table.

The output is written only when its bytes differ, so a no-op reconfigure does
not make every dependent object look stale.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def source_manifest(directory: Path) -> str:
    """``name:size:checksum;`` per source, in name order.

    The recipe is the one the committed fallback copy and
    ``GpuSurface.TheCommittedShaderBlobMatchesItsSources`` use: the sum of
    ``byte * (index + 1)`` modulo 2^32. It is deliberately not a 64-bit hash;
    two implementations of 64-bit wrapping in different languages disagreed
    once already.
    """
    entries = []
    for path in sorted(
        (p for p in directory.iterdir() if p.suffix in (".vert", ".frag", ".comp")),
        key=lambda p: p.name,
    ):
        data = path.read_bytes()
        checksum = 0
        for index, byte in enumerate(data):
            checksum = (checksum + byte * (index + 1)) % (1 << 32)
        entries.append(f"{path.name}:{len(data)}:{checksum}")
    return ";".join(entries)


def ident(name: str) -> str:
    return "".join(c.lower() if c.isalnum() else "_" for c in name)


def read_entries(directory: Path):
    if not directory.is_dir():
        raise SystemExit(f"embed_shaders: '{directory}' does not exist")
    files = sorted(
        (p for p in directory.iterdir() if p.suffix == ".spv"),
        key=lambda p: p.name,
    )
    if not files:
        raise SystemExit(
            f"embed_shaders: no .spv under '{directory}'. An empty set would build a renderer "
            "that reports every shader as missing, so this is an error."
        )

    entries = []
    for path in files:
        name = path.name.removesuffix(".spv")
        stage_name = Path(name).suffix
        stages = {".vert": "Vertex", ".frag": "Fragment", ".comp": "Compute"}
        if stage_name not in stages:
            raise SystemExit(
                f"embed_shaders: '{path.name}' has no stage before its .spv extension"
            )
        entries.append(
            {
                "name": name,
                "key": ident(name),
                "stage": stages[stage_name],
                "bytes": path.read_bytes(),
            }
        )
    return entries


def emit_blob(out, entry) -> None:
    data = entry["bytes"]
    if len(data) % 4 != 0:
        raise SystemExit(f"embed_shaders: '{entry['name']}.spv' is not a whole number of words")
    words = [
        int.from_bytes(data[offset : offset + 4], "little")
        for offset in range(0, len(data), 4)
    ]
    out.append(f"// {entry['name']}: {len(data)} bytes, {len(words)} SPIR-V words.")
    out.append(f"namespace {entry['key']} {{")
    out.append("inline constexpr uint32_t kWords[] = {")
    line = "  "
    for word in words:
        token = f"0x{word:08x}u,"
        if len(line) + len(token) >= 108:
            out.append(line)
            line = "  "
        line += token
    if line.strip():
        out.append(line)
    out.append("};")
    out.append(f"inline constexpr Stage kStage = Stage::{entry['stage']};")
    out.append(f'inline constexpr const char* kName = "{entry["name"]}";')
    out.append(
        "inline constexpr Blob kBlob { kWords, sizeof(kWords) / sizeof(uint32_t), kStage, kName };"
    )
    out.append(f"}} // namespace {entry['key']}")
    out.append("")


def generate(args) -> str:
    source_dir = args.source_dir
    entries = read_entries(source_dir)
    vulkan_entries = (
        read_entries(args.vulkan_source_dir) if args.vulkan_source_dir else []
    )

    manifest = ""
    if args.shader_source_dir:
        if not args.shader_source_dir.is_dir():
            raise SystemExit(
                f"embed_shaders: ShaderSourceDir '{args.shader_source_dir}' does not exist"
            )
        manifest = source_manifest(args.shader_source_dir)

    if args.committed_file and manifest and args.committed_file.is_file():
        text = args.committed_file.read_text(encoding="utf-8")
        marker = 'CORAL_CANVAS_SHADERS_SOURCE_MANIFEST "'
        start = text.find(marker)
        if start >= 0:
            start += len(marker)
            end = text.find('"', start)
            committed = text[start:end]
            # Order is the script's choice, and two languages can legitimately disagree about
            # `.` versus `_` (image.frag against image_cubic.frag). Compare the entries as a
            # sorted set, which is also what GpuSurface.TheCommittedShaderBlobMatchesItsSources
            # does at run time.
            if sorted(committed.split(";")) != sorted(manifest.split(";")):
                raise SystemExit(
                    f"embed_shaders: '{args.committed_file}' is stale.\n"
                    f"  it was generated from: {committed}\n"
                    f"  the current sources are:  {manifest}\n"
                    "Regenerate it from the compiled .spv and copy the result over the "
                    "committed file, keeping its banner."
                )

    out: list[str] = [
        "/*",
        " * Copyright (C) 2026 The Coral Project",
        " *",
        ' * Licensed under the Apache License, Version 2.0 (the "License");',
        " * you may not use this file except in compliance with the License.",
        " * You may obtain a copy of the License at",
        " *",
        " *      http://www.apache.org/licenses/LICENSE-2.0",
        " *",
        " * Unless required by applicable law or agreed to in writing, software",
        ' * distributed under the License is distributed on an "AS IS" BASIS,',
        " * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.",
        " * See the License for the specific language governing permissions and",
        " * limitations under the License.",
        " */",
        "",
        "/*",
        " * GENERATED FILE - DO NOT EDIT.",
        " *",
        " * Produced by tools/embed_shaders.py from the GLSL under shaders/.",
        " * Every shader here was compiled to SPIR-V by tools/shader_compiler at build time, so",
        " * changing a shader and rebuilding is the whole workflow; there is no run-time",
        " * compilation step and no shader text in the binary.",
        " */",
        "",
        "#ifndef CORAL_GPU_CANVAS_SHADERS_H",
        "#define CORAL_GPU_CANVAS_SHADERS_H",
        "",
    ]
    if manifest:
        out.extend(
            [
                "/**",
                " * A manifest of every shader source: `name:size:checksum;`, in name order.",
                " *",
                " * The committed fallback copy carries the same stamp, and the build compares the two: a",
                " * shader edited",
                " * without regenerating that copy is then a configure error rather than a picture that differs",
                " * on the",
                " * machines that fall back to it. `GpuSurface.TheCommittedShaderBlobMatchesItsSources` checks the same",
                " * number against the sources at run time, for a build that has no shader compiler to compare with.",
                " */",
                "#ifndef CORAL_CANVAS_SHADERS_SOURCE_MANIFEST",
                f'#define CORAL_CANVAS_SHADERS_SOURCE_MANIFEST "{manifest}"',
                "#endif",
                "",
            ]
        )
    out.extend(
        [
            "#include <cstddef>",
            "#include <cstdint>",
            "",
            "namespace graphics {",
            "namespace gpu {",
            "namespace shaders {",
            "",
            "/** Which stage a blob is for, mirroring `backend::ShaderStage` without including the backend. */",
            "enum class Stage : uint8_t { Vertex, Fragment, Compute };",
            "",
            "/** One compiled shader: a pointer into static storage and a word count, not a byte count. */",
            "struct Blob {",
            "  const uint32_t* words = nullptr;",
            "  size_t wordCount = 0;",
            "  Stage stage = Stage::Vertex;",
            "  const char* name = nullptr;",
            "",
            "  [[nodiscard]] bool isValid() const noexcept { return words != nullptr && wordCount > 0; }",
            "};",
            "",
        ]
    )
    for entry in entries:
        emit_blob(out, entry)

    out.extend(
        [
            "/**",
            " * @return true when @p a and @p b are the same text, which avoids a `<cstring>` include here.",
            " */",
            "[[nodiscard]] inline bool compare(const char* a, const char* b) noexcept {",
            "  size_t index = 0;",
            "  while (a[index] != '\\0' && a[index] == b[index]) ++index;",
            "  return a[index] == b[index];",
            "}",
            "",
            "/**",
            " * @return the blob a shader was embedded under, or an invalid one when the name is unknown.",
            " *",
            " * Looked up by the *source* file name -- `quad.vert`, not `quad.vert.spv` -- so that a caller names",
            " * the shader the way the source tree does and a rename is one edit. A missing name is not fatal",
            " * here: it reports an invalid blob, and the caller decides whether that is a fallback or a failure.",
            " */",
            "[[nodiscard]] inline Blob find(const char* name) noexcept {",
            "  if (name == nullptr) return Blob {};",
        ]
    )
    for entry in entries:
        out.append(f'  if (compare(name, "{entry["name"]}")) return {entry["key"]}::kBlob;')
    out.extend(
        [
            "  return Blob {};",
            "}",
            "",
            "/** @return how many shaders are embedded, which a diagnostic reports. */",
            f"[[nodiscard]] inline size_t count() noexcept {{ return {len(entries)}; }}",
            "",
        ]
    )

    if vulkan_entries:
        out.extend(
            [
                "/**",
                " * The same shaders compiled for the **Vulkan** environment.",
                " *",
                " * A separate table rather than a replacement, because the two modules differ in a way that",
                " * matters: only this one carries the `DescriptorSet`/`Binding` decorations a",
                " * `VkDescriptorSetLayout` is built from. The OpenGL backend consumes the set above through",
                " * SPIRV-Cross, which neither needs nor understands those decorations.",
                " */",
                "namespace vulkan {",
                "",
            ]
        )
        for entry in vulkan_entries:
            emit_blob(out, entry)
        out.extend(
            [
                "/** @return the Vulkan-environment blob for @p name, or an invalid one when it is not embedded. */",
                "[[nodiscard]] inline Blob find(const char* name) noexcept {",
                "  if (name == nullptr) return Blob {};",
            ]
        )
        for entry in vulkan_entries:
            out.append(f'  if (compare(name, "{entry["name"]}")) return {entry["key"]}::kBlob;')
        out.extend(
            [
                "  return Blob {};",
                "}",
                "",
                f"[[nodiscard]] inline size_t count() noexcept {{ return {len(vulkan_entries)}; }}",
                "",
                "} // namespace vulkan",
                "",
            ]
        )
    else:
        # `GpuRenderer` names `shaders::vulkan` unconditionally, so a build without the Vulkan
        # backend still needs the namespace. An empty table is the correct answer there.
        out.extend(
            [
                "namespace vulkan {",
                "",
                "[[nodiscard]] inline Blob find(const char*) noexcept { return Blob {}; }",
                "[[nodiscard]] inline size_t count() noexcept { return 0; }",
                "",
                "} // namespace vulkan",
                "",
            ]
        )

    out.extend(
        [
            "} // namespace shaders",
            "} // namespace gpu",
            "} // namespace graphics",
            "",
            "#endif // CORAL_GPU_CANVAS_SHADERS_H",
            "",
        ]
    )
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--vulkan-source-dir", type=Path, default=None)
    parser.add_argument("--shader-source-dir", type=Path, default=None)
    parser.add_argument("--committed-file", type=Path, default=None)
    args = parser.parse_args()

    content = generate(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    encoded = content.encode("utf-8")
    if args.output.is_file() and args.output.read_bytes() == encoded:
        print(f"embed_shaders: {args.output} is unchanged")
        return 0
    args.output.write_bytes(encoded)
    print(f"embed_shaders: wrote {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
