#!/usr/bin/env python3
"""使用固件同版 cJSON 构建并运行主机逻辑测试。"""
import argparse
import os
from pathlib import Path
import subprocess
import shutil

ROOT = Path(__file__).resolve().parents[1]
CORE_DIR = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))

def _default_idf_path() -> Path:
    """按项目固定的平台版本定位同版 ESP-IDF 框架包。

    直接取 packages/framework-espidf 可能解析到本机残留的其他分支框架
    （例如新平台遗留的目录），与本项目实际编译使用的 IDF 版本不一致。
    因此解析 platformio.ini 的平台版本，读取其 platform.json 声明的
    框架版本号，优先命中带版本后缀的安装目录，未命中再退回裸目录。
    """
    pio_ini = ROOT / "platformio.ini"
    platform_line = next(
        (line.split("=", 1)[1].strip() for line in pio_ini.read_text().splitlines()
         if line.strip().startswith("platform ")), None)
    if platform_line:
        name, _, spec = platform_line.partition("@")
        platform_dir = CORE_DIR / "platforms" / f"{name.strip()}@{spec.strip()}"
        platform_json = platform_dir / "platform.json"
        if platform_json.is_file():
            import json
            fw = json.loads(platform_json.read_text()).get("packages", {}).get("framework-espidf", {})
            want = str(fw.get("version", "")).lstrip("~^>=< ")
            base = want.split(".")[0] if want else ""
            if base:
                candidate = CORE_DIR / "packages" / f"framework-espidf@{want}"
                if candidate.is_dir():
                    return candidate
    return CORE_DIR / "packages" / "framework-espidf"

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--idf-path", type=Path, default=_default_idf_path())
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
