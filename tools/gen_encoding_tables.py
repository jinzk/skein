#!/usr/bin/env python3
"""Generate compact GB18030/GBK tables for the Skein encoding module.

The generated files are committed so normal builds do not need Python.
Run this script only when intentionally changing the table source.

Data source:
  * GB18030: Python's built-in ``gb18030`` codec (GB18030-2005 mapping).
  * GBK: Python's built-in ``gbk`` codec.
  * GBK must be a strict subset of GB18030; the script asserts this.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

LEADS = range(0x81, 0xFF)
TRAILS = list(range(0x40, 0x7F)) + list(range(0x80, 0xFF))
TABLE_SIZE = len(LEADS) * len(TRAILS)

EXPECTED_GBK_VALID = 21791
EXPECTED_GB18030_TWO_BYTE = 23940
EXPECTED_RANGES = 207


def decode_two_byte(encoding: str) -> list[int | None]:
    table: list[int | None] = []
    for lead in LEADS:
        for trail in TRAILS:
            try:
                text = bytes((lead, trail)).decode(encoding)
            except UnicodeDecodeError:
                table.append(None)
                continue
            if len(text) != 1:
                table.append(None)
                continue
            codepoint = ord(text)
            assert codepoint <= 0xFFFF, (encoding, lead, trail, codepoint)
            table.append(codepoint)
    assert len(table) == TABLE_SIZE
    return table


def zigzag(value: int) -> int:
    return value * 2 if value >= 0 else -value * 2 - 1


def encode_varint(value: int) -> list[int]:
    assert value >= 0
    result: list[int] = []
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            result.append(byte | 0x80)
        else:
            result.append(byte)
            return result


def build_gb18030_two_byte(
    table: list[int | None],
) -> tuple[list[int], list[int], list[tuple[int, int]]]:
    assert len(table) == EXPECTED_GB18030_TWO_BYTE
    assert all(codepoint is not None for codepoint in table)

    runs: list[tuple[int, int]] = []
    index = 0
    while index < len(table):
        start = index
        codepoint = table[index]
        assert codepoint is not None
        index += 1
        while index < len(table) and table[index] == table[index - 1] + 1:
            index += 1
        runs.append((index - start, codepoint))

    lengths = [length for length, _ in runs]
    deltas: list[int] = []
    previous = 0
    for _, codepoint in runs:
        deltas.extend(encode_varint(zigzag(codepoint - previous)))
        previous = codepoint

    assert sum(lengths) == TABLE_SIZE
    assert max(lengths) <= 0xFF
    for length, codepoint in runs:
        assert codepoint + length - 1 <= 0xFFFF
    return lengths, deltas, runs


def build_gbk_bitmap(gb18030_table: list[int | None]) -> tuple[bytes, int]:
    gbk_table = decode_two_byte("gbk")
    valid = 0
    for index, (gbk_codepoint, gb18030_codepoint) in enumerate(
        zip(gbk_table, gb18030_table)
    ):
        if gbk_codepoint is None:
            continue
        assert gb18030_codepoint is not None, index
        assert gbk_codepoint == gb18030_codepoint, (
            index,
            gbk_codepoint,
            gb18030_codepoint,
        )
        valid += 1
    assert valid == EXPECTED_GBK_VALID, valid

    bits = bytearray((TABLE_SIZE + 7) // 8)
    for index, codepoint in enumerate(gbk_table):
        if codepoint is not None:
            bits[index >> 3] |= 1 << (index & 7)
    return bytes(bits), valid


def build_gb18030_ranges() -> list[tuple[int, int, int]]:
    ranges: list[tuple[int, int, int]] = []
    start_index = previous_index = start_codepoint = previous_codepoint = None

    for lead in LEADS:
        for second in range(0x30, 0x3A):
            for third in LEADS:
                for fourth in range(0x30, 0x3A):
                    index = (
                        ((lead - 0x81) * 10 + (second - 0x30)) * 126
                        + (third - 0x81)
                    ) * 10 + (fourth - 0x30)
                    try:
                        text = bytes((lead, second, third, fourth)).decode(
                            "gb18030"
                        )
                        codepoint = ord(text) if len(text) == 1 else None
                    except UnicodeDecodeError:
                        codepoint = None

                    if codepoint is None:
                        if start_index is not None:
                            ranges.append(
                                (start_index, previous_index, start_codepoint)
                            )
                            start_index = start_codepoint = None
                        continue

                    if start_index is None:
                        start_index = previous_index = index
                        start_codepoint = previous_codepoint = codepoint
                    elif index == previous_index + 1 and codepoint == previous_codepoint + 1:
                        previous_index = index
                        previous_codepoint = codepoint
                    else:
                        ranges.append(
                            (start_index, previous_index, start_codepoint)
                        )
                        start_index = previous_index = index
                        start_codepoint = previous_codepoint = codepoint

    if start_index is not None:
        ranges.append((start_index, previous_index, start_codepoint))

    assert len(ranges) == EXPECTED_RANGES, len(ranges)
    for first, last, codepoint in ranges:
        assert first <= last
        assert codepoint + (last - first) <= 0x10FFFF
    for previous, current in zip(ranges, ranges[1:]):
        assert previous[1] < current[0]
        assert previous[2] + (previous[1] - previous[0]) < current[2]
    return ranges


def format_bytes(name: str, values: list[int] | bytes, per_line: int = 16) -> str:
    lines = [f"static const uint8_t {name}[{len(values)}] = {{"]
    for offset in range(0, len(values), per_line):
        chunk = ", ".join(f"0x{value:02X}" for value in values[offset : offset + per_line])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def format_ranges(ranges: list[tuple[int, int, int]]) -> str:
    lines = [
        f"static const Gb18030Range GB18030_RANGES[{len(ranges)}] = {{",
    ]
    for first, last, codepoint in ranges:
        lines.append(f"    {{{first}u, {last}u, {codepoint}u}},")
    lines.append("};")
    return "\n".join(lines)


def generate() -> tuple[str, str]:
    gb18030_table = decode_two_byte("gb18030")
    lengths, deltas, runs = build_gb18030_two_byte(gb18030_table)
    bitmap, gbk_valid = build_gbk_bitmap(gb18030_table)
    ranges = build_gb18030_ranges()

    header = (
        "// Generated by tools/gen_encoding_tables.py. Do not edit by hand.\n"
        "// GB18030 data: Python codec \"gb18030\" (GB18030-2005 mapping).\n"
        "// GBK validity: Python codec \"gbk\"; GBK is a strict subset of GB18030.\n"
        "\n"
    )
    gb18030_text = (
        header
        + f"// GB18030 two-byte table: {len(runs)} runs, "
        f"{sum(lengths)} entries.\n"
        + f"// GB18030 four-byte table: {len(ranges)} ranges.\n"
        + "\n"
        + format_bytes("GB18030_RUN_LENGTHS", lengths)
        + "\n\n"
        + format_bytes("GB18030_RUN_DELTAS", deltas)
        + "\n\n"
        + format_ranges(ranges)
        + "\n"
    )
    gbk_text = (
        header
        + f"// GBK valid positions: {gbk_valid} / {TABLE_SIZE}.\n"
        + "\n"
        + format_bytes("GBK_VALID_BITS", bitmap)
        + "\n"
    )
    return gb18030_text, gbk_text


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify that the committed files match the generated data",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=root / "src/runtime/data",
        help="directory for the generated .inc files",
    )
    args = parser.parse_args()

    gb18030_text, gbk_text = generate()
    outputs = {
        args.output_dir / "encoding_gb18030_data.inc": gb18030_text,
        args.output_dir / "encoding_gbk_validity.inc": gbk_text,
    }

    if args.check:
        failed = False
        for path, expected in outputs.items():
            try:
                actual = path.read_text(encoding="utf-8")
            except OSError as exc:
                print(f"{path}: {exc}", file=sys.stderr)
                failed = True
                continue
            if actual != expected:
                print(f"{path}: out of date", file=sys.stderr)
                failed = True
        return 1 if failed else 0

    args.output_dir.mkdir(parents=True, exist_ok=True)
    for path, text in outputs.items():
        path.write_text(text, encoding="utf-8")
        print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
