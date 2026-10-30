/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

//
// Created by shinys on 25. 7. 29.
//

#ifndef LED_H
#define LED_H

/**
 * @brief Defines the different blinking patterns for the LEDs.
 */
enum blink_type
{
    BLINK_SLOW = 0, ///< Slow blinking pattern.
    BLINK_FAST,     ///< Fast blinking pattern.
    BLINK_DOUBLE,   ///< Double blink pattern.
    BLINK_TRIPLE,   ///< Triple blink pattern.
    BLINK_SOLID,    ///< Solid (always on) state.
    BLINK_MAX,      ///< Sentinel for the number of blink types.
};

/**
 * @brief Defines the available LEDs that can be controlled.
 */
enum blink_led
{
    LED_RED = 0, ///< The red LED.
    LED_BLU = 1, ///< The blue LED.
    LED_MAX,     ///< Sentinel for the number of LEDs.
};

/**
 * @brief Initializes the LED indicator system.
 *
 * This function sets up the GPIOs and timers required for the LED blinking patterns.
 * It should be called once during application startup.
 */
void init_led(void);

/**
 * @brief Sets a specific blinking pattern for an LED.
 *
 * @param led The LED to control (e.g., LED_RED, LED_BLU).
 * @param type The blinking pattern to apply (e.g., BLINK_FAST, BLINK_SOLID).
 */
void led_set(enum blink_led led, enum blink_type type);

/**
 * @brief Turns off a specific LED.
 *
 * @param led The LED to turn off.
 */
void led_off(enum blink_led led);

#endif // LED_H
