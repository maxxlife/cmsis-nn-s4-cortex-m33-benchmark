#!/usr/bin/env python3
"""Check loop/indexing logic with scalar CMSIS-NN and a software DSP model."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cmsis-nn", type=Path, required=True)
parser.add_argument("--cc", default="cc")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
cmsis = args.cmsis_nn.resolve()
source = cmsis / "Source/NNSupportFunctions/arm_nn_vec_mat_mult_t_s4.c"
if not source.is_file():
    parser.error(f"Missing CMSIS-NN source: {source}")
with tempfile.TemporaryDirectory(prefix="s4-rowtiling-") as tmp:
    exe = Path(tmp) / "host_test"
    command = [args.cc, "-std=c11", "-O2", "-g", "-Wall", "-Wextra",
               "-DBENCH_HOST_TEST", "-fsanitize=address", "-fno-omit-frame-pointer",
               f"-I{root / 'src'}", f"-I{root / 'tests'}", f"-I{cmsis / 'Include'}",
               f"-I{cmsis / 'Source/NNSupportFunctions'}",
               str(root / "src/main.c"), str(root / "src/kernels.c"),
               str(source), "-o", str(exe)]
    subprocess.run(command, check=True)
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    subprocess.run([str(exe)], env=env, check=True)
