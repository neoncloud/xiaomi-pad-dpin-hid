"""Precision Touchpad collections for the combined USB descriptor."""

REPORT_ID_TOUCHPAD = 1
REPORT_ID_CAPABILITIES = 2
REPORT_ID_CERTIFICATION = 3
REPORT_ID_INPUT_MODE = 5
REPORT_ID_SELECTIVE_REPORTING = 6

MAX_CONTACTS = 5
LOGICAL_MAX_X = 4095
LOGICAL_MAX_Y = 4095

# The physical values use the Microsoft sample units: 0.01 inch.
# Xiaomi Touch reports 3683/2175 logical units with resolutions of
# 33/37 units per millimetre, or approximately 111.6 x 58.8 mm.
PHYSICAL_MAX_X = 440
PHYSICAL_MAX_Y = 232

DEFAULT_PTPHQA_BLOB = bytes.fromhex(
    "fc28fe8440cb9a870dbe573cb6700988"
    "07972d2be33834b66cedb0f7e59cf6c2"
    "2e841be8b45178431f284b7c2d53affc"
    "47701b596f7443c4f34718531aa2a171"
    "c7950e315521d3b51ee90cbaecb88919"
    "3eb3af75819d53b94157f46d3925297c"
    "87d9b498457da7269c653b856889d73b"
    "bdff1467f22bf02a4154f0fd2c667cf8"
    "c08f331303f1d3c10b89d91b62cd51b7"
    "80b8af3a10c18a5be88a56f08caafa35"
    "e942c4d855c338cc2b535c6952d5c873"
    "02387c73b641e7ff05d82b799ae23460"
    "8fa3321f097862bc80e30fbd65200813"
    "c1e2ee532d867ea75ac5d37d98be3148"
    "1ffbdaafa2a86a89d6bff2d3322a9ae4"
    "cf17b7b8f4e13308248bc443a5e524c2"
)

def _u16(value: int) -> bytes:
    if not 0 <= value <= 0xFFFF:
        raise ValueError(f"value does not fit uint16: {value}")
    return bytes([value & 0xFF, value >> 8])


