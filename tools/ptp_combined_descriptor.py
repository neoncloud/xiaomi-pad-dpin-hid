"""Build the reviewed stock keyboard/mouse plus Precision Touchpad descriptor."""

import hashlib

from ptp_touchpad_descriptor import build_descriptor


PTP_SHA256 = "db8b353ad52a81af5b60ce1b0aaa2666b62b411e8f9aec021bf8264e1c7c08c9"
STOCK_HID0_SHA256 = "0ac38b057ec9608b20557ce069c520e1789bf8a597059126dc76d464f7599755"
COMBINED_SHA256 = "a5c015f28d485401c7f754e714ac4e741cee99b8fcf2579a4b17bc22486caa2b"
ID_MAP = {1: 16, 2: 17, 3: 18, 5: 20, 6: 21}
GLOBAL_RESET = bytes.fromhex("050015002500350045005500650075009500")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def items(data: bytes):
    pos = 0
    while pos < len(data):
        prefix = data[pos]
        size = (0, 1, 2, 4)[prefix & 3]
        if prefix == 0xFE or pos + size + 1 > len(data):
            raise ValueError("long/truncated HID item")
        yield pos, prefix, data[pos + 1:pos + 1 + size]
        pos += size + 1


def parse(data: bytes) -> list[dict]:
    """Strict HID subset including Output, signed ranges, PUSH/POP and bit offsets.

    Unsupported locals/designators/delimiters are refused, never silently ignored.
    """
    state = dict.fromkeys(range(10), 0)
    stack, path, usages, fields = [], [], [], []
    minimum = None
    offsets = {}
    for _, prefix, raw in items(data):
        kind, tag = (prefix >> 2) & 3, prefix >> 4
        value = int.from_bytes(raw, "little")
        if kind == 1:
            if tag in (10, 11):
                if raw:
                    raise ValueError("PUSH/POP must have no payload")
                if tag == 10:
                    stack.append(state.copy())
                elif stack:
                    state = stack.pop()
                else:
                    raise ValueError("POP underflow")
            elif tag in state and raw:
                if tag == 8 and not 1 <= value <= 255:
                    raise ValueError("invalid Report ID")
                if tag == 5:
                    value &= 15
                    value = value - 16 if value & 8 else value
                elif tag in (1, 3) or (tag == 2 and state[1] < 0) or (tag == 4 and state[3] < 0):
                    value = int.from_bytes(raw, "little", signed=True)
                state[tag] = value
            else:
                raise ValueError("unsupported global")
        elif kind == 2:
            usage = (value >> 16, value & 65535) if len(raw) == 4 else (state[0], value)
            if not raw:
                raise ValueError("empty local")
            if tag == 0:
                usages.append(usage)
            elif tag == 1 and minimum is None:
                minimum = usage
            elif tag == 2 and minimum is not None and usage[0] == minimum[0] and 0 <= usage[1] - minimum[1] <= 255:
                usages.extend((usage[0], n) for n in range(minimum[1], usage[1] + 1))
                minimum = None
            else:
                raise ValueError("unsupported/unpaired local")
        elif kind == 0:
            if minimum is not None:
                raise ValueError("unpaired usage minimum")
            if tag == 10 and len(usages) == 1 and len(raw) == 1:
                path.append((value, *usages[0]))
            elif tag == 12 and path and not raw:
                path.pop()
            elif tag in (8, 9, 11) and path and raw:
                if not state[8] or not 1 <= state[7] <= 32 or not 1 <= state[9] <= 4096:
                    raise ValueError("invalid report dimensions/ID")
                report_kind = {8: "input", 9: "output", 11: "feature"}[tag]
                key = (report_kind, state[8])
                fields.append(dict(kind=report_kind, report_id=state[8], usages=tuple(usages),
                    size=state[7], count=state[9], flags=value,
                    logical=(state[1], state[2]), physical=(state[3], state[4]),
                    exponent=state[5], unit=state[6], path=tuple(path), offset=offsets.get(key, 0)))
                offsets[key] = offsets.get(key, 0) + state[7] * state[9]
            else:
                raise ValueError("unsupported/unbalanced main item")
            usages = []
        else:
            raise ValueError("reserved item")
    if stack or path or usages or minimum is not None or not fields:
        raise ValueError("unbalanced/empty descriptor")
    return fields


def report_lengths(fields: list[dict]) -> dict[tuple[str, int], int]:
    bits = {}
    for f in fields:
        key = (f["kind"], f["report_id"])
        bits[key] = max(bits.get(key, 0), f["offset"] + f["size"] * f["count"])
    if any(n % 8 for n in bits.values()):
        raise ValueError("report must be byte aligned")
    return {k: 1 + n // 8 for k, n in bits.items()}


def validate_stock(stock: bytes, expected_sha256: str) -> list[dict]:
    if sha256(stock) != expected_sha256.lower():
        raise ValueError("stock SHA256 mismatch")
    fields = parse(stock)
    if {f["report_id"] for f in fields} != {1, 2}:
        raise ValueError("stock must use only IDs 1/2")
    for f in fields:
        expected = (1, 1, 6 if f["report_id"] == 1 else 2)
        if f["path"][0] != expected or f["kind"] == "feature":
            raise ValueError("stock ID/TLC contract changed")
    lengths = report_lengths(fields)
    if lengths.get(("input", 1)) != 9 or ("input", 2) not in lengths or max(lengths.values()) > 9:
        raise ValueError("stock report_length=9 baseline changed")
    for axis in (48, 49):
        if not any(f["kind"] == "input" and f["report_id"] == 2 and
                   (1, axis) in f["usages"] and f["flags"] & 7 == 6 for f in fields):
            raise ValueError("stock relative mouse X/Y missing")
    return fields


def build_combined(stock: bytes) -> bytes:
    original_fields = validate_stock(stock, STOCK_HID0_SHA256)
    ptp = build_descriptor()
    if sha256(ptp) != PTP_SHA256:
        raise ValueError("PTP descriptor baseline changed")

    combined = bytearray(stock + GLOBAL_RESET)
    report_id = 0
    for _, prefix, payload in items(ptp):
        if prefix == 0x85:
            report_id = ID_MAP[payload[0]]
            payload = bytes([report_id])
        combined.append(prefix)
        combined.extend(payload)
        if prefix == 0xB1 and ((report_id == 20 and payload == b"\x02") or
                               (report_id == 21 and payload == b"\x03")):
            # Padding produces the 9/13-byte configuration feature buffers.
            count = 7 if report_id == 20 else 10
            combined.extend(bytes.fromhex("a415002500750895") + bytes([count])
                            + bytes.fromhex("b103b4"))

    fields = parse(bytes(combined))
    lengths = report_lengths(fields)
    expected = {
        ("input", 1): 9, ("input", 2): 7, ("output", 1): 2, ("input", 16): 30,
        ("feature", 17): 2, ("feature", 18): 34, ("feature", 20): 9, ("feature", 21): 13,
    }
    if fields[:len(original_fields)] != original_fields or lengths != expected:
        raise ValueError("combined descriptor changed the stock layout or wire contract")
    if sha256(combined) != COMBINED_SHA256:
        raise ValueError("combined descriptor differs from the reviewed version")
    return bytes(combined)
