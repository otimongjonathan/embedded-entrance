#include "config.h"
#include "state.h"
#include "storage.h"
#include "door.h"
#include "rfid.h"
#include "comms.h"
#include "console.h"
#include "driver/spi_master.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <stdlib.h>
#include <stdio.h>

void app_main(void) {
    spi_bus_config_t bus = {
        .miso_io_num = PIN_MISO, .mosi_io_num = PIN_MOSI,
        .sclk_io_num = PIN_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);

    storage_init();
    state_init();
    door_init();
    rfid_init();
    comms_init();
    console_init();

    rfid_start();
    
    // Wait for WiFi to connect before starting MQTT
    vTaskDelay(pdMS_TO_TICKS(3000)); 
    comms_start();
    
    console_start();

    setenv("TZ", "EAT-3", 1); tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    printf("\n=== CoCIS Entrance Node ===\ntype 'help'\n");
}