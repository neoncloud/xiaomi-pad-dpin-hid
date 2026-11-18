"""Preserve the stock report layouts with touch Confidence and full pen data."""

from ptp_combined_descriptor import parse, report_lengths


PEN_SWIPE_REPORT_ID = 4
PEN_SWIPE_DESCRIPTOR = bytes.fromhex(
    "05010906a1018504"  # Keyboard application, separate report ID.
    "3500450055006500"  # Reset inherited physical ranges and units.
    "050719e029e715002501750195088102"  # Modifier byte.
    "750895018103"  # Reserved byte.
    "1900297315002573750895068100c0"  # Six-key array.
)


def build_descriptor(stock: bytes) -> bytes:
    pen_header = bytes.fromhex("050d0902a1018503")
    touch, pen = stock.split(pen_header, 1)
    finger_padding = bytes.fromhex("09328102750681030951")
    finger_confidence = bytes.fromhex("0932810209478102750581030951")
    if touch.count(finger_padding) != 10:
        raise ValueError("unexpected stock touchscreen layout")
    pressure = bytes.fromhex("0930150026ff1f75108102")
    tilt = bytes.fromhex("093d15c4253c75088102093e8102")
    switches = bytes.fromhex("0932810275048103")
    if pen.count(pressure) != 1 or pen.count(tilt) != 1 or pen.count(switches) != 1:
        raise ValueError("unexpected stock pen layout")
    pen = pen.replace(switches, bytes.fromhex("09328102095a810275038103"))
    pen = pen.replace(pressure, bytes.fromhex("0930150026ff3f75108102"))
    pen = pen.replace(tilt, bytes.fromhex("093d15c4253c35c4453c651475088102093e8102"))
    # The physical range is 360.00 degrees, encoded in hundredths of a degree.
    # Windows sign-extends two-byte limit items even with a zero minimum.
    twist = bytes.fromhex("09411500279f8c0000350047a08c0000550e6514751095018102")
    if not pen.endswith(bytes.fromhex("093e8102c0c0")):
        raise ValueError("unexpected pen collection boundary")
    descriptor = (touch.replace(finger_padding, finger_confidence) + pen_header +
                  pen[:-2] + twist + pen[-2:] + PEN_SWIPE_DESCRIPTOR)

    stock_fields = parse(stock)
    fields = parse(descriptor)
    expected_lengths = report_lengths(stock_fields)
    expected_lengths.update({("input", 3): 12, ("input", PEN_SWIPE_REPORT_ID): 9})
    if report_lengths(fields) != expected_lengths:
        raise ValueError("unexpected digitizer report lengths")

    def public_fields(rows):
        added = {((0x0D, usage),) for usage in (0x41, 0x47, 0x5A)}
        return [row for row in rows if not row["flags"] & 1 and
                row["usages"] not in added and row["report_id"] != PEN_SWIPE_REPORT_ID]

    expected = [dict(row) for row in public_fields(stock_fields)]
    for row in expected:
        if row["kind"] != "input" or row["report_id"] != 3:
            continue
        if row["usages"] == ((0x0D, 0x30),):
            row["logical"] = (0, 16383)
        if row["usages"] in (((0x0D, 0x3D),), ((0x0D, 0x3E),)):
            row["physical"] = (-60, 60)
            row["unit"] = 0x14
    if public_fields(fields) != expected:
        raise ValueError("unexpected digitizer field changes")
    confidence = [row for row in fields if row["usages"] == ((0x0D, 0x47),)]
    if [row["offset"] for row in confidence] != [slot * 48 + 2 for slot in range(10)]:
        raise ValueError("unexpected Confidence offsets")
    barrel2, = [row for row in fields if row["usages"] == ((0x0D, 0x5A),)]
    angle, = [row for row in fields if row["usages"] == ((0x0D, 0x41),)]
    if (barrel2["report_id"], barrel2["offset"], barrel2["size"]) != (3, 4, 1):
        raise ValueError("unexpected secondary barrel position")
    if (angle["report_id"], angle["offset"], angle["size"], angle["logical"],
        angle["physical"], angle["unit"], angle["exponent"]) != (
            3, 72, 16, (0, 35999), (0, 36000), 0x14, -2):
        raise ValueError("unexpected pen twist field")
    return descriptor
