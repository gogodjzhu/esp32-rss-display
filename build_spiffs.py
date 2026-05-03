Import("env")
import os
import sys
import subprocess

# Generate SPIFFS image from data/www/ directory
framework_dir = env.PioPlatform().get_package_dir("framework-espidf")
spiffsgen = os.path.join(framework_dir, "components", "spiffs", "spiffsgen.py")
data_dir = os.path.join(env.subst("$PROJECT_DIR"), "data", "www")
build_dir = env.subst("$BUILD_DIR")

os.makedirs(build_dir, exist_ok=True)
os.makedirs(data_dir, exist_ok=True)

output_file = os.path.join(build_dir, "storage.bin")

cmd = [
    sys.executable, spiffsgen,
    "0x80000",  # storage 分区大小，与 partitions.csv 中 storage 大小一致
    data_dir,
    output_file,
    "--page-size", "256",
    "--obj-name-len", "32",
    "--meta-len", "4",
    "--use-magic",
]

print("Generating SPIFFS image from %s" % data_dir)
result = subprocess.run(cmd, capture_output=True, text=True)
if result.returncode != 0:
    print("SPIFFS image generation failed:", result.stderr)
    sys.exit(1)

fsize = os.path.getsize(output_file)
print("SPIFFS image generated: %s (%d bytes)" % (output_file, fsize))

# Register storage.bin for flashing via PlatformIO's FLASH_EXTRA_IMAGES
env.Append(FLASH_EXTRA_IMAGES=[("0x210000", "$BUILD_DIR/storage.bin")])
print("Registered storage.bin at offset 0x210000 for flashing")
