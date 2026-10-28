Import("env")
import os
import sys
import subprocess

# 使用官方分区解析器，自动处理表中省略的地址和对齐
sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "tools"))
from firmware_assets import load_partition

partition = load_partition(env, "storage")
framework_dir = env.PioPlatform().get_package_dir("framework-espidf")
spiffsgen = os.path.join(framework_dir, "components", "spiffs", "spiffsgen.py")
data_dir = os.path.join(env.subst("$PROJECT_DIR"), "data", "www")
build_dir = env.subst("$BUILD_DIR")
os.makedirs(build_dir, exist_ok=True)
output_file = os.path.join(build_dir, "storage.bin")
cmd = [sys.executable, spiffsgen, str(partition.size), data_dir, output_file,
       "--page-size", "256", "--obj-name-len", "32", "--meta-len", "4", "--use-magic"]
subprocess.run(cmd, check=True)
env.Append(FLASH_EXTRA_IMAGES=[(hex(partition.offset), output_file)])
print("SPIFFS: %s -> %s (%d bytes)" % (data_dir, hex(partition.offset), partition.size))
