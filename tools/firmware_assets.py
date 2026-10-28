"""读取 ESP-IDF 分区表，供资源构建钩子共用。"""
import importlib.util
from pathlib import Path


def load_partition(env, label):
    framework = Path(env.PioPlatform().get_package_dir("framework-espidf"))
    parser_path = framework / "components/partition_table/gen_esp32part.py"
    spec = importlib.util.spec_from_file_location("firmware_partition_table", parser_path)
    parser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(parser)
    # 导入模块不会执行 CLI 的默认值设置；与项目分区表地址保持一致
    parser.offset_part_table = 0x8000
    csv_path = Path(env.subst("$PROJECT_DIR")) / env.BoardConfig().get("build.partitions")
    table = parser.PartitionTable.from_csv(csv_path.read_text(encoding="utf-8"))
    table.verify()
    table.verify_size_fits(8 * 1024 * 1024)
    return table[label]
