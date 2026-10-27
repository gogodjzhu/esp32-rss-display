Import("env")
import os
import re

# 把中文子集字体烧录到专用 font 分区。
#
# 字体文件不参与 app 二进制链接，而是作为独立的 flash 镜像写入 font 分区，
# 运行期由 lib/cjk_font 用 esp_partition_mmap 直接映射（零拷贝、不占 RAM）。
# 分区偏移从 partitions_s3.csv 中的 font 行解析，避免与分区表脱节。

project_dir = env.subst("$PROJECT_DIR")
partition_csv = os.path.join(project_dir, "partitions_s3.csv")
font_file = os.path.join(project_dir, "assets", "fonts", "cjk_subset.ttf")

if not os.path.isfile(font_file):
    print("build_font: 找不到字体文件 %s" % font_file)
    env.Exit(1)

offset = None
with open(partition_csv, "r", encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [x.strip() for x in line.split(",")]
        if fields[0] == "font":
            # 期望显式给出偏移，如 0x290000
            m = re.match(r"^0[xX][0-9a-fA-F]+$", fields[3]) if len(fields) > 3 else None
            if m:
                offset = fields[3]
            break

if offset is None:
    print("build_font: 未能在 %s 中解析 font 分区偏移" % partition_csv)
    env.Exit(1)

env.Append(FLASH_EXTRA_IMAGES=[(offset, font_file)])
print("build_font: 注册字体 %s 于偏移 %s" % (font_file, offset))
