/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

//
// Created by vl011 on 2025-08-28.
//

#include "sw.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event.h"
#include "nconfig.h"
#include "pb.h"
#include "pb_encode.h"
#include "pbmsg.h"
#include "pca9557.h"
#include "status.pb.h"
#include "webserver.h"

#define PCA9557_ADDR 0x18

#define GPIO_MAIN CONFIG_EXPANDER_GPIO_SW_12V
#define GPIO_USB CONFIG_EXPANDER_GPIO_SW_5V
#define GPIO_PWR CONFIG_EXPANDER_GPIO_TRIGGER_POWER
#define GPIO_RST CONFIG_EXPANDER_GPIO_TRIGGER_RESET

#define POWER_DELAY (CONFIG_TRIGGER_POWER_DELAY_MS * 1000)
#define RESET_DELAY (CONFIG_TRIGGER_RESET_DELAY_MS * 1000)

static const char* TAG = "control";

// Cached output levels as SW_MAIN | SW_USB bits. Changed only with switch_mutex held, so the
// compare, the expander write and the cache update are one step: a cut-off can neither be
// skipped as a no-op nor be undone by a request that read the cache before it. Read without
// the lock in one load, so main and usb always come from the same moment.
static atomic_uint load_switch_outputs;

static unsigned load_outputs(void)
{
    return atomic_load_explicit(&load_switch_outputs, memory_order_acquire);
}

static void store_outputs(unsigned outputs)
{
    atomic_store_explicit(&load_switch_outputs, outputs, memory_order_release);
}

// Outputs a protective cut could not confirm off, which no confirmed write or resync of that
// output has settled since: retry_load_switch_cuts() cuts only these, once (it clears them
// whatever its result). Guarded by switch_mutex, so a request that switches an output in the
// meantime always wins over a stale retry.
static unsigned cut_owed;
// Outputs switched on (a confirmed write, or a resync that read them on) and not written off
// again since, set next to each cache store and taken by the warning monitor in one exchange: the
// one record of switch-ons, so each is seen once, even an off-then-on between two of its status
// reads, and one a cut has already turned off re-arms nothing.
static atomic_uint switched_on;

// Lock order: switch_mutex, then expander_mutex, then the PCA9557 driver's own lock.
// persist_mutex is never held together with the others: a cut-off never waits for NVS.
static SemaphoreHandle_t switch_mutex;   // load switch changes: compare, expander write, cache
static SemaphoreHandle_t expander_mutex; // expander output writes and the PWR/RST press state
static SemaphoreHandle_t persist_mutex;  // saving the output state, in order
#define MUTEX_TIMEOUT (pdMS_TO_TICKS(100))
#define SWITCH_LOCK_TIMEOUT (pdMS_TO_TICKS(1000))
#define PROTECTIVE_WRITE_ATTEMPTS 3

// A PWR or RST "button press": the line is held low for hold_us, then released.
typedef struct
{
    const char* name;
    uint8_t pin;
    uint32_t hold_us;
    esp_timer_handle_t timer;
    volatile bool pressed; // set and cleared with expander_mutex held
    int release_attempts;  // used by the release callback only
} trigger_t;

static trigger_t power_trigger = {.name = "power", .pin = GPIO_PWR, .hold_us = POWER_DELAY};
static trigger_t reset_trigger = {.name = "reset", .pin = GPIO_RST, .hold_us = RESET_DELAY};
#define TRIGGER_RELEASE_RETRY_US 20000
#define TRIGGER_RELEASE_ATTEMPTS 10

bool get_restore_output_state()
{
    char value[6];
    return nconfig_read(RESTORE_OUTPUT_STATE, value, sizeof(value)) == ESP_OK && strcmp(value, "true") == 0;
}

// With persist_mutex held. Reads the cache as it is now, so whichever save runs last
// writes the latest state.
static esp_err_t write_output_state(void)
{
    unsigned outputs = load_outputs();
    char state[] = {
        (outputs & SW_MAIN) ? '1' : '0',
        (outputs & SW_USB) ? '1' : '0',
        '\0',
    };
    return nconfig_write(OUTPUT_STATE, state);
}

esp_err_t persist_load_switch_state()
{
    if (!persist_mutex)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(persist_mutex, portMAX_DELAY);
    esp_err_t err = get_restore_output_state() ? write_output_state() : ESP_OK;
    xSemaphoreGive(persist_mutex);
    return err;
}

