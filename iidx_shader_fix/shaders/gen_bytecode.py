#!/usr/bin/env python3
"""Extract IIDX AFP HLSL from bm2dx.dll, compile with fxc, emit shaders_bytecode.h.

Runtime match key: (src_len, profile, entry) when Wine D3DXCompileShader fails.
vs.1.1 / ps.1.1 come from D3DXAssembleShader sources in the same DLL; their
prebuilt CSOs are produced via d3dx9_43 (asm_blob.cpp) and embedded here.
"""
from __future__ import annotations

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FXC = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
OUT_H = os.path.join(ROOT, "shaders_bytecode.h")

BM2DX = os.path.join(ROOT, "bm2dx.dll")

# (name, src_len, profile, entry, marker, must_all, must_not)
EXTRACT = [
    (
        "iidx_mrt_ps",
        757,
        "ps_2_b",
        "main",
        b"targetColor1 = psOut.targetColor0",
        (b"sampler0",),
        (b"0.2275f",),
    ),
    (
        "iidx_mrt_zero_ps",
        764,
        "ps_2_b",
        "main",
        b"targetColor1 = float4(0.0f,0.0f,0.0f,0.0f)",
        (b"sampler0",),
        (),
    ),
    (
        "iidx_mrt_tint_ps",
        791,
        "ps_2_b",
        "main",
        b"0.2275f,0.7412f",
        (b"targetColor1",),
        (),
    ),
    (
        "iidx_afp_hsv_mrt_ps",
        3236,
        "ps_2_b",
        "main",
        b"shift_color",
        (b"targetColor1",),
        (),
    ),
    (
        "iidx_afp_hsv_6172_ps",
        6172,
        "ps_2_b",
        "HSVPixelShader",
        b"HSVPixelShader",
        (b"func_rgb_to_hls",),
        (),
    ),
    (
        "afp_gray_ps",
        870,
        "ps_2_b",
        "RenderGrayPixelShader",
        b"RenderGrayPixelShader",
        (b"RGB2Y",),
        (),
    ),
    (
        "afp_sepia_ps",
        1170,
        "ps_2_b",
        "RenderSepiaPixelShader",
        b"RenderSepiaPixelShader",
        (b"YCbCr",),
        (),
    ),
    (
        "afp_raster_ps",
        1265,
        "ps_2_b",
        "RasterScrollShader",
        b"RasterScrollShader",
        (b"g_amplitude",),
        (),
    ),
    (
        "afp_alphablur_ps",
        1361,
        "ps_2_b",
        "AlphaBlurShader",
        b"AlphaBlurShader",
        (b"g_offset",),
        (),
    ),
]

# Assemble sources (not fxc). CSOs produced by asm_blob.cpp / d3dx9_43.
ASM = [
    ("iidx_vs11", 164, b"vs.1.1", 88),
    ("iidx_ps11", 144, b"ps.1.1", 76),
]


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def iter_cstrings(data: bytes, marker: bytes):
    start = 0
    while True:
        i = data.find(marker, start)
        if i < 0:
            return
        b = i
        while b > 0 and data[b - 1] != 0:
            b -= 1
        e = i
        while e < len(data) and data[e] != 0:
            e += 1
        yield data[b:e]
        start = i + 1


def find_shader(
    data: bytes,
    marker: bytes,
    src_len: int,
    must_all: tuple[bytes, ...] = (),
    must_not: tuple[bytes, ...] = (),
) -> bytes:
    for raw in iter_cstrings(data, marker):
        if len(raw) != src_len:
            continue
        if any(m not in raw for m in must_all):
            continue
        if any(m in raw for m in must_not):
            continue
        return raw
    raise ValueError(
        f"no match len={src_len} marker={marker!r} must={must_all!r} not={must_not!r}"
    )


