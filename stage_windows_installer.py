from pathlib import Path
import hashlib,json,shutil
root=Path.home()/"ESP32-AutoMesh"
out=root/"Windows-Installer"
images=out/"images"/"esp32c3-supermini-4mb"
manifest={
    "schema":1,
    "version":"0.3.0-dev",
    "compatible_chips":["ESP32-C3"],
    "flash_size_bytes":4194304,
    "board_profile":"ESP32-C3 SuperMini with GPIO8 active-low LED",
    "roles":{}
}
names={"gateway":"provisioner.bin","node":"onoff_server.bin"}
for role,name in names.items():
    src=root/role/"build"
    dest=images/role
    dest.mkdir(parents=True,exist_ok=True)
    filelist={
       "bootloader.bin":src/"bootloader"/"bootloader.bin",
       "partitions.bin":src/"partition_table"/"partition-table.bin",
       "otadata.bin":src/"ota_data_initial.bin",
       "application.bin":src/name
    }
    filemap={}
    for outname,source in filelist.items():
        assert source.is_file(),str(source)
        target=dest/outname
        shutil.copy2(source,target)
        filemap[outname]={"size":target.stat().st_size,
                          "sha256":hashlib.sha256(target.read_bytes()).hexdigest()}
    manifest["roles"][role]={"files":filemap}
(out/"manifest.json").write_text(json.dumps(manifest,indent=2),encoding="utf-8")
print("Release bundle staged",out,flush=True)
for role in names:
 print(role,manifest["roles"][role]["files"]["application.bin"]["size"],flush=True)