esp_err_t set_restore_output_state(bool enabled)
{
    if (!persist_mutex)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(persist_mutex, SWITCH_LOCK_TIMEOUT) == pdFALSE)
        return ESP_ERR_TIMEOUT;
    esp_err_t err = enabled ? write_output_state() : ESP_OK;
    if (err == ESP_OK)
        err = nconfig_write(RESTORE_OUTPUT_STATE, enabled ? "true" : "false");
    xSemaphoreGive(persist_mutex);
    return err;
}

static void restore_load_switch_state()
{
    if (!get_restore_output_state())
        return;

    char state[3];
    esp_err_t err = nconfig_read(OUTPUT_STATE, state, sizeof(state));
    if (err != ESP_OK || strlen(state) != 2 ||
        (state[0] != '0' && state[0] != '1') || (state[1] != '0' && state[1] != '1'))
    {
        ESP_LOGW(TAG, "Failed to read saved load switch state");
        return;
    }

    err = set_load_switches(state[0] == '1', state[1] == '1');
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Failed to restore load switch state: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "Restored load switches: main=%s usb=%s",
                 state[0] == '1' ? "on" : "off", state[1] == '1' ? "on" : "off");
}

size_t encode_load_switch_status(uint8_t* buf, size_t size)
{
    StatusMessage message = StatusMessage_init_zero;
    message.which_payload = StatusMessage_sw_status_tag;
    LoadSwStatus* sw_status = &message.payload.sw_status;

    unsigned outputs = load_outputs();
    sw_status->main = (outputs & SW_MAIN) != 0;
    sw_status->usb = (outputs & SW_USB) != 0;

    return encode_pb_message(StatusMessage_fields, &message, buf, size);
}

// Reads the outputs back after the expander was reset (boot, critical cut-off).
esp_err_t sync_load_switch_status_from_hw()
{
    if (!switch_mutex)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(switch_mutex, portMAX_DELAY);
    unsigned was = load_outputs();
    unsigned outputs = was;
    bool level = false;
    esp_err_t err = pca9557_get_level(CONFIG_EXPANDER_GPIO_SW_12V, &level);
    if (err == ESP_OK)
    {
        outputs = level ? outputs | SW_MAIN : outputs & ~SW_MAIN;
        err = pca9557_get_level(CONFIG_EXPANDER_GPIO_SW_5V, &level);
        if (err == ESP_OK)
            outputs = level ? outputs | SW_USB : outputs & ~SW_USB;
    }
    store_outputs(outputs);
    if (outputs & ~was)
        atomic_fetch_or(&switched_on, outputs & ~was);
    cut_owed &= outputs; // an output read off needs no cut
    xSemaphoreGive(switch_mutex);
    if (err != ESP_OK)
        return err;

    ws_publish_switch_state();
    return ESP_OK;
}

struct cut_result
{
    unsigned tried;  // the outputs written
    unsigned failed; // of those, not confirmed off (still on in the cache)
};

/**
 * Sets the outputs in mask (SW_MAIN, SW_USB); the others keep their level. A protective
 * change (a cut-off) waits for the locks instead of timing out, writes even when the cache
 * already reads off, and retries a failed write. owed_only (protective): only the outputs in
 * mask that cut_owed still holds. *res (may be NULL): what a protective change wrote.
 */
static esp_err_t change_outputs(unsigned mask, bool main_on, bool usb_on, bool protective, bool owed_only,
                                struct cut_result* res)
{
    mask &= SW_MAIN | SW_USB;
    if (res)
        *res = (struct cut_result){.tried = mask, .failed = mask}; // until a write is confirmed
    if (!switch_mutex)
        return ESP_ERR_INVALID_STATE; // init_sw() has not run
    if (xSemaphoreTake(switch_mutex, protective ? portMAX_DELAY : SWITCH_LOCK_TIMEOUT) == pdFALSE)
    {
        ESP_LOGW(TAG, "Control error: load switches busy");
        return ESP_ERR_TIMEOUT;
    }

    if (owed_only)
    {
        mask &= cut_owed;
        if (res)
            *res = (struct cut_result){.tried = mask, .failed = mask};
    }
    unsigned was = load_outputs();
    unsigned cur = was;
    unsigned confirmed = 0; // outputs whose write was acknowledged
    bool write_main = (mask & SW_MAIN) && (protective || main_on != ((was & SW_MAIN) != 0));
    bool write_usb = (mask & SW_USB) && (protective || usb_on != ((was & SW_USB) != 0));
    esp_err_t main_err = ESP_OK;
    esp_err_t usb_err = ESP_OK;