def compile_one(name: str, src: bytes, profile: str, entry: str, src_len: int):
    hlsl_path = os.path.join(HERE, name + ".hlsl")
    cso_path = os.path.join(HERE, name + ".cso")
    key = fnv1a64(src + b"\0" + entry.encode() + b"\0" + profile.encode())
    compile_src = src  # stock game HLSL; diagnostics live in the DLL hook
    with open(hlsl_path, "wb") as f:
        f.write(compile_src)
    cmd = [FXC, "/nologo", "/T", profile, "/E", entry, "/Fo", cso_path, hlsl_path]
    print(" ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        raise SystemExit(r.returncode)
    with open(cso_path, "rb") as f:
        cso = f.read()
    print(f"  {name}: match_src_len={src_len} cso={len(cso)}")
    return (name, profile, entry, src_len, key, cso)


def emit_bytes_array(lines: list[str], symbol: str, blob: bytes) -> None:
    lines.append(f"static const uint8_t {symbol}[{len(blob)}] = {{")
    for i in range(0, len(blob), 16):
        chunk = blob[i : i + 16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    lines.append("")


def ensure_asm_csos(dll: bytes) -> list[tuple[str, int, bytes]]:
    """Write .asm sources from bm2dx; require matching .cso from d3dx assemble."""
    out = []
    for name, src_len, marker, expect_cso in ASM:
        src = find_shader(dll, marker, src_len)
        asm_path = os.path.join(HERE, name + ".asm")
        with open(asm_path, "wb") as f:
            f.write(src)
        cso_path = os.path.join(HERE, name + ".cso")
        if not os.path.isfile(cso_path):
            print(
                f"missing {cso_path}; build/run asm_blob.cpp with d3dx9_43.dll first",
                file=sys.stderr,
            )
            raise SystemExit(1)
        with open(cso_path, "rb") as f:
            cso = f.read()
        if len(cso) != expect_cso:
            print(
                f"{name}.cso size {len(cso)} != expected {expect_cso}",
                file=sys.stderr,
            )
            raise SystemExit(1)
        print(f"  {name}: asm_src_len={src_len} cso={len(cso)}")
        out.append((name, src_len, cso))
    return out


def main() -> int:
    if not os.path.isfile(FXC):
        print("fxc not found:", FXC, file=sys.stderr)
        return 1
    if not os.path.isfile(BM2DX):
        print("bm2dx.dll not found:", BM2DX, file=sys.stderr)
        return 1

    dll = open(BM2DX, "rb").read()
    entries = []
    for name, src_len, profile, entry, marker, must_all, must_not in EXTRACT:
        src = find_shader(dll, marker, src_len, must_all, must_not)
        entries.append(compile_one(name, src, profile, entry, src_len))

    asm_entries = ensure_asm_csos(dll)

    lines = [
        "/* Auto-generated by shaders/gen_bytecode.py — do not edit */",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "typedef struct IidxShaderBlob {",
        "    uint64_t key;",
        "    uint32_t src_len;",
        "    const char *profile;",
        "    const char *entry;",
        "    const char *name;",
        "    const uint8_t *data;",
        "    uint32_t size;",
        "} IidxShaderBlob;",
        "",
    ]

    for name, profile, entry, src_len, key, cso in entries:
        emit_bytes_array(lines, f"k_{name}_cso", cso)

    for name, src_len, cso in asm_entries:
        emit_bytes_array(lines, f"k_{name}_cso", cso)

    lines.append(f"#define IIDX_SHADER_BLOB_COUNT {len(entries)}")
    lines.append(
        "static const IidxShaderBlob g_iidx_shader_blobs[IIDX_SHADER_BLOB_COUNT] = {"
    )
    for name, profile, entry, src_len, key, cso in entries:
        lines.append(
            f'    {{ 0x{key:016X}ULL, {src_len}u, "{profile}", "{entry}", "{name}", '
            f"k_{name}_cso, {len(cso)}u }},"
        )
    lines.append("};")
    lines.append("")

    lines.append("typedef struct AsmShaderBlob {")
    lines.append("    uint32_t src_len;")
    lines.append("    const char *tag;")
    lines.append("    const uint8_t *data;")
    lines.append("    uint32_t size;")
    lines.append("} AsmShaderBlob;")
    lines.append("")
    lines.append(
        f"#define ASM_BLOB_COUNT {len(asm_entries)}"
    )
    lines.append("static const AsmShaderBlob g_asm_blobs[ASM_BLOB_COUNT] = {")
    for name, src_len, cso in asm_entries:
        lines.append(
            f'    {{ {src_len}u, "{name}", k_{name}_cso, {len(cso)}u }},'
        )
    lines.append("};")
    lines.append("")

    with open(OUT_H, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print("Wrote", OUT_H, f"compile_blobs={len(entries)} asm_blobs={len(asm_entries)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
