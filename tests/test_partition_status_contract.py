from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_status_reports_live_zigbee_partition_from_flash_table():
    source = (ROOT / "main" / "web_service.c").read_text(encoding="utf-8")

    assert 'esp_partition_find_first(' in source
    assert 'ESP_PARTITION_SUBTYPE_ANY, "zb_storage"' in source
    assert '\\"zigbee_storage_available\\"' in source
    assert '\\"zigbee_storage_offset\\"' in source
    assert '\\"zigbee_storage_size\\"' in source


def test_repository_partition_table_reserves_zigbee_nvs_without_shrinking_ota():
    table = (ROOT / "partitions.csv").read_text(encoding="utf-8")

    assert "ota_0,      app,  ota_0,    0x10000,  0x1e0000" in table
    assert "ota_1,      app,  ota_1,    0x1f0000, 0x1e0000" in table
    assert "capture,    data, 0x40,     0x3e0000, 0x1c000" in table
    assert "zb_storage, data, nvs,      0x3fc000, 0x4000" in table
