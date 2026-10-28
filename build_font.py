Import("env")
import os
import sys

# 字体作为独立 flash 资源，运行期 mmap 加载
sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "tools"))
from firmware_assets import load_partition

partition = load_partition(env, "font")
font_file = os.path.join(env.subst("$PROJECT_DIR"), "assets", "fonts", "cjk_subset.ttf")
if not os.path.isfile(font_file) or not 0 < os.path.getsize(font_file) <= partition.size:
    raise ValueError("字体文件缺失、为空或超出 font 分区大小: %s" % font_file)
env.Append(FLASH_EXTRA_IMAGES=[(hex(partition.offset), font_file)])
print("字体: %s -> %s" % (font_file, hex(partition.offset)))
