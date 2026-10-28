#!/usr/bin/env python3
"""使用固件同版 cJSON 构建并运行主机逻辑测试。"""
import argparse
import os
from pathlib import Path
import subprocess
import shutil

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--idf-path", type=Path, default=Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")) / "packages/framework-espidf")
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
build = ROOT / ".build" / ("host-sanitize" if args.sanitize else "host")
cmake = shutil.which("cmake") or str(args.idf_path.parent / "tool-cmake/bin/cmake")
ctest = shutil.which("ctest") or str(Path(cmake).with_name("ctest"))
subprocess.run([cmake, "-S", str(ROOT / "test/host"), "-B", str(build),
                f"-DIDF_PATH={args.idf_path.resolve()}",
                f"-DENABLE_SANITIZERS={'ON' if args.sanitize else 'OFF'}"], check=True)
subprocess.run([cmake, "--build", str(build), "--parallel"], check=True)
subprocess.run([ctest, "--test-dir", str(build), "--output-on-failure"], check=True)
