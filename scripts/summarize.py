#!/usr/bin/env python3
"""Convert one completed board log into a Markdown comparison table."""
import argparse
import csv
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("log", type=Path)
args = parser.parse_args()
text = args.log.read_text(errors="replace").replace("\r", "")
marker = "S4_ROWTILING,"
if marker not in text:
    parser.error("No board benchmark header found (host logs are not benchmark data)")
text = text[text.rfind(marker):]
version = text.splitlines()[0].strip()
if version not in ("S4_ROWTILING,v0.1", "S4_ROWTILING,v0.2", "S4_ROWTILING,v0.3", "S4_ROWTILING,v0.4"):
    parser.error(f"Unsupported benchmark version: {version}")
if "FAIL," in text or "ERROR," in text or "DONE,PASS" not in text:
    parser.error("Last run is incomplete or failed; no performance conclusion")
if version == "S4_ROWTILING,v0.4":
    if "UNPACK_U16,PASS,inputs=65536" not in text:
        parser.error("Missing 65536-input unpack pass")
    if "CORRECTNESS,PASS,cases=1152,kernels=10" not in text:
        parser.error("Missing complete 10-kernel correctness pass")
elif version == "S4_ROWTILING,v0.3":
    if "UNPACK_U16,PASS,inputs=65536" not in text:
        parser.error("Missing 65536-input unpack pass")
    if "CORRECTNESS,PASS,cases=1152,kernels=8" not in text:
        parser.error("Missing complete 8-kernel correctness pass")
else:
    if "CORRECTNESS,PASS,cases=1152,kernels=4" not in text:
        parser.error("Missing complete correctness pass")

data = {}
for line in text.splitlines():
    start = line.find("RESULT,")
    if start < 0:
        continue
    fields = next(csv.reader([line[start:]]))
    if len(fields) != 9:
        parser.error(f"Malformed result: {line}")
    _, rows, cols, offset, name, minimum, median, maximum, checksum = fields
    minimum, median, maximum = map(int, (minimum, median, maximum))
    if not 0 < minimum <= median <= maximum:
        parser.error(f"Invalid timings: {line}")
    key = (int(rows), int(cols), int(offset))
    group = data.setdefault(key, {})
    if name in group:
        parser.error(f"Duplicate kernel {name} for {key}")
    group[name] = (median, checksum)

if len(data) != 10:
    parser.error(f"Expected 10 shape/offset groups, found {len(data)}")

print("Median DWT CPU cycles; data in SRAM; warmed calls; IRQs masked.")
print(f"Benchmark: {version.split(',')[1]}. Check build_metadata.json for matching optimization levels.\n")

if version == "S4_ROWTILING,v0.4":
    names = (
        "cmsis_stock", "cmsis_u16_or", "tile1",
        "tile2", "tile2_u16_mask", "tile2_u16_or",
        "tile4", "tile4_u16_mask", "tile4_u16_or", "tile4_u16_or_nospill",
    )
    print("Ratios above 1.000x mean the second kernel in the ratio is faster.\n")
    print("| M × K | Offset | stock | cmsis_u16_or | tile2_C | tile4_C | tile4_C_nospill | stock / cmsis_u16_or | stock / tile4_C_nospill | tile2_C / tile4_C_nospill |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for (m, k, offset), group in sorted(data.items()):
        if set(group) != set(names) or len({item[1] for item in group.values()}) != 1:
            parser.error(f"Missing kernels or checksum mismatch: {(m, k, offset)}")
        c = {name: group[name][0] for name in names}
        print(
            f"| {m} × {k} | {offset} | "
            f"{c['cmsis_stock']} | {c['cmsis_u16_or']} | {c['tile2_u16_or']} | "
            f"{c['tile4_u16_or']} | {c['tile4_u16_or_nospill']} | "
            f"{c['cmsis_stock'] / c['cmsis_u16_or']:.3f}× | "
            f"{c['cmsis_stock'] / c['tile4_u16_or_nospill']:.3f}× | "
            f"{c['tile2_u16_or'] / c['tile4_u16_or_nospill']:.3f}× |"
        )
else:
    print("Done.")
