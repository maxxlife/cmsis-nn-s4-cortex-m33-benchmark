#!/usr/bin/env python3
"""Convert one completed board log into Markdown comparison tables (SRAM + RRAM)."""
import argparse
import csv
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("log", type=Path)
args = parser.parse_args()
text = args.log.read_text(errors="replace").replace("\r", "")
marker = "S4_ROWTILING,"
if marker not in text:
    parser.error("No board benchmark header found")
text = text[text.rfind(marker):]
if "FAIL," in text or "ERROR," in text or "DONE,PASS" not in text:
    parser.error("Last run is incomplete or failed")

data = {}
rram_data = {}
mem_addr_line = ""

for line in text.splitlines():
    if "MEM_ADDR," in line:
        mem_addr_line = line[line.find("MEM_ADDR,"):].strip()
    start = line.find("RESULT,")
    if start >= 0 and not line.startswith("RRAM_RESULT,"):
        fields = next(csv.reader([line[start:]]))
        if len(fields) == 9:
            _, rows, cols, offset, name, minimum, median, maximum, checksum = fields
            key = (int(rows), int(cols), int(offset))
            data.setdefault(key, {})[name] = (int(median), checksum)
    if line.startswith("RRAM_RESULT,"):
        m = re.match(
            r"RRAM_RESULT,([^,]+),(\d+),(\d+),([^,]+),min=(\d+),med=(\d+),max=(\d+)",
            line.strip(),
        )
        if m:
            mode, rows, cols, name, mn, med, mx = m.groups()
            rram_data.setdefault((mode, int(rows), int(cols)), {})[name] = int(med)

print("## 1. SRAM Benchmark (`DWT->CYCCNT` median cycles, warmed, IRQs masked)\n")
names = (
    "cmsis_stock", "cmsis_u16_or", "tile1",
    "tile2", "tile2_u16_mask", "tile2_u16_or",
    "tile4", "tile4_u16_mask", "tile4_u16_or", "tile4_u16_or_nospill",
)
print("| M × K | Offset | stock | cmsis_u16_or (PR #250) | tile2_C | tile4_C | tile4_C_nospill | stock / cmsis_u16_or | stock / tile4_C_nospill |")
print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
for (m, k, offset), group in sorted(data.items()):
    c = {name: group[name][0] for name in names}
    print(
        f"| {m} × {k} | {offset} | "
        f"{c['cmsis_stock']} | {c['cmsis_u16_or']} | {c['tile2_u16_or']} | "
        f"{c['tile4_u16_or']} | {c['tile4_u16_or_nospill']} | "
        f"{c['cmsis_stock'] / c['cmsis_u16_or']:.3f}× | "
        f"{c['cmsis_stock'] / c['tile4_u16_or_nospill']:.3f}× |"
    )

if rram_data:
    print("\n## 2. RRAM (C-AHB Cache) vs. SRAM Streaming Benchmark (`offset=37`)\n")
    if mem_addr_line:
        print(f"Hardware memory configuration: `{mem_addr_line}`\n")
    print("| M × K | Weights Size | SRAM `stock` | RRAM `stock` | RRAM `cmsis_u16_or` (PR #250) | RRAM `tile2_C` | RRAM `tile4_C_nospill` | RRAM `stock / cmsis_u16_or` | RRAM `stock / tile4_C_nospill` |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for (mode, m, k), rgroup in sorted(rram_data.items()):
        if mode != "default_rram":
            continue
        sram_stock = data[(m, k, 37)]["cmsis_stock"][0]
        w_kb = (m * k) / 2048.0
        print(
            f"| {m} × {k} | {w_kb:.0f} KB | {sram_stock} | "
            f"{rgroup['cmsis_stock']} | {rgroup['cmsis_u16_or']} | "
            f"{rgroup['tile2_u16_or']} | {rgroup['tile4_u16_or_nospill']} | "
            f"{rgroup['cmsis_stock'] / rgroup['cmsis_u16_or']:.3f}× | "
            f"{rgroup['cmsis_stock'] / rgroup['tile4_u16_or_nospill']:.3f}× |"
        )
