from pathlib import Path

root = Path.home() / "ESP32-AutoMesh"
for role in ("gateway", "node"):
    path = root / role
    defaults = path / "sdkconfig.defaults"
    s = defaults.read_text(encoding="utf-8")
    assert 'CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y' in s, role
    s = s.replace('CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y', '''# AutoMesh: 2 OTA app slots, no factory app
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y''')
    defaults.write_text(s, encoding="utf-8")

    main = path / "main" / "main.c"
    code = main.read_text(encoding="utf-8")
    assert '#include "nvs_flash.h"' in code
    code = code.replace('#include "nvs_flash.h"', '#include "nvs_flash.h"\n#include "esp_ota_ops.h"', 1)
    assert 'void app_main(void)' in code
    routine = '''static void automesh_mark_boot_healthy(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (p && esp_ota_get_state_partition(p, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) ESP_LOGE(TAG, "Failed to accept OTA app: %s", esp_err_to_name(err));
    }
}

'''
    code = code.replace('void app_main(void)', routine + 'void app_main(void)', 1)
    if role == "gateway":
        needle = '    automesh_bridge_start();'
        assert needle in code
        code = code.replace(needle, '    automesh_mark_boot_healthy();\n' + needle, 1)
    else:
        needle = '''    if (err) {
        ESP_LOGE(TAG, "Bluetooth mesh init failed (err %d)", err);
    }
}'''
        assert needle in code
        code = code.replace(needle, '''    if (err) {
        ESP_LOGE(TAG, "Bluetooth mesh init failed (err %d)", err);
        return;
    }
    automesh_mark_boot_healthy();
}''', 1)
    main.write_text(code, encoding="utf-8")

    sdk = path / "sdkconfig"
    if sdk.is_file():
        s = sdk.read_text(encoding="utf-8")
        s = s.replace('CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y',
                      '# CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE is not set')
        s = s.replace('# CONFIG_PARTITION_TABLE_CUSTOM is not set',
                      'CONFIG_PARTITION_TABLE_CUSTOM=y')
        s = s.replace('CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp_large.csv"',
                      'CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"')
        s = s.replace('# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set',
                      'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y')
        sdk.write_text(s, encoding="utf-8")

print("Custom 4MB OTA partitions and rollback health marker enabled")

