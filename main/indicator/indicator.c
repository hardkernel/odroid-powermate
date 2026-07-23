//
// Created by shinys on 25. 7. 29.
//

#include "indicator.h"

#include <led_indicator_ledc.h>

#define LED_STATUS_GPIO CONFIG_GPIO_LED_STATUS
#define LED_WIFI_GPIO CONFIG_GPIO_LED_WIFI

static const blink_step_t slow_blink[] = {
    {LED_BLINK_HOLD, LED_STATE_ON, 500},
    {LED_BLINK_HOLD, LED_STATE_OFF, 500},
    {LED_BLINK_HOLD, LED_STATE_ON, 500},
    {LED_BLINK_HOLD, LED_STATE_OFF, 500},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t fast_blink[] = {
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t double_blink[] = {
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 500},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t triple_blink[] = {
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 100},
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_HOLD, LED_STATE_OFF, 500},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t solid_blink[] = {
    {LED_BLINK_HOLD, LED_STATE_ON, 100},
    {LED_BLINK_LOOP, 0, 0},
};

blink_step_t const* led_mode[] = {
    [BLINK_SLOW] = slow_blink,     [BLINK_FAST] = fast_blink,   [BLINK_DOUBLE] = double_blink,
    [BLINK_TRIPLE] = triple_blink, [BLINK_SOLID] = solid_blink, [BLINK_MAX] = NULL,
};

led_indicator_handle_t led_handle[LED_MAX] = {0};
int recent_type[LED_MAX] = {-1, -1};

void init_led(void)
{
    led_indicator_ledc_config_t ledc_config = {0};
    led_indicator_config_t config = {0};

    ledc_config.is_active_level_high = false;
    ledc_config.timer_inited = false;
    ledc_config.timer_num = LEDC_TIMER_0;
    ledc_config.gpio_num = LED_STATUS_GPIO;
    ledc_config.channel = LEDC_CHANNEL_0;

    config.blink_lists = led_mode;
    config.blink_list_num = BLINK_MAX;

    led_indicator_new_ledc_device(&config, &ledc_config, &led_handle[LED_RED]);

    ledc_config.is_active_level_high = false;
    ledc_config.timer_inited = false;
    ledc_config.timer_num = LEDC_TIMER_0;
    ledc_config.gpio_num = LED_WIFI_GPIO;
    ledc_config.channel = LEDC_CHANNEL_1;

    led_indicator_new_ledc_device(&config, &ledc_config, &led_handle[LED_BLU]);
}

void led_set(enum blink_led led, enum blink_type type)
{
    if (recent_type[led] != -1)
        led_indicator_stop(led_handle[led], recent_type[led]);

    recent_type[led] = type;
    led_indicator_start(led_handle[led], type);
}

void led_off(enum blink_led led)
{
    if (recent_type[led] != -1)
        led_indicator_stop(led_handle[led], recent_type[led]);
    recent_type[led] = -1;
}