    if (write_main || write_usb)
    {
        if (xSemaphoreTake(expander_mutex, protective ? portMAX_DELAY : MUTEX_TIMEOUT) == pdFALSE)
        {
            xSemaphoreGive(switch_mutex);
            ESP_LOGW(TAG, "Control error: expander busy");
            return ESP_ERR_TIMEOUT;
        }
        for (int attempt = 0; attempt < (protective ? PROTECTIVE_WRITE_ATTEMPTS : 1); ++attempt)
        {
            if (write_main)
            {
                main_err = pca9557_set_level(GPIO_MAIN, main_on);
                if (main_err == ESP_OK)
                {
                    confirmed |= SW_MAIN;
                    // One store per write, so the word matches the hardware between the two.
                    cur = (cur & ~SW_MAIN) | (main_on ? SW_MAIN : 0);
                    store_outputs(cur);
                    if (main_on) // a write is a level change, so on means off-to-on
                        atomic_fetch_or(&switched_on, SW_MAIN);
                    else // switched off again (a cut included): no re-arm is owed any more
                        atomic_fetch_and(&switched_on, ~SW_MAIN);
                    write_main = false;
                }
            }
            if (write_usb)
            {
                usb_err = pca9557_set_level(GPIO_USB, usb_on);
                if (usb_err == ESP_OK)
                {
                    confirmed |= SW_USB;
                    cur = (cur & ~SW_USB) | (usb_on ? SW_USB : 0);
                    store_outputs(cur);
                    if (usb_on)
                        atomic_fetch_or(&switched_on, SW_USB);
                    else
                        atomic_fetch_and(&switched_on, ~SW_USB);
                    write_usb = false;
                }
            }
            if (!write_main && !write_usb)
                break;
        }
        xSemaphoreGive(expander_mutex);
    }
    // A failed write leaves its cache bit as it was: a cut owes only outputs still on. The retry
    // (owed_only) is final: what it tried is owed no more, whatever its result.
    unsigned not_off = protective ? ((write_main ? SW_MAIN : 0) | (write_usb ? SW_USB : 0)) & cur : 0;
    cut_owed = owed_only ? (cut_owed & ~mask) : ((cut_owed & ~confirmed) | not_off);
    if (res)
        res->failed = not_off;

    unsigned now = cur;
    bool changed = now != was;
    xSemaphoreGive(switch_mutex);
    // Every call that gets here publishes, even with no change: a client is sent the outputs
    // as they are when its frame is written, so this need not wait for the save or the events.
    ws_publish_switch_state();
    esp_err_t persist_err = changed ? persist_load_switch_state() : ESP_OK;

    if (main_err != ESP_OK || usb_err != ESP_OK)
        ESP_LOGW(TAG, "Failed to set load switches: main=%s usb=%s", esp_err_to_name(main_err),
                 esp_err_to_name(usb_err));
    if (persist_err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to save load switch state: %s", esp_err_to_name(persist_err));
        push_eventf(EV_WARNING, "failed to save load switch state: %s", esp_err_to_name(persist_err));
    }
    if (changed)
        push_eventf(EV_INFO, "load switches set: main=%s usb=%s", (now & SW_MAIN) ? "on" : "off",
                    (now & SW_USB) ? "on" : "off");
    return main_err != ESP_OK ? main_err : usb_err;
}

esp_err_t set_load_switch_outputs(unsigned mask, bool main_on, bool usb_on)
{
    ESP_LOGI(TAG, "Set load switches:%s%s%s%s", (mask & SW_MAIN) ? " main=" : "",
             (mask & SW_MAIN) ? (main_on ? "on" : "off") : "", (mask & SW_USB) ? " usb=" : "",
             (mask & SW_USB) ? (usb_on ? "on" : "off") : "");
    return change_outputs(mask, main_on, usb_on, false, false, NULL);
}

esp_err_t cut_load_switch_outputs(unsigned mask, unsigned* failed)
{
    struct cut_result res;
    esp_err_t err = change_outputs(mask, false, false, true, false, &res);
    if (failed)
        *failed = res.failed;
    return err;
}

esp_err_t retry_load_switch_cuts(unsigned* tried, unsigned* failed)
{
    struct cut_result res;
    esp_err_t err = change_outputs(SW_MAIN | SW_USB, false, false, true, true, &res);
    if (tried)
        *tried = res.tried;
    if (failed)
        *failed = res.failed;
    return err;
}

unsigned take_switched_on_load_switches(void)
{
    return atomic_exchange(&switched_on, 0);
}

esp_err_t set_load_switches(bool main_on, bool usb_on)
{
    return set_load_switch_outputs(SW_MAIN | SW_USB, main_on, usb_on);
}

