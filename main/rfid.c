#include "rfid.h"
#include "config.h"
#include "state.h"
#include "door.h"
#include "storage.h"
#include "comms.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>

static const char *TAG = "rfid";

#define REG_COMMAND      0x01
#define REG_COM_IRQ      0x04
#define REG_ERROR        0x06
#define REG_FIFO_DATA    0x09
#define REG_FIFO_LEVEL   0x0A
#define REG_BIT_FRAMING  0x0D
#define REG_MODE         0x11
#define REG_TX_CONTROL   0x14
#define REG_TX_ASK       0x15
#define REG_T_MODE       0x2A
#define REG_T_PRESCALER  0x2B
#define REG_T_RELOAD_H   0x2C
#define REG_T_RELOAD_L   0x2D
#define CMD_IDLE         0x00
#define CMD_TRANSCEIVE   0x0C
#define CMD_RESET        0x0F
#define PICC_REQIDL      0x26
#define PICC_ANTICOLL1   0x93

typedef struct {
    spi_device_handle_t spi;
    gpio_num_t          rst;
    uint32_t            last_tag;
} rc522_t;

static rc522_t s_en, s_ex;

static void rc522_w(rc522_t *d, uint8_t r, uint8_t v) {
    uint8_t tx[2] = { (uint8_t)((r << 1) & 0x7E), v };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    spi_device_transmit(d->spi, &t);
}
static uint8_t rc522_r(rc522_t *d, uint8_t r) {
    uint8_t tx[2] = { (uint8_t)(((r << 1) & 0x7E) | 0x80), 0 }, rx[2] = {0};
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_transmit(d->spi, &t);
    return rx[1];
}
static esp_err_t rc522_read_fifo(rc522_t *d, uint8_t *buf, int count) {
    uint8_t tx[19] = {0}, rx[19] = {0};
    if (count < 1 || count > 18) return ESP_ERR_INVALID_SIZE;
    tx[0] = (uint8_t)(((REG_FIFO_DATA << 1) & 0x7E) | 0x80);
    spi_transaction_t t = {
        .length = (size_t)(8 * (count + 1)),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_transmit(d->spi, &t);
    if (err == ESP_OK) memcpy(buf, &rx[1], (size_t)count);
    return err;
}
static void rc522_init(rc522_t *d, spi_device_handle_t spi, gpio_num_t rst) {
    d->spi = spi; d->rst = rst; d->last_tag = 0;
    gpio_set_direction(rst, GPIO_MODE_OUTPUT);
    gpio_set_level(rst, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(rst, 1); vTaskDelay(pdMS_TO_TICKS(50));
    rc522_w(d, REG_COMMAND, CMD_RESET); vTaskDelay(pdMS_TO_TICKS(50));
    
    // --- DIAGNOSTIC: Read the version register ---
    uint8_t ver = rc522_r(d, 0x37);
    ESP_LOGW(TAG, "RC522 on GPIO %d: version 0x%02X (expected 0x91 or 0x92)",
             (int)rst, ver);
    // ---------------------------------------------

    rc522_w(d, REG_T_MODE,      0x8D);
    rc522_w(d, REG_T_PRESCALER, 0x3E);
    rc522_w(d, REG_T_RELOAD_H,  30);
    rc522_w(d, REG_T_RELOAD_L,  0);
    rc522_w(d, REG_TX_ASK,      0x40);
    rc522_w(d, REG_MODE,        0x3D);
    uint8_t v = rc522_r(d, REG_TX_CONTROL);
    if (!(v & 0x03)) v |= 0x03;
    rc522_w(d, REG_TX_CONTROL, v);
}
static int transceive(rc522_t *d, uint8_t *tx, int txlen, uint8_t fr,
                      uint8_t *rx, int *rxlen, int wait_ms) {
    rc522_w(d, REG_COMMAND,    CMD_IDLE);
    rc522_w(d, REG_COM_IRQ,    0x7F);
    rc522_w(d, REG_FIFO_LEVEL, 0x80);
    for (int i = 0; i < txlen; i++) rc522_w(d, REG_FIFO_DATA, tx[i]);
    rc522_w(d, REG_COMMAND,    CMD_TRANSCEIVE);
    rc522_w(d, REG_BIT_FRAMING, fr | 0x80);
    int irq = 0;
    TickType_t t0 = xTaskGetTickCount();
    while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(wait_ms)) {
        irq = rc522_r(d, REG_COM_IRQ);
        if (irq & 0x30) break;
        vTaskDelay(1);
    }
    rc522_w(d, REG_BIT_FRAMING, 0x00);
    if (!(irq & 0x30)) return -1;
    if (rc522_r(d, REG_ERROR) & 0x1B) return -2;
    int n = rc522_r(d, REG_FIFO_LEVEL);
    if (rx && rxlen) {
        int cap = *rxlen;
        int read_count = n < cap ? n : cap;
        if (rc522_read_fifo(d, rx, read_count) != ESP_OK) return -3;
        *rxlen = n;
    }
    return 0;
}
static int read_uid(rc522_t *d, uint8_t uid[4]) {
    uint8_t req = PICC_REQIDL, atqa[2]; int an = sizeof(atqa);
    if (transceive(d, &req, 1, 0x07, atqa, &an, 40) != 0 || an != 2) return -1;
    uint8_t ac[2] = { PICC_ANTICOLL1, 0x20 };
    uint8_t buf[5]; int bn = sizeof(buf);
    if (transceive(d, ac, 2, 0x00, buf, &bn, 40) != 0 || bn != 5) return -2;
    uint8_t checksum = buf[0] ^ buf[1] ^ buf[2] ^ buf[3];
    if (checksum != buf[4]) {
        ESP_LOGE(TAG, "UID checksum mismatch: %02X:%02X:%02X:%02X "
                 "(received %02X, expected %02X)",
                 buf[0], buf[1], buf[2], buf[3], buf[4], checksum);
        return -3;
    }
    memcpy(uid, buf, 4);
    return 0;
}
static uint32_t uid_to_u32(const uint8_t u[4]) {
    return ((uint32_t)u[0] << 24) | ((uint32_t)u[1] << 16) |
           ((uint32_t)u[2] << 8)  |  (uint32_t)u[3];
}

static void feedback(bool ok, bool after_hours) {
    gpio_set_level(PIN_LED_GREEN, ok ? 1 : 0);
    gpio_set_level(PIN_LED_RED,   ok ? 0 : 1);
    gpio_set_level(PIN_BUZZER, 1);
    vTaskDelay(pdMS_TO_TICKS(ok ? (after_hours ? 200 : 80) : 500));
    gpio_set_level(PIN_BUZZER, 0);
    gpio_set_level(PIN_LED_GREEN, 0);
    gpio_set_level(PIN_LED_RED, 0);
}

static void handle_scan(rc522_t *d, bool is_entry) {
    if (!d) return;
    uint8_t uid[4];
    
    int res = read_uid(d, uid);
    if (res != 0) {
        if (res == -1) d->last_tag = 0;
        return;
    }
    
    uint32_t tag = uid_to_u32(uid);

    if (tag == d->last_tag) return;
    d->last_tag = tag;

    time_t t_now = time(NULL);
    result_t r = state_evaluate(tag, is_entry, t_now);
    bool ok = (r == RES_OK || r == RES_OK_AFTER_HOURS);
    if (ok) door_open(is_entry);
    feedback(ok, r == RES_OK_AFTER_HOURS);

    storage_log_event(t_now, tag, is_entry, state_result_str(r));
    comms_publish_event(tag, is_entry, state_result_str(r));
    comms_publish_occupancy(state_occupancy());
    state_persist();

    ESP_LOGW(TAG, "RFID card %08lX on %s reader: %s",
             (unsigned long)tag, is_entry ? "entry" : "exit",
             state_result_str(r));
}

static void rfid_task(void *arg) {
    for (;;) {
        handle_scan(&s_en, true);
        handle_scan(&s_ex, false);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void rfid_init(void) {
    spi_device_interface_config_t dcfg = {
        // Lowered SPI speed to 1MHz to match Wokwi's simulator exactly
        .clock_speed_hz = 1 * 1000 * 1000, .mode = 0, .queue_size = 1,
    };
    spi_device_handle_t s;

    dcfg.spics_io_num = PIN_RFID_EN_CS;
    esp_err_t err = spi_bus_add_device(SPI3_HOST, &dcfg, &s);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Entry reader SPI setup failed: %s", esp_err_to_name(err));
        return;
    }
    rc522_init(&s_en, s, PIN_RFID_EN_RST);

    dcfg.spics_io_num = PIN_RFID_EX_CS;
    err = spi_bus_add_device(SPI3_HOST, &dcfg, &s);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Exit reader SPI setup failed: %s", esp_err_to_name(err));
        return;
    }
    rc522_init(&s_ex, s, PIN_RFID_EX_RST);

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_LED_GREEN) | (1ULL << PIN_LED_RED) |
                        (1ULL << PIN_BUZZER),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
}

void rfid_start(void) {
    xTaskCreate(rfid_task, "rfid", 8192, NULL, 6, NULL);
}