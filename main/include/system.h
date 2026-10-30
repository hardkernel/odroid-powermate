/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file system.h
 * @brief Declares system-level functions for core operations like rebooting and starting services.
 */

#ifndef SYSTEM_H
#define SYSTEM_H

/**
 * @brief Starts a timer that will reboot the system after a specified duration.
 *
 * This function is non-blocking. It schedules a system reboot to occur in the future.
 * @param sec The number of seconds to wait before rebooting.
 */
void start_reboot_timer(int sec);

/**
 * @brief Stops any pending reboot timer that was previously started.
 *
 * If a reboot timer is active, this function will cancel it, preventing the system from rebooting.
 */
void stop_reboot_timer();

/**
 * @brief Initializes and starts the main web server.
 *
 * This function sets up the HTTP server, registers all URI handlers for web pages,
 * API endpoints (like control and settings), and the WebSocket endpoint. It also
 * initializes the status monitor that provides real-time data.
 */
void start_webserver(void);

#endif // SYSTEM_H
