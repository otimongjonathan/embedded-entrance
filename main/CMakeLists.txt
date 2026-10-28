idf_component_register(
    SRCS
        "main.c"
        "rfid.c"
        "door.c"
        "state.c"
        "storage.c"
        "comms.c"
        "console.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver esp_wifi esp_netif esp_event
        nvs_flash mqtt json
        fatfs sdmmc
)
