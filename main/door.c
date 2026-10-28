#include "door.h"
#include "config.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

typedef struct { int channel; int ms; } door_req_t;
static QueueHandle_t s_q;

static void door_task(void *arg) {
    door_req_t r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY)) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, r.channel, 4915);  /* ~90 deg */
            ledc_update_duty(LEDC_LOW_SPEED_MODE, r.channel);
            vTaskDelay(pdMS_TO_TICKS(r.ms));
            ledc_set_duty(LEDC_LOW_SPEED_MODE, r.channel, 3277);  /* ~0 deg */
            ledc_update_duty(LEDC_LOW_SPEED_MODE, r.channel);
        }
    }
}

void door_init(void) {
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_16_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 50, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);

    ledc_channel_config_t c0 = {
        .gpio_num = PIN_SERVO_EN, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0,
        .duty = 3277, .hpoint = 0,
    };
    ledc_channel_config(&c0);

    ledc_channel_config_t c1 = {
        .gpio_num = PIN_SERVO_EX, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_0,
        .duty = 3277, .hpoint = 0,
    };
    ledc_channel_config(&c1);

    s_q = xQueueCreate(4, sizeof(door_req_t));
    xTaskCreate(door_task, "door", 2048, NULL, 4, NULL);
}

void door_open(bool is_entry_door) {
    door_req_t r = {
        .channel = is_entry_door ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1,
        .ms = DOOR_OPEN_MS
    };
    xQueueSend(s_q, &r, 0);
}