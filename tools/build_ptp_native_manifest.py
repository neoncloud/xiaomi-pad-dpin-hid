#!/usr/bin/env python3
"""Generate a pinned native-hook manifest from the stock vendor ELF."""

from __future__ import annotations

import hashlib
from pathlib import Path

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_REG_X16
from elftools.elf.elffile import ELFFile


EXPECTED_SHA256 = "17b70644c4cd1ed8421616704eb72641aad5b7c00d94ecf029d324ce02d5a922"

FUNCTIONS = {
    "hidg_write_retry": (0xB870, 0xB928),
    "reliable_write": (0xB928, 0xB9FC),
    "send_shortcut": (0xBA04, 0xBBC4),
    "handle_touchscreen": (0xC36C, 0xC90C),
    "handle_pen": (0xC90C, 0xCCD4),
    "handle_touchpad": (0xCCD4, 0xD9BC),
    "tp_process_gestures": (0xD9BC, 0xDD0C),
    "tp_process_tap1": (0xDD0C, 0xDFB0),
    "tp_process_tap2": (0xDFB0, 0xE378),
    "do_setup": (0xE68C, 0xF008),
    "do_teardown": (0xF008, 0xF2A0),
    "create_hid_func": (0xF65C, 0xF9E8),
    "bridge_thread": (0x99E8, 0xB0A0),
}

# These are exact instruction sites used by the native shim.  The names are
# stable manifest keys; the generated header carries the bytes checked before
# installation.
INTERCEPTS = {
    "create_hid": (0xEAC4, "create_hid"),
    "setup_enter": (0x9C24, "setup_enter"),
    "setup_leave": (0x9C28, "setup_leave"),
    "touchpad_enter": (0xADE0, "touchpad_enter"),
    "touchpad_leave": (0xADE4, "touchpad_leave"),
    "teardown_enter": (0xAFE4, "teardown_enter"),
    "teardown_leave": (0xAFE8, "teardown_leave"),
    "hidg0_open_enter": (0xA16C, "hidg0_open_enter"),
    "hidg0_open_leave": (0xA170, "hidg0_open_leave"),
    "hidg1_open_enter": (0xA180, "hidg1_open_enter"),
    "hidg1_open_leave": (0xA184, "hidg1_open_leave"),
    "hidg0_read_enter": (0xAC88, "hidg0_read_enter"),
    "hidg0_read_leave": (0xAC8C, "hidg0_read_leave"),
    "hidg1_read_enter": (0xACCC, "hidg1_read_enter"),
    "hidg1_read_leave": (0xACD0, "hidg1_read_leave"),
    "touchpad_read_enter": (0xCD34, "touchpad_read_enter"),
    "touchpad_read_leave": (0xCD38, "touchpad_read_leave"),
    "close_bridge_hidg1": (0xA43C, "close"),
    "close_bridge_hidg0": (0xA450, "close"),
    "close_bridge_event": (0xAFD0, "close"),
    "close_teardown_hidg0": (0xF0BC, "close"),
    "close_teardown_hidg1": (0xF0C8, "close"),
    "digitizer_create": (0xEB0C, "digitizer_create"),
    "screen_first_read": (0xC3C8, "digitizer_read_enter"),
    "screen_first_read_return": (0xC3CC, "screen_read_leave"),
    "screen_next_read": (0xC40C, "digitizer_read_enter"),
    "screen_next_read_return": (0xC410, "screen_read_leave"),
    "screen_output": (0xC4F8, "screen_write"),
    "screen_output_return": (0xC4FC, "screen_write_leave"),
    "pen0_enter": (0xAE14, "pen_enter"),
    "pen1_enter": (0xAE34, "pen_enter"),
    "pen0_leave": (0xAE18, "pen_leave"),
    "pen1_leave": (0xAE38, "pen_leave"),
    "pen_read": (0xC974, "digitizer_read_enter"),
    "pen_read_return": (0xC978, "pen_read_leave"),
    "aux_input_read": (0xAE88, "digitizer_read_enter"),
    "aux_input_read_return": (0xAE8C, "pen_aux_read_leave"),
    "bridge_select_return": (0xA91C, "pen_action_tick"),
    "pen_output": (0xCBF8, "pen_write"),
    "pen_output_return": (0xCBFC, "pen_write_leave"),
    "screen_initial_report": (0xA7B4, "screen_empty"),
    "screen_initial_return": (0xA7B8, "screen_write_leave"),
    "screen_final_report": (0xAF74, "screen_empty"),
    "screen_final_return": (0xAF78, "screen_write_leave"),
    "touchpad_screen_write_return": (0xD8C0, "screen_write_leave"),
    "pen_initial_report": (0xA7D8, "pen_empty"),
    "pen_final_report": (0xAFB0, "pen_empty"),
}

INTERESTING_PLT = {
    "__open_2",
    "close",
    "read",
    "write",
    "__write_chk",
    "ioctl",
}


def va_bytes(data: bytes, section_va: int, va: int, size: int) -> bytes:
    offset = va - section_va
    if offset < 0 or offset + size > len(data):
        raise ValueError(f"VA {va:#x} is outside section at {section_va:#x}")
    return data[offset : offset + size]


