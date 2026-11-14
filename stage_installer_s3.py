from pathlib import Path
import shutil,hashlib,json
root=Path.home()/"ESP32-AutoMesh"
dst=root/"Windows-Installer"
settings=[
    ("esp32c3-supermini-4mb","ESP32-C3",4,"ESP32-C3 SuperMini (GPIO8 active-low LED)", "GPIO8 (active-low)",root),
    ("esp32s3-generic-4mb","ESP32-S3",4,"ESP32-S3 generic USB Serial/JTAG, 4MB flash", "Disabled until LED board pin/type is known",root/"build-targets"/"esp32s3-4mb"),
]
manifest={"schema":2, "version":"0.3.1-dev", "profiles":{}}
for key,chip,mb,label,led,source in settings:
    profile={
        "chip":chip,
        "esptool_chip":chip.lower().replace("-",""),
        "flash_size_bytes":mb*1024*1024,
        "flash_mode":"dio",
        "flash_freq":"80m",
        "board_profile":label,
        "led_profile":led,
        "roles":{},
    }
    for role,appname in (("gateway","provisioner.bin"),("node","onoff_server.bin")):
        build=source/role/"build"
        files={
            "bootloader.bin":build/"bootloader"/"bootloader.bin",
            "partitions.bin":build/"partition_table"/"partition-table.bin",
            "otadata.bin":build/"ota_data_initial.bin",
            "application.bin":build/appname,
        }
        stagedir=dst/"images"/key/role
        stagedir.mkdir(parents=True,exist_ok=True)
        meta={}
        for name,file in files.items():
            assert file.is_file(),f"Missing built {chip}/{role}: {file}"
            target=stagedir/name
            shutil.copyfile(file,target)
            data=target.read_bytes()
            meta[name]={"sha256":hashlib.sha256(data).hexdigest(),"size":len(data)}
        assert meta["application.bin"]["size"] <= 0x1c0000, f"{chip}/{role} exceeds OTA slot"
        profile["roles"][role]={"files":meta}
        print(f"Staged {key}/{role}: {meta['application.bin']['size']} application bytes",flush=True)
    manifest["profiles"][key]=profile
(dst/"manifest.json").write_text(json.dumps(manifest,indent=2,ensure_ascii=False),encoding="utf8")
print("manifest schema2 ready",flush=True)

