from pathlib import Path
import shutil
root=Path.home()/"ESP32-AutoMesh"
variants=root/"build-targets"
for role in ("node","gateway"):
    src=root/role
    dst=variants/"esp32s3-4mb"/role
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src,dst,ignore=shutil.ignore_patterns("build","sdkconfig","sdkconfig.old",".git","__pycache__","managed_components"))
    print("Created",dst)
    # Explicitly define target to avoid accidental cross-target reuse.
    defs=dst/"sdkconfig.defaults"
    data=defs.read_text(encoding="utf-8")
    data=data.replace("CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y","CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y")
    data+="\n# Separate S3 target, BLE Mesh, 4MB flash, no PSRAM required\nCONFIG_IDF_TARGET_ESP32S3=y\n"
    defs.write_text(data,encoding="utf-8")
    # For unknown ESP32-S3 boards do not toggle GPIO47 or other candidate pins.
    if role=="node":
        board=dst/"main"/"board.c"
        s=board.read_text(encoding="utf-8")
        s=s.replace("#if CONFIG_IDF_TARGET_ESP32C3\n    // GPIO8", "#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3\n    // GPIO8")
        s=s.replace("#if !CONFIG_IDF_TARGET_ESP32C3\n    board_led_init();", "#if !CONFIG_IDF_TARGET_ESP32C3 && !CONFIG_IDF_TARGET_ESP32S3\n    board_led_init();")
        board.write_text(s,encoding="utf-8")
        # Guard duplicate legacy demo LED writes in generic S3 model.
        print("S3 node uses generic no-LED profile; no assumptions about board pins")
print("Prepared build-targets/esp32s3-4mb")

