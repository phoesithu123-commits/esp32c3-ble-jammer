import os
Import("env")

def merge_bin_action(source, target, env):
    APP_BIN = "$BUILD_DIR/${PROGNAME}.bin"
    MERGED_BIN = "$BUILD_DIR/merged-firmware.bin"
    
    board_config = env.BoardConfig()
    mcu = board_config.get("build.mcu", "esp32c3")
    flash_mode = board_config.get("build.flash_mode", "dio")
    flash_freq = board_config.get("build.f_cpu", "160000000L")
    flash_size = board_config.get("upload.flash_size", "4MB")
    
    # ESP32-C3 flash offsets
    flash_images = [
        "0x1000", "$BUILD_DIR/bootloader.bin",
        "0x8000", "$BUILD_DIR/partitions.bin",
        "0xe000", "$BUILD_DIR/boot_app0.bin",
        "0x10000", APP_BIN
    ]
    
    cmd = (
        f"\"$PYTHONEXE\" \"$OBJCOPY\" --chip {mcu} merge_bin"
        f" -o {MERGED_BIN}"
        f" --flash_mode {flash_mode}"
        f" --flash_size {flash_size}"
        f" {' '.join(flash_images)}"
    )
    
    print(f"Merging firmware: {cmd}")
    env.Execute(cmd)
    
    # Also copy individual files
    import shutil
    build_dir = env.subst("$BUILD_DIR")
    output_dir = os.path.join(build_dir, "firmware_output")
    os.makedirs(output_dir, exist_ok=True)
    
    for f in ["bootloader.bin", "partitions.bin", "boot_app0.bin", "${PROGNAME}.bin", "merged-firmware.bin"]:
        src = env.subst(f"$BUILD_DIR/{f}")
        if os.path.exists(src):
            shutil.copy(src, output_dir)
            print(f"Copied {src} -> {output_dir}")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bin_action)
