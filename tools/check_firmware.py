#!/usr/bin/env python3
"""校验 S3 构建契约、资源尺寸及实际生成的分区表。"""
import argparse
import configparser
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--idf-path", type=Path, default=Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")) / "packages/framework-espidf")
parser.add_argument("--build-dir", type=Path, default=ROOT / ".pio/build/esp32s3")
args = parser.parse_args()
config = configparser.ConfigParser()
config.read(ROOT / "platformio.ini")
assert config["platformio"]["default_envs"] == "esp32s3"
assert [s for s in config.sections() if s.startswith("env:")] == ["env:esp32s3"]
env = config["env:esp32s3"]
assert env["board"] == "esp32-s3-devkitc-1"
assert env["board_build.partitions"] == "partitions_s3.csv"
assert env["build_unflags"] == "-fuse-cxa-atexit"
assert env["lib_ldf_mode"] == "off"
assert 'CONFIG_IDF_TARGET="esp32s3"' in (ROOT / "sdkconfig.defaults").read_text()

# 仅检查项目维护文件；依赖、缓存和生成物不属于源码契约
files = subprocess.check_output(["git", "ls-files", "-co", "--exclude-standard", "-z"], cwd=ROOT).decode().split("\0")
chip = re.compile(r"esp32[-_]?([a-z][0-9])(?=[^a-z0-9]|$)", re.I)
for name in files:
    path = ROOT / name
    if not path.is_file():
        continue
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        continue
    for match in chip.finditer(name + "\n" + text):
        assert match.group(1).lower() == "s3", f"{name}: 非目标芯片标识 {match.group(0)}"

spec = importlib.util.spec_from_file_location("partition_parser", args.idf_path / "components/partition_table/gen_esp32part.py")
partitions = importlib.util.module_from_spec(spec)
spec.loader.exec_module(partitions)
partitions.offset_part_table = 0x8000
expected = partitions.PartitionTable.from_csv((ROOT / env["board_build.partitions"]).read_text())
expected.verify()
expected.verify_size_fits(8 * 1024 * 1024)
assert expected["nvs"].offset == 0x9000
assert expected["factory"].offset == 0x10000 and expected["factory"].size == 0x200000
assert expected["storage"].offset == 0x210000 and expected["storage"].size == 0x80000
assert expected["font"].offset == 0x290000 and expected["font"].size == 0x200000
actual = partitions.PartitionTable.from_binary((args.build_dir / "partitions.bin").read_bytes())
for entry in expected:
    built = actual[entry.name]
    assert (entry.offset, entry.size, entry.type, entry.subtype) == (built.offset, built.size, built.type, built.subtype)
images = [
    ("factory", args.build_dir / "firmware.bin"),
    ("storage", args.build_dir / "storage.bin"),
    ("font", ROOT / "assets/fonts/cjk_subset.ttf"),
]
manifest = []
for label, path in images:
    size = path.stat().st_size
    assert 0 < size <= expected[label].size, f"{label}: 资源超限或为空"
    if label == "storage":
        assert size == expected[label].size
    manifest.append({"partition": label, "offset": hex(expected[label].offset), "bytes": size, "file": str(path)})
project = json.loads((args.build_dir / "project_description.json").read_text())
assert project["target"] == "esp32s3"
components = project["build_components"]
assert "src" in components and "network_worker" in components
# 历史模块已清理；lib 下每个注册组件都必须真正进入构建
for legacy in ("image_fetcher", "rss_reader"):
    assert not (ROOT / "lib" / legacy).exists(), f"历史模块 {legacy} 仍在源码树中"
registered = sorted(p.parent.name for p in (ROOT / "lib").glob("*/CMakeLists.txt"))
missing = [name for name in registered if name not in components]
assert registered and not missing, f"组件未进入构建: {missing}"
print(json.dumps(manifest, indent=2, ensure_ascii=False))
print("PASS: S3 target, partition binary, resource bounds, component boundaries")