def build_descriptor(max_contacts: int = MAX_CONTACTS) -> bytes:
    if not 3 <= max_contacts <= 5:
        raise ValueError("Windows Precision Touchpad requires 3..5 contacts")

    d: list[int] = []
    contact_id_bits = (max_contacts - 1).bit_length()
    contact_id_padding_bits = 8 - 2 - contact_id_bits

    # Touch Pad top-level collection.
    d += [
        0x05, 0x0D,                    # Usage Page (Digitizers)
        0x09, 0x05,                    # Usage (Touch Pad)
        0xA1, 0x01,                    # Collection (Application)
        0x85, REPORT_ID_TOUCHPAD,      # Report ID (Touchpad input)
    ]

    for _ in range(max_contacts):
        d += [
            0x09, 0x22,                # Usage (Finger)
            0xA1, 0x02,                # Collection (Logical)
            0x15, 0x00,                # Logical Minimum (0)
            0x25, 0x01,                # Logical Maximum (1)
            0x09, 0x47,                # Usage (Confidence)
            0x09, 0x42,                # Usage (Tip Switch)
            0x95, 0x02,                # Report Count (2)
            0x75, 0x01,                # Report Size (1)
            0x81, 0x02,                # Input (Data,Var,Abs)
            0x95, 0x01,                # Report Count (1)
            0x75, contact_id_bits,     # Report Size (Contact ID)
            0x25, max_contacts - 1,    # Logical Maximum (contact ID)
            0x09, 0x51,                # Usage (Contact Identifier)
            0x81, 0x02,                # Input (Data,Var,Abs)
            0x75, 0x01,                # Report Size (1)
            0x95, contact_id_padding_bits,
            0x81, 0x03,                # Input (Cnst,Var,Abs)
            0xA4,                      # Push globals: isolate X/Y page/units/ranges
            0x05, 0x01,                # Usage Page (Generic Desktop)
            0x15, 0x00,                # Logical Minimum (0)
            0x26, 0xFF, 0x0F,          # Logical Maximum (4095)
            0x75, 0x10,                # Report Size (16)
            0x55, 0x0E,                # Unit Exponent (-2)
            0x65, 0x13,                # Unit (Inch, EngLinear)
            0x09, 0x30,                # Usage (X)
            0x35, 0x00,                # Physical Minimum (0)
            0x46, 0xB8, 0x01,          # Physical Maximum (440)
            0x95, 0x01,                # Report Count (1)
            0x81, 0x02,                # Input (Data,Var,Abs)
            0x46, 0xE8, 0x00,          # Physical Maximum (232)
            0x09, 0x31,                # Usage (Y)
            0x81, 0x02,                # Input (Data,Var,Abs)
            0xB4,                      # Pop: restore Digitizers before next Finger
            0xC0,                      # End Collection (Logical)
        ]

    # Scan time, contact count, and report-level buttons.
    d += [
        0xA4,                          # Push: time units apply only to Scan Time
        0x55, 0x0C,                    # Unit Exponent (-4)
        0x66, 0x01, 0x10,              # Unit (Seconds)
        0x47, 0xFF, 0xFF, 0x00, 0x00,  # Physical Maximum (65535)
        0x27, 0xFF, 0xFF, 0x00, 0x00,  # Logical Maximum (65535)
        0x75, 0x10,                    # Report Size (16)
        0x95, 0x01,                    # Report Count (1)
        0x05, 0x0D,                    # Usage Page (Digitizers)
        0x09, 0x56,                    # Usage (Scan Time)
        0x81, 0x02,                    # Input (Data,Var,Abs)
        0xB4,                          # Pop: counts/buttons/features are unitless
        0x09, 0x54,                    # Usage (Contact Count)
        0x15, 0x00,                    # Logical Minimum (0)
        0x25, 0x7F,                    # Logical Maximum (127)
        0x75, 0x08,                    # Report Size (8)
        0x95, 0x01,                    # Report Count (1)
        0x81, 0x02,                    # Input (Data,Var,Abs)
        0x05, 0x09,                    # Usage Page (Button)
        0x19, 0x01,                    # Usage Minimum (Button 1)
        0x29, 0x03,                    # Usage Maximum (Button 3)
        0x15, 0x00,                    # Logical Minimum (0)
        0x25, 0x01,                    # Logical Maximum (1)
        0x75, 0x01,                    # Report Size (1)
        0x95, 0x03,                    # Report Count (3)
        0x81, 0x02,                    # Input (Data,Var,Abs)
        0x95, 0x05,                    # Report Count (5)
        0x81, 0x03,                    # Input (Cnst,Var,Abs)
    ]

    # Device capabilities feature report: contact maximum and click-pad type.
    d += [
        0x05, 0x0D,                    # Usage Page (Digitizers)
        0x85, REPORT_ID_CAPABILITIES,  # Report ID (Capabilities)
        0x09, 0x55,                    # Usage (Contact Count Maximum)
        0x09, 0x59,                    # Usage (Pad Type)
        0x15, 0x00,                    # Logical Minimum (0)
        0x25, 0x0F,                    # Logical Maximum (15)
        0x75, 0x04,                    # Report Size (4)
        0x95, 0x02,                    # Report Count (2)
        0xB1, 0x02,                    # Feature (Data,Var,Abs)
    ]

    # Segmented PTPHQA status. The kernel HID gadget caps feature reports at
    # 64 bytes, so expose the 256-byte blob as eight 32-byte segments.
    d += [
        0x06, 0x00, 0xFF,              # Usage Page (Vendor Defined)
        0x85, REPORT_ID_CERTIFICATION, # Report ID (PTPHQA segment)
        0x09, 0xC6,                    # Usage (Segment number)
        0x15, 0x00,                    # Logical Minimum (0)
        0x25, 0x07,                    # Logical Maximum (7)
        0x75, 0x08,                    # Report Size (8)
        0x95, 0x01,                    # Report Count (1)
        0xB1, 0x02,                    # Feature (Data,Var,Abs)
        0x09, 0xC7,                    # Usage (Segment data)
        0x26, 0xFF, 0x00,              # Logical Maximum (255)
        0x96, 0x20, 0x00,              # Report Count (32)
        0x75, 0x08,                    # Report Size (8)
        0xB1, 0x02,                    # Feature (Data,Var,Abs)
        0xC0,                          # End Collection (Touch Pad)
    ]

    # Configuration top-level collection.
    d += [
        0x05, 0x0D,                    # Usage Page (Digitizers)
        0x09, 0x0E,                    # Usage (Configuration)
        0xA1, 0x01,                    # Collection (Application)
        0x85, REPORT_ID_INPUT_MODE,    # Report ID (Input mode)
        0x09, 0x22,                    # Usage (Finger)
        0xA1, 0x02,                    # Collection (Logical)
        0x09, 0x52,                    # Usage (Input Mode)
        0x15, 0x00,                    # Logical Minimum (0)
        0x25, 0x0A,                    # Logical Maximum (10)
        0x75, 0x08,                    # Report Size (8)
        0x95, 0x01,                    # Report Count (1)
        0xB1, 0x02,                    # Feature (Data,Var,Abs)
        0xC0,                          # End Collection (Logical)
        0x09, 0x22,                    # Usage (Finger)
        0xA1, 0x00,                    # Collection (Physical)
        0x85, REPORT_ID_SELECTIVE_REPORTING,
        0x09, 0x57,                    # Usage (Surface Switch)
        0x09, 0x58,                    # Usage (Button Switch)
        0x15, 0x00,                    # Logical Minimum (0)
        0x25, 0x01,                    # Logical Maximum (1)
        0x75, 0x01,                    # Report Size (1)
        0x95, 0x02,                    # Report Count (2)
        0xB1, 0x02,                    # Feature (Data,Var,Abs)
        0x75, 0x01,                    # Report Size (1)
        0x95, 0x0E,                    # Report Count (14)
        0xB1, 0x03,                    # Feature (Cnst,Var,Abs)
        0xC0,                          # End Collection (Physical)
        0xC0,                          # End Collection (Configuration)
    ]

    return bytes(d)
