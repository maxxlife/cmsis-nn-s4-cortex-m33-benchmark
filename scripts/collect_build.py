#!/usr/bin/env python3
"""Capture build provenance and assembly without changing the SDK checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path, help="Application build directory or a sysbuild parent with one application")
parser.add_argument("--out", type=Path, default=Path("evidence"))
args = parser.parse_args()
build = args.build.resolve()
if not (build / "compile_commands.json").is_file() or not (build / "zephyr/zephyr.elf").is_file():
    candidates = [path.parent for path in build.glob("*/compile_commands.json")
                  if (path.parent / "zephyr/zephyr.elf").is_file()]
    if len(candidates) == 1:
        build = candidates[0]
    elif len(candidates) > 1:
        parser.error("Multiple application builds found; select one: " + ", ".join(map(str, candidates)))
print(f"Application build: {build}")
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
cache_file = build / "CMakeCache.txt"
commands_file = build / "compile_commands.json"
elf = build / "zephyr/zephyr.elf"
for path in (cache_file, commands_file, elf):
    if not path.is_file():
        parser.error(f"Missing build file: {path}")
cache = {}
for line in cache_file.read_text().splitlines():
    if not line.startswith(("#", "//")) and "=" in line and ":" in line:
        key, value = line.split("=", 1)
        cache[key.split(":", 1)[0]] = value

def capture(command):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return {"command": command, "returncode": result.returncode, "output": result.stdout}

entries = json.loads(commands_file.read_text())
chosen = [entry for entry in entries if Path(entry["file"]).name in
          ("kernels.c", "main.c", "arm_nn_vec_mat_mult_t_s4.c")]
stock = next((entry for entry in chosen if Path(entry["file"]).name == "arm_nn_vec_mat_mult_t_s4.c"), None)
if stock is None:
    parser.error("Stock CMSIS-NN s4 source was not found in compile_commands.json")
optimization = []
for entry in chosen:
    argv = entry.get("arguments") or shlex.split(entry["command"])
    flags = [token for token in argv if re.fullmatch(r"-O(?:[0-9]+|s|z|g|fast)?", token)]
    optimization.append({"file": entry["file"], "ordered_flags": flags,
                         "effective_level": flags[-1] if flags else "implicit default"})
expected_sources = {"main.c", "kernels.c", "arm_nn_vec_mat_mult_t_s4.c"}
levels = {entry["effective_level"] for entry in optimization}
flags_match = (len(optimization) == 3
               and {Path(entry["file"]).name for entry in optimization} == expected_sources
               and len(levels) == 1 and "implicit default" not in levels)
stock_path = Path(stock["file"])
if not stock_path.is_absolute():
    stock_path = (Path(stock["directory"]) / stock_path).resolve()
cmsis = stock_path.parents[2]
metadata = {"commands": chosen, "cmsis_path": str(cmsis),
    "optimization_check": {"status": "PASS" if flags_match else "FAIL", "sources": optimization,
                           "scope": "Last explicit -O level only; inspect full commands for other differences."},
    "cache": {
    key: value for key, value in cache.items() if key in
    ("BOARD", "BOARD_QUALIFIERS", "ZEPHYR_BASE", "ZEPHYR_SDK_INSTALL_DIR",
     "CMAKE_C_COMPILER", "CMAKE_C_FLAGS", "CMAKE_BUILD_TYPE")}}
metadata["cmsis_revision"] = capture(["git", "-C", str(cmsis), "rev-parse", "HEAD"])
metadata["cmsis_tracked_changes"] = capture(["git", "-C", str(cmsis), "status", "--porcelain", "--untracked-files=no"])
metadata["source_sha256"] = {
    str(path.relative_to(cmsis)): hashlib.sha256(path.read_bytes()).hexdigest()
    for path in (stock_path, cmsis / "Include/arm_nnsupportfunctions.h")}
compiler = cache.get("CMAKE_C_COMPILER")
if compiler:
    metadata["compiler_version"] = capture([compiler, "--version"])
    stock_args = stock.get("arguments") or shlex.split(stock["command"])
    preprocessing_args = []
    skip = False
    for token in stock_args[1:]:
        if skip:
            skip = False
            continue
        if token in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
        elif token not in ("-c", "-MD", "-MMD", "-MP"):
            preprocessing_args.append(token)
    result = subprocess.run([stock_args[0], *preprocessing_args, "-dM", "-E"],
                            cwd=stock["directory"], capture_output=True, text=True)
    metadata["stock_feature_macros"] = {
        "returncode": result.returncode,
        "macros": [line for line in result.stdout.splitlines() if any(key in line for key in
                    ("ARM_MATH_DSP", "ARM_MATH_MVEI", "__ARM_FEATURE_DSP", "CMSIS_NN_USE_SINGLE_ROUNDING"))],
        "stderr": result.stderr}
for key, filename, switches in (
    ("CMAKE_OBJDUMP", "disassembly.txt", ["-d", "-S"]),
    ("CMAKE_NM", "symbols.txt", ["-S", "--size-sort"]),
    ("CMAKE_SIZE", "size.txt", []),
):
    tool = cache.get(key)
    if tool:
        result = capture([tool, *switches, str(elf)])
        (out / filename).write_text(result["output"])
        metadata[key] = {"tool": tool, "returncode": result["returncode"]}
    else:
        metadata[key] = {"error": "Tool not recorded in CMakeCache; collect manually"}
shutil.copyfile(build / "zephyr/.config", out / "zephyr.config.txt")
shutil.copyfile(commands_file, out / "compile_commands.json")
(out / "build_metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
print(f"Build evidence written to {out}")
for entry in optimization:
    print(f"{Path(entry['file']).name}: {' '.join(entry['ordered_flags'])} -> {entry['effective_level']}")
if not flags_match:
    print("FLAGS,FAIL: optimization levels differ or required source commands are missing/ambiguous.", file=sys.stderr)
    print("Evidence was saved, but repeat the benchmark after fixing the compile flags.", file=sys.stderr)
    sys.exit(2)
print(f"FLAGS,PASS: all three sources end with {next(iter(levels))}")
print("Check stock_feature_macros for ARM_MATH_DSP=1 and inspect disassembly for spills.")
