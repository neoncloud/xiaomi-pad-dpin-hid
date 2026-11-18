#!/usr/bin/env python3
"""Generate C assets from the pinned native PTP manifest."""

from __future__ import annotations

import hashlib
from pathlib import Path

from elftools.elf.elffile import ELFFile

from dpin_digitizer_descriptor import build_descriptor
from build_ptp_native_manifest import build_manifest, va_bytes
from ptp_combined_descriptor import build_combined
from ptp_touchpad_descriptor import DEFAULT_PTPHQA_BLOB


def c_bytes(name: str, data: bytes) -> str:
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for offset in range(0, len(data), 12):
        lines.append("    " + ", ".join(f"0x{x:02x}" for x in data[offset:offset + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def write_header(stock_elf: Path, output: Path) -> None:
    manifest = build_manifest(stock_elf)
    with stock_elf.open("rb") as stream:
        rodata = ELFFile(stream).get_section_by_name(".rodata")
        if rodata is None:
            raise ValueError("stock ELF is missing .rodata")
        address = rodata["sh_addr"]
        data = rodata.data()
    stock = va_bytes(data, address, 0x3368, 133)
    stock_digitizer = va_bytes(data, address, 0x33ED, 914)
    combined = build_combined(stock)
    if hashlib.sha256(stock_digitizer).hexdigest() != (
        "2235bd04d655a092c83b7dfb3915ba67b4b63ec4a1f0ef332851bbed5bdf3a25"
    ):
        raise ValueError("stock digitizer descriptor mismatch")
    digitizer = build_descriptor(stock_digitizer)
    if hashlib.sha256(digitizer).hexdigest() != (
        "0b416366272cf5314ce9770f63925d711902a4c1f6b6674cd09c9f425a878aa4"
    ):
        raise ValueError("digitizer descriptor differs from the reviewed version")

    functions = manifest["functions"]
    intercepts = manifest["intercepts"]
    header = [
        "#ifndef PTP_NATIVE_GENERATED_H",
        "#define PTP_NATIVE_GENERATED_H",
        "#include <stdint.h>",
        f'#define PTP_SOURCE_SHA256 "{manifest["source_sha256"]}"',
        f"#define PTP_FUNCTION_COUNT {len(functions)}",
        f"#define PTP_INTERCEPT_COUNT {len(intercepts)}",
        f"#define PTP_SEND_SHORTCUT_VA 0x{functions['send_shortcut']['va']:x}u",
        "typedef struct { const char *name; uint32_t va; const uint8_t *bytes; } PtpFuncSpec;",
        "typedef struct { uint32_t kind; uint32_t va; const uint8_t *bytes; } PtpInterceptSpec;",
        c_bytes("k_stock_descriptor", stock),
        c_bytes("k_combined_descriptor", combined),
        c_bytes("k_stock_digitizer_descriptor", stock_digitizer),
        c_bytes("k_digitizer_descriptor", digitizer),
        c_bytes("k_ptphqa", DEFAULT_PTPHQA_BLOB),
    ]
    for name, row in functions.items():
        raw = bytes.fromhex(row["bytes32"])
        header.append(c_bytes(f"k_prologue_{name}", raw))
    header.append("static const PtpFuncSpec k_functions[PTP_FUNCTION_COUNT] = {")
    for name, row in functions.items():
        header.append(f'    {{"{name}", 0x{row["va"]:x}u, k_prologue_{name}}},')
    header.append("};")
    kinds = sorted({row["kind"] for row in intercepts.values()})
    for kind in kinds:
        header.append(f"#define PTP_IX_{kind.upper()} 0x{list(kinds).index(kind) + 1:x}u")
    for name, row in sorted(intercepts.items()):
        header.append(c_bytes(f"k_ix_{name}", bytes.fromhex(row["bytes"])))
    header.append("static const PtpInterceptSpec k_intercept_sites[PTP_INTERCEPT_COUNT] = {")
    for name, row in sorted(intercepts.items()):
        header.append(
            f'    {{PTP_IX_{row["kind"].upper()}, 0x{row["va"]:x}u, k_ix_{name}}},'
        )
    header.append("};")
    header.append("#endif")
    output.write_text("\n".join(header) + "\n", encoding="ascii")
