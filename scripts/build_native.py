#!/usr/bin/env python3
"""Build with the active Python ABI; no global install or shell interpolation."""
import os
from pathlib import Path
import subprocess
import sys
import sysconfig
import tempfile

import pybind11

root = Path(__file__).resolve().parents[1]
output = root / "web" / ("_native" + sysconfig.get_config_var("EXT_SUFFIX"))
command = [
    os.environ.get("CXX", "clang++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
    "-Wpedantic", "-pthread", "-shared", "-fPIC",
    "-I" + str(root / "include"), "-I" + pybind11.get_include(),
    "-I" + sysconfig.get_path("include"),
    str(root / "bindings/python.cpp"), str(root / "src/engine.cpp"),
    str(root / "src/storage.cpp"), "-o", str(output),
]
if sys.platform == "darwin":
    command += ["-undefined", "dynamic_lookup"]
print(f"Building {output.name} for Python {sys.version.split()[0]}", flush=True)
(root / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="native-build-", dir=root / "build") as temporary:
    compiled = Path(temporary) / output.name
    command[command.index("-o") + 1] = str(compiled)
    subprocess.run(command, check=True)
    os.replace(compiled, output)