def build_plt_map(elf: ELFFile, data: bytes) -> dict[int, str]:
    rela = elf.get_section_by_name(".rela.plt")
    dynsym = elf.get_section_by_name(".dynsym")
    plt = elf.get_section_by_name(".plt")
    if not rela or not dynsym or not plt:
        raise ValueError("missing PLT relocation sections")
    got_names = {
        rel["r_offset"]: dynsym.get_symbol(rel["r_info_sym"]).name
        for rel in rela.iter_relocations()
    }
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    result: dict[int, str] = {}
    entry_size = plt["sh_entsize"] or 16
    plt_data = plt.data()
    for offset in range(entry_size, plt["sh_size"], entry_size):
        addr = plt["sh_addr"] + offset
        insns = list(md.disasm(va_bytes(plt_data, plt["sh_addr"], addr, entry_size), addr))
        if len(insns) < 4 or insns[0].mnemonic != "adrp" or insns[1].mnemonic != "ldr":
            continue
        ldr = insns[1]
        if not ldr.operands:
            continue
        if ldr.operands[1].type != ARM64_OP_MEM or ldr.operands[1].mem.base != ARM64_REG_X16:
            continue
        page = insns[0].operands[1].imm
        got = page + ldr.operands[1].mem.disp
        name = got_names.get(got)
        if name:
            result[addr] = name
    return result


def function_record(data: bytes, section_va: int, start: int, end: int) -> dict:
    prologue = va_bytes(data, section_va, start, 32)
    return {
        "va": start,
        "size": end - start,
        "bytes32": prologue.hex(),
        "sha256_32": hashlib.sha256(prologue).hexdigest(),
    }


def collect_calls(
    data: bytes, section_va: int, start: int, end: int, plt_map: dict[int, str]
) -> list[dict]:
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    calls = []
    for ins in md.disasm(va_bytes(data, section_va, start, end - start), start):
        if ins.mnemonic not in {"bl", "b"} or not ins.operands:
            continue
        op = ins.operands[0]
        if op.type != ARM64_OP_IMM:
            continue
        target = op.imm
        internal = next(
            (name for name, (fstart, fend) in FUNCTIONS.items() if fstart <= target < fend),
            None,
        )
        imported = plt_map.get(target)
        if (ins.mnemonic == "bl" and (internal or imported)) or (
            ins.mnemonic == "b" and internal and not start <= target < end
        ):
            calls.append(
                {
                    "call_va": ins.address,
                    "return_va": ins.address + 4,
                    "instruction": va_bytes(data, section_va, ins.address, 4).hex(),
                    "mnemonic": ins.mnemonic,
                    "target_va": target,
                    "target": internal or imported,
                    "target_kind": "internal" if internal else "plt",
                }
            )
    return calls


def classify(function: str, call: dict) -> str | None:
    target = call["target"]
    if target == "write" and function in {
        "handle_touchpad",
        "tp_process_gestures",
        "tp_process_tap1",
        "tp_process_tap2",
    }:
        return "touchpad_direct_write"
    if target == "__write_chk" and function in {"hidg_write_retry", "reliable_write"}:
        return "shared_helper_write_callsite"
    if target == "read" and function == "handle_touchpad":
        return "touchpad_read"
    if target == "__open_2" and function == "bridge_thread":
        if call["return_va"] in {0xA170, 0xA184}:
            return "hidg_open"
    if (
        target == "close"
        and function == "do_teardown"
        and call["return_va"] in {0xF0C0, 0xF0CC}
    ):
        return "hidg_teardown_close"
    if target in {"reliable_write", "hidg_write_retry"} and function == "handle_touchpad":
        return "touchpad_helper_call"
    return None


def build_manifest(stock_elf: Path) -> dict:
    raw = stock_elf.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != EXPECTED_SHA256:
        raise ValueError(f"stock ELF hash mismatch: {digest}")
    with stock_elf.open("rb") as stream:
        elf = ELFFile(stream)
        text = elf.get_section_by_name(".text")
        if not text:
            raise ValueError("missing .text")
        text_data = text.data()
        text_va = text["sh_addr"]
        plt_map = build_plt_map(elf, text_data)

    functions = {
        name: function_record(text_data, text_va, start, end)
        for name, (start, end) in FUNCTIONS.items()
    }
    shortcut = functions["send_shortcut"]["va"]
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    decoder.detail = True
    shortcut_callers = {
        (ins.address, ins.mnemonic) for ins in decoder.disasm(text_data, text_va)
        if ins.mnemonic in {"bl", "b"} and ins.operands[0].imm == shortcut
    }
    if shortcut_callers != {(0xDAB4, "bl"), (0xDD08, "b")}:
        raise ValueError(f"unexpected direct send_shortcut callers: {sorted(shortcut_callers)}")
    calls = []
    for name, (start, end) in FUNCTIONS.items():
        for call in collect_calls(text_data, text_va, start, end, plt_map):
            call["function"] = name
            call["class"] = classify(name, call)
            calls.append(call)

    for address, symbol in ((0xAE88, "read"), (0xA918, "select")):
        if not any(call["call_va"] == address and call["target"] == symbol for call in calls):
            raise ValueError(f"unexpected stock {symbol} call at {address:#x}")

    intercepts = {}
    for name, (va, kind) in INTERCEPTS.items():
        intercepts[name] = {
            "va": va,
            "kind": kind,
            "bytes": va_bytes(text_data, text_va, va, 4).hex(),
        }
    for call in calls:
        if call["class"] == "touchpad_direct_write":
            name = f"direct_write_{call['call_va']:x}"
            intercepts[name] = {
                "va": call["call_va"],
                "kind": "direct_write",
                "bytes": call["instruction"],
            }
        elif call["class"] == "shared_helper_write_callsite":
            name = f"helper_write_{call['call_va']:x}"
            intercepts[name] = {
                "va": call["call_va"],
                "kind": "helper_write",
                "bytes": call["instruction"],
            }
    if len({row["va"] for row in intercepts.values()}) != len(intercepts):
        raise ValueError("duplicate intercept address")

    return {
        "source_sha256": digest,
        "functions": functions,
        "intercepts": intercepts,
    }