// Releases a PWR or RST line. A failed release is retried for TRIGGER_RELEASE_ATTEMPTS x
// 20 ms: a line left low is a held button. The press state clears after the last attempt
// either way, so a broken expander cannot refuse presses for good.
static void trigger_off_callback(void* arg)
{
    trigger_t* trigger = arg;
    esp_err_t err = ESP_ERR_TIMEOUT;
    bool give_up = ++trigger->release_attempts >= TRIGGER_RELEASE_ATTEMPTS;
    if (xSemaphoreTake(expander_mutex, MUTEX_TIMEOUT) == pdTRUE)
    {
        err = pca9557_set_level(trigger->pin, 1);
        if (err == ESP_OK || give_up)
            trigger->pressed = false;
        xSemaphoreGive(expander_mutex);
    }
    else if (give_up)
    {
        trigger->pressed = false;
    }

    if (err == ESP_OK)
        return;
    if (!give_up)
    {
        esp_timer_start_once(trigger->timer, TRIGGER_RELEASE_RETRY_US);
        return;
    }
    ESP_LOGE(TAG, "Failed to release the %s line: %s", trigger->name, esp_err_to_name(err));
    push_eventf(EV_WARNING, "%s button release failed: %s", trigger->name, esp_err_to_name(err));
}

void config_sw()
{
    ESP_ERROR_CHECK(pca9557_set_output(GPIO_MAIN));
    ESP_ERROR_CHECK(pca9557_set_output(GPIO_USB));
    ESP_ERROR_CHECK(pca9557_set_output(GPIO_PWR));
    ESP_ERROR_CHECK(pca9557_set_output(GPIO_RST));

    ESP_ERROR_CHECK(pca9557_set_level(GPIO_PWR, 1));
    ESP_ERROR_CHECK(pca9557_set_level(GPIO_RST, 1));

    ESP_ERROR_CHECK(sync_load_switch_status_from_hw());
}

void init_sw()
{
    ESP_ERROR_CHECK(pca9557_init(PCA9557_ADDR));

    expander_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(expander_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    switch_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(switch_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    persist_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(persist_mutex ? ESP_OK : ESP_ERR_NO_MEM);

    config_sw();
    restore_load_switch_state();

    const esp_timer_create_args_t power_timer_args = {
        .callback = &trigger_off_callback, .arg = &power_trigger, .name = "power_trigger_off"};
    ESP_ERROR_CHECK(esp_timer_create(&power_timer_args, &power_trigger.timer));

    const esp_timer_create_args_t reset_timer_args = {
        .callback = &trigger_off_callback, .arg = &reset_trigger, .name = "reset_trigger_off"};
    ESP_ERROR_CHECK(esp_timer_create(&reset_timer_args, &reset_trigger.timer));
}

// Holds the line low for the trigger time. A press while the previous one is still held
// is refused (ESP_ERR_INVALID_STATE): restarting the timer would merge them into one
// longer press, and a power button held long enough forces the board off.
static esp_err_t press(trigger_t* trigger)
{
    ESP_LOGI(TAG, "Trig %s", trigger->name);
    if (!expander_mutex || !trigger->timer)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(expander_mutex, MUTEX_TIMEOUT) == pdFALSE)
    {
        ESP_LOGW(TAG, "Control error");
        return ESP_ERR_TIMEOUT;
    }
    if (trigger->pressed)
    {
        xSemaphoreGive(expander_mutex);
        ESP_LOGW(TAG, "%s press refused: the previous one is still held", trigger->name);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = pca9557_set_level(trigger->pin, 0);
    if (err == ESP_OK)
    {
        trigger->pressed = true;
        trigger->release_attempts = 0;
    }
    xSemaphoreGive(expander_mutex);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to press %s: %s", trigger->name, esp_err_to_name(err));
        return err;
    }

    push_eventf(EV_INFO, "%s triggered", trigger->name);
    if (esp_timer_start_once(trigger->timer, trigger->hold_us) != ESP_OK)
        trigger_off_callback(trigger); // Release now rather than hold the line.
    return ESP_OK;
}

esp_err_t trig_power()
{
    return press(&power_trigger);
}

esp_err_t trig_reset()
{
    return press(&reset_trigger);
}

esp_err_t set_main_load_switch(bool on)
{
    return set_load_switch_outputs(SW_MAIN, on, false);
}

esp_err_t set_usb_load_switch(bool on)
{
    return set_load_switch_outputs(SW_USB, false, on);
}

bool get_main_load_switch() { return (load_outputs() & SW_MAIN) != 0; }

bool get_usb_load_switch() { return (load_outputs() & SW_USB) != 0; }

unsigned get_load_switch_outputs(void) { return load_outputs(); }
