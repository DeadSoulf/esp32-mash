from pathlib import Path
root=Path.home()/"ESP32-AutoMesh"
for role in ("gateway","node"):
    path=root/role
    p=path/"sdkconfig.defaults"
    s=p.read_text(encoding="utf-8")
    if 'CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y' not in s:
        s += '\nCONFIG_ESPTOOLPY_FLASHSIZE_4MB=y\n'
        p.write_text(s,encoding="utf-8")
    p=path/"sdkconfig"
    if p.is_file():
        s=p.read_text(encoding="utf-8")
        s=s.replace("CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y","# CONFIG_ESPTOOLPY_FLASHSIZE_2MB is not set")
        s=s.replace("# CONFIG_ESPTOOLPY_FLASHSIZE_4MB is not set","CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y")
        s=s.replace('CONFIG_ESPTOOLPY_FLASHSIZE="2MB"','CONFIG_ESPTOOLPY_FLASHSIZE="4MB"')
        p.write_text(s,encoding="utf-8")
print("Flash size configured for 4MB ESP32-C3")

